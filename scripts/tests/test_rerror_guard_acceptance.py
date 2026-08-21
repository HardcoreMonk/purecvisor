#!/usr/bin/env python3
                          
                                                                              
                                                                                 
                                                                       
                                                                   
                                                   
 
                      
                                                                                            
                                          

                                            

                                                                      
                                                         
                                            
                                                                    
                               

                                                           
                                        

                                                     
                                                           
                              

                                                           
                                                                             
   
import contextlib
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
GATE = ROOT / "scripts" / "check_rerror_guard.py"
BASELINE_FILE = ROOT / "scripts" / "rerror_guard_baseline.txt"

                                                    
                                              
PROBE = ROOT / "ui" / "modules" / "__rerror_guard_gate_probe__.js"
PROBE_REL = PROBE.relative_to(ROOT).as_posix()
                                       
PROBE_EP = "GATE_ACCEPTANCE_PROBE"
                                    
PROBE_GET_KEY = "%s|GET|EP.%s#1" % (PROBE_REL, PROBE_EP)

_HEAD = ("// 임시 게이트 인수 테스트 프로브 — scripts/tests/test_rerror_guard_acceptance.py\n"
         "// 가 만들고 지운다. 이 파일이 트리에 남아 있으면 원복이 실패한 것이다.\n")


def probe_src(body):
    return _HEAD + "(function () {\n  async function probe() {\n%s  }\n" \
                   "  window.__gateProbe = probe;\n})();\n" % body


                         
GUARDED = probe_src(
    "    var r = await fetchGet(EP.%s());\n"
    "    if (r.error) { return null; }\n"
    "    return r.items;\n" % PROBE_EP)
                                    
UNGUARDED_GET = probe_src(
    "    var r = await fetchGet(EP.%s());\n"
    "    return r.items;\n" % PROBE_EP)
                                          
UNGUARDED_POST = probe_src(
    "    await fetchPost(EP.%s(), { k: 1 });\n" % PROBE_EP)


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


def test_guarded_probe_stays_green():
                                           

                                                          
    with probe_source(GUARDED):
        r = run_gate()
    assert r.returncode == 0, (
        f"`if (r.error)` 가드가 있는 프로브가 FAIL 했다:\n{r.stdout}\n{r.stderr}")


def test_unguarded_call_in_real_scan_path_is_blocked():
                                      

                                                             
                                                        
                   
    with probe_source(UNGUARDED_POST):
        r = run_gate()
    assert r.returncode == 1, (
        f"스캔 경로에 놓은 미검사 호출부를 게이트가 못 막음:\n{r.stdout}\n{r.stderr}")
    assert "기준선 밖" in r.stdout and "미검사 호출부" in r.stdout, (
        f"미검사 호출부 FAIL 절이 아니다:\n{r.stdout}")
    assert PROBE_REL in r.stdout, (
        f"게이트가 실제 스캔 경로의 프로브 파일을 보지 못했다:\n{r.stdout}")
    assert "[Tier A]" in r.stdout, (
        f"상태 변경 호출을 Tier A 로 보고하지 않았다:\n{r.stdout}")


def test_baseline_entry_suppresses_and_its_removal_fails():
                                                 

                                                
                                                  
                                                  
    orig = BASELINE_FILE.read_text(encoding="utf-8")
    with probe_source(UNGUARDED_GET):
        with patched_text(BASELINE_FILE, orig.rstrip("\n") + "\n" + PROBE_GET_KEY + "\n"):
            suppressed = run_gate()
        remained = run_gate()
    assert suppressed.returncode == 0, (
        f"기준선에 등재한 호출부가 여전히 FAIL 한다(래칫 미적용):"
        f"\n{suppressed.stdout}\n{suppressed.stderr}")
    assert remained.returncode == 1, (
        f"기준선 항목을 지웠는데도 PASS 한다(래칫이 판정에 관여하지 않는다):"
        f"\n{remained.stdout}\n{remained.stderr}")
    assert PROBE_GET_KEY in remained.stdout, (
        f"기준선에서 뺀 항목이 신규 위반으로 보고되지 않았다:\n{remained.stdout}")


if __name__ == "__main__":
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for t in tests:
        t()
        print(f"  ok  {t.__name__}")
    print("OK")
