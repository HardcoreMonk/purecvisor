#!/usr/bin/env python3
                          
                                                                   
                                                                      
                                                                        
                                                         
 
                      
                                                       
                                              
                                                             

             
                                 
                                       
                               
                                                           
                                                    
                     
                                                    
                                                       
                                                               
                                                          
                                             
                                         
                                                                       
   
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import check_snapshot_rollback_order as gate              

GATE = Path(__file__).resolve().parent.parent / "check_snapshot_rollback_order.py"
SOURCE = gate.DEFAULT_SOURCE

EXISTS_GUARD = "    if (!_zfs_snapshot_exists(d->vm_name, d->snap_name)) {"
LIBVIRT_WORKER = "_libvirt_rollback_worker"
LIBVIRT_DONE = "_on_libvirt_rollback_done"
LIBVIRT_LOOKUP = (
    "    virDomainSnapshotPtr snap = virDomainSnapshotLookupByName(dom, d->snap_name, 0);\n"
)
LIBVIRT_REVERT = "    int rc = virDomainRevertToSnapshot(snap, 0);"
LIBVIRT_AUDIT = (
    '    pcv_audit_log(pcv_drain_task_actor(res), "vm.snapshot.rollback", d->vm_name, ok ? "ok" : "fail",\n'
    '                  ok ? 0 : PURE_RPC_ERR_ZFS_OPERATION, 0, "local");\n'
)
LIBVIRT_RESTART = (
    "    if (pcv_snapshot_rollback_should_restart(PCV_ROLLBACK_PATH_LIBVIRT, d->was_running, ok))\n"
    "        _rollback_submit_restart(d->vm_name, pcv_drain_task_actor(res));\n"
)
LIBVIRT_RESPOND = "    pure_uds_server_send_response(d->server, d->connection, resp);\n"
LIBVIRT_UNLOCK = "    g_clear_pointer(&d->keeper, pcv_snapshot_lock_keeper_stop_and_release);"
OLD_UNLOCK = "    g_clear_pointer(&d->lock, pcv_vm_lock_release);"                                         
ZFS_DONE = "_on_rollback_done"
ZFS_RESTART = (
    "    if (pcv_snapshot_rollback_should_restart(PCV_ROLLBACK_PATH_ZFS, d->was_running, _ok))\n"
    "        _rollback_submit_restart(d->vm_name, pcv_drain_task_actor(res));\n"
)
ZFS_UNLOCK = LIBVIRT_UNLOCK


def _run_gate_on(text: str) -> int:
    handle = tempfile.NamedTemporaryFile("w", suffix="_handler_snapshot.c", delete=False)
    handle.write(text)
    handle.close()
    try:
        result = subprocess.run([sys.executable, str(GATE), handle.name],
                                capture_output=True, text=True)
        return result.returncode
    finally:
        os.unlink(handle.name)


def _source() -> str:
    return SOURCE.read_text(encoding="utf-8")


def _mutate_function(text: str, name: str, old: str, new: str) -> str:
                                                                    
    body = gate.function_body(text, name)
    assert old in body, f"{name} 포맷 변경? ({old.strip()[:60]})"
    return text.replace(body, body.replace(old, new, 1))


def _assert_red(mutated: str, item: str) -> None:
                                                                       
    reasons = gate.check_text(mutated)
    assert any(r.startswith(item) for r in reasons), reasons
    assert _run_gate_on(mutated) == 1


def test_gate_passes_on_current_tree():
                         
    result = subprocess.run([sys.executable, str(GATE)], capture_output=True, text=True)
    assert result.returncode == 0, f"{result.stdout}\n{result.stderr}"
    assert gate.check_text(_source()) == []


def test_missing_existence_check_fails():
                                        
    text = _source()
    assert EXISTS_GUARD in text, "존재 확인 가드 포맷 변경?"
    mutated = text.replace(EXISTS_GUARD, "    if (0) {")
    assert gate.check_text(mutated)
    assert _run_gate_on(mutated) == 1


def test_existence_check_after_shutdown_fails():
                                           
    text = _source()
    worker = gate.function_body(text, "_rollback_worker")
    start = worker.index(EXISTS_GUARD)
    end = worker.index("    }\n", start) + len("    }\n")
    guard_block = worker[start:end]
    anchor = '    gchar *dataset = g_strdup_printf("%s/%s@%s",\n'
    assert anchor in worker, "ZFS 롤백 단계 주석 포맷 변경?"
    moved = worker.replace(guard_block, "").replace(anchor, guard_block + anchor)
    mutated = text.replace(worker, moved)
    assert any(r.startswith("①") for r in gate.check_text(mutated))
    assert _run_gate_on(mutated) == 1


def test_no_restart_on_rollback_failure_fails():
                                              
    mutated = _mutate_function(_source(), "_on_rollback_done",
                               "_rollback_submit_restart(d->vm_name, pcv_drain_task_actor(res));", "")
    _assert_red(mutated, "②")


def test_libvirt_lookup_after_shutdown_fails():
                                                         
    text = _source()
    worker = gate.function_body(text, LIBVIRT_WORKER)
    assert LIBVIRT_LOOKUP in worker and LIBVIRT_REVERT in worker, "폴백 워커 포맷 변경?"
    moved = worker.replace(LIBVIRT_LOOKUP, "    virDomainSnapshotPtr snap = NULL;\n")
    moved = moved.replace(
        LIBVIRT_REVERT,
        "    snap = virDomainSnapshotLookupByName(dom, d->snap_name, 0);\n" + LIBVIRT_REVERT)
    _assert_red(text.replace(worker, moved), "③")


def _with_revert_tail(tail: str) -> str:
                                                                           
    text = _source()
    worker = gate.function_body(text, LIBVIRT_WORKER)
    start = worker.index(LIBVIRT_REVERT)
    end = worker.index("    virDomainFree(dom);", start)
    return text.replace(worker, worker[:start] + tail + worker[end:])


def test_libvirt_error_read_after_snapshot_free_fails():
                                                                                 
    mutated = _with_revert_tail(
        LIBVIRT_REVERT + "\n    virDomainSnapshotFree(snap);\n    virErrorPtr verr = virGetLastError();\n")
    reasons = gate.check_text(mutated)
    assert any("read the libvirt error before freeing" in r for r in reasons), reasons
    assert _run_gate_on(mutated) == 1


def test_libvirt_error_read_before_snapshot_free_passes():
                                            
    mutated = _with_revert_tail(
        LIBVIRT_REVERT + "\n    virErrorPtr verr = virGetLastError();\n    virDomainSnapshotFree(snap);\n")
    assert gate.check_text(mutated) == []
    assert _run_gate_on(mutated) == 0


def test_libvirt_revert_tail_missing_error_read_and_free_fails():
                                                                                       
    mutated = _with_revert_tail(LIBVIRT_REVERT + "\n")
    reasons = gate.check_text(mutated)
    assert any(r.startswith("③") and "없다" in r for r in reasons), reasons
    assert _run_gate_on(mutated) == 1


def test_libvirt_revert_tail_missing_free_only_fails():
                                              
    mutated = _with_revert_tail(LIBVIRT_REVERT + "\n    virErrorPtr verr = virGetLastError();\n")
    assert any(r.startswith("③") for r in gate.check_text(mutated))
    assert _run_gate_on(mutated) == 1


def test_libvirt_done_without_fail_audit_fails():
                                                                      
    _assert_red(_mutate_function(_source(), LIBVIRT_DONE, LIBVIRT_AUDIT, ""), "④")


def test_libvirt_worker_responds_directly_fails():
                                             
    inject = ('    gchar *e = pure_rpc_build_error_response(d->rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "x");\n'
              "    pure_uds_server_send_response(d->server, d->connection, e);\n")
    _assert_red(_mutate_function(_source(), LIBVIRT_WORKER, LIBVIRT_REVERT, inject + LIBVIRT_REVERT), "④")


def test_libvirt_restart_before_response_fails():
                                       
    text = _mutate_function(_source(), LIBVIRT_DONE, LIBVIRT_RESTART, "")
    _assert_red(_mutate_function(text, LIBVIRT_DONE, LIBVIRT_RESPOND, LIBVIRT_RESTART + LIBVIRT_RESPOND), "⑤")


def test_libvirt_restart_before_lock_release_fails():
                                                               
    text = _mutate_function(_source(), LIBVIRT_DONE, LIBVIRT_RESTART, "")
    _assert_red(_mutate_function(text, LIBVIRT_DONE, LIBVIRT_UNLOCK, LIBVIRT_RESTART + LIBVIRT_UNLOCK), "⑤")


def test_zfs_restart_before_lock_release_fails():
                                                                           
    text = _mutate_function(_source(), ZFS_DONE, ZFS_RESTART, "")
    _assert_red(_mutate_function(text, ZFS_DONE, ZFS_UNLOCK, ZFS_RESTART + ZFS_UNLOCK), "②")


def test_old_unlock_token_still_recognized():
                                                                                                  
    text = _mutate_function(_source(), LIBVIRT_DONE, LIBVIRT_UNLOCK, OLD_UNLOCK)
    text = _mutate_function(text, ZFS_DONE, ZFS_UNLOCK, OLD_UNLOCK)
    assert gate.check_text(text) == []
    assert _run_gate_on(text) == 0


def test_libvirt_worker_restart_fails():
                                                                   
    inject = "    _rollback_submit_restart(d->vm_name, PCV_AUDIT_ACTOR_SYSTEM);\n"
    _assert_red(_mutate_function(_source(), LIBVIRT_WORKER, LIBVIRT_REVERT, LIBVIRT_REVERT + inject), "⑤")


def test_libvirt_worker_vm_start_submit_fails():
                                                    
    inject = "    (void)pcv_vm_start_submit(&req, NULL, NULL, &err);\n"
    _assert_red(_mutate_function(_source(), LIBVIRT_WORKER, LIBVIRT_REVERT, LIBVIRT_REVERT + inject), "⑤")


def test_libvirt_done_without_restart_fails():
                                                            
    _assert_red(_mutate_function(_source(), LIBVIRT_DONE, LIBVIRT_RESTART, ""), "⑤")


def test_old_synchronous_fallback_name_fails_extraction():
                                                                               
    text = _source().replace(LIBVIRT_WORKER, "_libvirt_snapshot_rollback")
    try:
        gate.check_text(text)
    except gate.ExtractError:
        pass
    else:
        raise AssertionError("ExtractError 가 나야 한다")
    assert _run_gate_on(text) == 2


def test_comment_only_call_does_not_pass():
                                                  
    text = _source()
    mutated = text.replace(EXISTS_GUARD, "    /* _zfs_snapshot_exists(x) */ if (0) {")
    assert _run_gate_on(mutated) == 1


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
    print(f"[test_snapshot_rollback_order] {len(tests) - failed}/{len(tests)} passed")
    sys.exit(1 if failed else 0)
