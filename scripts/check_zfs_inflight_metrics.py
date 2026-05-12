#!/usr/bin/env python3




from __future__ import annotations

import re
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
    exporter_h = read("src/modules/daemons/prometheus_exporter.h")
    exporter_c = read("src/modules/daemons/prometheus_exporter.c")
    zfs_driver_c = read("src/modules/storage/zfs_driver.c")

    require(
        "prometheus exporter header exposes inflight lock helper",
        "pcv_prom_zfs_inflight_lock_observe" in exporter_h,
    )
    require(
        "prometheus exporter source implements inflight lock helper",
        "pcv_prom_zfs_inflight_lock_observe" in exporter_c,
    )

    for token in (
        "purecvisor_zfs_inflight_lock_acquired_total",
        "purecvisor_zfs_inflight_lock_wait_ms_bucket",
        "purecvisor_zfs_inflight_lock_wait_ms_sum",
        "purecvisor_zfs_inflight_lock_wait_ms_count",
    ):
        require(f"prometheus exporter registers {token}", token in exporter_c)

    require(
        "zfs driver includes prometheus exporter",
        '#include "../daemons/prometheus_exporter.h"' in zfs_driver_c,
    )
    for outcome in ("ok", "busy", "error"):
        require(
            f"zfs driver records {outcome} inflight lock outcome",
            re.search(
                rf'pcv_prom_zfs_inflight_lock_observe\s*\([^;]*"{outcome}"',
                zfs_driver_c,
                re.DOTALL,
            )
            is not None,
        )

    print("[PASS] ZFS inflight lock metric static contract is present")
    return 0


if __name__ == "__main__":
    sys.exit(main())
