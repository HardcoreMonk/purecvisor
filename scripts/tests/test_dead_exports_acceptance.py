#!/usr/bin/env python3
                          
                                                          
                                                                                 
                                                                       
                                                                   
                                                   
 
                      
                                                                        
                                

                                                                
                                                
                      

                                                                         
            
   
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
from check_dead_exports import find_dead              


def _real_tree():
    headers = [p.read_text(errors="replace") for p in
               list((ROOT / "src").rglob("*.h")) + list((ROOT / "include").rglob("*.h"))]
    c_raw = [p.read_text(errors="replace") for p in (ROOT / "src").rglob("*.c")]
    return headers, c_raw


def test_handlers_not_false_flagged():
                                                    
    headers, c_raw = _real_tree()
    dead = find_dead(headers, c_raw)
                                                                    
    assert "pcv_rbac_check_permission" not in dead, "실사용 함수 오탐(pcv_rbac_check_permission)"
                                                          
    assert "pcv_rest_server_start" not in dead, "실사용 함수 오탐(pcv_rest_server_start)"


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
    print(f"[test_dead_exports_acceptance] {len(tests) - failed}/{len(tests)} passed")
    sys.exit(1 if failed else 0)
