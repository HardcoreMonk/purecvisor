#!/usr/bin/env python3
                          
                                                                        
             
                                                                            
                                      
                                                                     
                                             
 
                      
                                                       
                                                     
               
                                                                                

             
                                                   
                                      
                          
                           
                                                        
                                                                 
                      
                                                                
                                                                
                                                                                   
                                                                           
                              
                                   
   
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GATE = ROOT / "scripts" / "check_ws_broadcast_auth.py"
WS = ROOT / "src" / "api" / "ws_server.c"
WS_SEND = ROOT / "src" / "api" / "ws_send.h"

BROADCAST_GUARD = """        if (!_ws_conn_is_authed(conn)) {
            i++;
            continue;
        }
"""
HEARTBEAT_GUARD = """        if (!_ws_conn_is_authed(conn))
            continue;
"""
BROADCAST_SEND = "        gboolean sent = pcv_ws_send_text_if_open(conn, msg);"
BROADCAST_DISPATCH = ("    if (!pcv_ws_dispatch_on_owner(owner, _ws_broadcast_fanout, msg, queued ? _ws_fanout_queued_free : g_free)) {\n"
                      "        if (queued) g_atomic_int_add(&ws_fanout_queued, -1);\n"
                      "        g_free(msg);\n"
                      "    }\n")
MT_BODY = """pcv_ws_broadcast_job_complete_mt(const gchar *job_id, const gchar *method,
                                  const gchar *status, const gchar *error_msg)
{
    _broadcast_job_complete(job_id, method, status, error_msg, -1, -1, NULL);
}
"""
HELPER_OPEN_CHECK = """    if (soup_websocket_connection_get_state(conn) != SOUP_WEBSOCKET_STATE_OPEN)
        return FALSE;"""

NEW_FANOUT = """
void
pcv_ws_broadcast_extra(const gchar *msg)
{
    g_mutex_lock(&G.mu);
    for (guint i = 0; i < G.clients->len; i++) {
        SoupWebsocketConnection *conn = g_ptr_array_index(G.clients, i);
%s        %s(conn, msg);
    }
    g_mutex_unlock(&G.mu);
}
"""
ROGUE_SENDER = """/* rogue.c — 헬퍼를 거치지 않는 전송(반례) */
#include "api/ws_send.h"
void rogue_send(SoupWebsocketConnection *conn)
{
    %s(conn, "{\\"type\\":\\"rogue\\"}");
}
"""


def _run(text: str | None = None, src_root: Path | None = None) -> subprocess.CompletedProcess:
    extra = ["--src-root", str(src_root)] if src_root else []
    if text is None:
        return subprocess.run([sys.executable, str(GATE), *extra], capture_output=True, text=True)
    fd, path = tempfile.mkstemp(suffix="-ws_server.c")
    try:
        with os.fdopen(fd, "w") as handle:
            handle.write(text)
        return subprocess.run([sys.executable, str(GATE), path, *extra],
                              capture_output=True, text=True)
    finally:
        os.unlink(path)


def _real() -> str:
    text = WS.read_text(errors="replace")
    assert BROADCAST_GUARD in text and HEARTBEAT_GUARD in text and BROADCAST_SEND in text
    assert BROADCAST_DISPATCH in text and MT_BODY in text
    return text


def _src_tree(directory: Path, rogue_call: str | None = None, helper_text: str | None = None) -> Path:
                                                                    
    src = directory / "src"
    (src / "api").mkdir(parents=True)
    if helper_text is None:
        shutil.copy2(WS_SEND, src / "api" / "ws_send.h")
    else:
        (src / "api" / "ws_send.h").write_text(helper_text)
    if rogue_call:
        (src / "api" / "rogue.c").write_text(ROGUE_SENDER % rogue_call)
    return src


def test_current_tree_pass():
    result = _run()
    assert result.returncode == 0, result.stdout + result.stderr


def test_broadcast_guard_removed_fails():
    result = _run(_real().replace(BROADCAST_GUARD, "", 1))
    assert result.returncode == 1, result.stdout
    assert "_ws_broadcast_fanout" in result.stdout


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
    for call in ("pcv_ws_send_text_if_open", "soup_websocket_connection_send_text"):
        unguarded = _run(_real() + NEW_FANOUT % ("", call))
        assert unguarded.returncode == 1, (call, unguarded.stdout)
        assert "pcv_ws_broadcast_extra" in unguarded.stdout, (call, unguarded.stdout)
    guarded = _run(_real() + NEW_FANOUT % (HEARTBEAT_GUARD, "pcv_ws_send_text_if_open"))
    assert guarded.returncode == 0, guarded.stdout


def test_unrelated_comment_passes():
    result = _run(_real() + "\n/* unrelated trailing comment */\n")
    assert result.returncode == 0, result.stdout + result.stderr


def test_broadcast_bypassing_owner_context_fails():
    direct = "    _ws_broadcast_fanout(msg);\n    g_free(msg);\n"
    result = _run(_real().replace(BROADCAST_DISPATCH, direct, 1))
    assert result.returncode == 1, result.stdout
    assert "R2" in result.stdout and "pcv_ws_dispatch_on_owner" in result.stdout


def test_mt_back_to_default_context_fails():
    legacy = MT_BODY.replace("    _broadcast_job_complete(job_id, method, status, error_msg, -1, -1, NULL);\n",
                             "    pcv_drain_invoke(NULL, _ws_some_cb, NULL);\n")
    text = _real()
    assert MT_BODY in text
    result = _run(text.replace(MT_BODY, legacy, 1))
    assert result.returncode == 1, result.stdout
    assert "R2" in result.stdout and "pcv_drain_invoke" in result.stdout


def test_init_ctx_from_default_context_fails():
    text = _real()
    old = "g_main_context_ref_thread_default();"
    assert text.count("GMainContext *owner = " + old) == 1
    result = _run(text.replace("GMainContext *owner = " + old,
                               "GMainContext *owner = g_main_context_ref(g_main_context_default());", 1))
    assert result.returncode == 1, result.stdout
    assert "R2" in result.stdout and "pcv_ws_server_init" in result.stdout


def test_broadcast_owner_from_default_context_fails():
    text = _real()
    old = "    GMainContext *owner = _ws_owner_ref();\n    gboolean queued ="
    assert old in text
    new = old.replace("_ws_owner_ref()", "g_main_context_default()")
    result = _run(text.replace(old, new, 1))
    assert result.returncode == 1, result.stdout
    assert "R2" in result.stdout and "pcv_ws_broadcast()의 소유 컨텍스트" in result.stdout


def test_mt_bypassing_broadcast_path_fails():
    bypass = MT_BODY.replace("    _broadcast_job_complete(job_id, method, status, error_msg, -1, -1, NULL);\n",
                             "    pcv_ws_dispatch_on_owner(g_main_context_default(), _ws_some_cb, NULL, NULL);\n")
    text = _real()
    assert MT_BODY in text
    result = _run(text.replace(MT_BODY, bypass, 1))
    assert result.returncode == 1, result.stdout
    assert "R2" in result.stdout and "pcv_ws_broadcast_job_complete_mt()" in result.stdout


def test_owner_ref_not_from_ctx_fails():
    text = _real()
    old = "? g_main_context_ref(G.ctx) : NULL;"
    assert old in text
    result = _run(text.replace(old, "? g_main_context_ref(g_main_context_default()) : NULL;", 1))
    assert result.returncode == 1, result.stdout
    assert "R2" in result.stdout and "_ws_owner_ref" in result.stdout


def test_raw_send_outside_helper_fails_and_helper_passes():
    with tempfile.TemporaryDirectory(prefix="pcv-ws-r4-red-") as directory:
        red = _run(src_root=_src_tree(Path(directory), "soup_websocket_connection_send_text"))
    assert red.returncode == 1, red.stdout
    assert "R4" in red.stdout and "src/api/rogue.c" in red.stdout
    with tempfile.TemporaryDirectory(prefix="pcv-ws-r4-green-") as directory:
        green = _run(src_root=_src_tree(Path(directory), "pcv_ws_send_text_if_open"))
    assert green.returncode == 0, green.stdout + green.stderr


def test_helper_without_open_check_fails():
    helper = WS_SEND.read_text()
    assert helper.count(HELPER_OPEN_CHECK) == 1
    with tempfile.TemporaryDirectory(prefix="pcv-ws-r4-helper-") as directory:
        result = _run(src_root=_src_tree(Path(directory), None, helper.replace(HELPER_OPEN_CHECK, "", 1)))
    assert result.returncode == 1, result.stdout
    assert "R4" in result.stdout and "SOUP_WEBSOCKET_STATE_OPEN" in result.stdout


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
