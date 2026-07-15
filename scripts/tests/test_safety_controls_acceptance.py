#!/usr/bin/env python3
"""안전통제 게이트 자기검증 인수 테스트 (Task 3).

설계: docs/superpowers/specs/2026-07-11-safety-control-effect-test-gate-design.md

① 게이트가 현재 레지스트리 상태에서 PASS(exit 0)로 착지한다.
② 감사에서 확인된 실제 무동작(untested-baseline) 통제가 레지스트리에 surface된다
   (gap이 은폐되지 않고 보임) — graceful-drain / alert-silence / sriov-disable.
③ 시정 트랙에서 effect_test로 실제 확인된 통제는 tested로 잠겨 있다
   (session-revoke / vm-op-lock / restart-breaker 재도출이 조용히 되돌려지지 않음).

주의: 계획 초안(2026-07-11 seed)은 session-revoke를 untested-baseline으로 가정했으나,
Task 2 재도출로 session-revoke는 test_session_revoke.sh가 효과를 단언하는 tested다.
이 파일은 그 stale 가정을 반영하지 않고, 현재 contracts/safety_controls.json 실제
상태를 기준으로 단언한다.

재도출 이력(Task 4, 2026-07-14): PR #25가 backup-retention을 tested로 승격했고, 이
배치가 alert-silence/sriov-disable/graceful-drain을 추가로 tested 승격했다. 그 결과
untested-baseline은 self-healing-restart 1건만 남았다.

재도출 이력(2026-07-15): self-healing-restart 결정 로직(running-guard +
virDomainCreate 3분기)을 self_healing_restart.c로 추출해
test_self_healing_restart.c::test_running_guard_skip 반사실 단언으로 tested
승격 — 마지막 untested-baseline 항목이 해소되어 감사 무동작 통제 리스트가
빈다. 이 파일의 기대 리스트도 그 실제 상태로 재도출한다(드리프트 시 이
acceptance 자체가 즉시 FAIL하도록 Makefile check-safety-controls에 배선됨).
"""
import re
import sys
import subprocess
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
GATE = ROOT / "scripts" / "check_safety_controls.py"
REGISTRY = ROOT / "contracts" / "safety_controls.json"
BASELINE = ROOT / "contracts" / "safety_controls_baseline.txt"


def test_gate_passes():
    r = subprocess.run([sys.executable, str(ROOT / "scripts" / "check_safety_controls.py")],
                        capture_output=True, text=True)
    assert r.returncode == 0, f"{r.stdout}\n{r.stderr}"


def test_noop_controls_surfaced():
    """감사에서 확인된 실제 무동작 통제가 있다면 surface(gap 은폐 없음).

    2026-07-15: self-healing-restart 승격으로 untested-baseline 항목이 0건이
    됐다 — 알려진 gap 리스트가 비어 있는 것 자체가 현재 실제 상태다."""
    reg = json.loads(REGISTRY.read_text())
    known_gaps = []
    untested = {cid for cid, spec in reg.items() if spec.get("status") == "untested-baseline"}
    assert untested == set(known_gaps), \
        f"알려진 무동작 통제 리스트와 실제 untested-baseline 상태가 어긋남: {sorted(untested)}"


def test_tested_controls_locked_in():
    """시정 트랙에서 effect_test로 확인된 통제는 tested — 재도출이 조용히 되돌려지지 않음."""
    reg = json.loads(REGISTRY.read_text())
    for cid in ["session-revoke", "vm-op-lock", "restart-breaker",
                "backup-retention", "alert-silence", "sriov-disable", "graceful-drain",
                "hips-approval-expiry", "apikey-role-enforce", "vm-create-iso-validation",
                "isolated-network-drop", "qos-rehydrate", "self-healing-restart"]:
        assert cid in reg and reg[cid]["status"] == "tested", \
            f"시정 확인 통제 {cid}가 tested에서 이탈"


def test_duplicate_key_fails():
    """레지스트리 최상위 키 중복 주입(ADR-0025 반사실) — json.loads의 last-wins 사각지대에서
    status가 조용히 뒤집히는 것을 막는 중복 감지가 실제로 발동해야 한다."""
    orig = REGISTRY.read_text()
    try:
        m = re.search(r'"graceful-drain":\s*\{[^{}]*\}', orig)
        assert m, "graceful-drain 블록을 찾지 못함 — 레지스트리 포맷 변경?"
        dup = orig[:m.end()] + ",\n  " + m.group(0) + orig[m.end():]
        REGISTRY.write_text(dup)
        r = subprocess.run([sys.executable, str(GATE)], capture_output=True, text=True)
        assert r.returncode == 1 and "중복 키" in r.stderr, f"{r.stdout}\n{r.stderr}"
    finally:
        REGISTRY.write_text(orig)


def test_tested_in_baseline_fails():
    """tested 통제가 baseline에 잔존(ADR-0025 반사실) — 승격 후 baseline 미정리로
    tested→untested가 조용히 강등 마스킹되는 것을 막는 체크가 실제로 발동해야 한다."""
    orig = BASELINE.read_text()
    try:
        BASELINE.write_text(orig + "\nsession-revoke\n")
        r = subprocess.run([sys.executable, str(GATE)], capture_output=True, text=True)
        assert r.returncode == 1 and "baseline에 잔존" in r.stderr, f"{r.stdout}\n{r.stderr}"
    finally:
        BASELINE.write_text(orig)


if __name__ == "__main__":
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for t in tests:
        t()
        print(f"  ok  {t.__name__}")
    print("OK")
