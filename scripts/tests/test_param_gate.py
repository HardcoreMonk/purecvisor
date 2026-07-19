
"""RPC param-key 계약 게이트 3방 diff(diff_method) 단위 테스트."""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from check_rpc_param_contract import diff_method

def _req(*ks): return list(ks)

def test_drift_detected():

    r = diff_method("m", {"required": ["name"], "optional": []},
                    handler_req={"vm_id"}, sent_by_consumer={"cli": {"name"}})
    assert r["drift"]

def test_missing_required():
    r = diff_method("m", {"required": ["name"], "optional": []},
                    handler_req={"name"}, sent_by_consumer={"cli": {"bridge_name"}})
    assert ("cli", frozenset({"name"})) in r["missing"]

def test_ignored_key_warn():
    r = diff_method("m", {"required": ["name"], "optional": []},
                    handler_req={"name"}, sent_by_consumer={"cli": {"name", "inbound"}})
    assert ("cli", "inbound") in r["ignored"]

def test_alias_group_satisfied():

    r = diff_method("m", {"required": [["interface", "vm_name"]], "optional": []},
                    handler_req={frozenset({"interface", "vm_name"})},
                    sent_by_consumer={"cli": {"vm_name"}})
    assert not r["missing"] and not r["drift"]

def test_clean_pass():
    r = diff_method("m", {"required": ["name"], "optional": ["x"]},
                    handler_req={"name"}, sent_by_consumer={"cli": {"name", "x"}})
    assert not r["drift"] and not r["missing"] and not r["ignored"]

if __name__ == "__main__":
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for t in tests:
        t()
        print(f"  ok  {t.__name__}")
    print("OK")
