#!/usr/bin/env python3
"""check_transport_bind.py self-test (Wave C Item 6 / A02·V12).

① 현행 트리에서 게이트 PASS(exit 0) + 불변식 인식(scan_text == []).
② 합성 full-file: bind_plaintext 존중 + 루프백 리스닝 → PASS.
③ 반사실(합성): 평문 리스닝을 무조건 soup_server_listen_all(0.0.0.0)로 되돌림 → RED.
④ 정본 rest_server.c 변형 temp 사본 → RED (추적 소스 불변).
   - g_inet_address_new_loopback 제거.
   - "bind_plaintext" config 키 제거.
"""
import os
import sys
import tempfile
import subprocess
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from check_transport_bind import scan_text, TARGET  # noqa: E402

GATE = Path(__file__).resolve().parent.parent / "check_transport_bind.py"

PASS_FILE = '''
gboolean pcv_rest_server_start(void) {
    const gchar *bind_mode = pcv_config_get_string("server", "bind_plaintext", "loopback");
    gboolean ok;
    if (g_strcmp0(bind_mode, "all") == 0) {
        ok = soup_server_listen_all(self->soup, self->port, OPT, &lerr);
    } else {
        GInetAddress *lo = g_inet_address_new_loopback(AF_INET);
        GSocketAddress *addr = g_inet_socket_address_new(lo, self->port);
        ok = soup_server_listen(self->soup, addr, OPT, &lerr);
        g_object_unref(addr);
        g_object_unref(lo);
    }
    return ok;
}
'''

# 반사실: 무조건 전역 노출(bind_plaintext 미참조·루프백 없음·단수형 listen 없음)
FAIL_UNCONDITIONAL = '''
gboolean pcv_rest_server_start(void) {
    gboolean ok = soup_server_listen_all(self->soup, self->port, OPT, &lerr);
    return ok;
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


def test_red_unconditional_listen_all():
    """③ 무조건 listen_all(0.0.0.0) → RED."""
    assert _run_gate_on(FAIL_UNCONDITIONAL) == 1


def test_real_file_loopback_removed_fails():
    """④ 정본 g_inet_address_new_loopback 제거 사본 → RED."""
    orig = TARGET.read_text()
    assert "g_inet_address_new_loopback" in orig, "정본에서 루프백 생성 미발견 — 소스 변경?"
    assert _run_gate_on(orig.replace("g_inet_address_new_loopback", "g_inet_address_new_any")) == 1


def test_real_file_config_key_removed_fails():
    """④ 정본 "bind_plaintext" config 키 제거 사본 → RED."""
    orig = TARGET.read_text()
    assert "bind_plaintext" in orig, "정본에서 bind_plaintext 미발견 — 소스 변경?"
    assert _run_gate_on(orig.replace("bind_plaintext", "bind_removed")) == 1


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
    print(f"[test_transport_bind] {len(tests) - failed}/{len(tests)} passed")
    sys.exit(1 if failed else 0)
