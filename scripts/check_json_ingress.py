#!/usr/bin/env python3
"""check_json_ingress.py — JSON 파싱 초크포인트 게이트.
데몬 경계 파일에서 waiver 없는 raw json_parser_load_from_data를 래칫 차단.
설계: docs/superpowers/specs/2026-07-11-json-ingress-chokepoint-gate-design.md
"""
import re, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
INGRESS = ["src/api/ws_server.c", "src/api/uds_server.c", "src/api/grpc_server.c",
           "src/api/dispatcher.c", "src/api/rest_server.c"]
RAW_RE = re.compile(r'\bjson_parser_load_from_data\s*\(')
WAIVER = "PCV_PARSE_TRUSTED"
BASELINE_FILE = ROOT / "scripts" / "json_ingress_baseline.txt"


def _strip_block_comments(text: str) -> str:
    """/* ... */ 블록 주석을 제거하되, 걸친 줄 수만큼 개행으로 대체해
    라인 번호·인접성(직전 줄 waiver 판정)을 보존한다."""
    return re.sub(r'/\*.*?\*/',
                  lambda m: '\n' * m.group().count('\n'),
                  text, flags=re.DOTALL)


# 알려진 한계(로직 변경 아님, 기록용):
#   (a) waiver(PCV_PARSE_TRUSTED)는 같은/직전 "물리" 줄에 있어야 인식된다 — 여러 줄에
#       걸친 블록주석 안에 waiver를 두면 직전 물리 줄 판정에서 벗어나 위반으로 fail-safe될 수 있음.
#   (b) 같은 줄에서 실제 호출과 나란히 C 문자열 리터럴 안에 `/*`/`*/`가 있으면 블록주석
#       스트리퍼가 오탐지할 수 있음(in-tree에는 그런 코드 없음, 이론상 한계만 기록).
def count_violations(text: str) -> int:
    orig = text.splitlines()
    # 블록주석을 개행 보존하며 제거 → 실제 호출 탐지용. 라인 수·인덱스는 orig와 정렬.
    # 단, waiver(/* PCV_PARSE_TRUSTED */)도 블록주석이라 여기서 사라지므로 waiver 판정은
    # 원본 라인(orig) 대상으로 한다.
    stripped = _strip_block_comments(text).splitlines()
    viol = 0
    for i, sline in enumerate(stripped):
        code = re.sub(r'//.*', '', sline)          # 라인 주석 제거
        if not RAW_RE.search(code):                # 주석(라인/블록) 밖의 실제 호출만
            continue
        line = orig[i]
        prev = orig[i - 1] if i > 0 else ""
        if WAIVER in line or WAIVER in prev:       # waiver: 같은 줄/직전 줄(원본 기준)
            continue
        viol += 1
    return viol


def main() -> int:
    total = 0
    per_file = {}
    for rel in INGRESS:
        p = ROOT / rel
        if not p.exists():
            continue
        v = count_violations(p.read_text(errors="replace"))
        per_file[rel] = v
        total += v
    baseline = int(BASELINE_FILE.read_text().strip()) if BASELINE_FILE.exists() else 0
    print(f"[check-json-ingress] 경계 파일 raw 파싱(waiver 제외) {total} / baseline {baseline}")
    for rel, v in per_file.items():
        if v:
            print(f"    {rel}: {v}")
    if total > baseline:
        print(f"[FAIL] 무가드 파싱 {total} > baseline {baseline} — "
              f"신규 raw json_parser_load_from_data. 래퍼(pcv_rpc_parse_guarded) 경유 또는 "
              f"waiver(PCV_PARSE_TRUSTED) 필요.", file=sys.stderr)
        return 1
    if total < baseline:
        print(f"[INFO] 위반 {total} < baseline {baseline} — baseline을 {total}로 낮추세요(래칫).")
    print("[PASS] 신규 무가드 경계 파싱 없음")
    return 0


if __name__ == "__main__":
    sys.exit(main())
