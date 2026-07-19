#!/usr/bin/env python3
"""check_password_policy.py self-test (Q-2 / A07).

① 현행 트리에서 게이트 PASS(exit 0).
② 반사실: handle_auth_user_create 의 복잡도 검증 호출을 제거한 사본에서 RED.
③ 로직 단위: 호출 있는/없는 함수 본문 스니펫 판정.
"""
import os
import sys
import tempfile
import subprocess
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from check_password_policy import scan_text, TARGET  # noqa: E402

GATE = Path(__file__).resolve().parent.parent / "check_password_policy.py"

GOOD = '''
void
handle_auth_user_create(JsonObject *params, const gchar *rpc_id)
{
    const gchar *pw_reason = NULL;
    if (!pcv_validate_password_complexity(password, &pw_reason)) {
        return;
    }
    do_create();
}
'''

BAD = '''
void
handle_auth_user_create(JsonObject *params, const gchar *rpc_id)
{
    do_create();  /* 복잡도 검증 없음 */
}
'''


def test_good_has_call():
    has_call, found = scan_text(GOOD)
    assert found and has_call


def test_bad_missing_call():
    has_call, found = scan_text(BAD)
    assert found and not has_call


def test_gate_passes_on_current_tree():
    r = subprocess.run([sys.executable, str(GATE)], capture_output=True, text=True)
    assert r.returncode == 0, f"{r.stdout}\n{r.stderr}"


def test_reverted_source_fails():
    """② 반사실: 실제 handler_auth.c 에서 복잡도 검증 호출문을 제거한 사본에서 RED."""
    orig = TARGET.read_text()
    needle = "if (!pcv_validate_password_complexity(password, &pw_reason)) {"
    assert needle in orig, "복잡도 검증 호출을 찾지 못함 — 소스 포맷 변경?"
    # 호출 조건을 항상-거짓 상수로 치환 → pcv_validate_password_complexity 심볼 제거
    reverted = orig.replace(needle, "if (0) {", 1)
    assert "pcv_validate_password_complexity(" not in reverted.split(
        "handle_auth_user_create", 1)[1], "치환 후에도 호출 잔존"
    with tempfile.NamedTemporaryFile("w", suffix=".c", delete=False) as f:
        f.write(reverted)
        tmp = f.name
    try:
        r = subprocess.run([sys.executable, str(GATE), tmp], capture_output=True, text=True)
        assert r.returncode == 1, f"호출 제거 사본에서 게이트가 RED 가 아님:\n{r.stdout}\n{r.stderr}"
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
    print(f"[test_password_policy] {len(tests) - failed}/{len(tests)} passed")
    sys.exit(1 if failed else 0)
