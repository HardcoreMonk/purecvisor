#!/usr/bin/env python3
                          
                                                             
                                                                                      
                                                                                                                            
 
                      
                                                                                                                      
                                                                    

                                                              
                                                        
                                                 

                 
                                                          
                                                            
                                                         
                                          

                                                                   
                                          
   
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_cors_anchor import strip_comments              

TARGET_REL = "src/api/ws_server.c"
TARGET = ROOT / TARGET_REL

VERIFY_CALL = "pcv_jwt_verify("
URL_QUERY = "g_uri_get_query"
TOKEN_LIT = '"token"'


def _top_level_blocks(code: str):
                                                
                                                      
    blocks = []
    depth = 0
    start = -1
    in_str = None
    i, n = 0, len(code)
    while i < n:
        c = code[i]
        if in_str:
            if c == "\\" and i + 1 < n:
                i += 2
                continue
            if c == in_str:
                in_str = None
            i += 1
            continue
        if c == '"' or c == "'":
            in_str = c
            i += 1
            continue
        if c == "{":
            if depth == 0:
                start = i
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0 and start >= 0:
                blocks.append(code[start:i + 1])
                start = -1
        i += 1
    return blocks


def scan_text(text: str):
                                         
                                                              
                                                                  
    code = strip_comments(text)
    violations = 0
    has_message_auth = False
    for body in _top_level_blocks(code):
        reads_url_token = (URL_QUERY in body) and (TOKEN_LIT in body)
        calls_verify = VERIFY_CALL in body
        if calls_verify and reads_url_token:
            violations += 1
        elif calls_verify and not reads_url_token:
            has_message_auth = True
    return violations, has_message_auth


def main(argv=None) -> int:
    argv = list(sys.argv[1:]) if argv is None else list(argv)
    target = Path(argv[0]) if argv else TARGET
    rel = argv[0] if argv else TARGET_REL
    text = target.read_text(errors="replace")
    violations, has_message_auth = scan_text(text)

    print(f"[check-ws-token-url] URL-query 토큰 인증 블록 {violations}건 / "
          f"메시지 경로 인증 {'유지' if has_message_auth else '없음'}")

    if violations:
        print(f"[FAIL] {rel}: URL 쿼리 토큰을 읽어 pcv_jwt_verify 로 인증하는 "
              f"블록 {violations}건 — URL-query 토큰 인증 재도입(프록시/로그 유출, A07)",
              file=sys.stderr)
        return 1
    if not has_message_auth:
        print(f"[FAIL] {rel}: 메시지 경로 pcv_jwt_verify 인증이 사라짐 — "
              "WS 인증 자체가 제거됨(과잉 삭제)", file=sys.stderr)
        return 1
    print("[PASS] URL-query 토큰 인증 없음 + 메시지 경로 pcv_jwt_verify 인증 유지")
    return 0


if __name__ == "__main__":
    sys.exit(main())
