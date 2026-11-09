#!/usr/bin/env python3
                          
                                                                  
                                                 
                                                                                                    
 
                      
                                                          
                         

                                                                          
                                                        
                        
   
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "split"))
import split_target

                                                   
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
                                                                     
REST_RE = re.compile(r'\bpcv_rest_build_rpc(?:_name)?\s*\(\s*"([a-z][a-z0-9_.]+)"')
                                                                       
                                                                
FE_HELP_RE = re.compile(r"""(?<![A-Za-z0-9])_?(?:rpc|RPC)\s*\(\s*['"]([a-z][a-z0-9_.]+)['"]""")


def strip_comments(s: str) -> str:
    s = re.sub(r'/\*.*?\*/', '', s, flags=re.DOTALL)
    s = re.sub(r'//[^\n]*', '', s)
    return s


def read(p: Path) -> str:
    return p.read_text(errors="replace") if p.exists() else ""


def is_source_js(p: Path) -> bool:
    n = p.name
                                                    
                                                  
                                                                     
                                                                 
                                                      
                                               
    if n == "help.js":
        return False
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
    r'json_object_set_(?:string|int|boolean|double|array|object|null)_member\s*\(\s*'
    r'([A-Za-z_][A-Za-z0-9_]*)\s*,\s*"([a-z_][a-z0-9_]*)"')


def extract_consumer_sent(src: str, method_re, diag: list = None) -> dict:
                                                           

                                                                 
                                                            
                                                                  
                                               
                                               
                                                      
       
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
                                                                      
                                                                           
    return set(REST_RE.findall(strip_comments(src)))


def extract_fe_helper(src: str) -> set:
                                                             
    return {m for m in FE_HELP_RE.findall(strip_comments(src)) if "." in m}


def extract_grpc_methods(src: str, registered: set) -> set:
                                                                                 

                                                          
                                            
                                                          
                                                  
                                    
       
    lits = re.findall(r'"([a-z][a-z0-9_]+(?:\.[a-z0-9_]+)+)"', strip_comments(src))
    return {m for m in lits if m in registered}


def extract_test_consumed(tests_dir: Path, registered: set) -> set:
                                                                    
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

DISPATCHER_ROUTES_REL = "src/api/dispatcher.c"
DISPATCHER_SPLIT_FILES: tuple[str, ...] = split_target.load("dispatcher").split_files

def split_files(config: str = "dispatcher") -> tuple[str, ...]:

    return split_target.load(config).split_files
MIN_ROUTES = 300
RENAME_MAP_REL = "scripts/split/dispatcher_rename_map.tsv"
POLICY_DEF_RE = re.compile(r'\bg_method_policies\s*\[\s*\]\s*=')
_REPO_ROOT = Path(__file__).resolve().parent.parent

def dispatcher_family(root: Path) -> list:

    paths = [root / DISPATCHER_ROUTES_REL] + [root / rel for rel in DISPATCHER_SPLIT_FILES]
    missing = [str(p) for p in paths if not p.exists()]
    if missing and Path(root).resolve() == _REPO_ROOT:
        raise FileNotFoundError("dispatcher 계열 파일 없음: " + ", ".join(missing))
    return [p for p in paths if p.exists()]

def dispatcher_family_text(root: Path) -> str:

    return "\n".join(f"/* ==== {p.relative_to(root)} ==== */\n" + read(p) for p in dispatcher_family(root))

def dispatcher_route_file(root: Path) -> Path:

    p = root / DISPATCHER_ROUTES_REL
    n = len(ROUTE_RE.findall(strip_comments(read(p))))
    if n < MIN_ROUTES:
        raise ValueError(f"{p}: 라우트 등록 {n}개 < 하한 {MIN_ROUTES} — 라우트 원천을 못 읽었다")
    return p

def dispatcher_policy_file(root: Path) -> Path:

    hits = [p for p in dispatcher_family(root) if POLICY_DEF_RE.search(strip_comments(read(p)))]
    if len(hits) != 1:
        raise ValueError(f"g_method_policies 정의 파일이 하나가 아니다: {[str(h) for h in hits]}")
    return hits[0]

def split_renames(root: Path = _REPO_ROOT) -> dict:

    p = root / RENAME_MAP_REL
    if not p.exists():
        p = _REPO_ROOT / RENAME_MAP_REL
    out = {}
    if p.exists():
        for line in p.read_text().splitlines():
            if line.strip() and not line.startswith("#"):
                old, new = line.split("\t")
                out[old] = new
    return out

def current_fn_name(root: Path, name: str, config: str = "dispatcher") -> str:

    if config == "rest_server":
        return rest_fn_name_in(rest_family_text(root), name, root)
    text = strip_comments(dispatcher_family_text(root))
    if re.search(r'\b' + re.escape(name) + r'\s*\([^;{]*\)\s*\{', text):
        return name
    return split_renames(root).get(name, name)

VM_SPLIT_COMPLETE = True

def vm_family_rel(config: str) -> list[str]:

    t = split_target.load(config)
    return [t.source, *t.split_files]

def vm_family(config: str, root: Path = _REPO_ROOT) -> list[Path]:

    root = Path(root)
    source, *split = vm_family_rel(config)
    if not (root / source).exists():
        raise FileNotFoundError(root / source)
    files = [root / source]
    for rel in split:
        p = root / rel
        if p.exists():
            files.append(p)
        elif VM_SPLIT_COMPLETE and root.resolve() == _REPO_ROOT:
            raise FileNotFoundError(p)
    return files

def vm_family_text(config: str, root: Path = _REPO_ROOT) -> str:

    root = Path(root)
    text = "".join(f"/* ==== {p.relative_to(root).as_posix()} ==== */\n{p.read_text()}" for p in vm_family(config, root))
    floor = split_target.load(config).extra.get("min_functions", 1)
    if root.resolve() == _REPO_ROOT:
        import c_blocks
        found = len(c_blocks.find_functions(text))
        if found < floor:
            raise ValueError(f"VM 계열 {config}: 함수 {found}개 < 하한 {floor} — 파일 묶음을 못 읽었다")
    return text

REST_ROUTES_REL = "src/api/rest_server.c"

REST_SPLIT_COMPLETE = True

def rest_family(root: Path = _REPO_ROOT) -> list[Path]:

    root = Path(root)
    files = [root / REST_ROUTES_REL]
    for rel in split_files("rest_server"):
        p = root / rel
        if p.exists():
            files.append(p)
        elif REST_SPLIT_COMPLETE and root.resolve() == _REPO_ROOT:
            raise FileNotFoundError(p)
    if not files[0].exists():
        raise FileNotFoundError(files[0])
    return files

def rest_family_text(root: Path = _REPO_ROOT) -> str:

    root = Path(root)
    return "".join(f"/* ==== {p.relative_to(root).as_posix()} ==== */\n{p.read_text()}" for p in rest_family(root))

_DEF_RE_TMPL = r"^(?:[A-Za-z_][\w \t\*]*?[ \t\*])?{name}\s*\([^;{{]*\)\s*\{{"

def _matching_brace(text: str, open_at: int) -> int:

    depth, j, n = 0, open_at, len(text)
    while j < n:
        c = text[j]
        if c == "/" and text.startswith("/*", j):
            k = text.find("*/", j + 2)
            j = n if k < 0 else k + 2
            continue
        if c == "/" and text.startswith("//", j):
            k = text.find("\n", j)
            j = n if k < 0 else k
            continue
        if c in "\"'":
            k = j + 1
            while k < n and text[k] != c and text[k] != "\n":
                k += 2 if text[k] == "\\" else 1
            j = k + 1
            continue
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return j
        j += 1
    return -1

def function_body_in(text: str, name: str) -> str | None:

    m = re.search(_DEF_RE_TMPL.format(name=re.escape(name)), text, re.M)
    if not m:
        return None
    i = m.end() - 1
    j = _matching_brace(text, i)
    return None if j < 0 else text[i:j + 1]

def rest_renames(root: Path = _REPO_ROOT) -> dict:

    rel = split_target.load("rest_server").rename_map
    p = Path(root) / rel
    if not p.exists():
        p = _REPO_ROOT / rel
    out = {}
    for line in p.read_text().splitlines():
        if line.strip() and not line.startswith("#"):
            old, new = line.split("\t")
            out[old] = new
    return out

def rest_fn_name_in(text: str, name: str, root: Path = _REPO_ROOT) -> str:

    if function_body_in(text, name) is not None:
        return name
    return rest_renames(root).get(name, name)

REQUEST_FN = "_on_request"

def rest_block_functions(root: Path = _REPO_ROOT) -> tuple[str, ...]:

    rel = split_target.load("rest_server").blocks
    if not rel:
        return ()
    p = Path(root) / rel
    if not p.exists():
        p = _REPO_ROOT / rel
    header, out = None, []
    for line in p.read_text().splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        cols = line.split("\t")
        if header is None:
            header = cols
            continue
        row = dict(zip(header, cols))
        if row.get("kind") != "keep" and row.get("function", "-") not in ("-", ""):
            out.append(row["function"])
    return tuple(out)

def _definition_span(text: str, name: str) -> tuple[int, int] | None:

    m = re.search(_DEF_RE_TMPL.format(name=re.escape(name)), text, re.M)
    if not m:
        return None
    j = _matching_brace(text, m.end() - 1)
    return None if j < 0 else (m.end() - 1, j + 1)

def rest_request_spans(text: str, functions=None, var: str | None = None) -> list[tuple[int, int]] | None:

    fns = rest_block_functions() if functions is None else tuple(functions)
    if var is None:
        var = split_target.load("rest_server").context_var or "rq"
    span = _definition_span(text, REQUEST_FN)
    if span is None:
        return None
    lo, hi = span
    if not fns:
        return [(lo, hi)]
    call = re.compile(r"\b(" + "|".join(map(re.escape, fns)) + r")\s*\(\s*" + re.escape(var) + r"\s*\)")
    out, pos = [], lo
    for c in call.finditer(text, lo, hi):
        d = _definition_span(text, c.group(1))
        if d is None:
            return None
        out += [(pos, c.start()), d]
        pos = c.end()
    out.append((pos, hi))
    return out

def rest_request_scope(text: str, functions=None, var: str | None = None) -> str | None:

    spans = rest_request_spans(text, functions, var)
    return None if spans is None else "".join(text[a:b] for a, b in spans)

def rest_scope_origin(spans: list[tuple[int, int]], pos: int) -> int:

    for a, b in spans:
        if pos < b - a:
            return a + pos
        pos -= b - a
    return spans[-1][1] if spans else 0

_FAMILY_MARK_RE = re.compile(r"^/\* ==== (.+?) ==== \*/\n", re.M)

def rest_text_location(text: str, pos: int) -> str:

    mark = None
    for mark in _FAMILY_MARK_RE.finditer(text, 0, pos + 1):
        pass
    if mark is None or mark.start() > pos:
        return f"L{text.count(chr(10), 0, pos) + 1}"
    return f"{mark.group(1)}:L{text.count(chr(10), mark.end(), pos) + 1}"
