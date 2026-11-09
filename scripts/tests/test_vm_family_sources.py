#!/usr/bin/env python3
                          
                                                                                                  
                                              
                                                                 
                                                  
                                                                    
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
sys.path.insert(0, str(ROOT / "scripts" / "split"))
import c_blocks              
import rpc_extract              
import split_target              

CONFIGS = ("vm_hotplug", "vm_lifecycle", "vm_manager")
FLOORS = {"vm_hotplug": 32, "vm_lifecycle": 66, "vm_manager": 70}


def _tsv(rel: str) -> list[tuple[str, str]]:
    rows = []
    for line in (ROOT / rel).read_text().splitlines():
        if line.strip() and not line.startswith("#"):
            a, b = line.split("\t")
            rows.append((a, b))
    return rows


class VmFamilySources(unittest.TestCase):
    def test_source_comes_first_and_split_files_follow_config_order(self):
        for config in CONFIGS:
            t = split_target.load(config)
            self.assertEqual(rpc_extract.vm_family_rel(config), [t.source, *t.split_files])
            fam = [p.relative_to(ROOT).as_posix() for p in rpc_extract.vm_family(config)]
            self.assertEqual(fam[0], t.source)
            self.assertEqual(fam[1:], [r for r in t.split_files if (ROOT / r).exists()])

    def test_family_defines_every_original_function(self):
        for config in CONFIGS:
            names = [f.name for f in c_blocks.find_functions(rpc_extract.vm_family_text(config))]
            self.assertEqual(len(names), FLOORS[config], config)
            self.assertEqual(split_target.load(config).extra["min_functions"], FLOORS[config], config)

    def test_function_floor_rejects_empty_read(self):
        saved = rpc_extract.vm_family
        rpc_extract.vm_family = lambda config, root=ROOT: []
        try:
            for config in CONFIGS:
                with self.assertRaises(ValueError):
                    rpc_extract.vm_family_text(config)
        finally:
            rpc_extract.vm_family = saved

    def test_temp_root_reads_only_present_files_without_floor(self):
        with tempfile.TemporaryDirectory() as tmp:
            r = Path(tmp)
            source = split_target.load("vm_lifecycle").source
            (r / source).parent.mkdir(parents=True)
            shutil.copy(ROOT / source, r / source)
            self.assertEqual([p.name for p in rpc_extract.vm_family("vm_lifecycle", r)], ["handler_vm_lifecycle.c"])
            self.assertIn("==== " + source, rpc_extract.vm_family_text("vm_lifecycle", r))

    def test_missing_source_raises(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(FileNotFoundError):
                rpc_extract.vm_family("vm_manager", Path(tmp))

    def test_strict_mode_rejects_missing_split_file_at_repo_root(self):
        saved_flag, saved_rel = rpc_extract.VM_SPLIT_COMPLETE, rpc_extract.vm_family_rel
        rpc_extract.VM_SPLIT_COMPLETE = True
        rpc_extract.vm_family_rel = lambda config: [split_target.load(config).source, "src/modules/virt/vm_no_such_split_file.c"]
        try:
            with self.assertRaises(FileNotFoundError):
                rpc_extract.vm_family("vm_manager")
        finally:
            rpc_extract.VM_SPLIT_COMPLETE, rpc_extract.vm_family_rel = saved_flag, saved_rel

    def test_manifest_function_defined_exactly_once_in_its_family(self):
        renames = dict(_tsv("scripts/split/vm_family_rename_map.tsv"))
        back = {new: old for old, new in renames.items()}
        targets = {Path(r).stem: c for c in CONFIGS for r in split_target.load(c).split_files}
        names = {c: [f.name for f in c_blocks.find_functions(rpc_extract.vm_family_text(c))] for c in CONFIGS}
        for fn, target in _tsv("scripts/split/vm_family_manifest.tsv"):
            self.assertIn(target, targets)
            found = names[targets[target]]
            self.assertEqual(found.count(fn) + found.count(back.get(fn, "\0")), 1, fn)

    def test_rename_map_targets_are_prefixed(self):
        for old, new in _tsv("scripts/split/vm_family_rename_map.tsv"):
            self.assertTrue(new.startswith(("pcv_disp_", "pcv_vm_")), new)


if __name__ == "__main__":
    unittest.main()
