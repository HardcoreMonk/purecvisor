#!/usr/bin/env python3

from __future__ import annotations

import json
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
POLICY = ROOT / "src/api/dispatcher_policy.c"
OFFLOAD = ROOT / "src/modules/dispatcher/rpc_offload.c"
DISPATCHER = ROOT / "src/api/dispatcher.c"
HANDLER = ROOT / "src/modules/dispatcher/handler_vm_config_history.c"
VIEW = ROOT / "src/modules/virt/vm_config_history_view.c"
PARAMS = ROOT / "contracts/rpc_params.json"
REST = ROOT / "src/api/rest_map_compute.c"
METHODS = ("vm.config.history.list", "vm.config.history.get")

def rest_vm_path_problems(text: str) -> list[str]:

    i = text.find('g_strcmp0(action, "config-history") == 0')
    if i < 0:
        return ["config-history 분기 없음"]
    seg = text[i:i + 1200]
    rej = seg.find('if (json_object_has_member(p, "vm") || json_object_has_member(p, "uuid")) {')
    put = seg.find('json_object_set_string_member(p, "vm", name);')
    if rej < 0 or put < 0 or rej > put or "400" not in seg[rej:put]:
        return ["쿼리 vm·uuid 거절(400)이 경로 이름 넣기보다 먼저 있어야 한다"]
    return []

def strip_comments(text: str) -> str:

    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)

def body(text: str, fname: str) -> str:

    m = re.search(r"^[\w \t*]*\b" + re.escape(fname) + r"\s*\([^;{]*\)\s*\{", text, re.M)
    if not m:
        raise AssertionError(f"함수 {fname} 없음")
    i, depth = m.end(), 1
    while depth and i < len(text):
        depth += (text[i] == "{") - (text[i] == "}")
        i += 1
    return text[m.end():i]

def policy_problems(policy_text: str) -> list[str]:

    code = strip_comments(policy_text)
    errs = []
    for m in METHODS:
        if not re.search(r'\{\s*"' + re.escape(m) + r'"\s*,\s*0\s*\}', code):
            errs.append(f"정책 표 VIEWER(0) 아님: {m}")
        owner = body(code, "pcv_disp_vm_method_requires_owner_scope")
        if f'g_strcmp0(method, "{m}") == 0' not in owner:
            errs.append(f"소유자 검사 면제 목록에 없음: {m}")
        read_tbl = re.search(r"k_vm_read_methods\[\]\s*=\s*\{(.*?)\n\};", code, re.S)
        if not read_tbl or f'"{m}"' in read_tbl.group(1):
            errs.append(f"VM 읽기 사전 검사 표에 있으면 안 됨: {m}")
    return errs

def handler_problems(handler_text: str) -> list[str]:

    code = strip_comments(handler_text)
    errs = []
    if "pcv_vm_cfg_row_visible" not in body(code, "_row_visible_cb"):
        errs.append("list 콜백이 행 판정을 부르지 않는다")
    if "_row_visible_cb" not in body(code, "pcv_vm_cfg_history_list_build"):
        errs.append("list가 판정 콜백으로 거르지 않는다")
    if body(code, "pcv_vm_cfg_history_get_build").count("pcv_vm_cfg_row_visible") < 2:
        errs.append("get이 행·직전 행을 판정하지 않는다")
    if re.search(r"\bvir[A-Z]\w*\s*\(|virt_conn_pool_", code):
        errs.append("처리기가 libvirt·연결 풀을 부른다(SCOPE_READ 금지)")
    return errs

class RpcContract(unittest.TestCase):
    def test_policy_viewer_and_owner_scope_exempt(self):
        text = POLICY.read_text()
        self.assertEqual(policy_problems(text), [])
        mutated = re.sub(r'\n[^\n]*g_strcmp0\(method, "vm\.config\.history\.list"\) == 0 \|\|[^\n]*', "", text, count=1)
        self.assertNotEqual(mutated, text)
        self.assertTrue(policy_problems(mutated))

    def test_scope_read_lane(self):
        lanes = dict(re.findall(r'\{\s*"([a-z0-9_.]+)"\s*,\s*PCV_OFFLOAD_LANE_(\w+)\s*\}', strip_comments(OFFLOAD.read_text())))
        routes = dict(re.findall(r'g_rpc_routes,\s*"([a-z0-9_.]+)",\s*\(gpointer\)(\w+)\)', strip_comments(DISPATCHER.read_text())))
        for m in METHODS:
            self.assertEqual(lanes.get(m), "SCOPE_READ", m)
        self.assertEqual(routes.get("vm.config.history.list"), "handle_vm_config_history_list")
        self.assertEqual(routes.get("vm.config.history.get"), "handle_vm_config_history_get")

    def test_handler_judges_every_row(self):
        text = HANDLER.read_text()
        self.assertEqual(handler_problems(text), [])
        mutated = text.replace("return pcv_vm_cfg_row_visible(ud, owner, owner_tenant);", "return TRUE;")
        self.assertNotEqual(mutated, text)
        self.assertTrue(handler_problems(mutated))

    def test_no_internal_id_in_projection(self):
        proj = body(strip_comments(VIEW.read_text()), "pcv_vm_cfg_row_project")
        self.assertIn('json_object_set_string_member(o, "id", row->entry_id)', proj)
        self.assertNotIn("row->id", proj)
        self.assertNotIn("row->prev_id", proj)
        self.assertIn("row->prev_entry_id", proj)

    def test_rest_vm_path_rejects_query_target(self):

        self.assertEqual(rest_vm_path_problems(REST.read_text()), [])
        mutated = REST.read_text().replace('if (json_object_has_member(p, "vm") || json_object_has_member(p, "uuid")) {',
                                           'if (FALSE) {', 1)
        self.assertTrue(rest_vm_path_problems(mutated))

    def test_rpc_params_registry(self):
        reg = json.loads(PARAMS.read_text())
        self.assertEqual(reg["vm.config.history.list"]["required"], [])
        self.assertEqual(set(reg["vm.config.history.list"]["optional"]), {"vm", "uuid", "since", "until", "limit", "cursor"})
        self.assertEqual(reg["vm.config.history.get"]["required"], ["id"])

if __name__ == "__main__":
    sys.exit(0 if unittest.main(exit=False, verbosity=1).result.wasSuccessful() else 1)
