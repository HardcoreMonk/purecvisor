#!/usr/bin/env python3
                          
                                                           
                                                                                 
                                                                       
                                                                   
                                                   
 
                      
                                                                         
                                                   

                           
                                                           
                                                    
   
import contextlib
import io
import os
import sys
import tempfile
import subprocess
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import check_security_headers
import rpc_extract
from check_security_headers import scan_text, UI_ANCHOR

ROOT = Path(__file__).resolve().parents[2]
CSP_CALL = 'soup_message_headers_replace(rh, "Content-Security-Policy", PCV_CSP_POLICY);'

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


def _ui_file() -> Path:

    hits = [p for p in rpc_extract.rest_family() if UI_ANCHOR in p.read_text()]
    assert len(hits) == 1, f"/ui 앵커가 있는 계열 파일이 하나가 아님: {hits}"
    return hits[0]

def test_reverted_source_fails():
                                                                  
                                                      
    orig = _ui_file().read_text()
    needle = CSP_CALL
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

@contextlib.contextmanager
def _family_root(mutate=None):

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        for p in rpc_extract.rest_family():
            rel = p.relative_to(ROOT).as_posix()
            text = p.read_text()
            (root / rel).parent.mkdir(parents=True, exist_ok=True)
            (root / rel).write_text(mutate(rel, text) if mutate else text)
        saved = check_security_headers.REST_ROOT
        check_security_headers.REST_ROOT = root
        try:
            yield root
        finally:
            check_security_headers.REST_ROOT = saved

def _main_rc() -> tuple:
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        rc = check_security_headers.main([])
    return rc, out.getvalue() + err.getvalue()

def test_family_mode_reads_ui_block_wherever_it_lives():

    ui_rel = _ui_file().relative_to(ROOT).as_posix()
    with _family_root():
        rc, out = _main_rc()
    assert rc == 0, out
    with _family_root(lambda rel, t: t.replace(CSP_CALL, "", 1) if rel == ui_rel else t):
        rc, out = _main_rc()
    assert rc == 1 and "Content-Security-Policy" in out, out

def test_family_mode_ui_block_in_split_file_is_read():

    block = "\nvoid\nmoved_ui(void)\n{\n" + GOOD_BLOCK + "}\n"
    no_csp = block.replace('soup_message_headers_replace(rh, "Content-Security-Policy", PCV_CSP_POLICY);', "", 1)
    for extra, want in ((block, 0), (no_csp, 1)):
        def mutate(rel, t, extra=extra):
            t = t.replace(UI_ANCHOR, 'g_str_has_prefix(path, "/ui-moved")')
            return t + extra if rel == "src/api/rest_policy.c" else t
        with _family_root(mutate):
            rc, out = _main_rc()
        assert rc == want, out

def test_family_mode_without_ui_block_fails():

    with _family_root(lambda rel, t: t.replace(UI_ANCHOR, 'g_str_has_prefix(path, "/ui-gone")')):
        rc, out = _main_rc()
    assert rc == 1 and "찾지 못함" in out, out


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
