#!/usr/bin/env python3
"""check_security_headers.py — /ui 정적 응답 보안 헤더 부착 게이트 (Q-1 / A05).

근거: 보안 Quick 시정 Q-1. REST JSON 응답(_send_json)은 CSP·X-Frame-Options 등
보안 헤더를 붙이지만, /ui HTML/JS 정적 서빙 핸들러는 _send_json 을 거치지 않아
그 헤더가 누락됐다(clickjacking / MIME 스니핑 노출).

불변식(rest_server.c /ui 정적 서빙 블록):
  1. `if (g_str_has_prefix(path, "/ui"))` 블록 본문에서 응답 헤더에
     Content-Security-Policy 와 X-Frame-Options 를 soup_message_headers_replace 로
     설정해야 한다.

반사실: /ui 블록의 CSP(또는 X-Frame-Options) replace 호출을 지우면 그 헤더가
누락돼 게이트가 RED 가 된다.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_cors_anchor import strip_comments  # noqa: E402  (문자열 보존 주석 제거 재사용)

TARGET_REL = "src/api/rest_server.c"
TARGET = ROOT / TARGET_REL

UI_ANCHOR = 'g_str_has_prefix(path, "/ui")'

# /ui 블록 본문에 존재해야 하는 헤더 replace 호출 (변수명 무관).
REQUIRED = [
    ("Content-Security-Policy",
     re.compile(r'soup_message_headers_replace\s*\([^;]*?"Content-Security-Policy"')),
    ("X-Frame-Options",
     re.compile(r'soup_message_headers_replace\s*\([^;]*?"X-Frame-Options"')),
]


def _extract_block(text: str, anchor: str):
    """anchor 위치 이후 첫 '{' 부터 매칭 '}' 까지의 블록 본문을 반환. 없으면 None."""
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
    """(missing_headers, block_found) 반환.
    missing_headers: /ui 블록 본문에서 못 찾은 필수 헤더 이름 리스트."""
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
