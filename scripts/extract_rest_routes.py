
"""
extract_rest_routes.py — rest_server.c에서 REST 라우팅을 추출

세그먼트 기반 디스패치 (resource/name/action/sub) 를 정적 분석으로 추적해
(METHOD, URL_PATTERN, RPC_METHOD) 튜플 목록을 생성한다.

출력: 정렬된 마크다운 표 행 (REST_ENDPOINTS.md에 추가용)
"""
import re
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parent.parent / "src/api/rest_server.c"

RE_RESOURCE = re.compile(r'g_strcmp0\(resource,\s*"([^"]+)"\)\s*==\s*0')
RE_NAME_LITERAL = re.compile(r'g_strcmp0\(name,\s*"([^"]+)"\)\s*==\s*0')
RE_ACTION   = re.compile(r'g_strcmp0\(action,\s*"([^"]+)"\)\s*==\s*0')
RE_SUB      = re.compile(r'g_strcmp0\(sub,\s*"([^"]+)"\)\s*==\s*0')
RE_NAME_EMPTY = re.compile(r'\*name\s*==\s*[\'"]\\?0[\'"]')
RE_NAME_NONEMPTY = re.compile(r'\*name\s*!=\s*[\'"]\\?0[\'"]')
RE_ACTION_EMPTY = re.compile(r'\*action\s*==\s*[\'"]\\?0[\'"]')
RE_METHOD = re.compile(r'g_strcmp0\(method,\s*"([A-Z]+)"\)\s*==\s*0')
RE_RPC = re.compile(r'_build_rpc(?:_name|_named)?\s*\(\s*"([a-z_.]+)"')

def extract():
    text = SRC.read_text()
    lines = text.splitlines()

    start = end = None
    for i, ln in enumerate(lines):
        if start is None and 'REST → RPC 라우팅 테이블' in ln:
            start = i
        if start and 'cleanup:' in ln:
            end = i
            break
    if not start:
        print("ERROR: routing table not found", file=sys.stderr); sys.exit(1)
    end = end or len(lines)

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
            cur_name_state = "nonempty"
            cur_action_state = "nonempty"
            cur_method = None; cur_sub = None
            continue

        if RE_ACTION_EMPTY.search(ln):
            cur_action_state = "empty"
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
            elif cur_name_state == "nonempty":
                parts.append("{name}")
                if cur_action_state != "empty" and cur_action:
                    parts.append(cur_action)
                    if cur_sub:
                        parts.append(cur_sub)
            url = "/" + "/".join(parts)

            method = cur_method or ("POST" if cur_action else "GET")
            routes.append((method, url, rpc, i + 1))

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

    print(f"# Extracted {len(uniq)} routes from rest_server.c\n")
    print("| HTTP | 경로 | RPC 메서드 | 소스 |")
    print("|------|------|-----------|------|")
    for method, url, rpc, line in uniq:
        print(f"| `{method}` | `{url}` | `{rpc}` | L{line} |")

if __name__ == "__main__":
    main()
