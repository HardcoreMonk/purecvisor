#!/usr/bin/env python3
"""핸들러 -32602 가드키 정적 추출기 단위 테스트."""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from rpc_extract import extract_fn_body, extract_handler_required, extract_method_to_fn


def test_method_to_fn():
    disp = 'g_hash_table_insert(g_rpc_routes, "network.mode_set", (gpointer)handle_net_mode);'
    assert extract_method_to_fn(disp)["network.mode_set"] == "handle_net_mode"


def test_fn_body_brace_match():
    src = 'void f(int a){ if(a){x();} } void g(){y();}'
    body = extract_fn_body(src, "f")
    assert "x();" in body and "y();" not in body


def test_require_macro():
    body = 'PCV_REQUIRE_PARAM(params, "vm_id", v, id, s, c);'
    assert extract_handler_required(body) == {"vm_id"}


def test_require_macro_or():
    body = 'PCV_REQUIRE_PARAM_OR(params, "name", "vm_id", v, id, s, c);'
    # OR = alias-group, frozenset 로 표현
    assert extract_handler_required(body) == {frozenset({"name", "vm_id"})}


def test_has_member_guard():
    body = ('if (!params || !json_object_has_member(params, "name") '
            '|| !json_object_has_member(params, "mode")) { return; }')
    assert extract_handler_required(body) == {"name", "mode"}


if __name__ == "__main__":
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for t in tests:
        t()
        print(f"  ok  {t.__name__}")
    print("OK")
