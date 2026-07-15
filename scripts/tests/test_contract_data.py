import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
REG = json.loads((ROOT / "contracts" / "rpc_params.json").read_text())
BASE = json.loads((ROOT / "contracts" / "rpc_param_baseline.json").read_text())

def test_registry_shape():
    for m, spec in REG.items():
        assert set(spec) <= {"required", "optional", "keys_via"}, m
        assert isinstance(spec.get("required", []), list), m
        assert isinstance(spec.get("optional", []), list), m

def test_baseline_methods_in_registry():
    for e in BASE["known_consumer_mismatches"]:
        assert e["method"] in REG, f"baseline 메서드 {e['method']}가 레지스트리 부재"
        assert e["consumer"] in {"cli", "fe"}, e

def test_baseline_covers_all_24():
    # 감사 부록 A: -32602 파손 16 + 무시 클래스는 WARN이라 baseline 제외.
    # TUI 제거(src/tui/ 삭제 + 게이트 TUI_RE 제거)로 tui consumer 7건이
    # baseline에서 소멸 → 잔여 cli 15건이 새 하한선.
    hard = {(e["method"], e["consumer"]) for e in BASE["known_consumer_mismatches"]}
    assert len(hard) >= 15, f"TUI 제거 후 cli 15건 미달: {len(hard)}"


if __name__ == "__main__":
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_") and callable(v)]
    failed = 0
    for t in tests:
        try:
            t()
            print(f"OK   {t.__name__}")
        except AssertionError as e:
            failed += 1
            print(f"FAIL {t.__name__}: {e}")
    print(f"[test_contract_data] {len(tests) - failed}/{len(tests)} passed")
    sys.exit(1 if failed else 0)
