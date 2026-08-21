#!/usr/bin/env python3
                          
                                                                        
                                                                                      
                                                                                                                            
 
                      
                                                                                                                                 
   
                                            

    
                                                                
                                                
                                
                                                      
                                               

       
                      
                                                   
                                 

    
                                         
                                      
                          
                                                  
                                             

       
                                                
                                                 
   
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
                                                            
                                                                            
                                                                                    
                                    
    ".add", ".remove", ".approve", ".reject", ".retry", ".cancel",
    ".resume", ".backup", ".enable", ".disable", ".bind", ".unbind",
                                                          
                                                   
                                                
    ".ack",
                                                           
                                                        
                                                                 
                                               
                                                                       
                                             
    ".subscribe", ".unsubscribe", ".rotate",
    "cloud.", "vm.export", "vm.import", "vm.start", "vm.stop",
    "vm.pause", "vm.resume", "vm.limit", "vm.clone", "vm.resize",
    "vm.snapshot", "vm.guest.exec", "vm.guest.shutdown",
)

                                                          
                                                                
 
                                        
                                                     
                                                       
                                                      
                                       
                                                 
DESTRUCTIVE_READ_EXCEPTIONS = frozenset({
             
    "daemon.update_check",                                   
    "vm.delete.status",                                  
    "vm.export.status",                                   
    "vm.import.status",                                   
    "vm.snapshot.list",                                    
    "vm.snapshot.schedule.list",                               
    "vpc.attachment.list",                                                     
                                                         
                                                        
                                                               
                                                                  
                                                               
                               
                                                       
                                                                
    "push.subscribe",                                                   
    "push.unsubscribe",                                                  
                                                            
                                                     
                                                
                                                                
                                                                
                                                                   
                                                        
                                                         
                                                                       
    "auth.totp.status",                                             
    "auth.totp.disable",                                              
    "auth.totp.recovery.regenerate",                                    
})


@dataclass(frozen=True)
class PolicyContract:
    method: str
    min_role: int
    owner_scope: bool
    reason: str


                                                 
                                        
                                                                                
                                                                              
                                                                             
POLICY_CONTRACTS = (
                                                                             
                                                                            
                                                                            
                                                                             
              
    PolicyContract(
        method="alert.history",
        min_role=0,
        owner_scope=False,
        reason="공용 알림 이력 조회 — REST/dispatcher VIEWER 정합",
    ),
    PolicyContract(
        method="healing.pending",
        min_role=0,
        owner_scope=False,
        reason="공용 자가치유 승인 대기 조회 — REST/dispatcher VIEWER 정합",
    ),
    PolicyContract(
        method="healing.history",
        min_role=0,
        owner_scope=False,
        reason="공용 자가치유 실행 이력 조회 — REST/dispatcher VIEWER 정합",
    ),
    PolicyContract(
        method="agent.history",
        min_role=0,
        owner_scope=False,
        reason="공용 AI 합의 이력 조회 — REST/dispatcher VIEWER 정합",
    ),
    PolicyContract(
        method="config.history",
        min_role=0,
        owner_scope=False,
        reason="공용 설정 변경 이력 조회 — REST/dispatcher VIEWER 정합",
    ),
    PolicyContract(
        method="storage.pool.forecast",
        min_role=0,
        owner_scope=False,
        reason="공용 저장소 용량 전망 조회 — REST/dispatcher VIEWER 정합",
    ),
    PolicyContract(
        method="backup.replicate",
        min_role=2,
        owner_scope=False,
        reason="크로스 노드 ZFS send/recv와 원격 SSH 실행은 admin-only",
    ),
                                                                 
                                                                 
                                                                    
                                                           
                                                         
                                              
                                                             
                                                  
                                                 
    PolicyContract(
        method="push.list",
        min_role=2,
        owner_scope=False,
        reason="전 사용자 구독 명단(username+endpoint) 노출이라 admin-only — "
               "정책행 소실 시 '.list' 규칙으로 VIEWER fail-open",
    ),
    PolicyContract(
        method="push.test",
        min_role=2,
        owner_scope=False,
        reason="전 구독자에게 실제 아웃바운드 푸시를 발생시키므로 admin-only — "
               "정책행 소실 시 조회자가 벤더 푸시 서비스로 발송을 유발",
    ),
                                                                             
                                                                
                                                
                                                           
                                                       
                                                                       
                      
    PolicyContract(
        method="suricata.ips.drop.list",
        min_role=2,
        owner_scope=False,
        reason="전 테넌트에 걸린 IPS 차단 SID 목록 노출이라 admin-only — "
               "정책행 소실 시 '.list' 규칙으로 VIEWER fail-open",
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

                                                                
                                                                    
                                                              
    viewer_destructive: list[str] = []
    for method, role in policies.items():
        if (role == 0 and method not in DESTRUCTIVE_READ_EXCEPTIONS
                and any(k in method for k in DESTRUCTIVE_KEYWORDS)):
            viewer_destructive.append(method)

    if viewer_destructive:
        print()
        print("\033[31m[FAIL]\033[0m 다음 destructive 메서드가 VIEWER(0) 로 매핑됐습니다")
        print("       (매핑은 있으나 조회자도 호출 가능 = fail-open 회귀). 최소 OPERATOR(1) 필요.")
        print("       실제 조회성이거나 caller-subject 로 고정된 자기 귀속 변이면")
        print("       DESTRUCTIVE_READ_EXCEPTIONS 에 근거 주석과 함께 추가.")
        print()
        for m in sorted(viewer_destructive):
            print(f"  - {m}  (현재 role=0)")
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
