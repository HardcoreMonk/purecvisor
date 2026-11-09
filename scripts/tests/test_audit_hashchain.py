#!/usr/bin/env python3
                          
                                                                       
                                                                                 
                                                                       
                                                                   
                                                   
 
                      
                                                                                     
                                        

import contextlib
import io
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import check_audit_hashchain as gate              
import rpc_extract

GATE = Path(__file__).resolve().parent.parent / "check_audit_hashchain.py"
REPO = Path(__file__).resolve().parents[2]
IMPACT_FN = "pcv_rest_audit_chain_health_impact"


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
        gate.rest_text(),
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
    source = gate.rest_text()
    mutated = source.replace('"audit_chain"', '"removed_audit_status"')
    assert mutated != source
    assert _run_with_source(3, mutated) == 1


def test_historical_break_promoted_to_critical_fails():
    source = gate.rest_text()
    mutated = source.replace(
        "return PCV_REST_AUDIT_HEALTH_INFORMATIONAL;",
        "return PCV_REST_AUDIT_HEALTH_CRITICAL;",
    )
    assert mutated != source
    assert _run_with_source(3, mutated) == 1


def test_missing_metric_wiring_fails():
    source = gate.TELEMETRY_TARGET.read_text()
    mutated = source.replace("purecvisor_audit_chain_ok", "removed_chain_metric")
    assert mutated != source
    assert _run_with_source(4, mutated) == 1

def _impact_definition(text: str) -> str:

    body = rpc_extract.function_body_in(text, IMPACT_FN)
    assert body is not None, "정보 등급 함수 정의를 찾지 못함 — 소스 포맷 변경?"
    name_at = re.search(rf"^{IMPACT_FN}\s*\(", text, re.M).start()
    start = text.rfind("\n", 0, name_at - 1) + 1
    return text[start:text.index(body, name_at) + len(body)] + "\n"

@contextlib.contextmanager
def _rest_root(files: dict):

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        for rel, text in files.items():
            (root / rel).parent.mkdir(parents=True, exist_ok=True)
            (root / rel).write_text(text)
        saved = gate.ROOT
        gate.ROOT = root
        try:
            yield root
        finally:
            gate.ROOT = saved

def _main_rc() -> tuple:
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        rc = gate.main([str(GATE)])
    return rc, out.getvalue() + err.getvalue()

def test_impact_only_in_split_file_passes_and_absent_fails():

    family = {p.relative_to(REPO).as_posix(): p.read_text() for p in rpc_extract.rest_family()}
    definition = _impact_definition("".join(family.values()))
    base = {rel: text.replace(definition, "") for rel, text in family.items()}
    assert sum(text.count(definition) for text in base.values()) == 0
    policy = "src/api/rest_policy.c"
    with _rest_root({**base, policy: base.get(policy, "") + definition}):
        rc, out = _main_rc()
    assert rc == 0, f"분할 파일의 정보 등급 함수를 못 읽음:\n{out}"
    with _rest_root(base):
        rc, out = _main_rc()
    assert rc == 1 and "legacy break" in out, f"앵커 0건인데 RED가 아님:\n{out}"

def test_missing_rest_family_is_error():

    with _rest_root({"src/api/rest_policy.c": "int x;\n"}):
        rc, out = _main_rc()
    assert rc == 2 and "rest 계열" in out, out


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
