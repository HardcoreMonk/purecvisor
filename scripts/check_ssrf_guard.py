
"""check_ssrf_guard.py — 아웃바운드 리다이렉트 금지 게이트 (Wave A / A10·V4).

근거: docs/operations/2026-07-16-security-assessment-owasp-ismsp.md §8 시정 3.

불변식(데몬 소스 전체):
  아웃바운드 HTTP 클라이언트 메시지를 만드는 모든 `soup_message_new(` 호출은
  그 직후 WINDOW 행 이내에 `soup_message_set_flags(..., SOUP_MESSAGE_NO_REDIRECT)`
  로 리다이렉트 추종을 금지해야 한다. 리다이렉트를 추종하면 링크로컬/루프백
  denylist나 endpoint allowlist를 3xx 응답으로 우회할 수 있다(SSRF).

  현재 대상 2곳:
    - src/modules/daemons/alert_engine.c (webhook POST)
    - src/modules/ai/ai_agent.c          (AI provider POST)

반사실: 어느 한 곳에서든 SOUP_MESSAGE_NO_REDIRECT를 제거하면 게이트가 RED가 된다.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src"

WINDOW = 12

NEW_RE = re.compile(r'\bsoup_message_new\s*\(')
NO_REDIRECT_TOKEN = "SOUP_MESSAGE_NO_REDIRECT"

def strip_code(text: str) -> str:
    """C 주석·문자열·문자 리터럴 내용을 공백/개행으로 치환(줄 번호 1:1 유지).

    식별자(soup_message_new / SOUP_MESSAGE_NO_REDIRECT)만 코드로 남기므로, 주석이나
    문자열 안에 우연히 같은 토큰이 있어도 오탐/누락되지 않는다."""
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

def find_unguarded_in_text(rel_path: str, text: str) -> list:
    """soup_message_new( 호출 중 WINDOW행 내에 NO_REDIRECT가 없는 사이트를 반환.

    반환: [(rel_path, line)] — 무가드 아웃바운드 메시지 생성 위치."""
    stripped = strip_code(text)
    lines = stripped.split('\n')
    unguarded = []
    for idx, line in enumerate(lines):
        if NEW_RE.search(line):
            window = lines[idx: idx + 1 + WINDOW]
            if not any(NO_REDIRECT_TOKEN in w for w in window):
                unguarded.append((rel_path, idx + 1))
    return unguarded

def scan_tree() -> tuple:
    """(총 아웃바운드 사이트 수, 무가드 리스트) 반환."""
    total = 0
    unguarded = []
    for p in sorted(SRC.rglob("*.c")):
        rel = str(p.relative_to(ROOT))
        text = p.read_text(errors="replace")
        stripped = strip_code(text)
        total += sum(1 for ln in stripped.split('\n') if NEW_RE.search(ln))
        unguarded.extend(find_unguarded_in_text(rel, text))
    return total, unguarded

def main(argv=None) -> int:

    argv = list(sys.argv[1:]) if argv is None else list(argv)
    if argv:
        p = Path(argv[0])
        text = p.read_text(errors="replace")
        stripped = strip_code(text)
        total = sum(1 for ln in stripped.split('\n') if NEW_RE.search(ln))
        unguarded = find_unguarded_in_text(str(p), text)
    else:
        total, unguarded = scan_tree()
    print(f"[check-ssrf-guard] 아웃바운드 soup_message_new 사이트 {total}개 / "
          f"무가드 {len(unguarded)}개 (window {WINDOW}행)")
    if unguarded:
        print(f"[FAIL] 리다이렉트 미차단 아웃바운드 메시지 {len(unguarded)}건 "
              f"(soup_message_new 직후 {WINDOW}행 내 {NO_REDIRECT_TOKEN} 필요):",
              file=sys.stderr)
        for rel, ln in unguarded:
            print(f"  - {rel}:{ln}", file=sys.stderr)
        return 1
    print(f"[PASS] 모든 아웃바운드 soup_message_new에 {NO_REDIRECT_TOKEN} 적용됨")
    return 0

if __name__ == "__main__":
    sys.exit(main())
