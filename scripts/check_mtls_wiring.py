#!/usr/bin/env python3
                          
                                                        
                                                                                      
                                                                                                                            
 
                      
                                                                                                                 
                                                                  

                                                        

                                                         
                                                     

                                                        
                                     
                                                       
                                                                    
                                                        
                                                               

                                            
                               

                                                                 
                                                           
   
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TARGET_REL = "src/api/rest_server.c"
TARGET = ROOT / TARGET_REL

CONFIG_KEY = "client_auth"
TLS_DATABASE = "soup_server_set_tls_database"
TLS_AUTH_MODE = "soup_server_set_tls_auth_mode"


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


def scan_text(text: str):
                                  
    code = strip_comments(text)
    reasons = []
    if CONFIG_KEY not in code:
        reasons.append(f'"{CONFIG_KEY}" config 키 미참조 — mTLS 정책 분기 부재'
                       ' (server-cert 전용, 클라이언트 인증서 미요구)')
    if TLS_DATABASE not in code:
        reasons.append(f"{TLS_DATABASE}( 미호출 — CA 검증 DB 미배선"
                       " (클라이언트 인증서 신뢰체인 검증 불가)")
    if TLS_AUTH_MODE not in code:
        reasons.append(f"{TLS_AUTH_MODE}( 미호출 — 클라이언트 인증 모드"
                       " (REQUESTED/REQUIRED) 미설정 (클라이언트 인증서 미요구)")
    return reasons


def main(argv=None) -> int:
    argv = list(sys.argv[1:]) if argv is None else list(argv)
    target = Path(argv[0]) if argv else TARGET
    rel = argv[0] if argv else TARGET_REL
    text = target.read_text(errors="replace")

    reasons = scan_text(text)
    ok = not reasons
    print(f"[check-mtls-wiring] mTLS 클라이언트 인증서 검증 배선 {'예' if ok else '아니오'}")

    if reasons:
        print(f"[FAIL] mTLS 배선 불변식 위반 {len(reasons)}건 ({rel}):", file=sys.stderr)
        for r in reasons:
            print(f"  - {r}", file=sys.stderr)
        return 1
    print(f"[PASS] mTLS client_auth 정책 + CA DB + 인증 모드 배선 존재 ({rel})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
