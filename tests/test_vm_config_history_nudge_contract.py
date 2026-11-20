#!/usr/bin/env python3

from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
NUDGE = ROOT / "src/modules/daemons/vm_config_history_nudge.c"
DISPATCHER = ROOT / "src/api/dispatcher.c"
AUDIT = ROOT / "src/modules/audit/pcv_audit.c"
SOURCE = ROOT / "src/modules/daemons/vm_config_history_source.c"
OFFLOAD = ROOT / "src/modules/dispatcher/rpc_offload.c"
VIRT_EVENTS = ROOT / "src/modules/daemons/virt_events.c"
STORE = ROOT / "src/modules/core/vm_config_history_store.c"
PURE = [ROOT / f"src/modules/virt/vm_config_history_{n}.c" for n in ("norm", "decide", "view")]
NEW_TUS = sorted((ROOT / "src").rglob("*config_history*.[ch]"))
ASYNC7 = {"vm.create", "vm.delete", "vm.limit", "vm.clone", "vm.import.ova", "vm.resize_disk", "vm.disk.live_resize"}
ROUTE_RE = re.compile(r'g_hash_table_insert\(\s*g_rpc_routes\s*,\s*"([a-z0-9_.]+)"')

def strip_comments(text: str) -> str:

    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)

def table(text: str, name: str) -> list[tuple[str, ...]]:

    m = re.search(r"\b" + re.escape(name) + r"\[\]\s*=\s*\{(.*?)\n\};", text, re.S)
    if not m:
        raise AssertionError(f"표 {name} 없음")
    rows = []
    for item in re.findall(r"\{([^{}]*)\}", m.group(1)):
        rows.append(tuple(re.findall(r'"([^"]*)"|\bNULL\b', item)))
    return rows

def in_scope(method: str) -> bool:

    return (method.startswith("vm.") or re.match(r"device\.(disk|nic|gpu)\.", method) is not None
            or method.startswith("sriov.") or method.startswith("qos.vm.")
            or (method.startswith("tenant_overlay.") and method.endswith("_vm")))

def check_tables(nudge_text: str, routes: set[str]) -> list[str]:

    errs = []
    nudge = [r[0] for r in table(nudge_text, "k_nudge")]
    exempt_rows = table(nudge_text, "k_exempt")
    exempt = [r[0] for r in exempt_rows]
    for r in exempt_rows:
        if len(r) < 2 or not r[1].strip():
            errs.append(f"면제 사유 없음: {r[0]}")
    for dup in {m for m in nudge + exempt if (nudge + exempt).count(m) > 1}:
        errs.append(f"두 번 이상 등장: {dup}")
    for m in sorted(r for r in routes if in_scope(r)):
        if (m in nudge) == (m in exempt):
            errs.append(f"넛지·면제 중 정확히 하나가 아님: {m}")
    for m in sorted(set(nudge + exempt) - routes):
        errs.append(f"라우트에 없는 표 항목: {m}")
    for m in sorted(ASYNC7 - set(nudge)):
        errs.append(f"비동기 넛지 누락: {m}")
    return errs

def routes_of(text: str) -> set[str]:

    return set(ROUTE_RE.findall(strip_comments(text))) | {"vm.create"}

def body(text: str, fname: str) -> str:

    m = re.search(r"^[\w \t*]*\b" + re.escape(fname) + r"\s*\([^;{]*\)\s*\{", text, re.M)
    if not m:
        raise AssertionError(f"함수 {fname} 없음")
    i, depth = m.end(), 1
    while depth and i < len(text):
        depth += (text[i] == "{") - (text[i] == "}")
        i += 1
    return text[m.end():i]

def check_authorized_order(dispatcher_text: str, source_text: str, offload_text: str | None = None) -> list[str]:

    bad = []
    d = strip_comments(dispatcher_text)
    flow = body(d, "purecvisor_dispatcher_dispatch")
    role = flow.find("_dispatch_role_check(")
    owner = flow.find("if (!_dispatch_owner_check(method, params, rpc_id_str")
    run = flow.find("pcv_dispatcher_run_authorized_hooks(method, params)")
    route = flow.find("_dispatch_route(")
    create = flow.find("handle_vm_create(")
    if not (0 <= role < owner < flow.find("goto rpc_done", owner) < run < min(route, create)):
        bad.append("메인 경로: 역할·별칭 → 소유자 검사(실패면 rpc_done) → 권한 통과 훅 → 핸들러 순서")
    if d.count("pcv_dispatcher_run_authorized_hooks(") != 1:
        bad.append("dispatcher.c의 권한 통과 훅 실행 지점은 메인 경로 한 곳")
    if d.count(".owner_check = _dispatch_owner_check,") != 2:
        bad.append("레인 두 경로(사전 검사·오프로드)가 같은 _dispatch_owner_check를 쓴다")
    o = strip_comments(offload_text if offload_text is not None else OFFLOAD.read_text())
    lane = body(o, "_lane_worker")
    chk, hook, handler = lane.find("r->owner_check("), lane.find("pcv_dispatcher_run_authorized_hooks("), lane.find("r->handler(")
    if not (0 <= chk < hook < handler) or o.count("pcv_dispatcher_run_authorized_hooks(") != 1:
        bad.append("레인: 소유자 검사 통과 뒤·핸들러 직전에 한 번")
    start = body(strip_comments(source_text), "pcv_vm_cfg_hist_start_default")
    if "pcv_dispatcher_register_authorized_hook(pcv_vm_cfg_nudge_authorized_hook" not in start:
        bad.append("넛지는 권한 통과 훅으로 등록")
    if "register_pre_hook" in start:
        bad.append("넛지를 pre-dispatch 훅으로 등록하지 않는다")
    return bad

class NudgeContract(unittest.TestCase):
    def test_nudge_table_complete(self):
        nudge_text = NUDGE.read_text()
        routes = routes_of(DISPATCHER.read_text())
        self.assertEqual(check_tables(nudge_text, routes), [])

        mutated = re.sub(r'\n\s*\{"vm\.limit", NULL, NULL\},', "", nudge_text, count=1)
        self.assertNotEqual(mutated, nudge_text)
        self.assertTrue(check_tables(mutated, routes))

        self.assertTrue(check_tables(nudge_text, routes | {"vm.new_mutation"}))

    def test_audit_tap_single_site(self):
        text = strip_comments(AUDIT.read_text())
        fn = body(text, "pcv_audit_log")
        tap = fn.find("_completion_tap")
        self.assertGreaterEqual(tap, 0, "pcv_audit_log가 완료 탭을 부르지 않는다")
        self.assertLess(tap, fn.find("AUDIT_RATE_LIMIT"), "탭은 속도 제한보다 먼저")
        self.assertLess(tap, fn.find("g_async_queue_push"), "탭은 큐 넣기보다 먼저")
        setter = body(text, "pcv_audit_set_completion_tap")
        self.assertIn("g_atomic_pointer_set", setter)
        self.assertNotIn("pcv_audit_chain", body(strip_comments(NUDGE.read_text()), "pcv_vm_cfg_nudge_audit_tap"))

    def test_hook_and_tap_registered_before_serving(self):
        src = strip_comments(SOURCE.read_text())
        start = body(src, "pcv_vm_cfg_hist_start_default")
        self.assertIn("pcv_dispatcher_register_authorized_hook(pcv_vm_cfg_nudge_authorized_hook", start)
        self.assertIn("pcv_audit_set_completion_tap(pcv_vm_cfg_nudge_audit_tap", start)
        init = body(strip_comments(VIRT_EVENTS.read_text()), "init_virt_events_daemon")
        self.assertLess(init.find("pcv_vm_cfg_hist_start_default"), init.find("g_thread_try_new"))
        shutdown = body(strip_comments(VIRT_EVENTS.read_text()), "pcv_virt_events_shutdown")
        self.assertGreater(shutdown.find("pcv_vm_cfg_hist_shutdown"), shutdown.find("g_thread_join"))

    def test_intent_opens_only_after_authorization(self):

        disp, src, off = DISPATCHER.read_text(), SOURCE.read_text(), OFFLOAD.read_text()
        self.assertEqual(check_authorized_order(disp, src, off), [])

        line = "    pcv_dispatcher_run_authorized_hooks(method, params);"
        i = disp.index(line)
        moved = disp[:i] + disp[i + len(line):]
        moved = moved.replace("    if (!_run_pre_hooks(method, params, rpc_id_str)) {", line + "\n    if (!_run_pre_hooks(method, params, rpc_id_str)) {", 1)
        self.assertTrue(check_authorized_order(moved, src, off))

        self.assertTrue(check_authorized_order(disp, src.replace("pcv_dispatcher_register_authorized_hook(", "pcv_dispatcher_register_pre_hook(", 1), off))

        lane_moved = off.replace("        gboolean permitted = !r->owner_check ||",
                                 "        pcv_dispatcher_run_authorized_hooks(r->method, r->params);\n        gboolean permitted = !r->owner_check ||", 1)
        lane_moved = lane_moved.replace("            pcv_dispatcher_run_authorized_hooks(r->method, r->params);\n            r->handler(", "            r->handler(", 1)
        self.assertNotEqual(lane_moved, off)
        self.assertTrue(check_authorized_order(disp, src, lane_moved))

    def test_virt_events_enqueue_only(self):
        text = strip_comments(VIRT_EVENTS.read_text())
        calls = set(re.findall(r"\b(pcv_vm_cfg_[a-z_]+)\s*\(", text))
        allowed = {"pcv_vm_cfg_hist_note_uuid", "pcv_vm_cfg_hist_note_started", "pcv_vm_cfg_hist_note_undefined",
                   "pcv_vm_cfg_hist_coverage", "pcv_vm_cfg_hist_start_default", "pcv_vm_cfg_hist_shutdown"}
        self.assertEqual(calls - allowed, set())
        for need in ("pcv_vm_cfg_hist_note_uuid", "pcv_vm_cfg_hist_note_started", "pcv_vm_cfg_hist_note_undefined",
                     "pcv_vm_cfg_hist_coverage"):
            self.assertIn(need, calls)
        self.assertIn("metadata_callback_id", text)
        close = body(text, "_event_listener_close")
        self.assertIn("metadata_callback_id", close)
        self.assertIn("VIR_DOMAIN_EVENT_ID_METADATA_CHANGE", text)

    def test_pure_tus_no_io(self):
        for p in PURE:
            text = strip_comments(p.read_text())
            self.assertIsNone(re.search(r"\bvir[A-Z]\w*\s*\(", text), p.name)
            self.assertNotIn("sqlite3_", text, p.name)

    def test_no_secure_flag(self):
        self.assertTrue(NEW_TUS)
        for p in NEW_TUS:
            self.assertNotIn("VIR_DOMAIN_XML_" + "SECURE", p.read_text(), p.name)

    def test_no_name_keyed_history_sql(self):
        sql = " ".join(re.findall(r'"([^"]*)"', strip_comments(STORE.read_text())))
        self.assertIsNone(re.search(r"vm_name\s*(=|LIKE|IN)", sql), "config_change를 vm_name으로 찾는 SQL 금지(C2)")
        self.assertIn("current_name = ?1", sql)

if __name__ == "__main__":
    sys.exit(0 if unittest.main(exit=False, verbosity=1).result.wasSuccessful() else 1)
