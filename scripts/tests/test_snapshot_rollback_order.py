#!/usr/bin/env python3



















import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import check_snapshot_rollback_order as gate

GATE = Path(__file__).resolve().parent.parent / "check_snapshot_rollback_order.py"
SOURCE = gate.DEFAULT_SOURCE

EXISTS_GUARD = "    if (!_zfs_snapshot_exists(d->vm_name, d->snap_name)) {"
LIBVIRT_LOOKUP = (
    "    virDomainSnapshotPtr snap = virDomainSnapshotLookupByName(dom, snap_name, 0);\n"
)
LIBVIRT_REVERT = "    int rc = virDomainRevertToSnapshot(snap, 0);"


def _run_gate_on(text: str) -> int:
    handle = tempfile.NamedTemporaryFile("w", suffix="_handler_snapshot.c", delete=False)
    handle.write(text)
    handle.close()
    try:
        result = subprocess.run([sys.executable, str(GATE), handle.name],
                                capture_output=True, text=True)
        return result.returncode
    finally:
        os.unlink(handle.name)


def _source() -> str:
    return SOURCE.read_text(encoding="utf-8")


def test_gate_passes_on_current_tree():

    result = subprocess.run([sys.executable, str(GATE)], capture_output=True, text=True)
    assert result.returncode == 0, f"{result.stdout}\n{result.stderr}"
    assert gate.check_text(_source()) == []


def test_missing_existence_check_fails():

    text = _source()
    assert EXISTS_GUARD in text, "존재 확인 가드 포맷 변경?"
    mutated = text.replace(EXISTS_GUARD, "    if (0) {")
    assert gate.check_text(mutated)
    assert _run_gate_on(mutated) == 1


def test_existence_check_after_shutdown_fails():

    text = _source()
    worker = gate.function_body(text, "_rollback_worker")
    start = worker.index(EXISTS_GUARD)
    end = worker.index("    }\n", start) + len("    }\n")
    guard_block = worker[start:end]
    anchor = '    gchar *dataset = g_strdup_printf("%s/%s@%s",\n'
    assert anchor in worker, "ZFS 롤백 단계 주석 포맷 변경?"
    moved = worker.replace(guard_block, "").replace(anchor, guard_block + anchor)
    mutated = text.replace(worker, moved)
    assert any(r.startswith("①") for r in gate.check_text(mutated))
    assert _run_gate_on(mutated) == 1


def test_no_restart_on_rollback_failure_fails():

    text = _source()
    needle = "gboolean restarted = rdom && virDomainCreate(rdom) == 0;"
    assert needle in text, "재기동 포맷 변경?"
    mutated = text.replace(needle, "gboolean restarted = FALSE;")
    assert any(r.startswith("②") for r in gate.check_text(mutated))
    assert _run_gate_on(mutated) == 1


def test_libvirt_lookup_after_shutdown_fails():

    text = _source()
    fallback = gate.function_body(text, "_libvirt_snapshot_rollback")
    assert LIBVIRT_LOOKUP in fallback and LIBVIRT_REVERT in fallback, "폴백 포맷 변경?"
    moved = fallback.replace(LIBVIRT_LOOKUP, "    virDomainSnapshotPtr snap = NULL;\n")
    moved = moved.replace(
        LIBVIRT_REVERT,
        "    snap = virDomainSnapshotLookupByName(dom, snap_name, 0);\n" + LIBVIRT_REVERT)
    mutated = text.replace(fallback, moved)
    assert any(r.startswith("③") for r in gate.check_text(mutated))
    assert _run_gate_on(mutated) == 1


def test_libvirt_not_found_without_audit_fails():

    text = _source()
    fallback = gate.function_body(text, "_libvirt_snapshot_rollback")
    needle = ('        pcv_audit_log(NULL, "vm.snapshot.rollback", vm_id, "fail", '
              'PURE_RPC_ERR_ZFS_OPERATION, 0, "local");\n'
              '        gchar *e = pure_rpc_build_error_response(rpc_id, '
              'PURE_RPC_ERR_ZFS_OPERATION, "Snapshot not found");')
    assert needle in fallback, "미존재 audit 포맷 변경?"
    moved = fallback.replace(needle, needle.split("\n", 1)[1])
    mutated = text.replace(fallback, moved)
    assert any(r.startswith("④") for r in gate.check_text(mutated))
    assert _run_gate_on(mutated) == 1


def test_comment_only_call_does_not_pass():

    text = _source()
    mutated = text.replace(EXISTS_GUARD, "    /* _zfs_snapshot_exists(x) */ if (0) {")
    assert _run_gate_on(mutated) == 1


if __name__ == "__main__":
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_") and callable(v)]
    failed = 0
    for t in tests:
        try:
            t()
            print(f"OK   {t.__name__}")
        except AssertionError as e:
            failed += 1
            print(f"FAIL {t.__name__}: {e}")
    print(f"[test_snapshot_rollback_order] {len(tests) - failed}/{len(tests)} passed")
    sys.exit(1 if failed else 0)
