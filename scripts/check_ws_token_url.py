#!/usr/bin/env python3
"""check_ws_token_url.py — WS URL-query 토큰 인증 경로 제거 게이트 (Q-5 / A07).

근거: 보안 Quick 시정 Q-5. WebSocket 인증이 URL ?token=<JWT> 즉시인증을 지원해,
토큰이 리버스 프록시/액세스 로그/Referer 로 유출됐다. URL-query 토큰 인증을 제거하고
메시지 경로({"type":"auth","token":...})로만 인증하도록 고정한다.

불변식(ws_server.c):
  1. URL 쿼리에서 토큰을 읽는 함수(본문에 g_uri_get_query + "token" 포함)는
     pcv_jwt_verify 를 호출하면 안 된다 — URL-query 토큰을 인증에 쓰는 것이므로.
  2. (positive control) 메시지 경로 인증은 유지돼야 한다: URL 쿼리를 읽지 않는
     함수에 pcv_jwt_verify 호출이 최소 1개 존재해야 한다.

반사실: URL 토큰을 읽는 함수(_ws_warn_deprecated_url_token 등)에 pcv_jwt_verify
호출을 되살리면(=URL-query 인증 재도입) 게이트가 RED 가 된다.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_cors_anchor import strip_comments  # noqa: E402

TARGET_REL = "src/api/ws_server.c"
TARGET = ROOT / TARGET_REL

VERIFY_CALL = "pcv_jwt_verify("
URL_QUERY = "g_uri_get_query"
TOKEN_LIT = '"token"'


def _top_level_blocks(code: str):
    """최상위 '{...}' 블록(함수 본문 등)들을 본문 문자열 리스트로 반환.
    깊이 0→1 로 여는 '{' 부터 다시 0 이 되는 '}' 까지를 한 블록으로 본다."""
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
    """(violations, has_message_auth) 반환.
    violations: URL-query 토큰을 읽으면서 pcv_jwt_verify 를 호출하는 블록 수.
    has_message_auth: URL 쿼리를 읽지 않는 블록에 pcv_jwt_verify 가 존재하는지."""
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
