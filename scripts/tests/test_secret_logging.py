#!/usr/bin/env python3
"""check_secret_logging.py self-test (Wave A / A09·V14·V16).

① 현행 트리에서 게이트 PASS(exit 0).
② 반사실: is_auth 산정에서 _body_has_secret 항을 제거하면 게이트 RED.
   - 로직 단위: scan_text가 경로-only is_auth를 used=False로 판정.
   - 실파일 단위: 실제 rest_server.c의 `|| _body_has_secret(body_str, body_len)`
     항을 제거하면 게이트가 exit 1.
③ 헬퍼 정의가 사라지면(defined=False) RED.
"""
import os
import sys
import tempfile
import subprocess
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from check_secret_logging import scan_text, TARGET  # noqa: E402

GATE = Path(__file__).resolve().parent.parent / "check_secret_logging.py"

DEF = 'static gboolean _body_has_secret(const gchar *body, gsize len) { return FALSE; }\n'

NEW_SNIPPET = DEF + '''
gboolean is_auth = (g_strstr_len(path, -1, "/auth/token") != nullptr) ||
                   _body_has_secret(body_str, body_len);
'''

# 시정 전(예전): 경로만 매칭 — 본문 기반 마스킹 없음.
OLD_SNIPPET = DEF + '''
gboolean is_auth = (g_strstr_len(path, -1, "/auth/token") != nullptr) ||
                   (g_strstr_len(path, -1, "/auth/refresh") != nullptr);
'''


def test_new_snippet_used():
    """현행: is_auth 산정에 _body_has_secret 결합 + 정의 존재."""
    defined, used = scan_text(NEW_SNIPPET)
    assert defined and used


def test_old_snippet_not_used():
    """② 반사실(로직): 경로-only is_auth → used=False(RED 조건)."""
    defined, used = scan_text(OLD_SNIPPET)
    assert defined and not used, "경로-only is_auth가 여전히 used로 잡힘"


def test_missing_definition():
    """③ 헬퍼 정의가 없으면 defined=False."""
    snippet = ('gboolean is_auth = (x) || _body_has_secret(body_str, body_len);')
    defined, used = scan_text(snippet)
    assert not defined and used


def test_gate_passes_on_current_tree():
    """① 현행 트리에서 게이트 PASS(exit 0)."""
    r = subprocess.run([sys.executable, str(GATE)], capture_output=True, text=True)
    assert r.returncode == 0, f"{r.stdout}\n{r.stderr}"


def test_reverted_source_fails():
    """② 반사실(temp 사본): is_auth의 _body_has_secret 항을 FALSE로 되돌린 사본에서
    게이트가 exit 1. 추적 소스는 절대 건드리지 않는다.

    호출 사이트의 실인자 형태 `_body_has_secret(body_str, body_len)`만 대체하므로
    정의부(`_body_has_secret(const gchar *body, gsize len)`)는 그대로 남는다 →
    defined=True, used=False → is_auth 결합 위반으로 RED."""
    orig = TARGET.read_text()
    needle = "_body_has_secret(body_str, body_len)"
    assert needle in orig, "is_auth의 _body_has_secret 호출을 찾지 못함 — 소스 포맷 변경?"
    reverted = orig.replace(needle, "FALSE")
    assert reverted != orig, "is_auth OR 항 패치가 적용되지 않음 — 소스 포맷 변경?"
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
    print(f"[test_secret_logging] {len(tests) - failed}/{len(tests)} passed")
    sys.exit(1 if failed else 0)
