#!/usr/bin/env python3
                          
                                                                       
                                                                                 
                                                                       
                                                                   
                                                   
 
                      
                                                                                     
                                        

import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import check_audit_hashchain as gate              

GATE = Path(__file__).resolve().parent.parent / "check_audit_hashchain.py"


def _run_with_source(index: int, text: str) -> int:
    with tempfile.NamedTemporaryFile("w", suffix=".c", delete=False) as handle:
        handle.write(text)
        path = handle.name
    try:
        sources = [gate.CHAIN_TARGET, gate.LIFECYCLE_TARGET, gate.MAIN_TARGET,
                   gate.REST_TARGET, gate.TELEMETRY_TARGET]
        sources[index] = Path(path)
        return subprocess.run(
            [sys.executable, str(GATE), *(str(source) for source in sources)],
            capture_output=True, text=True
        ).returncode
    finally:
        os.unlink(path)


def _current_signals() -> dict[str, bool]:
    return gate.analyze(
        gate.CHAIN_TARGET.read_text(),
        gate.LIFECYCLE_TARGET.read_text(),
        gate.MAIN_TARGET.read_text(),
        gate.REST_TARGET.read_text(),
        gate.TELEMETRY_TARGET.read_text(),
    )


def test_gate_passes_on_current_tree():
    result = subprocess.run(
        [sys.executable, str(GATE)], capture_output=True, text=True
    )
    assert result.returncode == 0, f"{result.stdout}\n{result.stderr}"


def test_all_contract_signals_are_present():
    signals = _current_signals()
    assert all(signals.values()), signals


def test_missing_immediate_transaction_fails():
    source = gate.CHAIN_TARGET.read_text()
    mutated = source.replace("BEGIN IMMEDIATE", "BEGIN DEFERRED")
    assert mutated != source
    assert _run_with_source(0, mutated) == 1


def test_missing_unique_predecessor_fails():
    source = gate.CHAIN_TARGET.read_text()
    mutated = source.replace("idx_audit_epoch_prev", "idx_removed_epoch_prev")
    assert mutated != source
    assert _run_with_source(0, mutated) == 1


def test_missing_retention_checkpoint_fails():
    source = gate.CHAIN_TARGET.read_text()
    mutated = source.replace("audit_chain_checkpoint", "removed_chain_anchor")
    assert mutated != source
    assert _run_with_source(0, mutated) == 1


def test_missing_health_wiring_fails():
    source = gate.REST_TARGET.read_text()
    mutated = source.replace('"audit_chain"', '"removed_audit_status"')
    assert mutated != source
    assert _run_with_source(3, mutated) == 1


def test_missing_metric_wiring_fails():
    source = gate.TELEMETRY_TARGET.read_text()
    mutated = source.replace("purecvisor_audit_chain_ok", "removed_chain_metric")
    assert mutated != source
    assert _run_with_source(4, mutated) == 1


if __name__ == "__main__":
    tests = [value for name, value in sorted(globals().items())
             if name.startswith("test_") and callable(value)]
    failed = 0
    for test in tests:
        try:
            test()
            print(f"OK   {test.__name__}")
        except AssertionError as error:
            failed += 1
            print(f"FAIL {test.__name__}: {error}")
    print(f"[test_audit_hashchain] {len(tests) - failed}/{len(tests)} passed")
    raise SystemExit(1 if failed else 0)
