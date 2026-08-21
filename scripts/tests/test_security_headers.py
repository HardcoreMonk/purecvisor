#!/usr/bin/env python3
                          
                                                           
                                                                                 
                                                                       
                                                                   
                                                   
 
                      
                                                                         
                                                   

                           
                                                           
                                                    
   
import os
import sys
import tempfile
import subprocess
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from check_security_headers import scan_text, TARGET              

GATE = Path(__file__).resolve().parent.parent / "check_security_headers.py"

                                                 
GOOD_BLOCK = '''
    if (g_str_has_prefix(path, "/ui")) {
        soup_message_headers_replace(rh, "Cache-Control", "no-cache");
        soup_message_headers_replace(rh, "Content-Security-Policy", PCV_CSP_POLICY);
        soup_message_headers_replace(rh, "X-Frame-Options", "SAMEORIGIN");
        return;
    }
'''

                                             
BAD_BLOCK = '''
    if (g_str_has_prefix(path, "/ui")) {
        soup_message_headers_replace(rh, "Cache-Control", "no-cache");
        return;
    }
'''


def test_good_block_clean():
                                          
    missing, found = scan_text(GOOD_BLOCK)
    assert found, "/ui 블록을 못 찾음"
    assert missing == [], f"오탐: {missing}"


def test_bad_block_flagged():
                                                 
    missing, found = scan_text(BAD_BLOCK)
    assert found
    assert "Content-Security-Policy" in missing
    assert "X-Frame-Options" in missing


def test_gate_passes_on_current_tree():
                                     
    r = subprocess.run([sys.executable, str(GATE)], capture_output=True, text=True)
    assert r.returncode == 0, f"{r.stdout}\n{r.stderr}"


def test_reverted_source_fails():
                                                                  
                                                      
    orig = TARGET.read_text()
    needle = 'soup_message_headers_replace(rh, "Content-Security-Policy", PCV_CSP_POLICY);'
    assert needle in orig, "/ui 블록의 CSP replace 호출을 찾지 못함 — 소스 포맷 변경?"
    reverted = orig.replace(needle, "", 1)
    with tempfile.NamedTemporaryFile("w", suffix=".c", delete=False) as f:
        f.write(reverted)
        tmp = f.name
    try:
        r = subprocess.run([sys.executable, str(GATE), tmp], capture_output=True, text=True)
        assert r.returncode == 1, f"CSP 제거 사본에서 게이트가 RED 가 아님:\n{r.stdout}\n{r.stderr}"
    finally:
        os.unlink(tmp)


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
    print(f"[test_security_headers] {len(tests) - failed}/{len(tests)} passed")
    sys.exit(1 if failed else 0)
