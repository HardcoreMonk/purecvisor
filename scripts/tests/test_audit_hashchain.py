#!/usr/bin/env python3
"""check_audit_hashchain.py self-test (Wave B 3-b / A09·2.9).

① 현행 트리에서 게이트 PASS(exit 0).
② analyze() 가 현행 pcv_audit.c 에서 모든 체인 신호를 True 로 인식.
③ 반사실(temp 사본): INSERT 에서 prev_hash/rec_hash 컬럼 제거 → 게이트 RED.
④ 반사실(temp 사본): pcv_audit_verify_chain 정의 제거 → 게이트 RED.
추적 소스는 절대 건드리지 않는다 (temp 사본에만 변형 적용).
"""
import os
import sys
import tempfile
import subprocess
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import check_audit_hashchain as gate  # noqa: E402

GATE = Path(__file__).resolve().parent.parent / "check_audit_hashchain.py"
TARGET = gate.TARGET


def _run_gate_on(text: str) -> int:
    with tempfile.NamedTemporaryFile("w", suffix=".c", delete=False) as f:
        f.write(text)
        tmp = f.name
    try:
        r = subprocess.run([sys.executable, str(GATE), tmp],
                           capture_output=True, text=True)
        return r.returncode
    finally:
        os.unlink(tmp)


def test_gate_passes_on_current_tree():
    """① 현행 트리에서 게이트 PASS(exit 0)."""
    r = subprocess.run([sys.executable, str(GATE)], capture_output=True, text=True)
    assert r.returncode == 0, f"{r.stdout}\n{r.stderr}"


def test_analyze_current_source_all_signals():
    """② 현행 pcv_audit.c: INSERT 컬럼·바인딩·계산·검증함수 모두 존재."""
    sig = gate.analyze(TARGET.read_text())
    assert sig["insert_found"]
    assert sig["insert_has_prev"] and sig["insert_has_rec"]
    assert sig["binds_prev"] and sig["binds_rec"]
    assert sig["computes_rechash"]
    assert sig["has_verify_fn"]


def test_insert_without_chain_columns_fails():
    """③ 반사실: INSERT 컬럼 목록에서 prev_hash/rec_hash 제거 → RED."""
    orig = TARGET.read_text()
    needle = ",prev_hash,rec_hash)"
    assert needle in orig, "INSERT 체인 컬럼을 찾지 못함 — 소스 포맷 변경?"
    mutated = orig.replace(needle, ")")
    assert mutated != orig
    # analyze 로직 단위 확인
    sig = gate.analyze(mutated)
    assert not (sig["insert_has_prev"] and sig["insert_has_rec"])
    # 실제 게이트 exit 1
    assert _run_gate_on(mutated) == 1


def test_missing_verify_fn_fails():
    """④ 반사실: pcv_audit_verify_chain 정의/호출 제거 → RED."""
    orig = TARGET.read_text()
    assert "pcv_audit_verify_chain" in orig
    mutated = orig.replace("pcv_audit_verify_chain", "pcv_audit_disabled_chain")
    sig = gate.analyze(mutated)
    assert not sig["has_verify_fn"]
    assert _run_gate_on(mutated) == 1


if __name__ == "__main__":
    tests = [v for k, v in sorted(globals().items())
             if k.startswith("test_") and callable(v)]
    failed = 0
    for t in tests:
        try:
            t()
            print(f"OK   {t.__name__}")
        except AssertionError as e:
            failed += 1
            print(f"FAIL {t.__name__}: {e}")
    print(f"[test_audit_hashchain] {len(tests) - failed}/{len(tests)} passed")
    sys.exit(1 if failed else 0)
