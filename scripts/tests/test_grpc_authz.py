
"""check_grpc_authz.py self-test (Wave B Item 2 / A01·V8).

① 현행 트리에서 게이트 PASS(exit 0) + 두 불변식 인식.
② 반사실(로직): empty-token 가드가 return으로 끝나지 않으면 무토큰 거부 미충족.
③ 반사실(합성 full-file, 게이트 단일 파일 모드):
   - 주입 토큰 제거 → 게이트 RED.
   - 무토큰 fall-through → 게이트 RED.
④ 정본 grpc_server.c 에서 주입 토큰을 제거한 temp 사본 → 게이트 RED.
"""
import os
import sys
import tempfile
import subprocess
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from check_grpc_authz import (
    scan_text, check_no_token_refusal, strip_code, TARGET, INJECT_TOKENS,
)

GATE = Path(__file__).resolve().parent.parent / "check_grpc_authz.py"

_INJECT_HELPER = '''
static char *inject(const char *p, int role) {
    json_object_set_int_member(o, "_pcv_caller_role", role);
    json_object_set_string_member(o, "_pcv_caller_sub", "grpc");
    return 0;
}
'''

PASS_FILE = _INJECT_HELPER + '''
void pcv_grpc_server_start(void) {
    G_grpc_auth_token = dup(cfg);
    if (!G_grpc_auth_token || !*G_grpc_auth_token) {
        LOG("refuse");
        return;
    }
    start_thread();
}
'''

FALLTHROUGH_FILE = _INJECT_HELPER + '''
void pcv_grpc_server_start(void) {
    G_grpc_auth_token = dup(cfg);
    if (!G_grpc_auth_token || !*G_grpc_auth_token) {
        if (bind != loop) { WARN("x"); return; }
        LOG("restricted to 127.0.0.1");
    }
    start_thread();
}
'''

NO_INJECT_FILE = '''
static char *inject(const char *p, int role) {
    return 0;
}
void pcv_grpc_server_start(void) {
    G_grpc_auth_token = dup(cfg);
    if (!G_grpc_auth_token || !*G_grpc_auth_token) {
        LOG("refuse");
        return;
    }
    start_thread();
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

def test_current_tokens_and_refusal():
    """① 정본: 주입 토큰 존재 + 무토큰 거부 인식."""
    text = TARGET.read_text()
    missing, refusal_ok, reason = scan_text(text)
    assert missing == [], f"주입 토큰 누락: {missing}"
    assert refusal_ok, f"무토큰 거부 미인식: {reason}"

def test_refusal_logic_return_ok():
    """② 로직: 가드가 return으로 끝나면 ok."""
    ok, _ = check_no_token_refusal(strip_code(PASS_FILE))
    assert ok

def test_refusal_logic_fallthrough_fails():
    """② 로직: 가드가 fall-through(INFO로 끝남)면 미충족."""
    ok, reason = check_no_token_refusal(strip_code(FALLTHROUGH_FILE))
    assert not ok, f"fall-through인데 ok로 판정: {reason}"

def test_gate_pass_on_synthetic():
    """③ 합성 PASS full-file → 게이트 exit 0."""
    assert _run_gate_on(PASS_FILE) == 0

def test_gate_red_on_fallthrough():
    """③ 무토큰 fall-through → 게이트 exit 1."""
    assert _run_gate_on(FALLTHROUGH_FILE) == 1

def test_gate_red_on_no_inject():
    """③ 주입 토큰 제거 → 게이트 exit 1."""
    assert _run_gate_on(NO_INJECT_FILE) == 1

def test_real_file_injection_removed_fails():
    """④ 정본에서 주입 토큰을 제거한 temp 사본 → 게이트 RED. 추적 소스 불변."""
    orig = TARGET.read_text()
    for tok in INJECT_TOKENS:
        assert tok in orig, f"{tok} 를 정본에서 찾지 못함 — 소스 변경?"
    reverted = orig
    for tok in INJECT_TOKENS:
        reverted = reverted.replace(tok, "_removed_key")
    assert _run_gate_on(reverted) == 1, "주입 토큰 제거 사본에서 게이트가 RED가 아님"

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
    print(f"[test_grpc_authz] {len(tests) - failed}/{len(tests)} passed")
    sys.exit(1 if failed else 0)
