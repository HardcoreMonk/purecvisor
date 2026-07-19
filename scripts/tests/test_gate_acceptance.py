
"""게이트 자기검증 인수 테스트 (Task 6).

① 게이트가 baseline 상태에서 PASS(exit 0)로 착지한다.
② baseline 이 감사 부록 A 대표 파손을 재현한다.
③ baseline 밖 신규 파손을 게이트가 FAIL 시킨다(회귀 차단).
"""
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
GATE = ROOT / "scripts" / "check_rpc_param_contract.py"

def test_gate_passes_at_baseline():
    r = subprocess.run([sys.executable, str(GATE)], capture_output=True, text=True)
    assert r.returncode == 0, f"게이트 미착지:\n{r.stdout}\n{r.stderr}"

def test_baseline_reproduces_audit_16():
    base = json.loads((ROOT / "contracts" / "rpc_param_baseline.json").read_text())
    methods = {e["method"] for e in base["known_consumer_mismatches"]}

    for m in ["network.mode_set", "device.nic.list", "ovn.acl.add",
              "container.snapshot.create"]:
        assert m in methods, f"감사 확증 {m}이 게이트 재현에서 누락"

def test_new_break_is_blocked():
    """baseline 밖 신규 위반을 게이트가 FAIL 시키는지 — registry 에 가짜 required 주입."""
    reg = ROOT / "contracts" / "rpc_params.json"
    orig = reg.read_text()
    try:
        d = json.loads(orig)

        d["storage.pool.health"] = {"required": ["__never_sent__"], "optional": []}
        reg.write_text(json.dumps(d, indent=2))
        r = subprocess.run([sys.executable, str(GATE)], capture_output=True, text=True)
        assert r.returncode == 1, f"신규 위반을 게이트가 못 막음:\n{r.stdout}\n{r.stderr}"
    finally:
        reg.write_text(orig)

def test_dead_method_reference_blocked():
    """B1: 레지스트리가 dispatcher 미등록 핸들러를 참조하면 keys_via 스킵과 구분해 FAIL(DEAD-METHOD)."""
    reg = ROOT / "contracts" / "rpc_params.json"
    orig = reg.read_text()
    try:
        d = json.loads(orig)
        d["test.__dead_method_probe__"] = {"required": ["name"], "optional": []}
        reg.write_text(json.dumps(d, indent=2))
        r = subprocess.run([sys.executable, str(GATE)], capture_output=True, text=True)
        assert r.returncode == 1, f"죽은 메서드 참조를 게이트가 못 막음:\n{r.stdout}\n{r.stderr}"
        assert "DEAD-METHOD" in r.stderr, f"DEAD-METHOD FAIL 메시지 누락:\n{r.stdout}\n{r.stderr}"
    finally:
        reg.write_text(orig)

if __name__ == "__main__":
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for t in tests:
        t()
        print(f"  ok  {t.__name__}")
    print("OK")
