#!/usr/bin/env python3









import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MODES = ("private-forward", "default-forward", "tcp-eof", "tcp-hup", "late-close",
         "private-ws-first", "default-ws-first", "simultaneous", "closing-then-tcp", "repeat")


def compile_fixture(binary, source):
    flags = shlex.split(subprocess.check_output(
        ["pkg-config", "--cflags", "--libs", "libsoup-3.0", "gio-2.0", "json-glib-1.0"], text=True))
    command = [os.environ.get("CC", "gcc-14"), "-std=gnu23", "-D_GNU_SOURCE", "-DPCV_CLUSTER_ENABLED=0",
               "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-g", "-O1",
               "-ffunction-sections", "-fdata-sections", "-I.", "-Isrc", "-Isrc/api", "-Iinclude",
               "-Iinclude/purecvisor", f'-DPCV_VNC_SOURCE="{source}"',
               "tests/fixtures/vnc_proxy_lifetime.c", "-Wl,--gc-sections",
               "-Wl,--wrap=soup_websocket_connection_close", "-Wl,--wrap=read", "-o", str(binary), *flags,
               *shlex.split(os.environ.get("PCV_VNC_TEST_CFLAGS", ""))]
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=90)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)


class VncProxyLifetime(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="pcv-vnc-")
        cls.binary = Path(cls.temp.name) / "vnc"
        compile_fixture(cls.binary, os.environ.get("PCV_VNC_TEST_SOURCE", "src/api/ws_server.c"))

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_mode(self, mode):
        command = [*shlex.split(os.environ.get("PCV_VNC_TEST_PREFIX", "")), str(self.binary), mode]
        result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(f"PASS {mode}:", result.stdout)
        print(result.stdout.strip(), flush=True)
        if os.environ.get("PCV_VNC_TEST_PREFIX"):
            print(result.stderr, flush=True)

    def test_private_context_binary_relay(self): self.run_mode("private-forward")
    def test_default_context_binary_relay(self): self.run_mode("default-forward")
    def test_tcp_eof_detaches_before_close_callout(self): self.run_mode("tcp-eof")
    def test_tcp_hup_detaches_before_close_callout(self): self.run_mode("tcp-hup")
    def test_async_closed_and_late_message(self): self.run_mode("late-close")
    def test_private_websocket_first(self): self.run_mode("private-ws-first")
    def test_default_websocket_first(self): self.run_mode("default-ws-first")
    def test_simultaneous_close(self): self.run_mode("simultaneous")
    def test_tcp_eof_during_websocket_close(self): self.run_mode("closing-then-tcp")
    def test_repeated_connections(self): self.run_mode("repeat")

    def test_rest_context_and_source_ownership_wiring(self):

        source = (ROOT / os.environ.get("PCV_VNC_TEST_SOURCE", "src/api/ws_server.c")).read_text()
        source = re.sub(r"/\*.*?\*/|//[^\n]*", "", source, flags=re.S)
        connected = source[source.index("static void\n_on_vnc_connected("):source.index("void\npcv_ws_server_init(")]
        self.assertNotIn("g_io_add_watch(", connected)
        self.assertIn("g_main_context_ref_thread_default()", connected)
        self.assertIn("g_source_attach(vp->tcp_watch, context)", connected)
        self.assertIn("g_io_channel_set_close_on_unref(vp->tcp_chan, FALSE)", connected)
        cleanup = source[source.index("static void\n_vnc_proxy_free("):source.index("static void\n_vnc_proxy_close(")]
        self.assertLess(cleanup.index("g_signal_handlers_disconnect_by_data"), cleanup.index("g_free(vp)"))
        self.assertLess(cleanup.index("g_source_destroy"), cleanup.index("close(vp->tcp_fd)"))
        self.assertIn("g_clear_pointer(&vp->tcp_watch, g_source_unref)", cleanup)
        rest = (ROOT / "src/api/rest_server.c").read_text()
        self.assertIn("g_main_context_push_thread_default(self->rest_ctx)", rest)
        self.assertIn("pcv_ws_server_init(self->soup)", rest)

    def test_gate_is_mandatory(self):
        makefile = (ROOT / "Makefile").read_text()
        all_gates = next(line for line in makefile.splitlines() if line.startswith("check-all:")).split()[1:]
        self.assertIn("check-vnc-proxy-lifetime", all_gates)
        self.assertIn("check-server-defect-contracts: check-vnc-proxy-lifetime", makefile)
        self.assertIn("@python3 tests/test_vnc_proxy_lifetime.py", makefile)


def mutations():

    source = (ROOT / "src/api/ws_server.c").read_text()
    variants = {
        "keep-signal-handlers": ("g_signal_handlers_disconnect_by_data(vp->ws, vp);", "(void)vp->ws;", "late-close"),
        "global-default-watch": ("g_source_attach(vp->tcp_watch, context);", "g_source_attach(vp->tcp_watch, NULL);", "private-forward"),
        "keep-tcp-source": ("g_source_destroy(vp->tcp_watch);", "/* mutation: keep source */", "private-ws-first"),
        "keep-tcp-fd": ("close(vp->tcp_fd);", "/* mutation: keep FD */", "private-ws-first"),
        "skip-ws-cleanup": ("PCV_LOG_INFO(WS_LOG_DOM, \"VNC WebSocket closed\");\n    _vnc_proxy_free(vp);",
                            "PCV_LOG_INFO(WS_LOG_DOM, \"VNC WebSocket closed\");\n    (void)vp;", "default-ws-first"),
        "close-before-cleanup": ("_vnc_proxy_free(vp);\n    if (soup_websocket_connection_get_state(ws)",
                                 "/* mutation: cleanup after callout */\n    if (soup_websocket_connection_get_state(ws)", "tcp-eof"),
    }
    with tempfile.TemporaryDirectory(prefix="pcv-vnc-mutants-") as directory:
        for name, (old, new, mode) in variants.items():
            assert mode in MODES
            if source.count(old) != 1:
                raise AssertionError(f"{name}: expected one semantic anchor, got {source.count(old)}")
            mutated = source.replace(old, new)
            if name == "close-before-cleanup":
                anchor = "g_object_unref(ws);\n}"
                if mutated.count(anchor) != 1:
                    raise AssertionError("close helper end must be unique")
                mutated = mutated.replace(anchor, "_vnc_proxy_free(vp);\n    " + anchor)
            path = Path(directory) / f"{name}.c"
            path.write_text(mutated)
            binary = Path(directory) / name
            compile_fixture(binary, path)
            result = subprocess.run([str(binary), mode], cwd=ROOT, capture_output=True, text=True, timeout=10)
            if result.returncode == 0:
                raise AssertionError(f"surviving semantic mutant: {name}")
            if not re.search(r"assertion failed|AddressSanitizer: heap-use-after-free", result.stderr):
                raise AssertionError(f"mutant lacks a valid failure oracle: {name}\n{result.stderr}")
            oracle = next(line.strip() for line in result.stderr.splitlines()
                          if "assertion failed" in line or "AddressSanitizer: heap-use-after-free" in line)
            print(f"REJECT {name}: exit {result.returncode}, {mode}\n  {oracle}", flush=True)


if __name__ == "__main__":
    if sys.argv[1:] == ["--mutations"]:
        mutations()
    else:
        unittest.main(verbosity=2)
