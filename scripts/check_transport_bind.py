#!/usr/bin/env python3
"""check_transport_bind.py — 전송 평문 루프백 바인딩 게이트 (Wave C Item 6 / A02·V12).

근거: docs/operations/2026-07-16-security-remediation-roadmap.md Item 6.

불변식(src/api/rest_server.c) — 평문 HTTP 리스닝이 config [server] bind_plaintext 를
존중해야 한다(기본 loopback → 127.0.0.1 만 바인딩, 외부 평문 차단). 구조적으로 셋 다
충족해야 PASS:

  (1) config 읽기: pcv_config_get_string 로 "bind_plaintext" 키를 읽는다
      (문자열 리터럴 bind_plaintext 가 코드에 존재).
  (2) 루프백 주소 생성: g_inet_address_new_loopback 로 127.0.0.1 주소를 만든다.
  (3) 루프백 리스닝: soup_server_listen( (단수형 — 특정 주소 바인딩)을 호출한다.
      soup_server_listen_all( (0.0.0.0)만 있고 단수형이 없으면 무조건 전역 노출.

TLS(HTTPS) 리스닝은 무변경(항상 soup_server_listen_all 로 외부 노출)이며 이 게이트의
대상이 아니다 — 게이트는 soup_server_listen_all 존재 자체를 금지하지 않고, 평문 경로에
루프백 바인딩 기계장치(1)(2)(3)가 있는지만 확인한다.

반사실: 평문 리스닝을 무조건 soup_server_listen_all(0.0.0.0)로 되돌려 bind_plaintext
        미존중/루프백 경로 제거 시 (1)(2)(3)가 사라져 RED.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TARGET_REL = "src/api/rest_server.c"
TARGET = ROOT / TARGET_REL

CONFIG_KEY = "bind_plaintext"
LOOPBACK_ADDR = "g_inet_address_new_loopback"
# 단수형 soup_server_listen( — soup_server_listen_all( 는 뒤에 '_all' 이 붙어 매칭 안 됨.
LISTEN_SINGULAR_RE = re.compile(r'\bsoup_server_listen\s*\(')


def strip_comments(text: str) -> str:
    """주석만 공백/개행으로 치환하고 문자열·문자 리터럴은 보존(줄 번호 1:1).
    "bind_plaintext" 문자열 리터럴 + 코드 식별자를 함께 검출하기 위함."""
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
        reasons.append(f'"{CONFIG_KEY}" config 키 미참조 — 평문 바인딩 스코프 미존중(무조건 노출)')
    if LOOPBACK_ADDR not in code:
        reasons.append(f"{LOOPBACK_ADDR} 미사용 — 127.0.0.1 루프백 바인딩 경로 부재")
    if not LISTEN_SINGULAR_RE.search(code):
        reasons.append("soup_server_listen( (단수형) 미사용 — 특정 주소 바인딩 없이 "
                       "soup_server_listen_all(0.0.0.0)만 존재")
    return reasons


def main(argv=None) -> int:
    argv = list(sys.argv[1:]) if argv is None else list(argv)
    target = Path(argv[0]) if argv else TARGET
    rel = argv[0] if argv else TARGET_REL
    text = target.read_text(errors="replace")

    reasons = scan_text(text)
    ok = not reasons
    print(f"[check-transport-bind] 평문 bind_plaintext 존중(loopback 기본) {'예' if ok else '아니오'}")

    if reasons:
        print(f"[FAIL] 평문 전송 바인딩 불변식 위반 {len(reasons)}건 ({rel}):", file=sys.stderr)
        for r in reasons:
            print(f"  - {r}", file=sys.stderr)
        return 1
    print(f"[PASS] 평문 HTTP 가 bind_plaintext(기본 loopback=127.0.0.1) 존중 ({rel})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
