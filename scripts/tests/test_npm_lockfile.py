#!/usr/bin/env python3
                          
                                                                     
                                                                                 
                                                                       
                                                                   
                                                   
 
                      
                                                                                   
                                                           

                           
                                                             
                                                
                                        
                             
                                                                
                         
                                                     
                                    
                                              
                                                           

                                             
   
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from check_npm_lockfile import (              
    ROOT, PKG_REL, LOCK_REL, satisfies, check_npx,
)

GATE = Path(__file__).resolve().parent.parent / "check_npm_lockfile.py"


def _run(argv):
    proc = subprocess.run([sys.executable, str(GATE)] + argv,
                          capture_output=True, text=True)
    return proc.returncode, proc.stdout + proc.stderr


class Sandbox:
    def __enter__(self):
        self.dir = Path(tempfile.mkdtemp(prefix="pcv-npm-gate-"))
        shutil.copy2(ROOT / PKG_REL, self.dir / PKG_REL)
        shutil.copy2(ROOT / LOCK_REL, self.dir / LOCK_REL)
        shutil.copy2(ROOT / "Makefile", self.dir / "Makefile")
        return self

    def __exit__(self, *exc):
        shutil.rmtree(self.dir, ignore_errors=True)
        return False

    def pkg(self):
        return json.loads((self.dir / PKG_REL).read_text(encoding="utf-8"))

    def lock(self):
        return json.loads((self.dir / LOCK_REL).read_text(encoding="utf-8"))

    def write_pkg(self, doc):
        (self.dir / PKG_REL).write_text(json.dumps(doc, indent=2), encoding="utf-8")

    def write_lock(self, doc):
        (self.dir / LOCK_REL).write_text(json.dumps(doc, indent=2), encoding="utf-8")

    def makefile(self):
        return (self.dir / "Makefile").read_text(encoding="utf-8")

    def write_makefile(self, text):
        (self.dir / "Makefile").write_text(text, encoding="utf-8")

    def first_dep_key(self):
        return next(k for k in self.lock()["packages"] if k)

    def run(self):
        return _run(["--package", str(self.dir / PKG_REL),
                     "--lock", str(self.dir / LOCK_REL),
                     "--root", str(self.dir)])


                                                              
def test_current_tree_passes():
    rc, out = _run([])
    assert rc == 0 and "[PASS]" in out, out


def test_sandbox_copy_passes():
    with Sandbox() as sb:
        rc, out = sb.run()
        assert rc == 0 and "[PASS]" in out, out


                                                         
def test_missing_integrity_fails():
    with Sandbox() as sb:
        lock = sb.lock()
        key = next(k for k in lock["packages"] if k)
        del lock["packages"][key]["integrity"]
        sb.write_lock(lock)
        rc, out = sb.run()
        assert rc == 1 and "불변식5" in out and "integrity 없음" in out, out


def test_sha1_integrity_rejected():
                                            
    with Sandbox() as sb:
        lock = sb.lock()
        key = next(k for k in lock["packages"] if k)
        lock["packages"][key]["integrity"] = "sha1-abcdefghijklmnopqrstuvwxyz0="
        sb.write_lock(lock)
        rc, out = sb.run()
        assert rc == 1 and "불변식6" in out, out


def test_malformed_integrity_rejected():
    with Sandbox() as sb:
        lock = sb.lock()
        key = next(k for k in lock["packages"] if k)
        lock["packages"][key]["integrity"] = "trust-me"
        sb.write_lock(lock)
        rc, out = sb.run()
        assert rc == 1 and "불변식6" in out, out


def test_missing_resolved_fails():
    with Sandbox() as sb:
        lock = sb.lock()
        key = next(k for k in lock["packages"] if k)
        del lock["packages"][key]["resolved"]
        sb.write_lock(lock)
        rc, out = sb.run()
        assert rc == 1 and "출처 불명" in out, out


                                                           
def test_foreign_registry_host_fails():
    with Sandbox() as sb:
        lock = sb.lock()
        key = next(k for k in lock["packages"] if k)
        lock["packages"][key]["resolved"] = "https://evil.example.com/pkg.tgz"
        sb.write_lock(lock)
        rc, out = sb.run()
        assert rc == 1 and "불변식7" in out and "evil.example.com" in out, out


def test_plain_http_resolved_fails():
    with Sandbox() as sb:
        lock = sb.lock()
        key = next(k for k in lock["packages"] if k)
        lock["packages"][key]["resolved"] = \
            "http://registry.npmjs.org/x/-/x-1.0.0.tgz"
        sb.write_lock(lock)
        rc, out = sb.run()
        assert rc == 1 and "불변식7" in out and "https" in out, out


def test_git_resolved_fails():
    with Sandbox() as sb:
        lock = sb.lock()
        key = next(k for k in lock["packages"] if k)
        lock["packages"][key]["resolved"] = "git+ssh://git@github.com/x/y.git#abc"
        sb.write_lock(lock)
        rc, out = sb.run()
        assert rc == 1 and "불변식7" in out, out


                                                     
def test_lockfile_version_downgrade_fails():
    with Sandbox() as sb:
        lock = sb.lock()
        lock["lockfileVersion"] = 1
        sb.write_lock(lock)
        rc, out = sb.run()
        assert rc == 1 and "불변식2" in out, out


def test_package_json_range_bump_without_lock_fails():
                                                      
    with Sandbox() as sb:
        pkg = sb.pkg()
        pkg["devDependencies"]["esbuild"] = "^99.0.0"
        sb.write_pkg(pkg)
        rc, out = sb.run()
        assert rc == 1, out
        assert "불변식3" in out, out                    
        assert "불변식4" in out and "드리프트" in out, out               


def test_lock_root_declaration_drift_fails():
                                 
    with Sandbox() as sb:
        lock = sb.lock()
        lock["packages"][""]["devDependencies"]["esbuild"] = "^0.1.0"
        sb.write_lock(lock)
        rc, out = sb.run()
        assert rc == 1 and "불변식3" in out, out


def test_declared_dep_missing_from_lock_fails():
    with Sandbox() as sb:
        lock = sb.lock()
        del lock["packages"]["node_modules/esbuild"]
        sb.write_lock(lock)
        rc, out = sb.run()
        assert rc == 1 and "불변식4" in out and "esbuild" in out, out


def test_wildcard_range_rejected():
    with Sandbox() as sb:
        pkg = sb.pkg()
        pkg["devDependencies"]["esbuild"] = "*"
        sb.write_pkg(pkg)
        lock = sb.lock()
        lock["packages"][""]["devDependencies"]["esbuild"] = "*"
        sb.write_lock(lock)
        rc, out = sb.run()
        assert rc == 1 and "불변식4" in out, out


def test_unparseable_range_is_fail_closed():
                                        
    with Sandbox() as sb:
        pkg = sb.pkg()
        pkg["devDependencies"]["esbuild"] = "0.x || >=1 <2"
        sb.write_pkg(pkg)
        lock = sb.lock()
        lock["packages"][""]["devDependencies"]["esbuild"] = "0.x || >=1 <2"
        sb.write_lock(lock)
        rc, out = sb.run()
        assert rc == 1 and "해석하지" in out, out


def test_empty_packages_fails():
    with Sandbox() as sb:
        lock = sb.lock()
        lock["packages"] = {"": lock["packages"][""]}
        sb.write_lock(lock)
        rc, out = sb.run()
        assert rc == 1, out
        assert "불변식5" in out or "불변식4" in out, out


                                                         
def test_npx_without_no_install_fails():
    with Sandbox() as sb:
        sb.write_makefile(sb.makefile().replace("npx --no-install esbuild",
                                                "npx esbuild"))
        rc, out = sb.run()
        assert rc == 1 and "불변식8" in out and "--no-install" in out, out


def test_npx_undeclared_package_fails():
    with Sandbox() as sb:
        sb.write_makefile(sb.makefile().replace("npx --no-install esbuild",
                                                "npx --no-install rollup"))
        rc, out = sb.run()
        assert rc == 1 and "불변식9" in out and "rollup" in out, out


def test_commented_npx_is_not_flagged():
                                              
    with Sandbox() as sb:
        sb.write_makefile("# npx esbuild --minify\n" + sb.makefile())
        rc, out = sb.run()
        assert rc == 0, out


                                                         
def test_missing_lock_is_exit2():
    with Sandbox() as sb:
        (sb.dir / LOCK_REL).unlink()
        rc, out = sb.run()
        assert rc == 2 and "package-lock.json" in out, out


def test_missing_package_json_is_exit2():
    with Sandbox() as sb:
        (sb.dir / PKG_REL).unlink()
        rc, out = sb.run()
        assert rc == 2, out


def test_malformed_json_is_exit2():
    with Sandbox() as sb:
        (sb.dir / LOCK_REL).write_text("{ nope", encoding="utf-8")
        rc, out = sb.run()
        assert rc == 2, out


                                                           
def test_caret_semantics():
    assert satisfies("4.11.4", "^4.11.4") is True
    assert satisfies("4.99.0", "^4.11.4") is True
    assert satisfies("5.0.0", "^4.11.4") is False
    assert satisfies("4.11.3", "^4.11.4") is False
                    
    assert satisfies("0.28.5", "^0.28.1") is True
    assert satisfies("0.29.0", "^0.28.1") is False
                   
    assert satisfies("0.0.3", "^0.0.3") is True
    assert satisfies("0.0.4", "^0.0.3") is False


def test_tilde_and_gte_semantics():
    assert satisfies("1.2.9", "~1.2.3") is True
    assert satisfies("1.3.0", "~1.2.3") is False
    assert satisfies("2.0.0", ">=1.5.0") is True
    assert satisfies("1.4.9", ">=1.5.0") is False
    assert satisfies("1.2.3", "1.2.3") is True
    assert satisfies("1.2.4", "1.2.3") is False


def test_unsupported_syntax_returns_none():
    for spec in ("1.x", ">=1 <2", "^1 || ^2", "npm:other@1.0.0",
                 "github:a/b", "workspace:*"):
        assert satisfies("1.0.0", spec) is None, spec
                                     
    assert satisfies("1.0.0-beta.1", "^1.0.0") is None


def test_check_npx_unit_flags_bare_npx():
    pkg = {"devDependencies": {"esbuild": "^0.28.1"}}
    bad, n = check_npx(pkg, [("Makefile", "\tnpx esbuild in.js\n")])
    assert n == 1 and any("불변식8" in b for b in bad), bad
    bad, n = check_npx(pkg, [("Makefile", "\tnpx --no-install esbuild in.js\n")])
    assert n == 1 and not bad, bad


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
    print(f"[test_npm_lockfile] {len(tests) - failed}/{len(tests)} passed")
    sys.exit(1 if failed else 0)
