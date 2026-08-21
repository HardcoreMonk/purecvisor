#!/usr/bin/env python3
                          
                                                                      
                                                                                 
                                                                       
                                                                   
                                                   
 
                      
                                                                                    
                                           

                                            

                                                                      
                                                    
                                                                            
                                                                        
                                           

                                                           
                                                         
                       

                                                      
                                                    
                                                        

                                                            
                                                                              
   
import contextlib
import subprocess
import sys
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


if __name__ == "__main__":
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for t in tests:
        t()
        print(f"  ok  {t.__name__}")
    print("OK")
