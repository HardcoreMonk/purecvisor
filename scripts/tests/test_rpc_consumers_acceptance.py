
"""소비/고아 게이트 반사실 self-test (ADR-0025 의무 2, Task 5).

check_rpc_consumers.py 의 존재 이유는 "불변식이 깨지면 RED가 된다"는 것 자체다.
이 파일은 그 반사실 5종을 직접 주입해 확인한다:
  1. baseline 그대로면 PASS(현재 등록 250 / 소비 226 / 고아 24 / baseline 24).
  2. dispatcher.c에 소비자 0인 신규 route 주입 → 신규 ORPHAN → FAIL.
  3. rest_server.c에 미등록 메서드를 소비하는 _build_rpc 주입 → 소비⊄등록 → FAIL.
  4. baseline에서 기존 고아 1건을 삭제(축소) → 그 메서드가 신규 고아로 재부상 → FAIL.
  5. test-covered 메서드를 dead-candidate로 표기 → MISLABEL → FAIL (정직 self-check).

Task 4의 test_orphan_gate.py 가 이미 #1/#2를 더 작은 형태로 커버한다 — 여기서는
ADR-0025가 요구하는 4종 전체를 한 파일에 모은 완결판(경미한 중복은 의도됨).

주의(SAFE RESTORE): 모든 주입 테스트는 원본을 read_text()로 보존한 뒤
try/finally로 무조건 write_text() 복원한다. assertion 실패로도 소스가
더러워진 채 남지 않아야 한다 — 절대 cp/sed 셸아웃으로 복원하지 않는다.
"""
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
GATE = ROOT / "scripts" / "check_rpc_consumers.py"
DISP = ROOT / "src" / "api" / "dispatcher.c"
REST = ROOT / "src" / "api" / "rest_server.c"
BASE = ROOT / "contracts" / "rpc_orphan_baseline.json"

def _run():
    return subprocess.run([sys.executable, str(GATE)], capture_output=True, text=True)

def test_passes_at_baseline():
    """정상 상태 — 게이트는 exit 0."""
    r = _run()
    assert r.returncode == 0, f"{r.stdout}\n{r.stderr}"

def test_new_orphan_is_blocked():
    """등록만 되고 어떤 경로로도 소비되지 않는 route 주입 → 신규 고아 FAIL."""
    orig = DISP.read_text()
    try:
        DISP.write_text(
            orig + '\n    g_hash_table_insert(g_rpc_routes, "test.__orphan_probe__", NULL);\n'
        )
        r = _run()
        assert r.returncode == 1 and "ORPHAN" in r.stderr, f"{r.stdout}\n{r.stderr}"
    finally:
        DISP.write_text(orig)

def test_consumed_but_unregistered_is_blocked():
    """REST 브릿지가 미등록 메서드를 소비 → 소비⊆등록 위반 FAIL."""
    orig = REST.read_text()
    try:
        REST.write_text(orig + '\n    rpc = _build_rpc("test.__unregistered__", NULL);\n')
        r = _run()
        assert r.returncode == 1, f"소비-미등록 미차단:\n{r.stdout}\n{r.stderr}"
    finally:
        REST.write_text(orig)

def test_dead_candidate_mislabel_is_blocked():
    """test-covered 메서드를 dead-candidate로 표기하면 게이트 FAIL (정직 self-check, quota.get 사건 재발방지)."""
    orig = BASE.read_text()
    try:
        d = json.loads(orig)

        d["orphans"]["quota.get"]["reason"] = "dead-candidate"
        BASE.write_text(json.dumps(d, indent=2, ensure_ascii=False))
        r = _run()
        assert r.returncode == 1 and "MISLABEL" in r.stderr, f"{r.stdout}\n{r.stderr}"
    finally:
        BASE.write_text(orig)

def test_baseline_deflation_is_blocked():
    """고아를 baseline에 추가로 은폐하려는 시도는 정당하나, 실제 고아가 아닌 항목 추가는 무해.
    핵심 반사실: baseline에서 실제 고아 하나를 빼면(축소) 그 고아가 신규로 떠서 FAIL."""
    orig = BASE.read_text()
    try:
        d = json.loads(orig)
        victim = next(iter(d["orphans"]))
        del d["orphans"][victim]
        BASE.write_text(json.dumps(d, indent=2))
        r = _run()
        assert r.returncode == 1 and victim in r.stderr, f"{r.stdout}\n{r.stderr}"
    finally:
        BASE.write_text(orig)

if __name__ == "__main__":
    for k, v in sorted(globals().items()):
        if k.startswith("test_"):
            v()
            print(f"  ok  {k}")
    print("OK")
