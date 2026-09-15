#!/usr/bin/env python3





from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
GATE = ROOT / "scripts/check_ui_asset_manifest.py"
MANIFEST = ROOT / "packaging/ui-assets.manifest"
DEPLOY = ROOT / "scripts/deploy.sh"
BUILD = ROOT / "packaging/deb/build-deb.sh"


def run_fixture(*, manifest: str | None = None, deploy: str | None = None,
                build: str | None = None) -> tuple[int, str]:
    with tempfile.TemporaryDirectory(prefix="pcv-ui-assets-") as tmp:
        tmp_path = Path(tmp)
        manifest_path = tmp_path / "ui-assets.manifest"
        deploy_path = tmp_path / "deploy.sh"
        build_path = tmp_path / "build-deb.sh"
        manifest_path.write_text(manifest if manifest is not None else MANIFEST.read_text(), encoding="utf-8")
        deploy_path.write_text(deploy if deploy is not None else DEPLOY.read_text(), encoding="utf-8")
        build_path.write_text(build if build is not None else BUILD.read_text(), encoding="utf-8")
        proc = subprocess.run(
            [
                sys.executable, str(GATE),
                "--manifest", str(manifest_path),
                "--deploy", str(deploy_path),
                "--build-script", str(build_path),
                "--source-root", str(ROOT),
            ],
            capture_output=True,
            text=True,
        )
        return proc.returncode, proc.stdout + proc.stderr


def test_current_tree_passes() -> None:
    rc, out = run_fixture()
    assert rc == 0 and "[PASS]" in out, out


def test_offline_entry_removed_fails() -> None:
    src = MANIFEST.read_text()
    mutated = src.replace("ui/offline.html ui/offline.html replace\n", "", 1)
    rc, out = run_fixture(manifest=mutated)
    assert rc == 1 and "offline" in out, out


def test_target_traversal_fails() -> None:
    src = MANIFEST.read_text()
    mutated = src.replace("fallback/maintenance.html", "fallback/../maintenance.html", 1)
    rc, out = run_fixture(manifest=mutated)
    assert rc == 1 and "안전하지 않은 target" in out, out


def test_duplicate_target_fails() -> None:
    src = MANIFEST.read_text() + "ui/offline.html ui/index.html replace\n"
    rc, out = run_fixture(manifest=src)
    assert rc == 1 and "중복 target" in out, out


def test_remote_atomic_rename_removed_fails() -> None:
    src = DEPLOY.read_text()
    old = 'sudo mv -fT -- "$staged_path" "$target_path"'
    assert old in src, "self-test anchor missing from deploy.sh"
    rc, out = run_fixture(deploy=src.replace(old, 'sudo cp "$staged_path" "$target_path"'))
    assert rc == 1 and "원자 rename" in out, out


def test_deb_seed_preservation_removed_fails() -> None:
    src = BUILD.read_text()
    old = '[ ! -e "$STATUS_TARGET" ]'
    assert old in src, "self-test anchor missing from build-deb.sh"
    rc, out = run_fixture(build=src.replace(old, "true", 1))
    assert rc == 1 and "기존 runtime" in out, out


if __name__ == "__main__":
    tests = [value for name, value in sorted(globals().items())
             if name.startswith("test_") and callable(value)]
    failed = 0
    for test in tests:
        try:
            test()
            print(f"OK   {test.__name__}")
        except AssertionError as error:
            failed += 1
            print(f"FAIL {test.__name__}: {error}")
    print(f"[test_ui_asset_manifest] {len(tests) - failed}/{len(tests)} passed")
    raise SystemExit(1 if failed else 0)
