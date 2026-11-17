#!/usr/bin/env python3
                          
                                                                    
                                                                         
                                              
                                                                                          
                                                                                      
                                                                                                 
                                                                                           
                                                                                          
                                                                                       
                                                                                                                       
                                                                                                  
                                                                                             
                                                                                                 
                                                                                                        
                                                                                           
              
                                                                                                         
                                                                                    
                                                                                         
                                                                                      
                                                                     
                                                      
 
                      
                                                             
                                       
                         
from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts" / "split"))
import c_blocks              

DAEMONS = ROOT / "src/modules/daemons"
SAMPLER = DAEMONS / "vm_diag_sampler.c"
EBPF_C = DAEMONS / "ebpf_telemetry.c"
EBPF_H = DAEMONS / "ebpf_telemetry.h"
PURE = (DAEMONS / "vm_diag.c", DAEMONS / "vm_diag_view.c")
VIEW = DAEMONS / "vm_diag_view.c"
HANDLER = ROOT / "src/modules/dispatcher/handler_monitor_diag.c"
DISPATCHER = ROOT / "src/api/dispatcher.c"
POLICY = ROOT / "src/api/dispatcher_policy.c"
OFFLOAD = ROOT / "src/modules/dispatcher/rpc_offload.c"
REST_MAP = ROOT / "src/api/rest_map_observe.c"
PARAMS = ROOT / "contracts/rpc_params.json"
HELP = ROOT / "ui/modules/help.js"
VM_LIFECYCLE = ROOT / "src/modules/dispatcher/handler_vm_lifecycle.c"
MONITOR = ROOT / "src/modules/dispatcher/handler_monitor.c"
FAMILY_TEST = ROOT / "scripts/tests/test_vm_family_sources.py"

CALL = "virConnectGetAllDomainStats("


def _read(p: Path) -> str:
    return p.read_text(encoding="utf-8")


def _code(src: str) -> str:
                        
    return c_blocks.masked(src)


def _no_comments(src: str) -> str:
                                              
    kinds = c_blocks.classify(src)
    return "".join(ch if k != "m" or ch == "\n" else " " for ch, k in zip(src, kinds))


def _call_args(code: str, start: int) -> str:
                                                       
    depth, i = 1, start
    while i < len(code) and depth:
        depth += {"(": 1, ")": -1}.get(code[i], 0)
        i += 1
    return code[start:i - 1]


def _fn_body(src: str, name: str) -> str:
    code = _code(src)
    for b in c_blocks.scan(src)[0]:
        if b.name == name:
            return code[b.body_open:b.body_close + 1]
    raise AssertionError(f"함수 없음: {name}")


def check_two_calls(src: str) -> list[str]:
    code = _code(src)
    calls = [_call_args(code, m.end()) for m in re.finditer(re.escape(CALL), code)]
    if len(calls) != 2:
        return [f"{CALL} 이 {len(calls)}번이다(2번이어야 한다)"]
    out = []
    running = [a for a in calls if "VIR_CONNECT_GET_ALL_DOMAINS_STATS_RUNNING" in a]
    if len(running) != 1:
        return [f"RUNNING 플래그 호출이 {len(running)}개다(1개 — 호출 ①만)"]
    one = running[0]
    two = calls[1] if calls[0] is one else calls[0]
    for tok in ("VIR_CONNECT_GET_ALL_DOMAINS_STATS_NOWAIT", "VIR_DOMAIN_STATS_VCPU",
                "VIR_DOMAIN_STATS_BLOCK", "VIR_DOMAIN_STATS_INTERFACE", "VIR_DOMAIN_STATS_BALLOON",
                "VIR_DOMAIN_STATS_CPU_TOTAL", "VIR_DOMAIN_STATS_STATE"):
        if tok not in one:
            out.append(f"호출 ①에 {tok} 가 없다")
    if "VIR_CONNECT_GET_ALL_DOMAINS_STATS_NOWAIT" not in two:
        out.append("호출 ②에 NOWAIT 가 없다")
    for tok in ("VIR_CONNECT_GET_ALL_DOMAINS_STATS_RUNNING", "VIR_DOMAIN_STATS_BLOCK"):
        if tok in two:
            out.append(f"호출 ②(전체 도메인)에 {tok} 가 있다 — 정지 VM 이미지를 연다(M5)")
    for tok in ("VIR_DOMAIN_STATS_STATE", "VIR_DOMAIN_STATS_VCPU", "VIR_DOMAIN_STATS_BALLOON"):
        if tok not in two:
            out.append(f"호출 ②에 {tok} 가 없다")
    return out


def check_ebpf_no_vm(c_src: str, h_src: str) -> list[str]:
    out = []
    for label, src in (("ebpf_telemetry.c", c_src), ("ebpf_telemetry.h", h_src)):
        code = _code(src)
        for tok in ("virConnectGetAllDomainStats", "VmExtMetrics", "pcv_ebpf_telemetry_get_vm",
                    "get_all_vms", "_vm_to_json", "EBPF_MAX_VMS"):
            if re.search(r"\b" + re.escape(tok) + r"\b", code) or (tok == "get_all_vms" and tok in code):
                out.append(f"{label}: {tok} 가 남아 있다")
        if "purecvisor_vm_net_" in _no_comments(src):
            out.append(f"{label}: purecvisor_vm_net_ 계열이 남아 있다(표본기 몫)")
    return out


def check_cycle_order(src: str) -> list[str]:
    body = _fn_body(src, "_ebpf_thread")
    out = []
    a = body.find("pcv_vm_diag_sampler_collect(")
    b = body.find("_collect_node_pressure(")
    c = body.find("pcv_vm_diag_sampler_finish(")
    if min(a, b, c) < 0:
        return [f"_ebpf_thread 에 호출이 빠졌다(collect={a}, pressure={b}, finish={c})"]
    for tok in ("pcv_vm_diag_sampler_collect(", "pcv_vm_diag_sampler_finish("):
        if body.count(tok) != 1:
            out.append(f"_ebpf_thread 의 {tok} 출현이 {body.count(tok)}번이다")
    if not (a < b < c):
        out.append("순서가 collect < _collect_node_pressure < finish 가 아니다(P3)")
    if "pcv_cgroup_psi_collect(" in body:
        out.append("_ebpf_thread 에 pcv_cgroup_psi_collect( 가 남아 있다(finish 안으로 옮김)")
    return out


LIBVIRT_CALL = re.compile(r"\bvir[A-Z]\w*\s*\(")


def check_pure(src: str, label: str) -> list[str]:
    return [f"{label}: libvirt 호출 {m.group(0)!r}" for m in LIBVIRT_CALL.finditer(_code(src))]


def check_store_lock(src: str) -> list[str]:
    code = _code(src)
    out = []
    pos, sections = 0, 0
    while True:
        a = code.find("g_mutex_lock(", pos)
        if a < 0:
            break
        b = code.find("g_mutex_unlock(", a)
        if b < 0:
            out.append("g_mutex_lock 뒤 unlock 이 없다")
            break
        sec = code[a:b]
        sections += 1
        for tok in ("vir", "fopen", "g_file_get_contents", "pcv_prom_"):
            if re.search(r"\b" + tok, sec):
                out.append(f"뮤텍스 구간 {sections}에 {tok} 가 있다")
        pos = b + 1
    if sections == 0:
        out.append("저장소 뮤텍스 구간이 없다")
    return out


def check_wiring(srcs: dict[str, str]) -> list[str]:
                                                              
    out = []
    d = _no_comments(srcs["dispatcher"])
    if not re.search(r'g_hash_table_insert\(\s*g_rpc_routes\s*,\s*"monitor\.vm\.diag"\s*,\s*\(gpointer\)handle_monitor_vm_diag\)', d):
        out.append("dispatcher.c: monitor.vm.diag 라우트가 없다")
    if not re.search(r'\{\s*"monitor\.vm\.diag"\s*,\s*0\s*\}', _no_comments(srcs["policy"])):
        out.append("dispatcher_policy.c: monitor.vm.diag 정책 행(VIEWER 0)이 없다")
    if not re.search(r'\{\s*"monitor\.vm\.diag"\s*,\s*PCV_OFFLOAD_LANE_SCOPE_READ\s*\}', _no_comments(srcs["offload"])):
        out.append("rpc_offload.c: monitor.vm.diag 가 SCOPE_READ 레인에 없다(HOST_READ 에서 옮김)")
    rest = _no_comments(srcs["rest"])
    i = rest.find('"vm-diag"')
    seg = rest[i:i + 600] if i >= 0 else ""
    if not ('pcv_rest_build_rpc("monitor.vm.diag"' in seg and 'g_hash_table_lookup(query, "vm")' in seg
            and 'json_object_set_string_member(p, "vm"' in seg):
        out.append("rest_map_observe.c: GET /monitor/vm-diag → monitor.vm.diag(질의 vm 문자열) 배선이 없다")
    import json as _json
    entry = _json.loads(srcs["params"]).get("monitor.vm.diag")
    if entry != {"required": [], "optional": ["vm"]}:
        out.append(f"contracts/rpc_params.json: monitor.vm.diag = {entry!r}")
    h = srcs["help"]
    if not re.search(r"\['monitor\.vm\.diag',\s*0,\s*'monitor diag'", h):
        out.append("help.js: monitor.vm.diag NS 행이 없다")
    if not re.search(r"\{\s*m:\s*'GET',\s*p:\s*'/monitor/vm-diag'", h):
        out.append("help.js: GET /monitor/vm-diag REST 행이 없다")
    return out


def _handler_body(src: str) -> str:
                                            
    nc = _no_comments(src)
    for b in c_blocks.scan(src)[0]:
        if b.name == "handle_monitor_vm_diag":
            return nc[b.body_open:b.body_close + 1]
    raise AssertionError("handle_monitor_vm_diag 없음")


def check_param_checks(src: str) -> list[str]:
    try:
        b = _handler_body(src)
    except AssertionError as e:
        return [str(e)]
    out = []
    hm = b.find('json_object_has_member(params, "vm")')
    init = b.find("pcv_res_caller_init(")
    if hm < 0:
        out.append('json_object_has_member(params, "vm") 판정이 없다(P12)')
    elif init >= 0 and hm > init:
        out.append("매개변수 검사가 호출자 해석보다 뒤다")
    for tok in ("JSON_NODE_HOLDS_VALUE(n)", "json_node_get_value_type(n) != G_TYPE_STRING",
                "VM_DIAG_VM_PARAM_MAX", "_vm_diag_reply_invalid("):
        if tok not in b:
            out.append(f"매개변수 검사에 {tok} 가 없다")
    try:
        if "PURE_RPC_ERR_INVALID_PARAMS" not in _fn_body(src, "_vm_diag_reply_invalid"):
            out.append("_vm_diag_reply_invalid 가 PURE_RPC_ERR_INVALID_PARAMS(−32602)가 아니다")
    except AssertionError as e:
        out.append(str(e))
    if "pcv_rpc_params_get_string(" in _no_comments(src):
        out.append("pcv_rpc_params_get_string( 을 쓴다(null 을 없음으로 받는 getter)")
    return out


def check_scope_wiring(src: str) -> list[str]:
    try:
        b = _code(_handler_body(src))
    except AssertionError as e:
        return [str(e)]
    out = []
    i = b.find("pcv_res_caller_init(")
    j = b.find("pcv_vm_read_scope_new(")
    k = b.find("pcv_vm_diag_view_json(")
    if min(i, j, k) < 0 or not (i < j < k):
        out.append("순서가 pcv_res_caller_init → pcv_vm_read_scope_new → pcv_vm_diag_view_json 이 아니다")
    if not re.search(r"\.all\s*=\s*pcv_vm_read_scope_all\(\s*scope\s*\)", b):
        out.append("ADMIN 분기(.all = pcv_vm_read_scope_all(scope))가 아니다")
    if not re.search(r"\.visible\s*=\s*_vm_diag_read_visible\b", b):
        out.append("술어가 _vm_diag_read_visible 이 아니다")
    try:
        pred = _fn_body(src, "_vm_diag_read_visible")
    except AssertionError as e:
        return out + [str(e)]
    if "pcv_vm_read_visible(" not in pred:
        out.append("술어가 pcv_vm_read_visible( 을 지나지 않는다(판정은 pcv_vm_read_decide 한 곳)")
    whole = _code(src)
    for tok in ("pcv_vm_diag_tenant_match(", "pcv_res_owner_tenant(", "strcmp(", "g_strcmp0("):
        if tok in whole:
            out.append(f"처리기에 {tok} 가 있다(판정·소유자 테넌트 조회는 vm_read_scope.c 몫)")
    return out


def check_vm_metrics(src: str) -> list[str]:
                                                                       
    out = []
    b = _code(src)
    nc = _no_comments(src)
    if re.search(r"/\s*5\b", _fn_body(src, "vm_metrics_worker")) or "info2.memory / 5" in b:
        out.append("vm_metrics_worker 에 / 5(지어낸 20%) 가 남아 있다")
    for tok, want in (("pcv_vm_diag_mem_ladder(", 1), ("virDomainListGetStats(", 1), ("pcv_vm_diag_parse(", 1)):
        if b.count(tok) != want:
            out.append(f"{tok} 출현 {b.count(tok)}(기대 {want})")
    if nc.count('"mem_source"') != 2:                   
        out.append(f'"mem_source" 출현 {nc.count(chr(34) + "mem_source" + chr(34))}(기대 2 — 문자열·null 분기)')
    if '"vda"' in nc:
        out.append('"vda" 고정 디스크가 남아 있다')
    if "strstr(xml, \"<target dev='\")" in nc:
        out.append("XML 첫 <target dev= 추출이 남아 있다")
    return out


def check_monitor_metrics(src: str) -> list[str]:
    out = []
    nc = _no_comments(src)
    b = _fn_body(src, "handle_monitor_metrics")
    if nc.count('"mem_alloc_mb"') != 1:
        out.append('handler_monitor.c "mem_alloc_mb" 출현이 1번이 아니다')
    if "pcv_vm_diag_store_guest_mem(" not in b:
        out.append("monitor.metrics mem_used_mb 가 진단 캐시(pcv_vm_diag_store_guest_mem)에서 오지 않는다")
    body_nc = nc[nc.find("handle_monitor_metrics(JsonObject"):]
    if 'json_object_set_double_member(res_obj, "mem_used_mb", info.memory' in body_nc:
        out.append("mem_used_mb 가 아직 balloon 현재 할당(info.memory)이다")
    return out


def check_rest_metrics_query(src: str) -> list[str]:
    rest = _no_comments(src)
    i = rest.find('g_strcmp0(name, "metrics") == 0')
    seg = rest[i:i + 500] if i >= 0 else ""
    if not ('g_hash_table_lookup(query, "vm_id")' in seg and 'json_object_set_string_member(p, "vm_id"' in seg
            and 'pcv_rest_build_rpc("monitor.metrics", p)' in seg):
        return ["rest_map_observe.c: GET /monitor/metrics 가 질의 vm_id 를 넘기지 않는다(D17 — 늘 −32602)"]
    return []



                                                                        
GUIDE_PAIR = ("docs/GUIDE.md", "ui/guide-content.md")
GUIDE_HINT_HEADING = "#### VM 진단 신호(잠정 기준)"
HINT_ROW = re.compile(r'\{\s*PCV_VM_DIAG_H_\w+,\s*"(\w+)",\s*([^,]+),\s*([^,]+),')


def hint_rules(vm_diag_c: str, vm_diag_h: str) -> dict[str, tuple[float, float]]:
                                                                                              
    defs = dict(re.findall(r"#define\s+(PCV_VM_DIAG_\w+)\s+([0-9.]+)", vm_diag_h))
    def val(tok: str) -> float:
        tok = tok.strip().replace("(gdouble)", "")
        if tok == "NAN":
            return float("nan")
        return float(defs.get(tok, tok))
    return {m.group(1): (val(m.group(2)), val(m.group(3))) for m in HINT_ROW.finditer(vm_diag_c)}


def guide_hint_table(text: str) -> dict[str, tuple[float, float]]:
                                                                                                                            
    out = {}
    head = text.find(GUIDE_HINT_HEADING)
    if head < 0:
        return out
    nxt = re.search(r"^#{2,4} ", text[head + len(GUIDE_HINT_HEADING):], re.M)
    sect = text[head: head + len(GUIDE_HINT_HEADING) + (nxt.start() if nxt else len(text))]
    for m in re.finditer(r"^\| `(\w+)` \| ([^|]+) \| ([^|]+) \|", sect, re.M):
        def num(cell: str) -> float:
            n = re.search(r"[≥>]\s*([0-9]+(?:\.[0-9]+)?)", cell)
            return float(n.group(1)) if n else float("nan")
        out[m.group(1)] = (num(m.group(2)), num(m.group(3)))
    return out


def _same(a: float, b: float) -> bool:
    return (a != a and b != b) or a == b


def guide_hint_mismatches(rules: dict, guides: dict[str, str]) -> list[str]:
    bad = []
    for rel, text in guides.items():
        table = guide_hint_table(text)
        for code, (notice, warn) in rules.items():
            if code not in table:
                bad.append(f"{rel}: 신호 표에 `{code}` 행이 없다")
            elif not (_same(table[code][0], notice) and _same(table[code][1], warn)):
                bad.append(f"{rel}: `{code}` 기준 {table[code]} != vm_diag.c {(notice, warn)}")
    return bad



                                                                                        
LAST_OK_DOCS = ("docs/GUIDE.md", "ui/guide-content.md")
LAST_OK_CODE = "if (!s->busy) st->last_ok_at_ms = s->wall_ms;"


def last_ok_doc_mismatches(docs: dict[str, str]) -> list[str]:
                                                                                         
    bad = []
    for rel, text in docs.items():
        defs = [m.group(0) for m in re.finditer(r"`last_ok_at_ms`(?:는| =)[^|\n]*", text)]
        if not defs:
            bad.append(f"{rel}: `last_ok_at_ms` 정의 구절이 없다")
        for d in defs:
            if "모든 묶음이 있던" in d or "busy가 아니던" not in d:
                bad.append(f"{rel}: 코드와 다른 정의: {d[:120]}")
    return bad



                                                                                                     
NOSTATS_DOCS = ("docs/GUIDE.md", "ui/guide-content.md")


def nostats_doc_mismatches(docs: dict[str, str]) -> list[str]:
                                                                                                           
    bad = []
    for rel, text in docs.items():
        for line in text.splitlines():
            if "`partial`+`guest_memory`" in line and "통계 주기 없음" in line:
                bad.append(f"{rel}: 주기 없음 → partial 이라고 적은 줄: {line[:120]}")
        if not any("`partial`이 아니라 `ok`이고" in l and "guest_stats_stale" in l for l in text.splitlines()):
            bad.append(f"{rel}: 주기 없는 VM 은 ok + guest_stats_stale 이라는 정정 줄이 없다")
    return bad



                                                                                                              
CPU_WAIT_DOCS = ("docs/GUIDE.md", "ui/guide-content.md")
CPU_WAIT_CODE = "_hint_eval(st, rec, PCV_VM_DIAG_H_CPU_WAIT, o, wait, wait, o);"


def cpu_wait_doc_mismatches(docs: dict[str, str]) -> list[str]:
                                                              
    bad = []
    for rel, text in docs.items():
        rows = [l for l in text.splitlines() if l.startswith("| `cpu_wait` |")]
        if not rows:
            bad.append(f"{rel}: 신호 표에 `cpu_wait` 행이 없다")
        for row in rows:
            if "상한 대기분을 뺀 값으로 판정" not in row:
                bad.append(f"{rel}: `cpu_wait` 행이 상한 대기분 빼기를 적지 않는다: {row[:120]}")
    return bad


class VmDiagContract(unittest.TestCase):
    def test_route_policy_lane_rest(self):
        srcs = {"dispatcher": _read(DISPATCHER), "policy": _read(POLICY), "offload": _read(OFFLOAD),
                "rest": _read(REST_MAP), "params": _read(PARAMS), "help": _read(HELP)}
        self.assertEqual(check_wiring(srcs), [])
        bad = dict(srcs, offload=srcs["offload"].replace('{ "monitor.vm.diag", PCV_OFFLOAD_LANE_SCOPE_READ },', "", 1))
        self.assertNotEqual(bad["offload"], srcs["offload"])
        self.assertTrue(check_wiring(bad))
                                       
        bad = dict(srcs, offload=srcs["offload"].replace('{ "monitor.vm.diag", PCV_OFFLOAD_LANE_SCOPE_READ },',
                                                         '{ "monitor.vm.diag", PCV_OFFLOAD_LANE_HOST_READ },', 1))
        self.assertTrue(check_wiring(bad))
        bad = dict(srcs, policy=srcs["policy"].replace('{ "monitor.vm.diag",           0 },', "", 1))
        self.assertTrue(check_wiring(bad))

    def test_param_checks(self):
        src = _read(HANDLER)
        self.assertEqual(check_param_checks(src), [])
                                                  
        bad = src.replace('if (params && json_object_has_member(params, "vm")) {',
                          'vm = pcv_rpc_params_get_string(params, "vm");\n    if (FALSE) {', 1)
        self.assertNotEqual(bad, src)
        self.assertTrue(check_param_checks(bad))
        bad = src.replace("json_node_get_value_type(n) != G_TYPE_STRING", "FALSE", 1)
        self.assertTrue(check_param_checks(bad))

    def test_handler_no_libvirt(self):
        src = _read(HANDLER)
        self.assertEqual(check_pure(src, HANDLER.name), [])
        self.assertTrue(check_pure(src + "\nstatic void x(void) { virConnectOpen(0); }\n", HANDLER.name))

    def test_vm_metrics_no_invented_value(self):
        src = _read(VM_LIFECYCLE)
        self.assertEqual(check_vm_metrics(src), [])
        bad = src.replace("guint64 mem_used = 0;", "guint64 mem_used = info2.memory / 5;", 1)
        self.assertNotEqual(bad, src)
        self.assertTrue(check_vm_metrics(bad))

    def test_vm_metrics_all_devices(self):
        src = _read(VM_LIFECYCLE)
        self.assertEqual(check_vm_metrics(src), [])
        bad = src + '\nstatic void x(void *d) { virDomainBlockStats(d, "vda", 0, 0); }\n'
        self.assertTrue(check_vm_metrics(bad))

    def test_monitor_metrics_fields(self):
        src = _read(MONITOR)
        self.assertEqual(check_monitor_metrics(src), [])
        bad = src.replace('json_object_set_double_member(res_obj, "mem_used_mb", guest_used_mb);',
                          'json_object_set_double_member(res_obj, "mem_used_mb", info.memory / 1024.0);', 1)
        self.assertNotEqual(bad, src)
        self.assertTrue(check_monitor_metrics(bad))

    def test_rest_monitor_metrics_query(self):
        src = _read(REST_MAP)
        self.assertEqual(check_rest_metrics_query(src), [])
        bad = src.replace('const gchar *vm_id = query ? g_hash_table_lookup(query, "vm_id") : NULL;',
                          "const gchar *vm_id = NULL;", 1)
        self.assertNotEqual(bad, src)
        self.assertTrue(check_rest_metrics_query(bad))

    def test_vm_lifecycle_family_floor(self):
                                                                                     
        import subprocess
        r = subprocess.run([sys.executable, str(FAMILY_TEST)], capture_output=True, text=True, cwd=ROOT)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_scope_wiring(self):
        src = _read(HANDLER)
        self.assertEqual(check_scope_wiring(src), [])
        bad = src.replace(".all = pcv_vm_read_scope_all(scope),", ".all = TRUE,", 1)
        self.assertNotEqual(bad, src)
        self.assertTrue(check_scope_wiring(bad))
                                                                       
        bad = src.replace("return cfg && pcv_vm_read_visible(ud, cfg->owner);",
                          "const gchar *t = cfg ? pcv_res_owner_tenant(cfg->owner) : NULL;\n"
                          "    return t && t[0] && strcmp(t, \"ta\") == 0;", 1)
        self.assertNotEqual(bad, src)
        self.assertTrue(check_scope_wiring(bad))
        bad = src.replace("return cfg && pcv_vm_read_visible(ud, cfg->owner);",
                          "return cfg && pcv_vm_diag_tenant_match(cfg->owner, \"ta\");", 1)
        self.assertNotEqual(bad, src)
        self.assertTrue(check_scope_wiring(bad))

    def test_two_calls_flags(self):
        src = _read(SAMPLER)
        self.assertEqual(check_two_calls(src), [])
                                    
        bad = src.replace("VIR_DOMAIN_STATS_STATE | VIR_DOMAIN_STATS_VCPU | VIR_DOMAIN_STATS_BALLOON,",
                          "VIR_DOMAIN_STATS_STATE | VIR_DOMAIN_STATS_VCPU | VIR_DOMAIN_STATS_BALLOON | "
                          "VIR_DOMAIN_STATS_BLOCK,", 1)
        self.assertNotEqual(bad, src)
        self.assertTrue(check_two_calls(bad))
                                 
        bad = src.replace(" | VIR_CONNECT_GET_ALL_DOMAINS_STATS_NOWAIT);", ");", 1)
        self.assertNotEqual(bad, src)
        self.assertTrue(check_two_calls(bad))

    def test_ebpf_has_no_vm_collection(self):
        c_src, h_src = _read(EBPF_C), _read(EBPF_H)
        self.assertEqual(check_ebpf_no_vm(c_src, h_src), [])
        bad = c_src + '\nstatic void x(void) { pcv_prom_gauge_set_labels("purecvisor_vm_net_rx_bytes_total", "", 0); }\n'
        self.assertTrue(check_ebpf_no_vm(bad, h_src))
        bad_h = h_src + "\nJsonArray *pcv_ebpf_telemetry_get_all_vms(void);\n"
        self.assertTrue(check_ebpf_no_vm(c_src, bad_h))

    def test_cycle_order(self):
        src = _read(EBPF_C)
        self.assertEqual(check_cycle_order(src), [])
                                                           
        moved = src.replace("        pcv_vm_diag_sampler_finish();\n", "", 1).replace(
            "        pcv_vm_diag_sampler_collect();\n",
            "        pcv_vm_diag_sampler_collect();\n        pcv_vm_diag_sampler_finish();\n", 1)
        self.assertNotEqual(moved, src)
        self.assertTrue(check_cycle_order(moved))
                                         
        dup = src.replace("        pcv_vm_diag_sampler_finish();\n",
                          "        pcv_cgroup_psi_collect(NULL, NULL);\n        pcv_vm_diag_sampler_finish();\n", 1)
        self.assertTrue(check_cycle_order(dup))

    def test_pure_tus_no_libvirt_calls(self):
        for p in PURE:
            src = _read(p)
            self.assertEqual(check_pure(src, p.name), [])
            self.assertTrue(check_pure(src + "\nstatic void x(void *d) { virDomainFree(d); }\n", p.name))
                                                  
        self.assertEqual(check_pure("static int f(const virTypedParameter *p) { return p != 0; }", "x"), [])

    def test_store_lock_holds_no_io(self):
        src = _read(VIEW)
        self.assertEqual(check_store_lock(src), [])
        bad = src.replace("    G_store = take;\n",
                          "    G_store = take;\n    pcv_prom_gauge_set_labels(\"x\", \"\", 0);\n", 1)
        self.assertNotEqual(bad, src)
        self.assertTrue(check_store_lock(bad))

    def test_guide_hint_table_matches_constants(self):
                                                                                                    
        rules = hint_rules(_read(ROOT / "src/modules/daemons/vm_diag.c"), _read(ROOT / "src/modules/daemons/vm_diag.h"))
        self.assertEqual(len(rules), 7)
        self.assertEqual(rules["cpu_wait"], (5.0, 20.0))
        self.assertEqual(rules["guest_stats_stale"][0], 30.0)
        guides = {rel: _read(ROOT / rel) for rel in GUIDE_PAIR}
        self.assertEqual(guide_hint_mismatches(rules, guides), [])
        bad = dict(guides)
        row = "| `cpu_wait` | `vcpu_delay_pct` − `throttled_pct`(상한 대기분을 뺀 값으로 판정) ≥ "                       
        bad["docs/GUIDE.md"] = bad["docs/GUIDE.md"].replace(row + "5 |", row + "7 |")
        self.assertNotEqual(bad, guides, "반사실 치환이 실제로 일어나야 한다")
        self.assertTrue(guide_hint_mismatches(rules, bad), "메모리 반사실: GUIDE 숫자를 바꾸면 RED")

    def test_last_ok_definition_matches_code(self):
                                                                                                     
        self.assertIn(LAST_OK_CODE, _no_comments(_read(ROOT / "src/modules/daemons/vm_diag.c")))
        docs = {rel: _read(ROOT / rel) for rel in LAST_OK_DOCS}
        self.assertEqual(last_ok_doc_mismatches(docs), [])
        bad = dict(docs)
        bad["docs/GUIDE.md"] = bad["docs/GUIDE.md"].replace("busy가 아니던", "모든 묶음이 있던", 1)
        self.assertTrue(last_ok_doc_mismatches(bad), "메모리 반사실: 옛 정의로 되돌리면 RED")

    def test_nostats_docs_match_l3(self):
                                                                                                                   
        docs = {rel: _read(ROOT / rel) for rel in NOSTATS_DOCS}
        self.assertEqual(nostats_doc_mismatches(docs), [])
        bad = dict(docs)
        bad["ui/guide-content.md"] = bad["ui/guide-content.md"].replace("(드라이버 없음·`last-update` 0·없음)", "(balloon 통계 주기 없음·드라이버 없음·`last-update` 0)", 1)
        self.assertNotEqual(bad, docs)
        self.assertTrue(nostats_doc_mismatches(bad), "메모리 반사실: 옛 문장으로 되돌리면 RED")

    def test_cpu_wait_residual_docs(self):
                                                                                              
        self.assertIn(CPU_WAIT_CODE, _no_comments(_read(ROOT / "src/modules/daemons/vm_diag.c")))
        docs = {rel: _read(ROOT / rel) for rel in CPU_WAIT_DOCS}
        self.assertEqual(cpu_wait_doc_mismatches(docs), [])
        bad = dict(docs)
        bad["docs/GUIDE.md"] = bad["docs/GUIDE.md"].replace("(상한 대기분을 뺀 값으로 판정)", "", 1)
        self.assertNotEqual(bad, docs)
        self.assertTrue(cpu_wait_doc_mismatches(bad), "메모리 반사실: 표 칸에서 판정 설명을 지우면 RED")


if __name__ == "__main__":
    unittest.main(verbosity=1)
