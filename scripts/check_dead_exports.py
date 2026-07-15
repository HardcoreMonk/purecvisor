#!/usr/bin/env python3
"""check_dead_exports.py — 헤더 선언 비-static pcv_* 함수 중 .c 사용처 0 노출/차단.
설계: docs/superpowers/specs/2026-07-11-dead-export-gate-design.md
"""
import re, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BASELINE_FILE = ROOT / "scripts" / "dead_exports_baseline.txt"
WAIVER = "PCV_DEAD_EXPORT_OK"
DECL_RE = re.compile(r'\b(pcv_[a-z0-9_]+)\s*\(')


def _splice_continuations(text: str) -> str:
    """C 표준 phase-2 line-splicing: 줄 끝 `\\`+개행을 제거하고 다음 줄과 합친다.

    문자열/문자 리터럴 파싱은 물리 줄 단위로 상태를 리셋한다(strip_code 참고).
    `\\`로 이어진 문자열 리터럴이 다음 물리줄에서 닫히면, 스플라이스 없이는
    그 물리줄 경계에서 문자열 상태가 리셋되어 닫는 따옴표를 새 문자열의
    시작으로 오인 → 뒤따르는 실코드(실호출)를 삼켜 오탐(FALSE ALARM: 생존
    함수가 dead로 잡힘)을 만든다. 토큰화 이전(comment/string 상태머신 이전)에
    논리 줄을 합쳐 이 경계 자체를 없앤다.
    """
    lines = text.split('\n')
    out = []
    buf = ''
    for line in lines:
        if line.endswith('\\'):
            buf += line[:-1]
        else:
            out.append(buf + line)
            buf = ''
    if buf:                     # 파일이 이어진 채로 끝나는 비정상 케이스 방어
        out.append(buf)
    return '\n'.join(out)


def strip_code(text: str) -> str:
    """주석·문자열·문자 리터럴을 제거한다.

    순차 정규식(주석 먼저 → 문자열 나중)은 구조적으로 틀린다:
      - '"' 같은 문자 리터럴 속 큰따옴표가 팬텀 문자열을 열고(문자 리터럴 미인식),
      - 문자열 속 `//`·`/*`(예: "http://…" URL)가 주석으로 오제거되어 따옴표 짝이 깨진다.
    두 경우 모두 뒤따르는 실코드를 삼켜 사용 카운트를 떨어뜨리고 오탐/미탐을 만든다.
    → 단일 좌→우 상태머신으로 주석 vs 문자열/문자 리터럴 우선순위를 위치로 결정한다.
    문자열/문자 리터럴은 줄 단위로 한정(짝 안 맞는 따옴표의 피해를 그 줄로 제한);
    블록 주석만 줄을 넘겨 상태를 유지한다. 단, `\\`-이음 물리줄은 이 줄 경계
    자체가 실제로는 없으므로(phase-2 line-splicing) 상태머신 이전에 합친다.
    """
    text = _splice_continuations(text)
    out = []
    in_block = False
    for line in text.split('\n'):
        res = []
        i, n = 0, len(line)
        while i < n:
            ch = line[i]
            if in_block:                                        # 블록 주석 내부
                if ch == '*' and i + 1 < n and line[i + 1] == '/':
                    in_block = False
                    i += 2
                else:
                    i += 1
                continue
            if ch == '/' and i + 1 < n and line[i + 1] == '*':  # 블록 주석 시작
                in_block = True
                i += 2
                continue
            if ch == '/' and i + 1 < n and line[i + 1] == '/':  # 라인 주석 → 줄 끝까지
                break
            if ch == '"' or ch == "'":                          # 문자열/문자 리터럴 (줄 한정)
                quote = ch
                i += 1
                while i < n:
                    if line[i] == '\\':                         # 이스케이프: 다음 문자 스킵
                        i += 2
                        continue
                    if line[i] == quote:
                        i += 1
                        break
                    i += 1
                res.append(' ')
                continue
            res.append(ch)
            i += 1
        out.append(''.join(res))
    return '\n'.join(out)


def collect_declared(header_texts) -> set:
    names = set()
    for t in header_texts:
        # static inline 정의(헤더 내 {로 끝남)는 제외: ';' 종단 프로토타입만 관심이나,
        # == 1 판정이 헤더 inline(.c 출현 0)을 자연 배제하므로 이름 수집은 단순 전량.
        names.update(DECL_RE.findall(strip_code(t)))
    return names


def count_uses(name: str, c_texts_stripped) -> int:
    pat = re.compile(r'\b' + re.escape(name) + r'\b')
    return sum(len(pat.findall(t)) for t in c_texts_stripped)


def _waived(name: str, c_texts_raw) -> bool:
    pat = re.compile(r'\b' + re.escape(name) + r'\b')
    for t in c_texts_raw:
        for m in pat.finditer(t):
            if WAIVER in t[max(0, m.start() - 200): m.start() + 50]:
                return True
    return False


def find_dead(header_texts, c_texts_raw) -> set:
    declared = collect_declared(header_texts)
    stripped = [strip_code(t) for t in c_texts_raw]
    dead = set()
    for name in declared:
        if count_uses(name, stripped) == 1 and not _waived(name, c_texts_raw):
            dead.add(name)   # .c 출현 정확히 1회 = 정의만 = 사용처 0
    return dead


def _load_baseline() -> set:
    if not BASELINE_FILE.exists():
        return set()
    return {ln.strip() for ln in BASELINE_FILE.read_text().splitlines()
            if ln.strip() and not ln.lstrip().startswith("#")}


def main() -> int:
    headers = [p.read_text(errors="replace") for p in
               list((ROOT / "src").rglob("*.h")) + list((ROOT / "include").rglob("*.h"))]
    c_raw = [p.read_text(errors="replace") for p in (ROOT / "src").rglob("*.c")]
    dead = find_dead(headers, c_raw)
    baseline = _load_baseline()
    new_dead = sorted(dead - baseline)
    stale = sorted(baseline - dead)
    print(f"[check-dead-exports] dead export 후보 {len(dead)} / baseline {len(baseline)}")
    if stale:
        print(f"[INFO] baseline {len(stale)}건 이제 사용됨(시정) — baseline에서 제거 권장: "
              f"{', '.join(stale[:10])}")
    if new_dead:
        print(f"[FAIL] 신규 dead export {len(new_dead)}건 (헤더 선언·사용처0):", file=sys.stderr)
        for n in new_dead:
            print(f"  - {n}  (배선 or 삭제; 의도적이면 PCV_DEAD_EXPORT_OK waiver)", file=sys.stderr)
        return 1
    print("[PASS] 신규 dead export 없음")
    return 0


if __name__ == "__main__":
    sys.exit(main())
