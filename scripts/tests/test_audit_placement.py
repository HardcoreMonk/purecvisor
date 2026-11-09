#!/usr/bin/env python3
                          
                                                                         
                                                                                 
                                                                       
                                                                   
                                                   
 
                      
                                                                                       
                                                             

                                             
                                                                  
                                                                 
                                                                         
                                                   
             

                                                              
   
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import check_audit_placement as gate              


                                                                  

def test_ws_regex_matches_plain_variant():
                                                           
    s = 'pcv_ws_broadcast_job_complete(job_id, "vm.start", "completed", NULL);'
    m = gate.WS_COMPLETE_RE.search(s)
    assert m is not None
    assert m.group(1) == "vm.start"


def test_ws_regex_matches_mt_variant():
                                               

                                                       
                                                 
       
    for s in (
        'pcv_ws_broadcast_job_complete_mt(job_id, "backup.restore", "failed", err);',
        'pcv_ws_broadcast_job_complete_mt(job_id, "vm.clone", "completed", NULL);',
        'pcv_ws_broadcast_job_complete_mt(ctx->job_id, "vm.export.ova",\n'
        '    "completed", NULL);',
    ):
        m = gate.WS_COMPLETE_RE.search(s)
        assert m is not None, f"_mt 변형 미매칭: {s!r}"
    assert gate.WS_COMPLETE_RE.search(
        'pcv_ws_broadcast_job_complete_mt(job_id, "backup.restore", "failed", err);'
    ).group(1) == "backup.restore"


def test_ws_regex_does_not_overmatch_unrelated():
                                        
    s = 'pcv_ws_broadcast("anomaly", payload);'
    assert gate.WS_COMPLETE_RE.search(s) is None


def test_ws_regex_matches_tracked_variants():

    for suffix in ("_tracked", "_tracked_mt"):
        statement = f'pcv_ws_broadcast_job_complete{suffix}(id, "backup.restore", "completed", NULL, persisted);'
        match = gate.WS_COMPLETE_RE.search(statement)
        assert match is not None
        assert match.group(1) == "backup.restore"




def test_audit_regex_matches_log_and_rpc():
                                                   

                                                           
                                                                       
       
    m_worker = gate.AUDIT_CALL_RE.search(
        'pcv_audit_log(NULL, "backup.restore", vm, d, 0, 0, "x");')
    assert m_worker is not None and m_worker.group(1) == "backup.restore"

    m_rpc = gate.AUDIT_CALL_RE.search('pcv_audit_log_rpc("vm.clone", params);')
    assert m_rpc is not None and m_rpc.group(1) == "vm.clone"


                                                  

def test_mt_only_methods_recognized_in_tree():
                                                            
    ws = gate.collect_methods(gate.WS_COMPLETE_RE) | gate.collect_durable_completion_methods()
    for m in ("backup.restore", "backup.replicate", "vm.clone",
              "vm.export.ova", "vm.import.ova"):
        assert m in ws, f"{m} 이 WS completion set 에 없음 (regex 위음성 회귀)"

def test_vm_delete_requires_job_completion(tmp_path, monkeypatch):

    import shutil
    for rel in ("src/api/ws_server.c", "src/utils/pcv_job_recovery.c", "src/modules/dispatcher/handler_backup.c",
                "src/modules/dispatcher/handler_accel.c", "src/modules/dispatcher/handler_vpc.c",
                *(p.relative_to(gate.REPO_ROOT).as_posix() for p in gate.rpc_extract.vm_family("vm_lifecycle"))):
        (tmp_path / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(gate.REPO_ROOT / rel, tmp_path / rel)
    for rel in (*gate.rpc_extract.DISPATCHER_SPLIT_FILES, gate.rpc_extract.DISPATCHER_ROUTES_REL):
        (tmp_path / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(gate.REPO_ROOT / rel, tmp_path / rel)
    monkeypatch.setattr(gate, "REPO_ROOT", tmp_path)
    assert "vm.delete" in gate.collect_durable_completion_methods()
    lifecycle = next(p for p in gate.rpc_extract.vm_family("vm_lifecycle", tmp_path)
                     if "_vm_delete_callback(GObject *src" in p.read_text())
    text = lifecycle.read_text()
    start = text.index("_vm_delete_callback(GObject *src")
    end = text.index("void handle_vm_delete_request(", start)
    lifecycle.write_text(text[:start] + text[start:end].replace("pcv_ws_finish_job(ctx->job_ref", "lost_completion(ctx->job_ref") + text[end:])
    assert "vm.delete" not in gate.collect_durable_completion_methods()

def test_audit_regex_matches_actor_call_expressions():

    old = re.compile(r'pcv_audit_log(?:_rpc)?\s*\(\s*[^)]*?"([a-z][a-z0-9_.]+)"', re.DOTALL)
    cases = {
        'pcv_audit_log(pcv_drain_task_actor(res), "vm.delete", ctx->target, "ok", 0, 0, "local");': "vm.delete",
        'pcv_audit_log(pcv_rpc_completion_actor(), "alert.ack", id_buf, "ok", 0, 0, "local");': "alert.ack",
        'pcv_audit_log(pcv_drain_task_actor(task),\n    "backup.replicate", t, "ok", 0, 0, "local");': "backup.replicate",
        'pcv_audit_log(PCV_AUDIT_ACTOR_SYSTEM, "plugin.load", path, "fail", 6, 0, "local");': "plugin.load",
        'pcv_audit_log(u ? u : "-", "auth.login", x, "ok", 0, 0, NULL);': "auth.login",
    }
    for text, method in cases.items():
        m = gate.AUDIT_CALL_RE.search(text)
        assert m is not None and m.group(1) == method, text
    assert old.search(next(iter(cases))) is None

def test_audit_regex_superset_on_tree():

    old = re.compile(r'pcv_audit_log(?:_rpc)?\s*\(\s*[^)]*?"([a-z][a-z0-9_.]+)"', re.DOTALL)
    assert gate.collect_methods(old) <= gate.collect_methods(gate.AUDIT_CALL_RE)

def _async_methods():
    return gate.extract_async_methods(gate.rpc_extract.dispatcher_route_file(gate.REPO_ROOT).read_text())

def test_late_response_table_matches_tree():

    assert gate.check_late_response_table(_async_methods()) == []

def test_late_response_unregistered_method_is_red():
    errs = gate.check_late_response_table([m for m in _async_methods() if m != "network.create"])
    assert any("network.create" in e for e in errs), errs

def test_late_response_table_drift_is_red(monkeypatch):
    table = {k: dict(v) for k, v in gate.LATE_RESPONSE_CALLBACKS.items()}
    del table["src/modules/network/network_manager.c"]
    table["src/modules/dispatcher/handler_vm_lifecycle.c"]["_gone_callback"] = ["vm.list"]
    monkeypatch.setattr(gate, "LATE_RESPONSE_CALLBACKS", table)
    errs = gate.check_late_response_table(_async_methods())
    assert any("표에 없는 나중 응답 콜백: src/modules/network/network_manager.c::network_action_callback" in e for e in errs), errs
    assert any("표가 낡았다: src/modules/dispatcher/handler_vm_lifecycle.c::_gone_callback" in e for e in errs), errs

def test_network_audit_visible():
    assert {"network.create", "network.delete"} <= gate.collect_audit_methods()

def test_literal_null_actor_detected(tmp_path):
    src = tmp_path / "src/modules/x"
    src.mkdir(parents=True)
    (src / "a.c").write_text('/* pcv_audit_log(NULL, "doc.only", t) */\nvoid f(void) {\n'
                             '    pcv_audit_log(NULL, "x.one", t, "ok", 0, 0, "local");\n'
                             '    pcv_audit_log( nullptr , "x.two", t, "ok", 0, 0, "local");\n'
                             '    pcv_audit_log(actor, "x.three", t, "ok", 0, 0, "local");\n}\n')
    assert gate.literal_null_actor_sites(tmp_path) == ["src/modules/x/a.c:3", "src/modules/x/a.c:4"]

def test_tree_has_no_literal_null_actor():
    assert gate.literal_null_actor_sites() == []


def test_gate_passes_on_current_tree():
                                                      
    assert gate.main() == 0


if __name__ == "__main__":
    sys.exit(__import__("pytest").main([__file__, "-q"]))
