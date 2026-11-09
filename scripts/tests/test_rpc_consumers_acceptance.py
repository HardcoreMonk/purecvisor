#!/usr/bin/env python3
                          
                                                                  
                                                                                 
                                                                       
                                                                   
                                                   
 
                      
                                                                                
                                                   

                                                          
                            
                                                                  
                                                         
                                                               
                                                            
                                                                            

                                                            
                                                

                                                   
                                                      
                                            
   
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
GATE = ROOT / "scripts" / "check_rpc_consumers.py"
DISP = ROOT / "src" / "api" / "dispatcher.c"
REST = ROOT / "src" / "api" / "rest_server.c"
BASE = ROOT / "contracts" / "rpc_orphan_baseline.json"


def _run():
    return subprocess.run([sys.executable, str(GATE)], capture_output=True, text=True)


def test_passes_at_baseline():
                              
    r = _run()
    assert r.returncode == 0, f"{r.stdout}\n{r.stderr}"


def test_new_orphan_is_blocked():
                                                       
    orig = DISP.read_text()
    try:
        DISP.write_text(
            orig + '\n    g_hash_table_insert(g_rpc_routes, "test.__orphan_probe__", NULL);\n'
        )
        r = _run()
        assert r.returncode == 1 and "ORPHAN" in r.stderr, f"{r.stdout}\n{r.stderr}"
    finally:
        DISP.write_text(orig)


def test_consumed_but_unregistered_is_blocked():
                                                
    orig = REST.read_text()
    try:

        REST.write_text(orig + '\n    rpc = pcv_rest_build_rpc("test.__unregistered__", NULL);\n')
        r = _run()
        assert r.returncode == 1, f"소비-미등록 미차단:\n{r.stdout}\n{r.stderr}"
    finally:
        REST.write_text(orig)

def test_split_cli_file_consumption_is_seen():

    probe = ROOT / "src" / "cli" / "zz_consumer_probe.c"
    assert not probe.exists(), "프로브 파일 이름이 이미 쓰이고 있다"
    try:
        probe.write_text('void zz(void) { purectl_send_request("test.__cli_split_unregistered__", NULL, NULL); }\n')
        r = _run()
        assert r.returncode == 1 and "test.__cli_split_unregistered__" in r.stderr, f"{r.stdout}\n{r.stderr}"
    finally:
        probe.unlink(missing_ok=True)

def test_empty_cli_glob_exits_2():

    sys.path.insert(0, str(ROOT / "scripts"))
    import check_rpc_consumers as gate
    saved = gate.CLI_FILES
    try:
        gate.CLI_FILES = []
        rc = gate.main()
        assert rc == 2, f"빈 CLI glob rc={rc} (기대 2)"
    finally:
        gate.CLI_FILES = saved


def test_dead_candidate_mislabel_is_blocked():
                                                                                             
    orig = BASE.read_text()
    try:
        d = json.loads(orig)
                                                                       
        d["orphans"]["quota.get"]["reason"] = "dead-candidate"
        BASE.write_text(json.dumps(d, indent=2, ensure_ascii=False))
        r = _run()
        assert r.returncode == 1 and "MISLABEL" in r.stderr, f"{r.stdout}\n{r.stderr}"
    finally:
        BASE.write_text(orig)


def test_baseline_deflation_is_blocked():
                                                             
                                                             
    orig = BASE.read_text()
    try:
        d = json.loads(orig)
        victim = next(iter(d["orphans"]))
        del d["orphans"][victim]
        BASE.write_text(json.dumps(d, indent=2))
        r = _run()
        assert r.returncode == 1 and victim in r.stderr, f"{r.stdout}\n{r.stderr}"
    finally:
        BASE.write_text(orig)


if __name__ == "__main__":
    for k, v in sorted(globals().items()):
        if k.startswith("test_"):
            v()
            print(f"  ok  {k}")
    print("OK")
