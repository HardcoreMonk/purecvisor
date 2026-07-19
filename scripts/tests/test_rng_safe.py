
"""check_rng_safe.py self-test (Wave B 6 / A02·V11).

① 현행 트리에서 게이트 PASS(exit 0).
② scan_rng_text / extract_pbkdf2_target 단위 인식.
③ 반사실(temp 사본): _fill_random_bytes 에 g_random 폴백 재도입 → 게이트 RED.
④ 반사실(temp 사본): PBKDF2_ITER_TARGET 를 600000 미만으로 낮춤 → 게이트 RED.
추적 소스는 절대 건드리지 않는다 (temp 사본에만 변형 적용).
"""
import os
import sys
import tempfile
import subprocess
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import check_rng_safe as gate

GATE = Path(__file__).resolve().parent.parent / "check_rng_safe.py"
RBAC = gate.RNG_FILES[0]
JWT = gate.RNG_FILES[1]

def _run_gate_on(rbac_text: str, jwt_text: str) -> int:
    """rbac/jwt 사본을 만들어 게이트를 override 경로로 실행."""
    paths = []
    for suffix, text in (("_rbac.c", rbac_text), ("_jwt.c", jwt_text)):
        f = tempfile.NamedTemporaryFile("w", suffix=suffix, delete=False)
        f.write(text)
        f.close()
        paths.append(f.name)
    try:
        r = subprocess.run([sys.executable, str(GATE), *paths],
                           capture_output=True, text=True)
        return r.returncode
    finally:
        for p in paths:
            os.unlink(p)

def test_gate_passes_on_current_tree():
    """① 현행 트리에서 게이트 PASS(exit 0)."""
    r = subprocess.run([sys.executable, str(GATE)], capture_output=True, text=True)
    assert r.returncode == 0, f"{r.stdout}\n{r.stderr}"

def test_scan_current_sources():
    """② 현행: 두 파일 모두 g_random 무·RAND_bytes 유, PBKDF2 target >= 600000."""
    for f in (RBAC, JWT):
        sig = gate.scan_rng_text(f.read_text())
        assert not sig["uses_grandom"], f"{f.name} 에 g_random 잔존"
        assert sig["uses_randbytes"], f"{f.name} 에 RAND_bytes 부재"
    t = gate.extract_pbkdf2_target(RBAC.read_text())
    assert t is not None and t >= gate.PBKDF2_MIN_ITERATIONS

def test_grandom_reintroduced_fails():
    """③ 반사실: rbac.c 의 _fill_random_bytes 에 g_random 폴백 재도입 → RED."""
    rbac = RBAC.read_text()
    anchor = "if (RAND_bytes(buf, (int)len) == 1)"
    assert anchor in rbac, "RNG 헬퍼 포맷 변경?"
    mutated = rbac.replace(
        anchor,
        "for (gsize i = 0; i < len; i++) buf[i] = (guchar)g_random_int_range(0, 256);\n"
        "    if (RAND_bytes(buf, (int)len) == 1)")
    sig = gate.scan_rng_text(mutated)
    assert sig["uses_grandom"]
    assert _run_gate_on(mutated, JWT.read_text()) == 1

def test_pbkdf2_downgrade_fails():
    """④ 반사실: PBKDF2_ITER_TARGET 를 100000 으로 낮춤 → RED."""
    rbac = RBAC.read_text()
    needle = "#define PBKDF2_ITER_TARGET  600000"
    assert needle in rbac, "PBKDF2_ITER_TARGET 정의 포맷 변경?"
    mutated = rbac.replace(needle, "#define PBKDF2_ITER_TARGET  100000")
    assert gate.extract_pbkdf2_target(mutated) == 100000
    assert _run_gate_on(mutated, JWT.read_text()) == 1

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
    print(f"[test_rng_safe] {len(tests) - failed}/{len(tests)} passed")
    sys.exit(1 if failed else 0)
