
"""
ADR-0023 vm.clone cleanup static guard.

The live ZFS rollback path cannot be covered by the normal unit test runner.
This guard keeps the worker structure from regressing silently:

* accepted responses expose the same job id used by WS completion.
* snapshot-created failure paths call the common cleanup helper.
* the helper destroys the target dataset before the source temporary snapshot.
* file disk clone and guest reset stay wired into the worker.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DISPATCHER_C = REPO_ROOT / "src" / "api" / "dispatcher.c"
VM_CLONE_PLAN_C = REPO_ROOT / "src" / "modules" / "virt" / "vm_clone_plan.c"

def _read(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except OSError as exc:
        print(f"ERROR: failed to read {path}: {exc}", file=sys.stderr)
        sys.exit(2)

def _function_body(text: str, name: str) -> str:
    pattern = re.compile(rf"static\s+\w+\s+{re.escape(name)}\s*\([^)]*\)\s*\{{")
    match = pattern.search(text)
    if not match:
        print(f"FAIL: missing {name}()", file=sys.stderr)
        sys.exit(1)

    depth = 0
    start = match.end() - 1
    for pos in range(start, len(text)):
        char = text[pos]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return text[start + 1:pos]

    print(f"ERROR: could not parse {name}()", file=sys.stderr)
    sys.exit(2)

def main() -> int:
    text = _read(DISPATCHER_C)
    plan_text = _read(VM_CLONE_PLAN_C)

    cleanup_body = _function_body(text, "_vm_clone_cleanup_failed_artifacts")
    destroy_dataset = cleanup_body.find("_vm_clone_destroy_dataset_recursive")
    destroy_snapshot = cleanup_body.find("_vm_clone_destroy_source_snapshot")
    remove_file = cleanup_body.find("_vm_clone_remove_target_file")
    if destroy_dataset < 0 or destroy_snapshot < 0 or remove_file < 0:
        print("FAIL: cleanup helper must destroy target dataset/source snapshot and remove target files")
        return 1
    if destroy_dataset > destroy_snapshot:
        print("FAIL: cleanup helper must destroy target dataset before source snapshot")
        return 1

    thread_body = _function_body(text, "_vm_clone_thread")
    if "source_snapshot_exists" not in thread_body:
        print("FAIL: vm.clone worker must track source snapshot lifetime")
        return 1
    cleanup_calls = thread_body.count("_vm_clone_cleanup_failed_artifacts(ctx")
    if cleanup_calls < 5:
        print("FAIL: expected snapshot-created failure paths to use cleanup helper")
        return 1
    if "pcv_vm_clone_copy_file_disk" not in thread_body:
        print("FAIL: vm.clone worker must execute qcow2/raw file disk copy")
        return 1
    if "pcv_vm_clone_reset_guest_identity" not in thread_body:
        print("FAIL: vm.clone worker must execute guest identity reset when requested")
        return 1

    for token in [
        "virt-sysprep",
        "virt-customize",
        "virt-filesystems",
        "guestfish",
        "set-uuid-random",
        "e2fsck-f",
        "fs-uuids",
        "lvm-uuids",
        "lvm-system-devices",
        "update-initramfs",
        "dracut",
        "grub2-mkconfig",
        "/.autorelabel",
    ]:
        if token not in plan_text:
            print(f"FAIL: guest reset command contract is missing {token}")
            return 1

    if 'json_object_set_string_member(obj, "job_id", job_id)' not in text:
        print("FAIL: accepted response must include the vm.clone job_id")
        return 1
    if 'json_object_set_boolean_member(obj, "guest_reset", guest_reset)' not in text:
        print("FAIL: accepted response must include whether guest_reset will run")
        return 1

    print("[PASS] vm.clone cleanup guard structure is present")
    return 0

if __name__ == "__main__":
    sys.exit(main())
