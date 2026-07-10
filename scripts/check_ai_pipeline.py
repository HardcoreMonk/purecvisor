#!/usr/bin/env python3
"""
check_ai_pipeline.py — ADR-0020 회귀 방지 정적 검증 (F-17)

BUG-20 재발 방지:
- anomaly_detector.c의 _emit_alert()가 pcv_healing_on_anomaly()를 호출하는가
- virt_events.c의 VM 라이프사이클 콜백이 pcv_healing_on_anomaly()를 호출하는가
- workload_predict.c의 pcv_predict_evaluate()가 pcv_healing_on_prediction()을 호출하는가
- vm-unresponsive 정책의 trigger_metric이 빈 문자열이 아닌가

pre-commit hook에서 AI 관련 C 파일이 변경되면 자동 실행.
실패 시 exit 1로 커밋 차단.
"""
from __future__ import annotations
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

CHECKS = [
# The AI pipeline has historically failed by becoming disconnected rather than
# by throwing a local error. These checks therefore look for producer -> healing
# call sites and the non-empty vm-unresponsive trigger metric that makes the
# policy reachable.
    {
        "name": "anomaly_detector → self_healing",
        "file": "src/modules/ai/anomaly_detector.c",
        "pattern": r"pcv_healing_on_anomaly\s*\(",
        "min_count": 1,
        "rule": "ADR-0020 규칙 1",
    },
    {
        "name": "virt_events → self_healing (vm-unresponsive)",
        "file": "src/modules/daemons/virt_events.c",
        "pattern": r"pcv_healing_on_anomaly\s*\(\s*\"vm-unresponsive\"",
        "min_count": 1,
        "rule": "ADR-0020 규칙 2",
    },
    {
        "name": "workload_predict → self_healing",
        "file": "src/modules/ai/workload_predict.c",
        "pattern": r"pcv_healing_on_prediction\s*\(",
        "min_count": 1,
        "rule": "ADR-0020 규칙 3",
    },
    {
        "name": "vm-unresponsive 정책 trigger_metric 비공백",
        "file": "src/modules/ai/self_healing.c",
        # _add_policy("vm-unresponsive", "vm-unresponsive", ...)
        # 빈 문자열은 규칙 4 위반
        "pattern": r'_add_policy\s*\(\s*"vm-unresponsive"\s*,\s*"vm-unresponsive"',
        "min_count": 1,
        "rule": "ADR-0020 규칙 4",
    },
]

RED = "\033[31m"
GREEN = "\033[32m"
YELLOW = "\033[33m"
RESET = "\033[0m"


def check_one(c: dict) -> bool:
    """Returns True if check passes."""
    path = REPO / c["file"]
    if not path.exists():
        print(f"{RED}[FAIL]{RESET} {c['name']} — file not found: {c['file']}")
        return False
    content = path.read_text(encoding="utf-8", errors="replace")
    matches = re.findall(c["pattern"], content)
    count = len(matches)
    if count >= c["min_count"]:
        print(f"{GREEN}[PASS]{RESET} {c['name']} ({count} call(s))")
        return True
    print(
        f"{RED}[FAIL]{RESET} {c['name']} — expected >= {c['min_count']} "
        f"matches of /{c['pattern']}/, found {count}"
    )
    print(f"       {YELLOW}→ {c['rule']}{RESET}")
    print(f"       {YELLOW}→ docs/adr/0020-ai-ops-pipeline-wiring.md 참조{RESET}")
    return False


def main() -> int:
    print("============================================================")
    print("ADR-0020 AI Ops 파이프라인 정적 검증 (BUG-20 회귀 방지)")
    print("============================================================")
    failed = 0
    for c in CHECKS:
        if not check_one(c):
            failed += 1
    print("------------------------------------------------------------")
    total = len(CHECKS)
    passed = total - failed
    if failed:
        print(f"{RED}FAIL{RESET}: {passed}/{total} checks passed")
        print()
        print("BUG-20 재발 위험: Producer → Self-Healing 호출 체인이 끊어졌습니다.")
        print("이 호출이 누락되면 anomaly/predict/vm-crash 이벤트가 self_healing에")
        print("도달하지 못해 AI Ops 전체가 dead code가 됩니다.")
        return 1
    print(f"{GREEN}PASS{RESET}: {passed}/{total} checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
