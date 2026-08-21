#!/usr/bin/env python3
                          
                                                                               
                                                                                      
                                                                                                                            
 
                      
                                                                                                                                        
                                                                                       

                                                                      

    
                                               
                                                               
                                                     
                                                         
                                                                  
                                                                     
                                                              
                                                               
                                                  

                   
                                                              
                                                                    
                                                        
                                                                            
                                                    
                                        

                          
                                                            
                                                          
                                                
                                     

                          
                                              
                                               
                                    

                                    
                                               
   
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src"

HELPER = "pcv_url_target_allowed"
CALL_RE = re.compile(r'\b' + re.escape(HELPER) + r'\s*\(')

TARGET_RELS = [
    "src/modules/daemons/alert_engine.c",
    "src/modules/ai/ai_agent.c",
    "src/modules/backup/backup_scheduler.c",
]

                                                 
WP_HELPER = "pcv_webpush_endpoint_allowed"
                                 
WP_CALL_RE = re.compile(r'(?<![\w])(?<!^)' + re.escape(WP_HELPER) + r'\s*\(', re.MULTILINE)
WP_DEF_REL = "src/modules/daemons/pcv_webpush.c"
WP_TARGET_RELS = [WP_DEF_REL]


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


def file_calls_helper(path: Path) -> bool:
                                                        
    text = path.read_text(errors="replace")
    return bool(CALL_RE.search(strip_code(text)))


def helper_defined() -> bool:
                                                                      
    for p in sorted((SRC / "utils").rglob("*.c")):
        if HELPER in strip_code(p.read_text(errors="replace")):
            return True
    return False


def wp_call_count(path: Path) -> int:
                                                              

                                                   
                                  
    return len(WP_CALL_RE.findall(strip_code(path.read_text(errors="replace"))))


def find_missing_wp_calls() -> list:
                                                            
    missing = []
    for rel in WP_TARGET_RELS:
        p = ROOT / rel
        if not p.exists():
            missing.append(f"{rel} (파일 없음)")
        elif wp_call_count(p) < 1:
            missing.append(rel)
    return missing


def find_missing_calls(targets) -> list:
                                   
    missing = []
    for t in targets:
        p = Path(t)
        rel = str(p.relative_to(ROOT)) if p.is_absolute() and str(p).startswith(str(ROOT)) else str(t)
        if not p.exists():
            missing.append(f"{rel} (파일 없음)")
        elif not file_calls_helper(p):
            missing.append(rel)
    return missing


def main(argv=None) -> int:
                                                         
                                                
    argv = list(sys.argv[1:]) if argv is None else list(argv)

    if argv:
        missing = find_missing_calls(argv)
        print(f"[check-ssrf-target-guard] 대상 {len(argv)}개 / "
              f"{HELPER} 호출 누락 {len(missing)}개 (단일 파일 모드)")
        if missing:
            print(f"[FAIL] {HELPER} 호출 없는 사이트 {len(missing)}건:", file=sys.stderr)
            for m in missing:
                print(f"  - {m}", file=sys.stderr)
            return 1
        print(f"[PASS] 지정 대상 모두 {HELPER} 호출")
        return 0

    targets = [ROOT / rel for rel in TARGET_RELS]
    missing = find_missing_calls(targets)
    hd = helper_defined()
    wp_missing = find_missing_wp_calls()

    print(f"[check-ssrf-target-guard] 아웃바운드 사이트 {len(TARGET_RELS)}개 / "
          f"{HELPER} 호출 누락 {len(missing)}개 / 헬퍼 정의 {'예' if hd else '아니오'} / "
          f"webpush 사이트 {len(WP_TARGET_RELS)}개 / {WP_HELPER} 호출 누락 {len(wp_missing)}개")

    fails = []
    if missing:
        fails.append(f"{HELPER} 호출 누락 {len(missing)}건: {', '.join(missing)}")
    if not hd:
        fails.append(f"{HELPER} 정의가 src/utils/ 에 없음 (SSRF 대상 검증 헬퍼 제거)")
    if wp_missing:
        fails.append(f"{WP_HELPER} 호출 누락 {len(wp_missing)}건: {', '.join(wp_missing)} "
                     f"(Web Push 구독 endpoint 가 무검증으로 장부에 들어간다)")

    if fails:
        print(f"[FAIL] 아웃바운드 대상 SSRF 검증 불변식 위반 {len(fails)}건:", file=sys.stderr)
        for f in fails:
            print(f"  - {f}", file=sys.stderr)
        return 1
    print(f"[PASS] 아웃바운드 3 사이트 모두 {HELPER} 호출 + 헬퍼 정의 존재 · "
          f"webpush {len(WP_TARGET_RELS)} 사이트 모두 {WP_HELPER} 호출")
    return 0


if __name__ == "__main__":
    sys.exit(main())
