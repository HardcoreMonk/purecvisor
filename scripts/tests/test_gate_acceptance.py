#!/usr/bin/env python3
                          
                                                                         
                                                                                 
                                                                       
                                                                   
                                                   
 
                      
                                                                                       
                            

                                        
                                 
                                         
   
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
                                               
                                                                           
                                                  
                                                   
    for m in ["device.nic.list"]:
        assert m in methods, f"감사 확증 {m}이 게이트 재현에서 누락"
                                                                   
                                                                    
                                                      
                                       
    for m in ["container.snapshot.create", "container.snapshot.delete"]:
        assert m not in methods, f"{m} 이 baseline 에 재등장 — 래칫 역행"
                                                       
                                             
                                                                                  
    for m in ["ovn.acl.add", "ovn.dhcp.enable"]:
        assert m not in methods, f"{m} 이 baseline 에 재등장 — 래칫 역행"
                                                                   
                                                              
                                                     
                                                          
                              
    for m in ["network.mode_set", "network.dhcp_toggle", "network.bind_phys",
              "network.qos.set", "network.qos.get", "network.qos.remove",
              "container.nic.detach", "vm.security_group.set"]:
        assert m not in methods, f"{m} 이 baseline 에 재등장 — 래칫 역행"


def test_p23f4_cli_keys_stay_resolved():
                                                             

                                                                 
                                                                 
                                       
       
    sys.path.insert(0, str(ROOT / "scripts"))
    from check_rpc_param_contract import load_ignored_baseline
    entries = set(load_ignored_baseline())
    resolved = [
        ("container.nic.detach", "mac"),
        ("network.mode_set", "bridge_name"),
        ("network.dhcp_toggle", "bridge_name"),
        ("network.dhcp_toggle", "action"),
        ("network.bind_phys", "bridge_name"),
        ("network.bind_phys", "physical_if"),
        ("network.qos.set", "iface"),
        ("network.qos.set", "rate"),
        ("network.qos.set", "burst"),
        ("network.qos.get", "iface"),
        ("network.qos.remove", "iface"),
        ("vm.security_group.set", "vm_name"),
        ("vm.security_group.set", "group_name"),
    ]
    for method, key in resolved:
        assert (method, "cli", key) not in entries, \
            f"{method}[cli] '{key}' 이 ignored baseline 에 재등장 — 래칫 역행"


def test_new_break_is_blocked():
                                                                       
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
