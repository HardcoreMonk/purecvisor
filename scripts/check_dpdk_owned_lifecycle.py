#!/usr/bin/env python3












from __future__ import annotations

import re
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent


def _read(relative: str) -> str:
    path = REPO_ROOT / relative
    try:
        return path.read_text(encoding="utf-8")
    except OSError as exc:
        print(f"ERROR: failed to read {path}: {exc}", file=sys.stderr)
        sys.exit(2)


def _function_body(text: str, name: str) -> str:
    match = re.search(rf"\b{re.escape(name)}\s*\([^;]*?\)\s*\{{", text, re.DOTALL)
    if not match:
        print(f"FAIL: missing {name}()", file=sys.stderr)
        sys.exit(1)

    depth = 0
    start = match.end() - 1
    for pos in range(start, len(text)):
        if text[pos] == "{":
            depth += 1
        elif text[pos] == "}":
            depth -= 1
            if depth == 0:
                return text[start + 1:pos]

    print(f"ERROR: could not parse {name}()", file=sys.stderr)
    sys.exit(2)


def _require(condition: bool, message: str) -> None:
    if not condition:
        print(f"FAIL: {message}", file=sys.stderr)
        sys.exit(1)


def _ordered(body: str, first: str, second: str, message: str) -> None:
    first_pos = body.find(first)
    second_pos = body.find(second)
    _require(first_pos >= 0 and second_pos >= 0 and first_pos < second_pos, message)


def main() -> int:
    manager = _read("src/modules/network/dpdk_manager.c")
    vm_manager = _read("src/modules/virt/vm_manager.c")
    vm_start = _read("src/modules/dispatcher/handler_vm_start.c")
    vm_lifecycle = _read("src/modules/dispatcher/handler_vm_lifecycle.c")
    dispatcher = _read("src/api/dispatcher.c")
    accel = _read("src/modules/dispatcher/handler_accel.c")
    cli = _read("src/cli/purecvisorctl.c")
    config = _read("src/utils/pcv_config.c")
    service = _read("packaging/deb/purecvisorsd.service")
    deb_builder = _read("packaging/deb/build-deb.sh")
    deploy = _read("scripts/deploy.sh")

    bridge_create = _function_body(manager, "pcv_dpdk_bridge_create")
    for token in (
        "datapath_type=netdev",
        "DPDK_OWNER_BRIDGE",
        "DPDK_MTU_KEY",
        "type=dpdk",
        "options:dpdk-devargs=",
        "mtu_request=",
        "wait-until",
    ):
        _require(token in bridge_create, f"bridge create is missing {token}")

    _require('DPDK_VHOST_RUNTIME_DIR "/run/libvirt/qemu"' in manager,
             "canonical DPDK vhost runtime must be /run/libvirt/qemu")
    _require("DPDK_SOCK_DIR" not in manager,
             "legacy DPDK_SOCK_DIR must not return as the canonical generator")

    path_builder = _function_body(manager, "pcv_dpdk_vhost_socket_path")
    for token in ("DPDK_VHOST_RUNTIME_DIR", "purecvisor-vhost-", "sun_path"):
        _require(token in path_builder, f"canonical vhost path builder is missing {token}")

    port_ensure = _function_body(manager, "pcv_dpdk_vm_port_ensure_endpoint")
    for token in (
        "dpdkvhostuserclient",
        "options:vhost-server-path=",
        "DPDK_OWNER_VHOST",
        "DPDK_BRIDGE_KEY",
        "DPDK_VM_KEY",
        "mtu_request=",
        "pcv_dpdk_vhost_runtime_preflight",
        '"type"',
        '"options:vhost-server-path"',
    ):
        _require(token in port_ensure, f"vhost ensure is missing {token}")
    port_wrapper = _function_body(manager, "pcv_dpdk_vm_port_ensure")
    _require("PCV_DPDK_VHOST_ENDPOINT_CANONICAL" in port_wrapper,
             "default vhost ensure wrapper must be canonical-only")

    create_worker = _function_body(vm_manager, "create_vm_thread")
    _ordered(create_worker, "pcv_dpdk_vhost_runtime_preflight",
             "purecvisor_vm_provision_file_disk",
             "DPDK runtime preflight must precede disk provisioning")
    _ordered(create_worker, "pcv_dpdk_vhost_runtime_preflight", "virDomainDefineXML",
             "DPDK runtime preflight must precede domain mutation")
    _ordered(create_worker, "virDomainDefineXML", "pcv_dpdk_vm_port_ensure",
             "VM create must define the domain before ensuring its DPDK port")
    _require("_dpdk_metadata_xml" in create_worker,
             "VM create must persist DPDK bridge metadata")
    _require("pcv_vm_dpdk_vhost_source_classify" in create_worker and
             "PCV_DPDK_VHOST_SOURCE_CANONICAL" in create_worker,
             "VM create must re-read one canonical domain vhost source")
    _require("purecvisor_vm_provision_file_disk" in create_worker,
             "VM create must use the fail-closed file disk provisioning leaf")

    file_provision = _function_body(vm_manager, "purecvisor_vm_provision_file_disk")
    for token in ("O_CREAT | O_EXCL", '"qemu-img", "create"',
                  '"qemu-img", "convert", "-n"', "g_unlink(disk_path)"):
        _require(token in file_provision,
                 f"file base image provisioning is missing {token}")

    iface_builder = _function_body(vm_manager, "_build_dpdk_iface_xml")
    _require("pcv_dpdk_vhost_socket_path" in iface_builder and
             "/var/run/purecvisor" not in iface_builder,
             "new DPDK XML must use only the canonical path helper")

    start_worker = _function_body(vm_start, "vm_start_worker_thread")
    _ordered(start_worker, "virDomainIsActive", "_reconcile_dpdk_vhost_for_start",
             "VM start must determine active state before DPDK mutation")
    _ordered(start_worker, "_reconcile_dpdk_vhost_for_start", "virDomainCreate",
             "VM start must reconcile the exact DPDK endpoint before domain start")

    dpdk_start = _function_body(vm_start, "_reconcile_dpdk_vhost_for_start")
    for token in (
        "pcv_vm_dpdk_metadata_read",
        "pcv_vm_dpdk_vhost_source_classify",
        "pcv_vm_dpdk_vhost_start_action",
        "PCV_DPDK_VHOST_START_MIGRATE_LEGACY",
        "PCV_DPDK_VHOST_START_DEFER_LEGACY",
        "pcv_vm_dpdk_vhost_migrate_legacy_xml",
        "VIR_DOMAIN_XML_INACTIVE",
        "pcv_dpdk_vm_port_ensure_endpoint",
        "PCV_DPDK_VHOST_ENDPOINT_ACTIVE_LEGACY",
        "migrated-legacy-to-canonical",
        "deferred-active-legacy",
    ):
        _require(token in dpdk_start, f"DPDK start state machine is missing {token}")
    _ordered(dpdk_start, "pcv_dpdk_vhost_runtime_preflight", "virDomainDefineXML",
             "inactive legacy migration must preflight before redefining XML")
    _ordered(dpdk_start, "virDomainDefineXML", "pcv_dpdk_vm_port_ensure_endpoint",
             "inactive migration must redefine/re-read before OVS reconciliation")
    _ordered(dpdk_start, "migrated-legacy-to-canonical",
             "pcv_dpdk_vm_port_ensure_endpoint",
             "persistent legacy migration must be audited before fallible OVS reconciliation")

    _require("g_mkdir_with_parents(sock_dir, 0700)" in config,
             "control UDS parent must remain root-only mode 0700")
    _require("chown root:root /var/run/purecvisor/daemon.sock" in service and
             "chmod 0600 /var/run/purecvisor/daemon.sock" in service,
             "control UDS package contract must remain root:root 0600")
    permission_surfaces = "\n".join((service, deb_builder, deploy))
    forbidden_runtime_mutation = re.search(
        r"(?:chmod|chown|setfacl|install\s+-d|mkdir)[^\n]*?/run/libvirt/qemu",
        permission_surfaces,
    )
    _require(forbidden_runtime_mutation is None,
             "PureCVisor packaging/deploy must not own or widen /run/libvirt/qemu")
    _require("setfacl" not in permission_surfaces,
             "control/runtime packaging must not introduce broad ACL mutation")

    delete_worker = _function_body(vm_lifecycle, "_vm_delete_worker")
    _ordered(delete_worker, "pcv_vm_dpdk_metadata_read", "pcv_dpdk_vm_port_delete",
             "VM delete must verify metadata before DPDK port cleanup")
    _ordered(delete_worker, "pcv_dpdk_vm_port_delete", "virDomainUndefineFlags",
             "VM delete must remove the DPDK port before undefining the domain")
    _require("virDomainDestroy(dom) < 0" in delete_worker,
             "VM delete must block undefine/storage deletion when active-domain destroy fails")
    restore_helper = _function_body(vm_lifecycle, "_vm_delete_restore_dpdk_port")
    _require("pcv_dpdk_vm_port_ensure" in restore_helper and
             delete_worker.count("_vm_delete_restore_dpdk_port") >= 5,
             "VM delete state/destroy/undefine/ZFS/file rollback paths must restore the DPDK port")

    rename = _function_body(vm_lifecycle, "handle_vm_rename_request")
    _require("pcv_vm_dpdk_metadata_read" in rename and "PCV_DPDK_META_OK" in rename,
             "vm.rename must fail closed for managed DPDK VMs")

    clone = _function_body(dispatcher, "_handle_vm_clone")
    _require("pcv_vm_dpdk_metadata_read" in clone and "PCV_DPDK_META_OK" in clone,
             "vm.clone must fail closed for managed DPDK VMs")

    bridge_handler = _function_body(accel, "handle_dpdk_bridge_create")
    _require("PCV_DPDK_MTU_MIN" in bridge_handler and
             "PCV_DPDK_MTU_MAX" in bridge_handler and
             "_dpdk_bridge_schedule" in bridge_handler,
             "dpdk.bridge.create must strictly validate and schedule MTU work")
    bridge_worker = _function_body(accel, "_dpdk_bridge_worker")
    _require("pcv_dpdk_bridge_create" in bridge_worker and
             "pcv_dpdk_bridge_delete" in bridge_worker,
             "DPDK bridge worker must execute create/delete manager effects")
    for token in ("pcv_job_update_status", "pcv_job_set_result",
                  "pcv_audit_log", "pcv_ws_broadcast_job_complete_mt"):
        _require(token in bridge_worker,
                 f"DPDK bridge worker terminal path is missing {token}")
    bridge_schedule = _function_body(accel, "_dpdk_bridge_schedule")
    for token in ("pcv_job_create", '"status", "accepted"',
                  '"job_id"', "pure_uds_server_send_response",
                  "g_task_run_in_thread"):
        _require(token in bridge_schedule,
                 f"DPDK bridge accepted/Job schedule path is missing {token}")
    _ordered(bridge_schedule, "pure_uds_server_send_response",
             "g_task_run_in_thread",
             "DPDK bridge must return the accepted Job before worker scheduling")
    _require("pure_uds_server_send_response" not in bridge_worker,
             "DPDK bridge worker must not reply on the closed request socket")

    cli_bridge = _function_body(cli, "cmd_dpdk_bridge")
    _require('"--mtu"' in cli_bridge and
             'json_object_set_int_member(params, "mtu", mtu)' in cli_bridge,
             "pcvctl DPDK bridge create must expose and forward --mtu")
    _require("_dpdk_bridge_wait_response" in cli_bridge,
             "pcvctl DPDK bridge mutation must wait for terminal Job state")

    print("[PASS] ADR-0053/0054 DPDK lifecycle and vhost runtime wiring is present")
    return 0


if __name__ == "__main__":
    sys.exit(main())
