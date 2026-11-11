#!/usr/bin/env python3
                          
                                                                                                          
                                                                                           
                                    
                                                                                                   
                                                                                                     
                                                                                                         
                                                                                                     
                                                                                                         
                                                                                                              
                                                                                              
                                                                                             
                                                                                         
                                                                                                           
                                                                                               
                                                                            
 
                                                                           
                                                                        
                              
                                         
from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
sys.path.insert(0, str(ROOT / "scripts" / "split"))
import c_blocks              
import rpc_extract              

LIVE_ATTACH = re.compile(r"virDomainAttachDeviceFlags\(\s*dom\s*,\s*plain_xml\s*,\s*VIR_DOMAIN_AFFECT_LIVE\s*\)")
CONFIG_ATTACH = re.compile(r"virDomainAttachDeviceFlags\(\s*dom\s*,\s*policy_xml\s*,\s*VIR_DOMAIN_AFFECT_CONFIG\s*\)")
ANY_ATTACH = re.compile(r"virDomainAttachDeviceFlags\(")
DETACH = re.compile(r"virDomainDetachDeviceFlags\(([^;]*?)\)\s*(?:==|<|;|\))")
FIXED_LIVE = re.compile(r"\bflags\s*=\s*VIR_DOMAIN_AFFECT_LIVE\s*;")
LIVE_SET = re.compile(r"\blive_attached\s*=\s*TRUE\s*;")
DEFAULT_POLICY = re.compile(r"\bPcvUsbPolicy\s+policy\s*=\s*PCV_USB_POLICY_OPTIONAL\s*;")


def family_text() -> str:
    return rpc_extract.vm_family_text("vm_hotplug")


def fn_bodies(src: str) -> dict[str, str]:
                                
    m = c_blocks.masked(src)
    return {b.name: m[b.body_open:b.body_close + 1] for b in c_blocks.scan(src)[0]}


def replace_in_fn(text: str, name: str, old: str, new: str) -> str:
                                                                                
    for b in c_blocks.scan(text)[0]:
        if b.name == name:
            body = text[b.body_open:b.body_close + 1]
            assert body.count(old) == 1, (name, old)
            return text[:b.body_open] + body.replace(old, new) + text[b.body_close + 1:]
    raise AssertionError(name)


def enclosing_if_condition(body: str, pos: int) -> str | None:
                                                                  
    depth, o = 0, -1
    for i in range(pos - 1, -1, -1):
        if body[i] == "}":
            depth += 1
        elif body[i] == "{":
            if depth == 0:
                o = i
                break
            depth -= 1
    head = body[:o].rstrip()
    if o < 0 or not head.endswith(")"):
        return None
    depth = 0
    for i in range(len(head) - 1, -1, -1):
        depth += (head[i] == ")") - (head[i] == "(")
        if depth == 0:
            if not re.search(r"\bif\s*$", head[:i]):
                return None
            return re.sub(r"\s+", " ", head[i + 1:-1]).strip()
    return None


def attach_problems(text: str) -> list[str]:
    b = fn_bodies(text).get("handle_vm_usb_attach")
    if b is None:
        return ["handle_vm_usb_attach 없음"]
    out = []
    if "pcv_usb_attach_plan(" not in b:
        out.append("판정 함수 pcv_usb_attach_plan( 호출이 없다")
    if not DEFAULT_POLICY.search(b):
        out.append("startup_policy 기본값 PcvUsbPolicy policy = PCV_USB_POLICY_OPTIONAL; 이 없다")
    if FIXED_LIVE.search(b):
        out.append("고정 대입 flags = VIR_DOMAIN_AFFECT_LIVE; 가 있다")
    lives, cfgs = list(LIVE_ATTACH.finditer(b)), list(CONFIG_ATTACH.finditer(b))
    if len(lives) != 1 or len(cfgs) != 1 or len(ANY_ATTACH.findall(b)) != 2:
        out.append(f"attach 호출이 LIVE(plain_xml)·CONFIG(policy_xml) 하나씩이 아니다({len(lives)}, {len(cfgs)})")
        return out
    live_at, cfg_at = lives[0].start(), cfgs[0].start()
    if live_at > cfg_at:
        out.append("① LIVE 호출이 ② CONFIG 호출보다 뒤다")
    sets = [m.start() for m in LIVE_SET.finditer(b)]
    if len(sets) != 1 or not live_at < sets[0] < cfg_at:
        out.append("live_attached = TRUE; 가 ① 뒤·② 앞에 정확히 한 번이 아니다")
    dets = list(DETACH.finditer(b))
    if len(dets) != 1:
        out.append(f"보상 virDomainDetachDeviceFlags( 가 하나가 아니다({len(dets)})")
        return out
    args = re.sub(r"\s+", "", dets[0].group(1))
    if args != "dom,plain_xml,VIR_DOMAIN_AFFECT_LIVE":
        out.append(f"보상이 정책 없는 XML의 LIVE detach가 아니다: {args}")
    if dets[0].start() < cfg_at:
        out.append("보상이 ② CONFIG 호출보다 앞이다")
    cond = enclosing_if_condition(b, dets[0].start())
    if cond != "live_attached":
        out.append(f"보상 조건이 정확히 live_attached가 아니다: {cond!r}")
    return out


PERSIST_MSGS = ("persistent must be a boolean", "startup_policy applies only when persistent is true")
PERSIST_READ = re.compile(r'pcv_rpc_params_get_bool\(\s*params\s*,\s*"persistent"\s*,\s*TRUE\s*,\s*&\s*(\w+)\s*\)')
PLAN_PERSIST_ARG = 7                                                                                                      


def raw_body(text: str, name: str) -> str | None:
                                                              
    for b in c_blocks.scan(text)[0]:
        if b.name == name:
            return text[b.body_open:b.body_close + 1]
    return None


def call_args(masked_body: str, callee: str) -> list[str] | None:
                                                            
    at = masked_body.find(callee + "(")
    if at < 0:
        return None
    depth, args, cur = 0, [], ""
    for ch in masked_body[at + len(callee):]:
        depth += (ch == "(") - (ch == ")")
        if depth == 0:
            break
        if ch == "," and depth == 1:
            args.append(cur)
            cur = ""
        elif depth >= 1 and not (ch == "(" and depth == 1):
            cur += ch
    return [re.sub(r"\s+", "", a) for a in args + [cur]]


def persistent_order_problems(text: str) -> list[str]:
                                                 
    raw, b = raw_body(text, "handle_vm_usb_attach"), fn_bodies(text).get("handle_vm_usb_attach")
    if raw is None or b is None:
        return ["handle_vm_usb_attach 없음"]
    gate, lock = b.find("pcv_res_gate("), b.find("pcv_vm_lock_acquire(")
    if gate < 0 or lock < 0:
        return [f"자원 관문·잠금 호출을 찾지 못했다({gate}, {lock})"]
    out = []
    for msg in PERSIST_MSGS:
        hits = [m.start() for m in re.finditer(r'\bbad\s*=\s*"' + re.escape(msg) + '"', raw)]
        if len(hits) != 1:
            out.append(f'bad = "{msg}" 가 한 번이 아니다({len(hits)})')
        elif hits[0] > min(gate, lock):
            out.append(f'"{msg}" 검사가 자원 관문·잠금보다 뒤다')
    return out


def persistent_plan_problems(text: str) -> list[str]:
                                                                    
    raw, b = raw_body(text, "handle_vm_usb_attach"), fn_bodies(text).get("handle_vm_usb_attach")
    if raw is None or b is None:
        return ["handle_vm_usb_attach 없음"]
    reads = PERSIST_READ.findall(raw)
    if len(reads) != 1:
        return [f'pcv_rpc_params_get_bool(params, "persistent", TRUE, &x) 가 한 번이 아니다({len(reads)})']
    args = call_args(b, "pcv_usb_attach_plan")
    if args is None or len(args) <= PLAN_PERSIST_ARG:
        return [f"pcv_usb_attach_plan( 호출 인자가 모자란다: {args}"]
    if args[PLAN_PERSIST_ARG] != reads[0]:
        return [f"persist 자리 인자가 params에서 읽은 {reads[0]}이 아니다: {args[PLAN_PERSIST_ARG]}"]
    return []


USB_LIST_KEYS = {"vendor_id", "product_id", "live", "persistent", "startup_policy", "missing"}
USB_LIST_SET = re.compile(r'json_object_set_\w+_member\(\s*usb\s*,\s*"(\w+)"')


def usb_list_key_problems(text: str) -> list[str]:
                                                               
    raw = raw_body(text, "handle_vm_usb_list")
    if raw is None:
        return ["handle_vm_usb_list 없음"]
    keys = USB_LIST_SET.findall(raw)
    if sorted(keys) != sorted(USB_LIST_KEYS):
        return [f"vm.usb.list 원소 키가 계약 6개와 다르다: {sorted(keys)}"]
    return []


DETACH_FLAGS = re.compile(r"\bunsigned\s+int\s+flags\s*=\s*\(\s*want_live\s*\?\s*VIR_DOMAIN_AFFECT_LIVE\s*:\s*0\s*\)\s*\|"
                          r"\s*\(\s*want_config\s*\?\s*VIR_DOMAIN_AFFECT_CONFIG\s*:\s*0\s*\)\s*;")
DETACH_PLAN = re.compile(r"pcv_usb_detach_plan\([^;]*&want_live\s*,\s*&want_config\s*,")


def detach_problems(text: str) -> list[str]:
    b = fn_bodies(text).get("handle_vm_usb_detach")
    if b is None:
        return ["handle_vm_usb_detach 없음"]
    out = []
    if not DETACH_PLAN.search(b):
        out.append("판정 함수 pcv_usb_detach_plan(…, &want_live, &want_config, …) 호출이 없다")
    if FIXED_LIVE.search(b):
        out.append("고정 대입 flags = VIR_DOMAIN_AFFECT_LIVE; 가 있다")
    if not DETACH_FLAGS.search(b):
        out.append("flags가 판정 결과(want_live·want_config)에서 오지 않는다")
    dets = list(DETACH.finditer(b))
    if len(dets) != 1 or re.sub(r"\s+", "", dets[0].group(1)) != "dom,xml,flags":
        out.append(f"virDomainDetachDeviceFlags(dom, xml, flags) 한 번이 아니다({len(dets)})")
    if "pcv_usb_hostdev_xml(v, p, NULL)" not in re.sub(r"\s+", " ", b):
        out.append("넘기는 XML이 정책 없는 pcv_usb_hostdev_xml(v, p, NULL)이 아니다")
    return out


class UsbAttachContract(unittest.TestCase):
    text = family_text()

    def _sub(self, old: str, new: str) -> str:
        return replace_in_fn(self.text, "handle_vm_usb_attach", old, new)

    def test_current_tree(self):
        self.assertEqual(attach_problems(self.text), [])

    def test_order_swapped_red(self):
        live = "virDomainAttachDeviceFlags(dom, plain_xml, VIR_DOMAIN_AFFECT_LIVE)"
        cfg = "virDomainAttachDeviceFlags(dom, policy_xml, VIR_DOMAIN_AFFECT_CONFIG)"
        swapped = replace_in_fn(replace_in_fn(self._sub(live, "@@L@@"), "handle_vm_usb_attach", cfg, live),
                                "handle_vm_usb_attach", "@@L@@", cfg)
        self.assertTrue(attach_problems(swapped))

    def test_compensation_removed_red(self):
        m = self._sub("virDomainDetachDeviceFlags(dom, plain_xml, VIR_DOMAIN_AFFECT_LIVE) == 0", "TRUE")
        self.assertTrue(attach_problems(m))

    def test_compensation_on_legacy_red(self):
        m = self._sub("if (live_attached) {", "if (live_attached || kind == PCV_USB_ATTACH_CONFIG_LEGACY) {")
        self.assertTrue(attach_problems(m))

    def test_compensation_unconditional_red(self):
        m = self._sub("if (live_attached) {", "if (TRUE) {")
        self.assertTrue(attach_problems(m))

    def test_fixed_live_flags_red(self):
        m = self._sub("gboolean live_attached = FALSE;",
                      "gboolean live_attached = FALSE;\n    unsigned int flags = VIR_DOMAIN_AFFECT_LIVE;")
        self.assertTrue(attach_problems(m))

    def test_plan_call_removed_red(self):
        self.assertTrue(attach_problems(self._sub("pcv_usb_attach_plan(", "pcv_usb_attach_gone(")))

    def test_default_policy_red(self):
        m = self._sub("PcvUsbPolicy policy = PCV_USB_POLICY_OPTIONAL;", "PcvUsbPolicy policy = PCV_USB_POLICY_MANDATORY;")
        self.assertTrue(attach_problems(m))

    def test_live_with_policy_xml_red(self):
                                                                                       
        m = self._sub("virDomainAttachDeviceFlags(dom, plain_xml, VIR_DOMAIN_AFFECT_LIVE)",
                      "virDomainAttachDeviceFlags(dom, policy_xml, VIR_DOMAIN_AFFECT_LIVE)")
        self.assertTrue(attach_problems(m))

    def test_persistent_checked_before_gate_and_lock(self):
                                                                                             
        self.assertEqual(persistent_order_problems(self.text), [])
        lock = "PcvVmLock *op_lock = pcv_vm_lock_acquire(vm_id, VM_OP_TUNING, &lock_err);"
        for msg in PERSIST_MSGS:
            with self.subTest(msg=msg):
                moved = self._sub(f'bad = "{msg}";', "bad = NULL;")
                self.assertTrue(persistent_order_problems(moved))           
                after_gate = replace_in_fn(moved, "handle_vm_usb_attach", "if (!*vm_id) {", f'bad = "{msg}";\n    if (!*vm_id) {{')
                self.assertTrue(persistent_order_problems(after_gate))                 
                after_lock = replace_in_fn(moved, "handle_vm_usb_attach", lock, f'{lock}\n    bad = "{msg}";')
                self.assertTrue(persistent_order_problems(after_lock))         

    def test_persistent_reaches_plan(self):
                                                                                       
        self.assertEqual(persistent_plan_problems(self.text), [])
        var = PERSIST_READ.findall(raw_body(self.text, "handle_vm_usb_attach"))[0]
        self.assertTrue(persistent_plan_problems(self._sub(f"policy, {var}, vm_id,", "policy, TRUE, vm_id,")))
        default_false = self._sub(f'"persistent", TRUE, &{var}', f'"persistent", FALSE, &{var}')
        self.assertTrue(persistent_plan_problems(default_false))                                 



class UsbListContract(unittest.TestCase):
    text = family_text()

    def _sub(self, old: str, new: str) -> str:
        return replace_in_fn(self.text, "handle_vm_usb_list", old, new)

    def test_usb_list_response_keys_unchanged(self):
                                                                                                           
        self.assertEqual(usb_list_key_problems(self.text), [])
        anchor = 'json_object_set_boolean_member(usb, "missing", u->missing);'
        added = self._sub(anchor, anchor + '\n        json_object_set_int_member(usb, "live_bus", u->live_bus);')
        self.assertTrue(usb_list_key_problems(added))
        self.assertTrue(usb_list_key_problems(self._sub(anchor, "")))
        self.assertTrue(usb_list_key_problems(self._sub('"startup_policy", u->startup_policy', '"policy", u->startup_policy')))


class UsbDetachContract(unittest.TestCase):
    text = family_text()

    def _sub(self, old: str, new: str) -> str:
        return replace_in_fn(self.text, "handle_vm_usb_detach", old, new)

    def test_current_tree(self):
        self.assertEqual(detach_problems(self.text), [])

    def test_always_live_and_config_red(self):
                                                                          
        m = self._sub("(want_live ? VIR_DOMAIN_AFFECT_LIVE : 0) |", "VIR_DOMAIN_AFFECT_LIVE |")
        self.assertTrue(detach_problems(m))

    def test_fixed_live_flags_red(self):
        m = self._sub("if (virDomainDetachDeviceFlags(dom, xml, flags) < 0) {",
                      "flags = VIR_DOMAIN_AFFECT_LIVE;\n    if (virDomainDetachDeviceFlags(dom, xml, flags) < 0) {")
        self.assertTrue(detach_problems(m))

    def test_plan_call_removed_red(self):
        self.assertTrue(detach_problems(self._sub("pcv_usb_detach_plan(", "pcv_usb_detach_gone(")))

    def test_policy_xml_red(self):
        m = self._sub("g_autofree gchar *xml = pcv_usb_hostdev_xml(v, p, NULL);",
                      "PcvUsbPolicy pol = PCV_USB_POLICY_OPTIONAL;\n    g_autofree gchar *xml = pcv_usb_hostdev_xml(v, p, &pol);")
        self.assertTrue(detach_problems(m))


if __name__ == "__main__":
    unittest.main()
