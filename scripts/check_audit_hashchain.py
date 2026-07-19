
"""check_audit_hashchain.py — 감사 로그 해시체인 정적 계약 게이트 (Wave B 3-b / A09·2.9).

[목적]
  ADR-0025 반사실 게이트. src/modules/audit/pcv_audit.c 의 감사 INSERT 경로가
  해시체인(prev_hash/rec_hash)을 계산·저장하지 않으면 FAIL 한다. 누군가 실수로
  체인 컬럼을 INSERT 에서 빼거나, rec_hash 계산/검증 함수를 제거하면 감사 로그가
  다시 위변조 탐지 불가 상태로 조용히 회귀하는 것을 막는다.

[검사 대상]
  src/modules/audit/pcv_audit.c

[검사 항목]
  ① audit_log INSERT 문에 prev_hash·rec_hash 컬럼이 모두 존재.
  ② 워커가 rec_hash 를 계산(_audit_rec_hash 호출)하고 prev_hash·rec_hash 를 바인딩.
  ③ 검증 함수 pcv_audit_verify_chain 이 정의됨.

[종료 코드]
  0: 해시체인 계약 통과
  1: 계약 위반 (체인 미계산/미저장/검증함수 부재)
  2: 대상 파일 파싱 실패
"""
from __future__ import annotations
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TARGET = ROOT / "src" / "modules" / "audit" / "pcv_audit.c"

INSERT_RE = re.compile(
    r'INSERT\s+INTO\s+audit_log\s*\((?P<cols>[^)]*)\)',
    re.IGNORECASE | re.DOTALL,
)

_STR_JOIN = re.compile(r'"\s*"', re.DOTALL)
BIND_RE = re.compile(r'sqlite3_bind_text\s*\([^;]*\b(prev_hash|rec_hash)\b')
RECHASH_CALL_RE = re.compile(r'\b_audit_rec_hash\s*\(')
VERIFY_FN_RE = re.compile(r'\bpcv_audit_verify_chain\s*\(')

def _flatten_c_string(s: str) -> str:
    """인접한 C 문자열 리터럴( "a" "b" )을 이어붙인다 (컬럼 목록 평탄화용)."""
    return _STR_JOIN.sub("", s)

def analyze(text: str) -> dict:
    """대상 소스 텍스트에서 해시체인 계약 신호를 추출한다."""
    flat = _flatten_c_string(text)

    insert_cols = ""
    m = INSERT_RE.search(flat)
    if m:
        insert_cols = m.group("cols")

    insert_has_prev = bool(re.search(r'\bprev_hash\b', insert_cols))
    insert_has_rec = bool(re.search(r'\brec_hash\b', insert_cols))

    binds = set(BIND_RE.findall(text))
    return {
        "insert_found": bool(m),
        "insert_has_prev": insert_has_prev,
        "insert_has_rec": insert_has_rec,
        "binds_prev": "prev_hash" in binds,
        "binds_rec": "rec_hash" in binds,
        "computes_rechash": bool(RECHASH_CALL_RE.search(text)),
        "has_verify_fn": bool(VERIFY_FN_RE.search(text)),
    }

def failures(sig: dict) -> list[str]:
    fails: list[str] = []
    if not sig["insert_found"]:
        fails.append("audit_log INSERT 문을 찾지 못함 — 감사 기록 경로 파손?")
        return fails
    if not (sig["insert_has_prev"] and sig["insert_has_rec"]):
        fails.append("INSERT 문에 prev_hash/rec_hash 컬럼 누락 — 해시체인 미저장")
    if not (sig["binds_prev"] and sig["binds_rec"]):
        fails.append("prev_hash/rec_hash 바인딩 누락 — 체인 값이 실제로 기록되지 않음")
    if not sig["computes_rechash"]:
        fails.append("_audit_rec_hash() 호출 부재 — rec_hash 미계산")
    if not sig["has_verify_fn"]:
        fails.append("pcv_audit_verify_chain() 정의 부재 — 검증 함수 제거됨")
    return fails

def main(argv: list[str]) -> int:
    target = Path(argv[1]) if len(argv) > 1 else TARGET
    if not target.exists():
        print(f"ERROR: {target} 미존재", file=sys.stderr)
        return 2
    text = target.read_text(errors="replace")
    sig = analyze(text)
    fails = failures(sig)

    print(f"[check-audit-hashchain] 대상: {target.name}")
    print(f"[check-audit-hashchain] INSERT prev/rec={sig['insert_has_prev']}/{sig['insert_has_rec']} "
          f"bind prev/rec={sig['binds_prev']}/{sig['binds_rec']} "
          f"compute={sig['computes_rechash']} verify_fn={sig['has_verify_fn']}")

    if fails:
        print("\033[31m[FAIL]\033[0m 감사 해시체인 계약 위반:", file=sys.stderr)
        for f in fails:
            print(f"  - {f}", file=sys.stderr)
        print("       근거: docs/operations 2026-07-16 보안 시정 로드맵 Item 3-b (A09/2.9)",
              file=sys.stderr)
        return 1

    print("\033[32m[PASS]\033[0m 감사 INSERT 경로가 prev_hash/rec_hash 를 계산·저장하고 "
          "검증 함수가 존재 (A09/2.9)")
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv))
