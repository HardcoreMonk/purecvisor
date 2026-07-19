
"""check_tls_min_version.py self-test (C2 / A02·V11·V12).

① 현행 트리에서 게이트 PASS(exit 0) + 불변식 인식(scan_text == []).
② 합성 full-file: G_TLS_GNUTLS_PRIORITY 고정 + min_version 존중 → PASS.
③ 반사실(합성): 우선순위 미고정(min_version 미참조) → RED.
④ 정본 pcv_tls.c 변형 temp 사본 → RED (추적 소스 불변).
   - G_TLS_GNUTLS_PRIORITY 제거.
   - "min_version" config 키 제거.
"""
import os
import sys
import tempfile
import subprocess
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from check_tls_min_version import scan_text, TARGET

GATE = Path(__file__).resolve().parent.parent / "check_tls_min_version.py"

PASS_FILE = '''
void pcv_tls_init_from_config(void) {
    const gchar *min_version = pcv_config_get_string("tls", "min_version", "1.2");
    const gchar *tls_priority;
    if (g_strcmp0(min_version, "1.3") == 0)
        tls_priority = "NORMAL:-VERS-ALL:+VERS-TLS1.3";
    else
        tls_priority = "NORMAL:-VERS-ALL:+VERS-TLS1.2:+VERS-TLS1.3";
    g_setenv("G_TLS_GNUTLS_PRIORITY", tls_priority, TRUE);
}
'''

FAIL_NO_PIN = '''
void pcv_tls_init_from_config(void) {
    const gchar *enabled = pcv_config_get_string("tls", "enabled", "false");
    if (g_strcmp0(enabled, "true") != 0)
        return;
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

def test_red_no_pin():
    """③ 우선순위 미고정 → RED."""
    assert _run_gate_on(FAIL_NO_PIN) == 1

def test_real_file_priority_env_removed_fails():
    """④ 정본 G_TLS_GNUTLS_PRIORITY 제거 사본 → RED."""
    orig = TARGET.read_text()
    assert "G_TLS_GNUTLS_PRIORITY" in orig, "정본에서 우선순위 환경변수 미발견 — 소스 변경?"
    assert _run_gate_on(orig.replace("G_TLS_GNUTLS_PRIORITY",
                                     "G_TLS_PRIORITY_REMOVED")) == 1

def test_real_file_config_key_removed_fails():
    """④ 정본 "min_version" config 키 제거 사본 → RED."""
    orig = TARGET.read_text()
    assert "min_version" in orig, "정본에서 min_version 미발견 — 소스 변경?"
    assert _run_gate_on(orig.replace("min_version", "min_removed")) == 1

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
    print(f"[test_tls_min_version] {len(tests) - failed}/{len(tests)} passed")
    sys.exit(1 if failed else 0)
