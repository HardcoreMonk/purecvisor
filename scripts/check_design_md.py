#!/usr/bin/env python3
# This guard is intentionally stricter than a markdown linter.
# It protects the repository contract that UI visual rules live in DESIGN.md,
# while GUIDE.md and the in-app guide only link to that visual source.
# The checker fails on missing sections, token names, preview wiring, and deploy
# coverage so future UI work cannot silently split the design system again.
# Keep the assertions literal: vague prose matches would hide drift.
"""DESIGN.md visual contract static guard."""

from __future__ import annotations

import re
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent

REQUIRED_SECTIONS = (
    "Visual Theme & Atmosphere",
    "Color Tokens & Roles",
    "Typography Rules",
    "Component States",
    "Dashboard Density",
    "Table Rules",
    "Card Rules",
    "Button Rules",
    "Modal Rules",
    "Responsive & Accessibility",
    "Do's and Don'ts",
    "Agent Prompt Guide",
)

REQUIRED_TOKENS = (
    "--bg",
    "--bg2",
    "--bg3",
    "--bg-panel",
    "--border",
    "--border-panel",
    "--fg",
    "--fg2",
    "--accent",
    "--cyan",
    "--green",
    "--yellow",
    "--red",
    "--font-sans",
    "--font-display",
    "--font-mono",
    "--r",
    "--pcv-spring",
)

REQUIRED_COMPONENT_TERMS = (
    ".hc",
    ".btn",
    ".btn-primary",
    ".modal",
    ".modal-wide",
    "progress bar",
    "라벨 전체",
    "table.card-mobile",
    "normal",
    "hover",
    "focus",
    "active",
    "disabled",
    "loading",
    "empty",
    "error",
)


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


def require_all(path: str, text: str, needles: tuple[str, ...]) -> None:
    for needle in needles:
        require(f"{path} must contain {needle}", needle in text)


def require_section(text: str, section: str) -> None:
    pattern = rf"^##\s+\d+\.\s+{re.escape(section)}\s*$"
    require(
        f"DESIGN.md must define section: {section}",
        re.search(pattern, text, re.MULTILINE) is not None,
    )


def main() -> int:
    design = read("DESIGN.md")
    agents = read("AGENTS.md")
    guide = read("docs/GUIDE.md")
    ui_guide = read("ui/guide-content.md")
    style = read("ui/style.css")
    preview = read("ui/samples/design-system-preview.html")
    deploy = read("scripts/deploy.sh")

    require("DESIGN.md must start with a project title", design.startswith("# PureCVisor Single Edge DESIGN.md"))
    for section in REQUIRED_SECTIONS:
        require_section(design, section)
    require_all("DESIGN.md", design, REQUIRED_TOKENS)
    require_all("DESIGN.md", design, REQUIRED_COMPONENT_TERMS)
    require(
        "DESIGN.md must link to its preview HTML",
        "ui/samples/design-system-preview.html" in design,
    )
    require(
        "DESIGN.md must keep UI visual rules separate from GUIDE.md",
        "docs/GUIDE.md" in design and "시각 규격" in design,
    )
    require(
        "DESIGN.md must define the current typography/icon baseline",
        all(term in design for term in ("Pretendard", "line-height", "1.5", "letter-spacing", "Coolicons")),
    )

    require(
        "AGENTS.md must require DESIGN.md before UI work",
        "UI 작업 전" in agents and "DESIGN.md" in agents,
    )
    require(
        "AGENTS.md must mention scripts/check_design_md.py",
        "scripts/check_design_md.py" in agents,
    )

    for path, text in (("docs/GUIDE.md", guide), ("ui/guide-content.md", ui_guide)):
        require(f"{path} must link DESIGN.md", "DESIGN.md" in text)
        require(
            f"{path} must link design preview",
            "ui/samples/design-system-preview.html" in text,
        )
        require(
            f"{path} must mention the design checker",
            "scripts/check_design_md.py" in text,
        )

    require(
        "preview must load the runtime stylesheet",
        'href="../style.css"' in preview,
    )
    require(
        "preview must load self-hosted Pretendard",
        'href="../vendor/pretendard/pretendard.css"' in preview,
    )
    require(
        "preview must expose design preview shell",
        "design-preview" in preview,
    )
    require(
        "preview must demonstrate table/card/button/modal surfaces",
        all(term in preview for term in ("<table", "class=\"hc", "class=\"btn", "class=\"modal")),
    )
    require(
        "preview must demonstrate local Coolicons",
        "../vendor/coolicons/coolicons.svg#ci-" in preview,
    )
    require(
        "preview must demonstrate low progress label readability",
        "class=\"pb-t\">2.0%" in preview,
    )
    require(
        "style.css must keep progress labels unclipped",
        all(term in style for term in (".pb-t", "height: 18px", "white-space: nowrap", "line-height: 16px")),
    )
    require(
        "style.css must expose the current body typography baseline",
        all(term in style for term in ("--font-sans", "font-family: var(--font-sans)", "line-height: 1.5", "letter-spacing: 0")),
    )
    require(
        "GUIDE.md and in-app guide must mention local Coolicons vendor asset",
        "ui/vendor/coolicons/coolicons.svg" in guide and "ui/vendor/coolicons/coolicons.svg" in ui_guide,
    )

    require(
        "deploy.sh must deploy ui/samples for linked previews",
        "pcv_ui_samples" in deploy and "ui/samples" in deploy,
    )

    print("[PASS] DESIGN.md visual contract is present")
    return 0


if __name__ == "__main__":
    sys.exit(main())
