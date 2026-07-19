
"""고아 불변식 단위테스트 (Task 4).

등록 − 전소비 ⊆ baseline(rpc_orphan_baseline.json) 을 확인한다. 정식
반사실 인수 테스트(baseline 축소, REST 소비-미등록 주입 등)는
Task 5의 test_rpc_consumers_acceptance.py 몫 — 여기서는 baseline 착지 +
dispatcher.c에 소비자 0인 신규 route 주입 시 ORPHAN FAIL만 확인한다.
"""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
GATE = ROOT / "scripts" / "check_rpc_consumers.py"
DISP = ROOT / "src" / "api" / "dispatcher.c"

def _run():
    return subprocess.run([sys.executable, str(GATE)], capture_output=True, text=True)

def test_gate_passes_at_orphan_baseline():
    r = _run()
    assert r.returncode == 0, f"{r.stdout}\n{r.stderr}"
    assert "고아 24 / baseline 24" in r.stdout, f"{r.stdout}"

def test_new_orphan_blocked():
    """dispatcher에 소비자 0인 route 주입 → 고아 FAIL."""
    orig = DISP.read_text()
    try:
        DISP.write_text(
            orig + '\n    g_hash_table_insert(g_rpc_routes, "test.__orphan_probe__", NULL);\n'
        )
        r = _run()
        assert r.returncode == 1, f"신규 고아를 게이트가 못 막음:\n{r.stdout}\n{r.stderr}"
        assert "ORPHAN" in r.stderr, f"ORPHAN FAIL 메시지 누락:\n{r.stdout}\n{r.stderr}"
        assert "test.__orphan_probe__" in r.stderr
    finally:
        DISP.write_text(orig)

if __name__ == "__main__":
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for t in tests:
        t()
        print(f"  ok  {t.__name__}")
    print("OK")
