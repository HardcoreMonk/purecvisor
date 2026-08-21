                          
                                                                     
                                                                                 
                                                                       
                                                                   
                                                   
 
                      
                                                                                   
import sys
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(SCRIPTS))
from rpc_extract import (REST_RE, FE_HELP_RE, extract_rest_methods,
                          extract_fe_helper, extract_grpc_methods, strip_comments)
from extract_rest_routes import extract as extract_rest_routes


def test_rest_build_rpc_extraction():
    src = '''
      rpc = _build_rpc("vm.start", NULL);
      x = _build_rpc_name("device.nic.attach", vm_id);
      dotless = _build_rpc("get_vnc_info", p);   /* dotless 등록 메서드도 포함해야 함 */
    '''
    got = extract_rest_methods(src)
    assert "vm.start" in got and "device.nic.attach" in got
    assert "get_vnc_info" in got                                                      


def test_fe_helper_extraction():
    src = "await rpc('security.action.approve', {x:1}); EP.RPC(\"vm.metrics\"); await _rpc('healing.pending', {});"
    got = extract_fe_helper(src)
    assert "security.action.approve" in got and "vm.metrics" in got
    assert "healing.pending" in got                               
    assert extract_fe_helper("x = sendrpc('should.not.match', {})") == set()                  


def test_secreq_cli_wrapper_extraction():
    from rpc_extract import SECREQ_RE
    assert SECREQ_RE.findall('security_request("security.baseline.status", NULL);') == ["security.baseline.status"]


def test_grpc_only_registered():
    src = 'call("vm.start"); call("not.registered.method");'
    registered = {"vm.start"}
    got = extract_grpc_methods(src, registered)
    assert got == {"vm.start"}           


def test_suricata_literal_name_and_action_routes_are_preserved():
                                                               
    routes = {(method, path, rpc) for method, path, rpc, _line in extract_rest_routes()}
    expected = {
        ("GET", "/suricata/status", "suricata.status"),
        ("GET", "/suricata/policy", "suricata.policy.get"),
        ("PUT", "/suricata/policy", "suricata.policy.set"),
        ("POST", "/suricata/rules/update", "suricata.rules.update"),
        ("GET", "/suricata/ips/status", "suricata.ips.status"),
        ("POST", "/suricata/ips/enable", "suricata.ips.enable"),
        ("POST", "/suricata/ips/disable", "suricata.ips.disable"),
        ("GET", "/suricata/ips/drop", "suricata.ips.drop.list"),
        ("POST", "/suricata/ips/drop", "suricata.ips.drop.add"),
        ("DELETE", "/suricata/ips/drop", "suricata.ips.drop.remove"),
    }
    assert expected <= routes


def test_local_vpc_dedicated_rest_routes_are_preserved():
                                                                     
    routes = {(method, path, rpc) for method, path, rpc, _line in extract_rest_routes()}
    expected = {
        ("GET", "/vpcs", "vpc.list"),
        ("POST", "/vpcs", "vpc.create"),
        ("GET", "/vpcs/status", "vpc.status"),
        ("POST", "/vpcs/reconcile", "vpc.reconcile"),
        ("GET", "/vpcs/{name}", "vpc.get"),
        ("DELETE", "/vpcs/{name}", "vpc.delete"),
        ("POST", "/vpcs/{name}/egress", "vpc.egress.set"),
        ("GET", "/vpcs/{name}/subnets", "vpc.subnet.list"),
        ("POST", "/vpcs/{name}/subnets", "vpc.subnet.create"),
        ("DELETE", "/vpc-subnets/{name}", "vpc.subnet.delete"),
        ("GET", "/vpcs/{name}/attachments", "vpc.attachment.list"),
        ("POST", "/vpc-attachments", "vpc.attachment.create"),
        ("DELETE", "/vpc-attachments/{name}", "vpc.attachment.delete"),
        ("GET", "/vpcs/{name}/services", "vpc.service.list"),
        ("POST", "/vpc-services", "vpc.service.publish"),
        ("DELETE", "/vpc-services/{name}", "vpc.service.unpublish"),
    }
    assert expected <= routes


if __name__ == "__main__":
    test_rest_build_rpc_extraction()
    test_fe_helper_extraction()
    test_secreq_cli_wrapper_extraction()
    test_grpc_only_registered()
    test_suricata_literal_name_and_action_routes_are_preserved()
    test_local_vpc_dedicated_rest_routes_are_preserved()
    print("OK")
