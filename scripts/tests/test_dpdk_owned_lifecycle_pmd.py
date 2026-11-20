#!/usr/bin/env python3

from __future__ import annotations

import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GATE = "scripts/check_dpdk_owned_lifecycle.py"
PMD = "src/modules/network/dpdk_pmd.c"
MANAGER = "src/modules/network/dpdk_manager.c"
START = "src/modules/dispatcher/handler_vm_start.c"

class PmdAffinityGateCounterfactuals(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="pcv-pmd-gate-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        text = (ROOT / GATE).read_text()
        sys.path.insert(0, str(ROOT / "scripts"))
        import rpc_extract
        files = {p.relative_to(ROOT).as_posix() for p in rpc_extract.dispatcher_family(ROOT)}
        for config in ("vm_manager", "vm_lifecycle"):
            files |= {p.relative_to(ROOT).as_posix() for p in rpc_extract.vm_family(config)}
        files |= {GATE, "scripts/rpc_extract.py", "scripts/split/split_target.py", "scripts/split/c_blocks.py",
                  "scripts/split/targets/dispatcher.toml", "scripts/split/targets/vm_manager.toml",
                  "scripts/split/targets/vm_lifecycle.toml", *re.findall(r'_read\("([^"]+)"\)', text)}
        if (ROOT / rpc_extract.RENAME_MAP_REL).exists():
            files.add(rpc_extract.RENAME_MAP_REL)
        for path in files:
            target = self.root / path
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / path, target)

    def gate(self):
        return subprocess.run(["python3", str(self.root / GATE)], cwd=self.root,
                              capture_output=True, text=True, timeout=10)

    def mutate(self, path, old, new, message):
        file = self.root / path
        text = file.read_text()
        self.assertEqual(text.count(old), 1, f"{path}: {old!r}")
        file.write_text(text.replace(old, new))
        result = self.gate()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn(message, result.stderr)

    def test_intact_tree_passes(self):
        result = self.gate()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_affinity_write_before_ownership_rejected(self):

        self.mutate(PMD, "        pcv_dpdk_pmd_choose(&eff, port, cur, prev, markers, plan);",
                    "        plan->action = PCV_PMD_APPLY;", "must follow the ownership decision")

    def test_choose_without_ownership_rejected(self):
        self.mutate(PMD, "    gboolean owned = pcv_dpdk_pmd_affinity_owned(cur_aff, prev_marker);",
                    "    gboolean owned = has_aff;", "judge ownership")

    def test_active_branch_with_input_rejected(self):
        self.mutate(START, "canonical_name, TRUE, NULL, pcv_drain_task_actor(task)",
                    "canonical_name, TRUE, &(PcvDpdkPmdInput){ 0 }, pcv_drain_task_actor(task)",
                    "PMD input NULL")

    def test_create_restore_wrapper_with_input_rejected(self):
        self.mutate(MANAGER, "PCV_DPDK_VHOST_ENDPOINT_CANONICAL, NULL, NULL, error);",
                    "PCV_DPDK_VHOST_ENDPOINT_CANONICAL, &(PcvDpdkPmdInput){ 0 }, NULL, error);",
                    "NULL PMD input")

    def test_prepare_callback_swallowing_failure_rejected(self):

        self.mutate(START, "    pcv_dpdk_pmd_input_clear(&pmd);\n    return ok;",
                    "    pcv_dpdk_pmd_input_clear(&pmd);\n    return TRUE;", "must return the reconcile result")

    def test_appctl_in_vm_start_prepare_rejected(self):
        self.mutate(START, "        g_autoptr(GArray) pmd_cores = pcv_dpdk_pmd_cache_snapshot(&pmd_state);",
                    "        pcv_dpdk_pmd_cache_refresh();\n"
                    "        g_autoptr(GArray) pmd_cores = pcv_dpdk_pmd_cache_snapshot(&pmd_state);",
                    "main-loop _vm_start_prepare must not reach pcv_dpdk_pmd_cache_refresh")

    def test_appctl_in_dpdk_init_rejected(self):
        self.mutate(MANAGER, "        pcv_dpdk_pmd_cache_init_from_mask();",
                    "        pcv_dpdk_pmd_cache_refresh();", "pcv_dpdk_init must not reach pcv_dpdk_pmd_cache_refresh")

    def test_gmu_in_cache_function_rejected(self):
        self.mutate(PMD, "    GArray *copy = _cache_union_locked();",
                    "    GArray *copy = _cache_union_locked(); /* G.mu */ (void)\"\";\n"
                    "    pcv_dpdk_ovs_run_locked(NULL, NULL, NULL, NULL);",
                    "must not use pcv_dpdk_ovs_run_locked")

    def test_get_without_if_exists_rejected(self):
        self.mutate(PMD, '"ovs-vsctl", "--timeout=10", "--if-exists", "get", "Interface", iface, column, NULL',
                    '"ovs-vsctl", "--timeout=10", "get", "Interface", iface, column, NULL',
                    "--if-exists (H1)")

    def test_global_open_vswitch_write_rejected(self):
        self.mutate(PMD, '    const gchar *show[] = {"ovs-appctl", "-T", "2", "dpif-netdev/pmd-rxq-show", NULL};',
                    '    const gchar *show[] = {"ovs-appctl", "-T", "2", "dpif-netdev/pmd-rxq-show", NULL};\n'
                    '    const gchar *bad[] = {"ovs-vsctl", "set", "Open_vSwitch", ".", '
                    '"other_config:pmd-rxq-isolate=false", NULL}; (void)bad;',
                    "must not write global Open_vSwitch settings")

if __name__ == "__main__":
    unittest.main()
