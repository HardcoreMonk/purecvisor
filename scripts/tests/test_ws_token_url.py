
"""check_ws_token_url.py self-test (Q-5 / A07).

① 현행 트리에서 게이트 PASS(exit 0).
② 반사실: URL 토큰을 읽는 함수에 pcv_jwt_verify 를 되살린 사본에서 RED(exit 1).
③ 로직 단위: URL-query 토큰 인증 블록 검출 / 메시지 경로 인증 인식 / 오탐 없음.
"""
import os
import sys
import tempfile
import subprocess
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from check_ws_token_url import scan_text, TARGET

GATE = Path(__file__).resolve().parent.parent / "check_ws_token_url.py"

URL_AUTH_BLOCK = '''
static gboolean _ws_auth_callback(SoupServerMessage *msg) {
    const gchar *query = g_uri_get_query(uri);
    GHashTable *params = g_uri_parse_params(query, -1, "&", 0, NULL);
    const gchar *t = g_hash_table_lookup(params, "token");
    gchar *subject = pcv_jwt_verify(t, &err);
    if (subject) return TRUE;
    return FALSE;
}
'''

MSG_AUTH_BLOCK = '''
static void _on_ws_message(SoupWebsocketConnection *conn, GBytes *message) {
    const gchar *token = json_object_get_string_member_with_default(obj, "token", "");
    gchar *subject = pcv_jwt_verify(token, &jwt_err);
    if (subject) mark_authed(conn);
}
'''

WARN_ONLY_BLOCK = '''
static void _ws_warn_deprecated_url_token(SoupServerMessage *msg) {
    const gchar *query = g_uri_get_query(uri);
    GHashTable *params = g_uri_parse_params(query, -1, "&", 0, NULL);
    const gchar *t = g_hash_table_lookup(params, "token");
    if (t && t[0] != '\\0') PCV_LOG_WARN(WS_LOG_DOM, "deprecated");
}
'''

def test_url_auth_flagged():
    """③ URL-query 토큰 인증 블록 → 위반 1."""
    violations, _ = scan_text(URL_AUTH_BLOCK)
    assert violations == 1, f"URL 토큰 인증이 안 잡힘: {violations}"

def test_message_auth_not_flagged_and_recognized():
    """③ 메시지 경로 인증: 위반 아님 + positive control 인식."""
    violations, has_msg = scan_text(MSG_AUTH_BLOCK)
    assert violations == 0
    assert has_msg

def test_warn_only_not_flagged():
    """③ 시정 후 deprecated 경고 블록: URL+token 을 읽어도 verify 없으면 위반 아님."""
    violations, _ = scan_text(WARN_ONLY_BLOCK)
    assert violations == 0

def test_gate_passes_on_current_tree():
    """① 현행 트리에서 게이트 PASS(exit 0)."""
    r = subprocess.run([sys.executable, str(GATE)], capture_output=True, text=True)
    assert r.returncode == 0, f"{r.stdout}\n{r.stderr}"

def test_reverted_source_fails():
    """② 반사실(temp 사본): 실제 ws_server.c 의 deprecated URL 토큰 경고 함수에
    pcv_jwt_verify 를 되살려 URL-query 인증을 재도입하면 게이트가 exit 1."""
    orig = TARGET.read_text()
    needle = 'const gchar *t = g_hash_table_lookup(params, "token");'
    assert needle in orig, "URL 토큰 추출 지점을 찾지 못함 — 소스 포맷 변경?"

    injected = needle + "\n    gchar *_sub = pcv_jwt_verify(t, NULL); (void)_sub;"
    reverted = orig.replace(needle, injected, 1)
    with tempfile.NamedTemporaryFile("w", suffix=".c", delete=False) as f:
        f.write(reverted)
        tmp = f.name
    try:
        r = subprocess.run([sys.executable, str(GATE), tmp], capture_output=True, text=True)
        assert r.returncode == 1, f"URL 인증 재도입 사본에서 게이트가 RED 가 아님:\n{r.stdout}\n{r.stderr}"
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
    print(f"[test_ws_token_url] {len(tests) - failed}/{len(tests)} passed")
    sys.exit(1 if failed else 0)
