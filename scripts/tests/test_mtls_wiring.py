
"""check_mtls_wiring.py self-test (C1 / A02·V12).

① 현행 트리에서 게이트 PASS(exit 0) + 불변식 인식(scan_text == []).
② 합성 full-file: client_auth 분기 + set_tls_database + set_tls_auth_mode → PASS.
③ 반사실(합성): server-cert 전용(mTLS 배선 없음) → RED.
④ 정본 rest_server.c 변형 temp 사본 → RED (추적 소스 불변).
   - soup_server_set_tls_database 제거.
   - "client_auth" config 키 제거.
"""
import os
import sys
import tempfile
import subprocess
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from check_mtls_wiring import scan_text, TARGET

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
    """① 게이트 PASS(exit 0)."""
    r = subprocess.run([sys.executable, str(GATE)], capture_output=True, text=True)
    assert r.returncode == 0, f"{r.stdout}\n{r.stderr}"

def test_current_invariant_recognized():
    """① 정본: 세 불변식 모두 충족(scan_text 사유 없음)."""
    reasons = scan_text(TARGET.read_text())
    assert reasons == [], f"불변식 미인식: {reasons}"

def test_gate_pass_on_synthetic():
    """② 합성 PASS full-file → 게이트 exit 0."""
    assert _run_gate_on(PASS_FILE) == 0

def test_red_server_cert_only():
    """③ server-cert 전용(mTLS 배선 없음) → RED."""
    assert _run_gate_on(FAIL_SERVER_CERT_ONLY) == 1

def test_real_file_tls_database_removed_fails():
    """④ 정본 soup_server_set_tls_database 제거 사본 → RED."""
    orig = TARGET.read_text()
    assert "soup_server_set_tls_database" in orig, "정본에서 CA DB 배선 미발견 — 소스 변경?"
    assert _run_gate_on(orig.replace("soup_server_set_tls_database",
                                     "soup_server_set_tls_removed")) == 1

def test_real_file_config_key_removed_fails():
    """④ 정본 "client_auth" config 키 제거 사본 → RED."""
    orig = TARGET.read_text()
    assert "client_auth" in orig, "정본에서 client_auth 미발견 — 소스 변경?"
    assert _run_gate_on(orig.replace("client_auth", "client_removed")) == 1

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
