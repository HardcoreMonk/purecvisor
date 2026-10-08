#!/usr/bin/env python3








import importlib.util
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("trace_gate", ROOT / "scripts/check_trace_reap_lifetime.py")
GATE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GATE)
TRACE = "src/modules/daemons/pcv_trace.c"
RPC = "src/api/dispatcher.c"


class TraceWiringTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="pcv-trace-wiring-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        for name in (TRACE, RPC):
            target = self.root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text((ROOT / name).read_text())

    def reject(self, path, before, after, reason):
        target = self.root / path
        text = target.read_text()
        self.assertEqual(text.count(before), 1, "counterexample must target one production statement")
        target.write_text(text.replace(before, after))
        with self.assertRaisesRegex(ValueError, reason):
            GATE.check(self.root)

    def test_current_source_passes(self):
        GATE.check(self.root)

    def test_stop_cannot_finalize_directly(self):
        self.reject(TRACE, '_trace_request_stop("debug.trace.stop", "stop");',
                    '_trace_finalize("debug.trace.stop", "stop");', "retain completion")

    def test_signal_boundary_cannot_release_guard(self):
        self.reject(TRACE, "if (g_trace->proc) g_subprocess_force_exit(g_trace->proc);",
                    "if (g_trace->proc) g_subprocess_force_exit(g_trace->proc); pcv_trace_release();",
                    "retain completion")

    def test_failed_wait_cannot_authorize_completion(self):
        self.reject(TRACE, "if (reaped && g_trace && g_trace->proc == proc &&",
                    "if (g_trace && g_trace->proc == proc &&", "successful wait")

    def test_other_process_cannot_authorize_completion(self):
        self.reject(TRACE, "g_trace->proc == proc &&", "TRUE &&", "matching process")

    def test_comment_cannot_replace_trace_rpc_ack(self):
        self.reject(RPC, 'json_object_set_boolean_member(res, "stop_requested", TRUE);',
                    '/* json_object_set_boolean_member(res, "stop_requested", TRUE); */',
                    "request ACK field stop_requested")


if __name__ == "__main__":
    unittest.main()
