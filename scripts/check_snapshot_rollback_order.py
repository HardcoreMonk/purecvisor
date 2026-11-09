#!/usr/bin/env python3
                          
                                                                       
                                                                
                                                                                   
                                                                     
                                                                            
                                                                         
                                                  
                                              
 
                      
                                                       
                                                                 
                                                                           

      
                                                                                          
                                              
                                                                          
                                                                             
                                                                                                    
                                                                                                  
                                                                                       
                                                                                        
                                                                                   
                                                                                              
                                                                      
                                                                                              
                                   
                                                                                  
                                                                                                    
                                                                                                    
                                                                                                                           

                                  
   
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_SOURCE = ROOT / "src" / "modules" / "dispatcher" / "handler_snapshot.c"


class ExtractError(Exception):
    pass


def strip_comments(text: str) -> str:
                                                                
    out: list[str] = []
    i, n = 0, len(text)
    state = "code"
    quote = ""
    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if state == "code":
            if ch == "/" and nxt == "/":
                state = "line"
                out.append("  ")
                i += 2
                continue
            if ch == "/" and nxt == "*":
                state = "block"
                out.append("  ")
                i += 2
                continue
            if ch in ('"', "'"):
                state = "literal"
                quote = ch
            out.append(ch)
            i += 1
        elif state == "literal":
            out.append(ch)
            if ch == "\\" and i + 1 < n:
                out.append(text[i + 1])
                i += 2
                continue
            if ch == quote or ch == "\n":
                state = "code"
            i += 1
        elif state == "line":
            if ch == "\n":
                state = "code"
                out.append("\n")
            else:
                out.append(" ")
            i += 1
        else:         
            if ch == "*" and nxt == "/":
                state = "code"
                out.append("  ")
                i += 2
                continue
            out.append("\n" if ch == "\n" else " ")
            i += 1
    return "".join(out)


def function_body(code: str, name: str) -> str:
                                                
    match = re.search(rf"\b{re.escape(name)}\s*\([^;{{]*\)\s*\{{", code)
    if not match:
        raise ExtractError(f"missing definition of {name}()")
    depth = 0
    start = match.end() - 1
    for pos in range(start, len(code)):
        if code[pos] == "{":
            depth += 1
        elif code[pos] == "}":
            depth -= 1
            if depth == 0:
                return code[start + 1:pos]
    raise ExtractError(f"unbalanced braces in {name}()")


def block_after(code: str, anchor: str) -> str | None:
                                              
    idx = code.find(anchor)
    if idx < 0:
        return None
    brace = code.find("{", idx)
    if brace < 0:
        return None
    depth = 0
    for pos in range(brace, len(code)):
        if code[pos] == "{":
            depth += 1
        elif code[pos] == "}":
            depth -= 1
            if depth == 0:
                return code[brace + 1:pos]
    return None


UNLOCK_TOKENS = ("pcv_vm_lock_release", "pcv_snapshot_lock_keeper_stop_and_release")


def _last_unlock(body: str) -> int:
                                                                                     
    return max(body.rfind(tok) for tok in UNLOCK_TOKENS)


def _first(body: str, token: str) -> int:
    idx = body.find(token)
    return idx if idx >= 0 else len(body) + 1


def check_text(text: str) -> list[str]:
                                                         
    code = strip_comments(text)
    fails: list[str] = []

    worker = function_body(code, "_rollback_worker")
    exists_at = worker.find("_zfs_snapshot_exists(")
    power_at = min(_first(worker, "virDomainShutdown("), _first(worker, "virDomainDestroy("))
    if exists_at < 0:
        fails.append("① _rollback_worker 가 _zfs_snapshot_exists() 로 스냅샷 존재를 확인하지 않는다")
    elif exists_at > power_at:
        fails.append("① _rollback_worker 가 스냅샷 존재 확인 전에 VM 을 종료/destroy 한다")
    elif "g_task_return_new_error(" not in worker[exists_at:power_at]:
        fails.append("① 스냅샷 미존재 시 VM 제어 전에 실패 반환(g_task_return_new_error)이 없다")

    done = function_body(code, "_on_rollback_done")
    if "virDomainCreate(" in worker:
        fails.append("② _rollback_worker 가 virDomainCreate 로 직접 재기동한다")
    elif ("pcv_snapshot_rollback_should_restart(PCV_ROLLBACK_PATH_ZFS" not in done
          or "_rollback_submit_restart(" not in done):
        fails.append("② ZFS 롤백 실패 뒤 끈 VM 을 정식 경로로 재기동하는 호출이 _on_rollback_done 에 없다")
    else:
        zfs_restart_at = done.find("_rollback_submit_restart(")
        zfs_respond_at = done.rfind("pure_uds_server_send_response(")
        zfs_unlock_at = _last_unlock(done)
        if zfs_respond_at < 0 or zfs_unlock_at < 0 or zfs_restart_at < zfs_respond_at or zfs_restart_at < zfs_unlock_at:
            fails.append("② ZFS 롤백 완료 콜백이 응답·잠금 해제(keeper 해제 포함) 전에 재기동을 요청한다 — 자기 잠금에 막힌다")

    lv_worker = function_body(code, "_libvirt_rollback_worker")
    lv_done = function_body(code, "_on_libvirt_rollback_done")
    lookup_at = lv_worker.find("virDomainSnapshotLookupByName(")
    shutdown_at = _first(lv_worker, "virDomainShutdown(")
    if lookup_at < 0 or lookup_at > shutdown_at:
        fails.append("③ libvirt 폴백 워커가 스냅샷 조회 전에 VM 을 종료한다")

    revert_at = lv_worker.find("virDomainRevertToSnapshot(")
    if revert_at >= 0:
        after = lv_worker[revert_at:]
        err_at, free_at = _first(after, "virGetLastError("), _first(after, "virDomainSnapshotFree(")
        if "virGetLastError(" not in after or "virDomainSnapshotFree(" not in after:
            fails.append("③ libvirt 폴백 워커의 virDomainRevertToSnapshot() 뒤에 virGetLastError() 또는 "
                         "virDomainSnapshotFree() 가 없다 — 오류 읽기→해제 순서를 확인할 수 없다 "
                         "(SNAPSHOT-ROLLBACK-LIBVIRT-ERROR-TEXT)")
        elif err_at > free_at:
            fails.append("③ libvirt 폴백 워커가 스냅샷 해제 뒤에 libvirt 오류를 읽는다 — "
                         "read the libvirt error before freeing the snapshot (SNAPSHOT-ROLLBACK-LIBVIRT-ERROR-TEXT)")
    else:
        fails.append("③ libvirt 폴백 워커에 virDomainRevertToSnapshot() 호출이 없다")

    worker_io = [tok for tok in ("pure_rpc_build_error_response(", "pure_rpc_build_success_response(",
                                 "pure_uds_server_send_response(", "pcv_audit_log(") if tok in lv_worker]
    if worker_io:
        fails.append(f"④ libvirt 폴백 워커가 직접 응답·감사한다({', '.join(worker_io)}) — "
                     "완료 콜백 _on_libvirt_rollback_done 만 한다")
    error_responses = lv_done.count("pure_rpc_build_error_response(")
    fail_audits = len(re.findall(r'pcv_audit_log\([^;]*"vm\.snapshot\.rollback"[^;]*"fail"', lv_done))
    if fail_audits < error_responses:
        fails.append(f"④ libvirt 폴백 오류 응답 {error_responses}건 중 fail audit 은 {fail_audits}건뿐이다")

    for tok in ("_rollback_submit_restart(", "pcv_vm_start_submit("):
        if tok in lv_worker:
            fails.append(f"⑤ libvirt 폴백 워커가 {tok}) 로 재기동한다 — 재기동은 메인 루프 완료 콜백만")
    restart_at = lv_done.find("_rollback_submit_restart(")
    respond_at = lv_done.rfind("pure_uds_server_send_response(")
    unlock_at = _last_unlock(lv_done)
    if restart_at < 0 or "pcv_snapshot_rollback_should_restart(PCV_ROLLBACK_PATH_LIBVIRT" not in lv_done:
        fails.append("⑤ libvirt 폴백 완료 콜백이 정식 경로 재기동"
                     "(pcv_snapshot_rollback_should_restart(PCV_ROLLBACK_PATH_LIBVIRT → _rollback_submit_restart)을 하지 않는다")
    elif respond_at < 0 or unlock_at < 0 or restart_at < respond_at or restart_at < unlock_at:
        fails.append("⑤ libvirt 폴백 완료 콜백이 응답·잠금 해제 전에 재기동을 요청한다")
    return fails


def main(argv: list[str]) -> int:
    source = Path(argv[1]) if len(argv) > 1 else DEFAULT_SOURCE
    try:
        text = source.read_text(encoding="utf-8")
    except OSError as exc:
        print(f"ERROR: failed to read {source}: {exc}", file=sys.stderr)
        return 2
    try:
        fails = check_text(text)
    except ExtractError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2
    if fails:
        print("[FAIL] snapshot rollback 선확인 계약 위반:", file=sys.stderr)
        for reason in fails:
            print(f"  - {reason}", file=sys.stderr)
        return 1
    print("[PASS] snapshot rollback: 존재 확인→전원 제어 순서·실패 재기동·폴백 audit 계약 유지")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
