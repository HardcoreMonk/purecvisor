#!/usr/bin/env python3
"""RPC 계약 게이트 공유 추출 원시함수.

check_rpc_consumers.py(method 존재)와 check_rpc_param_contract.py(param-key)가
공유한다. C/JS 소스에서 등록 route·소비 method 리터럴·param 키를 정적 추출한다.
stdlib(re, pathlib)만 사용.
"""
import re
from pathlib import Path

# 등록: g_hash_table_insert(g_rpc_routes, "x.y", ...)
ROUTE_RE = re.compile(r'g_hash_table_insert\s*\(\s*g_rpc_routes\s*,\s*"([a-z][a-z0-9_.]+)"')
# 특수 인라인 디스패치: g_strcmp0(method, "x.y")
SPECIAL_RE = re.compile(r'g_strcmp0\s*\(\s*method\s*,\s*"([a-z][a-z0-9_.]+)"')
# 소비 method 리터럴
CLI_RE = re.compile(r'purectl_send_request\s*\(\s*"([a-z][a-z0-9_.]+)"')
TUI_RE = re.compile(r'(?:tui_send_request|send_async_rpc)\s*\(\s*"([a-z][a-z0-9_.]+)"')
FE_RE_A = re.compile(
    r"""jsonrpc['"]?\s*:\s*['"]2\.0['"].{0,400}?method\s*:\s*['"]([a-z][a-z0-9_.]+)['"]""",
    re.DOTALL)
FE_RE_B = re.compile(
    r"""method\s*:\s*['"]([a-z][a-z0-9_.]+)['"].{0,400}?jsonrpc['"]?\s*:\s*['"]2\.0['"]""",
    re.DOTALL)


def strip_comments(s: str) -> str:
    s = re.sub(r'/\*.*?\*/', '', s, flags=re.DOTALL)
    s = re.sub(r'//[^\n]*', '', s)
    return s


def read(p: Path) -> str:
    return p.read_text(errors="replace") if p.exists() else ""


def is_source_js(p: Path) -> bool:
    n = p.name
    return not (n.endswith(".bundle.js") or n == "bundle.js" or n == "sw.js")


# 핸들러 -32602 가드키 추출 (① drift 감지)
METHOD_FN_RE = re.compile(
    r'g_hash_table_insert\s*\(\s*g_rpc_routes\s*,\s*"([a-z][a-z0-9_.]+)"\s*,\s*'
    r'(?:\(\s*gpointer\s*\)\s*)?&?\s*([A-Za-z_][A-Za-z0-9_]*)')
REQUIRE_MACRO_RE = re.compile(
    r'PCV_REQUIRE_PARAM(_OR)?\s*\(\s*[A-Za-z_][A-Za-z0-9_]*\s*,\s*'
    r'"([a-z_][a-z0-9_]*)"(?:\s*,\s*"([a-z_][a-z0-9_]*)")?')
HAS_MEMBER_GUARD_RE = re.compile(
    r'!\s*json_object_has_member\s*\(\s*[A-Za-z_][A-Za-z0-9_]*\s*,\s*"([a-z_][a-z0-9_]*)"\s*\)')

# 지배적 관용구 확장 (Task 6 Defect 2): 매크로/`!has_member` 외에
#   VAR = has_member(P,"K") ? get_*(P,"K") : NULL;  → 이후 !VAR / VAR==NULL 가드 → K 필수
#   VAR = P ? get_*_with_default(P,"K", NULL) : NULL; (container.snapshot.*)
#   VAR = get_*_member(P,"K");  (has_member 게이트 없는 직접 get — device.disk.attach)
# 판정: (a) NULL-기본값 pull 후보 추출 → (b) 널체크 존재 시 필수.
# 비-NULL 기본값(:0, :"egress", :"pcvpool")·널체크 없는 pull(ovn.acl.list switch)은 제외 → 선택.
GET_MEMBER_RE = re.compile(
    r'json_object_get_(?:string|int|boolean|double)_member(_with_default)?\s*\(\s*'
    r'[A-Za-z_]\w*\s*,\s*"([a-z_][a-z0-9_]*)"(?:\s*,\s*([A-Za-z0-9_]+)\s*)?\)')
BARE_GET_RE = re.compile(
    r'^json_object_get_(?:string|int|boolean|double)_member\s*\([^;]*\)$')


def _nulldefault_param_pulls(body: str) -> dict:
    """NULL-기본값(또는 게이트 없는 직접 get)으로 읽힌 param을 {var: key}로 수집."""
    out = {}
    for stmt in body.split(';'):
        am = re.search(r'([A-Za-z_]\w*)\s*=\s*(?!=)(.+)', stmt, re.DOTALL)
        if not am:
            continue
        var, rhs = am.group(1), am.group(2)
        gm = GET_MEMBER_RE.search(rhs)
        if not gm:
            continue
        with_default, key, default_arg = gm.group(1), gm.group(2), gm.group(3)
        rhs_s = rhs.strip()
        if with_default:
            cand = (default_arg == 'NULL')          # _with_default(P,"K", NULL)
        elif '?' in rhs and re.search(r':\s*NULL\b', rhs):
            cand = True                             # 삼항 NULL 기본값
        else:
            cand = bool(BARE_GET_RE.match(rhs_s))   # 게이트 없는 직접 get(=필수 가정)
        if cand:
            out[var] = key
    return out


def _var_null_checked(body: str, var: str) -> bool:
    v = re.escape(var)
    return bool(re.search(r'!\s*' + v + r'\b', body) or
                re.search(r'\b' + v + r'\s*==\s*NULL\b', body))


def extract_method_to_fn(dispatcher_src: str) -> dict:
    return {m: fn for m, fn in METHOD_FN_RE.findall(strip_comments(dispatcher_src))}


def extract_fn_body(src: str, fn: str) -> str:
    # 주석 내 불균형 중괄호가 depth 카운팅을 조기 종료시키므로 먼저 제거
    # (raw 소스에서 handle_network_qos_set 등이 1.2KB로 잘려 가드키를 놓치던 버그).
    src = strip_comments(src)
    m = re.search(r'\b' + re.escape(fn) + r'\s*\([^;{]*\)\s*\{', src)
    if not m:
        return ""
    start = m.end() - 1  # '{' 위치
    depth = 0
    for j in range(start, len(src)):
        if src[j] == '{':
            depth += 1
        elif src[j] == '}':
            depth -= 1
            if depth == 0:
                return src[start:j + 1]
    return src[start:]


def extract_handler_required(fn_body: str) -> set:
    body = strip_comments(fn_body)
    req = set()
    for is_or, k1, k2 in REQUIRE_MACRO_RE.findall(body):
        req.add(frozenset({k1, k2}) if (is_or and k2) else k1)
    for k in HAS_MEMBER_GUARD_RE.findall(body):
        req.add(k)
    # 지배적 관용구: NULL-기본값 pull + 널체크 가드
    for var, key in _nulldefault_param_pulls(body).items():
        if _var_null_checked(body, var):
            req.add(key)
    return req


# 소비 콜사이트별 전송키 추출 (② 커버리지 기반)
# var까지 포착: json_object_set_*_member(<var>, "key", ...)
SET_MEMBER_VAR_RE = re.compile(
    r'json_object_set_(?:string|int|boolean|double)_member\s*\(\s*'
    r'([A-Za-z_][A-Za-z0-9_]*)\s*,\s*"([a-z_][a-z0-9_]*)"')


def extract_consumer_sent(src: str, method_re, diag: list = None) -> dict:
    """send 콜의 실제 params 객체에 귀속된 전송키만 수집 (Task 6 Defect 1).

    함수 전체 union이 아니라, 각 `send("method", <var>, ...)`에 대해 그 <var>를
    할당한 `json_object_new()`(또는 같은 var의 직전 send)부터 send까지 구간의
    `json_object_set_*_member(<var>, "key")`만 귀속한다. 이로써 하나의 거대 함수에
    여러 method가 얽혀 있어도 키가 엉뚱한 method로 과결합되지 않는다.
    한 params 객체 수명 내부의 브랜치 union(안전 초집합)은 유지한다.
    귀속 실패(인라인 구성/할당 미발견) send는 diag에 기록(제공 시)하고 키 미귀속.
    """
    body = strip_comments(src)
    # 1) send 콜사이트: (pos, method, paramsvar) — method 리터럴 직후 2번째 인자 파싱
    sends = []
    for m in method_re.finditer(body):
        vm = re.match(r'\s*,\s*([A-Za-z_]\w*|NULL)', body[m.end():m.end() + 96])
        if not vm:
            if diag is not None:
                diag.append((m.group(1), 'inline-or-nonstd-params'))
            sends.append((m.start(), m.group(1), None))
            continue
        sends.append((m.start(), m.group(1), vm.group(1)))
    # 2) var별 json_object_new() 할당 위치
    allocs = {}
    for a in re.finditer(r'\b([A-Za-z_]\w*)\s*=\s*json_object_new\s*\(\s*\)', body):
        allocs.setdefault(a.group(1), []).append(a.start())
    # 3) var별 send 위치 (직전 send 경계용)
    sends_by_var = {}
    for pos, meth, var in sends:
        if var:
            sends_by_var.setdefault(var, []).append(pos)
    # 4) set_member: (pos, var, key)
    setmembers = [(sm.start(), sm.group(1), sm.group(2))
                  for sm in SET_MEMBER_VAR_RE.finditer(body)]
    out: dict = {}
    for pos, meth, var in sends:
        out.setdefault(meth, set())
        if not var or var == 'NULL':
            continue
        # 수명 시작 = max(직전 할당 < pos, 같은 var 직전 send < pos)
        start = -1
        for a in allocs.get(var, []):
            if a < pos and a > start:
                start = a
        for s in sends_by_var.get(var, []):
            if s < pos and s > start:
                start = s
        if start < 0:
            if diag is not None:
                diag.append((meth, f'no-alloc-for:{var}'))
            continue  # 할당 미발견 → 키 미귀속(과결합 재발 방지, 보수적)
        keys = {k for (p, vv, k) in setmembers if vv == var and start < p < pos}
        out[meth].update(keys)
    return out
