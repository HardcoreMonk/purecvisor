#!/usr/bin/env python3
                          
                                                                         
                                                                                 
                                                                       
                                                                   
                                                   
 
                      
                                                                                       
                                                             

                                             
                                                                  
                                                                 
                                                                         
                                                   
             

                                                              
   
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


                                                                    

def test_audit_regex_matches_log_and_rpc():
                                                   

                                                           
                                                                       
       
    m_worker = gate.AUDIT_CALL_RE.search(
        'pcv_audit_log(NULL, "backup.restore", vm, d, 0, 0, "x");')
    assert m_worker is not None and m_worker.group(1) == "backup.restore"

    m_rpc = gate.AUDIT_CALL_RE.search('pcv_audit_log_rpc("vm.clone", params);')
    assert m_rpc is not None and m_rpc.group(1) == "vm.clone"


                                                  

def test_mt_only_methods_recognized_in_tree():
                                                            
    ws = gate.collect_methods(gate.WS_COMPLETE_RE)
    for m in ("backup.restore", "backup.replicate", "vm.clone",
              "vm.export.ova", "vm.import.ova"):
        assert m in ws, f"{m} 이 WS completion set 에 없음 (regex 위음성 회귀)"


def test_gate_passes_on_current_tree():
                                                      
    assert gate.main() == 0


if __name__ == "__main__":
    sys.exit(__import__("pytest").main([__file__, "-q"]))
