#!/usr/bin/env python3
                          
                                                                                              
                                                                                      
                                                                       
 
                      
                                                                       
                                                        
                                                     
from __future__ import annotations

import contextlib
import importlib.util
import io
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("check_mainloop_reach", REPO / "scripts/check_mainloop_reach.py")
gate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(gate)

ROUTES = {"a.get": "h", "b.get": "other"}
EMPTY = "void h(void)\n{\n}\n"
SINK = "void h(void)\n{\n    g_usleep(1);\n}\n"
                                                        
OTHER = "void other(void)\n{\n    sleep(1);\n}\n"


class Base(unittest.TestCase):
    def setUp(self):
        self._td = tempfile.TemporaryDirectory()
        self.addCleanup(self._td.cleanup)
        self.root = Path(self._td.name)
        patcher = mock.patch.multiple(gate, MIN_FUNCTIONS=1, MIN_ROUTES=1, MIN_FILES=1, SENTINEL_FILES=(),
                                      SENTINEL_FUNCTIONS=(), REF_NOT_MAIN={})
        patcher.start()
        self.addCleanup(patcher.stop)

    def tree(self, text=EMPTY, routes=None, extra=None):
        files = {"src/h.c": text, "src/other.c": OTHER, **(extra or {})}
        gate._write_tree(self.root, files, routes or ROUTES)

    def run_gate(self, *args):
        out, err = io.StringIO(), io.StringIO()
        with mock.patch.object(sys, "argv", ["gate", "--root", str(self.root), *args]), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = gate.main()
        return rc, out.getvalue() + err.getvalue()

    def rows(self):
        return gate.collect(gate.Graph(self.root))[0]

    def route_rows(self, method="a.get"):
        return {r for r in self.rows() if r[1] == "route:" + method}

    @property
    def baseline(self):
        return self.root / "scripts/mainloop_reach_baseline.txt"


class RatchetTests(Base):
    def test_current_tree_matches_baseline(self):
        r = subprocess.run([sys.executable, str(REPO / "scripts/check_mainloop_reach.py")],
                           capture_output=True, text=True, check=False)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("PASS", r.stdout)

    def test_new_sink_fails_with_chain(self):
        self.tree()
        self.assertEqual(self.run_gate("--init-baseline")[0], 0)
        self.tree("static void helper(void)\n{\n    g_usleep(1);\n}\nvoid h(void)\n{\n    helper();\n}\n")
        rc, out = self.run_gate()
        self.assertEqual(rc, 1, out)
        self.assertIn("route:a.get\tsleep\thelper\tg_usleep", out)
        self.assertIn("helper <- h", out)

    def test_stale_baseline_fails(self):
        self.tree(SINK)
        self.assertEqual(self.run_gate("--init-baseline")[0], 0)
        self.tree()
        rc, out = self.run_gate()
        self.assertEqual(rc, 1, out)
        self.assertIn("낡은 기준선", out)

    def test_tighten_removes_stale(self):
        self.tree(SINK)
        self.assertEqual(self.run_gate("--init-baseline")[0], 0)
        self.tree()
        self.assertEqual(self.run_gate("--tighten")[0], 0)
        text = self.baseline.read_text()
        self.assertNotIn("route:a.get", text)
        self.assertIn("route:b.get", text)
        self.assertEqual(self.run_gate()[0], 0)

    def test_tighten_refuses_growth(self):
        self.tree()
        self.assertEqual(self.run_gate("--init-baseline")[0], 0)
        before = self.baseline.read_text()
        self.tree(SINK)
        rc, out = self.run_gate("--tighten")
        self.assertEqual(rc, 1, out)
        self.assertIn("--tighten은 줄이기만 한다", out)
        self.assertEqual(self.baseline.read_text(), before)

    def test_init_baseline_refuses_existing(self):
        self.tree(SINK)
        self.assertEqual(self.run_gate("--init-baseline")[0], 0)
        rc, out = self.run_gate("--init-baseline")
        self.assertEqual(rc, 2, out)
        self.assertIn("이미 있다", out)

    def test_rebaseline_requires_reason(self):
        self.tree(SINK)
        self.assertEqual(self.run_gate("--init-baseline")[0], 0)
        self.assertEqual(self.run_gate("--rebaseline", "")[0], 2)
        self.assertEqual(self.run_gate("--rebaseline", "   ")[0], 2)
        self.tree(SINK.replace("g_usleep", "usleep"))
        rc, out = self.run_gate("--rebaseline", "표지 이름 변경")
        self.assertEqual(rc, 0, out)
        self.assertIn("표지 이름 변경", out)
        self.assertIn("usleep", out)
        self.assertEqual(self.run_gate()[0], 0)

    def test_empty_baseline_rc2(self):
        self.tree(SINK)
        rc, out = self.run_gate("--baseline", "/dev/null")
        self.assertEqual(rc, 2, out)
        self.assertIn("비었습니다", out)


class EmptySetGuardTests(Base):
    def test_missing_sentinel_file_rc2(self):
        self.tree(SINK)
        with mock.patch.object(gate, "SENTINEL_FILES", ("src/없는파일.c",)):
            rc, out = self.run_gate("--report")
        self.assertEqual(rc, 2, out)
        self.assertIn("표지 파일 없음", out)

    def test_unresolved_route_rc2(self):
        self.tree(routes={"a.get": "없는_핸들러", "b.get": "other"})
        rc, out = self.run_gate("--report")
        self.assertEqual(rc, 2, out)
        self.assertIn("핸들러", out)

    def test_below_function_minimum_rc2(self):
        self.tree(SINK)
        with mock.patch.object(gate, "MIN_FUNCTIONS", 100000):
            rc, out = self.run_gate("--report")
        self.assertEqual(rc, 2, out)
        self.assertIn("하한", out)

    def test_ref_not_main_stale_rc2(self):
        self.tree(SINK)
        with mock.patch.object(gate, "REF_NOT_MAIN", {"사라진_함수": "근거"}):
            rc, out = self.run_gate("--report")
        self.assertEqual(rc, 2, out)
        self.assertIn("REF_NOT_MAIN", out)

    def test_no_rows_rc2(self):
        gate._write_tree(self.root, {"src/h.c": EMPTY}, {"a.get": "h"})
        rc, out = self.run_gate("--report")
        self.assertEqual(rc, 2, out)
        self.assertIn("도달 집합이 비었습니다", out)


class GraphRuleTests(Base):
    def test_macro_hidden_sink_found(self):
        self.tree("#define M() virt_conn_pool_acquire()\nvoid h(void)\n{\n    M();\n}\n")
        self.assertIn(("main", "route:a.get", "pool", "M", "virt_conn_pool_acquire"), self.rows())

    def test_include_c_found(self):
        self.tree('#include "h_part.c"\n')
        (self.root / "src/h_part.c").write_text(SINK)
        self.assertIn(("main", "route:a.get", "sleep", "h", "g_usleep"), self.rows())

    def test_thread_entry_not_followed(self):
        self.tree("static void w(void *d)\n{\n    g_usleep(1);\n}\nvoid h(void)\n{\n    g_task_run_in_thread(t, w);\n}\n")
        self.assertEqual(self.route_rows(), set())

    def test_thread_helper_param_not_followed(self):
        self.tree("static void _snap_run(GTask *t, GTaskThreadFunc fn)\n{\n    g_task_run_in_thread(t, fn);\n}\n"
                  "static void w(void *d)\n{\n    g_usleep(1);\n}\nvoid h(void)\n{\n    _snap_run(t, w);\n}\n")
        self.assertEqual(self.route_rows(), set())

    def test_completion_callback_followed(self):
        self.tree("static void cb(void *d)\n{\n    g_usleep(1);\n}\n"
                  "void h(void)\n{\n    pcv_drain_task_new(NULL, NULL, cb, NULL);\n}\n")
        self.assertIn(("main", "route:a.get", "sleep", "cb", "g_usleep"), self.rows())

    def test_rest_ctx_source_ignored(self):
        self.tree("static gboolean on_rest(void *d)\n{\n    g_usleep(1);\n}\nstatic gboolean on_main(void *d)\n{\n    sleep(1);\n}\n"
                  "void setup(void)\n{\n    pcv_drain_idle(ctx, on_rest, NULL);\n    pcv_drain_idle(NULL, on_main, NULL);\n}\n"
                  "void h(void)\n{\n}\n")
        rows = self.rows()
        self.assertFalse([r for r in rows if r[1] == "source:on_rest"])
        self.assertIn(("main", "source:on_main", "sleep", "on_main", "sleep"), rows)

    def test_comment_and_string_ignored(self):
        self.tree('/* g_usleep(1) */\nvoid h(void)\n{\n    // sleep(1)\n    s = "virDomainCreate(d)";\n}\n'
                  "void other(void)\n{\n    sleep(1);\n}\n", routes={"a.get": "h", "b.get": "other"})
        self.assertEqual(self.route_rows(), set())

    def test_lxc_receiver_rule(self):
        self.tree("void h(void)\n{\n    c->state(c);\n    r->destroy(r->data);\n}\n")
        self.assertEqual(self.route_rows(), {("main", "route:a.get", "lxc-method", "h", "->state")})

    def test_gvir_sink_found(self):
        self.tree("void h(void)\n{\n    gvir_connection_open(c, NULL, NULL);\n}\n")
        self.assertIn(("main", "route:a.get", "gvir", "h", "gvir_connection_open"), self.rows())

    def test_open_token_static_table_fails(self):
        self.tree(SINK, extra={"src/table.c": "static const Ops o = { .open = virConnectOpen };\n"})
        with mock.patch.object(gate, "OPEN_TOKEN_ENFORCED", False):
            self.assertEqual(self.run_gate("--init-baseline")[0], 0)
            self.assertEqual(self.run_gate()[0], 0, "규칙이 꺼져 있으면 같은 트리가 통과해야 한다")
        with mock.patch.object(gate, "OPEN_TOKEN_ENFORCED", True):
            rc, out = self.run_gate()
        self.assertEqual(rc, 1, out)
        self.assertIn("virConnectOpen은", out)
        self.assertIn("src/table.c", out)

    def _critical_case(self, body):
        self.tree(f"void h(void)\n{{\n    {body}\n}}\n")
        with mock.patch.object(gate, "CRITICAL_ZERO", False):
            self.assertEqual(self.run_gate("--init-baseline")[0], 0)
            self.assertEqual(self.run_gate()[0], 0, "규칙이 꺼져 있으면 같은 트리가 통과해야 한다")
        with mock.patch.object(gate, "CRITICAL_ZERO", True):
            rc, out = self.run_gate()
            self.assertEqual(rc, 1, out)
            self.assertIn("메인 도달 금지", out)
            rc2, out2 = self.run_gate("--rebaseline", "사유가 있어도 넘지 못한다")
            self.assertEqual(rc2, 1, out2)

    def test_critical_mutex_zero(self):
        self._critical_case("g_mutex_lock(&g_mutation_mu);")

    def test_critical_dns_zero(self):
        self._critical_case("getaddrinfo(a, b, c, d);")

    def test_real_tree_enforces_critical_zero(self):
        self.assertTrue(gate.CRITICAL_ZERO, "Task 15 뒤에는 g_mutation_mu·동기 DNS 0 규칙이 켜져 있어야 한다")

    def test_real_tree_enforces_open_token(self):
        self.assertTrue(gate.OPEN_TOKEN_ENFORCED, "Task 2 뒤에는 토큰 규칙이 켜져 있어야 한다(M12)")

    def test_open_token_allowed_in_open_file_and_comments(self):
        self.tree(SINK + "/* virConnectOpen 설명 */\n",
                  extra={gate.OPEN_TOKEN_FILE: "void f(void)\n{\n    virConnectOpen(u);\n}\n"})
        with mock.patch.object(gate, "OPEN_TOKEN_ENFORCED", True):
            self.assertEqual(gate.collect(gate.Graph(self.root))[3], [])


class OffloadRuleTests(Base):
    def lane(self, method, lane):
        p = self.root / "src/modules/dispatcher/rpc_offload.c"
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text('static const Row T[] = {\n    { "%s", PCV_OFFLOAD_LANE_%s },\n};\n' % (method, lane))

    def test_offload_route_dropped(self):
        self.tree("void h(void)\n{\n    virDomainCreate(d);\n}\n")
        self.assertTrue(self.route_rows())
        self.lane("a.get", "LIBVIRT_READ")
        self.assertEqual(self.route_rows(), set())

    def test_rest_defer_rendered_fn_not_walked(self):
                                                         
        for call, want in (("pcv_rest_defer_rendered(msg, ctx, render, NULL, NULL)", False), ("render(NULL)", True)):
            with self.subTest(call=call):
                self.tree(extra={"src/api/rest_server.c": "static void render(void *d)\n{\n    pcv_rest_rpc_over_uds(d);\n}\n"
                                 f"void _on_request(void)\n{{\n    {call};\n}}\n"})
                self.assertEqual(bool([r for r in self.rows() if r[0] == "rest"]), want)

    def test_rest_scope_has_no_sync_sinks(self):
                                                                                    
        base = (REPO / "scripts/mainloop_reach_baseline.txt").read_text().splitlines()
        self.assertEqual([ln for ln in base if ln.startswith("rest\t")], [])
        with mock.patch.multiple(gate, MIN_FUNCTIONS=3000, MIN_ROUTES=300, MIN_FILES=200):
            rows = gate.collect(gate.Graph(REPO))[0]
        self.assertEqual({r for r in rows if r[0] == "rest"}, set())

    def test_offload_commented_row_not_counted(self):
                                                             
        for text in ('/* { "a.get", PCV_OFFLOAD_LANE_LIBVIRT_READ }, */\n',
                     '// { "a.get", PCV_OFFLOAD_LANE_LIBVIRT_READ },\n',
                     '/* 설명:\n * { "a.get", PCV_OFFLOAD_LANE_LIBVIRT_READ },\n */\n'):
            with self.subTest(text=text):
                self.tree("void h(void)\n{\n    virDomainCreate(d);\n}\n")
                self.lane("b.get", "LIBVIRT_READ")
                table = self.root / "src/modules/dispatcher/rpc_offload.c"
                table.write_text(table.read_text() + text)
                self.assertEqual(self.route_rows(), {("main", "route:a.get", "libvirt", "h", "virDomainCreate")})

    def test_offload_main_only_api_fails(self):
        for api in ("pcv_poll_async_submit", "get_vm_metrics"):
            with self.subTest(api=api):
                self.tree(f"void {api}(void)\n{{\n}}\nvoid h(void)\n{{\n    {api}();\n}}\n")
                self.lane("a.get", "LIBVIRT_READ")
                rc, out = self.run_gate("--report")
                self.assertEqual(rc, 1, out)
                self.assertIn("메인 전용 API", out)
                self.assertIn(api, out)

    def test_host_read_guard_out_fails(self):
        self.tree("void h(void)\n{\n    pcv_spawn_sync_status(a);\n}\n")
        self.lane("a.get", "HOST_READ")
        rc, out = self.run_gate("--report")
        self.assertEqual(rc, 1, out)
        self.assertIn("가드 밖 spawn", out)

    def test_host_read_guarded_spawn_passes(self):
        self.tree("void h(void)\n{\n    pcv_spawn_sync(a);\n}\n")
        self.lane("a.get", "HOST_READ")
        self.assertEqual(self.run_gate("--report")[0], 0)

    def test_libvirt_read_spawn_fails(self):
        self.tree("void h(void)\n{\n    pcv_spawn_sync(a);\n}\n")
        self.lane("a.get", "LIBVIRT_READ")
        rc, out = self.run_gate("--report")
        self.assertEqual(rc, 1, out)
        self.assertIn("LIBVIRT_READ", out)

    def test_vm_device_mutate_spawn_fails(self):
                                                                       
        self.tree("void h(void)\n{\n    pcv_spawn_sync(a);\n}\n")
        self.lane("a.get", "VM_DEVICE_MUTATE")
        rc, out = self.run_gate("--report")
        self.assertEqual(rc, 1, out)
        self.assertIn("VM_DEVICE_MUTATE", out)


if __name__ == "__main__":
    unittest.main()
