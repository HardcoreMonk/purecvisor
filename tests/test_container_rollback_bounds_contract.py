#!/usr/bin/env python3
                                                                             
                                                                                                
                                                                                                 
                                                                          
                                                              
                                                                                           
                                                                                      
                                                                     
                                                           
                                                                 
                                                       
                                                                                
import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_container_exec_bounds_contract import (                                          
    function_body, matching_close, split_args, strip_comments)

ROOT = Path(__file__).resolve().parents[1]
DRIVER_H = "src/modules/lxc/lxc_driver.h"
REST_C = "src/api/rest_middleware.c"
CLI_C = "src/cli/cli_rpc.c"
CTL_C = "src/cli/purecvisorctl.c"
CTR_JS = "ui/modules/container.js"
METHOD = "container.snapshot.rollback"
GRACE_EXPECT = 30                                                 
SERVER_MARGIN_SEC = 45                                                                  
UI_MARGIN_SEC = 5                                                
UI_CONFIRM_PHRASE = "실행 중이면 멈췄다가 다시 켭니다"
CLI_WARNING = "Container will be stopped, rolled back, and started again if it was running!"


def grace_sec(text: str) -> int | None:
    m = re.search(r"^\s*#\s*define\s+PCV_LXC_STOP_GRACE_SEC\s+\(?\s*(\d+)\s*\)?\s*$",
                  strip_comments(text), re.M)
    return int(m.group(1)) if m else None


def rest_rollback_timeout(text: str) -> int | None:
    body = function_body(strip_comments(text),
                         r"\bgint\s+pcv_get_rpc_timeout\s*\(\s*const\s+gchar\s*\*\s*rpc_method\s*\)")
    if not body:
        return None
    m = re.search(r'g_strcmp0\s*\(\s*rpc_method\s*,\s*"' + re.escape(METHOD) + r'"\s*\)\s*==\s*0'
                  r'[^;]*?\breturn\s+(\d+)\s*;', body, re.S)
    return int(m.group(1)) if m else None


def cli_rollback_timeout(text: str) -> int | None:
    m = re.search(r"\bguint\s+sock_timeout\s*=([^;]*);", strip_comments(text), re.S)
    if not m:
        return None
    e = re.search(r'g_strcmp0\s*\(\s*method\s*,\s*"' + re.escape(METHOD) + r'"\s*\)\s*==\s*0\s*\)?\s*\?\s*(\d+)',
                  m.group(1))
    return int(e.group(1)) if e else None


def ui_rollback_timeout_ms(text: str) -> int | None:
    m = re.search(r"\bconst\s+CTR_ROLLBACK_TIMEOUT_MS\s*=\s*(\d+)\s*;", strip_comments(text))
    return int(m.group(1)) if m else None


def ui_rollback_fn(text: str) -> str | None:
                                                          
    return function_body(strip_comments(text), r"\basync\s+function\s+ctrSnapRb\s*\([^)]*\)")


def ui_call_args(fn_body: str) -> list[str] | None:
    m = re.search(r"\bfetchPost\s*\(\s*EP\.CTR_SNAP_ROLLBACK\s*\(", fn_body)
    if not m:
        return None
    open_idx = fn_body.index("(", m.start())
    close = matching_close(fn_body, open_idx)
    return split_args(fn_body[open_idx + 1:close]) if close > 0 else None


def check(texts: dict[str, str]) -> list[str]:
                                                       
    errs = []
    grace = grace_sec(texts[DRIVER_H])
    if grace != GRACE_EXPECT:
        errs.append(f"{DRIVER_H}: PCV_LXC_STOP_GRACE_SEC={grace} != {GRACE_EXPECT}")
    need = (grace or GRACE_EXPECT) + SERVER_MARGIN_SEC
    rest = rest_rollback_timeout(texts[REST_C])
    if rest is None or rest < need:
        errs.append(f"{REST_C}: {METHOD} 상한 {rest} < 정지 {grace}+{SERVER_MARGIN_SEC}초")
    cli = cli_rollback_timeout(texts[CLI_C])
    if cli is None or cli < need:
        errs.append(f"{CLI_C}: {METHOD} 상한 {cli} < 정지 {grace}+{SERVER_MARGIN_SEC}초")
    ui_ms = ui_rollback_timeout_ms(texts[CTR_JS])
    ui_need = ((rest or need) + UI_MARGIN_SEC) * 1000
    if ui_ms is None or ui_ms < ui_need:
        errs.append(f"{CTR_JS}: CTR_ROLLBACK_TIMEOUT_MS {ui_ms} < (REST {rest}+{UI_MARGIN_SEC}초)×1000")
    fn = ui_rollback_fn(texts[CTR_JS])
    if fn is None:
        errs.append(f"{CTR_JS}: ctrSnapRb 함수를 찾지 못함(판정 불가)")
    else:
        args = ui_call_args(fn)
        if args is None:
            errs.append(f"{CTR_JS}: ctrSnapRb 안 fetchPost(EP.CTR_SNAP_ROLLBACK…) 호출을 찾지 못함")
        elif len(args) != 3 or args[2] != "CTR_ROLLBACK_TIMEOUT_MS":
            errs.append(f"{CTR_JS}: ctrSnapRb 롤백 호출에 CTR_ROLLBACK_TIMEOUT_MS 상한이 없음")
        if UI_CONFIRM_PHRASE not in fn:
            errs.append(f"{CTR_JS}: 롤백 확인창에 '{UI_CONFIRM_PHRASE}' 문구가 없음")
    if CLI_WARNING not in strip_comments(texts[CTL_C]):
        errs.append(f"{CTL_C}: 롤백 경고 문구가 spec 값이 아님")
    return errs


def load_tree() -> dict[str, str]:
    return {rel: (ROOT / rel).read_text(errors="replace") for rel in (DRIVER_H, REST_C, CLI_C, CTL_C, CTR_JS)}


class ContainerRollbackBoundsContract(unittest.TestCase):
    def setUp(self):
        self.texts = load_tree()

    def assertRed(self, rel: str, old_re: str, new: str, needle: str):
        t = dict(self.texts)
        mutated, n = re.subn(old_re, new, t[rel], count=1)
        self.assertEqual(n, 1, f"반사실 기준이 정확히 한 번 있어야 함: {old_re!r}")
        t[rel] = mutated
        errs = check(t)
        self.assertTrue(any(needle in e for e in errs), f"'{needle}' 위반이 나와야 함: {errs}")

    def test_tree_satisfies_contract(self):
        self.assertEqual(check(self.texts), [])

    def test_red_when_grace_changes(self):
        self.assertRed(DRIVER_H, r"(#define\s+PCV_LXC_STOP_GRACE_SEC\s+)\d+", r"\g<1>20", "PCV_LXC_STOP_GRACE_SEC=20")

    def test_red_when_rest_bound_too_short(self):
        self.assertRed(REST_C, r'("container\.snapshot\.rollback"\s*\)\s*==\s*0\s*\)?\s*return\s+)\d+',
                       r"\g<1>74", f"{REST_C}: {METHOD} 상한 74")

    def test_red_when_rest_entry_missing(self):
        self.assertRed(REST_C, r'"container\.snapshot\.rollback"', '"container.snapshot.rollback.x"',
                       f"{REST_C}: {METHOD} 상한 None")

    def test_red_when_cli_bound_too_short(self):
        self.assertRed(CLI_C, r'("container\.snapshot\.rollback"\s*\)\s*==\s*0\s*\)?\s*\?\s*)\d+',
                       r"\g<1>20", f"{CLI_C}: {METHOD} 상한 20")

    def test_red_when_ui_bound_too_short(self):
        self.assertRed(CTR_JS, r"(const\s+CTR_ROLLBACK_TIMEOUT_MS\s*=\s*)\d+", r"\g<1>12000",
                       "CTR_ROLLBACK_TIMEOUT_MS 12000")

    def test_red_when_ui_call_drops_timeout(self):
        self.assertRed(CTR_JS, r"(fetchPost\(EP\.CTR_SNAP_ROLLBACK\(n\),\s*\{\s*snap_name:\s*s\s*\}),\s*CTR_ROLLBACK_TIMEOUT_MS\)",
                       r"\g<1>)", "롤백 호출에 CTR_ROLLBACK_TIMEOUT_MS 상한이 없음")

    def test_red_when_confirm_phrase_missing(self):
        self.assertRed(CTR_JS, r"실행 중이면 멈췄다가 다시 켭니다", "롤백합니다", "확인창에")

    def test_red_when_cli_warning_reverts(self):
        self.assertRed(CTL_C, r"Container will be stopped, rolled back, and started again if it was running!",
                       "Container will be stopped before rollback!", "롤백 경고 문구")


if __name__ == "__main__":
    sys.exit(0 if unittest.main(exit=False).result.wasSuccessful() else 1)
