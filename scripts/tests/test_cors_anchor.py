#!/usr/bin/env python3
                          
                                                             
                                                                                 
                                                                       
                                                                   
                                                   
 
                      
                                                                           
                                                        

                           
                                                 
                                              
                                                       
                                                      
                                                 
   
import os
import sys
import tempfile
import subprocess
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from check_cors_anchor import scan_text, TARGET              

GATE = Path(__file__).resolve().parent.parent / "check_cors_anchor.py"

                                            
OLD_BLOCK = '''
        if (!origin || origin[0] == '\\0') {
            allow = TRUE;
        } else if (strstr(origin, "://localhost") ||
                   strstr(origin, "://127.0.0.1") ||
                   strstr(origin, "://192.168.") ||
                   strstr(origin, "://10.")) {
            allow = TRUE;
        } else {
            const gchar *host = soup_message_headers_get_one(reqh, "Host");
            if (host && strstr(origin, host)) allow = TRUE;
        }
'''

                                  
NEW_BLOCK = '''
static gboolean _cors_origin_allowed(const gchar *origin, const gchar *host_hdr) {
    const gchar *sep = strstr(origin, "://");
    if (!sep) return FALSE;
    return g_strcmp0(origin, host_hdr) == 0;
}
if (origin && _cors_origin_allowed(origin, host_hdr)) { emit(); }
'''


def test_old_block_flagged():
                                                      
    fails, _ = scan_text(OLD_BLOCK)
    assert len(fails) >= 1, "예전 substring 블록이 게이트에 잡히지 않음"
    joined = " ".join(desc for _, desc, _ in fails)
    assert "substring" in joined


def test_new_block_clean():
                                                                  
    fails, has_helper = scan_text(NEW_BLOCK)
    assert fails == [], f"현행 스니펫이 오탐: {fails}"
    assert has_helper


def test_scheme_sep_not_flagged():
                                        
    fails, _ = scan_text('const gchar *sep = strstr(origin, "://");')
    assert fails == []


def test_host_substring_flagged():
                                            
    fails, _ = scan_text('if (host && strstr(origin, host)) allow = TRUE;')
    assert len(fails) == 1


def test_gate_passes_on_current_tree():
                                     
    r = subprocess.run([sys.executable, str(GATE)], capture_output=True, text=True)
    assert r.returncode == 0, f"{r.stdout}\n{r.stderr}"


def test_reverted_source_fails():
                                                                 
                                          
                                                 
    orig = TARGET.read_text()
    needle = "_cors_origin_allowed(origin, host_hdr)"
    assert needle in orig, "CORS 호출 사이트를 찾지 못함 — 소스 포맷 변경?"
    reverted = orig.replace(needle, 'strstr(origin, "://192.168.")', 1)
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
    print(f"[test_cors_anchor] {len(tests) - failed}/{len(tests)} passed")
    sys.exit(1 if failed else 0)
