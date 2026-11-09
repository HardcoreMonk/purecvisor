#!/usr/bin/env python3
                          
                                                                 
                                               
                                                                                         
 
                      
                                                         
   
                                                     

                                                     
                                              

                                         
   
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "split"))
import split_target

ROOT = Path(__file__).resolve().parent.parent
ROUTE_FILE_REL = "src/api/rest_server.c"
BANNER = "REST → RPC 라우팅 테이블"

MIN_ROUTES = 213

    
RE_RESOURCE = re.compile(r'g_strcmp0\(resource,\s*"([^"]+)"\)\s*==\s*0')
RE_NAME_LITERAL = re.compile(r'g_strcmp0\(name,\s*"([^"]+)"\)\s*==\s*0')
RE_ACTION   = re.compile(r'g_strcmp0\(action,\s*"([^"]+)"\)\s*==\s*0')
RE_SUB      = re.compile(r'g_strcmp0\(sub,\s*"([^"]+)"\)\s*==\s*0')
RE_NAME_EMPTY = re.compile(r'\*name\s*==\s*[\'"]\\?0[\'"]')
RE_NAME_NONEMPTY = re.compile(r'\*name\s*!=\s*[\'"]\\?0[\'"]')
RE_ACTION_EMPTY = re.compile(r'\*action\s*==\s*[\'"]\\?0[\'"]')
RE_METHOD = re.compile(r'g_strcmp0\(method,\s*"([A-Z]+)"\)\s*==\s*0')

                                                                   
RE_RPC = re.compile(r'(?<![A-Za-z0-9_])(?:pcv_rest)?_build_rpc(?:_name)?\s*\(\s*"([a-z_.]+)"')

def scan_files(root: Path | None = None, chain_files=None) -> list[tuple[Path, bool]]:

    root = Path(ROOT if root is None else root)
    if chain_files is None:
        chain_files = split_target.load("rest_server").chain_files
    files = [(root / ROUTE_FILE_REL, True)]
    files += [(root / rel, False) for rel in chain_files if (root / rel).exists()]
    return files


def _table_range(lines: list[str], until_cleanup: bool):

    start = next((i for i, ln in enumerate(lines) if BANNER in ln), None)
    if start is None:
        start = next((i for i, ln in enumerate(lines) if RE_RESOURCE.search(ln)), None)
    if start is None:
        return None
    end = len(lines)
    if until_cleanup:
        end = next((i for i in range(start, len(lines)) if 'cleanup:' in lines[i]), end)
    return start, end

def extract(root: Path | None = None, chain_files=None):

    routes = []
    found = False
    for path, until_cleanup in scan_files(root, chain_files):
        lines = path.read_text().splitlines()
        rng = _table_range(lines, until_cleanup)
        if rng is None:
            continue
        found = True
        routes += _extract_lines(lines, *rng, path.name)
    if not found:
        print("ERROR: routing table not found", file=sys.stderr); sys.exit(1)
    return routes

def _extract_lines(lines: list[str], start: int, end: int, fname: str):
    routes = []
    cur_resource = None
    cur_action = None
    cur_name_state = None                               
    cur_name_literal = None                     
    cur_action_state = None                  
    cur_method = None
    cur_sub = None

                                                                       
                                      
    for i in range(start, end):
        ln = lines[i]

        m = RE_RESOURCE.search(ln)
        if m:
            cur_resource = m.group(1)
            cur_action = None; cur_name_state = None; cur_name_literal = None
            cur_action_state = None; cur_method = None; cur_sub = None
            continue

        m = RE_NAME_LITERAL.search(ln)
        if m:
            cur_name_literal = m.group(1)
            cur_name_state = "literal"
            cur_action = None; cur_action_state = None; cur_method = None; cur_sub = None
                                                      

        if RE_NAME_EMPTY.search(ln):
            cur_name_state = "empty"; cur_name_literal = None
            cur_action = None; cur_action_state = None; cur_method = None
            continue
        if RE_NAME_NONEMPTY.search(ln):
            cur_name_state = "nonempty"; cur_name_literal = None
            cur_action = None; cur_action_state = None; cur_method = None
            continue

        m = RE_ACTION.search(ln)
        if m:
            cur_action = m.group(1)
                                                        
                                                                     
            if cur_name_state != "literal":
                cur_name_state = "nonempty"
            cur_action_state = "nonempty"

            m_inline = RE_METHOD.search(ln)
            cur_method = m_inline.group(1) if m_inline else None; cur_sub = None
            continue

        if RE_ACTION_EMPTY.search(ln):
            cur_action_state = "empty"
            if cur_name_state != "literal":
                cur_name_state = "nonempty"
            cur_method = None
            continue

        m = RE_SUB.search(ln)
        if m:
            cur_sub = m.group(1); cur_method = None; continue

        m = RE_METHOD.search(ln)
        if m:
            cur_method = m.group(1)
                                                      

        m = RE_RPC.search(ln)
        if m and cur_resource:
            rpc = m.group(1)
                       
            parts = [cur_resource]
            if cur_name_state == "literal" and cur_name_literal:
                parts.append(cur_name_literal)
                if cur_action_state != "empty" and cur_action:
                    parts.append(cur_action)
                    if cur_sub:
                        parts.append(cur_sub)
            elif cur_name_state == "nonempty":
                parts.append("{name}")
                if cur_action_state != "empty" and cur_action:
                    parts.append(cur_action)
                    if cur_sub:
                        parts.append(cur_sub)
            url = "/" + "/".join(parts)
                                                         
            method = cur_method or ("POST" if cur_action else "GET")
            routes.append((method, url, rpc, f"{fname}:L{i + 1}"))

    return routes

def main():
    routes = extract()
                
    seen = set()
    uniq = []
    for r in routes:
        key = (r[0], r[1], r[2])
        if key in seen: continue
        seen.add(key); uniq.append(r)
    uniq.sort(key=lambda x: (x[1], x[0]))
    names = ", ".join(p.name for p, _ in scan_files())
    if len(uniq) < MIN_ROUTES:
        print(f"ERROR: {names}에서 고유 route {len(uniq)}개 < 하한 {MIN_ROUTES} — "
              "라우팅 표를 못 읽었다(경로·표지·사슬 조각 파일 변경?)", file=sys.stderr)
        sys.exit(1)

    print(f"# Extracted {len(uniq)} routes from {names}\n")
    print("| HTTP | 경로 | RPC 메서드 | 소스 |")
    print("|------|------|-----------|------|")
    for method, url, rpc, loc in uniq:
        print(f"| `{method}` | `{url}` | `{rpc}` | {loc} |")

if __name__ == "__main__":
    main()
