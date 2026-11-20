#!/usr/bin/env python3
"""Turn WenQuanYi Bitmap Song PCF files into LVGL fmt_txt fonts.

Every glyph, including space, Latin letters, digits, and ASCII punctuation,
comes from WenQuanYi Bitmap Song. Temperature glyphs are the 16px face
scaled by 3.

Usage (from firmware/):
    python3 scripts/fonts/wqy_pcf_to_lvgl.py
"""

import argparse
import pathlib
import re
import struct
import tarfile
import zipfile
import sys

FIRMWARE_DIR = pathlib.Path(__file__).resolve().parents[2]
FONT_COMPONENT = FIRMWARE_DIR / "main/components/78__xiaozhi-fonts"
OUT_DIR = FONT_COMPONENT / "src/wqy"
DEFAULT_ARCHIVE = pathlib.Path(
    "/Users/QiZhen/Downloads/wqy-bitmapsong-pcf-1.0.0-RC1.tar.gz"
)
ASCII_FIRST = 0x20
ASCII_LAST = 0x7E

# Punctuation, units, arrows, and math already baked into the previous UI font.
EXTRA_SYMBOLS = (
    "，。！？：；、（）《》【】“”‘’—…·"
    "☀☁☂☃⚡❄🌧🌩🌦🌤🌥🌫"
    "$¥€£₿¢"
    "←↑→↓↖↗↘↙↔↕➜➝➞➤"
    "℃℉°‰"
    "±×÷≈≠≤≥∞√"
)
TEMP_SYMBOLS = "-0123456789°C"
INK_SOURCES = [
    "main/common/ink_weather_data.cc",
    "main/common/ink_calendar_data.cc",
    "main/ui/renderers/rawdraw/ink_weather_renderer.cc",
    "main/ui/renderers/rawdraw/ink_calendar_renderer.cc",
]
STRING_LITERAL = re.compile(r'"((?:[^"\\\n]|\\.)*)"')

FACES = (
    # archive member, pixel size, lv_font_t symbols (first owns the bitmaps)
    ("wenquanyi_9pt.pcf", 12, ("inksight_cjk_12",)),
    ("wenquanyi_13px.pcf", 14, ("inksight_cjk_14",)),
    (
        "wenquanyi_12pt.pcf",
        16,
        (
            "SourceHanSansSC_Regular_slim",
            "SourceHanSansSC_Medium_slim",
            "inksight_cjk_16",
        ),
    ),
)

PCF_METRICS = 1 << 2
PCF_BITMAPS = 1 << 3
PCF_BDF_ENCODINGS = 1 << 5
EMBEDDING_EXCEPTION = """\
As a special exception, if you create a document which uses this font, and
embed this font or unaltered portions of this font into the document, this
font does not by itself cause the resulting document to be covered by the
GNU General Public License. This exception does not however invalidate any
other reasons why the document might be covered by the GNU General Public
License. If you modify this font, you may extend this exception to your
version of the font, but you are not obligated to do so. If you do not wish
to do so, delete this exception statement from your version.
"""


class Glyph:
    def __init__(self, cp, adv, ofs_x, ofs_y, box_w, box_h, bitmap):
        self.cp = cp
        self.adv = adv
        self.ofs_x = ofs_x
        self.ofs_y = ofs_y
        self.box_w = box_w
        self.box_h = box_h
        self.bitmap = bitmap


class Face:
    def __init__(self, pixel_size, line_height, base_line, glyphs, source_name):
        self.pixel_size = pixel_size
        self.line_height = line_height
        self.base_line = base_line
        self.glyphs = glyphs
        self.source_name = source_name
        self.latin_source = None
        self.by_cp = {g.cp: g for g in glyphs}


def collect_codepoints():
    chars = {cp for cp in range(0x20, 0x7F)}
    for ch in EXTRA_SYMBOLS:
        chars.add(ord(ch))
    for ch in TEMP_SYMBOLS:
        chars.add(ord(ch))
    for hi in range(0xA1, 0xF8):
        for lo in range(0xA1, 0xFF):
            try:
                chars.add(ord(bytes((hi, lo)).decode("gb2312")))
            except UnicodeDecodeError:
                pass
    for rel in INK_SOURCES:
        text = (FIRMWARE_DIR / rel).read_text(encoding="utf-8")
        for literal in STRING_LITERAL.findall(text):
            for ch in literal:
                if ord(ch) > 0x7F:
                    chars.add(ord(ch))
    return chars


def _tables(data):
    if data[:4] != b"\x01fcp":
        raise ValueError("not a PCF font")
    count = struct.unpack_from("<I", data, 4)[0]
    tables = {}
    off = 8
    for _ in range(count):
        typ, fmt, size, toff = struct.unpack_from("<IIII", data, off)
        tables[typ] = toff
        off += 16
    return tables


def _load_pcf(data):
    tables = _tables(data)

    moff = tables[PCF_METRICS]
    mfmt = struct.unpack_from("<I", data, moff)[0]
    mendian = ">" if mfmt & 4 else "<"
    mp = moff + 4
    if mfmt & 0x100:
        nmetrics = struct.unpack_from(mendian + "H", data, mp)[0]
        mp += 2
        raw = data[mp:mp + nmetrics * 5]
        metrics = [
            tuple(b - 0x80 for b in raw[i:i + 5])
            for i in range(0, len(raw), 5)
        ]
    else:
        nmetrics = struct.unpack_from(mendian + "I", data, mp)[0]
        mp += 4
        metrics = []
        for i in range(nmetrics):
            metrics.append(struct.unpack_from(mendian + "hhhhhH", data, mp + i * 12)[:5])

    boff = tables[PCF_BITMAPS]
    bfmt = struct.unpack_from("<I", data, boff)[0]
    bendian = ">" if bfmt & 4 else "<"
    bp = boff + 4
    glyph_count = struct.unpack_from(bendian + "I", data, bp)[0]
    bp += 4
    offsets = struct.unpack_from(bendian + f"{glyph_count}I", data, bp)
    bp += 4 * glyph_count
    bp += 16  # bitmapSizes[4]
    bitmaps = data[bp:]
    pad = 1 << (bfmt & 3)
    bit_msb = bool(bfmt & 8)

    eoff = tables[PCF_BDF_ENCODINGS]
    efmt = struct.unpack_from("<I", data, eoff)[0]
    eendian = ">" if efmt & 4 else "<"
    first_col, last_col, first_row, last_row, _default = struct.unpack_from(
        eendian + "HHHHH", data, eoff + 4
    )
    cols = last_col - first_col + 1
    rows = last_row - first_row + 1
    index = struct.unpack_from(eendian + f"{cols * rows}H", data, eoff + 14)

    def glyph_id(cp):
        row, col = cp >> 8, cp & 0xFF
        if not (first_row <= row <= last_row and first_col <= col <= last_col):
            return None
        gid = index[(row - first_row) * cols + (col - first_col)]
        if gid == 0xFFFF or gid >= nmetrics or gid >= glyph_count:
            return None
        return gid

    return metrics, offsets, bitmaps, pad, bit_msb, glyph_id


def _pack_tight(width, height, pixel_on):
    """LVGL stride 0 packs each row at box_w bits, with no byte padding."""
    if width <= 0 or height <= 0:
        return b""
    total = width * height
    out = bytearray((total + 7) // 8)
    for y in range(height):
        for x in range(width):
            if pixel_on(x, y):
                bit = y * width + x
                out[bit >> 3] |= 0x80 >> (bit & 7)
    return bytes(out)


def _bit_on(bitmap, width, x, y):
    bit = y * width + x
    return bool(bitmap[bit >> 3] & (0x80 >> (bit & 7)))


def _pack_bitmap(blob, width, height, row_bytes, bit_msb):
    def pixel_on(x, y):
        source = blob[y * row_bytes + (x // 8)]
        if bit_msb:
            return (source >> (7 - (x & 7))) & 1
        return (source >> (x & 7)) & 1

    return _pack_tight(width, height, pixel_on)


def build_face(data, pixel_size, wanted, source_name):
    metrics, offsets, bitmaps, pad, bit_msb, glyph_id = _load_pcf(data)
    glyphs = []
    missing = []
    max_ascent = 0
    max_descent = 0
    for cp in sorted(wanted):
        gid = glyph_id(cp)
        if gid is None:
            missing.append(cp)
            continue
        left, right, advance, ascent, descent = metrics[gid]
        box_w = right - left
        box_h = ascent + descent
        if box_w < 0 or box_h < 0:
            box_w = 0
            box_h = 0
        if box_w and box_h:
            row_bits = pad * 8
            row_bytes = ((box_w + row_bits - 1) // row_bits) * pad
            start = offsets[gid]
            blob = bitmaps[start:start + row_bytes * box_h]
            if len(blob) != row_bytes * box_h:
                raise ValueError(f"U+{cp:04X} bitmap truncated in {source_name}")
            bitmap = _pack_bitmap(blob, box_w, box_h, row_bytes, bit_msb)
        else:
            box_w = 0
            box_h = 0
            bitmap = b""
        if advance < 0:
            advance = 0
        glyphs.append(Glyph(cp, advance, left, -descent, box_w, box_h, bitmap))
        if ascent > max_ascent:
            max_ascent = ascent
        if descent > max_descent:
            max_descent = descent
    if not glyphs:
        raise ValueError(f"no glyphs extracted from {source_name}")
    return Face(pixel_size, max_ascent + max_descent, max_descent, glyphs, source_name), missing


def _glyph_from_bdf(cp, advance, bbx, rows):
    width, height, xoff, yoff = bbx
    if advance < 0:
        advance = 0
    if width <= 0 or height <= 0:
        return Glyph(cp, advance, 0, 0, 0, 0, b"")
    if len(rows) != height:
        raise ValueError(f"U+{cp:04X} BDF bitmap has {len(rows)} rows, expected {height}")

    def pixel_on(x, y):
        row = rows[y]
        value = int(row, 16)
        return (value >> (len(row) * 4 - 1 - x)) & 1

    return Glyph(cp, advance, xoff, yoff, width, height, _pack_tight(width, height, pixel_on))


def load_bdf_ascii(text):
    """Read U+0020-U+007E. BBX y offset is already the LVGL baseline offset."""
    glyphs = {}
    copyright_line = ""
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        line = lines[i]
        if line.startswith("COPYRIGHT ") and not copyright_line:
            copyright_line = line.split(" ", 1)[1].strip().strip('"')
        if not line.startswith("STARTCHAR"):
            i += 1
            continue
        encoding = None
        advance = 0
        bbx = (0, 0, 0, 0)
        bitmap_rows = []
        i += 1
        while i < len(lines) and lines[i] != "ENDCHAR":
            if lines[i].startswith("ENCODING "):
                encoding = int(lines[i].split()[1])
            elif lines[i].startswith("DWIDTH "):
                advance = int(lines[i].split()[1])
            elif lines[i].startswith("BBX "):
                bbx = tuple(int(part) for part in lines[i].split()[1:5])
            elif lines[i] == "BITMAP":
                i += 1
                while i < len(lines) and lines[i] != "ENDCHAR":
                    bitmap_rows.append(lines[i])
                    i += 1
                continue
            i += 1
        if encoding is not None and ASCII_FIRST <= encoding <= ASCII_LAST:
            glyphs[encoding] = _glyph_from_bdf(encoding, advance, bbx, bitmap_rows)
        i += 1
    missing = [cp for cp in range(ASCII_FIRST, ASCII_LAST + 1) if cp not in glyphs]
    if missing:
        sample = "".join(chr(cp) for cp in missing[:20])
        raise ValueError(f"BDF is missing ASCII glyphs: {sample}")
    return glyphs, copyright_line


def load_galmuri(path):
    found = {}
    copyright_line = ""
    with zipfile.ZipFile(path) as archive:
        for member in archive.namelist():
            name = pathlib.PurePosixPath(member).name
            if name not in set(LATIN_BDF.values()) or "BDF/" not in member.replace("\\", "/"):
                continue
            raw = archive.read(member)
            try:
                text = raw.decode("utf-8")
            except UnicodeDecodeError:
                text = raw.decode("latin1")
            glyphs, notice = load_bdf_ascii(text)
            found[name] = glyphs
            copyright_line = copyright_line or notice
    missing = sorted(set(LATIN_BDF.values()) - set(found))
    if missing:
        raise ValueError(f"Galmuri archive is missing {', '.join(missing)}")
    return found, copyright_line


def splice_latin(face, galmuri, source_name):
    """Replace ASCII glyphs. The WenQuanYi line box stays put."""
    glyphs = []
    replaced = 0
    for glyph in face.glyphs:
        if ASCII_FIRST <= glyph.cp <= ASCII_LAST:
            glyphs.append(galmuri[glyph.cp])
            replaced += 1
        else:
            glyphs.append(glyph)
    present = {glyph.cp for glyph in glyphs}
    for cp in range(ASCII_FIRST, ASCII_LAST + 1):
        if cp not in present:
            glyphs.append(galmuri[cp])
            replaced += 1
    glyphs.sort(key=lambda glyph: glyph.cp)
    face.glyphs = glyphs
    face.by_cp = {glyph.cp: glyph for glyph in glyphs}
    face.latin_source = source_name
    return replaced


def _fits_line_box(face, glyph):
    if not glyph.box_h:
        return True
    top = face.line_height - face.base_line - glyph.ofs_y - glyph.box_h
    bottom = top + glyph.box_h
    return top >= 0 and bottom <= face.line_height


def scale_face(face, factor):
    glyphs = []
    for g in face.glyphs:
        if g.box_w and g.box_h:
            dst_w = g.box_w * factor
            dst_h = g.box_h * factor

            def pixel_on(x, y):
                return _bit_on(g.bitmap, g.box_w, x // factor, y // factor)

            bitmap = _pack_tight(dst_w, dst_h, pixel_on)
        else:
            dst_w = 0
            dst_h = 0
            bitmap = b""
        glyphs.append(Glyph(
            g.cp,
            g.adv * factor,
            g.ofs_x * factor,
            g.ofs_y * factor,
            dst_w,
            dst_h,
            bitmap,
        ))
    scaled = Face(
        face.pixel_size * factor,
        face.line_height * factor,
        face.base_line * factor,
        glyphs,
        face.source_name,
    )
    if face.latin_source:
        scaled.latin_source = f"{face.latin_source}, scaled x{factor}"
    return scaled


def _fits_lvgl(face):
    """LVGL's default glyph record stores a 20-bit index and 12-bit advance."""
    bitmap_bytes = sum(len(g.bitmap) for g in face.glyphs)
    if bitmap_bytes >= (1 << 20):
        raise ValueError(f"{face.source_name} bitmap is {bitmap_bytes} bytes, past 1MB")
    for g in face.glyphs:
        stored_adv = g.adv * 16
        if not (0 <= stored_adv < (1 << 12)):
            raise ValueError(f"U+{g.cp:04X} advance {g.adv}px does not fit")
        if g.box_w > 255 or g.box_h > 255:
            raise ValueError(f"U+{g.cp:04X} box {g.box_w}x{g.box_h} does not fit")
        if not (-128 <= g.ofs_x <= 127 and -128 <= g.ofs_y <= 127):
            raise ValueError(f"U+{g.cp:04X} offset ({g.ofs_x},{g.ofs_y}) does not fit")
        expected = (g.box_w * g.box_h + 7) // 8 if g.box_w and g.box_h else 0
        if len(g.bitmap) != expected:
            raise ValueError(
                f"U+{g.cp:04X} bitmap is {len(g.bitmap)} bytes, expected tight {expected}"
            )


def _cmaps(glyphs):
    cps = [g.cp for g in glyphs]
    groups = []
    start = 0
    while start < len(cps):
        end = start + 1
        while end < len(cps) and cps[end] - cps[start] <= 65534 and end - start < 65535:
            end += 1
        groups.append((start, end))
        start = end
    return groups


def _comment_char(cp):
    ch = chr(cp)
    if ch.isprintable() and ch not in {'\\', '"', '*', '/'}:
        return f' "{ch}"'
    return ""


def write_font_c(path, face, symbols, guard):
    _fits_lvgl(face)
    groups = _cmaps(face.glyphs)
    lines = []
    add = lines.append
    add("/**")
    add(f" * WenQuanYi Bitmap Song 1.0.0 RC1 ({face.source_name}, {face.pixel_size}px)")
    add(" * Copyright (C) 2004-2010 The WenQuanYi Project Board of Trustees and Qianqian Fang")
    add(" * License: GPL-2.0 with font embedding exception. See src/wqy/COPYING.")
    add(" * Generated by scripts/fonts/wqy_pcf_to_lvgl.py. Do not edit.")
    add(f" * Glyphs: {len(face.glyphs)}  line_height: {face.line_height}  base_line: {face.base_line}")
    add(" */")
    add("")
    add("#ifdef LV_LVGL_H_INCLUDE_SIMPLE")
    add('#include "lvgl.h"')
    add("#else")
    add('#include "lvgl.h"')
    add("#endif")
    add("")
    add(f"#ifndef {guard}")
    add(f"#define {guard} 1")
    add("#endif")
    add("")
    add(f"#if {guard}")
    add("")
    add("static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {")
    bitmap_index = 0
    indexes = []
    for g in face.glyphs:
        indexes.append(bitmap_index)
        add(f"    /* U+{g.cp:04X}{_comment_char(g.cp)} */")
        if not g.bitmap:
            add("    0x0,")
            bitmap_index += 1
            continue
        for row in range(0, len(g.bitmap), 16):
            chunk = g.bitmap[row:row + 16]
            add("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
        bitmap_index += len(g.bitmap)
    add("};")
    add("")
    add("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {")
    add("    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,")
    for g, index in zip(face.glyphs, indexes):
        add(
            "    {"
            f".bitmap_index = {index}, .adv_w = {g.adv * 16}, "
            f".box_w = {g.box_w}, .box_h = {g.box_h}, "
            f".ofs_x = {g.ofs_x}, .ofs_y = {g.ofs_y}"
            f"}}, /* U+{g.cp:04X} */"
        )
    add("};")
    add("")
    glyph_id = 1
    for group_i, (start, end) in enumerate(groups):
        group = face.glyphs[start:end]
        base = group[0].cp
        add(f"static const uint16_t unicode_list_{group_i}[] = {{")
        offsets = [g.cp - base for g in group]
        for row in range(0, len(offsets), 8):
            chunk = offsets[row:row + 8]
            add("    " + ", ".join(f"0x{n:x}" for n in chunk) + ",")
        add("};")
        add("")
        glyph_id += len(group)
    add("static const lv_font_fmt_txt_cmap_t cmaps[] = {")
    glyph_id = 1
    for group_i, (start, end) in enumerate(groups):
        group = face.glyphs[start:end]
        base = group[0].cp
        length = group[-1].cp - base + 1
        add("    {")
        add(f"        .range_start = {base}, .range_length = {length}, .glyph_id_start = {glyph_id},")
        add(f"        .unicode_list = unicode_list_{group_i}, .glyph_id_ofs_list = NULL,")
        add(f"        .list_length = {len(group)}, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY")
        add("    },")
        glyph_id += len(group)
    add("};")
    add("")
    add("#if LVGL_VERSION_MAJOR == 8")
    add("static lv_font_fmt_txt_glyph_cache_t cache;")
    add("#endif")
    add("")
    add("#if LVGL_VERSION_MAJOR >= 8")
    add("static const lv_font_fmt_txt_dsc_t font_dsc = {")
    add("#else")
    add("static lv_font_fmt_txt_dsc_t font_dsc = {")
    add("#endif")
    add("    .glyph_bitmap = glyph_bitmap,")
    add("    .glyph_dsc = glyph_dsc,")
    add("    .cmaps = cmaps,")
    add("    .kern_dsc = NULL,")
    add("    .kern_scale = 0,")
    add(f"    .cmap_num = {len(groups)},")
    add("    .bpp = 1,")
    add("    .kern_classes = 0,")
    add("    .bitmap_format = 0,")
    add("#if LVGL_VERSION_MAJOR == 8")
    add("    .cache = &cache")
    add("#endif")
    add("};")
    add("")
    owner = symbols[0]
    add("#if LVGL_VERSION_MAJOR >= 8")
    add(f"const lv_font_t {owner} = {{")
    add("#else")
    add(f"lv_font_t {owner} = {{")
    add("#endif")
    add("    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,")
    add("    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,")
    add(f"    .line_height = {face.line_height},")
    add(f"    .base_line = {face.base_line},")
    add("#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)")
    add("    .subpx = LV_FONT_SUBPX_NONE,")
    add("#endif")
    add("#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8")
    add("    .underline_position = -1,")
    add("    .underline_thickness = 1,")
    add("#endif")
    add("    .dsc = &font_dsc,")
    add("#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9")
    add("    .fallback = NULL,")
    add("#endif")
    add("    .user_data = NULL,")
    add("};")
    add("")
    for alias in symbols[1:]:
        add(f"const lv_font_t {alias} = {owner};")
    if len(symbols) > 1:
        add("")
    add(f"#endif /* {guard} */")
    add("")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines), encoding="utf-8")


def _ink(glyph):
    if not glyph.bitmap:
        return 0
    return sum(bin(b).count("1") for b in glyph.bitmap)


def _ascii(glyph):
    rows = []
    for y in range(glyph.box_h):
        row = []
        for x in range(glyph.box_w):
            row.append("#" if _bit_on(glyph.bitmap, glyph.box_w, x, y) else ".")
        rows.append("".join(row))
    return "\n".join(rows)


def _same_glyph(got, expect):
    return (
        (got.adv, got.box_w, got.box_h, got.ofs_x, got.ofs_y, got.bitmap)
        == (expect.adv, expect.box_w, expect.box_h, expect.ofs_x, expect.ofs_y, expect.bitmap)
    )


def check_face(face, pixel_size):
    zhong = face.by_cp.get(0x4E2D)
    latin = face.by_cp.get(0x41)
    digit = face.by_cp.get(0x30)
    space = face.by_cp.get(0x20)
    if zhong is None or latin is None or digit is None or space is None:
        raise SystemExit(f"{pixel_size}px is missing 中, A, 0, or space")
    if zhong.adv != pixel_size:
        raise SystemExit(f"{pixel_size}px 中 advance is {zhong.adv}, expected {pixel_size}")
    if _ink(zhong) < 20 or _ink(latin) < 8 or _ink(digit) < 4:
        raise SystemExit(f"{pixel_size}px 中, A, or 0 has no ink")
    if space.adv <= 0:
        raise SystemExit(f"{pixel_size}px space has no advance")
    if face.line_height < pixel_size or face.base_line < 0:
        raise SystemExit(f"{pixel_size}px line metrics {face.line_height}/{face.base_line} are unusable")
    if face.latin_source:
        raise SystemExit(f"{pixel_size}px ASCII is still marked as {face.latin_source}")
    if pixel_size == 16:
        if zhong.bitmap[:2] != bytes((0x01, 0x00)):
            raise SystemExit("16px 中 first row does not match the WenQuanYi bitmap")
        if (latin.adv, latin.box_w, latin.box_h) != (10, 9, 11):
            raise SystemExit("16px A is not the WenQuanYi glyph")
    for cp in range(ASCII_FIRST, ASCII_LAST + 1):
        if face.by_cp.get(cp) is None:
            raise SystemExit(f"{pixel_size}px is missing U+{cp:04X}")
    print(f"  {pixel_size}px 中 {zhong.box_w}x{zhong.box_h} adv {zhong.adv} "
          f"A {latin.box_w}x{latin.box_h} adv {latin.adv} "
          f"line {face.line_height}/{face.base_line} glyphs {len(face.glyphs)}")
    print("  A:")
    print("\n".join("    " + line for line in _ascii(latin).splitlines()))


def check_temp(temp, source):
    for ch in TEMP_SYMBOLS:
        if ord(ch) not in temp.by_cp or ord(ch) not in source.by_cp:
            raise SystemExit(f"temperature glyph {ch!r} missing")
        got = temp.by_cp[ord(ch)]
        base = source.by_cp[ord(ch)]
        if not _same_glyph(got, _scale_one(base, 3)):
            raise SystemExit(f"temperature {ch!r} was not scaled by 3")
    if temp.line_height != source.line_height * 3 or temp.base_line != source.base_line * 3:
        raise SystemExit("temperature line metrics were not scaled by 3")
    print(f"  temp x3 line {temp.line_height}/{temp.base_line} glyphs {len(temp.glyphs)}")


def _scale_one(glyph, factor):
    scaled = scale_face(Face(0, 0, 0, [glyph], ""), factor)
    return scaled.glyphs[0]


def write_copying(archive):
    with tarfile.open(archive, "r:gz") as tf:
        gpl = tf.extractfile("wqy-bitmapsong/COPYING").read().decode("latin1")
    text = (
        "WenQuanYi Bitmap Song, version 1.0.0 RC1\n"
        "Copyright (C) 2004-2010, The WenQuanYi Project Board of Trustees and Qianqian Fang\n"
        "https://wenq.org/\n"
        "\n"
        "The bitmap data in this directory is converted, unaltered in glyph shape,\n"
        "from wenquanyi_9pt.pcf, wenquanyi_13px.pcf, and wenquanyi_12pt.pcf.\n"
        "Space, Latin letters, digits, and ASCII punctuation come from the same faces.\n"
        "The 48px temperature glyphs are the 16px glyphs scaled by 3.\n"
        "\n"
        "License: GNU General Public License, version 2, with the font embedding\n"
        "exception below. The full GPL v2 text follows the exception.\n"
        "\n"
        "GPL v2.0 license with font embedding exception:\n"
        "\n"
        + EMBEDDING_EXCEPTION
        + "\n"
        + gpl
    )
    (OUT_DIR / "COPYING").write_text(text, encoding="utf-8")


def _show_missing(label, missing):
    if not missing:
        print(f"  {label}: every requested codepoint was present")
        return
    sample = "".join(chr(cp) for cp in missing[:40])
    print(f"  {label}: skipped {len(missing)} missing codepoints, sample: {sample}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=pathlib.Path, default=DEFAULT_ARCHIVE)
    args = parser.parse_args()
    if not args.archive.is_file():
        raise SystemExit(f"font archive not found: {args.archive}")

    wanted = collect_codepoints()
    print(f"requested codepoints: {len(wanted)}")
    members = {}
    with tarfile.open(args.archive, "r:gz") as tf:
        for name, _pixel, _symbols in FACES:
            member = tf.extractfile(f"wqy-bitmapsong/{name}")
            if member is None:
                raise SystemExit(f"{name} is not in the archive")
            members[name] = member.read()

    built = {}
    for name, pixel, symbols in FACES:
        face, missing = build_face(members[name], pixel, wanted, name)
        _show_missing(f"{pixel}px", missing)
        check_face(face, pixel)
        built[pixel] = (face, symbols)

    body = built[16][0]
    temp_wanted = {ord(ch) for ch in TEMP_SYMBOLS}
    temp_source = Face(
        16,
        body.line_height,
        body.base_line,
        [body.by_cp[cp] for cp in sorted(temp_wanted)],
        "wenquanyi_12pt.pcf",
    )
    temp = scale_face(temp_source, 3)
    check_temp(temp, temp_source)

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    for pixel, (face, symbols) in built.items():
        guard = "WQY_BITMAP_SONG_" + "_".join(symbols)
        guard = re.sub(r"[^A-Za-z0-9_]", "_", guard).upper()
        path = OUT_DIR / f"wqy_bitmap_song_{pixel}.c"
        write_font_c(path, face, symbols, guard)
        print(f"wrote {path.relative_to(FIRMWARE_DIR)} ({path.stat().st_size} bytes)")
    temp_path = OUT_DIR / "inksight_temp_48.c"
    write_font_c(temp_path, temp, ("inksight_temp_48",), "INKSIGHT_TEMP_48")
    print(f"wrote {temp_path.relative_to(FIRMWARE_DIR)} ({temp_path.stat().st_size} bytes)")
    write_copying(args.archive)
    print(f"wrote {(OUT_DIR / 'COPYING').relative_to(FIRMWARE_DIR)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
