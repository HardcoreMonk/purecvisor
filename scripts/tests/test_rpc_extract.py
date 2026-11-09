                          
                                                                     
                                                                                 
                                                                       
                                                                   
                                                   
 
                      
                                                                                   
import contextlib
import io
import re
import sys
import tempfile
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parent.parent
ROOT = SCRIPTS.parent
sys.path.insert(0, str(SCRIPTS))
import extract_rest_routes as routes_mod
import rpc_extract
from rpc_extract import (REST_RE, FE_HELP_RE, extract_rest_methods,
                          extract_fe_helper, extract_grpc_methods, strip_comments)
from extract_rest_routes import extract as extract_rest_routes


def test_rest_build_rpc_extraction():
    src = '''
      rpc = pcv_rest_build_rpc("vm.start", NULL);
      x = pcv_rest_build_rpc_name("device.nic.attach", vm_id);
      dotless = pcv_rest_build_rpc("get_vnc_info", p);   /* dotless 등록 메서드도 포함해야 함 */
      rq->rpc = pcv_rest_build_rpc("vm.list", NULL);     /*  2단계 구간 함수 모양 */
      decoy = my_build_rpc("decoy.suffix_only", NULL);
    '''
    got = extract_rest_methods(src)
    assert "vm.start" in got and "device.nic.attach" in got and "vm.list" in got
    assert "get_vnc_info" in got                                                      

    assert "decoy.suffix_only" not in got


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


def test_host_network_baseline_route_precedes_dynamic_bridge_routes():

    routes = {(method, path, rpc) for method, path, rpc, _line in extract_rest_routes()}
    assert ("GET", "/networks/host-baseline", "network.host.info") in routes
    assert ("GET", "/networks/{name}", "network.info") in routes
    assert ("POST", "/networks/{name}/mode", "network.mode_set") in routes
    assert not any(path.startswith("/networks/host-baseline/") for _, path, _ in routes)


def _assert_ovn_get_query_mapping(source: str, resource: str, key: str, method: str) -> None:

    start = source.index(f'g_strcmp0(name, "{resource}") == 0')
    end = source.find('} else if (g_strcmp0(name,', start + 1)
    section = source[start:end if end >= 0 else len(source)]
    lookup = section.index(f'g_hash_table_lookup(query, "{key}")')
    set_member = section.index(f'json_object_set_string_member(p, "{key}"')
    build = re.search(rf'\bpcv_rest_build_rpc\(\s*"{re.escape(method)}"\s*,\s*p\s*\)', section)
    assert build is not None, f"pcv_rest_build_rpc(\"{method}\", p) 없음"
    assert lookup < set_member < build.start()


def test_ovn_rest_get_queries_reach_canonical_rpc_params():

    source = rpc_extract.rest_family_text()
    _assert_ovn_get_query_mapping(source, "acl", "switch", "ovn.acl.list")
    _assert_ovn_get_query_mapping(source, "nat", "router", "ovn.nat.list")


def test_ovn_rest_query_mapping_counterfactual_rejects_wrong_key():

    source = '''
      if (g_strcmp0(name, "acl") == 0) {
        const gchar *wrong = g_hash_table_lookup(query, "wrong");
        json_object_set_string_member(p, "wrong", wrong);
        rq->rpc = pcv_rest_build_rpc("ovn.acl.list", p);
      } else if (g_strcmp0(name, "next") == 0) {}
    '''
    try:
        _assert_ovn_get_query_mapping(source, "acl", "switch", "ovn.acl.list")
    except (AssertionError, ValueError):
        return
    raise AssertionError("wrong OVN query key must fail the mapping contract")


_SKELETON = (
    "static void\n_on_request(SoupServer *server)\n{\n"
    "    /*\n     * REST → RPC 라우팅 테이블 시작\n     */\n"
    "    st = pcv_rest_map_x(rq);\n"
    "cleanup:\n"
    '    rq->rpc = pcv_rest_build_rpc("after.cleanup", NULL);   /* cleanup: 뒤는 rest_server.c에서 읽지 않는다 */\n'
    "}\n")
_CHAIN = (
    "PcvRestStep\npcv_rest_map_x(PcvRestRequest *rq)\n{\n"
    '    if (g_strcmp0(resource, "vms") == 0) {\n'
    '        rq->rpc = pcv_rest_build_rpc("vm.list", NULL);\n'
    "    }\n"
    "    return PCV_REST_STEP_NEXT;\n}\n")

def _routes_root(files: dict):
    tmp = tempfile.TemporaryDirectory()
    root = Path(tmp.name)
    for rel, text in files.items():
        (root / rel).parent.mkdir(parents=True, exist_ok=True)
        (root / rel).write_text(text)
    return tmp, root

def test_route_extractor_reads_chain_file_per_file():

    tmp, root = _routes_root({"src/api/rest_server.c": _SKELETON, "src/api/rest_map_x.c": _CHAIN})
    with tmp:
        got = routes_mod.extract(root, chain_files=("src/api/rest_map_x.c",))
    assert got == [("GET", "/vms", "vm.list", "rest_map_x.c:L5")], got

def test_route_extractor_skips_missing_chain_file_and_fails_floor():

    tmp, root = _routes_root({"src/api/rest_server.c": _SKELETON})
    with tmp:
        assert routes_mod.extract(root, chain_files=("src/api/rest_map_x.c",)) == []
        saved = routes_mod.ROOT
        routes_mod.ROOT = root
        err = io.StringIO()
        try:
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(err):
                try:
                    routes_mod.main()
                except SystemExit as e:
                    code = e.code
                else:
                    code = 0
        finally:
            routes_mod.ROOT = saved
    assert code == 1 and f"하한 {routes_mod.MIN_ROUTES}" in err.getvalue(), err.getvalue()

def test_route_extractor_uses_config_chain_files_in_source_order():

    files = [p.relative_to(ROOT).as_posix() for p, _ in routes_mod.scan_files(ROOT, chain_files=None)]
    assert files[0] == "src/api/rest_server.c"
    import split_target
    assert split_target.load("rest_server").chain_files == (
        "src/api/rest_map_compute.c", "src/api/rest_map_observe.c", "src/api/rest_map_platform.c")
    assert files[1:] == [f for f in split_target.load("rest_server").chain_files if (ROOT / f).exists()]

def test_rest_request_spans_fail_closed_on_missing_block_definition():

    with_def = "static void blk(Rq *rq)\n{\n    x();\n}\nstatic void _on_request(Rq *rq)\n{\n    blk(rq);\n}\n"
    assert rpc_extract.rest_request_spans(with_def, ("blk",), "rq") is not None
    without_def = with_def.replace("static void blk(Rq *rq)", "static void other(Rq *rq)")
    assert rpc_extract.rest_request_spans(without_def, ("blk",), "rq") is None
    assert rpc_extract.rest_request_scope(without_def, ("blk",), "rq") is None

if __name__ == "__main__":
    test_rest_build_rpc_extraction()
    test_rest_request_spans_fail_closed_on_missing_block_definition()
    test_fe_helper_extraction()
    test_secreq_cli_wrapper_extraction()
    test_grpc_only_registered()
    test_suricata_literal_name_and_action_routes_are_preserved()
    test_local_vpc_dedicated_rest_routes_are_preserved()
    test_host_network_baseline_route_precedes_dynamic_bridge_routes()
    test_ovn_rest_get_queries_reach_canonical_rpc_params()
    test_ovn_rest_query_mapping_counterfactual_rejects_wrong_key()
    test_route_extractor_reads_chain_file_per_file()
    test_route_extractor_skips_missing_chain_file_and_fails_floor()
    test_route_extractor_uses_config_chain_files_in_source_order()
    print("OK")
