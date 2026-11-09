#!/usr/bin/env python3






                          
                                                                        
                                                                                      
                                                                                                                            
 
                      
                                                                                                                                 
   
                                            

    
                                                                
                                                
                                
                                                      
                                               

       
                      
                                                   
                                 

    
                                         
                                      
                          
                                                  
                                             

       
                                                
                                                 
   

from __future__ import annotations
from dataclasses import dataclass
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rpc_extract

REPO_ROOT = Path(__file__).resolve().parent.parent

REST_ROOT = REPO_ROOT

                                                          
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
        method="healing.status",
        min_role=0,
        owner_scope=False,
        reason="공용 자가치유 모드·대기 건수 조회 — 화면이 실제 모드를 그린다",
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
        method="backup.replication.hostkey.scan",
        min_role=2,
        owner_scope=False,
        reason="원격 ssh-keyscan 실행·대상 키 노출은 admin-only",
    ),
    PolicyContract(
        method="backup.replication.hostkey.trust",
        min_role=2,
        owner_scope=False,
        reason="복제 대상 신뢰 등록은 admin-only",
    ),
    PolicyContract(
        method="backup.replication.hostkey.list",
        min_role=2,
        owner_scope=False,
        reason="등록 호스트 목록 — 정책행 소실 시 '.list' 규칙으로 VIEWER fail-open",
    ),
    PolicyContract(
        method="backup.replication.hostkey.remove",
        min_role=2,
        owner_scope=False,
        reason="복제 대상 신뢰 해제는 admin-only",
    ),

    PolicyContract(
        method="vpc.subnet.confirm",
        min_role=2,
        owner_scope=False,
        reason="확인은 자동 원복을 취소해 관리 대역을 가리는 subnet을 남기므로 admin-only",
    ),

    PolicyContract(
        method="vpc.pool.set",
        min_role=2,
        owner_scope=False,
        reason="풀 배정은 OPERATOR가 쓸 수 있는 VPC 대역을 정하므로 admin-only (계획 B-2 RB14)",
    ),
    PolicyContract(
        method="vpc.pool.list",
        min_role=2,
        owner_scope=False,
        reason="전 테넌트의 주소 풀 노출 — '.list' 접미라 REST 앞단이 VIEWER로 통과시킨다, 하향 금지 (계획 B-2 RB14)",
    ),

    PolicyContract(
        method="security.event.close",
        min_role=2,
        owner_scope=False,
        reason="열린 보안 사건을 닫아 위험 점수·열린 목록에서 빼므로 admin-only (SECURITY-EVENT-OPERATOR-CLOSE)",
    ),

    PolicyContract(
        method="auth.user.tenant.set",
        min_role=2,
        owner_scope=False,
        reason="테넌트 지정은 호스트 자원 허용 범위를 정하므로 admin-only (계획 A RA9)",
    ),

    PolicyContract(
        method="resource.register",
        min_role=2,
        owner_scope=False,
        reason="등록은 비ADMIN이 참조할 수 있는 호스트 파일·장치·브리지를 정하므로 admin-only (계획 A RA9)",
    ),
    PolicyContract(
        method="resource.unregister",
        min_role=2,
        owner_scope=False,
        reason="등록 해제는 테넌트의 허용을 함께 지우므로 admin-only (계획 A RA9)",
    ),
    PolicyContract(
        method="resource.grant",
        min_role=2,
        owner_scope=False,
        reason="허용이 내려가면 OPERATOR가 자기 테넌트에 호스트 자원을 열어 준다 — admin-only (계획 A RA9)",
    ),
    PolicyContract(
        method="resource.revoke",
        min_role=2,
        owner_scope=False,
        reason="허용 해제는 다른 테넌트의 자원 사용을 끊으므로 admin-only (계획 A RA9)",
    ),
    PolicyContract(
        method="resource.list",
        min_role=2,
        owner_scope=False,
        reason="전 테넌트의 등록·허용 목록 — '.list' 접미라 REST 앞단이 VIEWER로 통과시킨다, 하향 금지 (계획 A RA9)",
    ),
    PolicyContract(
        method="resource.unregistered.list",
        min_role=2,
        owner_scope=False,
        reason="모든 VM의 호스트 경로·장치·소유자 노출 — '.list' 접미라 REST 앞단이 VIEWER로 통과시킨다, 하향 금지 (계획 A RA9)",
    ),
    PolicyContract(
        method="resource.import",
        min_role=2,
        owner_scope=False,
        reason="일괄 등록·허용은 register·grant와 같은 권한 — admin-only (계획 A RA9)",
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

    m = re.search(r'\b' + re.escape(name) + r'\s*\([^;{]*\)\s*\{', text)
    if not m:
        return None
    brace_pos = m.end() - 1

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



ALIAS_CALL_RE = re.compile(
    r'!\s*pcv_dispatcher_vm_target_aliases_valid\s*\(\s*method\s*,\s*params\s*,')

_OWNER_FN = rpc_extract.current_fn_name(REPO_ROOT, "_vm_owner_scoped_method_allowed")
OWNER_CALL_RE = re.compile(r'allowed\s*=\s*' + re.escape(_OWNER_FN) + r'\s*\(')
ROLE_CHECK_FN = "_dispatch_role_check"
OWNER_CHECK_FN = "_dispatch_owner_check"
DISPATCH_FN = "purecvisor_dispatcher_dispatch"

def alias_wiring_failure(text: str) -> str | None:

    role = extract_c_function_body(text, ROLE_CHECK_FN)
    owner = extract_c_function_body(text, OWNER_CHECK_FN)
    dispatch = extract_c_function_body(text, DISPATCH_FN)
    if role is None or owner is None or dispatch is None:
        return f"{ROLE_CHECK_FN}·{OWNER_CHECK_FN}·{DISPATCH_FN} 정의를 찾지 못함"
    if not OWNER_CALL_RE.search(owner):
        return f"{OWNER_CHECK_FN}에서 {_OWNER_FN} 호출을 찾지 못함"
    if len(OWNER_CALL_RE.findall(text)) != 1:
        return f"{_OWNER_FN} 호출은 {OWNER_CHECK_FN} 한 곳이어야 함(별칭 검사를 건너뛰는 경로)"
    call = ALIAS_CALL_RE.search(role)
    if not call:
        return ("owner-scope 검사 앞에 pcv_dispatcher_vm_target_aliases_valid "
                "호출이 없음 (상충 별칭으로 소유자 확인 대상과 실행 대상이 갈림)")
    if "PURE_RPC_ERR_INVALID_PARAMS" not in role[call.start():]:
        return "별칭 상충 거절이 PURE_RPC_ERR_INVALID_PARAMS로 응답하지 않음"
    role_at = dispatch.find(ROLE_CHECK_FN + "(")
    submit_at = dispatch.find("pcv_rpc_offload_submit(")
    owner_at = dispatch.find(OWNER_CHECK_FN + "(")
    if role_at < 0 or submit_at < 0 or owner_at < 0:
        return f"dispatch 본문에서 {ROLE_CHECK_FN}·레인 제출·{OWNER_CHECK_FN} 호출을 찾지 못함"
    if not role_at < submit_at < owner_at:
        return f"dispatch 순서는 {ROLE_CHECK_FN} → 레인 제출 → {OWNER_CHECK_FN}이어야 함"
    return None

REST_METHOD_CALL_RE = re.compile(
    r'pcv_rest_rpc_envelope_method\s*\(\s*(?:rq\s*->\s*)?rpc\s*,\s*&\s*rpc_method\s*\)')
REST_PERMISSION_CALL_RE = re.compile(
    r'pcv_rbac_check_permission\s*\(\s*(?:rq\s*->\s*)?subject\s*,\s*rpc_method\s*\)')
REST_RAW_METHOD_RE = re.compile(r'strstr\s*\([^;]*\\"method\\"')

REST_FILE_MARK_RE = re.compile(r'^/\* ==== (.+?) ==== \*/\n', re.M)

def _raw_location(text: str, pos: int) -> str:

    mark = None
    for mark in REST_FILE_MARK_RE.finditer(text, 0, pos):
        pass
    if mark is None:
        return f"line {text.count(chr(10), 0, pos) + 1}"
    return f"{mark.group(1)}:{text.count(chr(10), mark.end(), pos) + 1}"

def rest_gate_failure(text: str) -> str | None:

    permission = REST_PERMISSION_CALL_RE.search(text)
    if not permission:
        return "pcv_rbac_check_permission(subject 또는 rq->subject, rpc_method) 호출을 찾지 못함"
    if not REST_METHOD_CALL_RE.search(text, 0, permission.start()):
        return ("권한 판정 앞에 pcv_rest_rpc_envelope_method(rpc 또는 rq->rpc, &rpc_method) 호출이 없음 "
                "(params 안 decoy method로 판정이 갈릴 수 있음)")
    raw = REST_RAW_METHOD_RE.search(text)
    if raw:
        return f"{_raw_location(text, raw.start())}: 원문 strstr로 \"method\"를 추출함 — 최상위 method 파싱만 허용"
    return None

def rest_wiring_text(args: list[str]) -> str:

    if len(args) > 1:
        return Path(args[1]).read_text(errors="replace")
    return rpc_extract.rest_family_text(REST_ROOT)

def main(argv: list[str] | None = None) -> int:
    args = sys.argv[1:] if argv is None else argv
    if args:

        dispatcher_c = Path(args[0])
        if not dispatcher_c.exists():
            print(f"ERROR: {dispatcher_c} 미존재", file=sys.stderr)
            return 2
        text = route_text = policy_text = owner_text = dispatcher_c.read_text(errors="replace")
    else:

        route_text = rpc_extract.dispatcher_route_file(REPO_ROOT).read_text(errors="replace")
        policy_text = rpc_extract.dispatcher_policy_file(REPO_ROOT).read_text(errors="replace")
        text = rpc_extract.dispatcher_family_text(REPO_ROOT)

        owner_text = policy_text

    routes = sorted(set(ROUTE_RE.findall(route_text)))
    if not routes:
        print("WARN: g_rpc_routes 등록 메서드를 찾지 못함")
        return 0

    m = POLICY_BLOCK.search(policy_text)
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

    owner_scope_body = extract_c_function_body(
        owner_text, rpc_extract.current_fn_name(REPO_ROOT, "_vm_method_requires_owner_scope"))
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

    wiring = alias_wiring_failure(text)
    if wiring:
        contract_failures.append(f"VM 대상 별칭 단일 대상 검사(ADR-0079): {wiring}")
    try:
        rest_text = rest_wiring_text(args)
    except FileNotFoundError as error:
        print(f"ERROR: REST 소스 미존재: {error}", file=sys.stderr)
        return 2
    rest_wiring = rest_gate_failure(rest_text)
    if rest_wiring:
        contract_failures.append(f"REST RBAC method 판정(ADR-0079): {rest_wiring}")

    if contract_failures:
        print()
        print("\033[31m[FAIL]\033[0m ADR-0019/0078 RBAC 정책 계약·배선 회귀 감지")
        print("       operator VM 단일 대상 action은 role=OPERATOR + owner-scope를 유지하고,")
        print("       REST/dispatcher 게이트는 실제 실행 대상(method·VM)과 같은 값을 판정해야 합니다.")
        print()
        for failure in contract_failures:
            print(f"  - {failure}")
        print()
        print("       참고: docs/GUIDE.md#vm-owner-scope, docs/adr/0019-rbac-uds-bypass-policy.md")
        return 1



    if missing_other:
        print()
        print("\033[31m[FAIL]\033[0m 다음 라우트는 g_method_policies에 매핑이 없습니다.")
        print("       dispatcher 기본 거부(ADR-0079)로 ADMIN 전용이 됩니다. REST 앞단")
        print("       (src/modules/auth/pcv_rbac.c _method_min_role)과 같은 등급의 행을 추가하세요.")
        print()
        for m in missing_other:
            print(f"  - {m}")
        print()
        print("       참고: docs/adr/0079-dispatcher-default-deny-and-single-target-alias.md")
        return 1

    print("\033[32m[PASS]\033[0m 모든 라우트가 정책 매핑됨 — 라우트 ⊆ 정책표 (ADR-0019/0078 준수)")
    print("\033[32m[PASS]\033[0m operator VM owner-scope 정책 계약 + 별칭 단일 대상 배선 준수")
    return 0


if __name__ == "__main__":
    sys.exit(main())
