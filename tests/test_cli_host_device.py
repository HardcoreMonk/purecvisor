#!/usr/bin/env python3
                          
                                                                                                              
                                                                                                             
                                                                                                         
                                                                                       
                                                                                                          
                                                                                                 
                                                                      
                                                                       
                                                                                
 
                                                                                     
                                                                                  
                                    
from __future__ import annotations

import csv
import io
import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
from test_cli_vm_usb import CLI, _ok, run_fake              


def _asg(vm, *, visible=True, active=True, live=False, persistent=False, **extra):
                                            
    base = {"vm": vm if visible else None, "vm_visible": visible, "vm_active": active,
            "live": live, "persistent": persistent}
    base.update(extra)
    return base


def _usb(dev_id, name, *, group="usb", present=True, hidden=False, blocked=None, assignments=()):
    vid, pid = dev_id.split(":")
    reason = blocked or (None if present else "not_present")
    return {"kind": "usb", "group": group, "id": dev_id, "present": present, "name": name,
            "vendor_name": None, "vendor_id": f"0x{vid}", "product_id": f"0x{pid}",
            "hidden_by_default": hidden, "assignable": reason is None,
            "blocked_reason": reason, "assignments": list(assignments)}


def _gpu(addr, name, driver, *, host_display=False, partial=False, blocked=None, companions=(), assignments=()):
    reason = "host_display" if host_display else blocked
    return {"kind": "pci", "group": "gpu", "id": addr, "present": True, "name": name, "vendor_name": None,
            "pci_addr": addr, "driver": driver, "host_display": host_display, "partial_assignment": partial,
            "hidden_by_default": False, "assignable": reason is None, "blocked_reason": reason,
            "companions": list(companions), "assignments": list(assignments)}


def _comp(addr, name, assignments=()):
                                                 
    return {"pci_addr": addr, "class_code": "040300", "name": name, "driver": "vfio-pci", "assignments": list(assignments)}


USB_KW = {"missing": False, "startup_policy": "mandatory", "stale_live_address": False, "ambiguous": False}

                                                            
DEVICES = [
    _gpu("0000:01:00.0", "NVIDIA Corporation GA104 [GeForce RTX 3070 Ti]", "vfio-pci",
         companions=[_comp("0000:01:00.1", "NVIDIA Corporation GA104 High Definition Audio Controller",
                           [_asg("win-guest1", live=True, persistent=True, managed="no")])],
         assignments=[_asg("win-guest1", live=True, persistent=True, managed="no")]),
    _gpu("0000:03:00.0", "Advanced Micro Devices, Inc. [AMD/ATI] Raphael", "amdgpu", host_display=True),
    _usb("8087:800a", "Intel Corp. Hub", hidden=True, blocked="hub"),
    _usb("248a:8327", "Telink Wireless Receiver",
         assignments=[_asg("ops-a", active=False, persistent=True, **USB_KW),
                      _asg("hidden-vm", visible=False, live=True, **USB_KW)]),
    _usb("0781:5581", "SanDisk Corp. Ultra", hidden=True),
    _usb("1038:22ac", None, present=False,
         assignments=[_asg("win-guest1", live=True, persistent=True,
                           **{**USB_KW, "startup_policy": "optional", "stale_live_address": True})]),
    _usb("8087:0029", "Intel Corp. AX200 Bluetooth", group="bluetooth"),
]
RESULT = {"devices": DEVICES, "vms": [{"name": "win-guest1", "active": True, "persistent": True}],
          "sources": {"usb": "available", "pci": "available", "names": "available"}, "truncated": False}


def _row(stdout, dev_id, assigned=None):
                                                                                    
                                               
    rows = [" ".join(l.split()) for l in stdout.splitlines()]
    rows = [l for l in rows if f" {dev_id} " in f" {l} " and (assigned is None or f" {assigned} " in l)]
    if len(rows) != 1:
        raise AssertionError(f"{dev_id}/{assigned}: {len(rows)} rows in\n{stdout}")
    return rows[0]


class CliHostDevices(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not CLI.exists():
            raise AssertionError("missing bin/pcvctl; run make cli first")

    def test_sends_host_device_list(self):                             
        proc, reqs = run_fake([_ok(RESULT)], ["host", "devices"])
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual([(r["method"], r["params"]) for r in reqs], [("host.device.list", {})])
        header = next(l for l in proc.stdout.splitlines() if "GROUP" in l)
        self.assertEqual(header.split(), ["GROUP", "ID", "NAME", "ASSIGNED", "STATE"])
        gpu = _row(proc.stdout, "0000:01:00.0")
        self.assertEqual(gpu.split()[0], "gpu")
        self.assertIn(" win-guest1 ", gpu)
        self.assertTrue(gpu.rstrip().endswith(" live;persistent;driver=vfio-pci"), gpu)
        igpu = _row(proc.stdout, "0000:03:00.0")
        self.assertIn(" host ", igpu)
        self.assertTrue(igpu.rstrip().endswith(" host-display;driver=amdgpu"), igpu)
        bt = _row(proc.stdout, "8087:0029")
        self.assertEqual(bt.split()[0], "bluetooth")
        self.assertTrue(bt.rstrip().endswith(" host -"), bt)
                            
        empty = dict(RESULT, devices=[], vms=[])
        proc, _ = run_fake([_ok(empty)], ["host", "devices"])
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn(" (no host devices)", proc.stdout.splitlines())

    def test_hidden_rows_need_all(self):                                            
        proc, reqs = run_fake([_ok(RESULT)], ["host", "devices"])
        self.assertEqual(proc.returncode, 0, proc.stderr)
        for dev_id in ("8087:800a", "0781:5581"):
            self.assertNotIn(dev_id, proc.stdout)
        self.assertIn("248a:8327", proc.stdout)
        proc, reqs = run_fake([_ok(RESULT)], ["host", "devices", "--all"])
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(reqs[0]["params"], {})                                    
        hub = _row(proc.stdout, "8087:800a")
        self.assertTrue(hub.rstrip().endswith(" host blocked=hub"), hub)                    
        self.assertIn("0781:5581", proc.stdout)
                                    
        only_hidden = dict(RESULT, devices=[DEVICES[2], DEVICES[4]])
        proc, _ = run_fake([_ok(only_hidden)], ["host", "devices"])
        self.assertIn(" (no host devices)", proc.stdout.splitlines())

    def test_json_passthrough(self):                                            
        for extra in ([], ["--all"]):
            with self.subTest(extra=extra):
                proc, reqs = run_fake([_ok(RESULT)], ["--format=json", "host", "devices", *extra])
                self.assertEqual(proc.returncode, 0, proc.stderr)
                self.assertEqual(reqs[0]["method"], "host.device.list")
                self.assertEqual(json.loads(proc.stdout)["result"], RESULT)

    def test_csv_columns(self):                                                 
        expected = [
            ["gpu", "0000:01:00.0", "NVIDIA Corporation GA104 [GeForce RTX 3070 Ti]", "win-guest1",
             "live;persistent;driver=vfio-pci"],
            ["gpu", "0000:03:00.0", "Advanced Micro Devices, Inc. [AMD/ATI] Raphael", "host", "host-display;driver=amdgpu"],
            ["usb", "8087:800a", "Intel Corp. Hub", "host", "blocked=hub"],
            ["usb", "248a:8327", "Telink Wireless Receiver", "ops-a", "persistent;vm-off"],
            ["usb", "248a:8327", "Telink Wireless Receiver", "(other VM)", "live;live-only"],
            ["usb", "0781:5581", "SanDisk Corp. Ultra", "host", "-"],
            ["usb", "1038:22ac", "-", "win-guest1", "live;persistent;stale-address;not-on-host"],
            ["bluetooth", "8087:0029", "Intel Corp. AX200 Bluetooth", "host", "-"],
        ]
        proc, _ = run_fake([_ok(RESULT)], ["--format=csv", "host", "devices", "--all"])
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn('"Advanced Micro Devices, Inc. [AMD/ATI] Raphael"', proc.stdout)
        self.assertEqual(list(csv.reader(io.StringIO(proc.stdout))),
                         [["group", "id", "name", "assigned", "state"], *expected])
        proc, _ = run_fake([_ok(RESULT)], ["--format=plain", "host", "devices", "--all"])
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual([l.split("\t") for l in proc.stdout.splitlines()], expected)
        proc, _ = run_fake([_ok(RESULT)], ["--format=csv", "host", "devices"])                  
        self.assertEqual([r[1] for r in csv.reader(io.StringIO(proc.stdout))][1:],
                         ["0000:01:00.0", "0000:03:00.0", "248a:8327", "248a:8327", "1038:22ac", "8087:0029"])

    def test_stale_row_tokens(self):                                                                
        proc, _ = run_fake([_ok(RESULT)], ["host", "devices"])
        self.assertEqual(proc.returncode, 0, proc.stderr)
        line = _row(proc.stdout, "1038:22ac")
        self.assertTrue(line.rstrip().endswith(" win-guest1 live;persistent;stale-address;not-on-host"), line)
                                                             
        tokens = [
            (_asg("v", live=True, **{**USB_KW, "missing": True}), True, "live;live-only;placeholder"),
            (_asg("v", live=True, persistent=True, **{**USB_KW, "ambiguous": True}), True, "live;persistent;ambiguous"),
            (_asg("v", persistent=True, **USB_KW), True, "persistent;attach-now"),
            (_asg("v", active=False, persistent=True, **USB_KW), True, "persistent;vm-off"),
            (_asg("v", persistent=True, **USB_KW), False, "persistent;not-on-host"),
        ]
        for asg, present, want in tokens:
            with self.subTest(want=want):
                res = dict(RESULT, devices=[_usb("1234:5678", "Dev", present=present, assignments=[asg])])
                proc, _ = run_fake([_ok(res)], ["--format=csv", "host", "devices"])
                self.assertEqual(list(csv.reader(io.StringIO(proc.stdout)))[1][3:], ["v", want])

    def test_masked_assignment_shows_other_vm(self):                                             
        proc, _ = run_fake([_ok(RESULT)], ["host", "devices"])
        self.assertEqual(proc.returncode, 0, proc.stderr)
        mine = _row(proc.stdout, "248a:8327", "ops-a")
        self.assertTrue(mine.rstrip().endswith(" ops-a persistent;vm-off"), mine)
        other = _row(proc.stdout, "248a:8327", "(other VM)")
        self.assertTrue(other.rstrip().endswith(" (other VM) live;live-only"), other)
        self.assertNotIn("hidden-vm", proc.stdout)

    def test_unknown_arg_is_usage(self):                                                        
        for extra in (["--json"], ["--al"], ["--all", "--all"], ["extra"], ["--all=yes"]):
            with self.subTest(extra=extra):
                proc, reqs = run_fake([], ["host", "devices", *extra])
                self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)
                self.assertEqual(reqs, [])
                self.assertIn("Usage: pcvctl host devices [--all]", proc.stdout)

    def test_server_error_exit_1(self):                             
        err = {"jsonrpc": "2.0", "id": 1, "error": {"code": -32000, "message": "cannot read a VM definition"}}
        proc, reqs = run_fake([err], ["host", "devices"])
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertEqual(reqs[0]["method"], "host.device.list")
        self.assertIn("cannot read a VM definition", proc.stderr)
        self.assertNotIn("GROUP", proc.stdout)

    def test_extra_state_tokens(self):                                                                 
        gone_pci = {"kind": "pci", "group": "gpu", "id": "0000:02:00.0", "present": False, "name": None,
                    "vendor_name": None, "hidden_by_default": False, "assignable": False, "blocked_reason": "not_present",
                    "pci_addr": "0000:02:00.0",
                    "assignments": [_asg("v", active=False, persistent=True, managed="yes")]}
        a = "0000:01:00.0"
        cases = [
            (_gpu(a, "G", "vfio-pci", partial=True, assignments=[_asg("v", live=True, persistent=True, managed="no")]),
             "live;persistent;driver=vfio-pci;partial"),
            (_gpu(a, "G", "vfio-pci", assignments=[_asg("v", active=False, persistent=True, managed="yes")]),
             "persistent;driver=vfio-pci;vm-off;managed=yes"),
            (_gpu(a, "G", "nvidia", blocked="not_vfio_bound"), "driver=nvidia;blocked=not_vfio_bound"),
            (_gpu(a, "G", "vfio-pci", blocked="iommu_group_unsafe"), "driver=vfio-pci;blocked=iommu_group_unsafe"),
            (_gpu(a, "G", None, host_display=True), "host-display;driver=-"),                                         
            (_usb("1234:5678", "U", blocked="duplicate_on_host"), "blocked=duplicate_on_host"),
            (_usb("1234:5678", "U", present=False, assignments=[_asg("v", persistent=True, **USB_KW)]),
             "persistent;not-on-host"),                                                                                
            (gone_pci, "persistent;not-on-host;vm-off"),                                                          
        ]
        for dev, want in cases:
            with self.subTest(want=want):
                proc, _ = run_fake([_ok(dict(RESULT, devices=[dev]))], ["--format=csv", "host", "devices"])
                self.assertEqual(proc.returncode, 0, proc.stderr)
                self.assertEqual(list(csv.reader(io.StringIO(proc.stdout)))[1][4], want)

    def test_companion_lines(self):                                                                 
        win = _asg("win", live=True, persistent=True, managed="no")
        gpu = _gpu("0000:01:00.0", "G", "vfio-pci", partial=True, assignments=[win], companions=[
            _comp("0000:01:00.1", "HDA", [win]),
            _comp("0000:01:00.2", "USB Ctl"),
            _comp("0000:01:00.3", None, [_asg("vm-b", active=False, persistent=True, managed="yes"),
                                         _asg("x", visible=False, live=True, persistent=True, managed="no")]),
        ])
        two = _gpu("0000:05:00.0", "H", "vfio-pci", companions=[_comp("0000:05:00.1", "HDA2", [
            _asg("a", active=False, persistent=True, managed="no"), _asg("b", active=False, persistent=True, managed="no")])],
            assignments=[_asg("a", active=False, persistent=True, managed="no"),
                         _asg("b", active=False, persistent=True, managed="no")])
        res = dict(RESULT, devices=[gpu, two, DEVICES[-1]])
        proc, _ = run_fake([_ok(res)], ["host", "devices"])
        self.assertEqual(proc.returncode, 0, proc.stderr)
        lines = proc.stdout.splitlines()
        i = next(k for k, l in enumerate(lines) if " 0000:01:00.0 " in l)
        self.assertEqual(lines[i + 1:i + 4], [
            "  + 0000:01:00.1 HDA",
            "  + 0000:01:00.2 USB Ctl [host]",
            "  + 0000:01:00.3 - [vm-b persistent;vm-off;managed=yes] [(other VM) live;persistent]",
        ])
        j = max(k for k, l in enumerate(lines) if " 0000:05:00.0 " in l)                         
        self.assertEqual(" ".join(lines[j].split()[-2:]), "b persistent;driver=vfio-pci;vm-off")
        self.assertEqual(lines[j + 1], "  + 0000:05:00.1 HDA2")
        self.assertTrue(" ".join(lines[j + 2].split()).startswith("bluetooth 8087:0029 "), lines[j + 2])
                                        
        for fmt in ("csv", "plain"):
            with self.subTest(fmt=fmt):
                proc, _ = run_fake([_ok(res)], [f"--format={fmt}", "host", "devices"])
                self.assertNotIn("+ 0000:01:00.", proc.stdout)
                self.assertEqual(len(proc.stdout.splitlines()), 5 if fmt == "csv" else 4)

    def test_notes_below_table(self):                                                     
        notes = ["(truncated: showing 7 devices — full list: pcvctl --format=json host devices --all)",
                 "(host USB device list not available on this host)",
                 "(host PCI device list not available on this host)",
                 "(device name database unavailable — IDs only)"]
        res = dict(RESULT, truncated=True, sources={"usb": "absent", "pci": "absent", "names": "unavailable"})
        for fmt in ("table", "plain"):
            with self.subTest(fmt=fmt):
                proc, _ = run_fake([_ok(res)], [f"--format={fmt}", "host", "devices"])
                self.assertEqual(proc.returncode, 0, proc.stderr)
                self.assertEqual(proc.stdout.splitlines()[-4:], notes)                           
        proc, _ = run_fake([_ok(res)], ["--format=csv", "host", "devices", "--all"])
        self.assertFalse([l for l in proc.stdout.splitlines() if l.startswith("(")], proc.stdout)
        self.assertEqual(len(list(csv.reader(io.StringIO(proc.stdout)))), 1 + 8)
        proc, _ = run_fake([_ok(res)], ["--format=json", "host", "devices"])
        self.assertEqual(json.loads(proc.stdout)["result"], res)
                         
        single = [({"truncated": True}, notes[0]),
                  ({"sources": {"usb": "absent", "pci": "available", "names": "available"}}, notes[1]),
                  ({"sources": {"usb": "available", "pci": "absent", "names": "available"}}, notes[2]),
                  ({"sources": {"usb": "available", "pci": "available", "names": "unavailable"}}, notes[3])]
        for extra, want in single:
            with self.subTest(want=want):
                proc, _ = run_fake([_ok(dict(RESULT, **extra))], ["host", "devices"])
                self.assertEqual([l for l in proc.stdout.splitlines() if l.startswith("(")], [want])
        proc, _ = run_fake([_ok(RESULT)], ["host", "devices"])                       
        self.assertEqual([l for l in proc.stdout.splitlines() if l.startswith("(")], [])

    def test_gpu_list_unchanged(self):                             
        gpus = [{"pci_addr": "0000:01:00.0", "device": "GA104", "driver": "vfio-pci"}]
        proc, reqs = run_fake([_ok(gpus)], ["gpu", "list"])
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual([(r["method"], r["params"]) for r in reqs], [("gpu.list", {})])
        self.assertIn(["PCI_ADDR", "DEVICE", "DRIVER"], [l.split() for l in proc.stdout.splitlines()])
        self.assertIn(["0000:01:00.0", "GA104", "vfio-pci"], [l.split() for l in proc.stdout.splitlines()])

    def test_gpu_attach_detach_unchanged(self):                                                   
        for action in ("attach", "detach"):
            with self.subTest(action=action):
                proc, reqs = run_fake([_ok({"status": "ok"})], ["gpu", action, "win", "0000:01:00.0"])
                self.assertEqual(proc.returncode, 0, proc.stderr)
                self.assertEqual([(r["method"], r["params"]) for r in reqs],
                                 [(f"device.gpu.{action}", {"vm_name": "win", "pci_addr": "0000:01:00.0"})])
        proc, reqs = run_fake([], ["gpu", "attach", "win"])
        self.assertEqual((proc.returncode, reqs), (2, []))
        self.assertIn("Usage: pcvctl gpu attach <vm_name> <canonical_pci_addr>", proc.stdout)


if __name__ == "__main__":
    unittest.main()
