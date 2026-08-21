#!/usr/bin/env python3
                          
                                                                
                                                                                      
                                                                                                                            
 
                      
                                                                                                                         
                                                                       

                                                                          

                               
                                                             
                                                               
                                                              
                      

     
                                        
                                                                 

                                                        
   
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TARGET_REL = "src/api/rest_server.c"
TARGET = ROOT / TARGET_REL

HELPER = "_body_has_secret"
                                                        
IS_AUTH_ASSIGN_RE = re.compile(r'\bis_auth\s*=(?P<rhs>.*?);', re.S)
HELPER_DEF_RE = re.compile(r'\bstatic\s+gboolean\s+_body_has_secret\s*\(')
HELPER_CALL_RE = re.compile(r'\b_body_has_secret\s*\(')


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
    defined = bool(HELPER_DEF_RE.search(code))
    used = False
    for m in IS_AUTH_ASSIGN_RE.finditer(code):
        if HELPER_CALL_RE.search(m.group('rhs')):
            used = True
            break
    return defined, used


def main(argv=None) -> int:
                                                          
                                                 
    argv = list(sys.argv[1:]) if argv is None else list(argv)
    target = Path(argv[0]) if argv else TARGET
    rel = argv[0] if argv else TARGET_REL
    text = target.read_text(errors="replace")
    defined, used = scan_text(text)

    print(f"[check-secret-logging] {HELPER} 정의 {'예' if defined else '아니오'} / "
          f"is_auth 산정 사용 {'예' if used else '아니오'}")

    fails = []
    if not defined:
        fails.append(f"{HELPER} 헬퍼 정의가 없음 (본문 기반 마스킹 판정 제거됨)")
    if not used:
        fails.append(f"is_auth 산정에서 {HELPER}(...) 미사용 — "
                     "/rpc 경유 평문 자격증명이 저널에 기록됨")

    if fails:
        print(f"[FAIL] 감사 로그 자격증명 마스킹 계약 위반 {len(fails)}건:", file=sys.stderr)
        for f in fails:
            print(f"  - {rel}: {f}", file=sys.stderr)
        return 1
    print(f"[PASS] {HELPER} 정의 + is_auth 산정에 결합 — 본문 기반 자격증명 마스킹 유지")
    return 0


if __name__ == "__main__":
    sys.exit(main())
