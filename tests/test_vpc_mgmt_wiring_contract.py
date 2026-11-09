#!/usr/bin/env python3
                                                                                               
                  
                                                                                          
                                                                               
                                                                                     
                                                                                  
                                                    
                                                                            
                                                                 
                        
                                                                         
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HANDLER = ROOT / "src" / "modules" / "dispatcher" / "handler_vpc.c"
RESOURCE_HANDLER = ROOT / "src" / "modules" / "dispatcher" / "handler_resource.c"
REJECT_PHRASE = "overlaps a reserved host management range"
POOL_REJECT_PHRASE = "outside this tenant's address pool"


def code(path: Path = HANDLER) -> str:
                                         
    return re.sub(r"/\*.*?\*/|//[^\n]*", lambda m: re.sub(r"[^\n]", " ", m[0]),
                  path.read_text(), flags=re.S)


def body(text: str, signature: str) -> str:
                                
    start = text.index(signature)
    i = text.index("{", start)
    depth = 0
    for j in range(i, len(text)):
        depth += (text[j] == "{") - (text[j] == "}")
        if depth == 0:
            return text[i:j + 1]
    raise AssertionError(signature)


def first(text: str, pattern: str) -> int:
                                     
    m = re.search(pattern, text)
    assert m, f"{pattern!r} 없음"
    return m.start()


CREATE_HANDLERS = ("handle_vpc_create(JsonObject", "handle_vpc_subnet_create(JsonObject")


def test_admission_before_enqueue():
                                                                                            
                                                        
    text = code()
    for sig in CREATE_HANDLERS:
        b = body(text, sig)
        target = first(b, r"data->target\s*=")
        admission = first(b, r"\b_mgmt_admission\(")
        enqueue = first(b, r"\b_enqueue\(")
        assert target < admission < enqueue, sig


def test_admission_after_cidr_validation():
                                                                                           
                                                                                         
    text = code()
    for sig in CREATE_HANDLERS:
        b = body(text, sig)
        assert first(b, r"\bpcv_vpc_cidr_parse\(") < first(b, r"\b_mgmt_admission\("), sig


def test_worker_records_pending_before_finish():
                                                                                            
                                                        
    text = code()
    b = body(text, "_vpc_worker(GTask")
    add = first(b, r"\bpcv_vpc_mgmt_pending_add\(")
    assert add < first(b, r"\bpcv_ws_finish_job\(")
    assert len(re.findall(r"\bpcv_vpc_mgmt_arm_pending\(", text)) == 1
    assert add < first(b, r"\bpcv_vpc_mgmt_arm_pending\(")


def test_attachment_create_rejects_pending():
                                                                                                                 
    b = body(code(), "handle_vpc_attachment_create(JsonObject")
    enqueue = first(b, r"\b_enqueue\(")
    assert first(b, r"\bpcv_vpc_mgmt_pending_lookup\(") < enqueue
    assert first(b, r"\bPURE_RPC_ERR_CONFLICT\b") < enqueue


def _forbidden_call_without_value(text: str, phrase: str) -> None:
                                                                                         
    hits = [m.start() for m in re.finditer(re.escape(phrase), text)]
    assert len(hits) == 1, hits
    at = hits[0]
    start = max(text.rfind(";", 0, at), text.rfind("{", 0, at), text.rfind("}", 0, at)) + 1
    call = text[start:text.index(";", at) + 1]
    assert "PURE_RPC_ERR_FORBIDDEN" in call, call
    literal = re.search(r'"(?:\\.|[^"\\])*' + re.escape(phrase) + r'(?:\\.|[^"\\])*"', call)
    assert literal, call
    assert "%" not in literal.group(0), literal.group(0)


def test_operator_reject_uses_forbidden_without_value():
                                                                                                  
                                            
    _forbidden_call_without_value(code(), REJECT_PHRASE)


def test_operator_pool_check_before_reserved():
                                                                                         
                                                                                       
                                                                                     
                                                                                                           
    text = code()
    b = body(text, "_mgmt_admission(VpcWorkerData")
    pool = first(b, r"\bpcv_res_pool_list\(")
    assert pool < first(b, r"\bpcv_vpc_mgmt_pool_covers\(") < first(b, r"\bpcv_vpc_mgmt_reserved_set\(")
    assert "_pool_covers(" not in text.replace("pcv_vpc_mgmt_pool_covers(", ""), "핸들러 안 복제 판정이 되살아났다"
    assert first(b, r"!\s*data->actor_is_admin") < pool
    assert first(b, re.escape(POOL_REJECT_PHRASE)) < first(b, r"\bpcv_vpc_mgmt_reserved_set\(")
    _forbidden_call_without_value(text, POOL_REJECT_PHRASE)


def test_caller_scope_uses_resource_caller():
                                                                                                 
                                                                                                          
                                                            
    b = body(code(), "_caller_scope(JsonObject")
    assert re.search(r"\bpcv_res_caller_init\(", b), b
    assert re.search(r"\bpcv_res_caller_clear\(", b), b
    assert not re.search(r"\bpcv_rbac_get_tenant\(", b), b


def test_unregistered_lists_vpc_subnets():
                                                                                                         
                                                                                                        
                                                        
    src = code(RESOURCE_HANDLER)
    b = body(src, "handle_resource_unregistered_list(JsonObject")
    assert '"vpc_subnets"' in b, b
    assert re.search(r"\bpcv_res_pool_list\(", b), b
    assert re.search(r"\bpcv_vpc_mgmt_pool_covers\(", body(src, "_append_outside_pool(JsonObject")), "포함 판정은 공용 TU"
    imp = body(src, "handle_resource_import(JsonObject")
    assert '"vpc_pool"' in src
                                                                        
    assert first(imp, r"\b_pool_items_precheck\(") < first(imp, r"\bpcv_res_import\(") < first(imp, r"\bpcv_res_pool_add\(")


def test_pool_set_checks_shrink_before_write():
                                                                                                         
                                                                                                
                                          
                                                                                                          
    b = body(code(), "handle_vpc_pool_set(JsonObject")
    status = first(b, r"\bpcv_vpc_status\(")
    outside = first(b, r"\bpcv_vpc_mgmt_pool_outside\(")
    conflict = first(b, r"\bPURE_RPC_ERR_CONFLICT\b")
    write = first(b, r"\bpcv_res_pool_set\(")
    assert status < outside < conflict < write, (status, outside, conflict, write)
    assert len(re.findall(r"\bpcv_res_pool_set\(", b)) == 1


def test_revert_unrecorded_unknown_subnet_is_not_reverted():
                                                                                 
                                                              
    b = body(code(), "_revert_unrecorded(const PcvVpcMgmtPending")
    assert not re.search(r"reverted\s*=\s*!\s*p->subnet_id", b), b
    assert re.search(r"gboolean\s+reverted\s*=\s*FALSE\s*;", b), b
    assert re.search(r"if\s*\(\s*p->subnet_id\s*\)\s*reverted\s*=", b), b
