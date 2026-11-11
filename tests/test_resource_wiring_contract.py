#!/usr/bin/env python3
                                                                                                
                                                                         
                                                                                          
                                                                             
                                                                           
                                                           
                                                                                                
                                                                           
                                                                      
                                                                     
                                                                       
import re
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import rpc_extract              
DISPATCHER = ROOT / "src" / "modules" / "dispatcher"
VM_CREATE = DISPATCHER / "handler_vm_create.c"
HOTPLUG = DISPATCHER / "handler_vm_hotplug.c"
OVA = DISPATCHER / "handler_vm_ova.c"
CONTAINER = DISPATCHER / "handler_container.c"
CONTAINER_MODE = DISPATCHER / "handler_container_mode.c"
VM_MANAGER = ROOT / "src" / "modules" / "virt" / "vm_manager.c"

                                                
SIDE_EFFECT = r"\b(?:pcv_vm_lock_acquire|pcv_job_admit|virDomainAttachDeviceFlags|virDomainUpdateDeviceFlags)\("

                                                
GATED = [
    (HOTPLUG, "handle_vm_mount_iso(JsonObject", "iso_path", "PCV_RES_ISO"),
    (HOTPLUG, "handle_vm_usb_attach(JsonObject", "vendor_id/product_id", "PCV_RES_USB"),
    (HOTPLUG, "handle_device_nic_attach(JsonObject", "bridge", "PCV_RES_BRIDGE"),
    (OVA, "handle_vm_import_ova(JsonObject", "ova_path", "PCV_RES_IMAGE"),
    (OVA, "handle_vm_import_ova(JsonObject", "pool", "PCV_RES_STORAGE"),
]

                                                                         
CREATE_REFS = [
    ("iso_path", "PCV_RES_ISO"),
    ("base_image", "PCV_RES_IMAGE"),
    ("storage_pool", "PCV_RES_STORAGE"),
    ("image_dir", "PCV_RES_STORAGE"),
    ("pci_addr", "PCV_RES_PCI"),
    ("network_bridge", "PCV_RES_BRIDGE"),
]


def strip_comments(text: str) -> str:
                                         
    return re.sub(r"/\*.*?\*/|//[^\n]*", lambda m: re.sub(r"[^\n]", " ", m[0]), text, flags=re.S)


def code(path: Path) -> str:
                                   
    family = {HOTPLUG: "vm_hotplug", VM_MANAGER: "vm_manager"}.get(path)
    return strip_comments(rpc_extract.vm_family_text(family) if family else path.read_text())


def body(text: str, signature: str) -> str:
                                
    assert signature in text, f"{signature!r} 정의 없음"
    start = text.index(signature)
    i = text.index("{", start)
    depth = 0
    for j in range(i, len(text)):
        depth += (text[j] == "{") - (text[j] == "}")
        if depth == 0:
            return text[i:j + 1]
    raise AssertionError(signature)


def first(text: str, pattern: str) -> int:
                                     
    m = re.search(pattern, text)
    assert m, f"{pattern!r} 없음"
    return m.start()


def call_args(text: str, at: int) -> str:
                                      
    i = text.index("(", at)
    depth = 0
    for j in range(i, len(text)):
        depth += (text[j] == "(") - (text[j] == ")")
        if depth == 0:
            return text[i + 1:j]
    raise AssertionError(text[at:at + 80])


def gate_call(b: str, param: str) -> int:
                                                                   
    for m in re.finditer(r"\bpcv_res_gate\(", b):
        if re.search(r'"' + re.escape(param) + r'"', call_args(b, m.start())):
            return m.start()
    raise AssertionError(f'pcv_res_gate(... "{param}" ...) 없음')


def check_gated(text: str, signature: str, param: str, kind: str) -> None:
    b = body(text, signature)
    at = gate_call(b, param)
    assert kind in call_args(b, at), (signature, param, kind)
    assert at < first(b, SIDE_EFFECT), (signature, param)


def check_create_refs(text: str) -> None:
                                                                                              
                                                                                              
    h = body(text, "handle_vm_create(PureCVisorDispatcher")
    at = first(h, r"\b_gate_create_refs\(")
    assert first(h, r"\bpcv_validate_base_image_path\(") < at < first(h, r"\bowner\s*=")
    assert at < first(h, SIDE_EFFECT)
    args = re.sub(r"\s+", "", call_args(h, at))
    for p in ("&iso_path", "&base_image", "&storage_pool", "&image_dir", "&pci_addr"):
        assert p in args, (p, args)
    first(body(text, "_res_deny(GSocketConnection"), r'\bpcv_res_audit_denied\(\s*"vm\.create"\s*,')
    g = body(text, "_gate_create_refs(JsonObject")
    first(g, r"\bpcv_res_caller_init\(")
    first(g, r"\bpcv_res_check\(")
    for param, kind in CREATE_REFS:
        first(g, r'"' + re.escape(param) + r'"\s*,\s*' + kind + r"\b")


def check_create_empty_pci(text: str) -> None:
                                                                                            
                                                                                                
                                                                                         
    g = body(text, "_gate_create_refs(JsonObject")
    head = g[:first(g, r"\bpcv_res_check\(")]
    at = first(head, r"\bif\s*\(\s*!value\b")
    inner = call_args(head, at)
    cond = re.sub(r"\s+", " ", inner).strip()
    assert cond == "!value || (!*value && refs[i].kind != PCV_RES_PCI)", cond
    rest = head[head.index("(", at) + len(inner) + 2:]
    assert re.match(r"\s*continue\s*;", rest), "그 조건의 본문은 continue다"


def check_overlay_tenant(text: str) -> None:
                                                                                                 
                                                                                
    h = body(text, "handle_vm_create(PureCVisorDispatcher")
    at = first(h, r"\b_gate_overlay_tenant\(")
    assert first(h, r"\bpcv_vm_validate_network_mode_tenant\(") < at < first(h, r"\bpcv_qos_sla_from_json\(")
    first(body(text, "_gate_overlay_tenant(JsonObject"), r"\bpcv_res_check_tenant_name\(")


def check_mount_iso_xml(text: str) -> None:
                                                                         
    b = body(text, "handle_vm_mount_iso(JsonObject")
    gate = gate_call(b, "iso_path")
    assign = first(b, r"\biso_path\s*=\s*iso_canon\s*;")
    assert gate < assign < first(b, r"\bg_file_test\(\s*iso_path\b")
    assert assign < first(b, r"\bg_markup_escape_text\(\s*iso_path\b")


def check_ova_uses_effective(text: str) -> None:
                                                                                         
                                                                                        
                                                 
    b = body(text, "handle_vm_import_ova(JsonObject")
    gate = gate_call(b, "ova_path")
    assert '"ova_path", PCV_RES_IMAGE, ova_path,' in re.sub(r"\s+", " ", call_args(b, gate)), "판정 값은 요청 원문 ova_path"
    assign = first(b, r"\bova_path\s*=\s*ova_canon\s*;")
    real = first(b, r"\brealpath\(\s*ova_path\b")
    assert gate < first(b, r"\brealpath\(")
    assert gate < assign < real
    assert real < first(b, r"\bg_file_test\(\s*real_ova\s*,\s*G_FILE_TEST_IS_REGULAR")
    assert real < first(b, r"ctx->ova_path\s*=\s*g_strdup\(\s*real_ova\s*\)")
    assert gate_call(b, "pool") < first(b, r"\bpool\s*=\s*pool_canon\s*;")


def line_count(rel: str) -> int:
    return len((ROOT / rel).read_text().splitlines())


def if_condition(b: str, pattern: str) -> str:
                                                                          
    for m in re.finditer(r"\bif\s*\(", b):
        cond = re.sub(r"\s+", " ", call_args(b, m.start()))
        if re.search(pattern, cond):
            return cond
    raise AssertionError(f"if ( ... {pattern!r} ... ) 없음")


def check_workers_recheck(vm: str, create: str, ova: str) -> None:
                                                                                                 
                                                                                         
    f = body(vm, "purecvisor_vm_provision_file_disk(const gchar *format")
    cond = if_condition(f, r"\bpcv_res_pin_matches\(")
    assert "g_file_test(base_image, G_FILE_TEST_IS_REGULAR)" in cond, cond
    assert "pcv_res_pin_matches(base_image, base_pin)" in cond, cond
    assert first(f, r"\bpcv_res_pin_matches\(") < first(f, r'"qemu-img"\s*,\s*"create"')
    z = body(vm, "purecvisor_vm_provision_zvol_disk(const gchar *zvol_name")
    zcond = if_condition(z, r"\bpcv_res_pin_matches\(")
    assert "g_file_test(base_image, G_FILE_TEST_IS_REGULAR)" in zcond, zcond
    assert "pcv_res_pin_matches(base_image, base_pin)" in zcond, zcond
    assert first(z, r"\bpcv_res_pin_matches\(") < first(z, r"\bpcv_disk_image_inspect\(") < first(z, r"\bpcv_zfs_create_locked\(") < first(z, r"\bpcv_disk_image_convert_into\(")
    assert first(f, r"\bpcv_disk_image_inspect\(") < first(f, r'"qemu-img"\s*,\s*"create"') < first(f, r"\bpcv_disk_image_convert_into\(")
                                          
    created = first(z, r"\bpcv_zfs_create_locked\(")
    after = z[created:]
    assert first(after, r"\bpcv_res_pin_matches\(") < first(after, r"\bpcv_disk_image_convert_into\(")
    assert first(after, r"\bpcv_disk_image_recheck\(") < first(after, r"\bpcv_disk_image_convert_into\(")
    w = body(vm, "create_vm_thread(GTask")
    for leaf in ("purecvisor_vm_provision_zvol_disk", "purecvisor_vm_provision_file_disk"):
        prov = call_args(w, first(w, r"\b" + leaf + r"\("))
        assert "&data->base_pin" in prov, prov
    a = body(vm, "purecvisor_vm_manager_create_vm_async(PureCVisorVmManager")
    first(a, r"\bdata->base_pin\s*=\s*\*base_image_pin\s*;")
    h = body(create, "handle_vm_create(PureCVisorDispatcher")
    assert "&base_pin" in call_args(h, first(h, r"\b_gate_create_refs\(")), "판정이 base image 핀을 받는다"
    assert "&base_pin" in call_args(h, first(h, r"\bpurecvisor_vm_manager_create_vm_async\(")), "핀을 워커로 넘긴다"
    g = body(create, "_gate_create_refs(JsonObject")
    first(g, r'"base_image"\s*,\s*PCV_RES_IMAGE\s*,\s*base_image\s*,\s*base_pin_out\s*}')
    assert "refs[i].pin" in call_args(g, first(g, r"\bpcv_res_check\(")), "판정 호출이 행의 핀 칸을 넘긴다"
    o = body(ova, "_ova_import_worker(GTask")
    assert first(o, r"\bpcv_res_pin_matches\(\s*ctx->ova_path\s*,\s*&ctx->ova_pin\s*\)") < first(o, r'"tar"\s*,\s*"-xf"')
    i = body(ova, "handle_vm_import_ova(JsonObject")
    assert "&ova_pin" in call_args(i, gate_call(i, "ova_path")), "OVA 판정이 핀을 받는다"
    first(i, r"\bctx->ova_pin\s*=\s*ova_pin\s*;")


def test_each_operator_ref_param_is_gated():
    for path, sig, param, kind in GATED:
        check_gated(code(path), sig, param, kind)
    text = code(VM_CREATE)
    check_create_refs(text)
    check_create_empty_pci(text)
    check_overlay_tenant(text)


                                                                                                   
USB_READ = r"\b(?:_usb_read_defs|virDomainGetXMLDesc|virDomainIsPersistent)\("


def check_usb_gate_before_read(text: str) -> None:
    b = body(text, "handle_vm_usb_attach(JsonObject")
    assert gate_call(b, "vendor_id/product_id") < first(b, USB_READ)


def test_usb_gate_before_definition_read():
    check_usb_gate_before_read(code(HOTPLUG))


def test_counterfactual_usb_read_before_gate_fails():
    text = code(HOTPLUG)
    anchor = "g_autofree gchar *usb_ref"
    assert text.count(anchor) == 1
    with pytest.raises(AssertionError):
        check_usb_gate_before_read(text.replace(anchor, "_usb_read_defs(dom, 0, 0, 0, 0, 0);\n    " + anchor))


def test_mount_iso_uses_gate_result_in_xml():
    check_mount_iso_xml(code(HOTPLUG))


def test_ova_uses_gate_result():
    check_ova_uses_effective(code(OVA))


def check_ova_inspects_vmdk(ova: str) -> None:
                                                                                   
    w = body(ova, "_ova_import_worker(GTask")
    at = first(w, r"\bpcv_disk_image_inspect\(")
    assert "PCV_DISK_IMAGE_OVA_VMDK" in call_args(w, at)
    assert at < first(w, r"\bpcv_zfs_create_locked\(")
    assert at < first(w, r'"qemu-img"\s*,\s*"convert"')
                                                                          
    ovf_reg = first(w, r"\bpcv_ova_import_member_is_regular\(ovf_path\)")
    assert ovf_reg < first(w, r"\bg_file_get_contents\(ovf_path")
    name = first(w, r"\bpcv_ova_import_member_name_is_safe\(vmdk_name\)")
    vmdk_reg = first(w, r"\bpcv_ova_import_member_is_regular\(vmdk_path\)")
    assert ovf_reg < name < first(w, r'vmdk_path\s*=\s*g_strdup_printf') < vmdk_reg < at


def test_ova_inspects_vmdk_before_use():
    check_ova_inspects_vmdk(code(OVA))


def test_counterfactual_ova_inspect_fails():
    ova = code(OVA)
    for mutated in (
        ova.replace("pcv_disk_image_inspect(vmdk_path", "pcv_disk_image_gone(vmdk_path", 1),
        ova.replace("PCV_DISK_IMAGE_OVA_VMDK", "PCV_DISK_IMAGE_BASE", 1),
        ova.replace("pcv_ova_import_member_is_regular(ovf_path)", "TRUE", 1),
        ova.replace("pcv_ova_import_member_name_is_safe(vmdk_name)", "TRUE", 1),
        ova.replace("pcv_ova_import_member_is_regular(vmdk_path)", "g_file_test(vmdk_path, G_FILE_TEST_EXISTS)", 1),
    ):
        with pytest.raises(AssertionError):
            check_ova_inspects_vmdk(mutated)


def check_resize_fixes_format(vm: str) -> None:
                                                                                            
    assert not re.search(r'"qemu-img"\s*,\s*"resize"', vm), "직접 argv 금지"
    w = body(vm, "resize_disk_thread(GTask")
    at = first(w, r"\bpcv_disk_image_resize_file\(")
    assert "disk_format" in call_args(w, at)
    assert first(w, r"\bpcv_disk_image_driver_format\(xml, d->target\)") < first(w, r"\bfree\(xml\)") < at


def test_resize_fixes_format():
    check_resize_fixes_format(code(VM_MANAGER))


def test_counterfactual_resize_format_fails():
    vm = code(VM_MANAGER)
    for mutated in (
        vm + '\nstatic const char *a[] = {"qemu-img", "resize", NULL};\n',
        vm.replace("pcv_disk_image_resize_file(disk_source, disk_format,", "pcv_disk_image_resize_file(disk_source, \"raw\",", 1),
        vm.replace("pcv_disk_image_driver_format(xml, d->target)", "g_strdup(\"raw\")", 1),
    ):
        with pytest.raises(AssertionError):
            check_resize_fixes_format(mutated)


def check_sg_sync_uses_fence(vpc_manager: str) -> None:
                                                                                                      
    b = body(vpc_manager, "pcv_vpc_security_group_sync_vm(const gchar *vm_name")
    assert first(b, r"\bpcv_vpc_ovn_sync_policy\(") < first(b, r"\bpcv_vpc_fence_policy_sync_failed\(")


def test_sg_sync_uses_fence():
    check_sg_sync_uses_fence(code(ROOT / "src" / "modules" / "network" / "vpc" / "vpc_manager.c"))


def test_counterfactual_sg_sync_fails():
    text = code(ROOT / "src" / "modules" / "network" / "vpc" / "vpc_manager.c")
    with pytest.raises(AssertionError):
        check_sg_sync_uses_fence(text.replace("pcv_vpc_fence_policy_sync_failed(", "pcv_vpc_store_gone(", 1))


def test_workers_recheck_only_pinned():
    check_workers_recheck(code(VM_MANAGER), code(VM_CREATE), code(OVA))


def test_counterfactual_workers_recheck_fails():
                                                         
    vm, create, ova = code(VM_MANAGER), code(VM_CREATE), code(OVA)
    zvol_at = vm.index("purecvisor_vm_provision_zvol_disk(const gchar *zvol_name")
    zvol_head, zvol_tail = vm[:zvol_at], vm[zvol_at:]
    for mutated in (
        (vm.replace("pcv_res_pin_matches(base_image, base_pin)", "pcv_res_gone(base_image, base_pin)", 1), create, ova),
        (vm.replace("g_file_test(base_image, G_FILE_TEST_IS_REGULAR)", "TRUE", 1), create, ova),                   
        (zvol_head + zvol_tail.replace("pcv_res_pin_matches(base_image, base_pin)", "TRUE", 1), create, ova),
        (vm.replace("&data->base_pin, &zvol_error", "NULL, &zvol_error", 1), create, ova),
        (zvol_head + zvol_tail.replace("    const gboolean has_base = base_image && *base_image;",                            
                                       '    pcv_zfs_create_locked(NULL, zvol_name, NULL, NULL);\n    const gboolean has_base = base_image && *base_image;', 1),
         create, ova),
        (vm.replace("data->base_image, &data->base_pin, &file_error", "data->base_image, NULL, &file_error", 1), create, ova),
        (re.sub(r"\bdata->base_pin\s*=\s*\*base_image_pin\s*;", ";", vm, count=1), create, ova),
        (vm, re.sub(r"\bbase_image,\s*&base_pin,(\s*owner)", r"base_image, NULL,\1", create, count=1), ova),
        (vm, create.replace("res_keep, &base_pin)", "res_keep, NULL)", 1), ova),
        (vm, create.replace("base_image, base_pin_out }", "base_image, NULL }", 1), ova),
        (vm, create, ova.replace("pcv_res_pin_matches(ctx->ova_path, &ctx->ova_pin)", "TRUE", 1)),
        (vm, create, ova.replace("&ova_canon, &ova_pin,", "&ova_canon, NULL,", 1)),
        (vm, create, ova.replace("    if (!pcv_res_pin_matches(ctx->ova_path",                                       
                                 '    const gchar *early[] = {"tar", "-xf", NULL};\n    if (!pcv_res_pin_matches(ctx->ova_path', 1)),
        (vm.replace("    const gboolean has_base = base_image && *base_image;",                                     
                    '    const gchar *early[] = {"qemu-img", "create", NULL};\n    const gboolean has_base = base_image && *base_image;', 1),
         create, ova),
        (vm, create, re.sub(r"\bctx->ova_pin\s*=\s*ova_pin\s*;", ";", ova, count=1)),
    ):
        assert mutated != (vm, create, ova)
        with pytest.raises(AssertionError):
            check_workers_recheck(*mutated)


def _drop_gate(text: str, signature: str, param: str) -> str:
                                                                                
    b = body(text, signature)
    start = text.index(b)
    at = start + gate_call(b, param)
    return text[:at] + "pcv_res_gone(" + text[at + len("pcv_res_gate("):]


def baseline(rel: str) -> int:
                                                    
    for line in (ROOT / "scripts" / "file_size_baseline.txt").read_text().splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[1] == rel:
            return int(parts[0])
    raise AssertionError(f"{rel} 기준선 없음")


def check_container_bridge(mode_text: str, handler_text: str) -> None:
                                                                                                 
                                                                                            
                                                                                                
                                                                                                   
    m = body(mode_text, "pcv_container_create_mode_check(JsonObject *params")
    at = first(m, r"\b_bridge_allowed\(\s*params")
    assert first(m, r"\bpcv_disp_dispatcher_caller_role\(") < at < first(m, r"\bpcv_lxc_userns_host_check\(")
    g = body(mode_text, "_bridge_allowed(JsonObject")
    first(g, r'"network_bridge"')
    check = first(g, r"\bpcv_res_check\(")
    assert "PCV_RES_BRIDGE" in call_args(g, check)
    first(g, r'\bpcv_res_decision_message\(\s*d\s*,\s*"network_bridge"\s*\)')
    first(g, r"\b_mode_reject\(\s*out\s*,\s*PURE_RPC_ERR_FORBIDDEN\b")
    first(g, r'\bpcv_res_audit_denied\(\s*"container\.create"\s*,\s*"network_bridge"\s*,\s*bridge\s*\)')
    h = body(handler_text, "handle_container_create(JsonObject *params")
    pre = first(h, r"\bpcv_container_create_mode_check\(")
    assert pre < first(h, r"\bpcv_vm_lock_acquire\(") and pre < first(h, r"\bpcv_lxc_create_async(?:_full)?\(")


def test_container_create_bridge_gated():
    check_container_bridge(code(CONTAINER_MODE), code(CONTAINER))
    rel = "src/modules/dispatcher/handler_container.c"
    assert line_count(rel) <= baseline(rel)


def test_counterfactual_removed_gates_fail():
                                                               
    for path, sig, param, kind in GATED:
        with pytest.raises(AssertionError):
            check_gated(_drop_gate(code(path), sig, param), sig, param, kind)
    text = code(VM_CREATE)
    with pytest.raises(AssertionError):
        check_create_refs(re.sub(r"\b_gate_create_refs\(\s*params", "_gone(params", text, count=1))
    for param, kind in CREATE_REFS:
        with pytest.raises(AssertionError):
            check_create_refs(text.replace(f'"{param}", {kind}', f'"{param}_x", {kind}', 1))
    with pytest.raises(AssertionError):
        check_create_refs(text.replace("pcv_res_check(&", "pcv_res_gone(&"))
                                                                                   
    empty_pci = "!value || (!*value && refs[i].kind != PCV_RES_PCI)"
    assert empty_pci in text
    for mutated in (
        text.replace(empty_pci, "!value || !*value", 1),
        text.replace(empty_pci, "!value || (!*value && refs[i].kind != PCV_RES_ISO)", 1),
        re.sub(re.escape(empty_pci) + r"\)\s*continue\s*;", empty_pci + ") break;", text, count=1),
    ):
        assert mutated != text
        with pytest.raises(AssertionError):
            check_create_empty_pci(mutated)
    with pytest.raises(AssertionError):
        check_create_refs(text.replace("pcv_res_audit_denied(", "pcv_res_gone("))                       
    with pytest.raises(AssertionError):
        check_overlay_tenant(re.sub(r"\b_gate_overlay_tenant\(\s*params", "_gone(params", text, count=1))
    with pytest.raises(AssertionError):
        check_overlay_tenant(text.replace("pcv_res_check_tenant_name(", "pcv_res_gone("))
    hot = code(HOTPLUG)
    with pytest.raises(AssertionError):
        check_mount_iso_xml(_drop_gate(hot, "handle_vm_mount_iso(JsonObject", "iso_path"))
    with pytest.raises(AssertionError):
        check_mount_iso_xml(re.sub(r"\biso_path\s*=\s*iso_canon\s*;", ";", hot, count=1))
    ova = code(OVA)
    with pytest.raises(AssertionError):
        check_ova_uses_effective(re.sub(r"\bova_path\s*=\s*ova_canon\s*;", ";", ova, count=1))          
                                                                         
    early = ova.replace("    g_autofree gchar *ova_canon = NULL;",
                        "    gchar *early = realpath(ova_path, NULL);\n    g_autofree gchar *ova_canon = NULL;", 1)
    assert early != ova
    with pytest.raises(AssertionError):
        check_ova_uses_effective(early)
                                                  
    with pytest.raises(AssertionError):
        check_ova_uses_effective(ova.replace("PCV_RES_IMAGE, ova_path, &ova_canon", "PCV_RES_IMAGE, real_ova, &ova_canon", 1))


def test_counterfactual_container_bridge_fails():
    mode, handler = code(CONTAINER_MODE), code(CONTAINER)
    for mutated in (
        re.sub(r"\b_bridge_allowed\(\s*params", "_gone(params", mode, count=1),                     
        mode.replace("PCV_RES_BRIDGE", "PCV_RES_ISO"),                                   
        mode.replace("pcv_res_check(&", "pcv_res_gone(&"),                                
        mode.replace("_mode_reject(out, PURE_RPC_ERR_FORBIDDEN", "_mode_reject(out, PURE_RPC_ERR_CONFLICT"),
        mode.replace("pcv_res_audit_denied(", "pcv_res_gone("),                           
    ):
        with pytest.raises(AssertionError):
            check_container_bridge(mutated, handler)


                                                                                     

def check_sriov_pci_order(create: str) -> None:
                                                                   
    h = body(create, "handle_vm_create(PureCVisorDispatcher")
    gate = first(h, r"\b_gate_create_refs\(")
    lock = first(h, r"\bpcv_vm_lock_acquire\(")
    for pat in (r'"nic_type=sriov requires pci_addr"', r"\bpcv_res_normalize\(\s*PCV_RES_PCI"):
        assert gate < first(h, pat) < lock, pat


def check_sriov_pci_replaced(create: str) -> None:
                                                      
    h = body(create, "handle_vm_create(PureCVisorDispatcher")
    at = first(h, r"\bpcv_res_normalize\(\s*PCV_RES_PCI")
    tail = h[at:first(h, r"\bpcv_vm_lock_acquire\(")]
    first(tail, r"\bg_ptr_array_add\(\s*res_keep\s*,\s*(\w+)\.ref\s*\)")
    var = re.search(r"\bg_ptr_array_add\(\s*res_keep\s*,\s*(\w+)\.ref\s*\)", tail).group(1)
    m = re.search(r"\bpci_addr\s*=\s*" + var + r"\.ref\s*;", tail)
    assert m, "pci_addr = <norm>.ref 대입 없음"


def check_worker_parsed_fields(vm: str) -> None:
    w = body(vm, "create_vm_thread(GTask")
    assert not re.search(r"\bsscanf\(\s*data->pci_addr", w), "워커 본문에 sscanf(data->pci_addr 금지"
    parse = first(w, r"\bpurecvisor_vm_sriov_pci_parse\(")
    assert parse < first(w, r"\bpurecvisor_vm_provision_")
    br = w[parse:parse + 600]
    br = br[:br.index("return;") + len("return;")] if "return;" in br else br
    assert "G_IO_ERROR_INVALID_ARGUMENT" in br and "return;" in br, br
    for m in re.finditer(r"\bg_strdup_printf\(", w):
        a = call_args(w, m.start())
        if "type='pci'" in a:
            tail = re.sub(r"\s+", "", a)
            assert tail.endswith("sriov_pci[0],sriov_pci[1],sriov_pci[2],sriov_pci[3]"), tail
            return
    raise AssertionError("type='pci' hostdev g_strdup_printf 없음")


def test_sriov_pci_checked_after_gate_before_lock():
    check_sriov_pci_order(code(VM_CREATE))


def test_sriov_pci_replaced_with_normalized_ref():
    check_sriov_pci_replaced(code(VM_CREATE))


def test_worker_uses_parsed_fields():
    check_worker_parsed_fields(code(VM_MANAGER))


def test_counterfactual_sriov_pci_fails():
    create, vm = code(VM_CREATE), code(VM_MANAGER)
                                             
    start = create.index("if (g_strcmp0(nic_type, \"sriov\") == 0) {\n        if (!pci_addr)")
    a1_at = create.index("g_autofree gchar *sriov_bridge", start)
    end = _block_end(create, create.index("if (", a1_at)) + len("\n    }")
    block = create[start:end]
    dropped = create[:start] + create[end:]
    lock_at = create.index(";", create.index("pcv_vm_lock_acquire(name, VM_OP_CREATING")) + 1
    moved = create[:start] + create[end:lock_at] + "\n" + block + "\n" + create[lock_at:]
    for mutated in (dropped, moved):
        assert mutated != create
        with pytest.raises(AssertionError):
            check_sriov_pci_order(mutated)
    no_assign = create.replace("pci_addr = pci_norm.ref;", ";", 1)
    assert no_assign != create
    with pytest.raises(AssertionError):
        check_sriov_pci_replaced(no_assign)
                                                                
    w_at = vm.index("create_vm_thread(GTask")
    call = re.search(r"\n[^\n]*\n?\s*if \(g_strcmp0\(data->nic_type, \"sriov\"\) == 0 &&\s*!purecvisor_vm_sriov_pci_parse\(.*?return;\s*\}", vm[w_at:], re.S)
    assert call
    cut = vm[:w_at] + vm[w_at:].replace(call.group(0), "", 1)
    prov = cut.index(";", cut.index("purecvisor_vm_provision_zvol_disk(zvol_name", w_at)) + 1
    later = cut[:prov] + "\n" + call.group(0) + "\n" + cut[prov:]
    zeros = vm.replace("sriov_pci[0], sriov_pci[1], sriov_pci[2], sriov_pci[3]);", "0, 0, 0, 0);", 1)
    for mutated in (later, zeros):
        assert mutated != vm
        with pytest.raises(AssertionError):
            check_worker_parsed_fields(mutated)


def check_worker_sriov_nic_check(vm: str) -> None:
    w = body(vm, "create_vm_thread(GTask")
    call = first(w, r"\bpurecvisor_vm_sriov_nic_check\(")
    assert first(w, r"\bpurecvisor_vm_sriov_pci_parse\(") < call < first(w, r"\bpurecvisor_vm_provision_")
    line_start = w.rfind("\n", 0, call) + 1
    assert w[line_start:call].strip() == "if (!", "도우미 호출은 줄 첫머리 if (!로 시작해야 한다(다른 조건 안에 숨지 않음)"
    br = w[call:call + 400]
    assert "return;" in br and "g_task_return_error(" in br, br
    args = re.sub(r"\s+", "", call_args(w, call))
    assert args.startswith("data->nic_type,data->network_bridge,data->network_mode,"), args


def test_worker_sriov_nic_check_before_provision():
    check_worker_sriov_nic_check(code(VM_MANAGER))


def test_counterfactual_worker_sriov_nic_check_fails():
    vm = code(VM_MANAGER)
    w_at = vm.index("create_vm_thread(GTask")
    call = re.search(r"\n[^\n]*if \(!purecvisor_vm_sriov_nic_check\(.*?return;\s*\}", vm[w_at:], re.S)
    assert call, "구현 전: 워커 호출 문장 없음"
    cut = vm[:w_at] + vm[w_at:].replace(call.group(0), "", 1)
    prov = cut.index(";", cut.index("purecvisor_vm_provision_zvol_disk(zvol_name", w_at)) + 1
    later = cut[:prov] + "\n" + call.group(0) + "\n" + cut[prov:]
    for mutated in (cut, later):
        assert mutated != vm
        with pytest.raises(AssertionError):
            check_worker_sriov_nic_check(mutated)


                                                                                        

def _block_end(text: str, if_at: int) -> int:
                                                  
    i = text.index("{", if_at)
    depth = 0
    for j in range(i, len(text)):
        depth += (text[j] == "{") - (text[j] == "}")
        if depth == 0:
            return j + 1
    raise AssertionError(text[if_at:if_at + 80])


def check_sriov_bridge_order(create: str) -> None:
    h = body(create, "handle_vm_create(PureCVisorDispatcher")
    msg = first(h, r"\bPCV_VM_SRIOV_NO_BRIDGE_MSG\b")
    assert first(h, r"\bpcv_res_normalize\(\s*PCV_RES_PCI") < msg
    assert first(h, r"\bpci_addr\s*=\s*pci_norm\.ref\s*;") < msg < first(h, r"\bpcv_vm_lock_acquire\(")
    assert "purecvisor_vm_resolve_network_bridge(" in h[max(0, msg - 240):msg], "A1 문구 앞 240자 안에 브리지 해석 호출이 있어야 한다"


def check_sriov_overlay_order(create: str) -> None:
    h = body(create, "handle_vm_create(PureCVisorDispatcher")
    msg = first(h, r"\bPCV_VM_SRIOV_OVERLAY_MSG\b")
    assert first(h, r"\bpcv_vm_validate_network_mode_tenant\(") < msg
    assert msg < first(h, r"\b_gate_overlay_tenant\(") and msg < first(h, r"\bqos_applicable\(")
                                                                                        
    cond = re.sub(r"\s+", "", h[h.rfind("if (", 0, msg):msg])
    assert 'g_strcmp0(nic_type,"sriov")==0' in cond and 'g_strcmp0(network_mode,"tenant-overlay")==0' in cond, cond


def _move_after(text: str, start: int, end: int, anchor_stmt: str) -> str:
    block = text[start:end]
    cut = text[:start] + text[end:]
    at = cut.index(";", cut.index(anchor_stmt)) + 1
    return cut[:at] + "\n" + block + "\n" + cut[at:]


def test_sriov_bridge_checked_in_block_before_lock():
    check_sriov_bridge_order(code(VM_CREATE))


def test_sriov_overlay_checked_after_combo_before_tenant_gate():
    check_sriov_overlay_order(code(VM_CREATE))


def test_counterfactual_sriov_bridge_overlay_fails():
    create = code(VM_CREATE)
    a1 = create.index("g_autofree gchar *sriov_bridge")
    a1_end = _block_end(create, create.index("if (", a1))
    q2 = create.index("if (g_strcmp0(nic_type, \"sriov\") == 0 &&\n        g_strcmp0(network_mode")
    q2_end = _block_end(create, q2)
    for mutated in (create[:a1] + create[a1_end:],
                    _move_after(create, a1, a1_end, "pcv_vm_lock_acquire(name, VM_OP_CREATING")):
        assert mutated != create
        with pytest.raises(AssertionError):
            check_sriov_bridge_order(mutated)
    for mutated in (create[:q2] + create[q2_end:],
                    _move_after(create, q2, q2_end, "_gate_overlay_tenant(params")):
        assert mutated != create
        with pytest.raises(AssertionError):
            check_sriov_overlay_order(mutated)


def test_counterfactual_sriov_conditions_and_args_fail():
    create, vm = code(VM_CREATE), code(VM_MANAGER)
    no_sriov = create.replace('if (g_strcmp0(nic_type, "sriov") == 0 &&\n        g_strcmp0(network_mode, "tenant-overlay") == 0)',
                              'if (g_strcmp0(network_mode, "tenant-overlay") == 0)', 1)
    no_overlay = create.replace('if (g_strcmp0(nic_type, "sriov") == 0 &&\n        g_strcmp0(network_mode, "tenant-overlay") == 0)',
                                'if (g_strcmp0(nic_type, "sriov") == 0)', 1)
    for mutated in (no_sriov, no_overlay):
        assert mutated != create
        with pytest.raises(AssertionError):
            check_sriov_overlay_order(mutated)
    null_mode = vm.replace("data->network_bridge,\n                                       data->network_mode, &nic_err)",
                           "data->network_bridge,\n                                       NULL, &nic_err)", 1)
    swapped = vm.replace("(data->nic_type, data->network_bridge,\n                                       data->network_mode,",
                         "(data->nic_type, data->network_mode,\n                                       data->network_bridge,", 1)
    for mutated in (null_mode, swapped):
        assert mutated != vm
        with pytest.raises(AssertionError):
            check_worker_sriov_nic_check(mutated)
