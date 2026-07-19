
"""check_mtls_wiring.py — mTLS 클라이언트 인증서 검증 배선 게이트 (C1 / A02·V12).

근거: 보안 평가 A02 — mTLS 가 문서화됐으나 서버 코드에 미배선(server-cert 만).

불변식(src/api/rest_server.c) — TLS 활성 경로에 mTLS 클라이언트 인증서 검증
정책(client_auth)이 실제로 배선돼 있어야 한다. 구조적으로 셋 다 충족해야 PASS:

  (1) client_auth 정책 분기: config [tls] client_auth 키를 읽는다
      (문자열 리터럴 client_auth 가 코드에 존재).
  (2) CA 검증 DB 배선: soup_server_set_tls_database( 를 호출한다
      (g_tls_file_database_new 로 만든 CA anchor DB 를 SoupServer 에 설정).
  (3) 클라이언트 인증 모드: soup_server_set_tls_auth_mode( 를 호출한다
      (G_TLS_AUTHENTICATION_REQUESTED/REQUIRED 로 클라이언트 인증서 요구).

셋 중 하나라도 없으면 mTLS 는 문서상 기능일 뿐 서버가 클라이언트 인증서를
검증하지 않는다(server-cert 만) → FAIL.

반사실: client_auth 분기 또는 set_tls_database/set_tls_auth_mode 배선을 제거해
        server-cert 전용(현행 이전)으로 되돌리면 (1)/(2)/(3) 이 사라져 RED.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TARGET_REL = "src/api/rest_server.c"
TARGET = ROOT / TARGET_REL

CONFIG_KEY = "client_auth"
TLS_DATABASE = "soup_server_set_tls_database"
TLS_AUTH_MODE = "soup_server_set_tls_auth_mode"

def strip_comments(text: str) -> str:
    """주석만 공백/개행으로 치환하고 문자열·문자 리터럴은 보존(줄 번호 1:1).
    "client_auth" 문자열 리터럴 + 코드 식별자를 함께 검출하기 위함."""
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
    """(reasons) — 빈 리스트면 PASS."""
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
