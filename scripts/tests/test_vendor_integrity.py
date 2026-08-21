#!/usr/bin/env python3
                          
                                                                      
                                                                                 
                                                                       
                                                                   
                                                   
 
                      
                                                                                    
                                                               

                           
                                                           
                                       
                       
                                            
                        
                        
                            
                                 
                                   
                                                                   
                                       

                                             
   
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from check_vendor_integrity import (              
    ROOT, MANIFEST_REL, VENDOR_REL, build_actual, scan_vendor, sha256_of,
)

GATE = Path(__file__).resolve().parent.parent / "check_vendor_integrity.py"
MANIFEST = ROOT / MANIFEST_REL
VENDOR = ROOT / VENDOR_REL


def _run(argv):
    proc = subprocess.run([sys.executable, str(GATE)] + argv,
                          capture_output=True, text=True)
    return proc.returncode, proc.stdout + proc.stderr


class Sandbox:
                                                     

    def __enter__(self):
        self.dir = Path(tempfile.mkdtemp(prefix="pcv-vendor-gate-"))
        (self.dir / "contracts").mkdir(parents=True, exist_ok=True)
        shutil.copytree(VENDOR, self.dir / VENDOR_REL, symlinks=True)
        shutil.copy2(MANIFEST, self.dir / MANIFEST_REL)
        return self

    def __exit__(self, *exc):
        shutil.rmtree(self.dir, ignore_errors=True)
        return False

                                                             
    @property
    def manifest_path(self):
        return self.dir / MANIFEST_REL

    def manifest(self):
        return json.loads(self.manifest_path.read_text(encoding="utf-8"))

    def write_manifest(self, doc):
        self.manifest_path.write_text(json.dumps(doc, indent=2, ensure_ascii=False),
                                      encoding="utf-8")

    def run(self):
        return _run(["--root", str(self.dir),
                     "--vendor-dir", str(self.dir / VENDOR_REL),
                     "--manifest", str(self.manifest_path)])


                                                             
def test_current_tree_passes():
    rc, out = _run([])
    assert rc == 0, out
    assert "[PASS]" in out, out


def test_sandbox_copy_passes():
                                                   
    with Sandbox() as sb:
        rc, out = sb.run()
        assert rc == 0 and "[PASS]" in out, out


                                                             
def test_mutated_vendor_byte_fails():
    with Sandbox() as sb:
        target = sb.dir / "ui/vendor/chart.umd.min.js"
        data = bytearray(target.read_bytes())
        data[0] ^= 0x01                     
        target.write_bytes(bytes(data))
        rc, out = sb.run()
        assert rc == 1 and "불변식5" in out and "SHA-256 불일치" in out, out


def test_mutated_vendor_byte_same_length_fails():
                                                 
    with Sandbox() as sb:
        target = sb.dir / "ui/vendor/pretendard/pretendard.css"
        text = target.read_text(encoding="utf-8")
                           
        mutated = text.replace("swap", "SWAP", 1)
        assert len(mutated) == len(text) and mutated != text
        target.write_text(mutated, encoding="utf-8")
        rc, out = sb.run()
        assert rc == 1 and "SHA-256 불일치" in out, out
        assert "크기" not in out, "크기는 같아야 한다(해시만으로 잡혀야 함)"


def test_unlisted_new_vendor_file_fails():
    with Sandbox() as sb:
        (sb.dir / "ui/vendor/analytics.min.js").write_text(
            "/* smuggled */\n", encoding="utf-8")
        rc, out = sb.run()
        assert rc == 1 and "불변식3" in out and "analytics.min.js" in out, out


def test_dropping_manifest_entry_fails():
                                                
    with Sandbox() as sb:
        doc = sb.manifest()
        doc["assets"] = [a for a in doc["assets"]
                         if a["path"] != "ui/vendor/novnc/novnc.esm.js"]
        sb.write_manifest(doc)
        rc, out = sb.run()
        assert rc == 1 and "불변식3" in out and "novnc.esm.js" in out, out


def test_missing_file_with_live_entry_fails():
    with Sandbox() as sb:
        (sb.dir / "ui/vendor/coolicons/coolicons.svg").unlink()
        rc, out = sb.run()
        assert rc == 1 and "불변식4" in out and "coolicons.svg" in out, out


def test_corrupted_sha_fails():
    with Sandbox() as sb:
        doc = sb.manifest()
        doc["assets"][0]["sha256"] = "0" * 64
        sb.write_manifest(doc)
        rc, out = sb.run()
        assert rc == 1 and "불변식5" in out, out


def test_corrupted_bytes_fails():
    with Sandbox() as sb:
        doc = sb.manifest()
        doc["assets"][0]["bytes"] = 1
        sb.write_manifest(doc)
        rc, out = sb.run()
        assert rc == 1 and "불변식5" in out and "크기" in out, out


def test_short_sha_rejected():
    with Sandbox() as sb:
        doc = sb.manifest()
        doc["assets"][0]["sha256"] = "deadbeef"
        sb.write_manifest(doc)
        rc, out = sb.run()
        assert rc == 1 and "64자 hex" in out, out


def test_missing_manifest_key_fails():
    with Sandbox() as sb:
        doc = sb.manifest()
        del doc["assets"][0]["source"]
        sb.write_manifest(doc)
        rc, out = sb.run()
        assert rc == 1 and "불변식1" in out and "source" in out, out


def test_empty_component_fails():
                                                              
    with Sandbox() as sb:
        doc = sb.manifest()
        doc["assets"][0]["component"] = None
        sb.write_manifest(doc)
        rc, out = sb.run()
        assert rc == 1 and "불변식1" in out and "component" in out, out


def test_null_version_is_allowed():
                                                   
    with Sandbox() as sb:
        doc = sb.manifest()
        for a in doc["assets"]:
            a["version"] = None
            a["license"] = None
        sb.write_manifest(doc)
        rc, out = sb.run()
        assert rc == 0, out


def test_removed_manifest_is_exit2():
    with Sandbox() as sb:
        sb.manifest_path.unlink()
        rc, out = sb.run()
        assert rc == 2 and "핀이 통째로 제거" in out, out


def test_empty_assets_fails():
    with Sandbox() as sb:
        sb.write_manifest({"assets": []})
        rc, out = sb.run()
        assert rc == 1 and "불변식1" in out, out


def test_path_escape_rejected():
    with Sandbox() as sb:
        doc = sb.manifest()
        doc["assets"][0]["path"] = "ui/vendor/../../etc/passwd"
        sb.write_manifest(doc)
        rc, out = sb.run()
        assert rc == 1 and "불변식2" in out, out


def test_absolute_path_rejected():
    with Sandbox() as sb:
        doc = sb.manifest()
        doc["assets"][0]["path"] = "/etc/passwd"
        sb.write_manifest(doc)
        rc, out = sb.run()
        assert rc == 1 and "불변식2" in out, out


def test_outside_vendor_path_rejected():
    with Sandbox() as sb:
        doc = sb.manifest()
        doc["assets"][0]["path"] = "ui/app.js"
        sb.write_manifest(doc)
        rc, out = sb.run()
        assert rc == 1 and "불변식2" in out, out


def test_duplicate_entry_rejected():
    with Sandbox() as sb:
        doc = sb.manifest()
        doc["assets"].append(dict(doc["assets"][0]))
        sb.write_manifest(doc)
        rc, out = sb.run()
        assert rc == 1 and "중복 등재" in out, out


def test_symlink_in_vendor_dir_fails():
    with Sandbox() as sb:
        outside = sb.dir / "outside.js"
        outside.write_text("/* outside the pin */\n", encoding="utf-8")
        link = sb.dir / "ui/vendor/linked.js"
        os.symlink(outside, link)
        rc, out = sb.run()
        assert rc == 1 and "심링크" in out, out


def test_malformed_manifest_is_exit2():
    with Sandbox() as sb:
        sb.manifest_path.write_text("{ not json", encoding="utf-8")
        rc, out = sb.run()
        assert rc == 2, out


                                                              
def test_print_actual_carries_provenance():
    with Sandbox() as sb:
        target = sb.dir / "ui/vendor/chart.umd.min.js"
        target.write_bytes(target.read_bytes() + b"\n/* upstream bump */\n")
        proc = subprocess.run(
            [sys.executable, str(GATE), "--print-actual",
             "--root", str(sb.dir),
             "--vendor-dir", str(sb.dir / VENDOR_REL),
             "--manifest", str(sb.manifest_path)],
            capture_output=True, text=True)
        assert proc.returncode == 0, proc.stderr
        doc = json.loads(proc.stdout)
        entry = next(a for a in doc["assets"]
                     if a["path"] == "ui/vendor/chart.umd.min.js")
        assert entry["component"] == "chart.js", entry
        assert entry["sha256"] == sha256_of(target), entry
                                                   
        sb.write_manifest(doc)
        rc, out = sb.run()
        assert rc == 0, out


def test_print_actual_leaves_new_paths_null():
                                                   
    with Sandbox() as sb:
        (sb.dir / "ui/vendor/newthing.js").write_text("x\n", encoding="utf-8")
        doc = build_actual(sb.dir, sb.dir / VENDOR_REL, sb.manifest())
        entry = next(a for a in doc["assets"]
                     if a["path"] == "ui/vendor/newthing.js")
        assert entry["component"] is None and entry["source"] is None, entry
                                                
        sb.write_manifest(doc)
        rc, out = sb.run()
        assert rc == 1 and "불변식1" in out, out


                                                             
def test_manifest_covers_every_tracked_vendor_file():
                                                        
    proc = subprocess.run(["git", "ls-files", VENDOR_REL],
                          cwd=str(ROOT), capture_output=True, text=True)
    if proc.returncode != 0:
        return                     
    tracked = {line for line in proc.stdout.split("\n") if line.strip()}
    pinned = {a["path"] for a in json.loads(
        MANIFEST.read_text(encoding="utf-8"))["assets"]}
    assert tracked <= pinned, f"미등재 추적 자산: {sorted(tracked - pinned)}"
    scanned = {p.relative_to(ROOT).as_posix() for p in scan_vendor(VENDOR)}
    assert pinned == scanned, (
        f"매니페스트≠실스캔: only-manifest={sorted(pinned - scanned)} "
        f"only-tree={sorted(scanned - pinned)}")


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
    print(f"[test_vendor_integrity] {len(tests) - failed}/{len(tests)} passed")
    sys.exit(1 if failed else 0)
