
"""check_grpc_authz.py — gRPC 인증/RBAC 게이트 (Wave B Item 2 / A01·V8).

근거: docs/operations/2026-07-16-security-remediation-roadmap.md Item 2.

불변식(src/api/grpc_server.c) — 둘 다 충족해야 PASS:

  (1) bounded caller role 주입: 요청 프록시가 UDS params에 _pcv_caller_role 과
      _pcv_caller_sub 를 주입해야 한다(그렇지 않으면 dispatcher가 caller role을
      ADMIN으로 기본 가정). 두 키 문자열이 코드에서 사라지면 FAIL.

  (2) 무토큰 기동 거부: pcv_grpc_server_start 에서 auth_token 이 미설정/빈 문자열이면
      서버를 시작하지 않아야 한다. 구조적으로 "empty-token 가드 블록의 마지막
      문장이 return; 이어야 한다"로 검증한다. 예전 permissive 코드(무토큰인데
      127.0.0.1이면 계속 진행 → INFO 로그로 fall-through)는 블록이 return으로
      끝나지 않으므로 RED가 된다.

반사실: 주입을 제거하면 (1)이, 무토큰 fall-through로 되돌리면 (2)가 RED.
"""
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
    """주석·문자열·문자 리터럴 내용을 공백/개행으로 치환(줄 번호·오프셋 1:1 유지).
    브레이스 매칭·문장 검출용 — 문자열/주석 속 { } ; 가 구조를 흐리지 않는다."""
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
    """주석만 공백/개행으로 치환하고 문자열·문자 리터럴은 보존(줄 번호 1:1).
    _pcv_caller_role 등 문자열 리터럴 토큰 존재 확인용."""
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
    """s[open_pos]=='{' 라 가정, 매칭되는 '}' 인덱스 반환(없으면 -1)."""
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
    """(ok, reason). code = strip_code 적용본.
    pcv_grpc_server_start 의 empty-token 가드 블록이 return; 으로 끝나면 ok."""
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
    """(missing_tokens, refusal_ok, refusal_reason) 반환."""
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
