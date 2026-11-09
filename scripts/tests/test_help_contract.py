#!/usr/bin/env python3
                          
                                                                            
                                                                    
                                                                                          
                                              
 
                      
                                                                  
                                                                

                                                                                     
                                                                            

                                                                                                               
   
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
GATE = ROOT / "scripts" / "check_help_contract.py"
HELP_JS = ROOT / "ui" / "modules" / "help.js"
POLICY = ROOT / "src" / "api" / "dispatcher_policy.c"
GUIDES = (ROOT / "docs" / "GUIDE.md", ROOT / "ui" / "guide-content.md")

sys.path.insert(0, str(ROOT / "scripts"))


def run_gate(*args):
    return subprocess.run([sys.executable, str(GATE), *args], capture_output=True, text=True)


def run_mutated_help(old, new, count=1):
                                                                          
    text = HELP_JS.read_text(encoding="utf-8")
    assert old in text, f"변이 표본이 help.js 에 없다(시험 갱신 필요): {old}"
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "help.js"
        path.write_text(text.replace(old, new, count) if count else text.replace(old, new), encoding="utf-8")
        return run_gate("--help-js", str(path))


def assert_fails(result, *needles):
    assert result.returncode == 1, f"게이트가 변이를 막지 못했다:\n{result.stdout}\n{result.stderr}"
    for needle in needles:
        assert needle in result.stdout, f"'{needle}' 가 출력에 없다:\n{result.stdout}"


def test_gate_passes_on_tree():
                                         
    r = run_gate()
    assert r.returncode == 0, f"게이트 미착지:\n{r.stdout}\n{r.stderr}"
    r = run_gate("--self-test")
    assert r.returncode == 0, f"자기시험 실패:\n{r.stdout}\n{r.stderr}"


def test_role_written_too_low_fails():
                                                                     
    assert_fails(run_mutated_help("['audit.search', 2,", "['audit.search', 0,"),
                 "역할 표가 정책표와 다르다", "audit.search: help=0 정책표=2")


def test_role_written_too_high_fails():
                                                   
    assert_fails(run_mutated_help("['vm.list', 0,", "['vm.list', 2,"), "vm.list: help=2 정책표=0")


def test_policy_row_removed_means_admin():
                                                                            
    text = POLICY.read_text(encoding="utf-8")
    row = re.search(r'\{\s*"vm\.list"\s*,\s*[^}]+\},?\n', text)
    assert row, "정책표에 vm.list 행이 없다(시험 갱신 필요)"
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "policy.c"
        path.write_text(text.replace(row.group(0), "", 1), encoding="utf-8")
        r = run_gate("--policy", str(path))
    assert_fails(r, "vm.list: help=0 정책표=2", "표에 없어 ADMIN")


def test_wrong_path_fails():
                                                              
    assert_fails(run_mutated_help("p: '/vms/import/import'", "p: '/vms/import'"),
                 "rest 계열에 없는 경로", "POST /vms/import")


def test_unread_body_key_fails():
                                                        
    r = run_mutated_help('"disks":["/dev/pcv-example-disk1"', '"devices":["/dev/pcv-example-disk1"')
    assert_fails(r, "읽지 않는 키 ['devices']", "필수 키가 빠졌다 ['disks']")


def test_unread_key_a11_table_each_fails():
                                                            
    cases = [
        ('"target_node":"pcv-example-node"', '"target":"pcv-example-node"', "['target']"),                  
        ('"image":"ubuntu:22.04"', '"template":"debian-12"', "['template']"),                               
        ('{"new_size_gb":50,"target":"vda"}', '{"size_gb":50}', "['size_gb']"),                                
        ('{"command":"uptime"}', '{"cmd":"uptime"}', "['cmd']"),                                             
        ('"clone_name":"pcv-example-clone"', '"dest":"pcv-example-clone"', "['dest']"),                      
        ('{"inbound_kbps":1000,"outbound_kbps":1000}\' },\n      { m: \'PUT\', p: \'/containers/{name}/health\'',
         '{"inbound":1000,"outbound":1000}\' },\n      { m: \'PUT\', p: \'/containers/{name}/health\'', "['inbound', 'outbound']"),      
    ]
    for old, new, needle in cases:
        assert_fails(run_mutated_help(old, new), needle)


def test_renamed_rest_key_is_read_through_rest_branch():
                                                                                 
    assert_fails(run_mutated_help('{"command":"ls"}', '{"cmd":"ls"}'), "REST 분기가 읽지 않는 키 ['cmd']")


def test_get_with_body_fails():
                                                                
    assert_fails(run_mutated_help("{ m: 'GET', p: '/vms', d: _L('VM 목록', 'VM list') }",
                                  "{ m: 'GET', p: '/vms', d: _L('VM 목록', 'VM list'), body: '{\"limit\":1}' }"),
                 "GET 예시에 본문이 있다")


def test_query_key_is_checked():
                                                      
    assert_fails(run_mutated_help("/audit/search?username=pcv-example-user", "/audit/search?user=pcv-example-user"),
                 "읽지 않는 키 ['user']")


def test_realistic_name_fails():
                                                                     
    assert_fails(run_mutated_help('{"name":"pcv-example-pool"}', '{"name":"tank"}'), "name=tank")
    assert_fails(run_mutated_help('{"username":"pcv-example-user"}', '{"username":"bob"}'), "username=bob")


def test_invalid_json_body_fails():
    assert_fails(run_mutated_help('{"command":"uptime"}', '{command:uptime}'), "본문이 JSON 이 아니다")


def test_unregistered_rpc_helper_fails():
                                     
    assert_fails(run_mutated_help("rpc('qos.stats',", "rpc('qos.statz',"), "등록되지 않은 RPC")


def test_vacuous_parse_fails():
                                                                 
    text = HELP_JS.read_text(encoding="utf-8")
    stripped = re.sub(r",\s*body:\s*'(?:[^'\\]|\\.)*'", "", text)
    assert stripped != text
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "help.js"
        path.write_text(stripped, encoding="utf-8")
        r = run_gate("--help-js", str(path))
    assert_fails(r, "하한 미만")


def test_missing_policy_block_fails():
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "policy.c"
        path.write_text("int x;\n", encoding="utf-8")
        r = run_gate("--policy", str(path))
    assert_fails(r, "g_method_policies 를 찾지 못했다")


                                                                              
                                                          
RPC_BLOCK = re.compile(r"""echo '(\{"jsonrpc":"2\.0","method":"([a-z_.]+)","params":\{.*?\},"id":"1"\})'""", re.S)
GUIDE_METHODS = ("container.health.set", "container.nic.detach", "storage.pool.create",
                 "security.baseline.refresh", "security.config.set")


def guide_examples(path):
    out = {}
    for m in RPC_BLOCK.finditer(path.read_text(encoding="utf-8")):
        if m.group(2) in GUIDE_METHODS:
            out.setdefault(m.group(2), []).append(json.loads(m.group(1))["params"])
    return out


def test_guide_rpc_examples_use_handler_keys():
                                                                     
    import check_fe_rpc_params as fe
    for path in GUIDES:
        found = guide_examples(path)
        assert set(found) == set(GUIDE_METHODS), f"{path.name}: 예시 누락 {set(GUIDE_METHODS) - set(found)}"
        for method, examples in found.items():
            read = fe.be_read_keys(method)
            assert read is not None, f"{method}: 핸들러 키를 읽지 못했다"
            required = fe.be_required_keys(method) or set()
            for params in examples:
                extra = set(params) - read
                assert not extra, f"{path.name} {method}: 핸들러가 읽지 않는 키 {sorted(extra)}"
                lacking = required - set(params)
                assert not lacking, f"{path.name} {method}: 필수 키 누락 {sorted(lacking)}"


def test_guide_copies_agree_on_examples():
                                                                                       
    first, second = (guide_examples(p) for p in GUIDES)
    assert first == second, f"GUIDE.md 와 guide-content.md 의 예시가 다르다:\n{first}\n{second}"


if __name__ == "__main__":
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for t in tests:
        t()
        print(f"  ok  {t.__name__}")
    print("OK")
