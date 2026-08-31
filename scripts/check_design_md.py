#!/usr/bin/env python3
                          
                                                                 
                                                                                      
                                                                                                                            
 
                      
                                                                                                                          
                                                              
                                                                             
                                                                      
                                                                                
                                                                           
                                                                          
                                             

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
    "Documentation Portal",
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
    "aria-labelledby",
    "aria-activedescendant",
    "native button",
    "noDismiss",
    "native dialog backdrop",
    "고정 상한",
    "fieldset",
    "VPC + Subnet 생성",
)

ALERT_SEVERITY_LANES = (
    ("crit", "--st-crit", "pill-crit", "CRIT"),
    ("warn", "--st-warn", "pill-warn", "WARN"),
    ("idle", "--st-idle", "pill-idle", "UNKNOWN"),
)

REQUIRED_THEME_IDS = (
    "supanova",
    "supanova-cyan",
    "supanova-hicontrast",
    "supanova-mockup",
)

                                                   
                                          
DOCS_DASHBOARD_PALETTE = (
    ("canvas", "bg", "#ffffff"),
    ("soft", "bg2", "#f5f7fa"),
    ("soft-strong", "bg3", "#edf1f5"),
    ("ink", "fg", "#171c24"),
    ("muted", "fg2", "#5c6675"),
    ("line", "border", "#dce1e8"),
    ("accent", "accent", "#12627a"),
    ("error", "red", "#b42332"),
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


def strip_html_comments(text: str) -> str:
    return re.sub(r"<!--.*?-->", "", text, flags=re.DOTALL)


def strip_css_comments(text: str) -> str:
    return re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)


def css_custom_properties(text: str, selector: str) -> dict[str, str]:
                                                                             
    match = re.search(
        rf"{re.escape(selector)}\s*\{{(?P<body>[^}}]*)\}}",
        strip_css_comments(text),
        re.DOTALL,
    )
    if match is None:
        return {}
    return {
        name: value.strip().lower()
        for name, value in re.findall(
            r"--([a-z0-9-]+)\s*:\s*([^;]+);",
            match.group("body"),
            re.IGNORECASE,
        )
    }


def has_alert_design_mapping(
    design: str,
    lane: str,
    token: str,
    pill: str,
    label: str,
) -> bool:
    design = strip_html_comments(design)
    section_match = re.search(
        r"^###\s+6\.1\s+알림 severity lane\s*$"
        r"(?P<body>.*?)(?=^##\s|\Z)",
        design,
        re.MULTILINE | re.DOTALL,
    )
    if section_match is None:
        return False
    row_pattern = (
        rf"^\|[^|\n]*`alert-row-{re.escape(lane)}`[^|\n]*"
        rf"\|[^|\n]*`{re.escape(token)}`[^|\n]*"
        rf"\|[^|\n]*`{re.escape(pill)}`[^|\n]*/"
        rf"[^|\n]*`{re.escape(label)}`[^|\n]*\|"
    )
    return re.search(
        row_pattern,
        section_match.group("body"),
        re.MULTILINE,
    ) is not None


def has_alert_css_mapping(style: str, lane: str, token: str) -> bool:
    style = strip_css_comments(style)
    rule_match = re.search(
        rf"\.alert-row-{re.escape(lane)}\s*\{{(?P<body>[^}}]*)\}}",
        style,
        re.DOTALL,
    )
    if rule_match is None:
        return False
    return re.search(
        rf"border-inline-start\s*:\s*3px\s+solid\s+"
        rf"var\(\s*{re.escape(token)}\s*\)\s*;",
        rule_match.group("body"),
    ) is not None


def has_alert_preview_mapping(
    preview: str,
    lane: str,
    pill: str,
    label: str,
) -> bool:
    preview = strip_html_comments(preview)

    def class_tokens(attributes: str) -> set[str]:
        class_match = re.search(
            r"\bclass\s*=\s*([\"'])(?P<value>.*?)\1",
            attributes,
            re.DOTALL | re.IGNORECASE,
        )
        if class_match is None:
            return set()
        return set(class_match.group("value").split())

    row_class = f"alert-row-{lane}"
    for row_match in re.finditer(
        r"<tr\b(?P<attrs>[^>]*)>(?P<body>.*?)</tr>",
        preview,
        re.DOTALL | re.IGNORECASE,
    ):
        if row_class not in class_tokens(row_match.group("attrs")):
            continue
        for span_match in re.finditer(
            r"<span\b(?P<attrs>[^>]*)>(?P<body>.*?)</span>",
            row_match.group("body"),
            re.DOTALL | re.IGNORECASE,
        ):
            if pill not in class_tokens(span_match.group("attrs")):
                continue
            return span_match.group("body").strip() == label
        return False
    return False


def self_test_alert_lane_matchers() -> int:
    for lane, token, pill, label in ALERT_SEVERITY_LANES:
        design = (
            "### 6.1 알림 severity lane\n\n"
            "| 행 modifier | lane token | pill과 텍스트 |\n"
            "|---|---|---|\n"
            f"| `alert-row-{lane}` | `{token}` | `{pill}` / `{label}` |\n"
            "\n## 7. Card Rules\n"
        )
        style = (
            f".alert-row-{lane} {{ "
            f"border-inline-start: 3px solid var({token}); }}\n"
        )
        preview = (
            f'<tr class="alert-row-{lane}"><td>'
            f'<span class="pill {pill}">{label}</span>'
            "</td></tr>"
        )
        stale_design = (
            "### 6.1 알림 severity lane\n\n"
            "| 행 modifier | lane token | pill과 텍스트 |\n"
            "|---|---|---|\n"
            "<!--\n"
            f"| `alert-row-{lane}` | `{token}` | `{pill}` / `{label}` |\n"
            "-->\n"
            f"| `alert-row-{lane}` | `--st-other` | "
            "`pill-other` / `MISMATCH` |\n"
            "\n## 7. Card Rules\n"
        )
        stale_style = (
            f"/* {style.strip()} */\n"
            f".alert-row-{lane} {{ "
            "border-inline-start: 3px solid var(--st-other); }\n"
        )
        stale_preview_wrong_pill = (
            f"<!-- {preview} -->\n"
            f'<tr class="alert-row-{lane}"><td>'
            f'<span class="pill pill-other">{label}</span>'
            "</td></tr>"
        )
        stale_preview_wrong_label = (
            f"<!-- {preview} -->\n"
            f'<tr class="alert-row-{lane}"><td>'
            f'<span class="pill {pill}">MISMATCH</span>'
            "</td></tr>"
        )
        require(
            f"self-test must accept valid {lane} DESIGN mapping",
            has_alert_design_mapping(design, lane, token, pill, label),
        )
        require(
            f"self-test must accept valid {lane} CSS mapping",
            has_alert_css_mapping(style, lane, token),
        )
        require(
            f"self-test must accept valid {lane} preview mapping",
            has_alert_preview_mapping(preview, lane, pill, label),
        )
        require(
            f"self-test must reject a swapped {lane} CSS token",
            not has_alert_css_mapping(
                style.replace(token, "--st-other"),
                lane,
                token,
            ),
        )
        require(
            f"self-test must reject a swapped {lane} DESIGN token",
            not has_alert_design_mapping(
                design.replace(token, "--st-other"),
                lane,
                token,
                pill,
                label,
            ),
        )
        require(
            f"self-test must reject a mismatched {lane} preview pill",
            not has_alert_preview_mapping(
                preview.replace(pill, "pill-other"),
                lane,
                pill,
                label,
            ),
        )
        require(
            f"self-test must reject a mismatched {lane} preview label",
            not has_alert_preview_mapping(
                preview.replace(f">{label}<", ">MISMATCH<"),
                lane,
                pill,
                label,
            ),
        )
        require(
            f"self-test must ignore stale commented {lane} DESIGN mappings",
            not has_alert_design_mapping(
                stale_design,
                lane,
                token,
                pill,
                label,
            ),
        )
        require(
            f"self-test must ignore stale commented {lane} CSS mappings",
            not has_alert_css_mapping(stale_style, lane, token),
        )
        require(
            f"self-test must ignore a stale commented {lane} preview pill",
            not has_alert_preview_mapping(
                stale_preview_wrong_pill,
                lane,
                pill,
                label,
            ),
        )
        require(
            f"self-test must ignore a stale commented {lane} preview label",
            not has_alert_preview_mapping(
                stale_preview_wrong_label,
                lane,
                pill,
                label,
            ),
        )
    print("[PASS] alert severity lane matcher self-test")
    return 0


def main() -> int:
    design = read("DESIGN.md")
    agents = read("AGENTS.md")
    guide = read("docs/GUIDE.md")
    ui_guide = read("ui/guide-content.md")
    style = read("ui/style.css")
    docs_html = read("ui/docs.html")
    help_js = read("ui/modules/help.js")
    nav_js = read("ui/modules/nav.js")
    shell_js = read("ui/modules/shell.js")
    app_js = read("ui/app.js")
    i18n_js = read("ui/i18n.js")
    uxlib_js = read("ui/modules/uxlib.js")
    accounts_js = read("ui/modules/accounts.js")
    theme = read("ui/modules/theme.js")
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
    require_all("DESIGN.md", design, REQUIRED_THEME_IDS)
    require_all("ui/modules/theme.js", theme, REQUIRED_THEME_IDS)
    docs_palette = css_custom_properties(docs_html, ":root")
    dashboard_palette = css_custom_properties(style, '[data-theme="supanova"]')
    for docs_name, dashboard_name, expected in DOCS_DASHBOARD_PALETTE:
        require(
            f"ui/docs.html --docs-{docs_name} must remain {expected}",
            docs_palette.get(f"docs-{docs_name}") == expected,
        )
        require(
            f"default Supanova --{dashboard_name} must match docs --docs-{docs_name}",
            dashboard_palette.get(dashboard_name) == expected,
        )
        require(
            f"DESIGN.md must publish default Supanova --{dashboard_name} {expected}",
            f"`--{dashboard_name}`" in design and f"`{expected}`" in design,
        )
    require_all(
        "DESIGN.md",
        design,
        (
            "#/restguide",
            "docs.html#14-rest-api",
            "별도 REST reader",
            "호환 진입점",
        ),
    )
    require_all(
        "ui/docs.html",
        docs_html,
        (
            "site-header-inner",
            "backdrop-filter: blur(18px)",
            "reader-mobile-toc",
            "reader-code-toolbar",
            "background: #0d0d0d",
            "behavior: 'instant'",
        ),
    )
    require(
        "retired REST guide must not remain in canonical product surfaces",
        all(
            "restguide" not in text
            for text in (help_js, nav_js, shell_js, app_js, i18n_js)
        ),
    )
    require(
        "retired REST guide renderer and CSS must be deleted",
        "renderRestGuide" not in help_js and ".rest-docs" not in style,
    )
    require(
        "retired REST bookmark must replace into the canonical chapter",
        re.search(r"restguide\s*:\s*['\"]\/ui\/docs\.html#14-rest-api['\"]", uxlib_js) is not None,
    )
    require(
        "API management must link directly to the canonical REST chapter",
        re.search(r"href\s*:\s*['\"]\/ui\/docs\.html#14-rest-api['\"]", accounts_js) is not None
        and "navigateTo('restguide')" not in accounts_js,
    )
    require(
        "retired service guide must not remain in the in-app help renderer",
        all(
            term not in help_js
            for term in ("renderServiceGuide", "filterGuide", "SERVICE GUIDE")
        ),
    )
    require(
        "DESIGN.md must document all four runtime Supanova themes",
        "허용 theme id" in design and "4개" in design,
    )
    for lane, token, pill, label in ALERT_SEVERITY_LANES:
        require(
            f"DESIGN.md must map alert-row-{lane} to {token}, {pill}, and {label}",
            has_alert_design_mapping(design, lane, token, pill, label),
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
            f"{path} must document both physical bridge uplink contracts",
            all(
                term in text
                for term in (
                    "bridge/dedicated",
                    "bridge/shared",
                    "--confirm-dedicated-uplink",
                    "--confirm-shared-uplink",
                    "/var/lib/purecvisor/networks/<bridge>.json",
                )
            ),
        )
        require(
            f"{path} must mark the shared guest MAC as non-reserved validation data",
            "02:16:3e:44:55:66" in text and "예약값" in text,
        )
        require(
            f"{path} must not advertise the nonexistent network info CLI route",
            "pcvctl network info" not in text,
        )
        require(
            f"{path} must not advertise the retired in-app service guide",
            "| 도움말 | 서비스 가이드" not in text,
        )
        require(
            f"{path} must not publish the retired multi-control-plane notes",
            "멀티 제어면 참고 기록" not in text,
        )
        require_all(
            path,
            text,
            (
                "### 네트워크 서비스 개요",
                "### 네트워크 서비스 활용 예제",
                "#### 예제 1 — NAT 네트워크에 VM 연결",
                "#### 예제 2 — Local VPC의 웹 서비스를 허용된 네트워크에 게시",
                "--backend linux --subnet-name web",
                'pcvctl vpc service-publish "$ATTACHMENT_ID"',
            ),
        )

    require(
        "ui/docs.html must not publish the retired multi-control-plane notes",
        "멀티 제어면 참고 기록" not in docs_html,
    )

    require_all(
        "ui/guide-content.md",
        ui_guide,
        (
            "### 14.7 브라우저 푸시 (SP2b)",
            "PushSubscription.toJSON()",
            "/api/v1/push/vapid/rotate",
            "### 14.8 2.0 RPC 예제 (RPC 전용)",
            "tenant_overlay.create",
            "debug.trace.start",
        ),
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
        "style.css must keep reviewed modal overlay controls responsive",
        all(
            term in style
            for term in (
                ".cmd-palette-box",
                ".cmd-item[aria-selected=\"true\"]",
                ".iso-browser-file:focus-visible",
                ".iso-browser-actions .btn { min-height: 40px; }",
                ".kbd-grid { grid-template-columns: 1fr;",
            )
        ),
    )
    for lane, token, pill, label in ALERT_SEVERITY_LANES:
        require(
            f"style.css must map alert-row-{lane} to {token}",
            has_alert_css_mapping(style, lane, token),
        )
        require(
            f"preview alert-row-{lane} must contain {pill} with {label}",
            has_alert_preview_mapping(preview, lane, pill, label),
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
    if sys.argv[1:] == ["--self-test"]:
        sys.exit(self_test_alert_lane_matchers())
    if sys.argv[1:]:
        print(
            "ERROR: usage: check_design_md.py [--self-test]",
            file=sys.stderr,
        )
        sys.exit(2)
    sys.exit(main())
