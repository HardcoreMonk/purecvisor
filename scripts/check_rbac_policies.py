
"""
ADR-0019 정적 분석 — RBAC 정책 매핑 누락 및 정책 계약 회귀 검출

[목적]
  dispatcher.c::g_rpc_routes에 등록된 모든 RPC 메서드가 g_method_policies에
  최소 role 매핑을 가지는지 검증. 매핑되지 않은 메서드는 VIEWER로 처리되어
  destructive RPC가 권한 없이 실행될 위험.
  또한 제품 정책상 operator에게 열리되 VM owner-scope를 반드시 통과해야 하는
  메서드가 admin-only로 회귀하거나 owner-scope에서 빠지는지 검증.

[검사 대상]
  src/api/dispatcher.c
    - g_rpc_routes에 g_hash_table_insert로 등록된 메서드 이름
    - g_method_policies 배열의 매핑 항목

[방식]
  1. dispatcher.c에서 g_rpc_routes 등록 패턴 추출
  2. g_method_policies 배열에서 매핑된 메서드 추출
  3. 등록되었지만 정책 없는 메서드 → 보고
     (단, vm.list/metrics 등 조회성은 매핑 없어도 VIEWER로 안전)
  4. 정책 계약 메서드의 최소 role과 owner-scope 포함 여부 검증

[종료 코드]
  0: 모든 등록 메서드가 정책 매핑 있음 (또는 의도적 default), 계약 준수
  1: destructive 메서드 매핑 누락 또는 정책 계약 회귀 (CRITICAL)
"""
from __future__ import annotations
from dataclasses import dataclass
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DISPATCHER_C = REPO_ROOT / "src" / "api" / "dispatcher.c"

ROUTE_RE = re.compile(
    r'g_hash_table_insert\s*\(\s*g_rpc_routes\s*,\s*"([a-z][a-z0-9_.]+)"',
)

POLICY_BLOCK = re.compile(
    r'g_method_policies\[\]\s*=\s*\{(.*?)\n\};',
    re.DOTALL,
)
POLICY_ENTRY = re.compile(
    r'\{\s*"([a-z][a-z0-9_.]+)"\s*,\s*([A-Z0-9_]+|\d+)\s*\}',
)

ROLE_VALUES = {
    "PCV_ROLE_VIEWER": 0,
    "PCV_ROLE_OPERATOR": 1,
    "PCV_ROLE_ADMIN": 2,
    "VIEWER": 0,
    "OPERATOR": 1,
    "ADMIN": 2,
}

DESTRUCTIVE_KEYWORDS = (
    ".delete", ".destroy", ".create", ".update", ".set", ".push",
    ".trigger", ".reset", ".rollback", ".restore", ".replicate", ".enter", ".exit",
    ".attach", ".detach", "auth.",
    "cloud.", "vm.export", "vm.import", "vm.start", "vm.stop",
    "vm.pause", "vm.resume", "vm.limit", "vm.clone", "vm.resize",
    "vm.snapshot", "vm.guest.exec", "vm.guest.shutdown",
)

READONLY_EXEMPT = frozenset({
    "daemon.update_check",
})

@dataclass(frozen=True)
class PolicyContract:
    method: str
    min_role: int
    owner_scope: bool
    reason: str

POLICY_CONTRACTS = (
    PolicyContract(
        method="backup.replicate",
        min_role=2,
        owner_scope=False,
        reason="크로스 노드 ZFS send/recv와 원격 SSH 실행은 admin-only",
    ),
    PolicyContract(
        method="device.nic.attach",
        min_role=1,
        owner_scope=True,
        reason="operator는 자기 VM에 한해 NIC 추가 가능",
    ),
    PolicyContract(
        method="device.nic.detach",
        min_role=1,
        owner_scope=True,
        reason="operator는 자기 VM에 한해 NIC 제거 가능",
    ),
    PolicyContract(
        method="vm.delete",
        min_role=1,
        owner_scope=True,
        reason="operator는 자기 VM 삭제만 가능",
    ),
    PolicyContract(
        method="vm.vnc",
        min_role=1,
        owner_scope=True,
        reason="operator는 자기 VM 콘솔만 가능",
    ),
    PolicyContract(
        method="get_vnc_info",
        min_role=1,
        owner_scope=True,
        reason="noVNC 접속 정보는 자기 VM에 한해 제공",
    ),
)

def parse_role(token: str) -> int | None:
    token = token.strip()
    if token.isdigit():
        return int(token)
    return ROLE_VALUES.get(token)

def extract_c_function_body(text: str, name: str) -> str | None:
    name_pos = text.find(name)
    if name_pos < 0:
        return None

    brace_pos = text.find("{", name_pos)
    if brace_pos < 0:
        return None

    depth = 0
    for pos in range(brace_pos, len(text)):
        ch = text[pos]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return text[brace_pos + 1:pos]
    return None

def explicit_owner_scope_state(body: str, method: str) -> bool | None:
    needle = f'"{method}"'
    pos = body.find(needle)
    if pos < 0:
        return None

    next_true = body.find("return TRUE", pos)
    next_false = body.find("return FALSE", pos)
    if next_true >= 0 and (next_false < 0 or next_true < next_false):
        return True
    if next_false >= 0 and (next_true < 0 or next_false < next_true):
        return False
    return None

def owner_scope_includes_method(body: str | None, method: str) -> bool:
    if not body:
        return False

    explicit = explicit_owner_scope_state(body, method)
    if explicit is not None:
        return explicit

    if not method.startswith("vm."):
        return False

    has_vm_prefix_gate = 'g_str_has_prefix(method, "vm.")' in body
    return has_vm_prefix_gate

def main() -> int:
    if not DISPATCHER_C.exists():
        print(f"ERROR: {DISPATCHER_C} 미존재", file=sys.stderr)
        return 2
    text = DISPATCHER_C.read_text(errors="replace")

    routes = sorted(set(ROUTE_RE.findall(text)))
    if not routes:
        print("WARN: g_rpc_routes 등록 메서드를 찾지 못함")
        return 0

    m = POLICY_BLOCK.search(text)
    policies: dict[str, int] = {}
    if m:
        for em in POLICY_ENTRY.finditer(m.group(1)):
            role = parse_role(em.group(2))
            if role is None:
                print(f"ERROR: 알 수 없는 role token: {em.group(2)}", file=sys.stderr)
                return 2
            policies[em.group(1)] = role

    print(f"[ADR-0019] g_rpc_routes 등록: {len(routes)}건")
    print(f"[ADR-0019] g_method_policies 매핑: {len(policies)}건")

    missing_destructive: list[str] = []
    missing_other: list[str] = []
    for method in routes:
        if method in policies:
            continue
        if method in READONLY_EXEMPT:
            continue
        if any(k in method for k in DESTRUCTIVE_KEYWORDS):
            missing_destructive.append(method)
        else:
            missing_other.append(method)

    if missing_destructive:
        print()
        print("\033[31m[FAIL]\033[0m 다음 destructive 메서드는 g_method_policies에")
        print("       매핑이 없습니다. RBAC 우회 위험 — 정책 추가 필수.")
        print()
        for m in missing_destructive:
            print(f"  - {m}")
        print()
        print("       참고: docs/adr/0019-rbac-uds-bypass-policy.md")
        return 1

    owner_scope_body = extract_c_function_body(text, "_vm_method_requires_owner_scope")
    contract_failures: list[str] = []
    for contract in POLICY_CONTRACTS:
        actual_role = policies.get(contract.method)
        if actual_role is None:
            contract_failures.append(
                f"{contract.method}: 정책 매핑 없음 "
                f"(기대 role={contract.min_role}, {contract.reason})"
            )
            continue
        if actual_role != contract.min_role:
            contract_failures.append(
                f"{contract.method}: role={actual_role}, 기대 role={contract.min_role} "
                f"({contract.reason})"
            )
        if contract.owner_scope and not owner_scope_includes_method(owner_scope_body, contract.method):
            contract_failures.append(
                f"{contract.method}: owner-scope 대상 아님 ({contract.reason})"
            )

    if contract_failures:
        print()
        print("\033[31m[FAIL]\033[0m ADR-0019 RBAC 정책 계약 회귀 감지")
        print("       operator VM 단일 대상 action은 role=OPERATOR + owner-scope를 유지해야 합니다.")
        print()
        for failure in contract_failures:
            print(f"  - {failure}")
        print()
        print("       참고: docs/GUIDE.md#vm-owner-scope, docs/adr/0019-rbac-uds-bypass-policy.md")
        return 1

    if missing_other:
        print()
        print(f"\033[33m[INFO]\033[0m {len(missing_other)}개 조회성 메서드가 매핑 없음 → VIEWER 기본 적용")
        for m in missing_other[:5]:
            print(f"  - {m}")
        if len(missing_other) > 5:
            print(f"  ... and {len(missing_other) - 5} more")

    print("\033[32m[PASS]\033[0m destructive 메서드 모두 정책 매핑됨 (ADR-0019 준수)")
    print("\033[32m[PASS]\033[0m operator VM owner-scope 정책 계약 준수")
    return 0

if __name__ == "__main__":
    sys.exit(main())
