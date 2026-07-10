#!/usr/bin/env python3
"""
domsafe_ratchet.py — ADR-013 DOM-safe 가시성 래칫

innerHTML/outerHTML/insertAdjacentHTML/document.write 사용처를
ui/app.js + ui/modules/*.js 전역에서 세어 파일별 목록 + 총계를 출력한다.

[성격] 게이트가 아니라 가시성 리포트다 — exit code는 항상 0.
기존 사용처(수백 건)를 한 번에 걷어낼 계획은 없고, 신규 코드가 이 패턴을
계속 늘리지 않는지 사람이 숫자 추이를 직접 추적하기 위한 래칫이다.
필요해지면 --max 임계값 인자를 추가하고 초과 시 exit 1 분기를 넣어
게이트로 승격할 수 있다.

check_xss.py와의 차이: check_xss.py는 "sanitizer(esc/escapeHtml/H.* 등)를
거치지 않은 위험한 innerHTML"만 좁혀 찾는 XSS 게이트(실패 가능)다. 이
스크립트는 sanitizer 여부와 무관하게 innerHTML류 API 자체의 총 사용 건수를
세어 ADR-013 DOM-safe(el/frag/clearEl) 전환 진행률을 추적한다.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
UI = ROOT / "ui"

PATTERN = re.compile(
    r'\.(innerHTML|outerHTML)\b|\.insertAdjacentHTML\s*\(|\bdocument\.write\s*\('
)


def scan_file(path):
    hits = []
    for i, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        if PATTERN.search(line):
            hits.append((i, line.strip()))
    return hits


def main():
    files = sorted(UI.glob("modules/*.js")) + [UI / "app.js"]
    files = [f for f in files if f.exists() and f.name not in ("bundle.js", "app.bundle.js")]

    total = 0
    for f in files:
        hits = scan_file(f)
        if not hits:
            continue
        print(f"\n{f.relative_to(ROOT)}: {len(hits)}")
        for ln, code in hits:
            print(f"  L{ln}: {code[:140]}")
        total += len(hits)

    print(f"\nTOTAL innerHTML/outerHTML/insertAdjacentHTML/document.write sites: {total}")
    print("(ADR-013 DOM-safe 가시성 래칫 — 게이트 아님, exit 0 고정. "
          "신규 diff가 이 숫자를 늘리는지는 코드리뷰에서 직접 확인)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
