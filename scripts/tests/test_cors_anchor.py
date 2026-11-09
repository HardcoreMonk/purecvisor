#!/usr/bin/env python3
                          
                                                             
                                                                                 
                                                                       
                                                                   
                                                   
 
                      
                                                                           
                                                        

                           
                                                 
                                              
                                                       
                                                      
                                                 
   
import contextlib
import io
import os
import re
import sys
import tempfile
import subprocess
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import check_cors_anchor
import rpc_extract
from check_cors_anchor import scan_text

REPO = Path(__file__).resolve().parents[2]

CALL_RE = re.compile(r"\b(?:pcv_rest)?_cors_origin_allowed\(origin, host_hdr\)")
HELPER_ANY_RE = re.compile(r"\w*_cors_origin_allowed")

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
                                                                 
                                          
                                                 
    orig = rpc_extract.rest_family_text()
    assert CALL_RE.search(orig), "CORS 호출 사이트를 찾지 못함 — 소스 포맷 변경?"
    reverted = CALL_RE.sub('strstr(origin, "://192.168.")', orig, count=1)
    with tempfile.NamedTemporaryFile("w", suffix=".c", delete=False) as f:
        f.write(reverted)
        tmp = f.name
    try:
        r = subprocess.run([sys.executable, str(GATE), tmp], capture_output=True, text=True)
        assert r.returncode == 1, f"되돌린 사본에서 게이트가 RED가 아님:\n{r.stdout}\n{r.stderr}"
    finally:
        os.unlink(tmp)

@contextlib.contextmanager
def _family_root(mutate=None, extra=None):

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        files = {p.relative_to(REPO).as_posix(): p.read_text() for p in rpc_extract.rest_family()}
        files.update(extra or {})
        for rel, text in files.items():
            (root / rel).parent.mkdir(parents=True, exist_ok=True)
            (root / rel).write_text(mutate(rel, text) if mutate else text)
        saved = check_cors_anchor.ROOT
        check_cors_anchor.ROOT = root
        try:
            yield root
        finally:
            check_cors_anchor.ROOT = saved

def _main_rc() -> tuple:
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        rc = check_cors_anchor.main([])
    return rc, out.getvalue() + err.getvalue()

def test_anchor_absent_in_family_fails():

    with _family_root(mutate=lambda rel, t: HELPER_ANY_RE.sub("_cors_check_removed", t)):
        rc, out = _main_rc()
    assert rc == 1 and "미사용" in out, f"앵커 0건인데 RED가 아님:\n{out}"

def test_helper_only_in_split_file_passes():

    split = ("gboolean\npcv_rest_cors_origin_allowed(const gchar *origin, const gchar *host_hdr)\n"
             "{\n    return g_strcmp0(origin, host_hdr) == 0;\n}\n")
    with _family_root(mutate=lambda rel, t: HELPER_ANY_RE.sub("_cors_check_removed", t)
                      if rel == "src/api/rest_server.c" else t,
                      extra={"src/api/rest_request_auth.c": split}):
        rc, out = _main_rc()
    assert rc == 0, f"분할 파일의 헬퍼를 못 읽음:\n{out}"

def test_violation_only_in_split_file_fails():

    bad = ('static gboolean\n_old_cors(const gchar *origin)\n'
           '{\n    return strstr(origin, "://10.") != NULL;\n}\n')
    with _family_root(extra={"src/api/rest_request_auth.c": bad}):
        rc, out = _main_rc()
    assert rc == 1 and "src/api/rest_request_auth.c:4" in out, f"분할 파일 위반을 못 잡음:\n{out}"


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
