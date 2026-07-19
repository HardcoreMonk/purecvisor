
"""check_cors_anchor.py — CORS 오리진 앵커 검증 게이트 (Wave A / A05·V3·V13).

근거: docs/operations/2026-07-16-security-assessment-owasp-ismsp.md §8 시정 1.

불변식(rest_server.c CORS 경로):
  1. Origin을 substring으로 매칭하는 우회 휴리스틱이 재등장하면 FAIL.
     금지 패턴(주석 제외, 문자열 리터럴 보존 상태에서 검사):
       - strstr(origin, "://<something>")  — 내부망 substring 화이트리스트
         (://localhost / ://127.0.0.1 / ://192.168. / ://10. 등)
       - strstr(origin, host)              — Host 반사 substring 비교
     정당한 scheme 구분자 탐색 strstr(origin, "://")는 허용한다(뒤에 닫는 따옴표라
     `"://[^"]` 정규식에 걸리지 않음 — _cors_origin_allowed 내부 호스트 추출용).
  2. 호스트 단위 정확 일치 검증 헬퍼 _cors_origin_allowed 가 정의·사용되어야 한다.

반사실: CORS 블록을 예전 substring 휴리스틱으로 되돌리면 금지 패턴이 재등장하거나
_cors_origin_allowed 사용이 사라져 게이트가 RED가 된다.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TARGET_REL = "src/api/rest_server.c"
TARGET = ROOT / TARGET_REL

REQUIRED_HELPER = "_cors_origin_allowed"

FORBIDDEN = [
    (re.compile(r'strstr\s*\(\s*origin\s*,\s*"://[^"]'),
     'origin substring 내부망 화이트리스트 (://localhost/127/192.168/10 등)'),
    (re.compile(r'strstr\s*\(\s*origin\s*,\s*host\b'),
     'origin ⊇ host 반사 substring 비교'),
]

def strip_comments(text: str) -> str:
    """C 주석(//, /* */)만 공백/개행으로 치환하고 문자열·문자 리터럴은 그대로 보존한다.

    check_error_codes.strip_code는 문자열 내용까지 지우지만, 이 게이트의 금지 패턴은
    문자열 리터럴 인자("://localhost")를 포함하므로 문자열을 보존해야 한다. 줄 번호를
    원본과 1:1로 유지하기 위해 개행은 개행으로 치환한다.
    """
    out = []
    i, n = 0, len(text)
    in_block = in_line = False
    in_str = None
    while i < n:
        ch = text[i]
        if in_line:
            if ch == '\n':
                in_line = False
                out.append('\n')
            else:
                out.append(' ')
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
    """(위반 리스트, 헬퍼사용여부) 반환. 위반 = (line, 설명, 원본줄)."""
    code = strip_comments(text)
    fails = []
    for i, line in enumerate(code.split('\n'), start=1):
        for rx, desc in FORBIDDEN:
            if rx.search(line):
                fails.append((i, desc, line.strip()))
    has_helper = REQUIRED_HELPER in code
    return fails, has_helper

def main(argv=None) -> int:

    argv = list(sys.argv[1:]) if argv is None else list(argv)
    target = Path(argv[0]) if argv else TARGET
    rel = argv[0] if argv else TARGET_REL
    text = target.read_text(errors="replace")
    fails, has_helper = scan_text(text)

    print(f"[check-cors-anchor] 금지 substring 패턴 {len(fails)}건 / "
          f"{REQUIRED_HELPER} 사용 {'예' if has_helper else '아니오'}")

    if fails:
        print(f"[FAIL] CORS origin substring 매칭 재등장 {len(fails)}건:", file=sys.stderr)
        for ln, desc, src in fails:
            print(f"  - {rel}:{ln} {desc}: {src}", file=sys.stderr)
        return 1
    if not has_helper:
        print(f"[FAIL] {REQUIRED_HELPER} 미사용 — 앵커 검증 헬퍼가 제거됨 "
              "(정확 일치 CORS 검증 회귀)", file=sys.stderr)
        return 1
    print(f"[PASS] CORS origin substring 매칭 없음 + {REQUIRED_HELPER} 앵커 검증 사용")
    return 0

if __name__ == "__main__":
    sys.exit(main())
