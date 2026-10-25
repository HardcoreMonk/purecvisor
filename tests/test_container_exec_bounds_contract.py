#!/usr/bin/env python3

import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NS_H = "src/modules/lxc/lxc_ns_exec.h"
REST_C = "src/api/rest_middleware.c"
CLI_C = "src/cli/cli_rpc.c"
API_JS = "ui/modules/api.js"
CTR_JS = "ui/modules/container.js"
UI_MODULES = "ui/modules"
TERMINAL_FN = "ctrRunCmd"

NS_KEYS = ("PCV_LXC_NS_READ_TIMEOUT_SEC", "PCV_LXC_NS_CHANGE_TIMEOUT_SEC",
           "PCV_LXC_NS_WIRING_BUDGET_SEC", "PCV_LXC_EXEC_TIMEOUT_SEC")
EXEC_CLIENT_MARGIN_SEC = 5
EXEC_UI_MARGIN_SEC = 10

def strip_comments(text: str) -> str:

    out, i, n = [], 0, len(text)
    while i < n:
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif text[i] in "\"'`":
            q, j = text[i], i + 1
            while j < n and text[j] != q:
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        else:
            out.append(text[i])
            i += 1
    return "".join(out)

def matching_close(text: str, open_idx: int) -> int:

    pair = {"(": ")", "{": "}"}
    opener = text[open_idx]
    closer, depth, i, n = pair[opener], 0, open_idx, len(text)
    while i < n:
        c = text[i]
        if c in "\"'`":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            i = j + 1
            continue
        if c == opener:
            depth += 1
        elif c == closer:
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1

def function_body(code: str, head_re: str) -> str | None:

    m = re.search(head_re + r"\s*\{", code)
    if not m:
        return None
    start = m.end() - 1
    end = matching_close(code, start)
    return None if end < 0 else code[start:end + 1]

def ns_constants(h_text: str) -> dict[str, int]:
    code = strip_comments(h_text)
    out = {}
    for key in NS_KEYS:
        m = re.search(r"^\s*#\s*define\s+" + key + r"\s+\(?\s*(\d+)\s*\)?\s*$", code, re.M)
        if m:
            out[key] = int(m.group(1))
    return out

def rest_timeouts(text: str) -> tuple[int | None, int | None]:

    code = strip_comments(text)
    d = re.search(r"\bREST_RPC_TIMEOUT_SEC_MW\s*=\s*(\d+)\s*;", code)
    body = function_body(code, r"\bgint\s+pcv_get_rpc_timeout\s*\(\s*const\s+gchar\s*\*\s*rpc_method\s*\)")
    exec_v = None
    if body:
        m = re.search(r'g_strcmp0\s*\(\s*rpc_method\s*,\s*"container\.exec"\s*\)\s*==\s*0'
                      r'[^;]*?\breturn\s+(\d+)\s*;', body, re.S)
        exec_v = int(m.group(1)) if m else None
    return (int(d.group(1)) if d else None), exec_v

def cli_timeouts(text: str) -> tuple[int | None, int | None]:

    code = strip_comments(text)
    m = re.search(r"\bguint\s+sock_timeout\s*=([^;]*);", code, re.S)
    if not m:
        return None, None
    chain = m.group(1)
    d = re.search(r":\s*(\d+)\s*$", chain.strip())
    e = re.search(r'g_strcmp0\s*\(\s*method\s*,\s*"container\.exec"\s*\)\s*==\s*0\s*\)?\s*\?\s*(\d+)', chain)
    return (int(d.group(1)) if d else None), (int(e.group(1)) if e else None)

def api_js_violations(text: str) -> list[str]:

    code = strip_comments(text)
    errs = []
    fwt = function_body(code, r"\bfunction\s+_fetchWithTimeout\s*\(\s*url\s*,\s*opts\s*,\s*timeoutMs\s*\)")
    if fwt is None:
        errs.append("_fetchWithTimeout(url, opts, timeoutMs) 정의 없음")
    else:
        st = re.search(r"\bsetTimeout\s*\(", fwt)
        close = matching_close(fwt, st.end() - 1) if st else -1
        if close < 0 or not re.search(r",\s*timeoutMs\b[^,]*$", fwt[st.end():close]):
            errs.append("_fetchWithTimeout이 timeoutMs를 setTimeout 상한에 쓰지 않음")
    fwr = function_body(code, r"\basync\s+function\s+_fetchWithRefresh\s*\(\s*url\s*,\s*opts\s*,\s*timeoutMs\s*\)")
    if fwr is None:
        errs.append("_fetchWithRefresh(url, opts, timeoutMs) 정의 없음")
    else:
        calls = re.findall(r"_fetchWithTimeout\s*\(\s*url\s*,\s*opts\s*(,\s*timeoutMs\s*)?\)", fwr)
        if not calls or any(not c for c in calls):
            errs.append("_fetchWithRefresh가 모든 _fetchWithTimeout 호출(재시도 포함)에 timeoutMs를 넘기지 않음")
    fp = function_body(code, r"\bfunction\s+fetchPost\s*\(\s*u\s*,\s*b\s*,\s*timeoutMs\s*\)")
    if fp is None:
        errs.append("fetchPost(u, b, timeoutMs) 정의 없음")
    elif not re.search(r"_fetchWithRefresh\s*\(\s*u\s*,\s*\{.*\}\s*,\s*timeoutMs\s*\)", fp, re.S):
        errs.append("fetchPost가 timeoutMs를 _fetchWithRefresh에 넘기지 않음")
    return errs

def ui_exec_timeout_ms(ctr_text: str) -> int | None:
    m = re.search(r"\bconst\s+CTR_EXEC_TIMEOUT_MS\s*=\s*(\d+)\s*;", strip_comments(ctr_text))
    return int(m.group(1)) if m else None

def split_args(args: str) -> list[str]:

    parts, cur, depth, i, n = [], [], 0, 0, len(args)
    while i < n:
        c = args[i]
        if c in "\"'`":
            j = i + 1
            while j < n and args[j] != c:
                j += 2 if args[j] == "\\" else 1
            cur.append(args[i:j + 1])
            i = j + 1
            continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == "," and depth == 0:
            parts.append("".join(cur).strip())
            cur = []
            i += 1
            continue
        cur.append(c)
        i += 1
    parts.append("".join(cur).strip())
    return parts

TOP_FUNC_RE = re.compile(r"^(?:async\s+)?function\s+([A-Za-z_$][\w$]*)\s*\(", re.M)

def enclosing_top_function(code: str, pos: int) -> str | None:

    name = None
    for m in TOP_FUNC_RE.finditer(code, 0, pos):
        name = m.group(1)
    return name

def ui_exec_call_violations(sources: dict[str, str]) -> tuple[int, int, list[str]]:

    count, terminal, errs = 0, 0, []
    for name, text in sources.items():
        code = strip_comments(text)
        for m in re.finditer(r"\bEP\.CTR_EXEC\s*\(", code):
            count += 1
            line = code.count("\n", 0, m.start()) + 1
            call = re.search(r"\bfetchPost\s*\(\s*$", code[:m.start()])
            if not call:
                errs.append(f"{name}:{line}: EP.CTR_EXEC가 fetchPost 첫 인자가 아님")
                continue
            open_idx = call.end() - 1
            while code[open_idx] != "(":
                open_idx -= 1
            close = matching_close(code, open_idx)
            args = split_args(code[open_idx + 1:close]) if close > 0 else []
            fn = enclosing_top_function(code, m.start())
            if name == CTR_JS and fn == TERMINAL_FN:
                terminal += 1
                if len(args) != 3 or args[2] != "CTR_EXEC_TIMEOUT_MS":
                    errs.append(f"{name}:{line}: 터미널 호출({fn}) fetchPost(EP.CTR_EXEC…)에 "
                                f"CTR_EXEC_TIMEOUT_MS 상한이 없음")
            elif len(args) != 2:
                errs.append(f"{name}:{line}: 터미널 밖 호출({fn}) fetchPost(EP.CTR_EXEC…)는 기본 fetch 상한을 "
                            f"써야 함(상한 인자 금지): {', '.join(args[2:])}")
    return count, terminal, errs

def check(texts: dict[str, str]) -> list[str]:

    errs = []
    ns = ns_constants(texts[NS_H])
    for key in NS_KEYS:
        if key not in ns:
            errs.append(f"{NS_H}: {key} 정의를 읽지 못함")
    rest_default, rest_exec = rest_timeouts(texts[REST_C])
    cli_default, cli_exec = cli_timeouts(texts[CLI_C])
    if rest_default is None:
        errs.append(f"{REST_C}: REST_RPC_TIMEOUT_SEC_MW 기본 상한을 읽지 못함")
    if cli_default is None:
        errs.append(f"{CLI_C}: sock_timeout 기본 상한을 읽지 못함")
    if errs:
        return errs
    client_floor = min(rest_default, cli_default)
    for key in NS_KEYS[:3]:
        if ns[key] >= client_floor:
            errs.append(f"{key}={ns[key]}초가 클라이언트 기본 상한 {client_floor}초보다 짧지 않음")
    exec_sec = ns["PCV_LXC_EXEC_TIMEOUT_SEC"]
    need = exec_sec + EXEC_CLIENT_MARGIN_SEC
    if rest_exec is None or rest_exec < need:
        errs.append(f"{REST_C}: container.exec 상한 {rest_exec} < 서버 {exec_sec}+{EXEC_CLIENT_MARGIN_SEC}초")
    if cli_exec is None or cli_exec < need:
        errs.append(f"{CLI_C}: container.exec 상한 {cli_exec} < 서버 {exec_sec}+{EXEC_CLIENT_MARGIN_SEC}초")
    errs += [f"{API_JS}: {e}" for e in api_js_violations(texts[API_JS])]
    ui_ms = ui_exec_timeout_ms(texts[CTR_JS])
    ui_need = (exec_sec + EXEC_UI_MARGIN_SEC) * 1000
    if ui_ms is None or ui_ms < ui_need:
        errs.append(f"{CTR_JS}: CTR_EXEC_TIMEOUT_MS {ui_ms} < (서버 {exec_sec}+{EXEC_UI_MARGIN_SEC}초)×1000")
    ui_sources = {k: v for k, v in texts.items()
                  if k.startswith(UI_MODULES + "/") and k != UI_MODULES + "/endpoints.js"}
    count, terminal, call_errs = ui_exec_call_violations(ui_sources)
    if count == 0:
        errs.append("ui/modules에서 EP.CTR_EXEC 호출을 하나도 찾지 못함(판정 불가)")
    if terminal == 0:
        errs.append(f"{CTR_JS}: 터미널 호출({TERMINAL_FN})의 EP.CTR_EXEC를 찾지 못함(판정 불가)")
    errs += call_errs
    return errs

def load_tree() -> dict[str, str]:
    texts = {}
    for rel in (NS_H, REST_C, CLI_C):
        p = ROOT / rel
        texts[rel] = p.read_text(errors="replace") if p.exists() else ""
    for p in sorted((ROOT / UI_MODULES).glob("*.js")):
        texts[f"{UI_MODULES}/{p.name}"] = p.read_text(errors="replace")
    return texts

def _once(text: str, old: str, new: str) -> str:
    assert text.count(old) == 1, f"반사실 기준 문자열이 정확히 한 번 있어야 함: {old!r}"
    return text.replace(old, new)

class ContainerExecBoundsContract(unittest.TestCase):
    def setUp(self):
        self.texts = load_tree()

    def assertRed(self, mutated: dict[str, str], needle: str):
        errs = check(mutated)
        self.assertTrue(any(needle in e for e in errs), f"'{needle}' 위반이 나와야 함: {errs}")

    def _with(self, rel: str, old: str, new: str) -> dict[str, str]:
        t = dict(self.texts)
        t[rel] = _once(t[rel], old, new)
        return t

    def test_tree_satisfies_contract(self):
        self.assertEqual(check(self.texts), [])

    def test_ns_constants_below_client_default(self):
        ns = ns_constants(self.texts[NS_H])
        rest_default, _ = rest_timeouts(self.texts[REST_C])
        cli_default, _ = cli_timeouts(self.texts[CLI_C])
        for key in NS_KEYS[:3]:
            self.assertLess(ns[key], rest_default, key)
            self.assertLess(ns[key], cli_default, key)

    def test_red_when_read_bound_reaches_rest_default(self):
        bad = re.sub(r"(#define\s+PCV_LXC_NS_READ_TIMEOUT_SEC\s+)\d+", r"\g<1>8", self.texts[NS_H])
        t = dict(self.texts)
        t[NS_H] = bad
        self.assertRed(t, "PCV_LXC_NS_READ_TIMEOUT_SEC=8")

    def test_red_when_rest_exec_entry_missing(self):
        t = dict(self.texts)
        t[REST_C] = self.texts[REST_C].replace('"container.exec"', '"container.exec.x"')
        self.assertRed(t, "container.exec 상한 None")

    def test_red_when_cli_exec_too_short(self):
        t = dict(self.texts)
        t[CLI_C] = re.sub(r'("container\.exec"\s*\)\s*==\s*0\s*\?\s*)\d+', r"\g<1>10", self.texts[CLI_C])
        self.assertRed(t, f"{CLI_C}: container.exec 상한 10")

    def test_red_when_ui_exec_call_drops_timeout(self):
        t = self._with(CTR_JS, "fetchPost(EP.CTR_EXEC(n), { command: cmd }, CTR_EXEC_TIMEOUT_MS)",
                       "fetchPost(EP.CTR_EXEC(n), { command: cmd })")
        self.assertRed(t, "터미널 호출(ctrRunCmd)")

    def test_red_when_non_terminal_exec_call_uses_long_timeout(self):
        t = self._with(CTR_JS, "fetchPost(EP.CTR_EXEC(n), { command: 'cat /etc/resolv.conf 2>/dev/null' })",
                       "fetchPost(EP.CTR_EXEC(n), { command: 'cat /etc/resolv.conf 2>/dev/null' }, "
                       "CTR_EXEC_TIMEOUT_MS)")
        self.assertRed(t, "터미널 밖 호출(ctrRenderTab)")

    def test_red_when_ui_exec_timeout_too_short(self):
        t = dict(self.texts)
        t[CTR_JS] = re.sub(r"(const\s+CTR_EXEC_TIMEOUT_MS\s*=\s*)\d+", r"\g<1>12000", self.texts[CTR_JS])
        self.assertRed(t, "CTR_EXEC_TIMEOUT_MS 12000")

    def test_red_when_settimeout_ignores_timeout(self):
        t = self._with(API_JS, "timeoutMs || PCV_FETCH_TIMEOUT_MS", "PCV_FETCH_TIMEOUT_MS")
        self.assertRed(t, "setTimeout 상한에 쓰지 않음")

    def test_red_when_api_retry_drops_timeout(self):
        code = self.texts[API_JS]
        m = re.search(r"return\s+_fetchWithTimeout\s*\(\s*url\s*,\s*opts\s*,\s*timeoutMs\s*\)", code)
        self.assertIsNotNone(m)
        t = dict(self.texts)
        t[API_JS] = code[:m.start()] + "return _fetchWithTimeout(url, opts)" + code[m.end():]
        self.assertRed(t, "재시도 포함")

if __name__ == "__main__":
    sys.exit(0 if unittest.main(exit=False).result.wasSuccessful() else 1)
