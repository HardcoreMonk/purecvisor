#!/usr/bin/env python3
                          
                                                                                              
                                                                                              
                                                                                                                        
                                                                   
                                                                                     
                                                                                          
                                                                               
                                                                 
                                                                          
                                            
                                                                              
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
from test_container_exec_bounds_contract import (              
    function_body, matching_close, split_args, strip_comments)

REQ_H = "src/modules/dispatcher/guest_exec_request.h"
REST_C = "src/api/rest_middleware.c"
CLI_C = "src/cli/cli_rpc.c"
GUEST_JS = "ui/modules/vm-guest.js"
CLIENT_MARGIN_SEC = 5                               
UI_MARGIN_SEC = 10                                   


def budget_sec(h_text: str) -> int | None:
    m = re.search(r"^\s*#\s*define\s+PCV_GUEST_EXEC_BUDGET_SEC\s+\(?\s*(\d+)\s*\)?\s*$", strip_comments(h_text), re.M)
    return int(m.group(1)) if m else None


def rest_timeout(text: str) -> int | None:
    code = strip_comments(text)
    body = function_body(code, r"\bgint\s+pcv_get_rpc_timeout\s*\(\s*const\s+gchar\s*\*\s*rpc_method\s*\)")
    if not body:
        return None
    m = re.search(r'g_strcmp0\s*\(\s*rpc_method\s*,\s*"vm\.guest\.exec"\s*\)\s*==\s*0'
                  r'[^;]*?\breturn\s+(\d+)\s*;', body, re.S)
    return int(m.group(1)) if m else None


def cli_timeout(text: str) -> int | None:
    m = re.search(r"\bguint\s+sock_timeout\s*=([^;]*);", strip_comments(text), re.S)
    if not m:
        return None
    e = re.search(r'g_strcmp0\s*\(\s*method\s*,\s*"vm\.guest\.exec"\s*\)\s*==\s*0\s*\)?\s*\?\s*(\d+)', m.group(1))
    return int(e.group(1)) if e else None


def ui_exec_calls(text: str) -> list[list[str]]:
                                                             
    code = strip_comments(text)
    calls = []
    for m in re.finditer(r"\bfetchPost\s*\(\s*EP\.VM_GUEST_EXEC\s*\(", code):
        open_idx = code.index("(", m.start())
        close = matching_close(code, open_idx)
        calls.append(split_args(code[open_idx + 1:close]) if close > 0 else [])
    return calls


def check(texts: dict[str, str]) -> list[str]:
                                  
    errs = []
    budget = budget_sec(texts[REQ_H])
    if budget is None:
        return [f"{REQ_H}: PCV_GUEST_EXEC_BUDGET_SEC 정의를 읽지 못함"]
    need = budget + CLIENT_MARGIN_SEC
    rest = rest_timeout(texts[REST_C])
    if rest is None or rest < need:
        errs.append(f"{REST_C}: vm.guest.exec 상한 {rest} < 서버 예산 {budget}+{CLIENT_MARGIN_SEC}초")
    cli = cli_timeout(texts[CLI_C])
    if cli is None or cli < need:
        errs.append(f"{CLI_C}: vm.guest.exec 상한 {cli} < 서버 예산 {budget}+{CLIENT_MARGIN_SEC}초")
    ui_need = (budget + UI_MARGIN_SEC) * 1000
    calls = ui_exec_calls(texts[GUEST_JS])
    if not calls:
        errs.append(f"{GUEST_JS}: fetchPost(EP.VM_GUEST_EXEC…) 호출을 찾지 못함(판정 불가)")
    for args in calls:
        ms = int(args[2]) if len(args) == 3 and args[2].isdigit() else None
        if ms is None or ms < ui_need:
            errs.append(f"{GUEST_JS}: gaExec fetchPost(EP.VM_GUEST_EXEC…) 상한 {args[2:] or None} < "
                        f"(서버 예산 {budget}+{UI_MARGIN_SEC}초)×1000")
    return errs


def load_tree() -> dict[str, str]:
    return {rel: (ROOT / rel).read_text(errors="replace") for rel in (REQ_H, REST_C, CLI_C, GUEST_JS)}


class GuestExecBoundsContract(unittest.TestCase):
    def setUp(self):
        self.texts = load_tree()

    def assertRed(self, rel: str, pattern: str, repl: str, needle: str):
        t = dict(self.texts)
        t[rel], n = re.subn(pattern, repl, self.texts[rel], count=1)
        self.assertEqual(n, 1, f"반사실 기준을 {rel}에서 찾지 못함: {pattern}")
        errs = check(t)
        self.assertTrue(any(needle in e for e in errs), f"'{needle}' 위반이 나와야 함: {errs}")

    def test_tree_satisfies_contract(self):
        self.assertEqual(check(self.texts), [])

    def test_values_are_read(self):
        self.assertEqual(budget_sec(self.texts[REQ_H]), 15)
        self.assertEqual(rest_timeout(self.texts[REST_C]), 20)
        self.assertEqual(cli_timeout(self.texts[CLI_C]), 20)
        self.assertEqual([a[2:] for a in ui_exec_calls(self.texts[GUEST_JS])], [["25000"]])

    def test_red_when_rest_entry_missing(self):
        self.assertRed(REST_C, r'"vm\.guest\.exec"', '"vm.guest.exec.x"', f"{REST_C}: vm.guest.exec 상한 None")

    def test_red_when_cli_too_short(self):
        self.assertRed(CLI_C, r'("vm\.guest\.exec"\s*\)\s*==\s*0\s*\?\s*)\d+', r"\g<1>10",
                       f"{CLI_C}: vm.guest.exec 상한 10")

    def test_red_when_ui_drops_timeout(self):
        self.assertRed(GUEST_JS, r"(fetchPost\(EP\.VM_GUEST_EXEC\(v\.name\), params), 25000\)", r"\g<1>)",
                       "gaExec fetchPost(EP.VM_GUEST_EXEC…) 상한 None")

    def test_red_when_budget_grows_past_clients(self):
        self.assertRed(REQ_H, r"(#define\s+PCV_GUEST_EXEC_BUDGET_SEC\s+)\d+", r"\g<1>18",
                       f"{REST_C}: vm.guest.exec 상한 20 < 서버 예산 18")


if __name__ == "__main__":
    sys.exit(0 if unittest.main(exit=False).result.wasSuccessful() else 1)
