#!/usr/bin/env python3
from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SELF = "scripts/check_internal_monitoring_absent.py"
SKIP_PARTS = {".git", ".astro", "dist", "node_modules"}
RINGS = {
    "알림 엔진 활성 경보 전이 링": (
        "ALERT_TRANSITION_MAX",
        "AlertTransition",
        "active_alarm_id",
        "pcv_alert_engine_get_active_alarms",
        "pcv_alert_engine_get_transitions",
    ),
    "프로세스 모니터 서비스 프로세스 목록": (
        "PROC_SERVICE_SCAN_LIMIT",
        "PROC_SERVICE_RESULT_LIMIT",
        "ServiceProcInfo",
        "service_procs",
        "_collect_service_processes",
        "pcv_process_monitor_get_service_snapshot",
    ),
    "호스트 지표 이력 링": (
        "EBPF_HOST_HISTORY_MAX",
        "host_history",
        "pcv_ebpf_telemetry_get_host_history",
    ),
}
KEEP = {
    "src/modules/daemons/alert_engine.c": (
        "pcv_alert_engine_get_history",
        "pcv_alert_engine_get_config",
        "pcv_alert_acknowledge",
    ),
    "src/modules/daemons/process_monitor.c": (
        "pcv_process_monitor_get_filtered",
    ),
    "src/modules/daemons/ebpf_telemetry.c": (
        "pcv_ebpf_telemetry_get_host",
    ),
}


def check(texts: dict[str, str]) -> list[str]:
    failures: list[str] = []
    for path in sorted(texts):
        if path == SELF:
            continue
        text = texts[path]
        for ring, tokens in RINGS.items():
            for token in tokens:
                if token in text:
                    failures.append(f"{path}: {ring} 식별자 {token}")
    sources = {path: text for path, text in texts.items()
               if path.startswith("src/") and path.endswith(".c")}
    for module, symbols in KEEP.items():
        if module not in texts:
            failures.append(f"{module}: 검사 대상 모듈 없음")
            continue
        for symbol in symbols:
            if not re.search(rf"(?m)^{re.escape(symbol)}\(", texts[module]):
                failures.append(f"{module}: 공개 조회 함수 정의 없음 {symbol}")
                continue
            call = re.compile(rf"\b{re.escape(symbol)}\(")
            if not any(call.search(text) for path, text in sources.items() if path != module):
                failures.append(f"{module}: 공개 조회 함수 소비자 없음 {symbol}")
    return failures


def tracked_paths(root: Path) -> list[str]:
    result = subprocess.run(["git", "-C", str(root), "ls-files", "-z"],
                            capture_output=True, check=False)
    if result.returncode == 0 and result.stdout:
        return [path for path in result.stdout.decode("utf-8", "surrogateescape").split("\0") if path]
    return [path.relative_to(root).as_posix() for path in root.rglob("*")
            if path.is_file() and not SKIP_PARTS.intersection(path.relative_to(root).parts)]


def read_texts(root: Path) -> dict[str, str]:
    texts: dict[str, str] = {}
    for path in tracked_paths(root):
        file = root / path
        if not file.is_file():
            continue
        with file.open("rb") as handle:
            head = handle.read(4096)
            if b"\0" in head:
                continue
            data = head + handle.read()
        texts[path] = data.decode("utf-8", errors="replace")
    return texts


def main(argv: list[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    root = Path(args[0]).resolve() if args else ROOT
    texts = read_texts(root)
    failures = check(texts)
    if failures:
        for failure in failures:
            print(f"FAIL {failure}")
        print(f"[check-internal-monitoring-absent] FAIL: {len(failures)}건")
        return 1
    print("[check-internal-monitoring-absent] PASS: 공통 데몬 모듈의 내부 관측 버퍼 0건, "
          f"알림 이력·프로세스 목록·호스트 지표 조회 유지 (텍스트 파일 {len(texts)}개)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
