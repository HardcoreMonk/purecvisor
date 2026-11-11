#!/usr/bin/env python3
                          
                                                                                                   
                                                                                                  
                                                  
        
                                                                                                   
                                                                                                
                                        
                                                                             
                                                                                
                                                                                              
                                                                                    
                                                                           
                                                                                                     
                    
                                               
                                 
                                                                                                              
                                                                                                        
                                                                                        
                                                                                               
                                                                                        
                                                       
 
                                                                              
                                                                                
                                
from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts" / "split"))
import c_blocks              

D = "src/modules/dispatcher/"
VIEW = D + "host_device_view.c"
SCAN = D + "host_device_scan.c"
HANDLER = D + "handler_host_device.c"
GPU = "src/modules/accel/gpu_manager.c"


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def no_comments(src: str) -> str:
                                                       
    kinds = c_blocks.classify(src)
    return "".join(ch if k != "m" or ch == "\n" else " " for ch, k in zip(src, kinds))


def body_span(src: str, name: str) -> tuple[int, int]:
    for b in c_blocks.scan(src)[0]:
        if b.name == name:
            return b.body_open, b.body_close + 1
    raise AssertionError(f"function {name} not found")


def body(src: str, name: str) -> str:
    o, c = body_span(src, name)
    return no_comments(src)[o:c]


def replace_in_fn(src: str, name: str, old: str, new: str) -> str:
    o, c = body_span(src, name)
    part = src[o:c]
    assert part.count(old) == 1, (name, old)
    return src[:o] + part.replace(old, new) + src[c:]


VM_MEMBER = re.compile(r'json_object_set_\w+_member\(\s*[^,]+,\s*"vm"\s*[,)]')
ASSIGNMENTS_SET = re.compile(r'json_object_set_array_member\(([^;]*?"assignments"[^;]*)\);', re.S)


def single_constructor_problems(src: str) -> list[str]:
                             
    code = no_comments(src)
    probs = []
    if code.count('"vm_visible"') != 1:
        probs.append(f'"vm_visible" literal count {code.count(chr(34) + "vm_visible" + chr(34))}')
    o, c = body_span(src, "view_assignment")
    for m in VM_MEMBER.finditer(code):
        if not (o <= m.start() < c):
            probs.append(f'"vm" member set outside view_assignment at offset {m.start()}')
    sets = ASSIGNMENTS_SET.findall(code)
    if len(sets) < 2:
        probs.append(f'"assignments" arrays found {len(sets)} (row + companion expected)')
    for args in sets:
        if "view_assignments(" not in args:
            probs.append(f'"assignments" built without view_assignments: {" ".join(args.split())[:80]}')
    return probs


FAIL_BRANCH = re.compile(r'==\s*PCV_HOST_READ_FAIL\s*\)\s*\{(.*?)\}', re.S)


def fail_closed_problems(src: str) -> list[str]:
                  
    probs = []
    read = body(src, "_host_read_domain")
    if read.count("pcv_host_device_read_outcome(") < 3:
        probs.append("_host_read_domain does not classify every query/read with pcv_host_device_read_outcome")
    h = body(src, "handle_host_device_list")
    m = FAIL_BRANCH.search(h)
    if not m:
        probs.append("handler has no '== PCV_HOST_READ_FAIL) {' branch")
    else:
        if "fail_msg = pcv_host_device_read_error_message(" not in m.group(1):
            probs.append("FAIL branch does not build the P14 message")
        if "continue" in m.group(1):
            probs.append("FAIL branch continues (skips the VM)")
    reply = h.find("if (fail_msg)")
    scan = h.find("pcv_host_device_scan(")
    if reply < 0 or scan < 0 or reply > scan:
        probs.append("failure reply is not sent before host scan")
    else:
        blk = h[reply:scan]
        if "_host_reply_error(" not in blk or "return;" not in blk:
            probs.append("failure block does not reply and return")
    return probs


IS_PERSISTENT = re.compile(r'\b(\w+)\s*=\s*virDomainIsPersistent\(')
IS_ACTIVE = re.compile(r'\b(\w+)\s*=\s*virDomainIsActive\(')


def query_rule_problems(src: str) -> list[str]:
                  
    code = no_comments(src)
    probs = []
    if "virDomainGetID(" in code:
        probs.append("virDomainGetID( used")
    if re.search(r'virDomainIsPersistent\([^)]*\)\s*==\s*1', code):
        probs.append("virDomainIsPersistent(...) == 1 folds -1 into 'not persistent'")
    a = IS_ACTIVE.search(code)
    if not a or not re.search(r'pcv_res_domain_active_state\(\s*' + a.group(1) + r'\s*,', code):
        probs.append("virDomainIsActive result does not reach pcv_res_domain_active_state")
    p = IS_PERSISTENT.search(code)
    if not p:
        probs.append("virDomainIsPersistent result not kept")
    elif not re.search(r'if\s*\(\s*' + p.group(1) + r'\s*<\s*0\s*\)\s*return\s+pcv_host_device_read_outcome\(', code):
        probs.append("negative virDomainIsPersistent is not classified")
    return probs


class SingleAssignmentConstructor(unittest.TestCase):
    def test_single_assignment_constructor(self):
        src = read(VIEW)
        self.assertEqual(single_constructor_problems(src), [])
                                                                     
        mut = replace_in_fn(src, "view_gpu_json", 'json_object_set_array_member(c, "assignments", ',
                            'json_object_set_string_member(c, "vm", "x");\n        '
                            'json_object_set_array_member(c, "assignments", ')
        self.assertTrue(any("outside view_assignment" in p for p in single_constructor_problems(mut)))
        mut = replace_in_fn(src, "view_gpu_json",
                            'view_assignments(cx->caller, g_ptr_array_index(r->comp_asg, k))', 'json_array_new()')
        self.assertTrue(any("without view_assignments" in p for p in single_constructor_problems(mut)))


class HandlerFailsClosed(unittest.TestCase):
    def test_handler_fails_closed_on_read(self):
        src = read(HANDLER)
        self.assertEqual(fail_closed_problems(src), [])
        mut = replace_in_fn(src, "handle_host_device_list",
                            "fail_msg = pcv_host_device_read_error_message(&caller, dn);", "(void)dn; continue;")
        self.assertTrue(fail_closed_problems(mut))
        mut = replace_in_fn(src, "handle_host_device_list",
                            "g_free(fail_msg);\n        g_ptr_array_unref(vms);\n        pcv_res_caller_clear(&rc);\n        return;",
                            "g_free(fail_msg);")                                              
        self.assertIn("failure block does not reply and return", fail_closed_problems(mut))


class HandlerQueryRules(unittest.TestCase):
    def test_handler_query_rules(self):
        src = read(HANDLER)
        self.assertEqual(query_rule_problems(src), [])
        mut = replace_in_fn(src, "_host_read_domain", "    PcvResDomState state = _host_active_state(dom);",
                            "    PcvResDomState state = virDomainGetID(dom) != (unsigned int)-1 ? PCV_RES_DOM_ACTIVE"
                            " : PCV_RES_DOM_INACTIVE;")
        self.assertIn("virDomainGetID( used", query_rule_problems(mut))
        mut = replace_in_fn(src, "_host_read_domain", "int persistent = virDomainIsPersistent(dom);",
                            "int persistent = virDomainIsPersistent(dom) == 1;")
        self.assertTrue(query_rule_problems(mut))


class VisibleCallback(unittest.TestCase):
    def test_visible_callback_uses_res_visible(self):
        src = read(HANDLER)
        self.assertIn("pcv_res_visible(", body(src, "_host_visible"))
        h = body(src, "handle_host_device_list")
        self.assertIn("pcv_res_caller_init(", h)
        self.assertRegex(h, r'PcvHostViewCaller\s+caller\s*=\s*\{[^}]*_host_visible[^}]*\}')
        mut = replace_in_fn(src, "_host_visible", "return pcv_res_visible(ctx, kind, id);",
                            "(void)ctx; (void)kind; (void)id; return TRUE;")
        self.assertNotIn("pcv_res_visible(", body(mut, "_host_visible"))


SPAWN = re.compile(r'\b(pcv_gpu_list|pcv_spawn\w*|g_spawn\w*|popen|system)\s*\(')


class NoSpawn(unittest.TestCase):
    def test_no_spawn_or_gpu_list(self):
        for rel in (VIEW, SCAN, HANDLER):
            code = c_blocks.masked(read(rel))
            self.assertIsNone(SPAWN.search(code), rel)
        self.assertIsNotNone(SPAWN.search(c_blocks.masked(read(SCAN) + "\nvoid x(void){ pcv_gpu_list(); }\n")))


class ScanSerial(unittest.TestCase):
    def test_scan_never_reads_serial(self):
        src = read(SCAN)
        self.assertNotIn('"serial"', no_comments(src))
        mut = replace_in_fn(src, "scan_usb_device", 'd->product_name = scan_read_text(dir, "product");',
                            'd->product_name = scan_read_text(dir, "serial");')
        self.assertIn('"serial"', no_comments(mut))


GPU_SHARED_CALL = re.compile(r'pcv_pci_iommu_group_dir\(\s*"/"\s*,\s*pci_addr\s*,')
LINK_SELF_READ = ("g_canonicalize_filename(", "g_file_read_link(", '"iommu_group"')


def group_resolver_problems(gpu_src: str, scan_src: str) -> list[str]:
                  
    probs = []
    g = body(gpu_src, "_gpu_iommu_slot_group")
    for bad in LINK_SELF_READ:
        if bad in g:
            probs.append(f"gpu_manager _gpu_iommu_slot_group resolves the group link itself ({bad})")
    if not GPU_SHARED_CALL.search(g):
        probs.append('gpu_manager _gpu_iommu_slot_group does not call pcv_pci_iommu_group_dir("/", pci_addr, ...)')
    if "g_canonicalize_filename(" in no_comments(scan_src):
        probs.append("host_device_scan.c resolves a link character-wise (g_canonicalize_filename()")
    for fn in ("scan_pci_func", "scan_gpu_group"):
        b = body(scan_src, fn)
        if "pcv_pci_iommu_group_dir(" not in b:
            probs.append(f"host_device_scan {fn} does not call pcv_pci_iommu_group_dir")
        if '"iommu_group"' in b:
            probs.append(f"host_device_scan {fn} reads the iommu_group link itself")
    return probs


class SharedIommuGroupResolver(unittest.TestCase):
    def test_both_callers_use_shared_resolver(self):
        gpu, scan = read(GPU), read(SCAN)
        self.assertEqual(group_resolver_problems(gpu, scan), [])
                                            
        mut = replace_in_fn(gpu, "_gpu_iommu_slot_group",
                            'pcv_pci_iommu_group_dir("/", pci_addr, &group_path, NULL, NULL)',
                            '(group_path = g_canonicalize_filename(g_file_read_link("x", NULL), "/sys/bus/pci/devices"))')
        probs = group_resolver_problems(mut, scan)
        self.assertTrue(any("g_canonicalize_filename(" in p for p in probs), probs)
        self.assertTrue(any("does not call" in p for p in probs), probs)
                                               
        mut = replace_in_fn(scan, "scan_gpu_group",
                            "!pcv_pci_iommu_group_dir(root, g->fn.bdf, &group_dir, NULL, NULL) || !group_dir",
                            '!(group_dir = g_canonicalize_filename("../../../kernel/iommu_groups/0", devices_dir))')
        probs = group_resolver_problems(gpu, mut)
        self.assertIn("host_device_scan scan_gpu_group does not call pcv_pci_iommu_group_dir", probs)
        self.assertTrue(any("character-wise" in p for p in probs), probs)
                                                 
        mut = replace_in_fn(scan, "scan_pci_func",
                            "(void)pcv_pci_iommu_group_dir(root, bdf, NULL, &fn->iommu_group, NULL);",
                            'fn->iommu_group = scan_link_name(dir, "iommu_group");')
        probs = group_resolver_problems(gpu, mut)
        self.assertIn("host_device_scan scan_pci_func reads the iommu_group link itself", probs)


class ErrorTexts(unittest.TestCase):
    def test_error_texts_present(self):
        want = {
            SCAN: ['"cannot read host USB devices"', '"cannot read host PCI devices"'],
            HANDLER: ['"cannot list VMs"'],
            VIEW: ['"cannot read the definition of VM \'%s\'"', '"cannot read a VM definition"'],
        }
        for rel, texts in want.items():
            code = no_comments(read(rel))
            for t in texts:
                self.assertIn(t, code, rel)


if __name__ == "__main__":
    unittest.main(verbosity=1)
