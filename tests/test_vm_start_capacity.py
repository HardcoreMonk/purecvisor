#!/usr/bin/env python3









from __future__ import annotations

import hashlib
import os
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class VmStartCapacityTests(unittest.TestCase):


    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="pcv-start-cap-build-")
        cls.addClassCleanup(cls.build.cleanup)
        cls.binary = Path(cls.build.name) / "start-capacity"
        compiler = shutil.which("gcc-14") or shutil.which("cc")
        if not compiler:
            raise RuntimeError("C23 compiler required")
        flags = shlex.split(subprocess.check_output(
            ["pkg-config", "--cflags", "--libs", "gio-2.0", "libvirt", "libxml-2.0"], text=True))
        source = os.environ.get("PCV_VM_START_CAPACITY_SOURCE", "src/modules/virt/vm_start_capacity.c")
        extra = shlex.split(os.environ.get("PCV_VM_START_CAPACITY_CFLAGS", ""))
        command = [compiler, "-std=gnu23", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
                   "-g", "-Isrc", "-Isrc/modules/virt", source, "tests/fixtures/vm_start_capacity.c",
                   "-o", str(cls.binary), *flags, "-ldl", *extra]
        result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="pcv-start-cap-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.sysfs = self.root / "sys"
        self.xml = self.root / "domain.xml"
        self.effects = self.root / "effects.log"
        self.global_pool = self.sysfs / "kernel/mm/hugepages/hugepages-2048kB"
        self.node_pool = self.sysfs / "devices/system/node/node0/hugepages/hugepages-2048kB"
        self.make_pool()

    def make_pool(self, free=10, reserved=0):
        for path, values in ((self.global_pool, {"nr_hugepages": 20, "free_hugepages": free+10,
                                                "resv_hugepages": reserved, "surplus_hugepages": 0}),
                             (self.node_pool, {"nr_hugepages": 10, "free_hugepages": free,
                                              "surplus_hugepages": 0})):
            path.mkdir(parents=True, exist_ok=True)
            for name, value in values.items():
                (path / name).write_text(f"{value}\n")

    @staticmethod
    def domain(memory="8", unit="MiB", page="<page size='2048' unit='KiB'/>",
               numa="<numatune><memory mode='strict' nodeset='0'/></numatune>", extra=""):
        return (f"<domain><name>capacity-vm</name><memory unit='{unit}'>{memory}</memory>"
                f"<memoryBacking><hugepages>{page}</hugepages></memoryBacking>{numa}{extra}</domain>")

    def run_start(self, xml=None, mode="normal", expect_counter_change=False):
        self.xml.write_text(self.domain() if xml is None else xml)
        before = {str(p):hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in self.sysfs.rglob("*") if p.is_file()}
        result = subprocess.run([str(self.binary), str(self.sysfs), str(self.xml),
                                 str(self.effects), mode], cwd=ROOT,
                                capture_output=True, text=True, timeout=5)
        after = {str(p):hashlib.sha256(p.read_bytes()).hexdigest()
                 for p in self.sysfs.rglob("*") if p.is_file()}
        if not expect_counter_change:
            self.assertEqual(before, after)
        return result, self.effects.read_text().splitlines()

    def test_supported_shortage_has_no_prepare_or_create_effect(self):
        self.make_pool(free=3)
        result, effects = self.run_start()
        self.assertEqual(result.returncode, 1)
        self.assertEqual(effects, [])
        self.assertIn("hugepage", result.stdout)

    def test_sufficient_has_prepare_then_create_once(self):
        self.make_pool(free=4)
        result, effects = self.run_start()
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        self.assertEqual(effects, ["PREPARE 1", "CREATE 1"])

    def test_reserved_pages_reduce_available_capacity(self):
        self.make_pool(free=6, reserved=3)
        result, effects = self.run_start()
        self.assertEqual(result.returncode, 1)
        self.assertEqual(effects, [])

    def test_reserved_exceeding_free_is_zero_available(self):
        self.make_pool(free=10, reserved=15)
        result, effects = self.run_start()
        self.assertEqual(result.returncode, 1)
        self.assertEqual(effects, [])
        self.assertIn("available=0", result.stdout)

    def test_global_capacity_does_not_cover_bound_node(self):
        self.make_pool(free=1)
        result, effects = self.run_start()
        self.assertEqual(result.returncode, 1)
        self.assertEqual(effects, [])

    def test_consumption_during_prepare_blocks_create(self):
        result, effects = self.run_start(mode="grow", expect_counter_change=True)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(effects, ["PREPARE 1"])

    def test_counter_change_inside_snapshot_blocks_both_effects(self):
        result, effects = self.run_start(mode="counter-change", expect_counter_change=True)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(effects, [])
        self.assertIn("counter changed", result.stdout)

    def test_missing_counter_blocks_both_effects(self):
        (self.global_pool / "resv_hugepages").unlink()
        result, effects = self.run_start()
        self.assertEqual(result.returncode, 1)
        self.assertEqual(effects, [])

    def test_invalid_counter_blocks_both_effects(self):
        for value in ("-1", "8junk", "18446744073709551616"):
            with self.subTest(value=value):
                (self.node_pool / "free_hugepages").write_text(value)
                result, effects = self.run_start()
                self.assertEqual(result.returncode, 1)
                self.assertEqual(effects, [])

    def test_inconsistent_node_and_global_counts_block_effects(self):
        for target, value in ((self.node_pool / "free_hugepages", "11"),
                              (self.global_pool / "free_hugepages", "4")):
            with self.subTest(target=target):
                self.make_pool()
                target.write_text(value)
                result, effects = self.run_start()
                self.assertEqual(result.returncode, 1)
                self.assertEqual(effects, [])

    def test_odd_memory_rounds_up(self):
        self.make_pool(free=1)
        result, effects = self.run_start(self.domain(memory="3"))
        self.assertEqual(result.returncode, 1)
        self.assertEqual(effects, [])

    def test_bytes_and_decimal_megabytes_have_distinct_page_demand(self):
        self.make_pool(free=1)
        result, effects = self.run_start(self.domain(memory="2097152", unit="bytes"))
        self.assertEqual(result.returncode, 0)
        self.assertEqual(effects, ["PREPARE 1", "CREATE 1"])
        result, effects = self.run_start(self.domain(memory="2097153", unit="bytes"))
        self.assertEqual(result.returncode, 1)
        self.assertEqual(effects, [])
        self.make_pool(free=10)
        result, effects = self.run_start(self.domain(memory="21", unit="MB"))
        self.assertEqual(result.returncode, 1)
        self.assertEqual(effects, [])

    def test_unbound_memory_uses_global_capacity(self):
        self.make_pool(free=0)
        result, effects = self.run_start(self.domain(numa=""))
        self.assertEqual(result.returncode, 0)
        self.assertEqual(effects, ["PREPARE 1", "CREATE 1"])

    def test_normal_memfd_does_not_require_hugepage_counters(self):
        (self.global_pool / "resv_hugepages").unlink()
        xml = "<domain><memory unit='MiB'>8</memory><memoryBacking><source type='memfd'/></memoryBacking></domain>"
        result, effects = self.run_start(xml)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(effects, ["PREPARE 1", "CREATE 1"])

    def test_unsupported_page_shape_is_deferred(self):
        for xml in (self.domain(page="<page size='1' unit='GiB'/>"), self.domain(page=""),
                    self.domain(page="<page size='2048' nodeset='0'/>"),
                    self.domain(extra="<cpu><numa><cell id='0' memory='8192'/></numa></cpu>"),
                    self.domain(numa="<numatune><memory nodeset='0-1'/></numatune>"),
                    self.domain(numa="<numatune><memory mode='preferred' nodeset='0'/></numatune>"),
                    self.domain(extra="<vcpu placement='auto'>2</vcpu>"),
                    self.domain(extra="<devices><memory model='dimm'/></devices>")):
            with self.subTest(xml=xml):
                result, effects = self.run_start(xml)
                self.assertEqual(result.returncode, 0)
                self.assertEqual(effects, ["PREPARE 1", "CREATE 1"])
                self.assertIn("deferred", result.stderr)

    def test_managed_save_is_deferred_without_false_shortage(self):
        self.make_pool(free=0, reserved=20)
        result, effects = self.run_start(mode="managed-save")
        self.assertEqual(result.returncode, 0)
        self.assertEqual(effects, ["PREPARE 1", "CREATE 1"])
        self.assertIn("managed-save", result.stderr)

    def test_managed_save_query_failure_is_not_deferred(self):
        result, effects = self.run_start(mode="managed-error")
        self.assertEqual(result.returncode, 1)
        self.assertEqual(effects, [])

    def test_xml_and_memory_invalidity_blocks_effects(self):
        for xml in ("<domain>", self.domain(memory="-1"), self.domain(memory="0"),
                    self.domain(memory="18446744073709551615"), self.domain(unit="unknown"),
                    self.domain(extra="<memory>8192</memory>"),
                    "<!DOCTYPE domain [<!ENTITY value '8'>]>" + self.domain(memory="&value;")):
            with self.subTest(xml=xml):
                result, effects = self.run_start(xml)
                self.assertEqual(result.returncode, 1)
                self.assertEqual(effects, [])

    def test_xml_read_failure_blocks_effects(self):
        result, effects = self.run_start(mode="xml-error")
        self.assertEqual(result.returncode, 1)
        self.assertEqual(effects, [])

    def test_prepare_failure_blocks_create(self):
        result, effects = self.run_start(mode="prepare-error")
        self.assertEqual(result.returncode, 1)
        self.assertEqual(effects, ["PREPARE 1"])

    def test_invalid_prepare_result_is_terminal_with_error(self):
        for mode in ("prepare-no-error", "lost-domain"):
            with self.subTest(mode=mode):
                result, effects = self.run_start(mode=mode)
                self.assertEqual(result.returncode, 1)
                self.assertEqual(effects, ["PREPARE 1"])
                self.assertIn("ERROR", result.stdout)

    def test_post_prepare_xml_and_restore_query_are_checked(self):
        for mode in ("memory-change", "xml-lost", "managed-late-error"):
            with self.subTest(mode=mode):
                result, effects = self.run_start(mode=mode)
                self.assertEqual(result.returncode, 1)
                self.assertEqual(effects, ["PREPARE 1"])
                self.assertIn("ERROR", result.stdout)

    def test_native_failure_propagates_after_attempt(self):
        result, effects = self.run_start(mode="native-error")
        self.assertEqual(result.returncode, 1)
        self.assertEqual(effects, ["PREPARE 1", "CREATE 1"])
        self.assertIn("fixture native error", result.stdout)

    def test_preparer_replacement_domain_is_used(self):
        result, effects = self.run_start(mode="replace-domain")
        self.assertEqual(result.returncode, 0)
        self.assertEqual(effects, ["PREPARE 1", "CREATE 2"])


class VmStartCapacityWiringTests(unittest.TestCase):


    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="pcv-start-cap-wire-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        gate = "scripts/check_dpdk_owned_lifecycle.py"
        text = (ROOT / gate).read_text()
        for path in {gate, *re.findall(r'_read\("([^"]+)"\)', text)}:
            target = self.root / path
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / path, target)

    def gate(self):
        return subprocess.run(["python3", str(self.root / "scripts/check_dpdk_owned_lifecycle.py")],
                              cwd=self.root, capture_output=True, text=True, timeout=5)

    def mutate(self, path, old, new, count=1):
        file = self.root / path
        text = file.read_text()
        self.assertIn(old, text)
        file.write_text(text.replace(old, new, count))
        result = self.gate()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("FAIL:", result.stderr)

    def test_intact_product_wiring_passes(self):
        result = self.gate()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_missing_worker_controller_call_is_rejected(self):
        self.mutate("src/modules/dispatcher/handler_vm_start.c", "!pcv_vm_start_with_capacity(",
                    "!fixture_bypass_capacity(")

    def test_native_start_bypass_in_worker_is_rejected(self):
        self.mutate("src/modules/dispatcher/handler_vm_start.c",
                    "InactiveDpdkPrepare prepare =", "virDomainCreate(dom);\nInactiveDpdkPrepare prepare =")

    def test_inactive_dpdk_callback_removal_is_rejected(self):
        self.mutate("src/modules/dispatcher/handler_vm_start.c",
                    "return _reconcile_dpdk_vhost_for_start(", "return fixture_skip_dpdk(")

    def test_post_prepare_check_removal_is_rejected(self):
        self.mutate("src/modules/virt/vm_start_capacity.c",
                    "if (!check_domain(*domain_io, sysfs_root, error)) return FALSE;\n    if (virDomainCreate",
                    "/* missing post-prepare check */\n    if (virDomainCreate")

    def test_active_capacity_readmission_is_rejected(self):
        self.mutate("src/modules/dispatcher/handler_vm_start.c", "if (domain_active) {",
                    "if (domain_active) {\npcv_vm_start_with_capacity(&dom, \"/sys\", NULL, NULL, &error);")


if __name__ == "__main__":
    unittest.main()
