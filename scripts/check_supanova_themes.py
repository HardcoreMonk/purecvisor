#!/usr/bin/env python3
# Theme names must stay synchronized across boot HTML, runtime modules,
# generated bundle, and CSS overrides.
# Missing or stale literals mean the UI can offer a theme that cannot actually render.
"""Supanova theme static contract guard."""

from __future__ import annotations

import re
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent
THEMES = ("supanova", "supanova-cyan", "supanova-hicontrast")
REMOVED_THEMES = ("supanova-emerald", "supanova-light")


def read(path: str) -> str:
    try:
        return (REPO_ROOT / path).read_text(encoding="utf-8")
    except OSError as exc:
        print(f"ERROR: failed to read {path}: {exc}", file=sys.stderr)
        sys.exit(2)


def require(description: str, condition: bool) -> None:
    if not condition:
        print(f"FAIL: {description}", file=sys.stderr)
        sys.exit(1)


def main() -> int:
    index_html = read("ui/index.html")
    app_js = read("ui/app.js")
    app_bundle = read("ui/app.bundle.js")
    theme_js = read("ui/modules/theme.js")
    style_css = read("ui/style.css")

    for theme in THEMES:
        quoted = f"'{theme}'"
        require(f"{theme} is allowed in ui/index.html", quoted in index_html or f'"{theme}"' in index_html)
        require(f"{theme} is allowed in ui/app.js", quoted in app_js)
        if theme != "supanova":
            require(f"{theme} has a theme module id", f"id: '{theme}'" in theme_js)
            require(f"{theme} is present in generated bundle", quoted in app_bundle)
        require(f"{theme} is present in the theme module", quoted in theme_js)

    for theme in REMOVED_THEMES:
        quoted = f"'{theme}'"
        double_quoted = f'"{theme}"'
        for path, text in (
            ("ui/index.html", index_html),
            ("ui/app.js", app_js),
            ("ui/modules/theme.js", theme_js),
            ("ui/app.bundle.js", app_bundle),
        ):
            require(f"{theme} must be removed from {path}", quoted not in text and double_quoted not in text)
        require(
            f"{theme} CSS override must be removed",
            re.search(rf'\[data-theme="{re.escape(theme)}"\]', style_css) is None,
        )

    require(
        "high-contrast CSS override exists",
        re.search(r'\[data-theme="supanova-hicontrast"\]', style_css) is not None,
    )
    require(
        "cyan CSS override exists",
        re.search(r'\[data-theme="supanova-cyan"\]', style_css) is not None,
    )
    require(
        "light theme color-scheme override is removed",
        re.search(r"color-scheme:\s*light", style_css) is None,
    )

    print("[PASS] Supanova theme static contract is present")
    return 0


if __name__ == "__main__":
    sys.exit(main())
