#!/usr/bin/env python3
                          
                                                                            
                                                                                 
                                                                       
                                                                   
                                                   
 
                      
                                                                                          
                                                       

                                      
                                        
                                       
                                                           
                                                           
                                                  
                                                                         
                                                                 
                                                       
                                                 
                                             
   
import os
import sys
import tempfile
import subprocess
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from check_container_owner_scope import (              
    ROOT, DISPATCHER_REL, HANDLER_REL, OWNER_REL,
    check_dispatcher, check_stamp, check_substrate,
)

GATE = Path(__file__).resolve().parent.parent / "check_container_owner_scope.py"

DISP = ROOT / DISPATCHER_REL
HANDLER = ROOT / HANDLER_REL
OWNER = ROOT / OWNER_REL


def _run_trio(disp_text: str, handler_text: str, owner_text: str) -> int:
                                                                     
    paths = []
    try:
        for txt, suffix in ((disp_text, "disp.c"),
                            (handler_text, "handler.c"),
                            (owner_text, "owner.c")):
            fd, p = tempfile.mkstemp(suffix=suffix)
            with os.fdopen(fd, "w") as f:
                f.write(txt)
            paths.append(p)
        r = subprocess.run([sys.executable, str(GATE), *paths],
                           capture_output=True, text=True)
        return r.returncode
    finally:
        for p in paths:
            os.unlink(p)


def _reals():
    return DISP.read_text(), HANDLER.read_text(), OWNER.read_text()


                                                                
def test_current_tree_pass():
    r = subprocess.run([sys.executable, str(GATE)], capture_output=True, text=True)
    assert r.returncode == 0, f"{r.stdout}\n{r.stderr}"


def test_current_invariants_recognized():
    disp_ok, disp_r = check_dispatcher(DISPATCHER_REL, DISP.read_text())
    stamp_ok, stamp_r = check_stamp(HANDLER_REL, HANDLER.read_text())
    sub_ok, sub_r = check_substrate(OWNER_REL, OWNER.read_text())
    assert disp_ok, f"강제 불변식 미인식: {disp_r}"
    assert stamp_ok, f"스탬프 불변식 미인식: {stamp_r}"
    assert sub_ok, f"저장소 불변식 미인식: {sub_r}"


                                                            
def test_real_trio_pass():
    d, h, o = _reals()
    assert _run_trio(d, h, o) == 0


                                                                 
def test_red_set_missing_start():
    d, h, o = _reals()
    needle = 'g_strcmp0(method, "container.start") == 0 ||'
    assert needle in d, "정본에서 owner-scope 세트의 container.start 미발견 — 소스 변경?"
    assert _run_trio(d.replace(needle, "FALSE ||"), h, o) == 1


def test_red_set_missing_clone():
    d, h, o = _reals()
    needle = 'g_strcmp0(method, "container.clone") == 0;'
    assert needle in d, "정본에서 owner-scope 세트의 container.clone 미발견 — 소스 변경?"
    assert _run_trio(d.replace(needle, "FALSE;"), h, o) == 1


def test_red_wiring_removed():
    d, h, o = _reals()
    needle = "_container_owner_scoped_method_allowed(method, params, connection"
    assert needle in d, "정본에서 operator 분기 게이트 호출 미발견 — 소스 변경?"
                                           
    assert _run_trio(d.replace(needle, "_bypass_owner_gate(method, params, connection"), h, o) == 1


def test_red_gate_fn_removed():
    d, h, o = _reals()
    assert "_container_owner_matches_caller" in d, "정본에서 게이트 함수 미발견 — 소스 변경?"
    assert _run_trio(d.replace("_container_owner_matches_caller", "_x_matches"), h, o) == 1


                                                              
def test_red_stamp_removed():
    d, h, o = _reals()
    assert "pcv_lxc_stamp_owner" in h, "정본 handler에서 스탬프 호출 미발견 — 소스 변경?"
    assert _run_trio(d, h.replace("pcv_lxc_stamp_owner", "no_stamp_owner"), o) == 1


                                                              
def test_red_substrate_read_fn_removed():
    d, h, o = _reals()
    assert "pcv_lxc_read_owner" in o, "정본 lxc_owner에서 read_owner 미발견 — 소스 변경?"
    assert _run_trio(d, h, o.replace("pcv_lxc_read_owner", "pcv_lxc_read_xowner")) == 1


def test_red_owner_file_literal_removed():
    d, h, o = _reals()
    assert "purecvisor.owner" in o, "정본 lxc_owner에서 purecvisor.owner 미발견 — 소스 변경?"
    assert _run_trio(d, h, o.replace("purecvisor.owner", "purecvisor.notowner")) == 1


                                                              
def test_unrelated_edit_still_pass():
    d, h, o = _reals()
    assert _run_trio(d + "\n/* unrelated comment */\n", h, o) == 0


if __name__ == "__main__":
    tests = [v for k, v in sorted(globals().items())
             if k.startswith("test_") and callable(v)]
    failed = 0
    for t in tests:
        try:
            t()
            print(f"OK   {t.__name__}")
        except AssertionError as e:
            failed += 1
            print(f"FAIL {t.__name__}: {e}")
    print(f"[test_container_owner_scope] {len(tests) - failed}/{len(tests)} passed")
    sys.exit(1 if failed else 0)
