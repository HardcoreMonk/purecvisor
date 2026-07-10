#!/usr/bin/env python3
# OVA import/export are fire-and-forget RPCs, so accepted != complete.
# This guard keeps the C worker wired to job state, audit result, WS completion,
# and failure cleanup without needing to run qemu-img/tar in CI.
# The patterns below are narrow by design: a missing literal usually means the
# observable async contract changed and needs an explicit review.
"""
OVA import/export async result static guard.

This catches the vm.clone-class failure mode where an accepted fire-and-forget
RPC later records a false success, loses its job id, or skips the WS completion
channel after the worker actually fails.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent
DISPATCHER_C = REPO_ROOT / "src" / "api" / "dispatcher.c"


def _read(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except OSError as exc:
        print(f"ERROR: failed to read {path}: {exc}", file=sys.stderr)
        sys.exit(2)


def _function_body(text: str, name: str) -> str:
    pattern = re.compile(
        rf"static\s+\w+\s+{re.escape(name)}\s*\([^)]*\)\s*\{{"
    )
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


def _fail(message: str) -> int:
    print(f"FAIL: {message}", file=sys.stderr)
    return 1


def main() -> int:
    text = _read(DISPATCHER_C)
    section_match = re.search(
        r"typedef struct \{(?P<ctx>.*?)\} OvaExportCtx;(?P<section>.*?)"
        r"/\* ── OVA Import",
        text,
        re.DOTALL,
    )
    if not section_match:
        return _fail("could not find the OVA export section")

    ctx_body = section_match.group("ctx")
    section = section_match.group("section")
    worker = _function_body(text, "_ova_export_worker")
    import_worker = _function_body(text, "_ova_import_worker")
    import_handler = _function_body(text, "_handle_vm_import_ova")

    required_tokens = {
        "OvaExportCtx carries job_id": "gchar *job_id;" in ctx_body,
        "accepted response exposes job_id": 'json_object_set_string_member(obj, "job_id", job_id)' in section,
        "worker context copies job_id": "ctx->job_id = g_strdup(job_id);" in section,
        "worker updates the accepted job": "pcv_job_set_result(ctx->job_id" in section,
        "worker broadcasts completion": 'pcv_ws_broadcast_job_complete(ctx->job_id, "vm.export.ova"' in section,
        "worker records actual result once": worker.count("_ova_export_record_result(ctx, audit_ok") == 1,
        "worker return follows actual result": "g_task_return_boolean(task, audit_ok)" in worker,
        "qemu-img failure reaches cleanup": "qemu-img convert failed" in worker and "goto ova_cleanup;" in worker,
        "tar failure reaches cleanup": "tar failed" in worker and "goto ova_cleanup;" in worker,
    }
    for description, ok in required_tokens.items():
        if not ok:
            return _fail(description)

    forbidden_tokens = [
        "cleanup 경유 시도 모두 ok",
        "기존 동작 유지",
    ]
    for token in forbidden_tokens:
        if token in section:
            return _fail(f"false-success compatibility comment remains: {token}")

    import_required_tokens = {
        "OVA import has zvol cleanup helper": "_ova_import_destroy_zvol" in text,
        "OVA import fails on zfs create failure": 'PCV_JOB_FAILED, "\\"zfs create failed\\""' in import_worker,
        "OVA import destroys created zvol on worker failure": "_ova_import_destroy_zvol(created_zvol_dataset)" in import_worker,
        "OVA import probes ZFS pool with zfs list": "use_zvol = pcv_spawn_sync(pool_argv" in import_worker,
        "OVA import preflights target domain": "virDomainLookupByName(conn, name)" in import_handler,
        "OVA import preflights raw .raw collision": '"%s/%s.raw"' in import_handler,
        "OVA import rejects disk collision before accepted": "Target VM disk already exists" in import_handler,
    }
    for description, ok in import_required_tokens.items():
        if not ok:
            return _fail(description)

    print("[PASS] OVA import/export async result guard structure is present")
    return 0


if __name__ == "__main__":
    sys.exit(main())
