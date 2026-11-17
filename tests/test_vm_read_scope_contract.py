#!/usr/bin/env python3
                          
                                                                                           
                                                             
                      
                                                                            
                                                                          
                     
                                                                                                   
                                                                                                         
                                                                                         
                                                                                                                      
                                                                                                       
                                                                                                          
                                                                                     
                                                                 
                                                                                                                
                                                                                               
                                                                                                 
                                                                                 
                                                                                                                    
                                                                                                        
                                                                                                        
                                                                                             
                       
                                                                                                      
                                                                                             
                                                                              
                                                                                                                 
                                                                                                         
                                                              
                                                                                                                           
                                                                                                 
                                                                                                      
                                                                                     
                                                                                                            
                                                                                                
                                                                                                             
                                                                                                 
                                                                                           
                                                                                                                      
                                                                                                               
                                                                                                 
                                                                                                 
                                                                                                                 
                                                                                                 
                                                                                                             
                                                                       
                                                                                                                           
                                                                                                                        
                                                                                                  
                                                                                                
                                                                                                                    
                                                                                               
                                                                                
                                                                                                                          
                                                                                                   
                                                                                                                            
                                                                                                   
                                                                                                                
                                                                                                                 
                                                                                                             
                                                                                                  
                        
                                                                                             
                                                                          
                                   
from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts" / "split"))
import c_blocks              

SCOPE_C = ROOT / "src/modules/resource/vm_read_scope.c"
GATE_C = ROOT / "src/modules/resource/resource_gate.c"
REST_SERVER = ROOT / "src/api/rest_server.c"
REST_REQUEST = ROOT / "src/api/rest_request.c"
GRPC_SERVER = ROOT / "src/api/grpc_server.c"
DISPATCHER = ROOT / "src/api/dispatcher.c"
DIAG_HANDLER = ROOT / "src/modules/dispatcher/handler_monitor_diag.c"
POLICY = ROOT / "src/api/dispatcher_policy.c"
PRINCIPAL_NAMES_C = ROOT / "src/modules/auth/pcv_principal_names.c"
OWNER_SCOPE = ROOT / "src/api/dispatcher_owner_scope.c"
OFFLOAD = ROOT / "src/modules/dispatcher/rpc_offload.c"
DOMAIN_REFS = ROOT / "src/modules/resource/resource_domain_refs.c"
VPC_MANAGER = ROOT / "src/modules/network/vpc/vpc_manager.c"
VNC_PROXY = ROOT / "src/api/vnc_proxy.h"
VM_LIFECYCLE = ROOT / "src/modules/dispatcher/handler_vm_lifecycle.c"
MONITOR = ROOT / "src/modules/dispatcher/handler_monitor.c"
READ_FILTER_C = ROOT / "src/modules/resource/vm_read_filter.c"
HOST_DEVICE_VIEW = ROOT / "src/modules/dispatcher/host_device_view.c"
BACKUP = ROOT / "src/modules/dispatcher/handler_backup.c"
                                                                                                
SCOPE_READ_ROUTES = ("monitor.vm.diag", "monitor.processes", "vm.numa.info", "backup.policy.list",
                     "vm.snapshot.schedule.list", "security_group.list", "healing.history", "security.event.list",
                     "security.event.get", "security.action.pending", "alert.history", "agent.history", "jobs.list",
                     "jobs.get", "vm.delete.status")
ALERT = ROOT / "src/modules/dispatcher/handler_alert.c"
JOB_QUEUE = ROOT / "src/utils/pcv_job_queue.c"
VM_OWNER_HANDLER = ROOT / "src/modules/dispatcher/handler_vm_owner.c"
CLI_MAIN = ROOT / "src/cli/purecvisorctl.c"
REST_PUBLIC = ROOT / "src/api/rest_public.c"
WS_SERVER = ROOT / "src/api/ws_server.c"
WS_LEGACY_VM = ("src/modules/dispatcher/handler_vm_start.c", "src/modules/dispatcher/handler_vm_diskio.c", "src/modules/virt/vm_ops.c",
                "src/modules/dispatcher/handler_vm_clone.c", "src/modules/dispatcher/handler_security.c")
WS_LEGACY_CONTAINER = ("src/modules/dispatcher/handler_container.c", "src/modules/dispatcher/handler_container_net.c")
LEGACY_JOB_CALL = re.compile(r"\bpcv_ws_broadcast_job_complete(?:_mt)?\s*\(")
CLI_OWNER = ROOT / "src/cli/cli_vm_owner.c"
REST_COMPUTE = ROOT / "src/api/rest_map_compute.c"
RPC_PARAMS = ROOT / "contracts/rpc_params.json"
JOBS = ROOT / "src/modules/dispatcher/handler_jobs.c"
VM_ADMIN = ROOT / "src/modules/dispatcher/handler_vm_admin.c"
SECURITY = ROOT / "src/modules/dispatcher/handler_security.c"
ALERT_ENGINE = ROOT / "src/modules/daemons/alert_engine.c"
TRACK_LANE_ROUTES = SCOPE_READ_ROUTES + ("healing.history", "security.event.list", "security.event.get", "security.action.pending",
                                         "alert.history", "agent.history", "jobs.list",
                                         "jobs.get", "vm.delete.status")
                                                                                   
                                                                              
MAIN_LOOP_SENSITIVE = ("pcv_vm_read_visible(", "pcv_vm_read_scope_new(", "pcv_vm_read_scope_new_parts(", "pcv_res_owner_tenant(",
                       "pcv_vm_read_xml_visible(", "pcv_vm_owner_map_current(", "pcv_vm_read_view_init(",
                       "pcv_vm_read_visible_name(", "pcv_vm_read_visible_uuid(", "pcv_vm_read_visible_domain_id(",
                       "pcv_vm_read_filter_processes(", "pcv_vm_read_mask_numa(", "pcv_vm_read_filter_by_member(",
                       "pcv_vm_read_filter_sg_vms(", "pcv_vm_read_filter_healing_history(",
                       "pcv_vm_read_filter_security_rows(", "pcv_vm_read_collect_security_rows(",
                       "pcv_vm_read_security_row_visible(", "pcv_vm_read_mask_security_alerts(",
                       "pcv_vm_read_filter_agent_comparison(", "pcv_vm_read_job_scope(")
ALLOWED_CALLERS = {
    "src/modules/resource/vm_read_scope.c": None,                      
    "src/modules/resource/vm_read_filter.c": None,                                    
    "src/modules/resource/resource_gate.c": None,                                           
    "src/modules/dispatcher/handler_monitor_diag.c": {"_vm_diag_read_visible", "handle_monitor_vm_diag"},               
    "src/api/dispatcher_owner_scope.c": {"pcv_disp_vm_read_allowed"},                
    "src/modules/dispatcher/handler_vm_lifecycle.c": {"vm_list_worker"},            
    "src/modules/dispatcher/handler_monitor.c": {"pcv_monitor_fleet_collect", "handle_monitor_processes"},                            
    "src/modules/dispatcher/handler_vm_admin.c": {"handle_vm_numa_info", "handle_vm_delete_status"},               
                                                                                       
    "src/modules/dispatcher/handler_jobs.c": {"handle_jobs_list", "handle_jobs_get"},                
    "src/api/rest_public.c": {"_metrics_render"},                                                      
    "src/api/ws_server.c": {"_ws_broadcast_scoped_fanout", "_ws_filtered_comparison"},                         
    "src/api/rest_public_render.c": {"pcv_rest_render_metrics", "_append_registry_scoped"},              
    "src/modules/dispatcher/handler_backup.c": {"handle_backup_policy_list"},                                        
    "src/modules/dispatcher/handler_security_group.c": {"handle_security_group_list"},               
    "src/modules/dispatcher/handler_host_device.c": {"handle_host_device_list"},                  
    "src/modules/dispatcher/host_device_view.c": {"view_name_visible"},                                   
    "src/modules/dispatcher/handler_alert.c": {"handle_alert_history", "handle_agent_history", "handle_healing_history"},              
    "src/modules/dispatcher/handler_security.c": {"handle_security_event_list", "handle_security_event_get",
                                                  "handle_security_action_pending"},               
    "src/modules/dispatcher/handler_resource.c": None,                                            
}
                                      
OWNER_READERS = (
    ("owner_scope", "_lookup_vm_owner", ("pcv_vm_owner_from_domain_xml(",)),
    ("domain_refs", "pcv_res_domain_refs_parse", ("pcv_vm_owner_from_domain_node(",)),
    ("vpc", "_xml_owner", ("pcv_vm_owner_from_domain_xml(",)),
    ("vnc", "_vnc_xml_port", ("pcv_vm_owner_from_domain_node(",)),
)
DECIDE = "pcv_vm_read_decide"

                                                                 
TARGETS: dict[Path, tuple[str, ...]] = {
    SCOPE_C: ("*",),
    READ_FILTER_C: ("*",),
    JOB_QUEUE: ("_job_visible_fn",),
    REST_PUBLIC: ("_own_failure_row", "_collector_scoped"),
    HOST_DEVICE_VIEW: ("view_name_visible",),
    DIAG_HANDLER: ("_vm_diag_read_visible", "handle_monitor_vm_diag"),
    OWNER_SCOPE: ("pcv_disp_vm_read_allowed", "_vm_read_target", "_vm_owner_lookup"),
}

                                                                                                
                                                               
PRECHECK_EXPECT = {"vm.metrics", "vm.guest.agent.status", "vm.guest.fsinfo", "vm.guest.ping", "monitor.metrics"}
CLASS_EXPECT = (10, 9, 29)
                                                                           
OUT_OF_SCOPE = ("network.qos.get", "quota.get", "ovn.switch.detail", "healing.status", "storage.zvol.list", "network.info")
READ_DENY = "Permission denied: VM not found or not visible"
OFFLOAD_ROW = re.compile(r'\{\s*"([a-z0-9_.]+)"\s*,\s*PCV_OFFLOAD_LANE_(\w+)\s*\}')

                                                                                   
CMP_LITERAL_OK: dict[tuple[str, str], tuple[str, ...]] = {
    ("vm_read_filter.c", "_proc_row_is_vm"): ('"vm"',),
    ("vm_read_filter.c", "_is_star"): ('"*"',),
    ("vm_read_filter.c", "_kind_is_vm"): ('"vm"',),
    ("vm_read_filter.c", "_alert_is_security"): ('"Security"',),
    ("vm_read_filter.c", "pcv_ws_scope_kind"): ('"metric"', '"metrics-update"', '"forecast"', '"healing"', '"healing-request"',
                                                 '"anomaly"', '"agent-comparison"', '"job.complete"'),
}
PRINCIPAL_WORD = re.compile(r"(owner|subject|\bsub\b|actor|tenant|user|principal|caller)", re.I)
CMP_CALL = re.compile(r"\b(?:g_strcmp0|strcmp|strncmp|strcasecmp|g_ascii_strcasecmp|g_str_equal)\s*\(")
SQL_EQ = re.compile(r'"[^"\n]*=\s*\?[^"\n]*"')


def _read(p: Path) -> str:
    return p.read_text(encoding="utf-8")


def _no_comments(src: str) -> str:
                                                  
    kinds = c_blocks.classify(src)
    return "".join(ch if k != "m" or ch == "\n" else " " for ch, k in zip(src, kinds))


def _bodies(src: str) -> dict[str, tuple[str, str]]:
                                            
    code, nc = c_blocks.masked(src), _no_comments(src)
    return {b.name: (code[b.body_open:b.body_close + 1], nc[b.body_open:b.body_close + 1])
            for b in c_blocks.scan(src)[0]}


def principal_compare_violations(srcs: dict[Path, str]) -> list[str]:
                                                                 
    out = []
    decide_seen = False
    for path, names in TARGETS.items():
        bodies = _bodies(srcs[path])
        wanted = list(bodies) if "*" in names else list(names)
        for name in wanted:
            if name not in bodies:
                out.append(f"{path.name}: 함수 없음 {name}")
                continue
            code, nc = bodies[name]
            hits = CMP_CALL.findall(code) + SQL_EQ.findall(nc)
            allowed = CMP_LITERAL_OK.get((path.name, name))
            if allowed and not SQL_EQ.findall(nc):
                bad = [a for a in _cmp_args(nc)
                       if not any(lit in a for lit in allowed) or PRINCIPAL_WORD.search(a)]
                hits = [f"literal-only 위반: {a}" for a in bad]
            if name == DECIDE:
                decide_seen = True
                if not CMP_CALL.search(code):
                    out.append(f"{path.name}: {DECIDE} 본문에 비교가 없다(비교 자리가 이 함수여야 한다)")
                continue
            if hits:
                out.append(f"{path.name}:{name}: {DECIDE} 밖 문자열 비교 {hits}")
    if not decide_seen:
        out.append(f"{DECIDE} 정의가 TARGETS 안에 없다")
    return out


def _cmp_args(nc_body: str) -> list[str]:
                                                           
    out = []
    for m in CMP_CALL.finditer(nc_body):
        i, depth = m.end() - 1, 0
        for k in range(i, len(nc_body)):
            if nc_body[k] == "(":
                depth += 1
            elif nc_body[k] == ")":
                depth -= 1
                if depth == 0:
                    out.append(nc_body[i + 1:k])
                    break
    return out


def _fn(src: str, name: str) -> str:
                                          
    nc = _no_comments(src)
    for b in c_blocks.scan(src)[0]:
        if b.name == name:
            return nc[b.body_open:b.body_close + 1]
    raise AssertionError(f"함수 없음: {name}")


def caller_tenant_injection_violations(srcs: dict[str, str]) -> list[str]:
                                                                 
    out = []
                                                                                               
    rs_raw = srcs["rest_server"]
    rs = c_blocks.masked(rs_raw)
    k = rs.find("pcv_rest_rpc_attach_auth_context(")
    if k < 0:
        out.append("rest_server.c: attach 호출이 없다")
    else:
        m = re.search(r"caller_tenant\s*=\s*_rest_caller_tenant\(\s*rq\s*,\s*caller_role\s*\)", rs)
        if not m or m.start() > k:
            out.append("rest_server.c: attach 앞에서 _rest_caller_tenant(rq, caller_role)로 테넌트를 풀지 않는다")
        if not re.search(r"pcv_rest_rpc_attach_auth_context\(\s*rq->rpc\s*,\s*rq->subject\s*,\s*caller_role\s*,\s*caller_tenant\s*\)", rs):
            out.append("rest_server.c: attach 에 caller_tenant 를 넘기지 않는다")
                              
    try:
        a = _fn(srcs["rest_request"], "pcv_rest_rpc_attach_auth_context")
        rm = a.find('json_object_remove_member(params, "_pcv_caller_tenant")')
        inj = a.find("pcv_res_inject_caller_tenant(params")
        if rm < 0 or inj < 0 or rm > inj:
            out.append("rest_request.c: 손님 _pcv_caller_tenant 삭제 → pcv_res_inject_caller_tenant 순서가 아니다")
    except AssertionError as e:
        out.append(str(e))
                                                    
    try:
        g = _fn(srcs["grpc"], "_grpc_inject_caller_identity")
        rm = g.find('json_object_remove_member(params, "_pcv_caller_tenant")')
        ss = g.find('json_object_set_string_member(params, "_pcv_caller_tenant"')
        sn = g.find('json_object_set_null_member(params, "_pcv_caller_tenant")')
        if min(rm, ss, sn) < 0 or not (rm < ss and rm < sn):
            out.append("grpc_server.c: 손님 키 삭제 뒤 _pcv_caller_tenant(문자열 또는 null)를 늘 넣지 않는다")
        if re.search(r"if\s*\(\s*role\s*[<>]=?\s*PCV_ROLE_ADMIN", g):
            out.append("grpc_server.c: 주입이 역할에 따라 갈린다(ADMIN 값은 pcv_res_actor_tenant 가 버린다)")
    except AssertionError as e:
        out.append(str(e))
                                             
    d = c_blocks.masked(srcs["dispatcher"])
    if "pcv_rbac_get_tenant(" in d:
        out.append("dispatcher.c: 메인 루프에서 pcv_rbac_get_tenant 를 부른다(R11·PD-3)")
    sa = d.find("pcv_rpc_completion_set_actor(audit_username)")
                                                                                              
    src_at = re.search(r"\bactor_tenant\s*=\s*pcv_res_actor_tenant\(\s*params\s*,\s*caller_role\s*\)", d)
    st = re.search(r"pcv_rpc_completion_set_actor_tenant\(\s*actor_tenant\s*\)", d)
    if sa < 0 or not src_at or not st or st.start() < sa or src_at.start() > st.start():
        out.append("dispatcher.c: set_actor 뒤 set_actor_tenant(actor_tenant = pcv_res_actor_tenant(params, caller_role))가 없다")
    if len(re.findall(r"\.actor_tenant\s*=\s*actor_tenant\b", d)) != 2:
        out.append("dispatcher.c: 레인 제출 묶음 둘(오프로드·사전 검사)이 같은 actor_tenant 를 싣지 않는다")
                                          
    try:
        body = c_blocks.masked(srcs["gate"])
        b = next(x for x in c_blocks.scan(srcs["gate"])[0] if x.name == "pcv_res_actor_tenant")
        at = body[b.body_open:b.body_close + 1]
        for tok in ("pcv_rbac_", "pcv_res_caller_init(", "pcv_res_owner_tenant(", "pcv_disp_dispatcher_caller_role("):
            if tok in at:
                out.append(f"resource_gate.c: pcv_res_actor_tenant 가 {tok} 를 부른다(역할은 인자 — I/O 없음)")
        if not re.search(r"role\s*>=\s*PCV_ROLE_ADMIN", at):
            out.append("resource_gate.c: pcv_res_actor_tenant 에 ADMIN 갈래가 없다(리뷰 M1)")
        if "pcv_res_name_is_valid(" not in at:
            out.append("resource_gate.c: pcv_res_actor_tenant 가 이름 규칙(RA7)을 보지 않는다")
    except StopIteration:
        out.append("resource_gate.c: pcv_res_actor_tenant 가 없다")
    return out


def _read_table(policy_src: str) -> dict[str, str | None]:
                                                                          
    nc = _no_comments(policy_src)
    i = nc.find("k_vm_read_methods[] = {")
    if i < 0:
        return {}
    j = nc.find("};", i)
    return {m: (k if k else None)
            for m, k in re.findall(r'\{\s*"([a-z0-9_.]+)"\s*,\s*(?:NULL|"([a-z_]+)")\s*\}', nc[i:j])}


def manipulation_scope_violations(policy_src: str) -> list[str]:
                                                                                 
                                                                                                                  
    out = []
    table = _read_table(policy_src)
    for m in ("get_vnc_info", "vm.vnc"):
        if m in table:
            out.append(f"{m} 가 읽기 표에 있다")
    nc = _no_comments(policy_src)
    i = nc.find("pcv_disp_vm_method_requires_owner_scope(const gchar *method)\n{")
    body = nc[i:nc.find("\n}\n", i)] if i >= 0 else ""
    if not re.search(r'g_strcmp0\(method, "get_vnc_info"\) == 0\)\s*\n\s*return TRUE;', body):
        out.append("소유자 검사 함수에 get_vnc_info → TRUE 갈래가 없다")
    return out


def escalation_push_violations(alert_src: str) -> list[str]:
                                                                                 
    nc = _no_comments(alert_src)
    out = []
    if re.search(r'pcv_webpush_notify\("escalation",\s*TRUE,\s*esc_msg\)', nc):
        out.append("에스컬레이션 푸시가 원문 esc_msg 를 보낸다")
    if not re.search(r'pcv_webpush_notify\("escalation",\s*TRUE,\s*push_msg\)', nc):
        out.append("에스컬레이션 푸시가 push_msg 를 보내지 않는다")
    if not re.search(r'g_strcmp0\(r->metric, "Security"\) == 0\)\s*g_snprintf\(push_msg[^;]*details hidden', nc, re.S):
        out.append("Security 경보 가림 갈래가 없다")
    return out


def _precheck_set(policy_src: str) -> set[str]:
                                                                                        
    nc = _no_comments(policy_src)
    for b in c_blocks.scan(policy_src)[0]:
        if b.name == "pcv_disp_vm_read_needs_precheck":
            return set(re.findall(r'"([a-z0-9_.]+)"', nc[b.body_open:b.body_close + 1]))
    return set()


def _offload_rows(offload_src: str) -> dict[str, str]:
    return {m: lane for m, lane in OFFLOAD_ROW.findall(_no_comments(offload_src))}


def _policy_rows(policy_src: str) -> dict[str, int]:
    nc = _no_comments(policy_src)
    i = nc.find("g_method_policies[]")
    j = nc.find("{ NULL", i)
    out: dict[str, int] = {}
    for m, r in re.findall(r'\{\s*"([^"]+)"\s*,\s*(\d)\s*\}', nc[i:j]):
        out[m] = min(out.get(m, 9), int(r))
    return out


def policy_classification(policy_src: str) -> tuple[tuple[int, int, int], list[str]]:
                                                                      
    out = []
    nc = _no_comments(policy_src)
    blk = next((b for b in c_blocks.scan(policy_src)[0] if b.name == "pcv_disp_vm_method_requires_owner_scope"), None)
    if blk is None:
        return (0, 0, 0), ["pcv_disp_vm_method_requires_owner_scope 없음"]
    body = nc[blk.body_open:blk.body_close + 1]
    if not re.search(r"if\s*\(\s*pcv_disp_vm_method_is_read\(\s*method\s*\)\s*\)\s*return\s+FALSE\s*;", body):
        out.append("조작 범위 함수가 읽기 표 메서드를 먼저 빼지 않는다")
    named = set(re.findall(r'"(vm\.[a-z0-9_.]+)"', body)) - {"vm."}
    read = set(_read_table(policy_src))
    vm = [m for m, r in _policy_rows(policy_src).items() if m.startswith("vm.") and r < 2]
    rd = [m for m in vm if m in read]
    ex = [m for m in vm if m in named and m not in read]
    mn = [m for m in vm if m not in read and m not in named]
    if read & named:
        out.append(f"읽기 표와 면제 목록이 겹친다: {sorted(read & named)}")
    if len(vm) != len(rd) + len(ex) + len(mn):
        out.append("분류 합이 맞지 않는다")
    return (len(rd), len(ex), len(mn)), out


def read_on_main_violations(policy_src: str, offload_src: str) -> list[str]:
    rows = _offload_rows(offload_src)
    pre = _precheck_set(policy_src)
    out = []
    for m in _read_table(policy_src):
        if rows.get(m) in ("LIBVIRT_READ", "HOST_READ") or m in pre:
            continue
        out.append(f"{m}: 읽기 표 메서드가 레인({rows.get(m)})·이어받기 집합 밖이다(메인 루프 판정)")
    if not _read_table(policy_src):
        out.append("읽기 표를 찾지 못했다")
    return out


def read_deny_literal_violations(texts: dict[str, str]) -> list[str]:
                                                                            
    hits = [(p, _no_comments(t).count(f'"{READ_DENY}"')) for p, t in texts.items()]
    hits = [(p, n) for p, n in hits if n]
    if hits != [("src/api/dispatcher_owner_scope.c", 1)]:
        return [f"읽기 거절 문구 리터럴 위치: {hits}"]
    return []


def out_of_scope_violations(policy_src: str, offload_src: str) -> list[str]:
    read = set(_read_table(policy_src))
    rows = _offload_rows(offload_src)
    return [f"{m}: 범위 밖 표면이 읽기 표·SCOPE_READ 에 있다" for m in OUT_OF_SCOPE
            if m in read or rows.get(m) == "SCOPE_READ"]


def _src_texts() -> dict[str, str]:
    return {str(p.relative_to(ROOT)): p.read_text(encoding="utf-8", errors="replace")
            for p in sorted((ROOT / "src").rglob("*")) if p.suffix in (".c", ".h")}


def _dispatch_fn(src: str, name: str) -> str:
    code = c_blocks.masked(src)
    for b in c_blocks.scan(src)[0]:
        if b.name == name:
            return code[b.body_open:b.body_close + 1]
    raise AssertionError(f"함수 없음: {name}")


def precheck_wiring_violations(disp_src: str, policy_src: str) -> list[str]:
                                                                                        
    out = []
    try:
        d = _dispatch_fn(disp_src, "purecvisor_dispatcher_dispatch")
    except AssertionError as e:
        return [str(e)]
    m = re.search(r"if\s*\(\s*caller_role\s*<\s*PCV_ROLE_ADMIN\s*&&\s*pcv_disp_vm_read_needs_precheck\(\s*method\s*\)\s*\)", d)
    if not m:
        out.append("dispatch: 이어받기 분기가 caller_role < PCV_ROLE_ADMIN 을 먼저 보지 않는다")
    sub_at = d.find("pcv_rpc_offload_submit_precheck(")
    role_at = d.find("_dispatch_role_check(")
    off_at = d.find("pcv_rpc_offload_submit(")
    if sub_at < 0 or not (0 <= role_at < sub_at < off_at):
        out.append("dispatch: 순서가 역할 검사 → 사전 검사 제출 → 레인 제출이 아니다")
    if not re.search(r"pcv_rpc_offload_submit_precheck\(\s*&preq\s*,\s*PCV_OFFLOAD_LANE_LIBVIRT_READ\s*,\s*_dispatch_continue_main\s*\)", d):
        out.append("dispatch: 사전 검사가 LIBVIRT_READ·_dispatch_continue_main 이 아니다")
    try:
        cont = _dispatch_fn(disp_src, "_dispatch_continue_main")
        need = ("pcv_rpc_completion_begin(", "pcv_rpc_completion_set_actor(", "pcv_rpc_completion_set_actor_tenant(",
                "pcv_mainloop_guard_rpc_enter(", "_dispatch_route(", "_rpc_finish(", "pcv_mainloop_guard_rpc_leave(")
        pos = [cont.find(t) for t in need]
        if min(pos) < 0 or pos != sorted(pos):
            out.append("_dispatch_continue_main: 범위·가드를 다시 열고 같은 라우팅·마무리를 순서대로 하지 않는다")
    except AssertionError as e:
        out.append(str(e))
    if _precheck_set(policy_src) != PRECHECK_EXPECT:
        out.append(f"이어받기 집합 {_precheck_set(policy_src)} != {PRECHECK_EXPECT}")
    return out


def owner_check_read_branch_violations(disp_src: str) -> list[str]:
                                                                                                          
    try:
        b = _dispatch_fn(disp_src, "_dispatch_owner_check")
    except AssertionError as e:
        return [str(e)]
    out = []
    adm = re.search(r"if\s*\(\s*caller_role\s*>=\s*PCV_ROLE_ADMIN\s*\)\s*return\s+TRUE\s*;", b)
    rd = re.search(r"if\s*\(\s*pcv_disp_vm_method_is_read\(\s*method\s*\)\s*\)\s*\{\s*allowed\s*=\s*pcv_disp_vm_read_allowed\(", b)
    op = b.find("caller_role != PCV_ROLE_OPERATOR")
    if not adm or not rd or op < 0 or not (adm.start() < rd.start() < op):
        out.append("_dispatch_owner_check: ADMIN 통과 → 읽기 표 판정 → OPERATOR 조작 검사 순서가 아니다")
    return out


def owner_reader_violations(srcs: dict[str, str]) -> list[str]:
                                                                               
    sys.path.insert(0, str(ROOT / "scripts"))
    import check_owner_reader              
    out = []
    for key, fn, need in OWNER_READERS:
        src = srcs[key]
        blk = next((b for b in c_blocks.scan(src)[0] if b.name == fn), None)
        if blk is None:
            out.append(f"{key}: 함수 없음 {fn}")
            continue
        body = c_blocks.masked(src)[blk.body_open:blk.body_close + 1]
        for tok in need:
            if tok not in body:
                out.append(f"{key}:{fn}: {tok} 를 부르지 않는다")
    rel = {"owner_scope": "src/api/dispatcher_owner_scope.c", "domain_refs": "src/modules/resource/resource_domain_refs.c",
           "vpc": "src/modules/network/vpc/vpc_manager.c", "vnc": "src/api/vnc_proxy.h"}
    out += check_owner_reader.violations({rel[k]: v for k, v in srcs.items()})
    return out


def _fn_raw(src: str, name: str) -> str:
    code = c_blocks.masked(src)
    for b in c_blocks.scan(src)[0]:
        if b.name == name:
            return code[b.body_open:b.body_close + 1]
    raise AssertionError(f"함수 없음: {name}")


def vm_list_violations(src: str) -> list[str]:
    out = []
    try:
        w = _fn_raw(src, "vm_list_worker")
        m = _fn_raw(src, "handle_vm_list_request")
    except AssertionError as e:
        return [str(e)]
    sc = w.find("pcv_vm_read_scope_new_parts(")
    vis = w.find("pcv_vm_read_xml_visible(")
    add = w.find("json_array_add_object_element(array, vm_obj)")
    if min(sc, vis, add) < 0 or not (sc < vis < add):
        out.append("vm_list_worker: 범위 생성 → 행 판정 → 배열에 넣기 순서가 아니다")
    if "pcv_vm_read_scope_free(" not in w:
        out.append("vm_list_worker: 범위를 해제하지 않는다")
    for tok in ("pcv_rbac_", "pcv_res_caller_init(", "pcv_vm_read_scope_new", "virConnect", "virDomain"):
        if tok in m:
            out.append(f"handle_vm_list_request(메인): {tok} 를 부른다(역할·주체·주입 키 복사만)")
    for tok in ("ctx->caller_role", "ctx->caller_sub", "ctx->caller_tenant_known", "ctx->caller_tenant"):
        if tok not in m:
            out.append(f"handle_vm_list_request: {tok} 를 채우지 않는다")
    return out


def vm_list_owner_field_violations(src: str) -> list[str]:
    try:
        w = c_blocks.masked(src)
        blk = next(b for b in c_blocks.scan(src)[0] if b.name == "vm_list_worker")
    except StopIteration:
        return ["vm_list_worker 없음"]
    raw = _no_comments(src)[blk.body_open:blk.body_close + 1]
    i = raw.find('"owner"')
    if i < 0:
        return ["vm_list_worker: ADMIN owner 칸이 없다"]
    head = raw[max(0, i - 400):i]
    if not re.search(r"if\s*\(\s*pcv_vm_read_scope_all\(\s*scope\s*\)\s*\)", head):
        return ["vm_list_worker: owner 칸이 ADMIN(pcv_vm_read_scope_all) 갈래 안에 있지 않다"]
    if raw.count('"owner"') != 2:                    
        return [f"vm_list_worker: owner 칸 쓰기가 {raw.count(chr(34) + 'owner' + chr(34))}곳이다(ADMIN 갈래의 두 꼴만)"]
    return []


def fleet_violations(src: str) -> list[str]:
    out = []
    try:
        b = _fn_raw(src, "pcv_monitor_fleet_collect")
    except AssertionError as e:
        return [str(e)]
    if "(void)params" in b:
        out.append("pcv_monitor_fleet_collect: params 를 버린다")
    init = b.find("pcv_res_caller_init(")
    scope = b.find("pcv_vm_read_scope_new(")
    vis = b.find("pcv_vm_read_xml_visible(")
    add = b.find("json_array_add_object_element(fleet_array, vm_obj)")
    host = b.find("JsonObject *host_obj = json_object_new();")
    if min(init, scope, vis, add, host) < 0 or not (init < scope < vis < add < host):
        out.append("pcv_monitor_fleet_collect: 호출자 해석 → 범위 → VM 행 판정 → 배열 → 호스트 객체 순서가 아니다")
    if host >= 0 and re.search(r"pcv_vm_read_(visible|xml_visible|scope_all)\(", b[host:]):
        out.append("pcv_monitor_fleet_collect: 호스트 객체 조립 구간에 VM 판정이 있다(호스트 집계는 거르지 않는다)")
    if "pcv_vm_read_scope_free(" not in b:
        out.append("pcv_monitor_fleet_collect: 범위를 해제하지 않는다")
    return out


def main_loop_call_site_violations(texts: dict[str, str]) -> list[str]:
    out = []
    for rel, text in texts.items():
        code = c_blocks.masked(text)
        if not any(tok in code for tok in MAIN_LOOP_SENSITIVE):
            continue
        allowed = ALLOWED_CALLERS.get(rel, set())
        if allowed is None:
            continue
        for b in c_blocks.scan(text)[0]:
            body = code[b.body_open:b.body_close + 1]
            hit = [t for t in MAIN_LOOP_SENSITIVE if t in body]
            if hit and b.name not in allowed:
                out.append(f"{rel}:{b.name}: {hit} — 레인·워커 함수 목록 밖(R11)")
    return out


def _route_handlers(disp_src: str) -> dict[str, str]:
    return dict(re.findall(r'g_hash_table_insert\(g_rpc_routes,\s*"([a-z0-9_.]+)",\s*\(gpointer\)(\w+)\)', disp_src))


def scope_read_route_violations(offload_src: str, disp_src: str) -> list[str]:
    out = []
    rows = _offload_rows(offload_src)
    handlers = _route_handlers(disp_src)
    allowed = {fn for fns in ALLOWED_CALLERS.values() if fns for fn in fns}
    for r in SCOPE_READ_ROUTES:
        if rows.get(r) != "SCOPE_READ":
            out.append(f"{r}: SCOPE_READ 가 아니다({rows.get(r)})")
        h = handlers.get(r)
        if r != "monitor.vm.diag" and r != "vm.snapshot.schedule.list" and h not in allowed:
            out.append(f"{r}: 처리기 {h} 가 레인 호출처 허용 목록 밖")
    for r in TRACK_LANE_ROUTES:
        if rows.get(r) == "HOST_READ":
            out.append(f"{r}: 이 트랙의 라우트가 HOST_READ 에 있다")
    return out


def map_admin_skip_violations(texts: dict[str, str]) -> list[str]:
    out = []
    for rel, text in texts.items():
        code = c_blocks.masked(text)
        if "pcv_vm_owner_map_current(" not in code:
            continue
        for b in c_blocks.scan(text)[0]:
            body = code[b.body_open:b.body_close + 1]
            if "pcv_vm_owner_map_current(" not in body or rel == "src/modules/resource/vm_read_scope.c":
                continue
            if (rel, b.name) == ("src/api/ws_server.c", "_ws_broadcast_scoped_fanout"):                              
                if not re.search(r"PCV_WS_SCOPE_FILTER\s*&&\s*role\s*<\s*PCV_ROLE_ADMIN", body):
                    out.append("_ws_broadcast_scoped_fanout: ADMIN 주체에도 지도를 만든다")
                continue
            if (rel, b.name) == ("src/api/rest_public.c", "_metrics_render"):                              
                if not re.search(r"if\s*\(\s*!\s*_collector_scoped\(c\)\s*\)", body):
                    out.append("_metrics_render: ADMIN·직접 로컬 갈래에서도 지도를 만든다")
                continue
            if (rel, b.name) != ("src/modules/resource/vm_read_filter.c", "pcv_vm_read_view_init"):
                out.append(f"{rel}:{b.name}: 지도를 요청 보기 밖에서 만든다")
                continue
            if not re.search(r"pcv_vm_read_scope_all\([^;]*\)\s*\?\s*NULL\s*:\s*pcv_vm_owner_map_current\(", body):
                out.append("pcv_vm_read_view_init: ADMIN 갈래에서도 지도를 만든다")
    return out


def schedule_shares_policy_violations(src: str) -> list[str]:
    out = []
    try:
        sched = _fn_raw(src, "handle_snapshot_schedule_list")
        pol = _no_comments(src)
        blk = next(b for b in c_blocks.scan(src)[0] if b.name == "handle_backup_policy_list")
        pol = pol[blk.body_open:blk.body_close + 1]
    except (AssertionError, StopIteration) as e:
        return [f"함수 없음: {e}"]
    if not re.search(r"handle_backup_policy_list\(\s*params\s*,", sched):
        out.append("handle_snapshot_schedule_list: 정책 목록 처리기에 위임하지 않는다")
    if "pure_rpc_build_success_response(" in sched:
        out.append("handle_snapshot_schedule_list: 응답을 따로 만든다(거르기를 우회)")
    if not re.search(r'pcv_vm_read_filter_by_member\([^;]*"vm_name"\s*,\s*TRUE\s*\)', pol):
        out.append('handle_backup_policy_list: "vm_name"·keep_star TRUE 거르기가 없다')
    return out


def healing_pending_violations(src: str) -> list[str]:
    try:
        b = _fn_raw(src, "handle_healing_pending")
    except AssertionError as e:
        return [str(e)]
    out = []
    role = b.find("pcv_disp_dispatcher_caller_role(")
    get = b.find("pcv_healing_get_pending_json(")
    if role < 0 or get < 0 or role > get:
        out.append("handle_healing_pending: 원천을 읽기 전에 역할을 보지 않는다")
    for tok in ("pcv_res_caller_init(", "pcv_vm_read_", "pcv_rbac_"):
        if tok in b:
            out.append(f"handle_healing_pending: {tok} — 역할만 본다(메인 루프, I/O 0)")
    return out


def single_not_found_violations(src: str) -> list[str]:
    nc = _no_comments(src)
    try:
        blk = next(b for b in c_blocks.scan(src)[0] if b.name == "handle_security_event_get")
    except StopIteration:
        return ["handle_security_event_get 없음"]
    body = nc[blk.body_open:blk.body_close + 1]
    out = []
    if body.count('"security event not found"') != 1:
        out.append(f'handle_security_event_get: "security event not found" 자리 {body.count(chr(34) + "security event not found" + chr(34))}곳')
    vis = body.find("pcv_vm_read_security_row_visible(")
    nf = body.find('"security event not found"')
    if vis < 0 or nf < 0 or vis > nf:
        out.append("handle_security_event_get: 범위 판정이 '없음' 응답 앞에 없다")
    return out


def alert_mask_violations(alert_src: str, engine_src: str) -> list[str]:
    out = []
    try:
        h = _fn_raw(alert_src, "handle_alert_history")
    except AssertionError as e:
        return [str(e)]
    mask = h.find("pcv_vm_read_mask_security_alerts(")
    page = h.find("pcv_disp_paginate_array(")
    if mask < 0 or page < 0 or mask > page:
        out.append("handle_alert_history: 페이지 전에 문구를 가리지 않는다")
    if "pcv_vm_read_filter_" in h:
        out.append("handle_alert_history: 행을 거른다(문구만 가린다 — 행 수 불변, D-16)")
    return out


def job_sql_violations(queue_src: str, scope_src: str) -> list[str]:
    out = []
    nc = _no_comments(queue_src)
    for lit in re.findall(r'"([^"\n]*)"', nc):
        if re.search(r"\bactor(_tenant)?\s*(=|IS)\s*\?", lit, re.I):
            out.append(f"장부 SQL 에 행위자 비교: {lit}")
    try:
        body = _bodies(queue_src)["_job_visible_fn"][0]
    except KeyError:
        return out + ["_job_visible_fn 없음"]
    if "->decide(" not in body:
        out.append("_job_visible_fn: 범위의 decide 콜백을 부르지 않는다")
    if "sqlite3_create_function_v2(" not in nc or "pcv_job_visible" not in nc:
        out.append("pcv_job_visible 등록이 없다")
    try:
        js = _bodies(scope_src)["pcv_vm_read_job_scope"][0]
    except KeyError:
        return out + ["pcv_vm_read_job_scope 없음"]
    if not re.search(r"->decide\s*=\s*pcv_vm_read_decide\b", js):
        out.append("pcv_vm_read_job_scope: decide 에 pcv_vm_read_decide 를 넘기지 않는다")
    return out


def job_not_found_violations(jobs_src: str, admin_src: str, texts: dict[str, str]) -> list[str]:
    out = []
    nc = _no_comments(jobs_src)
    blk = next((b for b in c_blocks.scan(jobs_src)[0] if b.name == "handle_jobs_get"), None)
    if not blk:
        return ["handle_jobs_get 없음"]
    body = nc[blk.body_open:blk.body_close + 1]
    if body.count('"Job not found"') != 1:
        out.append('handle_jobs_get: "Job not found" 자리가 하나가 아니다')
    get = body.find("pcv_job_get(")
    if get < 0 or "&js" not in body[get:get + 60] or get > body.find('"Job not found"'):
        out.append("handle_jobs_get: 범위 조회(pcv_job_get(.., &js))가 '없음' 자리 앞에 없다")
    hits = [rel for rel, t in texts.items() if '"not_found"' in _no_comments(t)]
    if hits != ["src/modules/dispatcher/vm_delete_status.c"]:
        out.append(f'"not_found" 조립 위치: {hits}')
    try:
        a = _fn_raw(admin_src, "handle_vm_delete_status")
    except AssertionError as e:
        return out + [str(e)]
    if "pcv_job_find_latest(\"vm.delete\", vm, &jscope, &job)" not in _no_comments(admin_src):
        out.append("handle_vm_delete_status: 범위 최신 찾기가 아니다")
    if "pcv_vm_read_job_scope(" not in a:
        out.append("handle_vm_delete_status: 요청 범위를 만들지 않는다")
    return out


def owner_set_wiring_violations(policy: str, offload: str, rest: str, cli: str, params_json: str) -> list[str]:
    import json
    out = []
    if not re.search(r'\{\s*"vm\.owner\.set"\s*,\s*2\s*\}', policy):
        out.append("정책표: vm.owner.set 2(ADMIN) 행이 없다")
    if _offload_rows(offload).get("vm.owner.set") != "VM_DEVICE_MUTATE":
        out.append("레인: vm.owner.set 이 VM_DEVICE_MUTATE 가 아니다")
    if 'pcv_rest_build_rpc("vm.owner.set"' not in rest or not re.search(r'"owner"\)\s*==\s*0\s*&&\s*g_strcmp0\(method,\s*"PUT"\)', rest):
        out.append("REST: PUT /vms/{n}/owner 매핑이 없다")
    if not re.search(r'\{"vm","owner",\s*cmd_vm_owner,\s*"Transfer VM ownership \(admin\)"\}', cli):
        out.append("CLI: 명령표 vm owner 행이 없다")
    try:
        row = json.loads(params_json).get("vm.owner.set")
    except ValueError as e:
        return out + [f"rpc_params.json 파싱 실패: {e}"]
    if not row or (row.get("required"), row.get("optional")) != (["name", "owner"], ["expected_owner"]):
        out.append(f"rpc_params.json: vm.owner.set 행 {row}")
    return out


def owner_set_reject_violations(src: str, names_src: str | None = None) -> list[str]:
                                                                                                            
                                                                                                  
    out = []
    names = _no_comments(names_src if names_src is not None else _read(PRINCIPAL_NAMES_C))
    i = names.find("k_reserved[] = {")
    block = names[i:names.find("};", i)] if i >= 0 else ""
    for tok in ("PCV_GRPC_CALLER_SUBJECT", "PCV_AUDIT_ACTOR_LOCAL", "PCV_AUDIT_ACTOR_SYSTEM", "PCV_PRINCIPAL_AI_AGENT",
                "PCV_PRINCIPAL_AI_OPS", "PCV_PRINCIPAL_UNKNOWN_ACTOR"):
        if tok not in block:
            out.append(f"예약 이름 목록에 {tok} 가 없다")
    if '"admin"' in block:
        out.append("예약 이름 목록에 admin 이 있다(실제 사용자 이름)")
    try:
        body = _fn_raw(src, "handle_vm_owner_set")
    except AssertionError as e:
        return out + [str(e)]
    rsv, exists, lock = body.find("pcv_principal_name_is_reserved(owner)"), body.find("pcv_rbac_user_exists("), body.find("pcv_vm_lock_acquire_ex(")
    if min(rsv, exists, lock) < 0 or not (rsv < exists < lock):
        out.append("handle_vm_owner_set: 예약 이름 거절 → 사용자 존재 확인 → 잠금 순서가 아니다")
    return out


def owner_set_user_violations(src: str) -> list[str]:
    try:
        body = _fn_raw(src, "handle_vm_owner_set")
    except AssertionError as e:
        return [str(e)]
    out = []
    if "exists != PCV_USER_PRESENT" not in body:
        out.append("handle_vm_owner_set: RBAC 에 없는 사용자를 거절하지 않는다")
    if "exists == PCV_USER_UNKNOWN" not in body:
        out.append("handle_vm_owner_set: 명부 조회 실패를 따로 답하지 않는다")
    return out


def owner_set_after_violations(src: str) -> list[str]:
    try:
        body = _fn_raw(src, "handle_vm_owner_set")
    except AssertionError as e:
        return [str(e)]
    marks = [body.find(t) for t in ("pcv_vm_owner_metadata_set(", "pcv_vm_diag_sampler_invalidate_owners(",
                                    "pcv_ws_close_vm_streams(", "pure_rpc_build_success_response(")]
    if min(marks) < 0 or marks != sorted(marks):
        return ["handle_vm_owner_set: 쓰기 → 표본기 무효화 → VNC 닫기 → 성공 응답 순서가 아니다"]
    return []


def cli_inspect_moved_violations(main_src: str, owner_src: str) -> list[str]:
    out = []
    if "void cmd_vm_inspect(int argc, char *argv[]) {" in _no_comments(main_src):
        out.append("purecvisorctl.c 에 cmd_vm_inspect 본문이 남았다")
    if "void cmd_vm_inspect(int argc, char *argv[]) {" not in _no_comments(owner_src):
        out.append("cli_vm_owner.c 에 cmd_vm_inspect 가 없다")
    return out


def owner_metadata_api_violations(texts: dict[str, str]) -> list[str]:
    out = []
    for rel, text in texts.items():
        code = _no_comments(text)
        if re.search(r"virDomainGetMetadata\s*\([^;]*PCV_VM_OWNER_NS_URI", code):
            out.append(f"{rel}: 소유자를 metadata 조회 API 로 읽는다")
        if "pcv_vm_owner_from_metadata_element" in c_blocks.masked(text):
            out.append(f"{rel}: API 조각용 해석 함수가 남아 있다")
    return out


def _rest_path_block(src: str, path: str) -> str:
    nc = _no_comments(src)
    i = nc.find(f'REST_API_PREFIX "{path}"')
    if i < 0:
        return ""
    b = nc.find("{", i)
    depth = 0
    for k in range(b, len(nc)):
        depth += nc[k] == "{"
        depth -= nc[k] == "}"
        if depth == 0:
            return nc[b:k + 1]
    return ""


def internal_paths_violations(src: str) -> list[str]:
    out = []
    for path in ("/internal/vms", "/internal/telemetry"):
        blk = _rest_path_block(src, path)
        gate, deny, render = blk.find("pcv_rest_collector_gate("), blk.find("_collector_denied("), blk.find("pcv_rest_defer_rendered(")
        if min(gate, deny, render) < 0 or not (gate < deny < render):
            out.append(f"{path}: 문지기 → 비ADMIN 403(_collector_denied) → 렌더 순서가 아니다")
    try:
        d = _fn_raw(src, "_collector_denied")
        raw = _no_comments(src)
        blk = next(b for b in c_blocks.scan(src)[0] if b.name == "_collector_denied")
        draw = raw[blk.body_open:blk.body_close + 1]
    except (AssertionError, StopIteration):
        return out + ["_collector_denied 없음"]
    if "_collector_scoped(" not in d:
        out.append("_collector_denied: 비ADMIN 토큰 판정이 없다")
    if not re.search(r'pcv_audit_log\([^;]*"denied"\s*,\s*PURE_RPC_ERR_FORBIDDEN\s*,\s*0\s*,\s*"rbac"\s*\)', draw):
        out.append("_collector_denied: 거절 감사 1행(denied·FORBIDDEN·rbac)이 없다")
    if 'pcv_rest_send_error(msg, 403, "FORBIDDEN", "Forbidden")' not in draw:
        out.append("_collector_denied: 403 FORBIDDEN 응답이 없다")
    return out


def recent_errors_violations(src: str) -> list[str]:
    out = []
    blk = _rest_path_block(src, "/health/recent-errors")
    if not re.search(r"_collector_scoped\(&cc\)\s*\?\s*pcv_audit_recent_failures\(\s*_own_failure_row\s*,", blk):
        out.append("recent-errors: 비ADMIN 토큰에 행 콜백(_own_failure_row)을 넘기지 않는다")
    try:
        body = _fn_raw(src, "_own_failure_row")
    except AssertionError as e:
        return out + [str(e)]
    if not re.search(r"pcv_vm_read_decide\([^;]*,\s*NULL\s*,\s*username\s*,\s*NULL\s*\)", body):
        out.append("_own_failure_row: pcv_vm_read_decide(역할, 주체, NULL, username, NULL) 이 아니다")
    return out


def metrics_worker_violations(src: str) -> list[str]:
    out = []
    try:
        handler = _fn_raw(src, "pcv_rest_metrics")
        worker = _fn_raw(src, "_metrics_render")
    except AssertionError as e:
        return [str(e)]
    for tok in ("pcv_vm_read_scope_new", "pcv_vm_owner_map_current(", "pcv_rbac_"):
        if tok in handler:
            out.append(f"pcv_rest_metrics(REST 스레드): {tok}")
    for tok in ("pcv_res_rest_tenant_subject(", "pcv_vm_read_scope_new_parts(", "pcv_vm_owner_map_current(", "_collector_scoped("):
        if tok not in worker:
            out.append(f"_metrics_render: {tok} 가 없다")
    return out


def ws_legacy_violations(texts: dict[str, str]) -> list[str]:
    out = []
    for rel in WS_LEGACY_VM:
        n = len(LEGACY_JOB_CALL.findall(c_blocks.masked(texts[rel])))
        if n:
            out.append(f"{rel}: 옛 job.complete 방송 {n}곳(행위자 없는 전부 분배)")
    n = sum(len(LEGACY_JOB_CALL.findall(c_blocks.masked(texts[rel]))) for rel in WS_LEGACY_CONTAINER)
    if n != 10:
        out.append(f"컨테이너 옛 방송 {n}곳(기대 10 — R10 그대로)")
    return out


def ws_producer_violations(texts: dict[str, str], filter_src: str) -> list[str]:
    try:
        kind_body = _fn(filter_src, "pcv_ws_scope_kind")
    except AssertionError as e:
        return [str(e)]
    out, seen = [], set()
    for rel, text in texts.items():
        for t in re.findall(r'\bpcv_ws_broadcast\s*\(\s*"([^"]+)"', _no_comments(text)):
            seen.add(t)
            if f'"{t}"' not in kind_body:
                out.append(f"{rel}: 방송 유형 {t} 가 분배 유형 표에 없다")
    if len(seen) < 7:
        out.append(f"생산자 유형이 {len(seen)}개뿐이다(스캔 확인)")
    return out


def ws_rbac_main_violations(ws_src: str) -> list[str]:
    out = []
    for fn in ("pcv_ws_broadcast", "_ws_broadcast_scoped", "pcv_ws_broadcast_job_complete_scoped"):
        try:
            body = _fn_raw(ws_src, fn)
        except AssertionError as e:
            out.append(str(e))
            continue
        for tok in ("pcv_rbac_", "pcv_ws_subject_cache", "pcv_vm_owner_map_current(", "pcv_vm_read_scope_new"):
            if tok in body:
                out.append(f"{fn}: {tok} — 메인에서 부르는 진입점에 명부·지도 조회")
    try:
        fan = _fn_raw(ws_src, "_ws_broadcast_scoped_fanout")
    except AssertionError as e:
        return out + [str(e)]
    if "pcv_ws_subject_cache_lookup(" not in fan:
        out.append("_ws_broadcast_scoped_fanout: 주체마다 명부 조회(분배 2단계)가 없다")
    return out


def principal_key_no_tenant_violations(srcs: dict[str, str]) -> list[str]:
                                                                               
    out = []
    rs_raw = srcs["rest_server"]
    rs = c_blocks.masked(rs_raw)
    blk = next((b for b in c_blocks.scan(rs_raw)[0] if b.name == "_rest_caller_tenant"), None)
    if blk is None:
        return ["rest_server.c: 함수 없음 _rest_caller_tenant"]
    body = rs[blk.body_open:blk.body_close + 1]
    m = re.search(r"(\w+)\s*=\s*pcv_res_rest_tenant_subject\(\s*(?:\(gint\)\s*)?caller_role\s*,\s*rq->key_role\s*>=\s*0\s*,"
                  r"\s*rq->subject\s*\)", body)
    if not m:
        out.append("rest_server.c: _rest_caller_tenant 가 pcv_res_rest_tenant_subject(caller_role, rq->key_role >= 0, rq->subject)"
                   " 로 찾을 주체를 정하지 않는다")
    calls = re.findall(r"pcv_rbac_get_tenant\(\s*([^)]*?)\s*\)", rs)
    if len(calls) != 1 or (m and calls[0] != m.group(1)):
        out.append(f"rest_server.c: pcv_rbac_get_tenant 는 순수 판정이 돌려준 주체로 한 번만 불러야 한다(지금 {calls})")
    if re.search(r"pcv_rbac_get_tenant\(\s*rq->subject\s*\)", rs):
        out.append("rest_server.c: 판정 없이 rq->subject 로 RBAC 테넌트를 읽는다")
    g_raw = srcs["gate"]
    g = c_blocks.masked(g_raw)
    gb = next((b for b in c_blocks.scan(g_raw)[0] if b.name == "pcv_res_rest_tenant_subject"), None)
    if gb is None:
        return out + ["resource_gate.c: 함수 없음 pcv_res_rest_tenant_subject"]
    pb = g[gb.body_open:gb.body_close + 1]
    ret_subject = pb.find("return subject")
    admin = re.search(r"if\s*\(\s*role\s*>=\s*PCV_ROLE_ADMIN\s*\)\s*return\s+NULL\s*;", pb)
    key = re.search(r"if\s*\(\s*api_key\s*\)\s*return\s+NULL\s*;", pb)
    if not admin or (ret_subject >= 0 and admin.start() > ret_subject):
        out.append("resource_gate.c: pcv_res_rest_tenant_subject 가 ADMIN 이면 주체보다 먼저 NULL 을 돌려주지 않는다")
    if not key or (ret_subject >= 0 and key.start() > ret_subject):
        out.append("resource_gate.c: pcv_res_rest_tenant_subject 가 API 키 요청이면 주체보다 먼저 NULL 을 돌려주지 않는다")
    for tok in ("pcv_rbac_", "pcv_res_owner_tenant(", "pcv_config_"):
        if tok in pb:
            out.append(f"resource_gate.c: pcv_res_rest_tenant_subject 가 {tok} 를 부른다(순수 판정이어야 한다)")
    return out


class VmReadScopeContract(unittest.TestCase):
    def test_ws_legacy_vm_calls_scoped(self):
        texts = _src_texts()
        self.assertEqual(ws_legacy_violations(texts), [])
        rel = "src/modules/virt/vm_ops.c"
        bad = dict(texts)
        bad[rel] = texts[rel].replace("pcv_ws_broadcast_job_complete_scoped(", "pcv_ws_broadcast_job_complete_mt(", 1)
        self.assertNotEqual(bad[rel], texts[rel])
        self.assertTrue(ws_legacy_violations(bad))

    def test_ws_producers_classified(self):
        texts, flt = _src_texts(), _read(READ_FILTER_C)
        self.assertEqual(ws_producer_violations(texts, flt), [])
        bad = flt.replace('"anomaly"', '"anomaly-x"', 1)
        self.assertNotEqual(bad, flt)
        self.assertTrue(ws_producer_violations(texts, bad))

    def test_ws_rbac_off_main(self):
        src = _read(WS_SERVER)
        self.assertEqual(ws_rbac_main_violations(src), [])
        blk = next(b for b in c_blocks.scan(src)[0] if b.name == "pcv_ws_broadcast")
        bad = src[:blk.body_open + 1] + "\n    (void)pcv_rbac_get_tenant(type);" + src[blk.body_open + 1:]
        self.assertTrue(ws_rbac_main_violations(bad))

    def test_internal_paths_admin_or_direct(self):
        src = _read(REST_PUBLIC)
        self.assertEqual(internal_paths_violations(src), [])
        bad = src.replace('        if (_collector_denied(msg, &cc, REST_API_PREFIX "/internal/vms")) return PCV_REST_STEP_CLEANUP;\n', "", 1)
        self.assertNotEqual(bad, src)
        self.assertTrue(internal_paths_violations(bad))

    def test_recent_errors_actor_filter(self):
        src = _read(REST_PUBLIC)
        self.assertEqual(recent_errors_violations(src), [])
        bad = src.replace("pcv_audit_recent_failures(_own_failure_row, &cc,", "pcv_audit_recent_failures(NULL, NULL,", 1)
        self.assertNotEqual(bad, src)
        self.assertTrue(recent_errors_violations(bad))

    def test_metrics_render_scope_in_worker(self):
        src = _read(REST_PUBLIC)
        self.assertEqual(metrics_worker_violations(src), [])
        bad = src.replace("pcv_res_rest_tenant_subject(c->role", "_no_lookup(c->role", 1)
        self.assertNotEqual(bad, src)
        self.assertTrue(metrics_worker_violations(bad))

    def test_auth_none_warn_text(self):
        src = _no_comments(_read(REST_PUBLIC))
        blk = next(b for b in c_blocks.scan(_read(REST_PUBLIC))[0] if b.name == "pcv_rest_collector_auth_log_config")
        body = src[blk.body_open:blk.body_close + 1]
        self.assertIn("모든 테넌트 VM 계열 공개", body)

    def test_owner_metadata_api_not_read(self):
        texts = _src_texts()
        self.assertEqual(owner_metadata_api_violations(texts), [])
        rel = "src/modules/virt/vm_owner_meta.c"
        bad = dict(texts)
        bad[rel] = texts[rel] + "\nstatic void x(virDomainPtr d) { (void)virDomainGetMetadata(d, 0, PCV_VM_OWNER_NS_URI, 0); }\n"
        self.assertTrue(owner_metadata_api_violations(bad))

    def test_owner_set_policy_and_lane(self):
        args = [_read(POLICY), _read(OFFLOAD), _read(REST_COMPUTE), _read(CLI_MAIN), _read(RPC_PARAMS)]
        self.assertEqual(owner_set_wiring_violations(*args), [])
        bad = list(args)
        bad[0] = re.sub(r'(\{\s*"vm\.owner\.set"\s*,\s*)2', r"\g<1>1", args[0], count=1)
        self.assertNotEqual(bad[0], args[0])
        self.assertTrue(owner_set_wiring_violations(*bad))

    def test_owner_set_rejects_grpc_subject(self):
        src, names = _read(VM_OWNER_HANDLER), _read(PRINCIPAL_NAMES_C)
        self.assertEqual(owner_set_reject_violations(src, names), [])
                                                
        bad = names.replace("    PCV_GRPC_CALLER_SUBJECT,\n", "", 1)
        self.assertNotEqual(bad, names)
        self.assertTrue(owner_set_reject_violations(src, bad))
                                                
        bad = src.replace("pcv_principal_name_is_reserved(owner)", "FALSE", 1)
        self.assertNotEqual(bad, src)
        self.assertTrue(owner_set_reject_violations(bad, names))

    def test_owner_set_requires_existing_user(self):
        src = _read(VM_OWNER_HANDLER)
        self.assertEqual(owner_set_user_violations(src), [])
        bad = src.replace("exists != PCV_USER_PRESENT", "FALSE", 1)
        self.assertNotEqual(bad, src)
        self.assertTrue(owner_set_user_violations(bad))

    def test_owner_set_invalidates_and_closes(self):
        src = _read(VM_OWNER_HANDLER)
        self.assertEqual(owner_set_after_violations(src), [])
        bad = re.sub(r"\s*pcv_vm_diag_sampler_invalidate_owners\(\);[^\n]*", "", src, count=1)
        self.assertNotEqual(bad, src)
        self.assertTrue(owner_set_after_violations(bad))

    def test_cli_monitor_moved(self):
        m, o = _read(CLI_MAIN), _read(CLI_OWNER)
        self.assertEqual(cli_inspect_moved_violations(m, o), [])
        self.assertTrue(cli_inspect_moved_violations(m + "\nvoid cmd_vm_inspect(int argc, char *argv[]) {\n}\n", o))

    def test_job_sql_uses_decide(self):
        q, sc = _read(JOB_QUEUE), _read(SCOPE_C)
        self.assertEqual(job_sql_violations(q, sc), [])
        bad = q.replace("pcv_job_visible(actor,actor_tenant)", "actor = ?", 1)
        self.assertNotEqual(bad, q)
        self.assertTrue(job_sql_violations(bad, sc))
        bad = sc.replace("= pcv_vm_read_decide;", "= NULL;", 1)
        self.assertNotEqual(bad, sc)
        self.assertTrue(job_sql_violations(q, bad))

    def test_job_not_found_paths(self):
        texts = _src_texts()
        j, a = _read(JOBS), _read(VM_ADMIN)
        self.assertEqual(job_not_found_violations(j, a, texts), [])
        bad = j.replace("pcv_job_get(job_id, &js)", "pcv_job_get(job_id, NULL)", 1)
        self.assertNotEqual(bad, j)
        self.assertTrue(job_not_found_violations(bad, a, texts))

    def test_healing_pending_non_admin_empty(self):
        src = _read(ALERT)
        self.assertEqual(healing_pending_violations(src), [])
        bad = src.replace("pcv_disp_dispatcher_caller_role(params, connection) < PCV_ROLE_ADMIN",
                          "pcv_res_caller_init(NULL, params, connection), FALSE", 1)
        self.assertNotEqual(bad, src)
        self.assertTrue(healing_pending_violations(bad))

    def test_single_not_found_paths(self):
        src = _read(SECURITY)
        self.assertEqual(single_not_found_violations(src), [])
        bad = src.replace("pcv_vm_read_security_row_visible(", "_skip_visible(", 1)
        self.assertTrue(single_not_found_violations(bad))

    def test_alert_mask_keeps_has_more(self):
        a, e = _read(ALERT), _read(ALERT_ENGINE)
        self.assertEqual(alert_mask_violations(a, e), [])
        bad = a.replace("pcv_vm_read_mask_security_alerts(", "_skip_mask(", 1)
        self.assertNotEqual(bad, a)
        self.assertTrue(alert_mask_violations(bad, e))

    def test_scope_read_routes(self):
        off, disp = _read(OFFLOAD), _read(DISPATCHER)
        self.assertEqual(scope_read_route_violations(off, disp), [])
                                           
        bad = off.replace('{ "vm.numa.info", PCV_OFFLOAD_LANE_SCOPE_READ }', '{ "vm.numa.info", PCV_OFFLOAD_LANE_HOST_READ }', 1)
        self.assertNotEqual(bad, off)
        self.assertTrue(scope_read_route_violations(bad, disp))

    def test_map_lists_admin_skip_map(self):
        texts = _src_texts()
        self.assertEqual(map_admin_skip_violations(texts), [])
        rel = "src/modules/resource/vm_read_filter.c"
                                          
        bad = dict(texts)
        bad[rel] = re.sub(r"pcv_vm_read_scope_all\(v->scope\)\s*\?\s*NULL\s*:\s*", "", texts[rel], count=1)
        self.assertNotEqual(bad[rel], texts[rel])
        self.assertTrue(map_admin_skip_violations(bad))
                                      
        h = "src/modules/dispatcher/handler_vm_admin.c"
        bad = dict(texts)
        bad[h] = texts[h].replace("    PcvVmReadView view;\n",
                                  "    PcvVmReadView view;\n    pcv_vm_owner_map_free(pcv_vm_owner_map_current(0));\n", 1)
        self.assertNotEqual(bad[h], texts[h])
        self.assertTrue(map_admin_skip_violations(bad))

    def test_schedule_list_shares_policy_filter(self):
        src = _read(BACKUP)
        self.assertEqual(schedule_shares_policy_violations(src), [])
                                        
        bad = re.sub(r'("vm_name"\s*,\s*)TRUE', r"\1FALSE", src, count=1)
        self.assertNotEqual(bad, src)
        self.assertTrue(schedule_shares_policy_violations(bad))

    def test_vm_list_worker_filters(self):
        src = _read(VM_LIFECYCLE)
        self.assertEqual(vm_list_violations(src), [])
                                               
        bad = re.sub(r"pcv_vm_read_xml_visible\(", "_skip_visible(", src, count=1)
        self.assertNotEqual(bad, src)
        self.assertTrue(vm_list_violations(bad))
                                    
        bad = src.replace("    ctx->caller_role = ", "    (void)pcv_vm_read_scope_new_parts(0, NULL, TRUE, NULL);\n    ctx->caller_role = ", 1)
        self.assertNotEqual(bad, src)
        self.assertTrue(vm_list_violations(bad))

    def test_vm_list_admin_owner_field(self):
        src = _read(VM_LIFECYCLE)
        self.assertEqual(vm_list_owner_field_violations(src), [])
                                                      
        bad = re.sub(r"if \(pcv_vm_read_scope_all\(scope\)\)", "if (TRUE)", src, count=1)
        self.assertNotEqual(bad, src)
        self.assertTrue(vm_list_owner_field_violations(bad))

    def test_fleet_collect_filters_vm_rows_only(self):
        src = _read(MONITOR)
        self.assertEqual(fleet_violations(src), [])
                                                  
        bad = re.sub(r"pcv_vm_read_xml_visible\(", "_skip_visible(", src, count=1)
        self.assertNotEqual(bad, src)
        self.assertTrue(fleet_violations(bad))

    def test_main_loop_call_sites(self):
        texts = _src_texts()
        self.assertEqual(main_loop_call_site_violations(texts), [])
                                                    
        rel = "src/modules/dispatcher/handler_vm_lifecycle.c"
        bad = dict(texts)
        bad[rel] = texts[rel].replace("    ctx->caller_role = ", "    (void)pcv_vm_read_scope_new_parts(0, NULL, TRUE, NULL);\n    ctx->caller_role = ", 1)
        self.assertNotEqual(bad[rel], texts[rel])
        self.assertTrue(main_loop_call_site_violations(bad))

    def test_owner_readers_single_function(self):
        srcs = {"owner_scope": _read(OWNER_SCOPE), "domain_refs": _read(DOMAIN_REFS), "vpc": _read(VPC_MANAGER),
                "vnc": _read(VNC_PROXY)}
        self.assertEqual(owner_reader_violations(srcs), [])
                                                             
        bad = dict(srcs, domain_refs=srcs["domain_refs"].replace("pcv_vm_owner_from_domain_node(root)",
                                                                 "find_owner(root)", 1)
                   + '\nstatic gchar *find_owner(xmlNode *n) { return node_is(n, "owner") ? NULL : NULL; }\n')
        self.assertNotEqual(bad["domain_refs"], srcs["domain_refs"])
        self.assertTrue(owner_reader_violations(bad))
                                              
        bad = dict(srcs, vnc=srcs["vnc"].replace("pcv_vm_owner_from_domain_node(", "_xpath_owner_lookup(", 1))
        self.assertNotEqual(bad["vnc"], srcs["vnc"])
        self.assertTrue(owner_reader_violations(bad))

    def test_precheck_only_non_admin(self):
        d, p = _read(DISPATCHER), _read(POLICY)
        self.assertEqual(precheck_wiring_violations(d, p), [])
                                                      
        bad = d.replace("if (caller_role < PCV_ROLE_ADMIN && pcv_disp_vm_read_needs_precheck(method)) {",
                        "if (pcv_disp_vm_read_needs_precheck(method)) {", 1)
        self.assertNotEqual(bad, d)
        self.assertTrue(precheck_wiring_violations(bad, p))
                                             
        bad = d.replace("    pcv_rpc_completion_set_actor_tenant(req->actor_tenant);\n", "", 1)
        self.assertNotEqual(bad, d)
        self.assertTrue(precheck_wiring_violations(bad, p))

    def test_owner_check_read_branch(self):
        d = _read(DISPATCHER)
        self.assertEqual(owner_check_read_branch_violations(d), [])
                                                                        
        bad = d.replace("    if (caller_role >= PCV_ROLE_ADMIN)\n        return TRUE;\n    gchar *deny_message = NULL;",
                        "    if (caller_role != PCV_ROLE_OPERATOR)\n        return TRUE;\n    gchar *deny_message = NULL;", 1)
        self.assertNotEqual(bad, d)
        self.assertTrue(owner_check_read_branch_violations(bad))

    def test_policy_vm_methods_classified(self):
        src = _read(POLICY)
        counts, bad = policy_classification(src)
        self.assertEqual(bad, [])
        self.assertEqual(counts, CLASS_EXPECT)
                                                   
        cut = src.replace('    { "vm.blkio.get",      NULL },\n', "", 1)
        self.assertNotEqual(cut, src)
        self.assertNotEqual(policy_classification(cut)[0], CLASS_EXPECT)
                                              
        cut = re.sub(r"    if \(pcv_disp_vm_method_is_read\(method\)\)[^\n]*\n        return FALSE;\n", "", src, count=1)
        self.assertNotEqual(cut, src)
        self.assertTrue(policy_classification(cut)[1])

    def test_read_methods_never_on_main(self):
        pol, off = _read(POLICY), _read(OFFLOAD)
        self.assertEqual(read_on_main_violations(pol, off), [])
                                                                
        cut = re.sub(r'\s*\{ "vm\.sla\.report", PCV_OFFLOAD_LANE_LIBVIRT_READ \},', "", off, count=1)
        self.assertNotEqual(cut, off)
        self.assertTrue(read_on_main_violations(pol, cut))
                                                                         
        cut = pol.replace('"vm.metrics", "vm.guest.agent.status", "vm.guest.fsinfo"', '"vm.metrics", "vm.guest.fsinfo"', 1)
        self.assertNotEqual(cut, pol)
        self.assertTrue(read_on_main_violations(cut, off))

    def test_read_deny_single_literal(self):
        texts = _src_texts()
        self.assertEqual(read_deny_literal_violations(texts), [])
                                          
        bad = dict(texts)
        bad["src/api/dispatcher.c"] = bad["src/api/dispatcher.c"] + f'\nstatic const char *x = "{READ_DENY}";\n'
        self.assertTrue(read_deny_literal_violations(bad))

    def test_escalation_push_masks_security(self):
        src = _read(ALERT_ENGINE)
        self.assertEqual(escalation_push_violations(src), [])
                                          
        bad = src.replace('pcv_webpush_notify("escalation", TRUE, push_msg)', 'pcv_webpush_notify("escalation", TRUE, esc_msg)', 1)
        self.assertNotEqual(bad, src)
        self.assertTrue(escalation_push_violations(bad))
                                                
        bad = src.replace('if (g_strcmp0(r->metric, "Security") == 0)', 'if (FALSE)', 1)
        self.assertNotEqual(bad, src)
        self.assertTrue(escalation_push_violations(bad))

    def test_manipulation_scope_unchanged(self):
        src = _read(POLICY)
        self.assertEqual(manipulation_scope_violations(src), [])
                                                     
        bad = src.replace('    { "backup.history",    "vm_name" },\n',
                          '    { "backup.history",    "vm_name" },\n    { "get_vnc_info", NULL },\n', 1)
        self.assertNotEqual(bad, src)
        self.assertTrue(manipulation_scope_violations(bad))
                                                         
        bad = re.sub(r'    if \(g_strcmp0\(method, "get_vnc_info"\) == 0\)[^\n]*\n        return TRUE;\n', "", src, count=1)
        self.assertNotEqual(bad, src)
        self.assertTrue(manipulation_scope_violations(bad))

    def test_out_of_scope_surfaces_untouched(self):
        pol, off = _read(POLICY), _read(OFFLOAD)
        self.assertEqual(out_of_scope_violations(pol, off), [])
        bad = pol.replace('    { "backup.history",    "vm_name" },\n',
                          '    { "backup.history",    "vm_name" },\n    { "network.qos.get", NULL },\n', 1)
        self.assertNotEqual(bad, pol)
        self.assertTrue(out_of_scope_violations(bad, off))

    def test_principal_key_no_tenant(self):
        srcs = {"rest_server": _read(REST_SERVER), "gate": _read(GATE_C)}
        self.assertEqual(principal_key_no_tenant_violations(srcs), [])
        g = srcs["gate"]
                                                                  
        m = re.search(r"    if \(api_key\)[^\n]*\n        return NULL;[^\n]*\n", g)
        self.assertIsNotNone(m)
        bad = dict(srcs, gate=g.replace(m.group(0), "", 1))
        self.assertTrue(principal_key_no_tenant_violations(bad))
                                         
        at = g.index("pcv_res_rest_tenant_subject(gint")
        tail = g[at:]
        guard = re.search(r"    if \(role >= PCV_ROLE_ADMIN\)\n        return NULL;[^\n]*\n", tail)
        self.assertIsNotNone(guard)
        bad = dict(srcs, gate=g[:at] + tail.replace(guard.group(0), "", 1))
        self.assertTrue(principal_key_no_tenant_violations(bad))
                                                   
        r = srcs["rest_server"]
        bad = dict(srcs, rest_server=re.sub(r"pcv_rbac_get_tenant\(\s*who\s*\)", "pcv_rbac_get_tenant(rq->subject)", r, count=1))
        self.assertNotEqual(bad["rest_server"], r)
        self.assertTrue(principal_key_no_tenant_violations(bad))

    def _inj_srcs(self) -> dict[str, str]:
        return {"rest_server": _read(REST_SERVER), "rest_request": _read(REST_REQUEST), "grpc": _read(GRPC_SERVER),
                "dispatcher": _read(DISPATCHER), "gate": _read(GATE_C)}

    def test_caller_tenant_injected_for_non_admin(self):
        srcs = self._inj_srcs()
        self.assertEqual(caller_tenant_injection_violations(srcs), [])
                                           
        bad = dict(srcs, rest_request=srcs["rest_request"].replace(
            'json_object_remove_member(params, "_pcv_caller_tenant");', ";", 1))
        self.assertNotEqual(bad["rest_request"], srcs["rest_request"])
        self.assertTrue(caller_tenant_injection_violations(bad))
                                                  
        bad = dict(srcs, rest_server=srcs["rest_server"].replace(
            "rq->subject, caller_role, caller_tenant)", "rq->subject, caller_role, NULL)", 1))
        self.assertNotEqual(bad["rest_server"], srcs["rest_server"])
        self.assertTrue(caller_tenant_injection_violations(bad))
                                                
        bad = dict(srcs, dispatcher=srcs["dispatcher"] + "\nstatic void x(void) { (void)pcv_rbac_get_tenant(\"a\"); }\n")
        self.assertTrue(caller_tenant_injection_violations(bad))
                                                
        g = srcs["gate"]
        anchor = "pcv_res_actor_tenant(JsonObject *params, gint role)\n{\n"
        self.assertIn(anchor, g)
        bad = dict(srcs, gate=g.replace(anchor, anchor + "    (void)pcv_rbac_get_tenant(NULL);\n", 1))
        self.assertTrue(caller_tenant_injection_violations(bad))
                                              
        gr = srcs["grpc"]
        old = '        if (tenant)\n            json_object_set_string_member(params, "_pcv_caller_tenant", tenant);'
        self.assertIn(old, gr)
        bad = dict(srcs, grpc=gr.replace(old, '        if (role >= PCV_ROLE_ADMIN) {}\n        else if (tenant)\n'
                                              '            json_object_set_string_member(params, "_pcv_caller_tenant", tenant);', 1))
        self.assertTrue(caller_tenant_injection_violations(bad))

    def _srcs(self) -> dict[Path, str]:
        return {p: _read(p) for p in TARGETS}

    def test_principal_compare_single_site(self):
        srcs = self._srcs()
        self.assertEqual(principal_compare_violations(srcs), [])
                                                
        scope = srcs[SCOPE_C]
        anchor = "pcv_vm_read_visible(PcvVmReadScope *s, const gchar *owner)\n{\n"
        self.assertIn(anchor, scope)
        bad = {**srcs, SCOPE_C: scope.replace(
            anchor, anchor + "    if (s && g_strcmp0(owner, s->subject) == 0) return TRUE;\n", 1)}
        self.assertTrue(principal_compare_violations(bad))
                                            
        h = srcs[DIAG_HANDLER]
        anchor = "_vm_diag_read_visible(const PcvVmDiagConfig *cfg, gpointer ud)\n{\n"
        self.assertIn(anchor, h)
        bad = {**srcs, DIAG_HANDLER: h.replace(
            anchor, anchor + "    if (cfg && strcmp(cfg->owner, \"x\") == 0) return TRUE;\n", 1)}
        self.assertTrue(principal_compare_violations(bad))
                               
        bad = {**srcs, SCOPE_C: scope.replace(
            anchor_free := "pcv_vm_read_scope_free(PcvVmReadScope *s)\n{\n",
            anchor_free + "    (void)\"SELECT 1 FROM jobs WHERE actor = ?\";\n", 1)}
        self.assertNotEqual(bad[SCOPE_C], scope)
        self.assertTrue(principal_compare_violations(bad))
                                                                
        flt = srcs[READ_FILTER_C]
        star = "_is_star(const gchar *v)\n{\n"
        self.assertIn(star, flt)
        bad = {**srcs, READ_FILTER_C: flt.replace(star, star + "    if (g_strcmp0(v, \"*\") == 0 && g_strcmp0(owner, \"*\")) return TRUE;\n", 1)}
        self.assertTrue(principal_compare_violations(bad))
        bad = {**srcs, READ_FILTER_C: flt.replace(star, star + "    if (g_strcmp0(v, \"admin\") == 0) return TRUE;\n", 1)}
        self.assertTrue(principal_compare_violations(bad))
                                              
        hv = srcs[HOST_DEVICE_VIEW]
        old = "    return pcv_vm_read_visible(caller->vm_scope, vm->owner);\n"
        self.assertIn(old, hv)
        bad = {**srcs, HOST_DEVICE_VIEW: hv.replace(old, "    return g_strcmp0(vm->owner, caller->subject) == 0;\n", 1)}
        self.assertTrue(principal_compare_violations(bad))
                                                          
        blk = next(b for b in c_blocks.scan(scope)[0] if b.name == DECIDE)
        body = scope[blk.body_open:blk.body_close + 1]
        gutted = re.sub(r"\b(?:g_strcmp0|strcmp)\s*\(", "_moved_cmp(", body)
        self.assertNotEqual(gutted, body)
        bad = {**srcs, SCOPE_C: scope[:blk.body_open] + gutted + scope[blk.body_close + 1:]}
        self.assertTrue(principal_compare_violations(bad))


if __name__ == "__main__":
    unittest.main()
