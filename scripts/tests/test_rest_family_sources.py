#!/usr/bin/env python3
                                                                                     
                                                                  
import shutil, sys, tempfile, unittest
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import rpc_extract              


class RestFamilySources(unittest.TestCase):
    def test_repo_family_starts_with_route_file(self):
        fam = rpc_extract.rest_family()
        self.assertEqual(fam[0].relative_to(ROOT).as_posix(), "src/api/rest_server.c")

    def test_missing_split_file_in_temp_root_is_skipped(self):
        with tempfile.TemporaryDirectory() as tmp:
            r = Path(tmp)
            (r / "src/api").mkdir(parents=True)
            shutil.copy(ROOT / "src/api/rest_server.c", r / "src/api/rest_server.c")
            self.assertEqual([p.name for p in rpc_extract.rest_family(r)], ["rest_server.c"])

    def test_current_fn_name_follows_rest_map(self):
                                                  
        got = rpc_extract.current_fn_name(ROOT, "_send_json", config="rest_server")
        self.assertIn(got, ("_send_json", "pcv_rest_send_json"))
        self.assertIsNotNone(rpc_extract.function_body_in(rpc_extract.rest_family_text(), got))

    def test_route_floor(self):
        import extract_rest_routes
        self.assertGreaterEqual(len({(m, u, r) for m, u, r, _ in extract_rest_routes.extract()}), 213)


                                                                           
def _write(root: Path, files: dict) -> None:
    for rel, text in files.items():
        (root / rel).parent.mkdir(parents=True, exist_ok=True)
        (root / rel).write_text(text)


class RestFamilyDetails(unittest.TestCase):
    def test_split_files_follow_route_file_in_config_order(self):
        with tempfile.TemporaryDirectory() as tmp:
            r = Path(tmp)
            _write(r, {"src/api/rest_server.c": "int a;\n", "src/api/rest_request_auth.c": "int c;\n",
                       "src/api/rest_response.c": "int b;\n", "src/api/rest_unrelated.c": "int d;\n"})
            self.assertEqual([p.name for p in rpc_extract.rest_family(r)],
                             ["rest_server.c", "rest_response.c", "rest_request_auth.c"])
            text = rpc_extract.rest_family_text(r)
            self.assertIn("/* ==== src/api/rest_response.c ==== */\nint b;", text)
            self.assertNotIn("int d;", text)

    def test_missing_route_file_raises(self):
        with tempfile.TemporaryDirectory() as tmp:
            _write(Path(tmp), {"src/api/rest_response.c": "int b;\n"})
            with self.assertRaises(FileNotFoundError):
                rpc_extract.rest_family(Path(tmp))

    def test_complete_mode_rejects_missing_split_file_only_in_repo_root(self):
        saved_flag, saved_split = rpc_extract.REST_SPLIT_COMPLETE, rpc_extract.split_files
        rpc_extract.REST_SPLIT_COMPLETE = True
        rpc_extract.split_files = lambda config="dispatcher": ("src/api/rest_no_such_split_file.c",)
        try:
            with self.assertRaises(FileNotFoundError):
                rpc_extract.rest_family()
            with tempfile.TemporaryDirectory() as tmp:
                _write(Path(tmp), {"src/api/rest_server.c": "int a;\n"})
                self.assertEqual(len(rpc_extract.rest_family(Path(tmp))), 1)
        finally:
            rpc_extract.REST_SPLIT_COMPLETE, rpc_extract.split_files = saved_flag, saved_split

    def test_repo_strict_mode_is_on_after_stage1(self):
                                                                      
                                                                      
        self.assertTrue(rpc_extract.REST_SPLIT_COMPLETE)
        fam = [p.relative_to(ROOT).as_posix() for p in rpc_extract.rest_family()]
        self.assertEqual(fam, ["src/api/rest_server.c", *rpc_extract.split_files("rest_server")])
        saved = rpc_extract.split_files
        rpc_extract.split_files = lambda config="dispatcher": (*saved(config), "src/api/rest_missing_after_stage1.c")
        try:
            with self.assertRaises(FileNotFoundError):
                rpc_extract.rest_family()
        finally:
            rpc_extract.split_files = saved

    def test_function_body_in_ignores_calls_prototypes_and_literal_braces(self):
        text = ("static void _send_json(SoupServerMessage *msg, guint status, const gchar *body);\n"
                "void caller(void)\n{\n    _send_json(msg, 200, \"{\");\n}\n"
                "static void\n_send_json(SoupServerMessage *msg, guint status, const gchar *body)\n"
                "{\n    const gchar *s = \"}\"; /* } */ // }\n    char c = '}';\n    if (s) { use(c); }\n}\n"
                "int after;\n")
        body = rpc_extract.function_body_in(text, "_send_json")
        self.assertIsNotNone(body)
        self.assertTrue(body.startswith("{\n    const gchar *s") and body.endswith("use(c); }\n}"), body)
        self.assertIsNone(rpc_extract.function_body_in("    _send_json(msg, 200, x);\n", "_send_json"))

    def test_rest_fn_name_in_prefers_defined_name_then_rename_map(self):
        new_def = "void\npcv_rest_send_json(SoupServerMessage *msg)\n{\n}\n"
        old_def = "static void\n_send_json(SoupServerMessage *msg)\n{\n}\n"
        self.assertEqual(rpc_extract.rest_fn_name_in(old_def, "_send_json"), "_send_json")
        self.assertEqual(rpc_extract.rest_fn_name_in(new_def, "_send_json"), "pcv_rest_send_json")
        self.assertEqual(rpc_extract.rest_fn_name_in("", "_error"), "pcv_rest_send_error")
        self.assertEqual(rpc_extract.rest_fn_name_in("", "_on_request"), "_on_request")
        with tempfile.TemporaryDirectory() as tmp:
            _write(Path(tmp), {"src/api/rest_server.c": "int a;\n", "src/api/rest_response.c": new_def})
            self.assertEqual(rpc_extract.current_fn_name(Path(tmp), "_send_json", config="rest_server"),
                             "pcv_rest_send_json")

    def test_route_floor_failure_exits_nonzero(self):
        import contextlib, io
        import extract_rest_routes
                                                                                            
        src = next(p for p in rpc_extract.rest_family() if p.name == "rest_map_compute.c")
        text = src.read_text()
        start = text.index("const gchar *sub = rq->sub;")
        cut = text.index("pcv_rest_build_rpc", start)
        cut = text.index("\n", text.index("pcv_rest_build_rpc", cut + 1)) + 1
        saved = extract_rest_routes.ROOT
        with tempfile.TemporaryDirectory() as tmp:
            _write(Path(tmp), {"src/api/rest_server.c": text[:cut] + "cleanup:\n"})
            extract_rest_routes.ROOT = Path(tmp)
            err = io.StringIO()
            try:
                with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(err):
                    with self.assertRaises(SystemExit) as cm:
                        extract_rest_routes.main()
            finally:
                extract_rest_routes.ROOT = saved
        self.assertEqual(cm.exception.code, 1)
        self.assertIn("하한 213", err.getvalue())


                                                                                             
class RestChainFiles(unittest.TestCase):
    CHAIN = ("src/api/rest_map_compute.c", "src/api/rest_map_observe.c", "src/api/rest_map_platform.c")

    def test_config_lists_three_chain_files_in_source_order(self):
        import split_target
        t = split_target.load("rest_server")
        self.assertEqual(t.chain_files, self.CHAIN)
        self.assertTrue(all(f.startswith("src/api/rest_") and f.endswith(".c") for f in t.chain_files))

    def test_family_reads_only_existing_chain_files(self):
                                                                                   
                                                                            
        saved = rpc_extract.split_files
        rpc_extract.split_files = lambda config="dispatcher": (
            *(f for f in saved(config) if f not in self.CHAIN), *self.CHAIN)                        
        try:
            with tempfile.TemporaryDirectory() as tmp:
                _write(Path(tmp), {"src/api/rest_server.c": "int a;\n", "src/api/rest_map_platform.c": "int p;\n"})
                fam = [p.name for p in rpc_extract.rest_family(Path(tmp))]
        finally:
            rpc_extract.split_files = saved
        self.assertEqual(fam, ["rest_server.c", "rest_map_platform.c"])

    def test_route_scan_follows_existing_chain_files(self):
                                                                           
        import extract_rest_routes
        with tempfile.TemporaryDirectory() as tmp:
            _write(Path(tmp), {"src/api/rest_server.c": "int a;\n", "src/api/rest_map_observe.c": "int o;\n",
                               "src/api/rest_map_compute.c": "int c;\n"})
            got = [(p.name, until) for p, until in extract_rest_routes.scan_files(Path(tmp))]
        self.assertEqual(got, [("rest_server.c", True), ("rest_map_compute.c", False), ("rest_map_observe.c", False)])


class RestFamilyFinal(unittest.TestCase):
                                                                                                                                                     
    EXPECTED = ("rest_server", "rest_policy", "rest_response", "rest_request", "rest_async", "rest_transport",
                "rest_request_auth", "rest_auth_endpoints", "rest_public", "rest_public_render", "rest_login", "rest_prelude",
                "rest_map_platform", "rest_map_observe", "rest_map_compute")

    def test_family_is_fifteen_files_in_pinned_order(self):
        fam = [p.relative_to(ROOT).as_posix() for p in rpc_extract.rest_family()]
        self.assertEqual(fam, [f"src/api/{n}.c" for n in self.EXPECTED])

    def test_chain_files_are_all_in_split_files(self):
        import split_target
        chain = split_target.load("rest_server").chain_files
        self.assertEqual(len(chain), 3)
        self.assertTrue(set(chain) <= set(rpc_extract.split_files("rest_server")))


if __name__ == "__main__":
    unittest.main()
