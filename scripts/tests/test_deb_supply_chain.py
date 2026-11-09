#!/usr/bin/env python3
                          
                                                                          
                                                                                 
                                                                       
                                                                   
                                                   
 
                      
                                                                                        
                                                               

                           
                                                       
                                                           
                                                                           
                                                     
                                                                 
                                       
                                              
                             
                                         

                                                              

                                                 
   
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from check_deb_supply_chain import (              
    ROOT, BUILD_REL, REQUIRED_FLOOR_COMPONENTS, REQUIRED_UI_ASSETS,
    REQUIRED_RUNTIME_RECOMMENDS,
    check, control_body, func_body,
)

GATE = Path(__file__).resolve().parent.parent / "check_deb_supply_chain.py"
BUILD = ROOT / BUILD_REL

APPLY_LINE = 'LIBDEPS="$(apply_security_floors "$LIBDEPS")"'
ALLDEPS_LINE = 'ALLDEPS="${LIBDEPS:+$LIBDEPS, }$SVCDEPS"'
FLOOR_ASSIGN = '[ -n "$mm" ] && floor=" (>= $mm)"'
MD5_LINE = '( cd "$STAGE" && find usr etc -type f -exec md5sum {} \\; > DEBIAN/md5sums )'
BUILD_LINE = 'fakeroot dpkg-deb --build --root-owner-group "$STAGE" "$OUT" >/dev/null'
VENDOR_LINE = '[ -d ui/vendor ]  && cp -a ui/vendor  "$STAGE/usr/local/share/purecvisor/ui/"'
ASSET_LOOP = ('for f in index.html style.css app.bundle.js sw.js i18n.js manifest.json '
              'offline.html maintenance.html maintenance-status.json; do')
FLOOR_LIBS_LINE = ('SECURITY_FLOOR_LIBS="libssl3 libssl3t64 libsoup-3.0-0 '
                   'libsqlite3-0 libglib2.0-0 libglib2.0-0t64"')
RECOMMENDS_LINE = ('Recommends: openvswitch-switch, ovn-central, ovn-host, zfsutils-linux, '
                   'cloud-image-utils, apparmor-utils')


def _run(build_text: str):
    with tempfile.TemporaryDirectory(prefix="pcv-deb-supply-") as td:
        p = Path(td) / "build-deb.sh"
        p.write_text(build_text, encoding="utf-8")
        proc = subprocess.run([sys.executable, str(GATE), "--build-script", str(p)],
                              capture_output=True, text=True)
        return proc.returncode, proc.stdout + proc.stderr


COPYRIGHT_LINE = 'install -m644 LICENSE "$STAGE/usr/share/doc/$PKG_NAME/copyright"'
NOTICES_LINE = ('install -m644 THIRD_PARTY_NOTICES.md '
                '"$STAGE/usr/share/doc/$PKG_NAME/THIRD_PARTY_NOTICES.md"')
LOGROTATE_LINE = 'install -m644 systemd/purecvisor.logrotate "$STAGE/etc/logrotate.d/purecvisor"'
CORE_CONF = "/etc/systemd/system/purecvisorsd.service.d/95-coredump-hardening.conf"
SANDBOX_CONF = "/etc/systemd/system/purecvisorsd.service.d/97-sandbox.conf"


def _mutate(src: str, old: str, new: str) -> str:
    assert old in src, f"self-test 앵커가 실파일에 없다(스크립트 구조 변경?): {old[:70]}"
    return src.replace(old, new, 1)


def _src() -> str:
    return BUILD.read_text(encoding="utf-8")


                                                              
def test_current_tree_passes():
    proc = subprocess.run([sys.executable, str(GATE)], capture_output=True, text=True)
    out = proc.stdout + proc.stderr
    assert proc.returncode == 0 and "[PASS]" in out, out


def test_untouched_copy_passes():
    rc, out = _run(_src())
    assert rc == 0 and "[PASS]" in out, out


                                                         
def test_floor_application_removed_fails():
    rc, out = _run(_mutate(_src(), APPLY_LINE + "\n", ""))
    assert rc == 1 and "불변식4" in out and "apply_security_floors" in out, out


def test_floor_application_commented_out_fails():
                                    
    rc, out = _run(_mutate(_src(), APPLY_LINE, "# " + APPLY_LINE))
    assert rc == 1 and "불변식4" in out, out


def test_floor_application_after_alldeps_fails():
                                               
    src = _mutate(_src(), APPLY_LINE + "\n", "")
    rc, out = _run(src + "\n" + APPLY_LINE + "\n")
    assert rc == 1 and "불변식4" in out and "뒤에" in out, out


def test_floor_function_removed_fails():
    src = _src()
    start = src.index("apply_security_floors() {")
    end = src.index("\n}\n", start) + len("\n}\n")
    rc, out = _run(src[:start] + src[end:])
    assert rc == 1 and "불변식1" in out, out


def test_floor_constraint_neutralized_fails():
                                         
    rc, out = _run(_mutate(_src(), FLOOR_ASSIGN, '[ -n "$mm" ] && floor=""'))
    assert rc == 1 and "불변식3" in out, out


def test_floor_version_lookup_removed_fails():
    rc, out = _run(_mutate(_src(),
                           'ver="$(dpkg-query -W -f=\'${Version}\' "$pkg" 2>/dev/null || true)"',
                           'ver="0.0"'))
    assert rc == 1 and "불변식3" in out, out


def test_openssl_dropped_from_floor_libs_fails():
    rc, out = _run(_mutate(_src(), FLOOR_LIBS_LINE,
                           'SECURITY_FLOOR_LIBS="libsoup-3.0-0 libsqlite3-0 '
                           'libglib2.0-0 libglib2.0-0t64"'))
    assert rc == 1 and "불변식2" in out and "openssl" in out, out


def test_glib_dropped_from_floor_libs_fails():
    rc, out = _run(_mutate(_src(), FLOOR_LIBS_LINE,
                           'SECURITY_FLOOR_LIBS="libssl3 libssl3t64 libsoup-3.0-0 '
                           'libsqlite3-0"'))
    assert rc == 1 and "불변식2" in out and "glib" in out, out


def test_floor_libs_definition_removed_fails():
    rc, out = _run(_mutate(_src(), FLOOR_LIBS_LINE + "\n", ""))
    assert rc == 1 and "불변식1" in out, out


def test_t64_variant_alone_is_accepted():
                                                    
    rc, out = _run(_mutate(_src(), FLOOR_LIBS_LINE,
                           'SECURITY_FLOOR_LIBS="libssl3t64 libsoup-3.0-0t64 '
                           'libsqlite3-0t64 libglib2.0-0t64"'))
    assert rc == 0, out


                                                           
def test_depends_bypassing_floor_fails():
    rc, out = _run(_mutate(_src(), "Depends: $ALLDEPS", "Depends: $LIBDEPS"))
    assert rc == 1 and "불변식4" in out and "Depends" in out, out


def test_alldeps_without_libdeps_fails():
    rc, out = _run(_mutate(_src(), ALLDEPS_LINE, 'ALLDEPS="$SVCDEPS"'))
    assert rc == 1 and "불변식4" in out, out


                                                          
def test_md5sums_removed_fails():
    rc, out = _run(_mutate(_src(), MD5_LINE + "\n", ""))
    assert rc == 1 and "불변식5" in out, out


def test_md5sums_partial_scope_fails():
    rc, out = _run(_mutate(
        _src(), MD5_LINE,
        '( cd "$STAGE" && find usr -type f -exec md5sum {} \\; > DEBIAN/md5sums )'))
    assert rc == 1 and "불변식5" in out and "전수" in out, out


def test_root_owner_group_removed_fails():
    rc, out = _run(_mutate(_src(), BUILD_LINE,
                           'fakeroot dpkg-deb --build "$STAGE" "$OUT" >/dev/null'))
    assert rc == 1 and "불변식6" in out, out


                                                    
def test_vendor_copy_removed_fails():
    rc, out = _run(_mutate(_src(), VENDOR_LINE + "\n", ""))
    assert rc == 1 and "불변식7" in out, out


def test_asset_verification_loop_removed_fails():
    src = _src()
    start = src.index(ASSET_LOOP)
    end = src.index("done\n", start) + len("done\n")
    rc, out = _run(src[:start] + src[end:])
    assert rc == 1 and "불변식8" in out, out


def test_bundle_dropped_from_asset_check_fails():
    rc, out = _run(_mutate(
        _src(), ASSET_LOOP,
        "for f in index.html style.css sw.js i18n.js manifest.json "
        "offline.html maintenance.html maintenance-status.json; do"))
    assert rc == 1 and "불변식8" in out and "app.bundle.js" in out, out


def test_openvswitch_runtime_recommend_removed_fails():
    rc, out = _run(_mutate(
        _src(), RECOMMENDS_LINE,
        "Recommends: ovn-central, ovn-host, zfsutils-linux, cloud-image-utils, apparmor-utils"))
    assert rc == 1 and "불변식9" in out and "openvswitch-switch" in out, out


                                                         
def test_copyright_install_removed_fails():
    rc, out = _run(_mutate(_src(), COPYRIGHT_LINE, ""))
    assert rc == 1 and "불변식10" in out and "copyright" in out, out


def test_third_party_notices_install_removed_fails():
    rc, out = _run(_mutate(_src(), NOTICES_LINE, ""))
    assert rc == 1 and "불변식10" in out and "THIRD_PARTY_NOTICES" in out, out


def test_logrotate_install_removed_fails():
    rc, out = _run(_mutate(_src(), LOGROTATE_LINE, ""))
    assert rc == 1 and "불변식11" in out and "logrotate" in out, out


def test_coredump_dropin_conffile_removed_fails():
    rc, out = _run(_mutate(_src(), CORE_CONF + "\n", ""))
    assert rc == 1 and "불변식11" in out and "conffiles" in out, out


def test_coredump_dropin_install_removed_fails():
    src = _src()
    start = src.index("install -m644 packaging/systemd/95-coredump-hardening.conf")
    end = src.index("\n", start)
    rc, out = _run(src[:start] + src[end:])
    assert rc == 1 and "불변식11" in out and "95-coredump" in out, out


def test_sandbox_dropin_install_removed_fails():
    src = _src()
    start = src.index("install -m644 packaging/systemd/97-sandbox.conf")
    end = src.index("\n", start)
    rc, out = _run(src[:start] + src[end:])
    assert rc == 1 and "불변식11" in out and "97-sandbox" in out, out

def test_sandbox_dropin_conffile_removed_fails():
    rc, out = _run(_mutate(_src(), SANDBOX_CONF + "\n" + LXC_ETC + "\nCF", LXC_ETC + "\nCF"))
    assert rc == 1 and "불변식11" in out and "conffiles" in out, out

def test_sandbox_dropin_mount_namespace_option_fails():

    with tempfile.TemporaryDirectory() as d:
        dst = Path(d) / "packaging/systemd"
        dst.mkdir(parents=True)
        body = (ROOT / "packaging/systemd/97-sandbox.conf").read_text(encoding="utf-8")
        (dst / "97-sandbox.conf").write_text(body + "ProtectSystem=strict\n", encoding="utf-8")
        bad, _ = check(_src(), root=Path(d))
    assert any("불변식12" in b and "하드 요구사항 1" in b for b in bad), bad

def _sandbox_check(body: str):
    with tempfile.TemporaryDirectory() as d:
        dst = Path(d) / "packaging/systemd"
        dst.mkdir(parents=True)
        (dst / "97-sandbox.conf").write_text(body, encoding="utf-8")
        return check(_src(), root=Path(d))[0]

def _sandbox_mutations():
    real = (ROOT / "packaging/systemd/97-sandbox.conf").read_text(encoding="utf-8")
    return {
        "invalid-value": real + "LockPersonality=maybe\n",
        "unknown-syscall-group": real + "SystemCallFilter=~@nosuchgroup\n",
        "invalid-notify": real + "NotifyAccess=bogus\n",
        "misspelled-section": real.replace("[Service]", "[Servic]"),
    }

@pytest.mark.skipif(shutil.which("systemd-analyze") is None, reason="systemd-analyze 없음")
def test_sandbox_dropin_ignored_option_fails():

    for name, text in sorted(_sandbox_mutations().items()):
        bad = _sandbox_check(text)
        assert any("불변식12" in b and "verify" in b for b in bad), (name, bad)

@pytest.mark.skipif(shutil.which("systemd-analyze") is None, reason="systemd-analyze 없음")
def test_sandbox_dropin_real_file_verifies_clean():
    real = (ROOT / "packaging/systemd/97-sandbox.conf").read_text(encoding="utf-8")
    assert not [b for b in _sandbox_check(real) if "불변식12" in b]

def test_sandbox_dropin_missing_fails():
    with tempfile.TemporaryDirectory() as d:
        bad, _ = check(_src(), root=Path(d))
    assert any("불변식12" in b and "97-sandbox.conf 없음" in b for b in bad), bad

def test_notices_cover_every_vendor_component():
    notices = (ROOT / "THIRD_PARTY_NOTICES.md").read_text(encoding="utf-8")
    for p in sorted((ROOT / "ui/vendor").iterdir()):
        rel = f"ui/vendor/{p.name}" + ("/" if p.is_dir() else "")
        assert rel in notices, f"{rel} 가 THIRD_PARTY_NOTICES.md 에 없다"


def test_logrotate_template_has_no_phantom_unit():
    text = (ROOT / "systemd/purecvisor.logrotate").read_text(encoding="utf-8")
    assert "purecvisormd" not in text.replace("purecvisormd 유닛은 없다", "")
    assert "reload purecvisorsd" in text



LXC_UNIT_PATH = ROOT / "packaging/systemd/purecvisor-lxc@.service"
LXC_UNIT_HEADER = ROOT / "src/modules/lxc/lxc_unit.h"
LXC_ETC = "/etc/systemd/system/purecvisor-lxc@.service"

def _lxc_check(unit_text=None, deploy_text=None, build_text=None, header_text=None):

    with tempfile.TemporaryDirectory(prefix="pcv-lxc-unit-") as d:
        root = Path(d)
        (root / "packaging/systemd").mkdir(parents=True)
        (root / "scripts").mkdir()
        (root / "src/modules/lxc").mkdir(parents=True)
        header = LXC_UNIT_HEADER.read_text(encoding="utf-8") if header_text is None else header_text
        (root / "src/modules/lxc/lxc_unit.h").write_text(header, encoding="utf-8")
        (root / "packaging/systemd/97-sandbox.conf").write_text(
            (ROOT / "packaging/systemd/97-sandbox.conf").read_text(encoding="utf-8"), encoding="utf-8")
        text = LXC_UNIT_PATH.read_text(encoding="utf-8") if unit_text is None else unit_text
        (root / "packaging/systemd/purecvisor-lxc@.service").write_text(text, encoding="utf-8")
        deploy = (ROOT / "scripts/deploy.sh").read_text(encoding="utf-8") if deploy_text is None else deploy_text
        (root / "scripts/deploy.sh").write_text(deploy, encoding="utf-8")
        bad, _ = check(_src() if build_text is None else build_text, root=root)
    return [b for b in bad if "불변식13" in b]

def _unit() -> str:
    return LXC_UNIT_PATH.read_text(encoding="utf-8")

def test_lxc_unit_contract_pass():
    assert _lxc_check() == []
    rc, out = _run(_src())
    assert rc == 0 and "불변식13" not in out, out

def test_red_lxc_unit_execstop_no_dash():
    bad = _lxc_check(_mutate(_unit(), "ExecStop=-/usr/bin/lxc-stop", "ExecStop=/usr/bin/lxc-stop"))
    assert any("ExecStop" in b for b in bad), bad

def test_red_lxc_unit_sandbox_option():
    for extra in ("LockPersonality=yes", "SystemCallFilter=~@swap", "NoNewPrivileges=yes", "Nice=-5"):
        bad = _lxc_check(_unit() + extra + "\n")
        assert any("금지 키" in b and extra.split("=")[0] in b for b in bad), (extra, bad)

def test_red_lxc_unit_capital_i():
    bad = _lxc_check(_mutate(_unit(), "PIDFile=/run/purecvisor-lxc/%i.pid", "PIDFile=/run/purecvisor-lxc/%I.pid"))
    assert any("%I" in b for b in bad), bad

def test_red_lxc_unit_env_dash():
    bad = _lxc_check(_mutate(_unit(), "EnvironmentFile=/run/", "EnvironmentFile=-/run/"))
    assert any("EnvironmentFile" in b for b in bad), bad

def test_red_lxc_unit_not_in_conffiles():
    bad = _lxc_check(build_text=_mutate(_src(), LXC_ETC + "\nCF", "CF"))
    assert any("conffiles" in b for b in bad), bad
    rc, out = _run(_mutate(_src(), LXC_ETC + "\nCF", "CF"))
    assert rc == 1 and "불변식13" in out and "conffiles" in out, out

def test_red_lxc_unit_install_section():
    bad = _lxc_check(_unit() + "\n[Install]\nWantedBy=multi-user.target\n")
    assert any("[Install]" in b for b in bad), bad

def test_red_lxc_unit_start_limit():
    bad = _lxc_check(_mutate(_unit(), "StartLimitIntervalSec=0\n", ""))
    assert any("StartLimitIntervalSec" in b for b in bad), bad

def test_red_lxc_unit_stop_timeout_not_longer_than_daemon():
    bad = _lxc_check(_mutate(_unit(), "\nTimeoutStopSec=90\n", "\nTimeoutStopSec=30\n"))
    assert any("TimeoutStopSec" in b for b in bad), bad

def test_red_lxc_unit_install_removed_from_deb():
    src = _src()
    start = src.index("install -m644 packaging/systemd/purecvisor-lxc@.service")
    end = src.index("\n", start)
    rc, out = _run(src[:start] + src[end:])
    assert rc == 1 and "불변식13" in out and "deb 에 싣지" in out, out

def _header() -> str:
    return LXC_UNIT_HEADER.read_text(encoding="utf-8")

def test_red_lxc_unit_timeout_order():

    start26 = "#define PCV_LXC_UNIT_START_TIMEOUT_SEC 26"
    for bad_hdr in (_mutate(_header(), start26, "#define PCV_LXC_UNIT_START_TIMEOUT_SEC 25"),
                    _mutate(_header(), start26, "#define PCV_LXC_UNIT_START_TIMEOUT_SEC 27"),
                    _mutate(_header(), "#define PCV_LXC_UNIT_PREP_BUDGET_SEC   4",
                            "#define PCV_LXC_UNIT_PREP_BUDGET_SEC   5")):
        bad = _lxc_check(header_text=bad_hdr)
        assert any("시작 상한 순서" in b for b in bad), bad
    bad = _lxc_check(_mutate(_unit(), "\nTimeoutStartSec=25\n", "\nTimeoutStartSec=26\n"))
    assert any("시작 상한 순서" in b for b in bad), bad
    bad = _lxc_check(header_text=_header().replace(start26, "#define PCV_LXC_UNIT_START_TIMEOUT_SEC_X 26"))
    assert any("시작 상한을 읽지 못함" in b for b in bad), bad

def test_red_lxc_unit_env_path_or_template_mismatch():

    bad = _lxc_check(header_text=_mutate(_header(), '"/run/purecvisor/lxc-unit.env"', '"/run/purecvisor/lxc.env"'))
    assert any("PCV_LXC_UNIT_ENV_PATH" in b for b in bad), bad
    bad = _lxc_check(_mutate(_unit(), "EnvironmentFile=/run/purecvisor/lxc-unit.env",
                             "EnvironmentFile=/run/purecvisor/other.env"))
    assert any("PCV_LXC_UNIT_ENV_PATH" in b for b in bad), bad
    bad = _lxc_check(header_text=_mutate(_header(), '"purecvisor-lxc@"', '"purecvisor-ctr@"'))
    assert any("PCV_LXC_UNIT_TEMPLATE" in b for b in bad), bad
    with tempfile.TemporaryDirectory(prefix="pcv-lxc-unit-nohdr-") as d:
        root = Path(d)
        (root / "packaging/systemd").mkdir(parents=True)
        (root / "packaging/systemd/purecvisor-lxc@.service").write_text(_unit(), encoding="utf-8")
        (root / "scripts").mkdir()
        (root / "scripts/deploy.sh").write_text((ROOT / "scripts/deploy.sh").read_text(encoding="utf-8"),
                                                encoding="utf-8")
        (root / "packaging/systemd/97-sandbox.conf").write_text(
            (ROOT / "packaging/systemd/97-sandbox.conf").read_text(encoding="utf-8"), encoding="utf-8")
        bad, _ = check(_src(), root=root)
    assert any("lxc_unit.h 없음" in b for b in bad), bad

def test_lxc_unit_requires_runtime_config():

    token = " -f /run/purecvisor/lxc-runtime/%i/config"
    assert token in _unit()
    bad = _lxc_check(_mutate(_unit(), token, ""))
    assert any("'-f /run/purecvisor/lxc-runtime/%i/config' 토큰" in b for b in bad), bad
    assert any("PCV_LXC_RUNTIME_CONFIG_DIR" in b for b in bad), bad
    bad = _lxc_check(header_text=_mutate(_header(), '"/run/purecvisor/lxc-runtime"', '"/run/purecvisor/lxc-rt"'))
    assert any("PCV_LXC_RUNTIME_CONFIG_DIR" in b for b in bad), bad
    bad = _lxc_check(_mutate(_unit(), token, " -f /run/purecvisor/other/%i/config"))
    assert any("PCV_LXC_RUNTIME_CONFIG_DIR" in b for b in bad), bad
    bad = _lxc_check(header_text=_header().replace("#define PCV_LXC_RUNTIME_CONFIG_DIR", "#define PCV_LXC_RUNTIME_CONFIG_DIR_X"))
    assert any("PCV_LXC_RUNTIME_CONFIG_DIR" in b for b in bad), bad

def _deploy() -> str:
    return (ROOT / "scripts/deploy.sh").read_text(encoding="utf-8")

def test_red_deploy_sh_no_lxc_unit():
    src = _deploy()

    marker = "            /etc/systemd/system/purecvisor-lxc@.service\n"
    idx = src.index(marker)
    line_start = src.rindex("\n        sudo install", 0, idx) + 1
    mutated = src[:line_start] + src[idx + len(marker):]
    bad = _lxc_check(deploy_text=mutated)
    assert any("deploy.sh" in b and "두 경로" in b for b in bad), bad
    bad = _lxc_check(deploy_text="")
    assert any("deploy.sh" in b for b in bad), bad

def test_red_deploy_sh_no_daemon_reload():
    src = _deploy()
    marker = ("            /etc/systemd/system/purecvisor-lxc@.service\n"
              "        sudo systemctl daemon-reload\n")
    bad = _lxc_check(deploy_text=_mutate(src, marker, marker.replace("        sudo systemctl daemon-reload\n", "")))
    assert any("daemon-reload" in b for b in bad), bad

def test_red_deploy_sh_nonblocking_lxc_unit():
    src = _deploy()
    bad = _lxc_check(deploy_text=_mutate(
        src, "            /etc/systemd/system/purecvisor-lxc@.service\n        sudo systemctl daemon-reload\n",
        "            /etc/systemd/system/purecvisor-lxc@.service || true\n        sudo systemctl daemon-reload\n"))
    assert any("비차단" in b for b in bad), bad

def test_missing_build_script_is_exit2():
    proc = subprocess.run(
        [sys.executable, str(GATE), "--build-script", "/nonexistent/build-deb.sh"],
        capture_output=True, text=True)
    assert proc.returncode == 2, proc.stdout + proc.stderr


def test_missing_control_heredoc_fails():
    src = _src()
    rc, out = _run(src.replace('cat > "$STAGE/DEBIAN/control" <<CTRL',
                               'cat > "$STAGE/DEBIAN/control.tmpl" <<CTRL', 1))
    assert rc == 1 and "[구조]" in out, out


                                                              
def test_func_body_balances_braces():
    code = 'f() {\n  if x; then { y; }\n  fi\n}\ng() { z; }\n'
    assert "if x" in func_body(code, "f") and "z" not in func_body(code, "f")
    assert func_body(code, "g").strip() == "z;"
    assert func_body(code, "missing") == ""


def test_control_body_extracts_metadata():
    ctrl = control_body(_src())
    assert "Package:" in ctrl and "Depends:" in ctrl, ctrl[:200]
    assert "md5sums" not in ctrl, "control heredoc 밖 내용을 삼켰다"


def test_runtime_recommend_constant_matches_control():
    ctrl = control_body(_src())
    assert REQUIRED_RUNTIME_RECOMMENDS == {"openvswitch-switch"}
    assert RECOMMENDS_LINE in ctrl


def test_gate_constants_match_real_script():
                                                   
    src = _src()
    bad, info = check(src)
    assert not bad, bad
    for spellings in REQUIRED_FLOOR_COMPONENTS.values():
        assert any(s in info["floor_libs"] for s in spellings), spellings
    for asset in REQUIRED_UI_ASSETS:
        assert asset in info["verified_assets"], asset


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
    print(f"[test_deb_supply_chain] {len(tests) - failed}/{len(tests)} passed")
    sys.exit(1 if failed else 0)
