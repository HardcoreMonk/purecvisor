#!/usr/bin/env python3
                          
                                                                          
                                                                                 
                                                                       
                                                                   
                                                   
 
                      
                                                                                        
                                                               

                           
                                                       
                                                           
                                                                           
                                                     
                                                                 
                                       
                                              
                             
                                         
                                                              

                                                 
   
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from check_deb_supply_chain import (              
    ROOT, BUILD_REL, REQUIRED_FLOOR_COMPONENTS, REQUIRED_UI_ASSETS,
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
ASSET_LOOP = 'for f in index.html style.css app.bundle.js sw.js i18n.js manifest.json; do'
FLOOR_LIBS_LINE = ('SECURITY_FLOOR_LIBS="libssl3 libssl3t64 libsoup-3.0-0 '
                   'libsqlite3-0 libglib2.0-0 libglib2.0-0t64"')


def _run(build_text: str):
    with tempfile.TemporaryDirectory(prefix="pcv-deb-supply-") as td:
        p = Path(td) / "build-deb.sh"
        p.write_text(build_text, encoding="utf-8")
        proc = subprocess.run([sys.executable, str(GATE), "--build-script", str(p)],
                              capture_output=True, text=True)
        return proc.returncode, proc.stdout + proc.stderr


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
        "for f in index.html style.css sw.js i18n.js manifest.json; do"))
    assert rc == 1 and "불변식8" in out and "app.bundle.js" in out, out


                                                         
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
