#!/usr/bin/env python3
                          
                                                          
                                                                                      
                                                                                                                            
 
                      
                                                                                                                   
                                                                  

                                                                    
                                                       
                                       

                                
                                                        
                                                                               
             

                                                          
                  
   
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_cors_anchor import strip_comments                                  

TARGET_REL = "src/api/rest_server.c"
TARGET = ROOT / TARGET_REL

UI_ANCHOR = 'g_str_has_prefix(path, "/ui")'

                                            
REQUIRED = [
    ("Content-Security-Policy",
     re.compile(r'soup_message_headers_replace\s*\([^;]*?"Content-Security-Policy"')),
    ("X-Frame-Options",
     re.compile(r'soup_message_headers_replace\s*\([^;]*?"X-Frame-Options"')),
]


def _extract_block(text: str, anchor: str):
                                                               
    idx = text.find(anchor)
    if idx < 0:
        return None
    brace = text.find("{", idx)
    if brace < 0:
        return None
    depth = 0
    for i in range(brace, len(text)):
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return text[brace:i + 1]
    return None


def scan_text(text: str):
                                         
                                                      
    code = strip_comments(text)
    block = _extract_block(code, UI_ANCHOR)
    if block is None:
        return [name for name, _ in REQUIRED], False
    missing = [name for name, rx in REQUIRED if not rx.search(block)]
    return missing, True


def main(argv=None) -> int:
    argv = list(sys.argv[1:]) if argv is None else list(argv)
    target = Path(argv[0]) if argv else TARGET
    rel = argv[0] if argv else TARGET_REL
    text = target.read_text(errors="replace")
    missing, found = scan_text(text)

    print(f"[check-security-headers] /ui 정적 블록 {'발견' if found else '미발견'} / "
          f"필수 헤더 누락 {len(missing)}건")

    if not found:
        print(f"[FAIL] {rel}: /ui 정적 서빙 블록('{UI_ANCHOR}')을 찾지 못함 — "
              "핸들러 구조 변경?", file=sys.stderr)
        return 1
    if missing:
        print(f"[FAIL] /ui 정적 응답에 보안 헤더 누락: {', '.join(missing)} "
              "(clickjacking/MIME 스니핑 노출 재발)", file=sys.stderr)
        return 1
    print("[PASS] /ui 정적 응답에 Content-Security-Policy + X-Frame-Options 부착됨")
    return 0


if __name__ == "__main__":
    sys.exit(main())
