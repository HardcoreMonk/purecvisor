import sys
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(SCRIPTS))
from rpc_extract import (REST_RE, FE_HELP_RE, extract_rest_methods,
                          extract_fe_helper, extract_grpc_methods, strip_comments)


def test_rest_build_rpc_extraction():
    src = '''
      rpc = _build_rpc("vm.start", NULL);
      x = _build_rpc_name("device.nic.attach", vm_id);
      dotless = _build_rpc("get_vnc_info", p);   /* dotless 등록 메서드도 포함해야 함 */
    '''
    got = extract_rest_methods(src)
    assert "vm.start" in got and "device.nic.attach" in got
    assert "get_vnc_info" in got   # REST _build_rpc 인자는 항상 메서드 — dotless 포함(거짓 고아 방지)


def test_fe_helper_extraction():
    src = "await rpc('security.action.approve', {x:1}); EP.RPC(\"vm.metrics\"); await _rpc('healing.pending', {});"
    got = extract_fe_helper(src)
    assert "security.action.approve" in got and "vm.metrics" in got
    assert "healing.pending" in got   # _rpc 헬퍼(selfhealing.js) 포함
    assert extract_fe_helper("x = sendrpc('should.not.match', {})") == set()   # 영숫자-선행 오검출 방지


def test_secreq_cli_wrapper_extraction():
    from rpc_extract import SECREQ_RE
    assert SECREQ_RE.findall('security_request("security.baseline.status", NULL);') == ["security.baseline.status"]


def test_grpc_only_registered():
    src = 'call("vm.start"); call("not.registered.method");'
    registered = {"vm.start"}
    got = extract_grpc_methods(src, registered)
    assert got == {"vm.start"}   # 등록된 것만


if __name__ == "__main__":
    test_rest_build_rpc_extraction()
    test_fe_helper_extraction()
    test_secreq_cli_wrapper_extraction()
    test_grpc_only_registered()
    print("OK")
