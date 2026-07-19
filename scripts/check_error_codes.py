
"""check_error_codes.py — raw 에러코드 리터럴(-32xxx) 재도입 방지 게이트 (DISP-6 회귀).
설계: docs/superpowers/specs/2026-07-15-disp6-error-code-unification-design.md §회귀 게이트

불변식(둘 다 위반 0):
  1. src/**/*.c 실코드의 raw -32xxx 리터럴 0 (에러코드 생산 사이트) — baseline 밖은
     신규 리터럴로 FAIL(단조 감소 래칫). 주석/문자열 리터럴 내부는 제외.
  2. PCV_ERR_[A-Z]* (구 병렬 enum) #define/사용 0 — baseline 없이 항상 FAIL.
     PCV_VM_ERR_*(pcv_error.h, VM 오퍼레이션 결과 도메인)는 별개라 제외 대상이지만,
     실제로는 식별자 경계상 "PCV_ERR_" 부분문자열이 나타나지 않아 정규식 자체가
     오탐하지 않는다(단어 전체가 하나의 \\w 런이라 중간 \\b가 없음) — 그래도 enum
     정의부(rpc_utils.h)는 명시적으로 제외한다(값 정의이지 리터럴 오용이 아님).
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BASELINE_FILE = ROOT / "scripts" / "error_codes_baseline.txt"
ENUM_DEF_FILE = "src/modules/dispatcher/rpc_utils.h"

RAW_RE = re.compile(r'-32[0-9]{3}\b')
PCV_ERR_RE = re.compile(r'\bPCV_ERR_[A-Z][A-Z0-9_]*\b')

def strip_code(text: str) -> str:
    """주석·문자열·문자 리터럴 내용을 공백으로 치환한다(길이·개행 보존 → line 번호 정확).

    check_dead_exports.strip_code와 달리 파일 전체를 문자 단위로 한 번에 순회한다(줄
    단위로 문자열 상태를 리셋하지 않음) — `\\`-이음으로 여러 물리줄에 걸친 문자열/블록
    주석도 안전하게 처리하면서, 매 원본 문자를 정확히 1문자(개행은 개행 그대로)로
    치환해 결과 텍스트의 줄 번호가 원본과 1:1로 유지된다. dead-export 게이트는 줄
    번호가 필요 없어 line-splicing(줄 병합)으로 처리했지만, 본 게이트는 위반
    file:line 리포팅이 핵심이라 병합 없는 이 방식이 맞다.
    """
    out = []
    i, n = 0, len(text)
    in_block = False
    in_line = False
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

def find_raw_literals_in_text(rel_path: str, text: str) -> list:
    """단일 파일 텍스트에서 raw -32xxx 리터럴 사이트를 "file:line" 식별자 리스트로 반환."""
    stripped = strip_code(text)
    ids = []
    for i, line in enumerate(stripped.split('\n'), start=1):
        if RAW_RE.search(line):
            ids.append(f"{rel_path}:{i}")
    return ids

def find_pcv_err_in_text(rel_path: str, text: str) -> list:
    """단일 파일 텍스트에서 PCV_ERR_* 식별자 사용을 "file:이름" 리스트로 반환.
    enum 정의부(ENUM_DEF_FILE)는 값 정의라 호출자 판단 없이 여기서 직접 제외한다."""
    if rel_path == ENUM_DEF_FILE:
        return []
    stripped = strip_code(text)
    return [f"{rel_path}:{m.group()}" for m in PCV_ERR_RE.finditer(stripped)]

def _load_baseline() -> set:
    if not BASELINE_FILE.exists():
        return set()
    return {ln.strip() for ln in BASELINE_FILE.read_text().splitlines()
            if ln.strip() and not ln.lstrip().startswith("#")}

def main() -> int:
    c_files = sorted((ROOT / "src").rglob("*.c"))
    raw_ids, pcv_err_hits = [], []
    for p in c_files:
        rel = str(p.relative_to(ROOT))
        text = p.read_text(errors="replace")
        raw_ids.extend(find_raw_literals_in_text(rel, text))
        pcv_err_hits.extend(find_pcv_err_in_text(rel, text))

    baseline = _load_baseline()
    new_raw = sorted(set(raw_ids) - baseline)
    stale = sorted(baseline - set(raw_ids))

    print(f"[check-error-codes] raw -32xxx 리터럴 {len(raw_ids)} / baseline {len(baseline)} / "
          f"PCV_ERR_ 재도입 {len(pcv_err_hits)}")
    if stale:
        print(f"[INFO] baseline {len(stale)}건 이제 정리됨 — baseline에서 제거 권장: "
              f"{', '.join(stale[:10])}")

    fails = []
    if new_raw:
        fails.append(f"신규 raw 에러코드 리터럴 {len(new_raw)}건 (PURE_RPC_ERR_* 상수 사용 필요)")
    if pcv_err_hits:
        fails.append(f"PCV_ERR_ 재도입 {len(pcv_err_hits)}건 (canonical PURE_RPC_ERR_*만 허용)")

    if fails:
        print(f"[FAIL] 에러코드 계약 위반 {len(fails)}건:", file=sys.stderr)
        for rid in new_raw:
            print(f"  - raw 리터럴: {rid}", file=sys.stderr)
        for h in pcv_err_hits:
            print(f"  - PCV_ERR_ 재도입: {h}", file=sys.stderr)
        return 1
    print("[PASS] raw -32xxx 리터럴 신규 없음(baseline 밖) + PCV_ERR_ 재도입 0")
    return 0

if __name__ == "__main__":
    sys.exit(main())
