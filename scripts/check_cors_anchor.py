#!/usr/bin/env python3
                          
                                                                      
                                                                                      
                                                                                                                            
 
                      
                                                                                                                               
                                                                   

                                                                          

                           
                                                  
                                      
                                                                 
                                                               
                                                                 
                                                             
                                                               
                                                           

                                                    
                                          
   
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TARGET_REL = "src/api/rest_server.c"
TARGET = ROOT / TARGET_REL

REQUIRED_HELPER = "_cors_origin_allowed"

                                                  
FORBIDDEN = [
    (re.compile(r'strstr\s*\(\s*origin\s*,\s*"://[^"]'),
     'origin substring 내부망 화이트리스트 (://localhost/127/192.168/10 등)'),
    (re.compile(r'strstr\s*\(\s*origin\s*,\s*host\b'),
     'origin ⊇ host 반사 substring 비교'),
]


def strip_comments(text: str) -> str:
                                                          

                                                              
                                                         
                                   
       
    out = []
    i, n = 0, len(text)
    in_block = in_line = False
    in_str = None                              
    while i < n:
        ch = text[i]
        if in_line:
            if ch == '\n':
                in_line = False
                out.append('\n')
            else:
                out.append(' ')
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
    fails = []
    for i, line in enumerate(code.split('\n'), start=1):
        for rx, desc in FORBIDDEN:
            if rx.search(line):
                fails.append((i, desc, line.strip()))
    has_helper = REQUIRED_HELPER in code
    return fails, has_helper


def main(argv=None) -> int:
                                                          
                                                 
    argv = list(sys.argv[1:]) if argv is None else list(argv)
    target = Path(argv[0]) if argv else TARGET
    rel = argv[0] if argv else TARGET_REL
    text = target.read_text(errors="replace")
    fails, has_helper = scan_text(text)

    print(f"[check-cors-anchor] 금지 substring 패턴 {len(fails)}건 / "
          f"{REQUIRED_HELPER} 사용 {'예' if has_helper else '아니오'}")

    if fails:
        print(f"[FAIL] CORS origin substring 매칭 재등장 {len(fails)}건:", file=sys.stderr)
        for ln, desc, src in fails:
            print(f"  - {rel}:{ln} {desc}: {src}", file=sys.stderr)
        return 1
    if not has_helper:
        print(f"[FAIL] {REQUIRED_HELPER} 미사용 — 앵커 검증 헬퍼가 제거됨 "
              "(정확 일치 CORS 검증 회귀)", file=sys.stderr)
        return 1
    print(f"[PASS] CORS origin substring 매칭 없음 + {REQUIRED_HELPER} 앵커 검증 사용")
    return 0


if __name__ == "__main__":
    sys.exit(main())
