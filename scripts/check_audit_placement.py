#!/usr/bin/env python3

from __future__ import annotations
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DISPATCHER_C = REPO_ROOT / "src" / "api" / "dispatcher.c"
SEARCH_DIRS = [
    REPO_ROOT / "src" / "api",
    REPO_ROOT / "src" / "modules" / "dispatcher",
    REPO_ROOT / "src" / "modules" / "cloud",
    REPO_ROOT / "src" / "modules" / "virt",
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







ASYNC_REG_BLOCK = re.compile(
    r'_async_method_names\[\]\s*=\s*\{(.*?)\};',
    re.DOTALL,
)
METHOD_LITERAL = re.compile(r'"([a-z][a-z0-9_.]+)"')


AUDIT_CALL_RE = re.compile(
    r'pcv_audit_log(?:_rpc)?\s*\(\s*[^)]*?"([a-z][a-z0-9_.]+)"',
    re.DOTALL,
)
WS_COMPLETE_RE = re.compile(
    r'pcv_ws_broadcast_job_complete\s*\(\s*[^;]*?"([a-z][a-z0-9_.]+)"',
    re.DOTALL,
)



DYNAMIC_AUDIT_RULES = [
    (
        {"vm.stop", "vm.pause", "vm.resume", "vm.limit"},
        ('g_strdup_printf("vm.%s"', "ctx->action"),
    ),
    (
        {"cloud.import", "cloud.export"},
        ('g_strdup_printf("cloud.%s"', "_update_status("),
    ),
    (
        {"cloud.import.finalize"},
        ('g_str_has_prefix(job_id, "finalize-")', "import.finalize"),
    ),
]


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
            for names, tokens in DYNAMIC_AUDIT_RULES:
                if all(token in txt for token in tokens):
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
