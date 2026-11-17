#!/usr/bin/env python3
                          
                                                                                                                
                                                                                                
                                               
                                                                                                       
                                                                                      
                                                                                                 
                                                                                     
                                                                                                          
                                                                                                            
                                                                                              
                                                                                
                                                                                                           
                                                                                                                 
                                                                                                  
                                                         
                                                                                            
                                                                                                 
                                                                                                                  
                                                                                    
                                                               
 
                                                                       
                                                                           
                          
from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts" / "split"))
import c_blocks              

MAIN = ROOT / "src/main.c"
RUN_TU = ROOT / "src/modules/dispatcher/vm_boot_autostart_run.c"
HANDLER = ROOT / "src/modules/dispatcher/handler_vm_start.c"
VM_STATE = ROOT / "src/modules/core/vm_state.c"
RUN_FN = "pcv_vm_boot_autostart_run"
ORDER = (
    "pcv_bootstrap_init_runtime_network(",
    "pcv_qos_restore(",
    RUN_FN + "(",
    "pcv_drain_notify_ready(",
    "g_main_loop_run(",
)
SCHEDULERS = re.compile(r"\b(g_idle_add\w*|g_timeout_add\w*|pcv_drain_invoke\w*|g_main_context_invoke\w*)\s*\(")


def order_problems(main_src: str) -> list[str]:
                                                        
    m = c_blocks.masked(main_src)
    out, pos = [], []
    for name in ORDER:
        n = m.count(name)
        if n != 1:
            out.append(f"main.c의 {name} 출현이 {n}번이다(1번이어야 한다)")
            pos.append(-1)
        else:
            pos.append(m.find(name))
    if not out:
        for (a, pa), (b, pb) in zip(zip(ORDER, pos), zip(ORDER[1:], pos[1:])):
            if not pa < pb:
                out.append(f"main.c에서 {a}가 {b}보다 뒤다")
    return out


def _balanced_args(m: str, open_paren: int) -> str:
    depth, i = 0, open_paren
    while i < len(m):
        if m[i] == "(":
            depth += 1
        elif m[i] == ")":
            depth -= 1
            if depth == 0:
                return m[open_paren + 1:i]
        i += 1
    return m[open_paren + 1:]


def schedule_problems(sources: dict[str, str]) -> list[str]:
                                                                            
    out = []
    ident = re.compile(r"\b" + RUN_FN + r"\b")
    for path, src in sorted(sources.items()):
        m = c_blocks.masked(src)
        for sm in SCHEDULERS.finditer(m):
            if ident.search(_balanced_args(m, sm.end() - 1)):
                out.append(f"{path}: {RUN_FN}가 {sm.group(1)} 인자로 쓰인다")
        if path.endswith(".h") or path.endswith("vm_boot_autostart_run.c"):
            continue          
        for im in ident.finditer(m):
            tail = m[im.end():im.end() + 8]
            if path != "src/main.c" or not re.match(r"\s*\(\s*\)\s*;", tail):
                out.append(f"{path}: {RUN_FN} 참조가 main.c 직접 호출문이 아니다")
    if not any(ident.search(c_blocks.masked(s)) for p, s in sources.items() if p == "src/main.c"):
        out.append(f"main.c에 {RUN_FN} 호출이 없다")
    return out


def _block_after(m: str, pos: int) -> str:
                                                 
    o = m.find("{", pos)
    if o < 0:
        return ""
    depth = 0
    for i in range(o, len(m)):
        if m[i] == "{":
            depth += 1
        elif m[i] == "}":
            depth -= 1
            if depth == 0:
                return m[o:i + 1]
    return m[o:]


WRITE_GUARD = re.compile(r"if\s*\(\s*!\s*pcv_boot_autostart_marker_write_at\(\s*PCV_BOOT_AUTOSTART_MARKER_PATH\s*,"
                         r"\s*\w+\s*,\s*PCV_BOOT_DECIDE_ACT\s*,")


def oracle_problems(run_src: str) -> list[str]:
                                               
    m = c_blocks.masked(run_src)
    out = []
    lit = re.search(r"PcvBootBridgeOracle\s+\w+\s*=\s*\{(.*?)\};", m, re.S)
    if not lit:
        out.append("연결 TU에 PcvBootBridgeOracle 초기화가 없다")
    else:
        for fn in ("pcv_network_bridge_uplink_mode", "pcv_vpc_bridge_is_managed"):
            if not re.search(r"\b" + fn + r"\b", lit.group(1)):
                out.append(f"오라클에 {fn}이 없다")
    try:
        ie = _fn_body(run_src, "_iface_exists")
    except AssertionError:
        ie = ""
    if "pcv_boot_autostart_iface_exists_at(" not in ie:
        out.append("_iface_exists가 pcv_boot_autostart_iface_exists_at(경로 안전만 보는 존재 검사)을 쓰지 않는다")
    if "pcv_validate_bridge_name(" in m:
        out.append("연결 TU가 pcv_validate_bridge_name으로 브리지 존재를 거른다(br0.100 같은 합법 이름을 없음으로 본다)")
    scan = m.find("pcv_boot_autostart_scan(")
    if scan < 0:
        return out + ["연결 TU가 pcv_boot_autostart_scan을 부르지 않는다"]
    g = WRITE_GUARD.search(m, scan)
    if not g:
        out.append("scan 뒤 act 표지 쓰기 성공을 확인하는 if (!pcv_boot_autostart_marker_write_at(…)) 분기가 없다")
    elif not re.search(r"\breturn\b", _block_after(m, g.end())):
        out.append("act 표지 쓰기 실패 분기가 return으로 끝나지 않는다")
    return out



CONTENDED_SET = re.compile(r"\*\s*contended\s*=\s*TRUE")
ACQUIRE_EX = re.compile(r"pcv_vm_lock_acquire_ex\(\s*vm_id\s*,\s*VM_OP_STARTING\s*,\s*err_msg\s*,\s*lock_busy\s*\)")


def _fn_body(src: str, name: str) -> str:
    m = c_blocks.masked(src)
    for b in c_blocks.scan(src)[0]:
        if b.name == name:
            return m[b.body_open:b.body_close + 1]
    raise AssertionError(f"{name} 없음")


def lock_busy_problems(handler_src: str, vm_state_src: str) -> list[str]:
                                                                                
    out = []
    b = _fn_body(handler_src, "_vm_start_prepare")
    if not ACQUIRE_EX.search(b):
        out.append("_vm_start_prepare가 pcv_vm_lock_acquire_ex(vm_id, VM_OP_STARTING, err_msg, lock_busy)로 잠그지 않는다")
    if re.search(r"\*\s*lock_busy\s*=", b):
        out.append("_vm_start_prepare가 *lock_busy를 직접 세운다(잠금 실패 전체가 경합으로 세어진다)")
    ins = _fn_body(vm_state_src, "_lock_insert")
    n = len(CONTENDED_SET.findall(ins))
    if n != 1:
        return out + [f"_lock_insert의 *contended = TRUE가 {n}번이다(1번이어야 한다)"]
    g = re.search(r"if\s*\(\s*pid_is_alive\(\s*existing_pid\s*\)\s*&&\s*!\s*lock_expired\s*\)\s*\{", ins)
    if not g:
        return out + ["_lock_insert에 살아 있는 소유자 분기가 없다"]
    if not CONTENDED_SET.search(_block_after(ins, g.end() - 1)):
        out.append("*contended = TRUE가 살아 있는 소유자 분기 밖에 있다")
    if len(CONTENDED_SET.findall(c_blocks.masked(vm_state_src))) != 1:
        out.append("vm_state.c의 다른 곳에서도 *contended = TRUE를 세운다")
    return out


def submit_literal_problems(run_src: str) -> list[str]:
                                 
    m = c_blocks.masked(run_src)
    lits = re.findall(r"PcvVmStartRequest\s+\w+\s*=\s*\{(.*?)\};", m, re.S)
    if len(lits) != 1:
        return [f"연결 TU의 PcvVmStartRequest 리터럴이 {len(lits)}개다(1개여야 한다)"]
    want = (r"\.origin\s*=\s*PCV_BOOT_AUTOSTART_ORIGIN\b", r"\.skip_if_active\s*=\s*TRUE\b",
            r"\.actor\s*=\s*PCV_AUDIT_ACTOR_SYSTEM\b", r"\.vcpu_count\s*=\s*0(?![\w.])", r"\.numa_node\s*=\s*0(?![\w.])")
    return [f"제출 리터럴에 {w}가 없다" for w in want if not re.search(w, lits[0])]


def _functions_with(src: str, needle: str) -> list[str]:
    m = c_blocks.masked(src)
    return [m[b.body_open:b.body_close + 1] for b in c_blocks.scan(src)[0] if needle in m[b.body_open:b.body_close + 1]]


def lock_busy_report_problems(run_src: str) -> list[str]:
                                                                                                     
    fns = _functions_with(run_src, "return PCV_BOOT_SUBMIT_LOCK_BUSY")
    if not fns:
        return ["연결 TU에 PCV_BOOT_SUBMIT_LOCK_BUSY 반환이 없다"]
    return ["LOCK_BUSY를 돌려주는 함수가 pcv_vm_start_report_rejected를 부른다"
            for f in fns if "pcv_vm_start_report_rejected(" in f]


def marker_before_queue_problems(run_src: str) -> list[str]:
                                                                                
    m = c_blocks.masked(run_src)
    g = WRITE_GUARD.search(m)
    q = m.find("pcv_boot_autostart_run_queue(")
    if q < 0:
        return ["연결 TU가 pcv_boot_autostart_run_queue를 부르지 않는다"]
    if not g:
        return ["act 표지 쓰기 성공 확인 분기가 없다"]
    out = []
    if not g.start() < q:
        out.append("pcv_boot_autostart_run_queue가 act 표지 쓰기보다 앞이다")
    if not re.search(r"\breturn\b", _block_after(m, g.end())):
        out.append("act 표지 쓰기 실패 분기가 return으로 끝나지 않는다")
    return out


NEW_FILES = (
    "src/modules/virt/vm_boot_autostart.h",
    "src/modules/virt/vm_boot_autostart.c",
    "src/modules/virt/vm_boot_autostart_scan.c",
    "src/modules/dispatcher/vm_boot_autostart_run.h",
    "src/modules/dispatcher/vm_boot_autostart_run.c",
)
FORBIDDEN = re.compile(r"\b(virDomainCreate\w*|virDomainSetAutostart)\s*\(")


def forbidden_call_problems(sources: dict[str, str]) -> list[str]:
                                                                          
    out = []
    for path in NEW_FILES:
        mt = FORBIDDEN.search(c_blocks.masked(sources[path]))
        if mt:
            out.append(f"{path}: {mt.group(1)} 호출")
    return out

SAMPLE = ROOT / "packaging/deb/daemon.conf.sample"
GUIDES = (ROOT / "docs/GUIDE.md", ROOT / "ui/guide-content.md")
CONFIG_LINE = re.compile(r"(?m)^vm_autostart_after_bootstrap\s*=\s*1\s*$")


def _network_section(conf_text: str) -> str:
                                              
    m = re.search(r"(?m)^\[network\]\s*$", conf_text)
    if not m:
        return ""
    nxt = re.search(r"(?m)^\[[A-Za-z0-9_.-]+\]\s*$", conf_text[m.end():])
    return conf_text[m.end():m.end() + nxt.start()] if nxt else conf_text[m.end():]


def _guide_conf_block(md: str) -> str:
                                                               
    for b in re.findall(r"(?ms)^```ini\n(.*?)^```", md):
        if re.search(r"(?m)^\[network\]\s*$", b):
            return b
    return ""


def sample_problems(sample: str, guides: dict[str, str]) -> list[str]:
    out = []
    if not CONFIG_LINE.search(_network_section(sample)):
        out.append("daemon.conf.sample [network]에 vm_autostart_after_bootstrap = 1이 없다")
    for name, md in guides.items():
        if not CONFIG_LINE.search(_network_section(_guide_conf_block(md))):
            out.append(f"{name} daemon.conf 참조 블록 [network]에 vm_autostart_after_bootstrap = 1이 없다")
    return out

def src_sources() -> dict[str, str]:
    return {str(p.relative_to(ROOT)): p.read_text(errors="replace") for p in sorted((ROOT / "src").rglob("*.[ch]"))}


class BootAutostartContract(unittest.TestCase):
    main_src = MAIN.read_text()

    def test_main_call_order(self):
        self.assertEqual(order_problems(self.main_src), [])
                                               
        moved = self.main_src.replace("    " + RUN_FN + "();", "", 1)
        self.assertNotEqual(moved, self.main_src)
        moved = moved.replace("    pcv_qos_restore();", "    " + RUN_FN + "();\n    pcv_qos_restore();", 1)
        self.assertTrue(order_problems(moved))

    def test_run_not_scheduled_by_source(self):
        sources = src_sources()
        self.assertEqual(schedule_problems(sources), [])
                                             
        mutated = self.main_src.replace("    " + RUN_FN + "();",
                                        "    g_idle_add((GSourceFunc)" + RUN_FN + ", NULL);", 1)
        self.assertNotEqual(mutated, self.main_src)
        self.assertTrue(schedule_problems({**sources, "src/main.c": mutated}))


    def test_oracle_wiring(self):
        run_src = RUN_TU.read_text()
        self.assertEqual(oracle_problems(run_src), [])
                                              
        no_phys = run_src.replace("        pcv_network_bridge_uplink_mode,\n", "        NULL,\n", 1)
        self.assertNotEqual(no_phys, run_src)
        self.assertTrue(oracle_problems(no_phys))
        ignored = re.sub(r"if \(!pcv_boot_autostart_marker_write_at\(", "if (0 && !pcv_boot_autostart_marker_write_at(", run_src, count=1)
        self.assertNotEqual(ignored, run_src)
        self.assertTrue(oracle_problems(ignored))
                                                     
        call = "pcv_boot_autostart_iface_exists_at(\"/sys/class/net\", bridge)"
        self.assertIn(call, run_src)
        validated = run_src.replace(call, "pcv_validate_bridge_name(bridge) && " + call, 1)
        self.assertTrue(oracle_problems(validated))


    def test_lock_busy_only_in_lock_branch(self):
        h, v = HANDLER.read_text(), VM_STATE.read_text()
        self.assertEqual(lock_busy_problems(h, v), [])
                                                             
        blanket = h.replace("    if (!op_lock)\n        return PCV_VM_START_REJECTED;\n",
                            "    if (!op_lock) {\n        if (lock_busy) *lock_busy = TRUE;\n        return PCV_VM_START_REJECTED;\n    }\n", 1)
        self.assertNotEqual(blanket, h)
        self.assertTrue(lock_busy_problems(blanket, v))
                                                 
        line = "            if (contended) *contended = TRUE;\n"
        self.assertIn(line, v)
        moved = v.replace(line, "", 1).replace("done:\n", "done:\n    if (!result && contended) *contended = TRUE;\n", 1)
        self.assertTrue(lock_busy_problems(h, moved))
                                   
        self.assertTrue(lock_busy_problems(h.replace("pcv_vm_lock_acquire_ex(vm_id, VM_OP_STARTING, err_msg, lock_busy)",
                                                     "pcv_vm_lock_acquire(vm_id, VM_OP_STARTING, err_msg)", 1), v))

    def test_submit_literal(self):
        run_src = RUN_TU.read_text()
        self.assertEqual(submit_literal_problems(run_src), [])
        for old, new in ((".skip_if_active = TRUE", ".skip_if_active = FALSE"),
                         (".origin = PCV_BOOT_AUTOSTART_ORIGIN", ".origin = \"vm.start\""),
                         (".actor = PCV_AUDIT_ACTOR_SYSTEM", ".actor = NULL"),
                         (".vcpu_count = 0", ".vcpu_count = 1")):
            mutated = run_src.replace(old, new, 1)
            self.assertNotEqual(mutated, run_src, old)
            self.assertTrue(submit_literal_problems(mutated), old)

    def test_lock_busy_does_not_report(self):
        run_src = RUN_TU.read_text()
        self.assertEqual(lock_busy_report_problems(run_src), [])
        ret = "            return PCV_BOOT_SUBMIT_LOCK_BUSY;"
        self.assertIn(ret, run_src)
        mutated = run_src.replace(ret, "            pcv_vm_start_report_rejected(vm, PCV_BOOT_AUTOSTART_ORIGIN, err, "
                                       "PCV_AUDIT_ACTOR_SYSTEM);\n" + ret, 1)
        self.assertTrue(lock_busy_report_problems(mutated))

    def test_marker_before_queue(self):
        run_src = RUN_TU.read_text()
        self.assertEqual(marker_before_queue_problems(run_src), [])
        call = "    pcv_boot_autostart_run_queue(names, n, &ops);\n"
        guard = "    g_autoptr(GError) werr = NULL;\n    if (!pcv_boot_autostart_marker_write_at(PCV_BOOT_AUTOSTART_MARKER_PATH, boot_id, PCV_BOOT_DECIDE_ACT"
        self.assertIn(call, run_src)
        self.assertIn(guard, run_src)
        moved = run_src.replace(call, "", 1).replace(guard, call + guard, 1)
        self.assertTrue(marker_before_queue_problems(moved))
        no_return = run_src.replace("                      (g_get_monotonic_time() - t0) / 1000, NULL);\n        return;\n",
                                    "                      (g_get_monotonic_time() - t0) / 1000, NULL);\n", 1)
        self.assertNotEqual(no_return, run_src)
        self.assertTrue(marker_before_queue_problems(no_return))

    def test_no_create_or_setautostart(self):
        sources = {p: (ROOT / p).read_text() for p in NEW_FILES}
        self.assertEqual(forbidden_call_problems(sources), [])
        path = "src/modules/dispatcher/vm_boot_autostart_run.c"
        for call in ("virDomainCreate(dom);", "virDomainCreateWithFlags(dom, 0);", "virDomainSetAutostart(dom, 1);"):
            mutated = sources[path].replace("        virDomainFree(dom);\n", "        " + call + "\n        virDomainFree(dom);\n", 1)
            self.assertNotEqual(mutated, sources[path])
            self.assertTrue(forbidden_call_problems({**sources, path: mutated}), call)


    def test_sample_config_key(self):
        sample = SAMPLE.read_text()
        guides = {p.name: p.read_text() for p in GUIDES}
        self.assertEqual(sample_problems(sample, guides), [])
                                      
        self.assertTrue(sample_problems(CONFIG_LINE.sub("", sample), guides))
        for name in guides:
            self.assertTrue(sample_problems(sample, {**guides, name: CONFIG_LINE.sub("", guides[name])}), name)


if __name__ == "__main__":
    unittest.main(verbosity=2)
