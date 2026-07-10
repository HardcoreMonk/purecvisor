#!/usr/bin/env python3
"""
backup.replicate shell-free contract check.

The replication path accepts a remote host and SSH username. Those values must
never be interpolated into a local shell command. Keep this check narrow so
other legacy backup paths can be remediated separately without weakening this
high-risk RPC.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
BACKUP_C = REPO_ROOT / "src" / "modules" / "backup" / "backup_scheduler.c"

FUNCTIONS = (
    "_remote_snapshot_exists",
    "_verify_replication",
    "_enforce_remote_retention",
    "pcv_backup_replicate",
)

FORBIDDEN_PATTERNS = (
    (re.compile(r'"\s*/?bin/sh\s*"\s*,\s*"-c"'), "local /bin/sh -c"),
    (re.compile(r'"\s*sh\s*"\s*,\s*"-c"'), "local sh -c"),
    (re.compile(r"\bg_shell_quote\s*\("), "shell quoting in replication path"),
)


def extract_c_function_body(text: str, name: str) -> str | None:
    match = re.search(rf"\b{name}\s*\([^;]*?\)\s*\{{", text, re.DOTALL)
    if not match:
        return None

    start = match.end() - 1
    depth = 0
    for pos in range(start, len(text)):
        if text[pos] == "{":
            depth += 1
        elif text[pos] == "}":
            depth -= 1
            if depth == 0:
                return text[start + 1:pos]
    return None


def main() -> int:
    text = BACKUP_C.read_text(errors="replace")
    failures: list[str] = []

    for fn in FUNCTIONS:
        body = extract_c_function_body(text, fn)
        if body is None:
            failures.append(f"{fn}: function body not found")
            continue
        for pattern, label in FORBIDDEN_PATTERNS:
            if pattern.search(body):
                failures.append(f"{fn}: forbidden {label}")

    body = extract_c_function_body(text, "pcv_backup_replicate")
    if body is None or "pcv_spawn_pipe_sync" not in body:
        failures.append("pcv_backup_replicate: must stream zfs send via pcv_spawn_pipe_sync")

    if failures:
        print("[FAIL] backup.replicate shell-free contract violated")
        for failure in failures:
            print(f"  - {failure}")
        return 1

    print("[PASS] backup.replicate uses argv/splice without local shell")
    return 0


if __name__ == "__main__":
    sys.exit(main())
