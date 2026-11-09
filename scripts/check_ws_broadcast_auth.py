#!/usr/bin/env python3
                          
                                                                       
                                                                             
                                                                  
                                      
                                                                              
                      
                                                                 
                                                
 
                      
                                                       
                                                        
                                                 
                                                        

   
                                                                            
                                                                             
                                                                        
                                                                      
                                                                           
                                                            
                                                                                    
                                                                                            
                                                                                               
                                                                   
                        
                                                                                        
                                                                             
   
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WS_SERVER_C = ROOT / "src" / "api" / "ws_server.c"
SRC_ROOT = ROOT / "src"
SEND_HELPER_REL = "api/ws_send.h"                                         
FANOUT_BODY = "_ws_broadcast_fanout"

FUNC_HEAD_RE = re.compile(
    r'^(?:static\s+)?(?:inline\s+)?(?:[A-Za-z_][\w\s\*]*?\s+\**)([A-Za-z_]\w*)\s*\(([^;{}]*)\)\s*\{',
    re.MULTILINE)
RAW_SEND_RE = re.compile(r'\bsoup_websocket_connection_send_text\s*\(')
SEND_RE = re.compile(r'\b(?:soup_websocket_connection_send_text|pcv_ws_send_text_if_open)\s*\(')
CLIENT_ITER_RE = re.compile(r'g_ptr_array_index\s*\(\s*G\.clients\s*,')
DISPATCH_RE = re.compile(r'\bpcv_ws_dispatch_on_owner\s*\([^;]*\b' + FANOUT_BODY + r'\b')
OWNER_INIT_RE = re.compile(
    r'\bGMainContext\s*\*\s*owner\s*=\s*g_main_context_ref_thread_default\s*\(\s*\)\s*;')
CTX_ASSIGN_RE = re.compile(r'\bG\.ctx\s*=\s*owner\s*;')
OWNER_REF_RE = re.compile(r'\bGMainContext\s*\*\s*owner\s*=\s*_ws_owner_ref\s*\(\s*\)\s*;')
OWNER_DISPATCH_RE = re.compile(r'\bpcv_ws_dispatch_on_owner\s*\(\s*owner\s*,')
MT_SAME_PATH_RE = re.compile(r'\b_broadcast_job_complete\s*\(')
OWNER_FROM_CTX_RE = re.compile(r'\bg_main_context_ref\s*\(\s*G\.ctx\s*\)')
DEFAULT_CTX_RE = re.compile(r'\bg_main_context_default\s*\(')
DRAIN_INVOKE_RE = re.compile(r'\bpcv_drain_invoke\s*\(')
OPEN_CHECK_RE = re.compile(
    r'soup_websocket_connection_get_state\s*\([^)]*\)\s*!=\s*SOUP_WEBSOCKET_STATE_OPEN')
GUARD_RE = re.compile(
    r'if\s*\(\s*!\s*_ws_conn_is_authed\s*\(\s*conn\s*\)\s*\)\s*'
    r'(?:\{[^{}]*?\bcontinue\s*;[^{}]*\}|[^;{}]*?\bcontinue\s*;)',
    re.DOTALL)


def strip_comments(text: str) -> str:
                                            
    def blank(match: re.Match) -> str:
        return re.sub(r'[^\n]', ' ', match.group(0))
    text = re.sub(r'/\*.*?\*/', blank, text, flags=re.DOTALL)
    return re.sub(r'//[^\n]*', blank, text)


def strip_comments_and_strings(text: str) -> str:
                                                                  
    def blank(match: re.Match) -> str:
        return re.sub(r'[^\n]', ' ', match.group(0))
    pattern = r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\''
    return re.sub(pattern, blank, text, flags=re.DOTALL)


def function_bodies(text: str) -> dict[str, tuple[int, str]]:
                                                                 
    bodies: dict[str, tuple[int, str]] = {}
    for match in FUNC_HEAD_RE.finditer(text):
        name = match.group(1)
        if name in {"if", "for", "while", "switch", "return", "sizeof"}:
            continue
        start = match.end() - 1
        depth = 0
        for pos in range(start, len(text)):
            if text[pos] == '{':
                depth += 1
            elif text[pos] == '}':
                depth -= 1
                if depth == 0:
                    bodies[name] = (start, text[start:pos + 1])
                    break
    return bodies


def check(text: str) -> list[str]:
                                       
    code = strip_comments(text)
    bodies = function_bodies(code)
    failures: list[str] = []

    fanouts = [name for name, (_, body) in bodies.items()
               if CLIENT_ITER_RE.search(body) and SEND_RE.search(body)]
    if FANOUT_BODY not in fanouts:
        failures.append(f"R2: 방송 본문 {FANOUT_BODY}() fan-out 루프를 찾지 못함 — 구조 변경 시 게이트를 갱신하라")
    broadcast = bodies.get("pcv_ws_broadcast")
    if not broadcast:
        failures.append("R2: pcv_ws_broadcast() 정의가 없음")
    else:
        _, body = broadcast
        if CLIENT_ITER_RE.search(body) or SEND_RE.search(body):
            failures.append("R2: pcv_ws_broadcast()가 G.clients를 직접 돌거나 보낸다 — "
                            "fan-out은 소유 컨텍스트에서만(pcv_ws_dispatch_on_owner)")
        if not DISPATCH_RE.search(body):
            failures.append(f"R2: pcv_ws_broadcast()가 pcv_ws_dispatch_on_owner(…, {FANOUT_BODY}, …)로 "
                            "방송 본문을 소유 컨텍스트에 넘기지 않음")
        if not OWNER_REF_RE.search(body) or not OWNER_DISPATCH_RE.search(body) \
                or DEFAULT_CTX_RE.search(body):
            failures.append("R2: pcv_ws_broadcast()의 소유 컨텍스트가 _ws_owner_ref()(=G.ctx)에서 오지 않음 — "
                            "방송 목적지는 WS 소유 컨텍스트여야 한다")
    mt = bodies.get("pcv_ws_broadcast_job_complete_mt")
    if not mt:
        failures.append("R2: pcv_ws_broadcast_job_complete_mt() 정의가 없음")
    elif not MT_SAME_PATH_RE.search(mt[1]) or DEFAULT_CTX_RE.search(mt[1]):
        failures.append("R2: pcv_ws_broadcast_job_complete_mt()가 _broadcast_job_complete()(=pcv_ws_broadcast 경로)를 "
                        "부르지 않음 — 방송 경로를 따로 두지 않는다")
    owner_ref = bodies.get("_ws_owner_ref")
    if not owner_ref:
        failures.append("R2: _ws_owner_ref() 정의가 없음")
    elif not OWNER_FROM_CTX_RE.search(owner_ref[1]) or DEFAULT_CTX_RE.search(owner_ref[1]):
        failures.append("R2: _ws_owner_ref()가 G.ctx의 참조를 돌려주지 않음")
    init = bodies.get("pcv_ws_server_init")
    if not init:
        failures.append("R2: pcv_ws_server_init() 정의가 없음")
    elif not OWNER_INIT_RE.search(init[1]) or not CTX_ASSIGN_RE.search(init[1]) \
            or DEFAULT_CTX_RE.search(init[1]):
        failures.append("R2: pcv_ws_server_init()의 G.ctx가 g_main_context_ref_thread_default()에서 오지 않음 — "
                        "WS 소유 컨텍스트는 init 시점의 thread-default(rest_ctx)여야 한다")
    for match in DRAIN_INVOKE_RE.finditer(code):
        line = code.count('\n', 0, match.start()) + 1
        failures.append(f"R2: line {line}: ws_server.c의 pcv_drain_invoke( — WS 전송 경로는 "
                        "pcv_ws_dispatch_on_owner로 소유 컨텍스트에만 넘긴다")

    for name in fanouts:
        offset, body = bodies[name]
        iter_at = CLIENT_ITER_RE.search(body).start()
        send = SEND_RE.search(body, iter_at)
        if not send:
            continue
        guard = GUARD_RE.search(body, iter_at, send.start())
        if not guard:
            line = code.count('\n', 0, offset + send.start()) + 1
            failures.append(
                f"R1: {name}() line {line}: G.clients fan-out 전송 앞에 "
                f"`if (!_ws_conn_is_authed(conn)) continue;` 가드가 없음")

    helper = bodies.get("_ws_conn_is_authed")
    if not helper:
        failures.append("R3: _ws_conn_is_authed() 정의가 없음")
    else:
        _, body = helper
        if '"pcv-ws-authed"' not in body or not re.search(r'==\s*1\b', body):
            failures.append('R3: _ws_conn_is_authed()가 "pcv-ws-authed" == 1 비교로 판정하지 않음')
    return failures


def check_send_sites(src_root: Path) -> list[str]:
                                       
    failures: list[str] = []
    helper_path = src_root / SEND_HELPER_REL
    for path in sorted([*src_root.rglob("*.c"), *src_root.rglob("*.h")]):
        rel = path.relative_to(src_root).as_posix()
        code = strip_comments_and_strings(path.read_text(errors="replace"))
        if rel == SEND_HELPER_REL:
            continue
        for match in RAW_SEND_RE.finditer(code):
            line = code.count('\n', 0, match.start()) + 1
            failures.append(f"R4: src/{rel} line {line}: soup_websocket_connection_send_text( 직접 호출 — "
                            "src/api/ws_send.h의 pcv_ws_send_text_if_open을 쓴다")
    if not helper_path.is_file():
        failures.append(f"R4: src/{SEND_HELPER_REL} 없음 — 텍스트 전송 헬퍼가 사라짐")
        return failures
    helper_code = strip_comments_and_strings(helper_path.read_text(errors="replace"))
    body = function_bodies(helper_code).get("pcv_ws_send_text_if_open")
    if not body:
        failures.append(f"R4: src/{SEND_HELPER_REL}에 pcv_ws_send_text_if_open() 정의가 없음")
        return failures
    _, text = body
    send = RAW_SEND_RE.search(text)
    check_at = OPEN_CHECK_RE.search(text)
    if not send:
        failures.append("R4: pcv_ws_send_text_if_open()이 soup_websocket_connection_send_text를 부르지 않음")
    elif not check_at or check_at.start() > send.start():
        failures.append("R4: pcv_ws_send_text_if_open()이 전송 앞에서 "
                        "SOUP_WEBSOCKET_STATE_OPEN을 확인하지 않음")
    return failures


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ws_server", nargs="?", type=Path, default=WS_SERVER_C,
                        help="검사할 ws_server.c (기본: 트리의 정본)")
    parser.add_argument("--src-root", type=Path, default=SRC_ROOT,
                        help="R4가 훑을 src 트리 (기본: 트리의 src/)")
    args = parser.parse_args(sys.argv[1:] if argv is None else argv)
    try:
        text = args.ws_server.read_text(errors="replace")
        if not args.src_root.is_dir():
            raise OSError(f"{args.src_root}는 디렉터리가 아님")
        failures = check(text) + check_send_sites(args.src_root)
    except OSError as exc:
        print(f"ERROR: 읽기 실패: {exc}", file=sys.stderr)
        return 2
    if failures:
        print("[FAIL] WebSocket fan-out 미인증 연결 차단·전송 경로 계약 위반:")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print("[PASS] WebSocket fan-out은 인증된 연결(pcv-ws-authed == 1)에만, "
          "텍스트 전송은 ws_send.h의 OPEN 확인 헬퍼로만, 방송은 소유 컨텍스트에서")
    return 0


if __name__ == "__main__":
    sys.exit(main())
