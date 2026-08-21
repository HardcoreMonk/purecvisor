#!/usr/bin/env python3
                          
                                                                     
                                                                                      
                                                                                                                            
 
                      
                                                                                                                              
                                                                  

                                                      

                                                                 
                                                            

                                             
                                                    
                                         
                                                               
                                            
                                                       

                                                     

                                                            
                              
   
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TARGET_REL = "src/utils/pcv_tls.c"
TARGET = ROOT / TARGET_REL

PRIORITY_ENV = "G_TLS_GNUTLS_PRIORITY"
CONFIG_KEY = "min_version"
SETENV = "g_setenv"


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
    if PRIORITY_ENV not in code:
        reasons.append(f"{PRIORITY_ENV} 미참조 — GnuTLS 우선순위 미고정"
                       " (SSL3/TLS1.0/1.1 다운그레이드 협상 가능)")
    if CONFIG_KEY not in code:
        reasons.append(f'"{CONFIG_KEY}" config 키 미참조 — [tls] min_version 미존중')
    if SETENV not in code:
        reasons.append(f"{SETENV}( 미호출 — 우선순위를 프로세스 전역에 고정하지 않음")
    return reasons


def main(argv=None) -> int:
    argv = list(sys.argv[1:]) if argv is None else list(argv)
    target = Path(argv[0]) if argv else TARGET
    rel = argv[0] if argv else TARGET_REL
    text = target.read_text(errors="replace")

    reasons = scan_text(text)
    ok = not reasons
    print(f"[check-tls-min-version] TLS 최소 버전 고정(min_version 존중) {'예' if ok else '아니오'}")

    if reasons:
        print(f"[FAIL] TLS 최소 버전 고정 불변식 위반 {len(reasons)}건 ({rel}):", file=sys.stderr)
        for r in reasons:
            print(f"  - {r}", file=sys.stderr)
        return 1
    print(f"[PASS] G_TLS_GNUTLS_PRIORITY 고정 + min_version config 존중 ({rel})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
