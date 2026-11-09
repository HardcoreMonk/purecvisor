#!/usr/bin/env python3
                          
                                                               
                                                                               
                      
                                                  
                                             
                                     
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class StartAdmissionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="pcv-admission-build-")
        cls.addClassCleanup(cls.build.cleanup)
        cls.binary = Path(cls.build.name) / "admission"
        flags = shlex.split(subprocess.check_output(
            ["pkg-config", "--cflags", "--libs", "gio-2.0", "libvirt", "libxml-2.0"], text=True))
        source = os.environ.get("PCV_VM_START_CAPACITY_SOURCE", "src/modules/virt/vm_start_capacity.c")
        command = [shutil.which("gcc-14") or "cc", "-std=gnu23", "-D_GNU_SOURCE",
                   "-Wall", "-Wextra", "-Werror", "-g", "-O1", "-Isrc", "-Isrc/modules/virt", source,
                   "tests/fixtures/vm_start_admission.c", "-o", str(cls.binary), *flags,
                   *shlex.split(os.environ.get("PCV_VM_START_CAPACITY_CFLAGS", ""))]
        result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

    def run_mode(self, mode):
        with tempfile.TemporaryDirectory(prefix="pcv-admission-case-") as directory:
            pool = Path(directory) / "kernel/mm/hugepages/hugepages-2048kB"
            pool.mkdir(parents=True)
            for name, value in {"nr_hugepages": 10, "free_hugepages": 10,
                                "resv_hugepages": 0, "surplus_hugepages": 0}.items():
                (pool / name).write_text(str(value) + "\n")
            command = [*shlex.split(os.environ.get("PCV_VM_START_CAPACITY_PREFIX", "")),
                       str(self.binary), directory, mode]
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PASS " + mode, result.stdout)
            print(result.stdout.strip(), flush=True)
            if os.environ.get("PCV_VM_START_CAPACITY_PREFIX"):
                print(result.stderr, flush=True)

    def test_competitor_rejected_before_prepare(self): self.run_mode("prepare")
    def test_guard_held_during_native_create(self): self.run_mode("native")
    def test_prepare_failure_releases_admission(self): self.run_mode("prepare-failure")
    def test_native_failure_releases_admission(self): self.run_mode("native-failure")
    def test_ordinary_memory_can_progress(self): self.run_mode("ordinary")
    def test_prepare_enabling_hugepages_requires_admission(self): self.run_mode("change")
    def test_retry_reads_consumption_again(self): self.run_mode("consume")


def mutations():
                                                           
    original = (ROOT / "src/modules/virt/vm_start_capacity.c").read_text()
    checks = "if (!check_domain(*domain_io, sysfs_root, error, &admission)) return FALSE;"
    variants = {
        "unlock-before-prepare": (checks, checks + "\n    admission_clear(&admission);"),
        "unlock-before-create": ("if (virDomainCreate(*domain_io) < 0)",
                                 "admission_clear(&admission);\n    if (virDomainCreate(*domain_io) < 0)"),
        "missing-release": ("g_auto(StartAdmission) admission = {0};", "StartAdmission admission = {0};"),
        "ordinary-admitted-too": (checks, "if (!admission_claim(&admission, error)) return FALSE;\n    " + checks),
    }
    with tempfile.TemporaryDirectory(prefix="pcv-admission-mutations-") as directory:
        for name, (old, new) in variants.items():
            if old not in original:
                raise AssertionError(f"mutation anchor missing: {name}")
            source = Path(directory) / (name + ".c")
            source.write_text(original.replace(old, new, 1))
            env = dict(os.environ, PCV_VM_START_CAPACITY_SOURCE=str(source))
            result = subprocess.run([sys.executable, __file__], cwd=ROOT, env=env,
                                    capture_output=True, text=True, timeout=45)
            if result.returncode == 0 or "FAILED (failures=" not in result.stderr:
                raise AssertionError(f"not rejected by runtime assertions: {name}\n{result.stdout}{result.stderr}")
            print(f"REJECT {name}: runtime effect assertions, exit={result.returncode}", flush=True)


if __name__ == "__main__":
    if sys.argv[1:] == ["--mutations"]:
        mutations()
    else:
        unittest.main(verbosity=2)
