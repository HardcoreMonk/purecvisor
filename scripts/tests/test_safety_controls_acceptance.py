#!/usr/bin/env python3
                          
                                                                     
                                                                                 
                                                                       
                                                                   
                                                   
 
                      
                                                                                   
                                 

                                                                               

                                        
                                                         
                                                                      
                                               
                                                                     

                                                                      
                                                                     
                                                               
              

                                                                     
                                                                     
                                                

                                                              
                                                 
                                                                    
                                                  
                                         
                                                               
   
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
                                                  

                                                                   
                                               
    reg = json.loads(REGISTRY.read_text())
    known_gaps = []
    untested = {cid for cid, spec in reg.items() if spec.get("status") == "untested-baseline"}
    assert untested == set(known_gaps), \
        f"알려진 무동작 통제 리스트와 실제 untested-baseline 상태가 어긋남: {sorted(untested)}"


def test_tested_controls_locked_in():
                                                                  
    reg = json.loads(REGISTRY.read_text())
    for cid in ["session-revoke", "vm-op-lock", "restart-breaker",
                "backup-retention", "alert-silence", "sriov-disable", "graceful-drain",
                "hips-approval-expiry", "apikey-role-enforce", "vm-create-iso-validation",
                "isolated-network-drop", "qos-rehydrate", "self-healing-restart"]:
        assert cid in reg and reg[cid]["status"] == "tested", \
            f"시정 확인 통제 {cid}가 tested에서 이탈"


def test_duplicate_key_fails():
                                                                     
                                                 
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
