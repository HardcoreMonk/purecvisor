#!/usr/bin/env python3
                          
                                                                      
                                                                                 
                                                                       
                                                                   
                                                   
 
                      
                                                                                    
                                           

                                            

                                                                      
                                                    
                                                                            
                                                                        
                                           

                                                           
                                                         
                       

                                                      
                                                    
                                                        

                                                            
                                                                              
   
import contextlib
import io
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
GATE = ROOT / "scripts" / "check_fe_rpc_params.py"
VIOLATION_FILE = ROOT / "scripts" / "fe_rpc_params_baseline.txt"

                                                   
                                               
PROBE = ROOT / "ui" / "modules" / "__fe_rpc_params_gate_probe__.js"
PROBE_REL = PROBE.relative_to(ROOT).as_posix()

                                                         
                                                  
COVERED_METHOD = "vm.list"
COVERED_KEY = "limit"
BOGUS_KEY = "__gate_probe_never_read__"
                                                             
REQUIRED_KEY_METHOD = "auth.user.delete"
REQUIRED_KEY = "username"

_HEAD = ("// 임시 게이트 인수 테스트 프로브 — scripts/tests/test_fe_rpc_params_acceptance.py\n"
         "// 가 만들고 지운다. 이 파일이 트리에 남아 있으면 원복이 실패한 것이다.\n")


def rpc_probe(method, params_body):
                                      
    return (_HEAD + "(function () {\n"
            "  async function probe(n) {\n"
            "    var r = await fetchPost(EP.RPC(), { jsonrpc: '2.0', method: '%s',"
            " params: {%s}, id: 'p' });\n"
            "    return r;\n"
            "  }\n"
            "  window.__gateProbe = probe;\n"
            "})();\n" % (method, params_body))


def run_gate():
    return subprocess.run([sys.executable, str(GATE)], capture_output=True, text=True)


@contextlib.contextmanager
def probe_source(content):
                                      

                                                     
    assert not PROBE.exists(), f"프로브 경로에 잔여 파일이 있다(수동 정리 필요): {PROBE}"
    PROBE.write_text(content, encoding="utf-8")
    try:
        yield
    finally:
        PROBE.unlink(missing_ok=True)


@contextlib.contextmanager
def patched_text(path, new_text):
                                                  
    orig = path.read_text(encoding="utf-8")
    path.write_text(new_text, encoding="utf-8")
    try:
        yield
    finally:
        path.write_text(orig, encoding="utf-8")


def test_gate_passes_at_baseline():
                                          
    r = run_gate()
    assert r.returncode == 0, f"게이트 미착지:\n{r.stdout}\n{r.stderr}"


def test_contract_conforming_probe_stays_green():
                                            

                                                          
    with probe_source(rpc_probe(COVERED_METHOD, " %s: 10 " % COVERED_KEY)):
        r = run_gate()
    assert r.returncode == 0, (
        f"백엔드가 읽는 키만 보내는 프로브가 FAIL 했다:\n{r.stdout}\n{r.stderr}")


def test_forward_violation_in_real_scan_path_is_blocked():
                                     

                                                         
                                                                
    with probe_source(rpc_probe(COVERED_METHOD, " %s: 1 " % BOGUS_KEY)):
        r = run_gate()
    assert r.returncode == 1, (
        f"스캔 경로에 놓은 정방향 위반을 게이트가 못 막음:\n{r.stdout}\n{r.stderr}")
    assert "UI 가 보내는데 백엔드가 읽지 않는" in r.stdout, (
        f"정방향 FAIL 절이 아니다:\n{r.stdout}")
    assert PROBE_REL in r.stdout, (
        f"게이트가 실제 스캔 경로의 프로브 파일을 보지 못했다:\n{r.stdout}")
    assert BOGUS_KEY in r.stdout, f"위반 키가 보고되지 않았다:\n{r.stdout}"


def test_reverse_violation_in_real_scan_path_is_blocked():
                                            
    with probe_source(rpc_probe(REQUIRED_KEY_METHOD, "")):
        r = run_gate()
    assert r.returncode == 1, (
        f"스캔 경로에 놓은 역방향 위반을 게이트가 못 막음:\n{r.stdout}\n{r.stderr}")
    assert "필수로 요구하는데 UI 가 보내지 않는" in r.stdout, (
        f"역방향 FAIL 절이 아니다:\n{r.stdout}")
    assert PROBE_REL in r.stdout and REQUIRED_KEY in r.stdout, (
        f"역방향 위반의 파일·키가 보고되지 않았다:\n{r.stdout}")


def var_probe(body_literal, ep="AUTH_USERS", verb="Post"):

    return (_HEAD + "(function () {\n"
            "  async function probe(n) {\n"
            "    const body = %s;\n"
            "    return await fetch%s(EP.%s(n), body);\n"
            "  }\n"
            "  window.__gateProbe = probe;\n"
            "})();\n" % (body_literal, verb, ep))

def test_variable_body_forward_violation_is_blocked():

    with probe_source(var_probe("{ username: n, password: n, %s: 1 }" % BOGUS_KEY)):
        r = run_gate()
    assert r.returncode == 1, f"변수 본문의 정방향 위반을 못 막음:\n{r.stdout}\n{r.stderr}"
    assert PROBE_REL in r.stdout and BOGUS_KEY in r.stdout, r.stdout

def test_variable_body_reverse_violation_is_blocked():

    with probe_source(var_probe("{ username: n }", ep="AUTH_ROLE")):
        r = run_gate()
    assert r.returncode == 1, f"변수 본문의 역방향 위반을 못 막음:\n{r.stdout}\n{r.stderr}"
    assert "필수로 요구하는데 UI 가 보내지 않는" in r.stdout and PROBE_REL in r.stdout, r.stdout

def test_variable_body_contract_conforming_stays_green():

    with probe_source(var_probe("{ username: n, role: 'viewer' }", ep="AUTH_ROLE")):
        r = run_gate()
    assert r.returncode == 0, f"계약을 지키는 변수 본문이 FAIL 했다:\n{r.stdout}\n{r.stderr}"

def test_unresolvable_variable_body_counts_as_uncovered():

    probe = (_HEAD + "(function () { async function probe(body) {"
             " return await fetchPost(EP.__GATE_PROBE_EP__(), body); } window.__gateProbe = probe; })();\n")
    with probe_source(probe):
        r = run_gate()
    assert r.returncode == 1 and "EP.__GATE_PROBE_EP__[POST]" in r.stdout, r.stdout

def dynamic_key_probe():

    return (_HEAD + "(function () {\n"
            "  async function probe(n, k) {\n"
            "    const body = { username: n };\n"
            "    body[k] = 1;\n"
            "    return await fetchPost(EP.AUTH_USERS(), body);\n"
            "  }\n"
            "  window.__gateProbe = probe;\n"
            "})();\n")

def test_incomplete_body_counts_as_uncovered():

    with probe_source(dynamic_key_probe()):
        r = run_gate()
    assert r.returncode == 1 and "EP.AUTH_USERS[POST]" in r.stdout, r.stdout

def test_incomplete_rpc_params_count_as_uncovered():

    with probe_source(rpc_probe(COVERED_METHOD, " ...extra ")):
        r = run_gate()
    assert r.returncode == 1 and COVERED_METHOD in r.stdout and "신규 EP" in r.stdout, r.stdout

def _rewrite(path, old, new):
    text = path.read_text(encoding="utf-8")
    assert old in text, f"{path} 에 {old!r} 가 없다 — 시험의 앵커를 현행 호출부에 맞출 것"
    return text.replace(old, new)

def test_bandwidth_body_key_rename_is_blocked():

    path = ROOT / "ui" / "modules" / "vm-guest.js"
    with patched_text(path, _rewrite(path, "inbound_kbps", "inbound_avg")):
        r = run_gate()
    assert r.returncode == 1 and "inbound_avg" in r.stdout and "vm.set_bandwidth" in r.stdout, r.stdout

def test_container_limits_body_key_rename_is_blocked():

    path = ROOT / "ui" / "modules" / "container.js"
    with patched_text(path, _rewrite(path, "cpu_weight", "cpu_shares")):
        r = run_gate()
    assert r.returncode == 1 and "cpu_shares" in r.stdout and "container.set_limits" in r.stdout, r.stdout

def test_container_create_form_is_covered():

    gate_text = GATE.read_text(encoding="utf-8")
    needle = '("CTR_LIST", "POST"): "container.create",'
    assert needle in gate_text, "EP_TO_RPC 에 CTR_LIST POST 가 없다"
    sys.path.insert(0, str(ROOT / "scripts"))
    import check_fe_rpc_params as gate
    read = gate.be_read_keys("container.create")
    assert read is not None and "image" in read and "static_ip" not in read, read

def test_baseline_entry_suppresses_and_its_removal_fails():
                                                 

                                                          
                                                          
                                                   
                      
    entry = "%s|%s|%s" % (PROBE_REL, COVERED_METHOD, BOGUS_KEY)
    orig = VIOLATION_FILE.read_text(encoding="utf-8")
    with probe_source(rpc_probe(COVERED_METHOD, " %s: 1 " % BOGUS_KEY)):
        with patched_text(VIOLATION_FILE, orig.rstrip("\n") + "\n" + entry + "\n"):
            suppressed = run_gate()
        remained = run_gate()
    assert suppressed.returncode == 0, (
        f"baseline 에 등재한 위반이 여전히 FAIL 한다(래칫 미적용):"
        f"\n{suppressed.stdout}\n{suppressed.stderr}")
    assert remained.returncode == 1, (
        f"baseline 항목을 지웠는데도 PASS 한다(래칫이 판정에 관여하지 않는다):"
        f"\n{remained.stdout}\n{remained.stderr}")


@contextlib.contextmanager
def _gate_on_root(files):

    sys.path.insert(0, str(ROOT / "scripts"))
    import check_fe_rpc_params as gate
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        for rel, text in files.items():
            (root / rel).parent.mkdir(parents=True, exist_ok=True)
            (root / rel).write_text(text, encoding="utf-8")
        saved = gate.ROOT
        gate.ROOT, gate._rest_injected = root, None
        try:
            yield gate
        finally:
            gate.ROOT, gate._rest_injected = saved, None

def test_rest_injected_keys_read_split_files():

    split = 'void f(JsonObject *params) {\n    json_object_set_string_member(params, "name", n);\n}\n'
    with _gate_on_root({"src/api/rest_server.c": "int x;\n", "src/api/rest_request.c": split}) as gate:
        assert gate.rest_injected_keys() == frozenset({"name"}), gate.rest_injected_keys()

def test_rest_injected_keys_absent_fails_self_test():

    with _gate_on_root({"src/api/rest_server.c": "int x;\n"}) as gate:
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = gate.self_test()
    assert rc == 1 and "REST 주입 키 표본" in out.getvalue(), out.getvalue()

if __name__ == "__main__":
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for t in tests:
        t()
        print(f"  ok  {t.__name__}")
    print("OK")
