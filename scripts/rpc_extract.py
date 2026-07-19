
"""RPC 계약 게이트 공유 추출 원시함수.

check_rpc_consumers.py(method 존재)와 check_rpc_param_contract.py(param-key)가
공유한다. C/JS 소스에서 등록 route·소비 method 리터럴·param 키를 정적 추출한다.
stdlib(re, pathlib)만 사용.
"""
import re
from pathlib import Path

ROUTE_RE = re.compile(r'g_hash_table_insert\s*\(\s*g_rpc_routes\s*,\s*"([a-z][a-z0-9_.]+)"')

SPECIAL_RE = re.compile(r'g_strcmp0\s*\(\s*method\s*,\s*"([a-z][a-z0-9_.]+)"')

CLI_RE = re.compile(r'purectl_send_request\s*\(\s*"([a-z][a-z0-9_.]+)"')

SECREQ_RE = re.compile(r'security_request\s*\(\s*"([a-z][a-z0-9_.]+)"')
FE_RE_A = re.compile(
    r"""jsonrpc['"]?\s*:\s*['"]2\.0['"].{0,400}?method\s*:\s*['"]([a-z][a-z0-9_.]+)['"]""",
    re.DOTALL)
FE_RE_B = re.compile(
    r"""method\s*:\s*['"]([a-z][a-z0-9_.]+)['"].{0,400}?jsonrpc['"]?\s*:\s*['"]2\.0['"]""",
    re.DOTALL)

REST_RE = re.compile(r'_build_rpc(?:_name)?\s*\(\s*"([a-z][a-z0-9_.]+)"')

FE_HELP_RE = re.compile(r"""(?<![A-Za-z0-9])_?(?:rpc|RPC)\s*\(\s*['"]([a-z][a-z0-9_.]+)['"]""")

def strip_comments(s: str) -> str:
    s = re.sub(r'/\*.*?\*/', '', s, flags=re.DOTALL)
    s = re.sub(r'//[^\n]*', '', s)
    return s

def read(p: Path) -> str:
    return p.read_text(errors="replace") if p.exists() else ""

def is_source_js(p: Path) -> bool:
    n = p.name
    return not (n.endswith(".bundle.js") or n == "bundle.js" or n == "sw.js")

METHOD_FN_RE = re.compile(
    r'g_hash_table_insert\s*\(\s*g_rpc_routes\s*,\s*"([a-z][a-z0-9_.]+)"\s*,\s*'
    r'(?:\(\s*gpointer\s*\)\s*)?&?\s*([A-Za-z_][A-Za-z0-9_]*)')
REQUIRE_MACRO_RE = re.compile(
    r'PCV_REQUIRE_PARAM(_OR)?\s*\(\s*[A-Za-z_][A-Za-z0-9_]*\s*,\s*'
    r'"([a-z_][a-z0-9_]*)"(?:\s*,\s*"([a-z_][a-z0-9_]*)")?')
HAS_MEMBER_GUARD_RE = re.compile(
    r'!\s*json_object_has_member\s*\(\s*[A-Za-z_][A-Za-z0-9_]*\s*,\s*"([a-z_][a-z0-9_]*)"\s*\)')

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
            cand = (default_arg == 'NULL')
        elif '?' in rhs and re.search(r':\s*NULL\b', rhs):
            cand = True
        else:
            cand = bool(BARE_GET_RE.match(rhs_s))
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

    src = strip_comments(src)
    m = re.search(r'\b' + re.escape(fn) + r'\s*\([^;{]*\)\s*\{', src)
    if not m:
        return ""
    start = m.end() - 1
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

    for var, key in _nulldefault_param_pulls(body).items():
        if _var_null_checked(body, var):
            req.add(key)
    return req

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

    sends = []
    for m in method_re.finditer(body):
        vm = re.match(r'\s*,\s*([A-Za-z_]\w*|NULL)', body[m.end():m.end() + 96])
        if not vm:
            if diag is not None:
                diag.append((m.group(1), 'inline-or-nonstd-params'))
            sends.append((m.start(), m.group(1), None))
            continue
        sends.append((m.start(), m.group(1), vm.group(1)))

    allocs = {}
    for a in re.finditer(r'\b([A-Za-z_]\w*)\s*=\s*json_object_new\s*\(\s*\)', body):
        allocs.setdefault(a.group(1), []).append(a.start())

    sends_by_var = {}
    for pos, meth, var in sends:
        if var:
            sends_by_var.setdefault(var, []).append(pos)

    setmembers = [(sm.start(), sm.group(1), sm.group(2))
                  for sm in SET_MEMBER_VAR_RE.finditer(body)]
    out: dict = {}
    for pos, meth, var in sends:
        out.setdefault(meth, set())
        if not var or var == 'NULL':
            continue

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
            continue
        keys = {k for (p, vv, k) in setmembers if vv == var and start < p < pos}
        out[meth].update(keys)
    return out

def extract_rest_methods(src: str) -> set:
    """rest_server.c의 REST→RPC 브릿지 소비 메서드. _build_rpc 인자는 항상 실제 메서드명이라
    dotless(예: get_vnc_info)도 포함한다 — dot 필터 시 dotless 등록 메서드가 거짓 고아가 된다."""
    return set(REST_RE.findall(strip_comments(src)))

def extract_fe_helper(src: str) -> set:
    """ui js의 rpc()/EP.RPC() 헬퍼 소비 (제네릭 /rpc passthrough)."""
    return {m for m in FE_HELP_RE.findall(strip_comments(src)) if "." in m}

def extract_grpc_methods(src: str, registered: set) -> set:
    """grpc_server.c의 dotted 리터럴 중 등록 route에 속하는 것만 (제네릭 UDS 재사용이라 명시 매핑만 정적 가시).

    [한계 — M-5] grpc_server.c 의 임의 dotted 문자열이 등록 메서드명과 우연히
    일치하면 '소비'로 계산돼 실제 고아를 숨길 수 있다(거짓 소비). 현재
    grpc_server.c 에는 메서드 dotted 리터럴이 없어(제네릭 UDS 재사용) 이 함수는
    빈 집합을 반환하며 위험은 이론적이다. grpc 가 명시 메서드 리터럴을 갖게 되면
    매칭을 소비 컨텍스트(디스패치 호출 인근)로 좁혀야 한다.
    """
    lits = re.findall(r'"([a-z][a-z0-9_]+(?:\.[a-z0-9_]+)+)"', strip_comments(src))
    return {m for m in lits if m in registered}

def extract_test_consumed(tests_dir: Path, registered: set) -> set:
    """tests/ 파일이 참조하는 등록 메서드 (production 소비 아님 — test 커버리지 라벨용)."""
    out = set()
    if not tests_dir.exists():
        return out
    for p in tests_dir.rglob("*"):
        if not p.is_file():
            continue
        try:
            t = p.read_text(errors="replace")
        except Exception:
            continue
        for m in re.findall(r"""['"]([a-z][a-z0-9_]+(?:\.[a-z0-9_]+)+)['"]""", t):
            if m in registered:
                out.add(m)
    return out
