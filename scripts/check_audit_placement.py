#!/usr/bin/env python3
                          
                                                                                
                                                                                      
                                                                                                                            
 
                      
                                                                                                                                         
   
                                            

    
                                                    
                                        
                                                                   
             
                                        

       
                                                            
                                                                

                
                                                
                                       
                                                                                   
               
                                 

    
                                             
                                                     
                                

       
                                             
               
          
   

from __future__ import annotations
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rpc_extract

REPO_ROOT = Path(__file__).resolve().parent.parent
SEARCH_DIRS = [
    REPO_ROOT / "src" / "api",
    REPO_ROOT / "src" / "modules" / "dispatcher",
    REPO_ROOT / "src" / "modules" / "virt",
    REPO_ROOT / "src" / "modules" / "network",
]

REQUIRED_FIRE_AND_FORGET_METHODS = [
    "backup.restore",
    "backup.replicate",
    "container.create",
    "container.start",
    "container.stop",
    "container.clone",
    "container.destroy",
    "vm.disk.live_resize",
    "vm.resize_disk",
    "vm.clone",
    "vm.export.ova",
    "vm.import.ova",
    "vm.delete",
]

                                                                             
                                                                              
                                                                            
                                                

                          
ASYNC_REG_BLOCK = re.compile(
    r'_async_method_names\[\]\s*=\s*\{(.*?)\};',
    re.DOTALL,
)
METHOD_LITERAL = re.compile(r'"([a-z][a-z0-9_.]+)"')

                                                                                             
AUDIT_CALL_RE = re.compile(
    r'pcv_audit_log(?:_rpc)?\s*\(\s*(?:[^;"()]|\([^;()]*\)|"(?:\\.|[^"\\])*")*?"([a-z][a-z0-9_.]+)"',
    re.DOTALL,
)
                                                                 
                                                               
                                                                        
                                                                   
                                                           
               
WS_COMPLETE_RE = re.compile(
    r'pcv_ws_broadcast_job_complete(?:_tracked)?(?:_mt)?\s*\(\s*[^;]*?"([a-z][a-z0-9_.]+)"',
    re.DOTALL,
)

                                               
                                                  
DYNAMIC_AUDIT_METHODS = {
    "dpdk.bridge.create",
    "dpdk.bridge.delete",
    "container.list",
    "network.create",
    "network.delete",
    "network.list",
    "security.action.approve",
    "storage.pool.list",
    "storage.zvol.create",
    "storage.zvol.delete",
    "storage.zvol.list",
    "suricata.ips.disable",
    "suricata.ips.drop.add",
    "suricata.ips.drop.remove",
    "suricata.ips.enable",
    "suricata.rules.update",
    "vm.limit",
    "vm.pause",
    "vm.resume",
    "vm.set_memory",
    "vm.set_vcpu",
    "vm.snapshot.create",
    "vm.snapshot.delete",
    "vm.snapshot.list",
    "vm.stop",
    "vpc.attachment.create",
    "vpc.attachment.delete",
    "vpc.create",
    "vpc.delete",
    "vpc.egress.set",
    "vpc.reconcile",
    "vpc.service.publish",
    "vpc.service.unpublish",
    "vpc.subnet.create",
    "vpc.subnet.delete",
}


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
    found.update(DYNAMIC_AUDIT_METHODS)
    return found


def collect_durable_completion_methods() -> set[str]:

    ws = (REPO_ROOT / "src/api/ws_server.c").read_text()
    recovery = (REPO_ROOT / "src/utils/pcv_job_recovery.c").read_text()
    if (not re.search(r'pcv_job_finish_async\(ref, status, result, error, _job_result_ready', ws)
            or not re.search(r'_broadcast_job_complete\(pcv_job_id\(ref\), pcv_job_method\(ref\)', ws)
            or 'pcv_drain_invoke(NULL, _deliver, copy)' not in recovery):
        return set()

    def body(text: str, name: str) -> str:
        masked = re.sub(r"/\*.*?\*/|//[^\n]*|'(?:\\.|[^'\\])*'|\"(?:\\.|[^\"\\])*\"",
                        lambda m: ' ' * len(m[0]), text, flags=re.S)
        start = re.search(r'\b' + re.escape(name) + r'\s*\([^;{}]*\)\s*\{', masked)
        if not start:
            return ''
        depth = 1
        for end in range(start.end(), len(masked)):
            depth += (masked[end] == '{') - (masked[end] == '}')
            if not depth:
                return text[start.end():end]
        return ''
    families = {
        "<dispatcher-family>": {
            "_on_vm_create_finished": ["vm.create"],
            "_ova_export_record_result": ["vm.export.ova"],
            "_ova_import_worker": ["vm.import.ova"],
        },
        "src/modules/dispatcher/handler_backup.c": {

            "_restore_finish": ["backup.restore"],
            "_incremental_worker": ["backup.incremental"],
            "_replicate_worker": ["backup.replicate"],
        },
        "src/modules/dispatcher/handler_accel.c": {
            "_dpdk_bridge_worker": ["dpdk.bridge.create", "dpdk.bridge.delete"],
        },
        "<vm-lifecycle-family>": {
            "_vm_delete_callback": ["vm.delete"],
        },
        "src/modules/dispatcher/handler_vpc.c": {
            "_vpc_worker": ["vpc.create", "vpc.delete", "vpc.egress.set", "vpc.subnet.create",
                            "vpc.subnet.delete", "vpc.attachment.create", "vpc.attachment.delete",
                            "vpc.service.publish", "vpc.service.unpublish", "vpc.reconcile"],
        },
    }
    methods = set()
    for path, workers in families.items():
        text = (rpc_extract.dispatcher_family_text(REPO_ROOT) if path == "<dispatcher-family>"
                else rpc_extract.vm_family_text("vm_lifecycle", REPO_ROOT) if path == "<vm-lifecycle-family>"
                else (REPO_ROOT / path).read_text())
        for worker, names in workers.items():
            contents = body(text, worker)
            if re.search(r'pcv_ws_finish_job\((?:ctx|d|data)->job_ref', contents):
                methods.update(names)
    return methods

LITERAL_NULL_ACTOR_RE = re.compile(r'\bpcv_audit_log\s*\(\s*(?:NULL|nullptr|0)\s*,')

def literal_null_actor_sites(root: Path | None = None) -> list[str]:

    root = root or REPO_ROOT
    out = []
    for c in sorted((root / "src").rglob("*.c")):
        text = re.sub(r"/\*.*?\*/|//[^\n]*", lambda m: re.sub(r"[^\n]", " ", m[0]),
                      c.read_text(errors="replace"), flags=re.S)
        for m in LITERAL_NULL_ACTOR_RE.finditer(text):
            out.append(f"{c.relative_to(root)}:{text.count(chr(10), 0, m.start()) + 1}")
    return out

LATE_RESPONSE_CALLBACKS = {
    "src/api/uds_server.c": {"on_read_done": []},

    "src/modules/dispatcher/rpc_offload.c": {"_offload_done": []},
    "src/modules/dispatcher/handler_backup.c": {"_snapshot_verify_done": ["backup.snapshot.verify"]},
    "src/modules/dispatcher/handler_jobs.c": {"_dlq_retry_done": ["webhook.dlq.retry"]},

    "src/modules/dispatcher/handler_storage.c": {"_zvol_op_done": ["storage.zvol.create", "storage.zvol.delete"]},
    "src/modules/dispatcher/handler_container.c": {
        "_on_exec_done": ["container.exec"],
        "_on_snap_create_done": ["container.snapshot.create"],
        "_on_snap_rollback_done": ["container.snapshot.rollback"],
        "_on_snap_delete_done": ["container.snapshot.delete"],
        "_on_snap_list_done": ["container.snapshot.list"],
    },

    "src/modules/dispatcher/handler_container_net.c": {
        "_cnet_done": ["container.nic.list", "container.nic.attach",
                       "container.nic.detach", "container.set_bandwidth"],
    },
    "src/modules/dispatcher/handler_snapshot.c": {
        "_on_rollback_done": ["vm.snapshot.rollback"],
        "_on_libvirt_rollback_done": ["vm.snapshot.rollback"],
        "_snap_sync_callback": ["vm.snapshot.create", "vm.snapshot.list", "vm.snapshot.delete"],
        "_delete_all_callback": ["vm.snapshot.delete_all"],
    },
    "src/modules/dispatcher/handler_vm_hotplug.c": {"hotplug_callback": ["vm.set_memory", "vm.set_vcpu"]},
    "src/modules/dispatcher/handler_vm_inspect.c": {"inspect_complete": ["vm.inspect"]},
    "src/modules/dispatcher/handler_vm_lifecycle.c": {
        "vm_list_callback": ["vm.list"],
        "vm_action_callback": ["vm.stop", "vm.pause", "vm.resume", "vm.limit"],
        "vm_metrics_callback": ["vm.metrics"],
    },
    "src/modules/dispatcher/handler_vm_guest.c": {
        "_guest_fsinfo_callback": ["vm.guest.fsinfo"],
        "_guest_ping_callback": ["vm.guest.ping"],
        "_guest_exec_callback": ["vm.guest.exec"],
        "_guest_shutdown_callback": ["vm.guest.shutdown"],
    },
    "src/modules/dispatcher/handler_replication_hostkey.c": {"_hk_done": ["backup.replication.hostkey.scan", "backup.replication.hostkey.trust"]},
    "src/modules/network/network_manager.c": {"network_action_callback": ["network.create", "network.delete"]},

    "src/modules/dispatcher/poll_async.c": {"_poll_async_done": ["storage.pool.list", "storage.zvol.list",
                                                                 "container.list", "network.list"]},
}

_FN_DEF_RE = re.compile(r'^(?:static\s+)?(?:[A-Za-z_][\w\s\*]*?)\b([A-Za-z_]\w*)\s*\(([^;{}]*)\)\s*\{', re.M)
_CALLBACK_PARAMS_RE = re.compile(r'^\s*GObject\s*\*[^,]*,\s*GAsyncResult\s*\*[^,]*,\s*gpointer\b[^,]*$', re.S)

def _mask_code(text: str) -> str:

    return re.sub(r"/\*.*?\*/|//[^\n]*|'(?:\\.|[^'\\])*'|\"(?:\\.|[^\"\\])*\"",
                  lambda m: re.sub(r"[^\n]", " ", m[0]), text, flags=re.S)

def late_response_callbacks() -> dict[str, set[str]]:

    found: dict[str, set[str]] = {}
    for d in SEARCH_DIRS:
        for c in sorted(d.glob("*.c")) if d.exists() else []:
            m = _mask_code(c.read_text(errors="replace"))
            funcs = {}
            for s in _FN_DEF_RE.finditer(m):
                depth, i = 1, s.end()
                while depth and i < len(m):
                    depth += (m[i] == "{") - (m[i] == "}")
                    i += 1
                funcs[s.group(1)] = (s.group(2), m[s.end():i])
            direct = {n for n, (_, b) in funcs.items() if "pure_uds_server_send_response" in b}
            for n, (params, b) in funcs.items():
                if not _CALLBACK_PARAMS_RE.match(params):
                    continue
                if n in direct or any(h != n and re.search(r"\b" + re.escape(h) + r"\s*\(", b) for h in direct):
                    found.setdefault(str(c.relative_to(REPO_ROOT)), set()).add(n)
    return found

def check_late_response_table(async_methods) -> list[str]:

    found = late_response_callbacks()
    errors = []
    for path, fns in sorted(found.items()):
        for fn in sorted(fns):
            if fn not in LATE_RESPONSE_CALLBACKS.get(path, {}):
                errors.append(f"표에 없는 나중 응답 콜백: {path}::{fn} — LATE_RESPONSE_CALLBACKS에 메서드와 함께 더하고 비동기 등록한다")
    for path, table in sorted(LATE_RESPONSE_CALLBACKS.items()):
        for fn, methods in sorted(table.items()):
            if fn not in found.get(path, set()):
                errors.append(f"표가 낡았다: {path}::{fn}이 없거나 응답하지 않는다")
            for method in methods:
                if method not in async_methods:
                    errors.append(f"나중 응답 메서드 {method}({path}::{fn})가 _async_method_names[]에 없다")
    return errors

def main() -> int:
    dispatcher_text = rpc_extract.dispatcher_route_file(REPO_ROOT).read_text(errors="replace")
    async_methods = extract_async_methods(dispatcher_text)
    if not async_methods:
        print("FAIL: g_async_methods 를 추출하지 못함 — 라우트 원천을 못 읽었거나 집합이 사라졌다",
              file=sys.stderr)
        return 1

    audit_methods = collect_audit_methods()
    ws_methods = collect_methods(WS_COMPLETE_RE) | collect_durable_completion_methods()
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

    late_errors = check_late_response_table(async_methods)
    if late_errors:
        print()
        print("\033[31m[FAIL]\033[0m 콜백에서 나중에 응답하는 RPC 계약 위반")
        for e in late_errors:
            print(f"  - {e}")
        return 1

    null_sites = literal_null_actor_sites()
    if null_sites:
        print()
        print("\033[31m[FAIL]\033[0m 리터럴 NULL 감사 행위자 — 범위·task 행위자 또는 PCV_AUDIT_ACTOR_* 를 쓴다")
        for s in null_sites:
            print(f"  - {s}")
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
