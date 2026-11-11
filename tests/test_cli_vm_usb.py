#!/usr/bin/env python3
                          
                                                                                                                     
                                                                                            
                                                                               
                                                                                                               
                                                                                           
                                                                                
 
                                                                                   
                                                  
                         
from __future__ import annotations

import json
import os
import socket
import subprocess
import tempfile
import threading
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CLI = ROOT / "bin" / "pcvctl"


def _ok(result):
    return {"jsonrpc": "2.0", "result": result, "id": 1}


def run_fake(responses, args):
                                                              
    with tempfile.TemporaryDirectory(prefix="pcv-cli-usb-") as temp:
        path = os.path.join(temp, "daemon.sock")
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        server.bind(path)
        server.listen(max(1, len(responses)))
        server.settimeout(0.1)
        requests, stop = [], threading.Event()

        def serve():
            index = 0
            while not stop.is_set() and index < len(responses):
                try:
                    conn, _ = server.accept()
                except socket.timeout:
                    continue
                with conn:
                    data = b""
                    while True:
                        chunk = conn.recv(8192)
                        if not chunk:
                            break
                        data += chunk
                    requests.append(json.loads(data.decode()))
                    conn.sendall(json.dumps(responses[index]).encode())
                    index += 1

        thread = threading.Thread(target=serve, daemon=True)
        thread.start()
        proc = subprocess.run([str(CLI), f"--socket={path}", "--no-color", *args], text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10, check=False)
        stop.set()
        thread.join(timeout=2)
        server.close()
        assert not thread.is_alive(), "fake UDS server did not stop"
        return proc, requests


class CliVmUsb(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not os.access(CLI, os.X_OK):
            raise AssertionError("missing bin/pcvctl; run make cli first")

    def test_attach_sends_startup_policy(self):     
        proc, reqs = run_fake([_ok({"attached": True})],
                              ["vm", "usb-attach", "v", "0x1234", "0x5678", "--startup-policy", "mandatory"])
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual([r["method"] for r in reqs], ["vm.usb.attach"])
        self.assertEqual(reqs[0]["params"], {"vm_id": "v", "vendor_id": "0x1234", "product_id": "0x5678",
                                             "startup_policy": "mandatory"})

    def test_attach_without_flag_sends_no_policy(self):     
        proc, reqs = run_fake([_ok({"attached": True})], ["vm", "usb-attach", "v", "0x1234", "0x5678"])
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(reqs[0]["params"], {"vm_id": "v", "vendor_id": "0x1234", "product_id": "0x5678"})

    def test_attach_usage_errors_send_nothing(self):     
        for extra in (["--startup-policy"], ["--bogus", "x"], ["extra"]):
            with self.subTest(extra=extra):
                proc, reqs = run_fake([], ["vm", "usb-attach", "v", "0x1234", "0x5678", *extra])
                self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)
                self.assertEqual(reqs, [])
                self.assertIn("Usage: pcvctl vm usb-attach <name> <vendor_id> <product_id> "
                              "[--startup-policy mandatory|requisite|optional]", proc.stdout)
        proc, reqs = run_fake([], ["vm", "usb-attach", "v", "0x1234"])
        self.assertEqual((proc.returncode, reqs), (2, []))

    def test_success_summary_line(self):     
                                                                                            
                                                                           
        result = {"attached": True, "vendor_id": "0x1234", "product_id": "0x5678",
                  "live": True, "config": True, "startup_policy": "optional"}
        proc, _ = run_fake([_ok(result)], ["vm", "usb-attach", "v", "0x1234", "0x5678"])
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("changed: live=yes, persistent=yes · startup_policy: optional", proc.stdout)
        self.assertNotIn("live: ", proc.stdout)
        detach = {"detached": True, "vendor_id": "0x1234", "product_id": "0x5678", "live": False, "config": True}
        proc, reqs = run_fake([_ok(detach)], ["vm", "usb-detach", "v", "0x1234", "0x5678"])
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(reqs[0]["method"], "vm.usb.detach")
        self.assertIn("changed: live=no, persistent=yes · startup_policy: -", proc.stdout)
        self.assertNotIn("live: ", proc.stdout)
                                        
        proc, _ = run_fake([_ok(result)], ["--format=json", "vm", "usb-attach", "v", "0x1234", "0x5678"])
        self.assertEqual(json.loads(proc.stdout)["result"], result)

    def test_list_columns(self):     
        rows = [{"vendor_id": "0x1234", "product_id": "0x5678", "live": True, "persistent": True,
                 "startup_policy": "optional", "missing": False},
                {"vendor_id": "0x248a", "product_id": "0x8327", "live": True, "persistent": False,
                 "startup_policy": "mandatory", "missing": True},
                {"vendor_id": "0x1038", "product_id": "0x22ac", "live": False, "persistent": True,
                 "startup_policy": "requisite", "missing": False}]
        proc, reqs = run_fake([_ok(rows)], ["vm", "usb-list", "v"])
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(reqs[0]["method"], "vm.usb.list")
        lines = proc.stdout.splitlines()
        header = next(l for l in lines if "VENDOR_ID" in l)
        self.assertEqual(header.split(), ["VENDOR_ID", "PRODUCT_ID", "LIVE", "PERSISTENT", "POLICY"])
        self.assertIn(["0x1234", "0x5678", "yes", "yes", "optional"], [l.split() for l in lines])
        self.assertIn(["0x248a", "0x8327", "missing", "no", "mandatory"], [l.split() for l in lines])
        self.assertIn(["0x1038", "0x22ac", "no", "yes", "requisite"], [l.split() for l in lines])
        for fmt in ("plain", "csv"):
            with self.subTest(fmt=fmt):
                proc, _ = run_fake([_ok(rows)], [f"--format={fmt}", "vm", "usb-list", "v"])
                self.assertEqual(proc.returncode, 0, proc.stderr)
                self.assertEqual(proc.stdout.splitlines(), [
                    "0x1234,0x5678,true,true,optional,false",
                    "0x248a,0x8327,true,false,mandatory,true",
                    "0x1038,0x22ac,false,true,requisite,false",
                ])

    def test_server_rejection_exits_1(self):     
        err = {"jsonrpc": "2.0", "id": 1, "error": {"code": -32602,
               "message": "startup_policy must be one of: mandatory, requisite, optional"}}
        proc, reqs = run_fake([err], ["vm", "usb-attach", "v", "0x1234", "0x5678", "--startup-policy", "Optional"])
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertEqual(reqs[0]["params"]["startup_policy"], "Optional")                    
        self.assertNotIn("changed:", proc.stdout)

    def test_live_only_sends_persistent_false(self):     
        result = {"attached": True, "vendor_id": "0x1234", "product_id": "0x5678",
                  "live": True, "config": False, "startup_policy": None}
        proc, reqs = run_fake([_ok(result)], ["vm", "usb-attach", "v", "0x1234", "0x5678", "--live-only"])
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual([r["method"] for r in reqs], ["vm.usb.attach"])
        self.assertEqual(reqs[0]["params"], {"vm_id": "v", "vendor_id": "0x1234", "product_id": "0x5678",
                                             "persistent": False})
        self.assertIs(reqs[0]["params"]["persistent"], False)                                         
        self.assertIn("changed: live=yes, persistent=no · startup_policy: -", proc.stdout)

    def test_live_only_with_policy_sends_both(self):     
                                                                                                        
        err = {"jsonrpc": "2.0", "id": 1, "error": {"code": -32602,
               "message": "startup_policy applies only when persistent is true"}}
        for flags in (["--live-only", "--startup-policy", "optional"], ["--startup-policy", "optional", "--live-only"]):
            with self.subTest(flags=flags):
                proc, reqs = run_fake([err], ["vm", "usb-attach", "v", "0x1234", "0x5678", *flags])
                self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
                self.assertEqual(reqs[0]["params"], {"vm_id": "v", "vendor_id": "0x1234", "product_id": "0x5678",
                                                     "startup_policy": "optional", "persistent": False})

    def test_no_flag_omits_persistent(self):           
        for flags in ([], ["--startup-policy", "requisite"]):
            with self.subTest(flags=flags):
                proc, reqs = run_fake([_ok({"attached": True})], ["vm", "usb-attach", "v", "0x1234", "0x5678", *flags])
                self.assertEqual(proc.returncode, 0, proc.stderr)
                self.assertNotIn("persistent", reqs[0]["params"])

    def test_live_only_unknown_suffix_is_usage(self):     
        for extra in (["--live-only=x"], ["--live-only=false"], ["--live-only", "extra"]):
            with self.subTest(extra=extra):
                proc, reqs = run_fake([], ["vm", "usb-attach", "v", "0x1234", "0x5678", *extra])
                self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)
                self.assertEqual(reqs, [])
                self.assertIn("Usage: pcvctl vm usb-attach <name> <vendor_id> <product_id> "
                              "[--startup-policy mandatory|requisite|optional] [--live-only]", proc.stdout)


if __name__ == "__main__":
    unittest.main()
