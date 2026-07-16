#!/usr/bin/env python3
"""check_ssrf_target_guard.py — 아웃바운드 대상 SSRF allowlist 게이트 (Wave B Item 5-a / A10·V4).

근거: docs/operations/2026-07-16-security-remediation-roadmap.md Item 5.

불변식:
  1. 아웃바운드 URL을 send하는 3개 사이트가 send 전에 대상 검증 헬퍼
     pcv_url_target_allowed() 를 호출해야 한다. 어느 하나라도 호출이 사라지면 FAIL.
       - src/modules/daemons/alert_engine.c (webhook)
       - src/modules/ai/ai_agent.c          (AI endpoint)
       - src/modules/backup/backup_scheduler.c (S3 --endpoint-url)
  2. 헬퍼 pcv_url_target_allowed 가 src/utils/ 에 정의되어 있어야 한다(제거 시 FAIL).

반사실: 세 사이트 중 하나에서 pcv_url_target_allowed 호출을 제거하면 게이트가 RED.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src"

HELPER = "pcv_url_target_allowed"
CALL_RE = re.compile(r'\b' + re.escape(HELPER) + r'\s*\(')

TARGET_RELS = [
    "src/modules/daemons/alert_engine.c",
    "src/modules/ai/ai_agent.c",
    "src/modules/backup/backup_scheduler.c",
]


def strip_code(text: str) -> str:
    """주석·문자열·문자 리터럴 내용을 공백/개행으로 치환(줄 번호 1:1 유지).
    실제 함수 호출만 코드로 남기므로 주석/문자열 속 토큰은 세지 않는다."""
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


def file_calls_helper(path: Path) -> bool:
    """path 소스가 pcv_url_target_allowed()를 코드에서 호출하는지."""
    text = path.read_text(errors="replace")
    return bool(CALL_RE.search(strip_code(text)))


def helper_defined() -> bool:
    """src/utils/ 하위 .c 중 pcv_url_target_allowed 정의(호출자 아님)를 포함하는지."""
    for p in sorted((SRC / "utils").rglob("*.c")):
        if HELPER in strip_code(p.read_text(errors="replace")):
            return True
    return False


def find_missing_calls(targets) -> list:
    """호출이 없는 대상 파일의 rel 경로 리스트."""
    missing = []
    for t in targets:
        p = Path(t)
        rel = str(p.relative_to(ROOT)) if p.is_absolute() and str(p).startswith(str(ROOT)) else str(t)
        if not p.exists():
            missing.append(f"{rel} (파일 없음)")
        elif not file_calls_helper(p):
            missing.append(rel)
    return missing


def main(argv=None) -> int:
    # argv 주어지면 그 파일들을 대상으로 호출 유무만 검사(self-test 반사실 단일 파일
    # 모드 — temp 사본 검사). 없으면 정본 3 사이트 + 헬퍼 정의 검사.
    argv = list(sys.argv[1:]) if argv is None else list(argv)

    if argv:
        missing = find_missing_calls(argv)
        print(f"[check-ssrf-target-guard] 대상 {len(argv)}개 / "
              f"{HELPER} 호출 누락 {len(missing)}개 (단일 파일 모드)")
        if missing:
            print(f"[FAIL] {HELPER} 호출 없는 사이트 {len(missing)}건:", file=sys.stderr)
            for m in missing:
                print(f"  - {m}", file=sys.stderr)
            return 1
        print(f"[PASS] 지정 대상 모두 {HELPER} 호출")
        return 0

    targets = [ROOT / rel for rel in TARGET_RELS]
    missing = find_missing_calls(targets)
    hd = helper_defined()

    print(f"[check-ssrf-target-guard] 아웃바운드 사이트 {len(TARGET_RELS)}개 / "
          f"{HELPER} 호출 누락 {len(missing)}개 / 헬퍼 정의 {'예' if hd else '아니오'}")

    fails = []
    if missing:
        fails.append(f"{HELPER} 호출 누락 {len(missing)}건: {', '.join(missing)}")
    if not hd:
        fails.append(f"{HELPER} 정의가 src/utils/ 에 없음 (SSRF 대상 검증 헬퍼 제거)")

    if fails:
        print(f"[FAIL] 아웃바운드 대상 SSRF 검증 불변식 위반 {len(fails)}건:", file=sys.stderr)
        for f in fails:
            print(f"  - {f}", file=sys.stderr)
        return 1
    print(f"[PASS] 아웃바운드 3 사이트 모두 {HELPER} 호출 + 헬퍼 정의 존재")
    return 0


if __name__ == "__main__":
    sys.exit(main())
