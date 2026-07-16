#!/usr/bin/env python3
"""
ADR-0018 정적 분석 — fire-and-forget audit 위치 검증

[목적]
  dispatcher.c의 g_async_methods 집합에 등록된 RPC 메서드는 반드시
  대응하는 워커 콜백에서 pcv_audit_log()를 호출해야 한다.
  주요 accepted fire-and-forget RPC는 pcv_ws_broadcast_job_complete()도
  함께 호출해야 한다.
  자동 검사를 통해 신규 PR이 audit 무결성을 깨지 않도록 보장.

[검사 대상]
  src/api/dispatcher.c               — g_async_methods 집합 추출
  src/modules/dispatcher/handler_*.c — 핸들러 콜백에서 pcv_audit_log 검색

[방식 — heuristic]
  1. dispatcher.c에서 g_async_methods 등록 메서드 목록 추출
  2. 각 메서드 이름을 가진 pcv_audit_log() 호출 검색
     (예: vm.start → grep 'pcv_audit_log.*"vm.start"' in handler_*.c + dispatcher.c)
  3. 매칭이 0건이면 ❌
  4. CI/pre-commit에서 호출 → 실패 시 차단

[제한]
  완벽한 정적 분석은 아님. 메서드 이름 문자열이 등록 위치 외에 다른 곳에서도
  audit 호출에 사용되어야 한다. 동적 매크로/strdup_printf 패턴은 잡지 못함.
  그러나 ADR-0018 위반의 90%는 잡을 수 있다.

[종료 코드]
  0: async registry/audit/WS completion 계약 통과
  1: 누락된 메서드 발견
  2: 파싱 실패
"""

from __future__ import annotations
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DISPATCHER_C = REPO_ROOT / "src" / "api" / "dispatcher.c"
SEARCH_DIRS = [
    REPO_ROOT / "src" / "api",
    REPO_ROOT / "src" / "modules" / "dispatcher",
    REPO_ROOT / "src" / "modules" / "cloud",       # cloud_migration.c — 동적 cloud.<dir>
    REPO_ROOT / "src" / "modules" / "virt",        # vm_manager 등 비동기 워커
]

REQUIRED_FIRE_AND_FORGET_METHODS = [
    "backup.restore",
    "backup.replicate",
    "backup.export_s3",
    "container.create",
    "container.clone",
    "container.destroy",
    "vm.disk.live_resize",
    "vm.resize_disk",
    "vm.clone",
    "vm.export.ova",
    "vm.import.ova",
]

# This required set is the release gate for high-risk async work. The generic
# g_async_methods scan catches newly registered async methods, while this list
# protects known worker families that must have all three signals: registry,
# actual-result audit, and WebSocket completion.

# g_async_methods 등록 블록 패턴
ASYNC_REG_BLOCK = re.compile(
    r'_async_method_names\[\]\s*=\s*\{(.*?)\};',
    re.DOTALL,
)
METHOD_LITERAL = re.compile(r'"([a-z][a-z0-9_.]+)"')

# audit 호출 패턴 — pcv_audit_log(... "method.name" ...) 또는 pcv_audit_log_rpc("method.name", ...)
AUDIT_CALL_RE = re.compile(
    r'pcv_audit_log(?:_rpc)?\s*\(\s*[^)]*?"([a-z][a-z0-9_.]+)"',
    re.DOTALL,
)
# WS completion 호출 패턴 — 메인컨텍스트 직결(pcv_ws_broadcast_job_complete)과
# 워커 스레드 마샬링 변형(_mt, A2-2 libsoup 스레드 어피니티, 커밋 09d66ae) 둘 다 인식.
# `_mt` 변형을 누락하면 backup.restore/replicate/export_s3·vm.clone/export.ova/
# import.ova·vm.resize_disk·vm.disk.live_resize 처럼 워커에서 `_mt`로만 완료를
# 브로드캐스트하는 fire-and-forget 메서드가 "WS completion 누락" 위음성으로 잡혀
# 게이트가 거짓 실패한다.
WS_COMPLETE_RE = re.compile(
    r'pcv_ws_broadcast_job_complete(?:_mt)?\s*\(\s*[^;]*?"([a-z][a-z0-9_.]+)"',
    re.DOTALL,
)

# 동적 메서드명 (g_strdup_printf 등) 처리용 명시 annotation
# 형식: /* ADR-0018-audit: vm.stop, vm.pause, ... */
ANNOTATION_RE = re.compile(
    r'ADR-0018-audit:\s*([a-z0-9_.,\s]+)',
)


def extract_async_methods(dispatcher_text: str) -> list[str]:
    m = ASYNC_REG_BLOCK.search(dispatcher_text)
    if not m:
        print("ERROR: g_async_methods 등록 블록을 찾지 못함 (_async_method_names[])",
              file=sys.stderr)
        sys.exit(2)
    block = m.group(1)
    return METHOD_LITERAL.findall(block)


def collect_methods(pattern: re.Pattern[str]) -> set[str]:
    found: set[str] = set()
    for d in SEARCH_DIRS:
        if not d.exists():
            continue
        for c in d.glob("*.c"):
            txt = c.read_text(errors="replace")
            for m in pattern.finditer(txt):
                found.add(m.group(1))
    return found


def collect_audit_methods() -> set[str]:
    found = collect_methods(AUDIT_CALL_RE)
    for d in SEARCH_DIRS:
        if not d.exists():
            continue
        for c in d.glob("*.c"):
            txt = c.read_text(errors="replace")
            # 동적 메서드명을 명시 annotation으로 보완
            for ann in ANNOTATION_RE.finditer(txt):
                names = [s.strip() for s in ann.group(1).split(",") if s.strip()]
                found.update(names)
    return found


def main() -> int:
    if not DISPATCHER_C.exists():
        print(f"ERROR: {DISPATCHER_C} 미존재", file=sys.stderr)
        return 2
    dispatcher_text = DISPATCHER_C.read_text(errors="replace")
    async_methods = extract_async_methods(dispatcher_text)
    if not async_methods:
        print("WARN: g_async_methods 비어있음 — 검사 대상 없음")
        return 0

    audit_methods = collect_audit_methods()
    ws_methods = collect_methods(WS_COMPLETE_RE)
    missing = [m for m in async_methods if m not in audit_methods]
    missing_required_registry = [
        m for m in REQUIRED_FIRE_AND_FORGET_METHODS if m not in async_methods
    ]
    missing_required_audit = [
        m for m in REQUIRED_FIRE_AND_FORGET_METHODS if m not in audit_methods
    ]
    missing_required_ws = [
        m for m in REQUIRED_FIRE_AND_FORGET_METHODS if m not in ws_methods
    ]

    print(f"[ADR-0018] g_async_methods 등록: {len(async_methods)}건")
    print(f"[ADR-0018] audit 호출 발견 메서드: {len(audit_methods)}건")
    print(f"[ADR-0018] WS completion 발견 메서드: {len(ws_methods)}건")

    if missing:
        print()
        print("\033[31m[FAIL]\033[0m 다음 fire-and-forget 메서드는 g_async_methods에")
        print("       등록되었지만 워커 콜백에서 pcv_audit_log()를 호출하지 않습니다.")
        print("       이를 방치하면 audit DB가 거짓 결과를 기록합니다.")
        print("       → 해당 핸들러의 _callback() 함수에 pcv_audit_log() 추가:")
        print()
        for m in missing:
            print(f"  - {m}")
        print()
        print("       참고: docs/adr/0018-fire-and-forget-audit-policy.md")
        return 1

    if missing_required_registry or missing_required_audit or missing_required_ws:
        print()
        print("\033[31m[FAIL]\033[0m 추가 fire-and-forget 계약 검증 실패")
        if missing_required_registry:
            print("  async registry 누락:")
            for m in missing_required_registry:
                print(f"    - {m}")
        if missing_required_audit:
            print("  worker-result audit 누락:")
            for m in missing_required_audit:
                print(f"    - {m}")
        if missing_required_ws:
            print("  WS completion 누락:")
            for m in missing_required_ws:
                print(f"    - {m}")
        return 1

    print("\033[32m[PASS]\033[0m async registry/audit/WS completion 계약 통과 (ADR-0018 준수)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
