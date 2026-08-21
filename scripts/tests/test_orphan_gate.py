#!/usr/bin/env python3
                          
                                                             
                                                                                 
                                                                       
                                                                   
                                                   
 
                      
                                                                           
                         

                                                        
                                          
                                                               
                                                     
   
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
GATE = ROOT / "scripts" / "check_rpc_consumers.py"
DISP = ROOT / "src" / "api" / "dispatcher.c"

                                                                      
SUMMARY_RE = re.compile(r"고아 (\d+) / baseline (\d+)")


def _run():
    return subprocess.run([sys.executable, str(GATE)], capture_output=True, text=True)


def test_gate_passes_at_orphan_baseline():
                                                            

                                                             
                                                     
                                                
                                  

                                                   
                                                         
                                                                     
                                                           
                                                     
       
    r = _run()
    assert r.returncode == 0, f"{r.stdout}\n{r.stderr}"
    m = SUMMARY_RE.search(r.stdout)
    assert m, f"고아 요약 줄이 없다 — 게이트 출력 계약이 바뀌었다:\n{r.stdout}"
    orphans, baseline = int(m.group(1)), int(m.group(2))
    ratchet = [ln.strip() for ln in r.stdout.splitlines() if "RATCHET:" in ln]
    assert orphans == baseline, (
        f"래칫 하향 — 고아 {orphans} != baseline {baseline}. baseline 에 있으나 이제 "
        f"소비되는 항목을 제거하라:\n  " + "\n  ".join(ratchet or ["(RATCHET 줄 없음)"])
    )


def test_new_orphan_blocked():
                                                
    orig = DISP.read_text()
    try:
        DISP.write_text(
            orig + '\n    g_hash_table_insert(g_rpc_routes, "test.__orphan_probe__", NULL);\n'
        )
        r = _run()
        assert r.returncode == 1, f"신규 고아를 게이트가 못 막음:\n{r.stdout}\n{r.stderr}"
        assert "ORPHAN" in r.stderr, f"ORPHAN FAIL 메시지 누락:\n{r.stdout}\n{r.stderr}"
        assert "test.__orphan_probe__" in r.stderr
    finally:
        DISP.write_text(orig)


if __name__ == "__main__":
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for t in tests:
        t()
        print(f"  ok  {t.__name__}")
    print("OK")
