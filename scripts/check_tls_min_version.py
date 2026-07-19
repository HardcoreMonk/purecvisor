
"""check_tls_min_version.py — TLS 최소 버전 고정 게이트 (C2 / A02·V11·V12).

근거: 보안 평가 A02 — TLS 버전/스위트 미고정(SSL3/TLS1.0/1.1 협상 가능).

불변식(src/utils/pcv_tls.c) — glib-networking(GnuTLS) 우선순위 문자열을 프로세스
전역으로 고정하여 SSL3/TLS1.0/TLS1.1 을 차단해야 한다. 구조적으로 셋 다 충족해야 PASS:

  (1) 우선순위 환경변수: G_TLS_GNUTLS_PRIORITY 를 참조한다
      (glib-networking 이 이 값을 GnuTLS priority 로 읽음).
  (2) config 존중: [tls] min_version 키를 읽는다
      (문자열 리터럴 min_version — 1.2 기본 / 1.3 상향을 운영자 config 로 선택).
  (3) 환경 고정: g_setenv( 로 우선순위를 프로세스 전역에 고정한다
      (TLS 첫 사용 전이어야 반영 — pcv_tls_init_from_config 초기).

셋 중 하나라도 없으면 TLS 하한선이 고정되지 않아 다운그레이드 협상이 가능하다 → FAIL.

반사실: G_TLS_GNUTLS_PRIORITY 고정 또는 min_version config 존중을 제거하면
        (1)/(2)/(3) 이 사라져 RED.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TARGET_REL = "src/utils/pcv_tls.c"
TARGET = ROOT / TARGET_REL

PRIORITY_ENV = "G_TLS_GNUTLS_PRIORITY"
CONFIG_KEY = "min_version"
SETENV = "g_setenv"

def strip_comments(text: str) -> str:
    """주석만 공백/개행으로 치환하고 문자열·문자 리터럴은 보존(줄 번호 1:1).
    "G_TLS_GNUTLS_PRIORITY"/"min_version" 문자열 리터럴 + 코드 식별자를 함께 검출."""
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
