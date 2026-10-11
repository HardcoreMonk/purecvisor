#!/usr/bin/env python3




















import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GATE = ROOT / "scripts" / "check_ws_broadcast_auth.py"
WS = ROOT / "src" / "api" / "ws_server.c"

BROADCAST_GUARD = """        if (!_ws_conn_is_authed(conn)) {
            i++;
            continue;
        }
"""
HEARTBEAT_GUARD = """        if (!_ws_conn_is_authed(conn))
            continue;
"""
BROADCAST_SEND = "        soup_websocket_connection_send_text(conn, msg);"

NEW_FANOUT = """
void
pcv_ws_broadcast_extra(const gchar *msg)
{
    g_mutex_lock(&G.mu);
    for (guint i = 0; i < G.clients->len; i++) {
        SoupWebsocketConnection *conn = g_ptr_array_index(G.clients, i);
%s        soup_websocket_connection_send_text(conn, msg);
    }
    g_mutex_unlock(&G.mu);
}
"""


def _run(text: str | None = None) -> subprocess.CompletedProcess:
    if text is None:
        return subprocess.run([sys.executable, str(GATE)], capture_output=True, text=True)
    fd, path = tempfile.mkstemp(suffix="-ws_server.c")
    try:
        with os.fdopen(fd, "w") as handle:
            handle.write(text)
        return subprocess.run([sys.executable, str(GATE), path],
                              capture_output=True, text=True)
    finally:
        os.unlink(path)


def _real() -> str:
    text = WS.read_text(errors="replace")
    assert BROADCAST_GUARD in text and HEARTBEAT_GUARD in text and BROADCAST_SEND in text
    return text


def test_current_tree_pass():
    result = _run()
    assert result.returncode == 0, result.stdout + result.stderr


def test_broadcast_guard_removed_fails():
    result = _run(_real().replace(BROADCAST_GUARD, "", 1))
    assert result.returncode == 1, result.stdout
    assert "pcv_ws_broadcast" in result.stdout


def test_heartbeat_guard_removed_fails():
    result = _run(_real().replace(HEARTBEAT_GUARD, "", 1))
    assert result.returncode == 1, result.stdout
    assert "_ws_heartbeat_cb" in result.stdout


def test_guard_after_send_fails():
    text = _real().replace(BROADCAST_GUARD, "", 1)
    text = text.replace(BROADCAST_SEND, BROADCAST_SEND + "\n" + BROADCAST_GUARD, 1)
    result = _run(text)
    assert result.returncode == 1, result.stdout


def test_guard_hidden_in_comment_fails():
    text = _real().replace(BROADCAST_GUARD, "/*\n" + BROADCAST_GUARD + "*/\n", 1)
    result = _run(text)
    assert result.returncode == 1, result.stdout


def test_helper_accepting_non_one_fails():
    text = _real()
    old = '"pcv-ws-authed")) == 1;'
    assert old in text
    result = _run(text.replace(old, '"pcv-ws-authed")) != 0;', 1))
    assert result.returncode == 1, result.stdout
    assert "R3" in result.stdout


def test_new_unguarded_fanout_fails_and_guarded_passes():
    unguarded = _run(_real() + NEW_FANOUT % "")
    assert unguarded.returncode == 1, unguarded.stdout
    assert "pcv_ws_broadcast_extra" in unguarded.stdout
    guarded = _run(_real() + NEW_FANOUT % HEARTBEAT_GUARD)
    assert guarded.returncode == 0, guarded.stdout


def test_unrelated_comment_passes():
    result = _run(_real() + "\n/* unrelated trailing comment */\n")
    assert result.returncode == 0, result.stdout + result.stderr


def main() -> int:
    tests = [value for name, value in sorted(globals().items())
             if name.startswith("test_") and callable(value)]
    for test in tests:
        test()
        print(f"ok {test.__name__}")
    print(f"PASS {len(tests)} tests")
    return 0


if __name__ == "__main__":
    sys.exit(main())
