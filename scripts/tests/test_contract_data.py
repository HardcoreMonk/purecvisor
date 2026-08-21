                          
                                                                       
                                                                                 
                                                                       
                                                                   
                                                   
 
                      
                                                                                     
import contextlib
import io
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
REG = json.loads((ROOT / "contracts" / "rpc_params.json").read_text())
BASE = json.loads((ROOT / "contracts" / "rpc_param_baseline.json").read_text())

sys.path.insert(0, str(ROOT / "scripts"))
import check_rpc_param_contract as gate              

def test_registry_shape():
    for m, spec in REG.items():
        assert set(spec) <= {"required", "optional", "keys_via"}, m
        assert isinstance(spec.get("required", []), list), m
        assert isinstance(spec.get("optional", []), list), m

def test_baseline_methods_in_registry():
    for e in BASE["known_consumer_mismatches"]:
        assert e["method"] in REG, f"baseline 메서드 {e['method']}가 레지스트리 부재"
        assert e["consumer"] in {"cli", "fe"}, e

def _observed_missing():
                                                        

                                                             
                                                  
                                                 
                                                         
       
    captured = {}
    original = gate.analyze

    def spy(*args, **kwargs):
        hard, missing, ignored = original(*args, **kwargs)
        captured["missing"] = set(missing)
        return hard, missing, ignored

    gate.analyze = spy
    try:
        with contextlib.redirect_stdout(io.StringIO()), \
             contextlib.redirect_stderr(io.StringIO()):
            gate.main()
    finally:
        gate.analyze = original
    assert "missing" in captured, "게이트가 analyze() 를 부르지 않았다 — 판정 경로 우회"
    return captured["missing"]


def test_baseline_matches_gate_observed_mismatches():
                                                         

                                                              
                                                       
                                                 
                                                             

                                                    
                                                           
                                                
                                                                 
                                                          
                                                     
                            
                                              
       
    observed = _observed_missing()
    baseline = gate._load_baseline()

    def fmt(entries):
        return sorted(f"{m}[{c}] 필수 {sorted(keys)}" for m, c, keys in entries)

    unrecorded = observed - baseline
    stale = baseline - observed
    assert not unrecorded, (
        "게이트가 관측했으나 baseline 에 없는 미전송 부채: " + str(fmt(unrecorded)))
    assert not stale, (
        "baseline 에 있으나 더는 관측되지 않는 항목 — 시정됐다면 줄을 지워 래칫을 "
        "전진시키고, 관측된 적 없다면 애초에 넣지 말 것: " + str(fmt(stale)))


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
