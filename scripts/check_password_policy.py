#!/usr/bin/env python3
                          
                                                    
                                                                                      
                                                                                                                            
 
                      
                                                                                                             
                                                                      

                                                            
                                             
                                                

                                            
                                                                       
                            

                              
   
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_cors_anchor import strip_comments              

TARGET_REL = "src/modules/dispatcher/handler_auth.c"
TARGET = ROOT / TARGET_REL

FUNC_RE = re.compile(r'\bhandle_auth_user_create\s*\(')
REQUIRED_CALL = "pcv_validate_password_complexity("


def _extract_func_body(code: str):
                                                             
                                                      
    m = FUNC_RE.search(code)
    if not m:
        return None
    brace = code.find("{", m.end())
    if brace < 0:
        return None
    depth = 0
    for i in range(brace, len(code)):
        c = code[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return code[brace:i + 1]
    return None


def scan_text(text: str):
                                    
    code = strip_comments(text)
    body = _extract_func_body(code)
    if body is None:
        return False, False
    return (REQUIRED_CALL in body), True


def main(argv=None) -> int:
    argv = list(sys.argv[1:]) if argv is None else list(argv)
    target = Path(argv[0]) if argv else TARGET
    rel = argv[0] if argv else TARGET_REL
    text = target.read_text(errors="replace")
    has_call, found = scan_text(text)

    print(f"[check-password-policy] handle_auth_user_create "
          f"{'발견' if found else '미발견'} / 복잡도 검증 호출 "
          f"{'예' if has_call else '아니오'}")

    if not found:
        print(f"[FAIL] {rel}: handle_auth_user_create 정의를 찾지 못함 — "
              "핸들러 구조 변경?", file=sys.stderr)
        return 1
    if not has_call:
        print(f"[FAIL] handle_auth_user_create 가 {REQUIRED_CALL} 를 호출하지 않음 "
              "— 생성 경로 비밀번호 강도 정책 미집행(A07 회귀)", file=sys.stderr)
        return 1
    print("[PASS] handle_auth_user_create 가 pcv_validate_password_complexity 호출")
    return 0


if __name__ == "__main__":
    sys.exit(main())
