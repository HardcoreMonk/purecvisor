
#include "handler_accel.h"
#include "modules/network/dpdk_manager.h"
#include "modules/network/sriov_manager.h"
#include "modules/dispatcher/rpc_utils.h"
#include "purecvisor/pcv_validate.h"

void handle_dpdk_status(JsonObject *p __attribute__((unused)), const gchar *id,
                         UdsServer *s, GSocketConnection *c)
{
    JsonObject *obj = pcv_dpdk_status();
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, obj);
    gchar *r = pure_rpc_build_success_response(id, n);
    pure_uds_server_send_response(s, c, r);
    g_free(r);
}

void handle_dpdk_hugepage_info(JsonObject *p __attribute__((unused)), const gchar *id,
                                UdsServer *s, GSocketConnection *c)
{
    JsonObject *obj = pcv_dpdk_hugepage_info();
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, obj);
    gchar *r = pure_rpc_build_success_response(id, n);
    pure_uds_server_send_response(s, c, r);
    g_free(r);
}

void handle_dpdk_bind(JsonObject *p, const gchar *id,
                       UdsServer *s, GSocketConnection *c)
{
    if (!json_object_has_member(p, "pci_addr")) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_INVALID_PARAMS, "Missing: pci_addr");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }
    const gchar *pci = json_object_get_string_member(p, "pci_addr");

    const gchar *drv = json_object_has_member(p, "driver")
        ? json_object_get_string_member(p, "driver") : NULL;

    gchar *guard_reason = NULL;
    if (pcv_dpdk_nic_is_protected(pci, &guard_reason)) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_INVALID_PARAMS,
            guard_reason ? guard_reason : "refusing to bind NIC in use by host");
        pure_uds_server_send_response(s, c, r);
        g_free(r); g_free(guard_reason);
        return;
    }
    g_free(guard_reason);

    GError *err = NULL;
    if (!pcv_dpdk_bind(pci, drv, &err)) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_ZFS_OPERATION,
            err ? err->message : "dpdk bind failed");
        pure_uds_server_send_response(s, c, r);
        g_free(r); if (err) g_error_free(err); return;
    }

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "bound");
    json_object_set_string_member(res, "pci_addr", pci);
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res);
    gchar *r = pure_rpc_build_success_response(id, n);
    pure_uds_server_send_response(s, c, r);
    g_free(r);
}

void handle_dpdk_unbind(JsonObject *p, const gchar *id,
                         UdsServer *s, GSocketConnection *c)
{
    if (!json_object_has_member(p, "pci_addr")) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_INVALID_PARAMS, "Missing: pci_addr");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }
    const gchar *pci = json_object_get_string_member(p, "pci_addr");

    GError *err = NULL;
    if (!pcv_dpdk_unbind(pci, &err)) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_ZFS_OPERATION,
            err ? err->message : "dpdk unbind failed");
        pure_uds_server_send_response(s, c, r);
        g_free(r); if (err) g_error_free(err); return;
    }

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "unbound");
    json_object_set_string_member(res, "pci_addr", pci);
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res);
    gchar *r = pure_rpc_build_success_response(id, n);
    pure_uds_server_send_response(s, c, r);
    g_free(r);
}

void handle_dpdk_list(JsonObject *p __attribute__((unused)), const gchar *id,
                       UdsServer *s, GSocketConnection *c)
{
    JsonArray *arr = pcv_dpdk_list();
    JsonNode *n = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(n, arr);
    gchar *r = pure_rpc_build_success_response(id, n);
    pure_uds_server_send_response(s, c, r);
    g_free(r);
}

void handle_dpdk_bridge_create(JsonObject *p, const gchar *id,
                                UdsServer *s, GSocketConnection *c)
{
    if (!json_object_has_member(p, "name")) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_INVALID_PARAMS, "Missing: name");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }
    const gchar *name = json_object_get_string_member(p, "name");

    if (!pcv_validate_bridge_name(name)) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid: name");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }

    const gchar *port = json_object_has_member(p, "dpdk_port")
        ? json_object_get_string_member(p, "dpdk_port") : NULL;

    if (port && *port && !pcv_validate_pci_addr(port)) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid: dpdk_port");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }

    GError *err = NULL;
    if (!pcv_dpdk_bridge_create(name, port, &err)) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_ZFS_OPERATION,
            err ? err->message : "dpdk bridge create failed");
        pure_uds_server_send_response(s, c, r);
        g_free(r); if (err) g_error_free(err); return;
    }

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "created");
    json_object_set_string_member(res, "name", name);
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res);
    gchar *r = pure_rpc_build_success_response(id, n);
    pure_uds_server_send_response(s, c, r);
    g_free(r);
}

void handle_dpdk_bridge_delete(JsonObject *p, const gchar *id,
                                UdsServer *s, GSocketConnection *c)
{
    if (!json_object_has_member(p, "name")) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_INVALID_PARAMS, "Missing: name");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }
    const gchar *name = json_object_get_string_member(p, "name");

    GError *err = NULL;
    if (!pcv_dpdk_bridge_delete(name, &err)) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_ZFS_OPERATION,
            err ? err->message : "dpdk bridge delete failed");
        pure_uds_server_send_response(s, c, r);
        g_free(r); if (err) g_error_free(err); return;
    }

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "deleted");
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res);
    gchar *r = pure_rpc_build_success_response(id, n);
    pure_uds_server_send_response(s, c, r);
    g_free(r);
}

void handle_sriov_status(JsonObject *p __attribute__((unused)), const gchar *id,
                          UdsServer *s, GSocketConnection *c)
{
    JsonObject *obj = pcv_sriov_status();
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, obj);
    gchar *r = pure_rpc_build_success_response(id, n);
    pure_uds_server_send_response(s, c, r);
    g_free(r);
}

void handle_sriov_enable(JsonObject *p, const gchar *id,
                          UdsServer *s, GSocketConnection *c)
{
    if (!json_object_has_member(p, "pf")) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_INVALID_PARAMS, "Missing: pf");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }
    const gchar *pf = json_object_get_string_member(p, "pf");

    gint num = json_object_has_member(p, "num_vfs")
        ? (gint)json_object_get_int_member(p, "num_vfs") : 1;

    GError *err = NULL;
    if (!pcv_sriov_enable(pf, num, &err)) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_ZFS_OPERATION,
            err ? err->message : "sriov enable failed");
        pure_uds_server_send_response(s, c, r);
        g_free(r); if (err) g_error_free(err); return;
    }

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "enabled");
    json_object_set_string_member(res, "pf", pf);
    json_object_set_int_member(res, "num_vfs", num);
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res);
    gchar *r = pure_rpc_build_success_response(id, n);
    pure_uds_server_send_response(s, c, r);
    g_free(r);
}

void handle_sriov_disable(JsonObject *p, const gchar *id,
                           UdsServer *s, GSocketConnection *c)
{
    if (!json_object_has_member(p, "pf")) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_INVALID_PARAMS, "Missing: pf");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }
    const gchar *pf = json_object_get_string_member(p, "pf");

    if (!pcv_validate_iface_name(pf)) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid: pf");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }

    GError *err = NULL;
    if (!pcv_sriov_disable(pf, &err)) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_ZFS_OPERATION,
            err ? err->message : "sriov disable failed");
        pure_uds_server_send_response(s, c, r);
        g_free(r); if (err) g_error_free(err); return;
    }

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "disabled");
    json_object_set_string_member(res, "pf", pf);
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res);
    gchar *r = pure_rpc_build_success_response(id, n);
    pure_uds_server_send_response(s, c, r);
    g_free(r);
}

void handle_sriov_list(JsonObject *p, const gchar *id,
                        UdsServer *s, GSocketConnection *c)
{
    const gchar *pf = json_object_has_member(p, "pf")
        ? json_object_get_string_member(p, "pf") : NULL;

    JsonArray *arr = pcv_sriov_list(pf);
    JsonNode *n = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(n, arr);
    gchar *r = pure_rpc_build_success_response(id, n);
    pure_uds_server_send_response(s, c, r);
    g_free(r);
}

void handle_sriov_set(JsonObject *p, const gchar *id,
                       UdsServer *s, GSocketConnection *c)
{
    if (!json_object_has_member(p, "pf") || !json_object_has_member(p, "vf_index")) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_INVALID_PARAMS, "Missing: pf, vf_index");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }

    const gchar *pf = json_object_get_string_member(p, "pf");
    gint vf_idx = (gint)json_object_get_int_member(p, "vf_index");

    if (!pcv_validate_iface_name(pf)) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid: pf");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }

    const gchar *mac = json_object_has_member(p, "mac")
        ? json_object_get_string_member(p, "mac") : NULL;
    gint vlan = json_object_has_member(p, "vlan")
        ? (gint)json_object_get_int_member(p, "vlan") : -1;
    gint spoof = json_object_has_member(p, "spoofchk")
        ? (gint)json_object_get_int_member(p, "spoofchk") : -1;

    if (mac && !pcv_validate_mac(mac)) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid: mac");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }

    GError *err = NULL;
    if (!pcv_sriov_set(pf, vf_idx, mac, vlan, spoof, &err)) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_ZFS_OPERATION,
            err ? err->message : "sriov set failed");
        pure_uds_server_send_response(s, c, r);
        g_free(r); if (err) g_error_free(err); return;
    }

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "configured");
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res);
    gchar *r = pure_rpc_build_success_response(id, n);
    pure_uds_server_send_response(s, c, r);
    g_free(r);
}

void handle_sriov_attach(JsonObject *p, const gchar *id,
                          UdsServer *s, GSocketConnection *c)
{
    if (!json_object_has_member(p, "vm_name") || !json_object_has_member(p, "pf")) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_INVALID_PARAMS, "Missing: vm_name, pf");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }

    const gchar *vm = json_object_get_string_member(p, "vm_name");
    const gchar *pf = json_object_get_string_member(p, "pf");

    if (!pcv_validate_vm_name(vm)) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid: vm_name");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }
    if (!pcv_validate_iface_name(pf)) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid: pf");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }

    gint vf_idx = json_object_has_member(p, "vf_index")
        ? (gint)json_object_get_int_member(p, "vf_index") : 0;

    GError *err = NULL;
    if (!pcv_sriov_attach_vm(vm, pf, vf_idx, &err)) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_ZFS_OPERATION,
            err ? err->message : "sriov attach failed");
        pure_uds_server_send_response(s, c, r);
        g_free(r); if (err) g_error_free(err); return;
    }

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "attached");
    json_object_set_string_member(res, "vm_name", vm);
    json_object_set_string_member(res, "pf", pf);
    json_object_set_int_member(res, "vf_index", vf_idx);
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res);
    gchar *r = pure_rpc_build_success_response(id, n);
    pure_uds_server_send_response(s, c, r);
    g_free(r);
}

void handle_sriov_detach(JsonObject *p, const gchar *id,
                          UdsServer *s, GSocketConnection *c)
{
    if (!json_object_has_member(p, "vm_name") || !json_object_has_member(p, "pci_addr")) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_INVALID_PARAMS, "Missing: vm_name, pci_addr");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }

    const gchar *vm = json_object_get_string_member(p, "vm_name");
    const gchar *pci = json_object_get_string_member(p, "pci_addr");

    if (!pcv_validate_vm_name(vm)) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid: vm_name");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }
    if (!pcv_validate_pci_addr(pci)) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid: pci_addr");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }

    GError *err = NULL;
    if (!pcv_sriov_detach_vm(vm, pci, &err)) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_ZFS_OPERATION,
            err ? err->message : "sriov detach failed");
        pure_uds_server_send_response(s, c, r);
        g_free(r); if (err) g_error_free(err); return;
    }

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "detached");
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res);
    gchar *r = pure_rpc_build_success_response(id, n);
    pure_uds_server_send_response(s, c, r);
    g_free(r);
}
