#!/usr/bin/env python3
                          
                                                           
                                                                                      
                                                                                                                            
 
                      
                                                                                                                    
                                                                   

                                                                      

                                           

                                                                    
                                                                
                                              

                                                                  
                                                    
                                                       
                                                              
                       

                                                    
   
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TARGET_REL = "src/api/grpc_server.c"
TARGET = ROOT / TARGET_REL

INJECT_TOKENS = ["_pcv_caller_role", "_pcv_caller_sub"]
START_FN = "pcv_grpc_server_start"
                                                                   
EMPTY_TOKEN_IF_RE = re.compile(r'if\s*\(\s*!\s*G_grpc_auth_token')
RETURN_TAIL_RE = re.compile(r'return\s*;\s*$')


def strip_code(text: str) -> str:
                                                     
                                                     
    out = []
    i, n = 0, len(text)
    in_block = in_line = False
    while i < n:
        ch = text[i]
        if in_line:
            out.append('\n' if ch == '\n' else ' ')
            if ch == '\n':
                in_line = False
            i += 1
            continue
        if in_block:
            if ch == '*' and i + 1 < n and text[i + 1] == '/':
                out.append('  ')
                i += 2
                in_block = False
            else:
                out.append('\n' if ch == '\n' else ' ')
                i += 1
            continue
        if ch == '/' and i + 1 < n and text[i + 1] == '*':
            in_block = True
            out.append('  ')
            i += 2
            continue
        if ch == '/' and i + 1 < n and text[i + 1] == '/':
            in_line = True
            out.append('  ')
            i += 2
            continue
        if ch == '"' or ch == "'":
            quote = ch
            out.append(' ')
            i += 1
            while i < n:
                c2 = text[i]
                if c2 == '\\' and i + 1 < n:
                    out.append(' \n' if text[i + 1] == '\n' else '  ')
                    i += 2
                    continue
                if c2 == quote:
                    out.append(' ')
                    i += 1
                    break
                out.append('\n' if c2 == '\n' else ' ')
                i += 1
            continue
        out.append(ch)
        i += 1
    return ''.join(out)


def strip_comments(text: str) -> str:
                                                 
                                            
    out = []
    i, n = 0, len(text)
    in_block = in_line = False
    in_str = None
    while i < n:
        ch = text[i]
        if in_line:
            out.append('\n' if ch == '\n' else ' ')
            if ch == '\n':
                in_line = False
            i += 1
            continue
        if in_block:
            if ch == '*' and i + 1 < n and text[i + 1] == '/':
                out.append('  ')
                i += 2
                in_block = False
            else:
                out.append('\n' if ch == '\n' else ' ')
                i += 1
            continue
        if in_str:
            out.append(ch)
            if ch == '\\' and i + 1 < n:
                out.append(text[i + 1])
                i += 2
                continue
            if ch == in_str:
                in_str = None
            i += 1
            continue
        if ch == '/' and i + 1 < n and text[i + 1] == '*':
            in_block = True
            out.append('  ')
            i += 2
            continue
        if ch == '/' and i + 1 < n and text[i + 1] == '/':
            in_line = True
            out.append('  ')
            i += 2
            continue
        if ch == '"' or ch == "'":
            in_str = ch
            out.append(ch)
            i += 1
            continue
        out.append(ch)
        i += 1
    return ''.join(out)


def _match_brace(s: str, open_pos: int) -> int:
                                                         
    depth = 0
    for i in range(open_pos, len(s)):
        c = s[i]
        if c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                return i
    return -1


def check_no_token_refusal(code: str) -> tuple:
                                           
                                                                    
    fn = re.search(r'\b' + re.escape(START_FN) + r'\s*\(', code)
    if not fn:
        return False, f"{START_FN} 함수 정의를 찾지 못함"
    body_open = code.find('{', fn.end())
    if body_open < 0:
        return False, f"{START_FN} 본문 여는 브레이스를 찾지 못함"
    body_close = _match_brace(code, body_open)
    if body_close < 0:
        return False, f"{START_FN} 본문 브레이스 매칭 실패"
    body = code[body_open:body_close + 1]

    guard = EMPTY_TOKEN_IF_RE.search(body)
    if not guard:
        return False, "empty-token 가드(if (!G_grpc_auth_token ...)) 없음 — 무토큰 거부 미구현"
    blk_open = body.find('{', guard.end())
    if blk_open < 0:
        return False, "empty-token 가드 블록 여는 브레이스 없음"
    blk_close = _match_brace(body, blk_open)
    if blk_close < 0:
        return False, "empty-token 가드 블록 브레이스 매칭 실패"
    block_inner = body[blk_open + 1:blk_close]
    if not RETURN_TAIL_RE.search(block_inner):
        return False, ("empty-token 가드 블록이 return; 으로 끝나지 않음 — "
                       "무토큰 기동이 fall-through로 허용됨(무인증 제어평면)")
    return True, "무토큰 기동 거부(empty-token 가드가 return으로 종료)"


def scan_text(text: str) -> tuple:
                                                          
    with_strings = strip_comments(text)
    missing = [t for t in INJECT_TOKENS if t not in with_strings]
    code = strip_code(text)
    refusal_ok, reason = check_no_token_refusal(code)
    return missing, refusal_ok, reason


def main(argv=None) -> int:
                                                          
                               
    argv = list(sys.argv[1:]) if argv is None else list(argv)
    target = Path(argv[0]) if argv else TARGET
    rel = argv[0] if argv else TARGET_REL
    text = target.read_text(errors="replace")

    missing, refusal_ok, reason = scan_text(text)

    print(f"[check-grpc-authz] role 주입 토큰 {len(INJECT_TOKENS) - len(missing)}/"
          f"{len(INJECT_TOKENS)} 존재 / 무토큰 거부 {'예' if refusal_ok else '아니오'}")

    fails = []
    if missing:
        fails.append(f"bounded role 주입 토큰 누락: {', '.join(missing)} "
                     f"(dispatcher가 ADMIN 기본 사용 → 무RBAC)")
    if not refusal_ok:
        fails.append(f"무토큰 기동 거부 미충족: {reason}")

    if fails:
        print(f"[FAIL] gRPC 인증/RBAC 불변식 위반 {len(fails)}건 ({rel}):", file=sys.stderr)
        for f in fails:
            print(f"  - {f}", file=sys.stderr)
        return 1
    print(f"[PASS] gRPC bounded role 주입 + 무토큰 기동 거부 충족 ({rel})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
