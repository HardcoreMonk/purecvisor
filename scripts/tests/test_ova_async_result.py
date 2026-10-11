#!/usr/bin/env python3



















import os
import subprocess
import sys
import tempfile
from pathlib import Path

GATE = Path(__file__).resolve().parent.parent / "check_ova_async_result.py"
DISPATCHER = Path(__file__).resolve().parent.parent.parent / "src" / "api" / "dispatcher.c"

POOL_CHECK = "    if (pool && pool[0] && !_valid_vm_storage_pool(pool)) {"
LOCK_CALL = "        if (!lock_vm_operation(name, VM_OP_CREATING, &import_lock_err)) {"
PREFLIGHT = "        virDomainPtr existing = virDomainLookupByName(conn, name);\n"
DISK_EXISTS_UNLOCK = (
    "        if (disk_exists) {\n"
    "            unlock_vm_operation(name);\n"
)
CTX_FREE_UNLOCK = "    if (ctx->holds_lock) unlock_vm_operation(ctx->vm_name);\n"


def _source() -> str:
    return DISPATCHER.read_text(encoding="utf-8")


def _run_gate_on(text: str) -> int:
    handle = tempfile.NamedTemporaryFile("w", suffix="_dispatcher.c", delete=False)
    handle.write(text)
    handle.close()
    try:
        return subprocess.run([sys.executable, str(GATE), handle.name],
                              capture_output=True, text=True).returncode
    finally:
        os.unlink(handle.name)


def _mutate(old: str, new: str) -> str:
    text = _source()
    assert text.count(old) == 1, f"앵커 포맷 변경? {old!r}"
    return text.replace(old, new)


def test_gate_passes_on_current_tree():

    result = subprocess.run([sys.executable, str(GATE)], capture_output=True, text=True)
    assert result.returncode == 0, f"{result.stdout}\n{result.stderr}"


def test_missing_pool_validation_fails():

    assert _run_gate_on(_mutate(POOL_CHECK, "    if (0) {")) == 1


def test_missing_name_lock_fails():

    assert _run_gate_on(_mutate(LOCK_CALL, "        if (0) {")) == 1


def test_lock_after_preflight_fails():

    text = _source()
    assert text.count(PREFLIGHT) == 1 and text.count(LOCK_CALL) == 1
    mutated = text.replace(PREFLIGHT, "        virDomainPtr existing = NULL;\n")
    mutated = mutated.replace(
        LOCK_CALL,
        "        virConnectPtr pre = NULL; (void)virDomainLookupByName(pre, name);\n" + LOCK_CALL)
    assert _run_gate_on(mutated) == 1


def test_missing_unlock_on_rejection_fails():

    assert _run_gate_on(_mutate(DISK_EXISTS_UNLOCK, "        if (disk_exists) {\n")) == 1


def test_ctx_free_without_unlock_fails():

    assert _run_gate_on(_mutate(CTX_FREE_UNLOCK, "")) == 1


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
    print(f"[test_ova_async_result] {len(tests) - failed}/{len(tests)} passed")
    sys.exit(1 if failed else 0)
