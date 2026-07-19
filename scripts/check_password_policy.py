
"""check_password_policy.py — user-create 비밀번호 복잡도 정책 게이트 (Q-2 / A07).

근거: 보안 Quick 시정 Q-2. auth.user.create 는 password 필수만 검증하고 강도
정책이 없었다(단순 비밀번호로 계정 생성 가능). 생성 경로에서 복잡도 검증 헬퍼
pcv_validate_password_complexity() 를 호출하도록 고정한다.

불변식(handler_auth.c handle_auth_user_create):
  1. handle_auth_user_create 함수 본문이 pcv_validate_password_complexity( 를
     호출해야 한다(생성 시 강도 정책 집행).

반사실: 그 호출을 제거하면 게이트가 RED 가 된다.
"""
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
    """handle_auth_user_create 정의의 '{'..'}' 본문을 반환. 없으면 None.
    (주석 제거된 code 에서 함수명은 정의부 1회만 등장 — 선언은 .h 에 있음.)"""
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
    """(has_call, func_found) 반환."""
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
