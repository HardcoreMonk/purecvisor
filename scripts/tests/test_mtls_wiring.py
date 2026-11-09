#!/usr/bin/env python3
                          
                                                              
                                                                                 
                                                                       
                                                                   
                                                   
 
                      
                                                                            
                                                 

                                                     
                                                                             
                                            
                                               
                                     
                               
   
import contextlib
import io
import os
import sys
import tempfile
import subprocess
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import check_mtls_wiring
import rpc_extract
from check_mtls_wiring import scan_text

REPO = Path(__file__).resolve().parents[2]

GATE = Path(__file__).resolve().parent.parent / "check_mtls_wiring.py"

PASS_FILE = '''
gboolean pcv_rest_server_start(void) {
    if (pcv_tls_is_enabled()) {
        soup_server_set_tls_certificate(self->soup, tls_cert);
        const gchar *client_auth = pcv_config_get_string("tls", "client_auth", "none");
        gboolean want_require = (g_strcmp0(client_auth, "require") == 0);
        if (want_require || g_strcmp0(client_auth, "request") == 0) {
            GTlsDatabase *db = g_tls_file_database_new(ca_path, &db_err);
            if (db) {
                soup_server_set_tls_database(self->soup, db);
                soup_server_set_tls_auth_mode(self->soup,
                    want_require ? G_TLS_AUTHENTICATION_REQUIRED
                                 : G_TLS_AUTHENTICATION_REQUESTED);
                g_object_unref(db);
            }
        }
    }
    return TRUE;
}
'''

                                                        
FAIL_SERVER_CERT_ONLY = '''
gboolean pcv_rest_server_start(void) {
    if (pcv_tls_is_enabled()) {
        soup_server_set_tls_certificate(self->soup, tls_cert);
    }
    return TRUE;
}
'''


def _run_gate_on(text: str) -> int:
    with tempfile.NamedTemporaryFile("w", suffix=".c", delete=False) as f:
        f.write(text)
        tmp = f.name
    try:
        r = subprocess.run([sys.executable, str(GATE), tmp],
                           capture_output=True, text=True)
        return r.returncode
    finally:
        os.unlink(tmp)


def test_current_tree_pass():
                             
    r = subprocess.run([sys.executable, str(GATE)], capture_output=True, text=True)
    assert r.returncode == 0, f"{r.stdout}\n{r.stderr}"


def test_current_invariant_recognized():
                                             
    reasons = scan_text(rpc_extract.rest_family_text())
    assert reasons == [], f"불변식 미인식: {reasons}"


def test_gate_pass_on_synthetic():
                                           
    assert _run_gate_on(PASS_FILE) == 0


def test_red_server_cert_only():
                                             
    assert _run_gate_on(FAIL_SERVER_CERT_ONLY) == 1


def test_real_file_tls_database_removed_fails():
                                                        
    orig = rpc_extract.rest_family_text()
    assert "soup_server_set_tls_database" in orig, "정본에서 CA DB 배선 미발견 — 소스 변경?"
    assert _run_gate_on(orig.replace("soup_server_set_tls_database",
                                     "soup_server_set_tls_removed")) == 1


def test_real_file_config_key_removed_fails():
                                                  
    orig = rpc_extract.rest_family_text()
    assert "client_auth" in orig, "정본에서 client_auth 미발견 — 소스 변경?"
    assert _run_gate_on(orig.replace("client_auth", "client_removed")) == 1

@contextlib.contextmanager
def _family_root(mutate=None, extra=None):

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        files = {p.relative_to(REPO).as_posix(): p.read_text() for p in rpc_extract.rest_family()}
        files.update(extra or {})
        for rel, text in files.items():
            (root / rel).parent.mkdir(parents=True, exist_ok=True)
            (root / rel).write_text(mutate(rel, text) if mutate else text)
        saved = check_mtls_wiring.ROOT
        check_mtls_wiring.ROOT = root
        try:
            yield root
        finally:
            check_mtls_wiring.ROOT = saved

def _main_rc() -> tuple:
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        rc = check_mtls_wiring.main([])
    return rc, out.getvalue() + err.getvalue()

def test_family_auth_mode_absent_fails():

    with _family_root(mutate=lambda rel, t: t.replace("soup_server_set_tls_auth_mode",
                                                      "soup_server_auth_mode_removed")):
        rc, out = _main_rc()
    assert rc == 1 and "soup_server_set_tls_auth_mode" in out, f"앵커 0건인데 RED가 아님:\n{out}"

def test_wiring_only_in_split_file_passes():

    def strip_wiring(text):
        for token in ("client_auth", "soup_server_set_tls_database", "soup_server_set_tls_auth_mode"):
            text = text.replace(token, token.upper())
        return text
    transport = "src/api/rest_transport.c"
    with _family_root(mutate=lambda rel, t: strip_wiring(t) + (PASS_FILE if rel == transport else ""),
                      extra={} if (REPO / transport).exists() else {transport: ""}):
        rc, out = _main_rc()
    assert rc == 0, f"분할 파일의 mTLS 배선을 못 읽음:\n{out}"
    with _family_root(mutate=lambda rel, t: strip_wiring(t)):
        rc, out = _main_rc()
    assert rc == 1, f"배선을 계열 전부에서 지웠는데 PASS — 대조 실패:\n{out}"


if __name__ == "__main__":
    tests = [v for k, v in sorted(globals().items())
             if k.startswith("test_") and callable(v)]
    failed = 0
    for t in tests:
        try:
            t()
            print(f"OK   {t.__name__}")
        except AssertionError as e:
            failed += 1
            print(f"FAIL {t.__name__}: {e}")
    print(f"[test_mtls_wiring] {len(tests) - failed}/{len(tests)} passed")
    sys.exit(1 if failed else 0)
