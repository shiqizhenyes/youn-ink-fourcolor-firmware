#!/usr/bin/env python3
"""Rasterize almanac 吉 + ruyi clouds to 1bpp C arrays (~20px tall).

Usage (from firmware/):
    python3 scripts/almanac_ornaments/gen_ornaments.py
"""

from __future__ import annotations

import pathlib
import re
import sys

from PIL import Image, ImageDraw

HERE = pathlib.Path(__file__).resolve().parent
FIRMWARE_DIR = HERE.parents[1]
OUT_C = FIRMWARE_DIR / "main/components/78__xiaozhi-fonts/src/inksight/inksight_almanac_ornaments.c"
OUT_H = FIRMWARE_DIR / "main/components/78__xiaozhi-fonts/include/almanac_ornaments.h"

TARGET_H = 20
HIRES = 4  # render then downscale for smoother 1bpp edges

PATH_CMD = re.compile(r"([MLCZ])|(-?\d+(?:\.\d+)?)")
SVG_PATH = re.compile(r'<path d="([^"]+)"', re.I)


def pack_bits(img: Image.Image) -> tuple[int, int, list[int]]:
    img = img.convert("L")
    w, h = img.size
    stride = (w + 7) // 8
    data = [0] * (stride * h)
    px = img.load()
    for y in range(h):
        for x in range(w):
            if px[x, y] < 160:
                data[y * stride + x // 8] |= 0x80 >> (x % 8)
    return w, h, data


def crop_ink(img: Image.Image) -> Image.Image:
    bbox = img.getbbox()
    if not bbox:
        return img
    return img.crop(bbox)


def scale_to_height(img: Image.Image, height: int) -> Image.Image:
    if img.size[1] == height:
        return img
    w = max(1, round(img.size[0] * height / img.size[1]))
    return img.resize((w, height), Image.Resampling.LANCZOS)


def to_1bpp(img: Image.Image) -> Image.Image:
    return img.point(lambda v: 0 if v < 160 else 255, mode="L").convert("1")


def cubic(p0, p1, p2, p3, n: int = 24):
    pts = []
    for i in range(n + 1):
        t = i / n
        u = 1.0 - t
        x = u * u * u * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t * t * t * p3[0]
        y = u * u * u * p0[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t * t * t * p3[1]
        pts.append((x, y))
    return pts


def parse_path(d: str) -> list[list[tuple[float, float]]]:
    tokens: list[str] = []
    for m in PATH_CMD.finditer(d):
        tokens.append(m.group(1) or m.group(2))
    polys: list[list[tuple[float, float]]] = []
    cur: list[tuple[float, float]] = []
    i = 0
    x = y = 0.0
    cmd = "M"
    while i < len(tokens):
        t = tokens[i]
        if t in "MLCZ":
            cmd = t
            i += 1
            if cmd == "Z":
                if cur:
                    polys.append(cur)
                    cur = []
            continue
        if cmd == "M":
            if cur:
                polys.append(cur)
                cur = []
            x, y = float(tokens[i]), float(tokens[i + 1])
            cur.append((x, y))
            i += 2
            cmd = "L"
        elif cmd == "L":
            x, y = float(tokens[i]), float(tokens[i + 1])
            cur.append((x, y))
            i += 2
        elif cmd == "C":
            x1, y1 = float(tokens[i]), float(tokens[i + 1])
            x2, y2 = float(tokens[i + 2]), float(tokens[i + 3])
            x3, y3 = float(tokens[i + 4]), float(tokens[i + 5])
            cur.extend(cubic((x, y), (x1, y1), (x2, y2), (x3, y3))[1:])
            x, y = x3, y3
            i += 6
        else:
            raise ValueError(f"unsupported SVG command {cmd}")
    if cur:
        polys.append(cur)
    return polys


def raster_ji(svg_path: pathlib.Path) -> Image.Image:
    text = svg_path.read_text(encoding="utf-8")
    vb = re.search(r'viewBox="0 0 (\d+) (\d+)"', text)
    vw, vh = (int(vb.group(1)), int(vb.group(2))) if vb else (500, 400)
    scale = HIRES
    img = Image.new("L", (vw * scale, vh * scale), 255)
    draw = ImageDraw.Draw(img)
    for d in SVG_PATH.findall(text):
        for poly in parse_path(d):
            pts = [(p[0] * scale, p[1] * scale) for p in poly]
            if len(pts) >= 3:
                draw.polygon(pts, fill=0)
    img = crop_ink(Image.eval(img, lambda v: 255 - v)).point(lambda v: 255 - v)
    img = scale_to_height(img, TARGET_H)
    return to_1bpp(img)


def raster_cloud(png_path: pathlib.Path) -> Image.Image:
    src = Image.open(png_path).convert("RGBA")
    w, h = src.size
    out = Image.new("L", (w, h), 255)
    sp = src.load()
    op = out.load()
    for y in range(h):
        for x in range(w):
            r, g, b, a = sp[x, y]
            if a >= 128 and r > 70 and r >= g + 10:
                op[x, y] = 0
    ink = Image.eval(out, lambda v: 255 - v)
    ink = crop_ink(ink)
    ink = Image.eval(ink, lambda v: 255 - v)
    ink = scale_to_height(ink, TARGET_H)
    return to_1bpp(ink)


def emit_array(name: str, img: Image.Image) -> tuple[str, str, int, int]:
    w, h, data = pack_bits(img)
    lines = [f"static const uint8_t k_{name}_bits[] = {{"]
    for i in range(0, len(data), 16):
        chunk = ", ".join(f"0x{b:02x}" for b in data[i : i + 16])
        lines.append(f"    {chunk},")
    lines.append("};")
    camel = "".join(p.title() for p in name.split("_"))
    decl = f"const inksight_mono_bitmap_t kAlmanac{camel} = {{{w}, {h}, k_{name}_bits}};"
    return "\n".join(lines), decl, w, h


def main() -> int:
    clouds = {
        "cloud_left": HERE / "cloud_left.png",
        "cloud_right": HERE / "cloud_right.png",
    }
    images = {k: raster_cloud(p) for k, p in clouds.items()}
    images["ji"] = raster_ji(HERE / "ji.svg")

    arrays = []
    decls = []
    sizes = {}
    for name, img in images.items():
        arr, decl, w, h = emit_array(name, img)
        arrays.append(arr)
        decls.append(decl)
        sizes[name] = (w, h)

    OUT_C.parent.mkdir(parents=True, exist_ok=True)
    OUT_C.write_text(
        "/* Generated by scripts/almanac_ornaments/gen_ornaments.py. Do not edit. */\n"
        '#include "almanac_ornaments.h"\n\n'
        + "\n\n".join(arrays)
        + "\n\n"
        + "\n".join(decls)
        + "\n",
        encoding="utf-8",
    )
    OUT_H.write_text(
        """/* Generated by scripts/almanac_ornaments/gen_ornaments.py. Do not edit. */
#pragma once

#include "inksight_assets.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const inksight_mono_bitmap_t kAlmanacCloudLeft;
extern const inksight_mono_bitmap_t kAlmanacCloudRight;
extern const inksight_mono_bitmap_t kAlmanacJi;

#ifdef __cplusplus
}
#endif
""",
        encoding="utf-8",
    )
    print(f"wrote {OUT_C.relative_to(FIRMWARE_DIR)}")
    print(f"wrote {OUT_H.relative_to(FIRMWARE_DIR)}")
    for name, (w, h) in sizes.items():
        print(f"  {name}: {w}x{h}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
