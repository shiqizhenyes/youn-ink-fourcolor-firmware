#!/usr/bin/env python3
"""Inline luci-lite.css, theme.js, chrome header, and optional sonic.js."""

import re
import sys
from pathlib import Path


def main() -> None:
    if len(sys.argv) not in (5, 6):
        raise SystemExit(
            "usage: inject_webui.py TEMPLATE CSS JS OUTPUT [--tool]"
        )
    template, css_path, js_path, output = sys.argv[1:5]
    tool_mode = len(sys.argv) == 6 and sys.argv[5] == "--tool"
    css = Path(css_path).read_text(encoding="utf-8")
    js = Path(js_path).read_text(encoding="utf-8")
    webui_dir = Path(css_path).parent
    header = ""
    header_path = webui_dir / "chrome-header.html"
    if header_path.exists():
        header = header_path.read_text(encoding="utf-8")
    sonic = ""
    sonic_path = webui_dir / "sonic.js"
    if sonic_path.exists():
        sonic = sonic_path.read_text(encoding="utf-8")

    text = Path(template).read_text(encoding="utf-8")
    if tool_mode:
        if "data-chrome=" not in text:
            text = text.replace(
                '<html lang="zh-CN">',
                '<html lang="zh-CN" data-chrome="lan">',
                1,
            )
        if header:
            text, n = re.subn(
                r"<header[\s\S]*?</header>",
                header,
                text,
                count=1,
            )
            if n == 0:
                text = text.replace("<body>", "<body>\n" + header, 1)
        text = re.sub(
            r'<link rel="stylesheet" href="[^"]*luci-lite\.css"\s*/?>',
            lambda _m: "<style>\n" + css + "\n</style>",
            text,
            count=1,
        )
        text = re.sub(
            r'<script src="[^"]*theme\.js"></script>',
            lambda _m: "<script>\n" + js + "\n</script>",
            text,
            count=1,
        )
        text = re.sub(
            r'<ul class="breadcrumb[^>]*>[\s\S]*?</ul>',
            "",
            text,
            count=1,
        )
    else:
        text = text.replace("__LUCI_LITE_CSS__", css)
        text = text.replace("__LUCI_THEME_JS__", js)
        text = text.replace("__LUCI_HEADER__", header)
        text = text.replace("__LUCI_SONIC_JS__", sonic)

    out = Path(output)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(text, encoding="utf-8")


if __name__ == "__main__":
    main()
