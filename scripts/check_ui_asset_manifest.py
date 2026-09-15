#!/usr/bin/env python3










from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_deb_apparmor import strip_comments


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_MANIFEST = ROOT / "packaging/ui-assets.manifest"
DEFAULT_DEPLOY = ROOT / "scripts/deploy.sh"
DEFAULT_BUILD = ROOT / "packaging/deb/build-deb.sh"

SAFE_SOURCE = re.compile(r"^ui/[A-Za-z0-9][A-Za-z0-9._-]*$")
SAFE_TARGET = re.compile(r"^(?:ui|fallback)/[A-Za-z0-9][A-Za-z0-9._-]*$")
REQUIRED_ENTRIES = {
    ("ui/offline.html", "ui/offline.html", "replace"),
    ("ui/maintenance.html", "fallback/maintenance.html", "replace"),
    ("ui/maintenance-status.json", "fallback/maintenance-status.json", "seed"),
}
REQUIRED_UI_TARGETS = {
    "ui/index.html",
    "ui/style.css",
    "ui/app.bundle.js",
    "ui/sw.js",
    "ui/i18n.js",
    "ui/manifest.json",
    "ui/offline.html",
}


@dataclass(frozen=True)
class Entry:
    source: str
    target: str
    policy: str


def parse_manifest(path: Path, source_root: Path) -> tuple[list[Entry], list[str]]:

    bad: list[str] = []
    entries: list[Entry] = []
    targets: set[str] = set()
    if not path.is_file():
        return [], [f"manifest가 없다: {path}"]

    for line_no, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        fields = line.split()
        if len(fields) != 3:
            bad.append(f"manifest:{line_no}: 필드는 source target policy 정확히 3개여야 한다")
            continue
        source, target, policy = fields
        if not SAFE_SOURCE.fullmatch(source):
            bad.append(f"manifest:{line_no}: 안전하지 않은 source: {source}")
        if not SAFE_TARGET.fullmatch(target):
            bad.append(f"manifest:{line_no}: 안전하지 않은 target: {target}")
        if policy not in {"replace", "seed"}:
            bad.append(f"manifest:{line_no}: 알 수 없는 policy: {policy}")
        if target in targets:
            bad.append(f"manifest:{line_no}: 중복 target: {target}")
        targets.add(target)
        if not (source_root / source).is_file():
            bad.append(f"manifest:{line_no}: source 파일이 없다: {source}")
        entries.append(Entry(source, target, policy))

    actual = {(e.source, e.target, e.policy) for e in entries}
    for required in sorted(REQUIRED_ENTRIES - actual):
        bad.append("필수 offline/fallback entry 누락: " + " ".join(required))
    missing_ui = sorted(REQUIRED_UI_TARGETS - {e.target for e in entries})
    if missing_ui:
        bad.append("필수 UI root target 누락: " + ", ".join(missing_ui))
    for entry in entries:
        if entry.policy == "seed" and not entry.target.startswith("fallback/"):
            bad.append(f"seed policy는 fallback runtime 상태에만 허용한다: {entry.target}")
    return entries, bad


def check_contract(
    manifest: Path,
    deploy: Path,
    build: Path,
    source_root: Path,
) -> tuple[list[str], dict[str, int]]:
    entries, bad = parse_manifest(manifest, source_root)
    if not deploy.is_file():
        bad.append(f"deploy script가 없다: {deploy}")
        deploy_raw = ""
    else:
        deploy_raw = deploy.read_text(encoding="utf-8", errors="replace")
    if not build.is_file():
        bad.append(f"Debian build script가 없다: {build}")
        build_raw = ""
    else:
        build_raw = build.read_text(encoding="utf-8", errors="replace")

    deploy_code = strip_comments(deploy_raw)
    build_code = strip_comments(build_raw)

    deploy_contract = {
        'UI_ASSET_MANIFEST="$PROJECT_DIR/packaging/ui-assets.manifest"':
            "direct deploy가 정본 manifest 경로를 선언하지 않는다",
        "load_ui_asset_manifest":
            "direct deploy가 manifest를 검증·로드하지 않는다",
        '"$RUNTIME_STAGE/ui-assets.manifest"':
            "remote secure stage에 manifest를 전송하지 않는다",
        "install_ui_manifest_assets":
            "remote가 manifest target을 설치하지 않는다",
        "install_local_ui_manifest_assets":
            "local deploy가 manifest target을 설치하지 않는다",
        '[[ "$policy" == "seed" ]] && sudo test -e "$target_path"':
            "direct deploy가 기존 maintenance status seed를 보존하지 않는다",
    }
    for token, message in deploy_contract.items():
        if token not in deploy_code:
            bad.append(message)
    if deploy_code.count('sudo mv -fT -- "$staged_path" "$target_path"') < 2:
        bad.append("remote/local direct deploy 중 하나의 UI 파일 교체가 원자 rename이 아니다")

    build_contract = {
        'UI_ASSET_MANIFEST="$PROJECT_DIR/packaging/ui-assets.manifest"':
            "Debian build가 정본 manifest 경로를 선언하지 않는다",
        "stage_ui_manifest_assets":
            "Debian stage가 manifest target을 조립하지 않는다",
        "fallback/.defaults/maintenance-status.json":
            "Debian package에 maintenance status seed 원본이 없다",
        'STATUS_TARGET="/usr/local/share/purecvisor/fallback/maintenance-status.json"':
            "postinst가 runtime maintenance status target을 선언하지 않는다",
        'mv -fT -- "$status_tmp" "$STATUS_TARGET"':
            "postinst의 초기 maintenance status 설치가 원자 rename이 아니다",
        '[ ! -e "$STATUS_TARGET" ]':
            "postinst가 기존 runtime maintenance status를 보존하지 않는다",
    }
    for token, message in build_contract.items():
        if token not in build_code:
            bad.append(message)

    return bad, {
        "entries": len(entries),
        "ui_targets": sum(e.target.startswith("ui/") for e in entries),
        "fallback_targets": sum(e.target.startswith("fallback/") for e in entries),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="UI offline/fallback manifest contract gate")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--deploy", type=Path, default=DEFAULT_DEPLOY)
    parser.add_argument("--build-script", type=Path, default=DEFAULT_BUILD)
    parser.add_argument("--source-root", type=Path, default=ROOT)
    args = parser.parse_args(argv)

    bad, info = check_contract(args.manifest, args.deploy, args.build_script, args.source_root)
    print(
        "[check-ui-asset-manifest] "
        f"entries={info['entries']} ui={info['ui_targets']} fallback={info['fallback_targets']}"
    )
    if bad:
        print("\033[31m[FAIL]\033[0m UI 정적 자산 manifest 계약 위반:", file=sys.stderr)
        for item in bad:
            print(f"  - {item}", file=sys.stderr)
        return 1
    print("\033[32m[PASS]\033[0m direct/deb 동일 manifest + atomic replace + status seed 보존")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
