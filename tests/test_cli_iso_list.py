#!/usr/bin/env python3
                          
                                                                                                
                                                                                                  
                                                                                              
                                                     
 
                                                                         
                            
from __future__ import annotations

import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
from test_cli_vm_usb import CLI, _ok, run_fake              

import os              

OBJECTS = [
    {"name": "ubuntu.iso", "path": "/var/lib/purecvisor/iso/ubuntu.iso", "dir": "/var/lib/purecvisor/iso", "size_mb": 1024},
    {"name": "nosize.iso", "path": "/var/lib/purecvisor/iso/nosize.iso", "dir": "/var/lib/purecvisor/iso"},
]


class CliIsoList(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not os.access(CLI, os.X_OK):
            raise AssertionError("missing bin/pcvctl; run make cli first")

    def _run(self, result, *flags):
        proc, reqs = run_fake([_ok(result)], [*flags, "iso", "list"])
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual([r["method"] for r in reqs], ["iso.list"])
        self.assertNotIn("(null)", proc.stdout + proc.stderr)
        self.assertNotIn("CRITICAL", proc.stderr)
        return proc.stdout

    def test_plain_objects(self):
        out = self._run(OBJECTS, "--format=plain")
        self.assertEqual(out.splitlines(), ["ubuntu.iso", "nosize.iso"])

    def test_table_objects(self):
        out = self._run(OBJECTS)
        header = next(l for l in out.splitlines() if "FILENAME" in l)
        self.assertEqual(header.split(), ["FILENAME", "SIZE(MB)"])
        self.assertEqual(next(l for l in out.splitlines() if "ubuntu.iso" in l).split(), ["ubuntu.iso", "1024"])
        self.assertEqual(next(l for l in out.splitlines() if "nosize.iso" in l).split(), ["nosize.iso", "-"])

    def test_legacy_string_elements(self):
        self.assertEqual(self._run(["a.iso", "b.iso"], "--format=plain").splitlines(), ["a.iso", "b.iso"])
        out = self._run(["a.iso"])
        self.assertIn(" a.iso", out)

    def test_other_types_skipped(self):
        out = self._run([7, None, {"path": "x"}, {"name": "ok.iso"}], "--format=plain")
        self.assertEqual(out.splitlines(), ["ok.iso"])

    def test_json_passthrough(self):
        out = self._run(OBJECTS, "--format=json")
        self.assertIn("ubuntu.iso", out)


if __name__ == "__main__":
    unittest.main()
