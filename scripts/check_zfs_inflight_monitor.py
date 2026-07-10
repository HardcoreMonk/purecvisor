#!/usr/bin/env python3
"""ZFS inflight lock monitor panel static contract guard."""

from __future__ import annotations

import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent


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
    monitor_js = read("ui/modules/monitor.js")
    app_bundle = read("ui/app.bundle.js")

    for token in (
        "purecvisor_zfs_inflight_lock_acquired_total",
        "purecvisor_zfs_inflight_lock_wait_ms_sum",
        "purecvisor_zfs_inflight_lock_wait_ms_count",
        "zfsLocks",
    ):
        require(f"monitor parses {token}", token in monitor_js)

    for token in ("ZFS inflight lock", "No ZFS inflight lock samples yet", "Avg wait"):
        require(f"monitor renders {token}", token in monitor_js)

    for token in (
        "ZFS inflight lock",
        "purecvisor_zfs_inflight_lock_acquired_total",
    ):
        require(f"generated bundle includes {token}", token in app_bundle)

    print("[PASS] ZFS inflight lock monitor static contract is present")
    return 0


if __name__ == "__main__":
    sys.exit(main())
