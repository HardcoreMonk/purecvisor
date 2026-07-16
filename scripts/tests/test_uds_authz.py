#!/usr/bin/env python3
"""check_uds_authz.py self-test (Wave C Item 1 / A01·V8).

① 현행 트리에서 게이트 PASS(exit 0) + 두 불변식 인식.
② 합성 full-file(게이트 단일 파일 모드): 소켓 0660 + SO_PEERCRED(양 경로) → PASS.
③ 반사실(합성):
   - umask(0117)→umask(0111) 되돌림 → RED.
   - 0666 리터럴 재도입 → RED.
   - io_uring accept 경로에서 게이트 제거(한쪽만) → RED.
   - SO_PEERCRED 토큰 제거 → RED.
④ 정본 uds_server.c 변형 temp 사본 → RED (추적 소스 불변).
   - umask(0117)→umask(0111).
   - SO_PEERCRED 토큰 제거.
   - io_uring 경로 게이트 호출 제거(한쪽만).
"""
import os
import sys
import tempfile
import subprocess
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from check_uds_authz import scan_text, TARGET  # noqa: E402

GATE = Path(__file__).resolve().parent.parent / "check_uds_authz.py"

# io_uring 경로 게이트 호출 라인(정확 매칭용 상수) ─────────────────
URING_GATE_LINE = "    if (!_uds_peer_is_root(client_fd)) { close(client_fd); return; }\n"

PASS_FILE = '''
#define UDS_LOG_DOM "uds_server"
static gboolean _uds_peer_is_root(int fd) {
    struct ucred cred;
    socklen_t len = sizeof(cred);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0)
        return FALSE;
    if (cred.uid != 0)
        return FALSE;
    return TRUE;
}
static void _uring_accept_cb(void *u, int result, void *data) {
    int client_fd = result;
''' + URING_GATE_LINE + '''    mode_t a = umask(0117);
    bind_it();
    umask(a);
}
static gboolean on_incoming_connection(void *s, void *c) {
    int peer_fd = g_socket_get_fd(sock);
    if (!_uds_peer_is_root(peer_fd)) { close_it(); return TRUE; }
    mode_t b = umask(0117);
    add_addr();
    umask(b);
    return TRUE;
}
'''

REVERT_UMASK = PASS_FILE.replace("umask(0117)", "umask(0111)")
RESIDUAL_0666 = PASS_FILE + "\n/* historical socket mode 0666 rw-rw-rw- */\n"
NO_PEERCRED_ONE_PATH = PASS_FILE.replace(URING_GATE_LINE, "")   # io_uring 경로 게이트 삭제
NO_PEERCRED_TOKEN = PASS_FILE.replace("SO_PEERCRED", "SO_PASSCRED")


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


def test_current_invariants_recognized():
    """① 정본: 소켓 0660 + SO_PEERCRED 양 경로 인식."""
    text = TARGET.read_text()
    perms_ok, perms_r, peer_ok, peer_r = scan_text(text)
    assert perms_ok, f"소켓 권한 불변식 미인식: {perms_r}"
    assert peer_ok, f"SO_PEERCRED 게이트 미인식: {peer_r}"


def test_gate_pass_on_synthetic():
    """② 합성 PASS full-file → 게이트 exit 0."""
    assert _run_gate_on(PASS_FILE) == 0


def test_red_revert_umask():
    """③ umask(0117)→0111 → RED."""
    assert _run_gate_on(REVERT_UMASK) == 1


def test_red_residual_0666():
    """③ 0666 리터럴 재도입 → RED."""
    assert _run_gate_on(RESIDUAL_0666) == 1


def test_red_peercred_one_path_removed():
    """③ io_uring accept 경로 게이트 제거(한쪽) → RED."""
    assert _run_gate_on(NO_PEERCRED_ONE_PATH) == 1


def test_red_peercred_token_removed():
    """③ SO_PEERCRED 토큰 제거 → RED."""
    assert _run_gate_on(NO_PEERCRED_TOKEN) == 1


def test_real_file_umask_revert_fails():
    """④ 정본 umask(0117)→0111 사본 → RED."""
    orig = TARGET.read_text()
    assert "umask(0117)" in orig, "정본에서 umask(0117) 미발견 — 소스 변경?"
    assert _run_gate_on(orig.replace("umask(0117)", "umask(0111)")) == 1


def test_real_file_peercred_token_removed_fails():
    """④ 정본 SO_PEERCRED 제거 사본 → RED."""
    orig = TARGET.read_text()
    assert "SO_PEERCRED" in orig, "정본에서 SO_PEERCRED 미발견 — 소스 변경?"
    assert _run_gate_on(orig.replace("SO_PEERCRED", "SO_PASSCRED")) == 1


def test_real_file_uring_gate_removed_fails():
    """④ 정본 io_uring 경로 게이트 호출 제거(한쪽) → RED."""
    orig = TARGET.read_text()
    assert "_uds_peer_is_root(client_fd)" in orig, "정본에서 io_uring 게이트 호출 미발견 — 소스 변경?"
    reverted = orig.replace("_uds_peer_is_root(client_fd)", "_gate_bypassed(client_fd)")
    assert _run_gate_on(reverted) == 1


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
    print(f"[test_uds_authz] {len(tests) - failed}/{len(tests)} passed")
    sys.exit(1 if failed else 0)
