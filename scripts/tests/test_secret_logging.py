#!/usr/bin/env python3
                          
                                                           
                                                                                 
                                                                       
                                                                   
                                                   
 
                      
                                                                         
                                                            

                           
                                                     
                                                       
                                                                        
                         
                                 
   
import os
import sys
import tempfile
import subprocess
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from check_secret_logging import scan_text, TARGET              

GATE = Path(__file__).resolve().parent.parent / "check_secret_logging.py"

DEF = 'static gboolean _body_has_secret(const gchar *body, gsize len) { return FALSE; }\n'

NEW_SNIPPET = DEF + '''
gboolean is_auth = (g_strstr_len(path, -1, "/auth/token") != nullptr) ||
                   _body_has_secret(body_str, body_len);
'''

                                  
OLD_SNIPPET = DEF + '''
gboolean is_auth = (g_strstr_len(path, -1, "/auth/token") != nullptr) ||
                   (g_strstr_len(path, -1, "/auth/refresh") != nullptr);
'''


def test_new_snippet_used():
                                                      
    defined, used = scan_text(NEW_SNIPPET)
    assert defined and used


def test_old_snippet_not_used():
                                                          
    defined, used = scan_text(OLD_SNIPPET)
    assert defined and not used, "경로-only is_auth가 여전히 used로 잡힘"


def test_missing_definition():
                                     
    snippet = ('gboolean is_auth = (x) || _body_has_secret(body_str, body_len);')
    defined, used = scan_text(snippet)
    assert not defined and used


def test_gate_passes_on_current_tree():
                                     
    r = subprocess.run([sys.executable, str(GATE)], capture_output=True, text=True)
    assert r.returncode == 0, f"{r.stdout}\n{r.stderr}"


def test_reverted_source_fails():
                                                                   
                                    

                                                                
                                                                    
                                                      
    orig = TARGET.read_text()
    needle = "_body_has_secret(body_str, body_len)"
    assert needle in orig, "is_auth의 _body_has_secret 호출을 찾지 못함 — 소스 포맷 변경?"
    reverted = orig.replace(needle, "FALSE")
    assert reverted != orig, "is_auth OR 항 패치가 적용되지 않음 — 소스 포맷 변경?"
    with tempfile.NamedTemporaryFile("w", suffix=".c", delete=False) as f:
        f.write(reverted)
        tmp = f.name
    try:
        r = subprocess.run([sys.executable, str(GATE), tmp], capture_output=True, text=True)
        assert r.returncode == 1, f"되돌린 사본에서 게이트가 RED가 아님:\n{r.stdout}\n{r.stderr}"
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
    print(f"[test_secret_logging] {len(tests) - failed}/{len(tests)} passed")
    sys.exit(1 if failed else 0)
