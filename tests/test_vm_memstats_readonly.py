#!/usr/bin/env python3

from __future__ import annotations

import os
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
from test_cli_vm_usb import CLI, _ok, run_fake

HANDLER = ROOT / "src/modules/dispatcher/handler_vm_hotplug.c"

def _strip_comments(src: str) -> str:
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    return re.sub(r"//[^\n]*", "", src)

def _function_body(src: str, name: str) -> str:
    m = re.search(r"\b" + re.escape(name) + r"\s*\([^)]*\)\s*\{", src)
    if not m:
        raise AssertionError(f"{name} not found")
    depth, i = 1, m.end()
    while depth:
        c = src[i]
        depth += c == "{"
        depth -= c == "}"
        i += 1
    return src[m.end():i - 1]

class MemStatsHandlerReadOnly(unittest.TestCase):
    def setUp(self):
        self.body = _function_body(_strip_comments(HANDLER.read_text(encoding="utf-8")),
                                   "handle_vm_memory_stats_request")

    def test_no_libvirt_write_calls(self):
        writes = re.findall(r"\bvirDomainSet\w+|\bvirDomain(?:Define|Update|Attach|Detach)\w*", self.body)
        self.assertEqual(writes, [], "vm.memory.stats 는 조회다 — VM 설정을 바꾸는 libvirt 호출 금지")

    def test_reads_stats_and_reports_guest_flag(self):
        self.assertIn("virDomainMemoryStats(", self.body)
        self.assertRegex(self.body, r'json_object_set_boolean_member\(obj,\s*"guest_stats"')

FULL = {"vm": "vm-a", "guest_stats": True, "actual_balloon_kb": 2097152, "rss_kb": 1048576,
        "unused_kb": 0, "available_kb": 2000000, "usable_kb": 1500000, "swap_in": 0, "swap_out": 0}
HOST_ONLY = {"vm": "vm-a", "guest_stats": False, "actual_balloon_kb": 2097152, "rss_kb": 1048576}

class CliMemStats(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not os.access(CLI, os.X_OK):
            raise AssertionError("missing bin/pcvctl; run make cli first")

    def _run(self, result, *flags):
        proc, reqs = run_fake([_ok(result)], [*flags, "vm", "memory-stats", "vm-a"])
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual([r["method"] for r in reqs], ["vm.memory.stats"])
        self.assertNotIn("CRITICAL", proc.stderr)
        return proc.stdout

    @staticmethod
    def _rows(out):
        return {l.split("\t")[0]: l.split("\t")[1] for l in out.splitlines() if "\t" in l}

    def test_plain_missing_is_dash_and_zero_is_zero(self):
        rows = self._rows(self._run(HOST_ONLY, "--format=plain"))
        self.assertEqual(rows.get("Actual Balloon (KB)"), "2097152")
        for k in ("Unused (KB)", "Available (KB)", "Usable (KB)", "Swap In", "Swap Out"):
            self.assertEqual(rows.get(k), "-", (k, rows))
        rows = self._rows(self._run(FULL, "--format=plain"))
        self.assertEqual(rows.get("Unused (KB)"), "0")
        self.assertEqual(rows.get("Swap In"), "0")

    def test_table_notes_missing_guest_stats(self):
        out = self._run(HOST_ONLY)
        self.assertIn("게스트 메모리 통계 없음", out)
        self.assertNotIn("게스트 메모리 통계 없음", self._run(FULL))
        legacy = dict(HOST_ONLY)
        legacy.pop("guest_stats")
        self.assertNotIn("게스트 메모리 통계 없음", self._run(legacy))

if __name__ == "__main__":
    unittest.main()
