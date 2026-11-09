#!/usr/bin/env python3
                          
                                                                                  
                                                    
                                                      
 
                      
                                                   
                                      
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import rpc_extract              

ROUTE = '    g_hash_table_insert(g_rpc_routes, "a.b{n}", (gpointer)h{n});\n'


def _tmp_root(files: dict) -> tempfile.TemporaryDirectory:
    d = tempfile.TemporaryDirectory()
    for rel, text in files.items():
        p = Path(d.name) / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)
    return d


class SourcesTest(unittest.TestCase):
    def setUp(self):
        self._saved = rpc_extract.DISPATCHER_SPLIT_FILES

    def tearDown(self):
        rpc_extract.DISPATCHER_SPLIT_FILES = self._saved

    def test_repo_family_starts_with_route_file(self):
        fam = rpc_extract.dispatcher_family(ROOT)
        self.assertEqual(fam[0], ROOT / rpc_extract.DISPATCHER_ROUTES_REL)
        self.assertEqual(len(fam), 1 + len(rpc_extract.DISPATCHER_SPLIT_FILES))
        self.assertEqual(rpc_extract.dispatcher_route_file(ROOT), ROOT / "src/api/dispatcher.c")
        self.assertIn(rpc_extract.dispatcher_policy_file(ROOT), fam)

    def test_route_floor_rejects_unreadable_source(self):
        with _tmp_root({"src/api/dispatcher.c": ROUTE.format(n=1)}) as d:
            with self.assertRaises(ValueError):
                rpc_extract.dispatcher_route_file(Path(d))

    def test_policy_file_must_be_unique(self):
        pol = "static const PcvMethodPolicy g_method_policies[] = {\n};\n"
        rpc_extract.DISPATCHER_SPLIT_FILES = ("src/api/dispatcher_policy.c",)
        with _tmp_root({"src/api/dispatcher.c": pol, "src/api/dispatcher_policy.c": pol}) as d:
            with self.assertRaises(ValueError):
                rpc_extract.dispatcher_policy_file(Path(d))
        with _tmp_root({"src/api/dispatcher.c": "int x;\n", "src/api/dispatcher_policy.c": pol}) as d:
            self.assertEqual(rpc_extract.dispatcher_policy_file(Path(d)).name, "dispatcher_policy.c")

    def test_missing_split_file_in_repo_root_raises(self):
        rpc_extract.DISPATCHER_SPLIT_FILES = ("src/api/no_such_split_file.c",)
        with self.assertRaises(FileNotFoundError):
            rpc_extract.dispatcher_family(ROOT)

    def test_current_fn_name_follows_rename_map(self):
        files = {"src/api/dispatcher.c": "void handle_x(void)\n{\n}\n",
                 "scripts/split/dispatcher_rename_map.tsv": "_handle_x\thandle_x\n"}
        with _tmp_root(files) as d:
            self.assertEqual(rpc_extract.current_fn_name(Path(d), "_handle_x"), "handle_x")
            self.assertEqual(rpc_extract.current_fn_name(Path(d), "handle_x"), "handle_x")


if __name__ == "__main__":
    unittest.main()
