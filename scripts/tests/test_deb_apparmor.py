#!/usr/bin/env python3
                          
                                                                
                                                                                 
                                                                       
                                                                   
                                                   
 
                      
                                                                              
                                                                

                           
                                                  
                                                                      
                                                                   
                                                       
                                                           
                                                            
                                                 
                                                                
                                       
                                                         
                                                        
                                                                
                                                             
   
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from check_deb_apparmor import (              
    check_postinst, check_postrm, check_helper, extract_generated,
    apparmor_action, purge_block,
    ROOT, BUILD_REL, HELPER_REL, APROF, DISLINK, FCLINK,
)

GATE = Path(__file__).resolve().parent.parent / "check_deb_apparmor.py"
BUILD = ROOT / BUILD_REL
HELPER = ROOT / HELPER_REL


                                                         
POSTINST_OK = '''#!/bin/sh
set -e
case "$1" in
  configure)
    APROF=/etc/apparmor.d/usr.local.bin.purecvisorsd
    DISDIR=/etc/apparmor.d/disable
    DISLINK="$DISDIR/usr.local.bin.purecvisorsd"
    FCLINK=/etc/apparmor.d/force-complain/usr.local.bin.purecvisorsd
    mkdir -p "$DISDIR"
    ln -sfn ../usr.local.bin.purecvisorsd "$DISLINK"
    rm -f "$FCLINK" 2>/dev/null || true
    apparmor_parser -R "$APROF" >/dev/null 2>&1 || true
    ;;
esac
exit 0
'''

POSTRM_OK = '''#!/bin/sh
set -e
case "$1" in
  remove|purge)
    systemctl daemon-reload || true
    ;;
esac
if [ "$1" = "purge" ]; then
    rm -f /etc/apparmor.d/disable/usr.local.bin.purecvisorsd 2>/dev/null || true
    rm -f /etc/apparmor.d/force-complain/usr.local.bin.purecvisorsd 2>/dev/null || true
    apparmor_parser -R /etc/apparmor.d/usr.local.bin.purecvisorsd >/dev/null 2>&1 || true
fi
exit 0
'''

HELPER_OK = '''#!/bin/sh
set -e
APROF=/etc/apparmor.d/usr.local.bin.purecvisorsd
FCDIR=/etc/apparmor.d/force-complain
FCLINK="$FCDIR/usr.local.bin.purecvisorsd"
DISDIR=/etc/apparmor.d/disable
DISLINK="$DISDIR/usr.local.bin.purecvisorsd"
adr0028_warn() {
  echo "pcv-apparmor: [경고] ADR-0028 — 기본은 미부착" >&2
}
case "${1:-status}" in
  enforce)
    adr0028_warn
    rm -f "$DISLINK"
    apparmor_parser -r "$APROF"
    ;;
  complain)
    adr0028_warn
    rm -f "$DISLINK"
    apparmor_parser -r -C "$APROF"
    ;;
esac
'''


def test_logic_postinst_ok():
    assert check_postinst(POSTINST_OK) == []


def test_logic_load_flagged():
                                               
    bad = check_postinst(POSTINST_OK.replace(
        '    mkdir -p "$DISDIR"',
        '    apparmor_parser -r -W -C "$APROF"\n    mkdir -p "$DISDIR"'))
    assert any("불변식1" in b for b in bad), bad


def test_logic_aa_complain_helper_call_flagged():
                                                                
    assert any("불변식1" in b for b in check_postinst(
        POSTINST_OK.replace('    mkdir -p "$DISDIR"', '    aa-complain purecvisorsd\n    mkdir -p "$DISDIR"')))
    assert any("불변식1" in b for b in check_postinst(
        POSTINST_OK.replace('    mkdir -p "$DISDIR"', '    pcv-apparmor complain\n    mkdir -p "$DISDIR"')))


def test_logic_apparmor_action_classification():
                                                           
    assert apparmor_action(["-R"]) == "remove"
    assert apparmor_action(["--remove"]) == "remove"
    assert apparmor_action(["-R", "-W"]) == "remove"
    for flags in ([], ["-C"], ["-a"], ["--add"], ["-r"], ["-r", "-W", "-C"],
                  ["--replace"], ["-R", "-r"], ["--version"], ["-Z"]):
        assert apparmor_action(flags) == "load", flags


def test_logic_all_attach_spellings_flagged():
                                                      

                                                         
       
    for call in ('apparmor_parser -a "$APROF"', 'apparmor_parser --add "$APROF"',
                 'apparmor_parser "$APROF"', 'apparmor_parser -C "$APROF"'):
        bad = check_postinst(POSTINST_OK.replace(
            '    mkdir -p "$DISDIR"', f'    {call}\n    mkdir -p "$DISDIR"'))
        assert any("불변식1" in b for b in bad), (call, bad)


def test_logic_disable_link_removed_flagged():
                                                
    bad = check_postinst(POSTINST_OK.replace(
        '    rm -f "$FCLINK" 2>/dev/null || true',
        '    rm -f "$FCLINK" 2>/dev/null || true\n    rm -f "$DISLINK"'))
    assert any("불변식2" in b and "제거한다" in b for b in bad), bad


def test_logic_composite_unlink_and_attach_flagged():
                                                         
    bad = check_postinst(POSTINST_OK.replace(
        '    mkdir -p "$DISDIR"',
        '    rm -f "$DISLINK"\n    apparmor_parser -C "$APROF"\n    mkdir -p "$DISDIR"'))
    assert any("불변식1" in b for b in bad) and any("불변식2" in b for b in bad), bad


def test_logic_missing_disable_link_flagged():
    bad = check_postinst(POSTINST_OK.replace(
        '    ln -sfn ../usr.local.bin.purecvisorsd "$DISLINK"\n', ''))
    assert any("불변식2" in b for b in bad), bad


def test_logic_missing_unload_flagged():
    bad = check_postinst(POSTINST_OK.replace(
        '    apparmor_parser -R "$APROF" >/dev/null 2>&1 || true\n', ''))
    assert any("불변식3" in b for b in bad), bad


def test_logic_force_complain_link_flagged():
    bad = check_postinst(POSTINST_OK.replace(
        '    rm -f "$FCLINK" 2>/dev/null || true',
        '    mkdir -p /etc/apparmor.d/force-complain\n'
        '    ln -sf ../usr.local.bin.purecvisorsd "$FCLINK"'))
    assert any("불변식4" in b for b in bad), bad


def test_logic_comment_is_not_code():
                                                       
    bad = check_postinst(POSTINST_OK.replace(
        '    mkdir -p "$DISDIR"',
        '    # 과거에는 apparmor_parser -r -W -C "$APROF" 를 했다\n    mkdir -p "$DISDIR"'))
    assert bad == [], bad


def test_logic_commented_out_disable_link_flagged():
                                                                
    bad = check_postinst(POSTINST_OK.replace(
        '    ln -sfn ../usr.local.bin.purecvisorsd "$DISLINK"',
        '    # ln -sf ../usr.local.bin.purecvisorsd "$DISLINK"'))
    assert any("불변식2" in b for b in bad), bad


def test_logic_postrm_ok_and_missing():
    assert check_postrm(POSTRM_OK) == []
    bad = check_postrm(POSTRM_OK.replace(
        '    rm -f /etc/apparmor.d/disable/usr.local.bin.purecvisorsd 2>/dev/null || true\n', ''))
    assert any("불변식5" in b for b in bad), bad


def test_logic_postrm_cleanup_outside_purge_flagged():
                                                                   
    moved = POSTRM_OK.replace(
        '    rm -f /etc/apparmor.d/disable/usr.local.bin.purecvisorsd 2>/dev/null || true\n', ''
    ).replace(
        '  remove|purge)\n    systemctl daemon-reload || true',
        '  remove|purge)\n    systemctl daemon-reload || true\n'
        '    rm -f /etc/apparmor.d/disable/usr.local.bin.purecvisorsd 2>/dev/null || true')
    assert "remove|purge" in moved
    bad = check_postrm(moved)
    assert any("불변식5" in b for b in bad), bad


def test_logic_purge_block_scoping():
                                                                
    block = purge_block(POSTRM_OK)
    assert "disable/usr.local.bin.purecvisorsd" in block
    assert "systemctl daemon-reload" not in block


def test_logic_helper_stdout_label_is_not_a_warning():
                                                        

                                                                      
                                                  
       
    decoy = HELPER_OK.replace(
        'adr0028_warn() {\n  echo "pcv-apparmor: [경고] ADR-0028 — 기본은 미부착" >&2\n}',
        'adr0028_warn() {\n  echo "ADR-0028 상태 라벨"\n}')
    assert any("불변식6" in b and "경고" in b for b in check_helper(decoy)), decoy


def test_logic_helper_ok_and_regressions():
    assert check_helper(HELPER_OK) == []
    no_warn = HELPER_OK.replace('  enforce)\n    adr0028_warn\n', '  enforce)\n')
    assert any("불변식6" in b and "경고" in b for b in check_helper(no_warn))
    no_rm = HELPER_OK.replace('  complain)\n    adr0028_warn\n    rm -f "$DISLINK"\n',
                              '  complain)\n    adr0028_warn\n')
    assert any("불변식6" in b and "disable" in b for b in check_helper(no_rm))


def test_missing_heredoc_flagged():
                                                         
    assert check_postinst("") and check_postrm("")


                                                         
def _run(build_text=None, helper_text=None):
                                                                  
    tmps = []
    try:
        argv = [sys.executable, str(GATE)]
        for flag, text, suffix in (("--build-script", build_text, ".sh"),
                                   ("--helper", helper_text, ".sh")):
            if text is None:
                continue
            with tempfile.NamedTemporaryFile("w", suffix=suffix, delete=False) as f:
                f.write(text)
                tmps.append(f.name)
                argv += [flag, f.name]
        r = subprocess.run(argv, capture_output=True, text=True)
        return r.returncode, r.stdout + r.stderr
    finally:
        for t in tmps:
            os.unlink(t)


def test_gate_passes_on_current_tree():
                                     
    r = subprocess.run([sys.executable, str(GATE)], capture_output=True, text=True)
    assert r.returncode == 0, f"{r.stdout}\n{r.stderr}"


def _mutate(text: str, old: str, new: str) -> str:
    assert old in text, f"앵커를 찾지 못함(소스 포맷 변경?): {old!r}"
    return text.replace(old, new, 1)


def test_real_postinst_reintroduced_load_fails():
                                                          
    src = BUILD.read_text()
    rc, out = _run(build_text=_mutate(
        src, '    mkdir -p "$DISDIR"',
        '    apparmor_parser -r -W -C "$APROF" >/dev/null 2>&1 || true\n    mkdir -p "$DISDIR"'))
    assert rc == 1 and "불변식1" in out, out


def test_real_postinst_all_attach_spellings_fail():
                                                     

                                                          
       
    src = BUILD.read_text()
    for call in ('apparmor_parser -a "$APROF"', 'apparmor_parser --add "$APROF"',
                 'apparmor_parser "$APROF"', 'apparmor_parser -C "$APROF"'):
        rc, out = _run(build_text=_mutate(
            src, '    mkdir -p "$DISDIR"', f'    {call}\n    mkdir -p "$DISDIR"'))
        assert rc == 1 and "불변식1" in out, (call, out)


def test_real_postinst_unlink_disable_then_attach_fails():
                                                               
    src = BUILD.read_text()
    rc, out = _run(build_text=_mutate(
        src, '    mkdir -p "$DISDIR"',
        '    rm -f "$DISLINK"\n    apparmor_parser -C "$APROF"\n    mkdir -p "$DISDIR"'))
    assert rc == 1 and "불변식1" in out and "불변식2" in out, out


def test_real_postinst_append_heredoc_hiding_attach_fails():
                                                           
    src = BUILD.read_text()
    hidden = (
        'cat >> "$STAGE/DEBIAN/postinst" <<\'MORE\'\n'
        'apparmor_parser -r -C /etc/apparmor.d/usr.local.bin.purecvisorsd\n'
        'MORE\n'
        'chmod 0755 "$STAGE/DEBIAN/postinst"'
    )
    rc, out = _run(build_text=_mutate(
        src, 'chmod 0755 "$STAGE/DEBIAN/postinst"', hidden))
    assert rc == 1 and "불변식1" in out, out


def test_real_postinst_non_heredoc_write_fails():
                                                                      
    src = BUILD.read_text()
    rc, out = _run(build_text=_mutate(
        src, 'chmod 0755 "$STAGE/DEBIAN/postinst"',
        'echo "apparmor_parser -r $APROF" >> "$STAGE/DEBIAN/postinst"\n'
        'chmod 0755 "$STAGE/DEBIAN/postinst"'))
    assert rc == 1 and "구조" in out, out


def test_real_postrm_cleanup_moved_out_of_purge_fails():
                                                       
    src = BUILD.read_text()
    moved = _mutate(
        src, '    rm -f /etc/apparmor.d/disable/usr.local.bin.purecvisorsd 2>/dev/null || true\n', '')
    moved = _mutate(
        moved, 'case "$1" in\n  remove|purge)\n    systemctl daemon-reload || true',
        'case "$1" in\n  remove|purge)\n    systemctl daemon-reload || true\n'
        '    rm -f /etc/apparmor.d/disable/usr.local.bin.purecvisorsd 2>/dev/null || true')
    rc, out = _run(build_text=moved)
    assert rc == 1 and "불변식5" in out, out


def test_real_postinst_without_disable_link_fails():
    src = BUILD.read_text()
    rc, out = _run(build_text=_mutate(
        src, '    ln -sfn ../usr.local.bin.purecvisorsd "$DISLINK"\n', ''))
    assert rc == 1 and "불변식2" in out, out


def test_real_postinst_without_unload_fails():
    src = BUILD.read_text()
    rc, out = _run(build_text=_mutate(
        src, '        apparmor_parser -R "$APROF" >/dev/null 2>&1 || true',
        '        true'))
    assert rc == 1 and "불변식3" in out, out


def test_real_postinst_force_complain_revival_fails():
    src = BUILD.read_text()
    rc, out = _run(build_text=_mutate(
        src, '    rm -f "$FCLINK" 2>/dev/null || true',
        '    mkdir -p /etc/apparmor.d/force-complain\n'
        '    ln -sf ../usr.local.bin.purecvisorsd "$FCLINK"'))
    assert rc == 1 and "불변식4" in out, out


def test_real_postrm_without_disable_cleanup_fails():
    src = BUILD.read_text()
    rc, out = _run(build_text=_mutate(
        src, '    rm -f /etc/apparmor.d/disable/usr.local.bin.purecvisorsd 2>/dev/null || true\n', ''))
    assert rc == 1 and "불변식5" in out, out


def test_real_helper_without_warning_fails():
    src = HELPER.read_text()
    rc, out = _run(helper_text=_mutate(src, '  enforce)\n    adr0028_warn\n', '  enforce)\n'))
    assert rc == 1 and "불변식6" in out, out


def test_real_helper_without_disable_removal_fails():
    src = HELPER.read_text()
    rc, out = _run(helper_text=_mutate(
        src, '  complain)\n    adr0028_warn\n    rm -f "$DISLINK"\n',
        '  complain)\n    adr0028_warn\n'))
    assert rc == 1 and "불변식6" in out, out


def test_gate_constants_match_adr():
                                                    
    adr = (ROOT / "docs/adr/0028-2.0-host-hardening-hidepid-over-apparmor-confinement.md").read_text()
    assert DISLINK in adr, "ADR-0028 에 disable 심링크 경로가 없다"
    assert "apparmor_parser -R" in adr, "ADR-0028 에 언로드 계약이 없다"
    assert APROF.rsplit("/", 1)[-1] in adr and FCLINK.split("/")[-2] in adr


if __name__ == "__main__":
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_") and callable(v)]
    failed = 0
    for t in tests:
        try:
            t()
            print(f"OK   {t.__name__}")
        except AssertionError as e:
            failed += 1
            print(f"FAIL {t.__name__}: {e}")
    print(f"[test_deb_apparmor] {len(tests) - failed}/{len(tests)} passed")
    sys.exit(1 if failed else 0)
