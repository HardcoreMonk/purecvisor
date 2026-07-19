
"""check_comment_coverage.py — 소스 파일 최소 자기설명(파일 헤더 + 최소 주석 밀도) 게이트.

설계: docs/superpowers/specs/2026-07-18-comment-coverage-gate-design.md
표준: docs/SOURCE_CODE_COMMENTING_STANDARD.md (이중 주석 모델)

불변식: src/·include/ 의 각 .c/.h 는 compliant(파일 헤더 존재 + 주석 비율 ≥ FLOOR).
신규/방치 non-compliant 파일은 baseline·waiver 밖이면 FAIL(ADR-0025 반사실, 단조감소 래칫).

사용:
  python3 scripts/check_comment_coverage.py            # 검사 (exit 0 pass / 1 fail)
  python3 scripts/check_comment_coverage.py --generate # baseline 재생성
  python3 scripts/check_comment_coverage.py --self-test # 게이트 자기검증(반사실)
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BASELINE_FILE = ROOT / "scripts" / "comment_coverage_baseline.txt"
WAIVER = "PCV_COMMENT_COVERAGE_OK"
SCAN_DIRS = ["src", "include"]
EXTS = {".c", ".h"}
FLOOR = 0.10
HEADER_WINDOW = 10

def classify(text):
    """(has_header, ratio) 반환.

    주석 판정: 물리줄 단위 좌→우 스캔으로 라인 주석(//)·블록 주석(/* */)만 센다
    (문자열 리터럴 속 // 오탐은 밀도 지표에 무의미하므로 단순 규칙 유지 — 이 게이트는
    보안 파서가 아니라 밀도 근사치다). code = 비공백·비주석·비전처리기 줄.
    has_header: 상단 HEADER_WINDOW 물리줄 안에서 첫 '의미 있는' 줄이 주석이면 True.
      - 헤더는 실코드보다 앞서야 한다(코드 뒤 주석은 헤더 아님).
      - 전처리기(#include/#ifndef 등)·공백은 헤더 탐색을 종료시키지 않는다(그 앞/뒤 헤더 허용).
    """
    comment = code = 0
    in_block = False
    header = False
    header_decided = False
    lines = text.split("\n")
    for idx, raw in enumerate(lines):
        s = raw.strip()

        is_comment_line = False
        was_in_block = in_block
        if in_block:
            is_comment_line = True
            if "*/" in s:
                in_block = False
        elif s.startswith("//"):
            is_comment_line = True
        elif s.startswith("/*"):
            is_comment_line = True
            if "*/" not in s[1:]:
                in_block = True
        elif s.startswith("*") and s:
            is_comment_line = True
        if s:
            if is_comment_line:
                comment += 1
            elif s.startswith("#"):
                pass
            else:
                code += 1

        if not header_decided and idx < HEADER_WINDOW:
            if not s or s.startswith("#"):
                pass
            elif is_comment_line or was_in_block:
                header = True
                header_decided = True
            else:
                header_decided = True
    ratio = comment / (comment + code) if (comment + code) else 1.0
    return header, ratio

def scan():
    """{relpath: (has_header, ratio, compliant, waived)} 반환."""
    result = {}
    for d in SCAN_DIRS:
        for path in sorted((ROOT / d).rglob("*")):
            if path.suffix not in EXTS or not path.is_file():
                continue
            rel = path.relative_to(ROOT).as_posix()
            text = path.read_text(encoding="utf-8", errors="replace")
            header, ratio = classify(text)
            waived = WAIVER in text
            compliant = header and ratio >= FLOOR
            result[rel] = (header, ratio, compliant, waived)
    return result

def load_baseline():
    if not BASELINE_FILE.exists():
        return set()
    out = set()
    for line in BASELINE_FILE.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            out.add(line)
    return out

def write_baseline(noncompliant):
    header = (
        "# 주석 커버리지 착지 baseline (2026-07-18)\n"
        "#\n"
        "# 파일 헤더 부재 또는 주석 비율 < %d%% 인 기존 .c/.h 명단(단조감소 래칫).\n"
        "# 이 명단 + PCV_COMMENT_COVERAGE_OK waiver 밖의 신규/방치 non-compliant 만 FAIL.\n"
        "# 보강해 compliant 가 되면 이 명단에서 제거한다(설계: comment-coverage-gate-design).\n"
        % int(FLOOR * 100)
    )
    body = "\n".join(sorted(noncompliant))
    BASELINE_FILE.write_text(header + body + "\n", encoding="utf-8")

def run_check():
    files = scan()
    baseline = load_baseline()
    noncompliant = {r for r, (_, _, c, w) in files.items() if not c and not w}

    new_violations = sorted(noncompliant - baseline)

    now_ok = sorted(b for b in baseline if b in files and files[b][2])

    stale = sorted(b for b in baseline if b not in files)

    total = len(files)
    compliant_n = sum(1 for _, (_, _, c, _) in files.items() if c)
    print(
        f"[check-comment-coverage] {compliant_n}/{total} compliant "
        f"(헤더+비율≥{int(FLOOR*100)}%), baseline 예외 {len(baseline & set(files))}건"
    )
    for b in now_ok:
        print(f"[INFO] baseline 항목 이제 compliant(제거 권장): {b}")
    for b in stale:
        print(f"[INFO] baseline 항목 파일 없음(prune 권장): {b}")

    if new_violations:
        print("\n[FAIL] 파일 헤더 부재 또는 주석 비율 < %d%% (baseline·waiver 밖):" % int(FLOOR * 100))
        for r in new_violations:
            hdr, ratio, _, _ = files[r]
            why = []
            if not hdr:
                why.append("헤더없음")
            if ratio < FLOOR:
                why.append(f"비율{ratio*100:.0f}%")
            print(f"  - {r}  ({', '.join(why)})")
        print(
            "\n시정: 파일 상단에 목적/아키텍처-위치 헤더 주석 추가(표준 §3) + 판단근거 주석.\n"
            "정당한 최소 파일이면 파일에 주석 '%s <사유>' 추가." % WAIVER
        )
        return 1
    print("[PASS] 신규 주석 커버리지 위반 없음")
    return 0

def self_test():
    """반사실: 합성 non-compliant(헤더없음·저밀도)를 게이트가 위반으로 잡는지 자기검증."""
    ok = True

    h, r = classify("int a=1;\nint b=2;\nint c=3;\n")
    if h or r >= FLOOR:
        print("[SELF-TEST FAIL] 헤더없음·무주석 파일이 compliant로 판정됨"); ok = False

    h2, r2 = classify("/* @file 목적 설명\n * 판단 근거 */\nint a=1;\n")
    if not (h2 and r2 >= FLOOR):
        print("[SELF-TEST FAIL] 정상 헤더 파일이 non-compliant로 판정됨"); ok = False

    h3, _ = classify("int a=1;\n// 뒤늦은 주석\n// 또 주석\n")
    if h3:
        print("[SELF-TEST FAIL] 상단 헤더 없는 파일이 헤더보유로 판정됨(window 확인)"); ok = False
    print("[SELF-TEST PASS] 게이트가 non-compliant를 정확히 식별" if ok else "[SELF-TEST FAILED]")
    return 0 if ok else 1

def main():
    if "--generate" in sys.argv:
        files = scan()
        nc = sorted(r for r, (_, _, c, w) in files.items() if not c and not w)
        write_baseline(nc)
        print(f"[generate] baseline {len(nc)}건 기록: {BASELINE_FILE.relative_to(ROOT)}")
        return 0
    if "--self-test" in sys.argv:
        return self_test()
    return run_check()

if __name__ == "__main__":
    sys.exit(main())
