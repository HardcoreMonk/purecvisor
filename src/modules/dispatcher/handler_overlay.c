
#include "handler_overlay.h"
#include "modules/network/ovs_overlay.h"
#include "modules/storage/iscsi_manager.h"
#include "modules/dispatcher/rpc_utils.h"
#include "utils/pcv_config.h"
#include "utils/pcv_validate.h"

void handle_overlay_create(JsonObject *params, const gchar *rpc_id,
                            UdsServer *server, GSocketConnection *connection)
{

    const gchar *name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : "pcvoverlay0";
    gint vni = json_object_has_member(params, "vni")
        ? (gint)json_object_get_int_member(params, "vni") : 100;
    const gchar *cidr = json_object_has_member(params, "cidr")
        ? json_object_get_string_member(params, "cidr") : NULL;

    if (!pcv_validate_bridge_name(name)) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid: name");
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
        return;
    }
    if (cidr && !pcv_validate_cidr(cidr)) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid: cidr");
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
        return;
    }

    GError *err = NULL;
    if (!pcv_overlay_create(name, vni, cidr, &err)) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            err ? err->message : "overlay create failed");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp); if (err) g_error_free(err);
        return;
    }

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "created");
    json_object_set_string_member(res, "name", name);
    json_object_set_int_member(res, "vni", vni);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

void handle_overlay_delete(JsonObject *params, const gchar *rpc_id,
                            UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : "pcvoverlay0";

    GError *err = NULL;
    pcv_overlay_delete(name, &err);
    if (err) g_error_free(err);

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "deleted");
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

void handle_overlay_list(JsonObject *params, const gchar *rpc_id,
                          UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    JsonArray *arr = pcv_overlay_list();
    JsonNode *node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(node, arr);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

void handle_overlay_info(JsonObject *params, const gchar *rpc_id,
                          UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : "pcvoverlay0";

    JsonObject *info = pcv_overlay_info(name);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, info);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

void handle_overlay_add_peer(JsonObject *params, const gchar *rpc_id,
                              UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : "pcvoverlay0";
    const gchar *peer_ip = json_object_has_member(params, "peer_ip")
        ? json_object_get_string_member(params, "peer_ip") : NULL;

    if (!peer_ip || !*peer_ip) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing: peer_ip");
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
        return;
    }

    if (!pcv_validate_bridge_name(name) || !pcv_validate_ip_literal(peer_ip)) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid: name or peer_ip");
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
        return;
    }

    GError *err = NULL;
    if (!pcv_overlay_add_peer(name, peer_ip, &err)) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            err ? err->message : "add_peer failed");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp); if (err) g_error_free(err);
        return;
    }

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "added");
    json_object_set_string_member(res, "peer_ip", peer_ip);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

void handle_overlay_remove_peer(JsonObject *params, const gchar *rpc_id,
                                 UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : "pcvoverlay0";
    const gchar *peer_ip = json_object_has_member(params, "peer_ip")
        ? json_object_get_string_member(params, "peer_ip") : NULL;

    if (!peer_ip) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing: peer_ip");
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
        return;
    }

    pcv_overlay_remove_peer(name, peer_ip, NULL);

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "removed");
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

void handle_iscsi_target_create(JsonObject *params, const gchar *rpc_id,
                                 UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm_name = json_object_has_member(params, "vm_name")
        ? json_object_get_string_member(params, "vm_name") : NULL;
    const gchar *zvol = json_object_has_member(params, "zvol_path")
        ? json_object_get_string_member(params, "zvol_path") : NULL;

    if (!vm_name || !zvol) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing: vm_name or zvol_path");
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
        return;
    }

    GError *err = NULL;
    if (!pcv_iscsi_target_create(vm_name, zvol, &err)) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            err ? err->message : "target create failed");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp); if (err) g_error_free(err);
        return;
    }

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "created");
    json_object_set_string_member(res, "vm_name", vm_name);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

void handle_iscsi_target_delete(JsonObject *params, const gchar *rpc_id,
                                 UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm_name = json_object_has_member(params, "vm_name")
        ? json_object_get_string_member(params, "vm_name") : NULL;

    if (!vm_name) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing: vm_name");
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
        return;
    }

    pcv_iscsi_target_delete(vm_name, NULL);

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "deleted");
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

void handle_iscsi_target_list(JsonObject *params, const gchar *rpc_id,
                               UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    JsonArray *arr = pcv_iscsi_target_list();
    JsonNode *node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(node, arr);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

void handle_iscsi_connect(JsonObject *params, const gchar *rpc_id,
                           UdsServer *server, GSocketConnection *connection)
{
    const gchar *target_ip = json_object_has_member(params, "target_ip")
        ? json_object_get_string_member(params, "target_ip") : NULL;
    const gchar *vm_name = json_object_has_member(params, "vm_name")
        ? json_object_get_string_member(params, "vm_name") : NULL;

    if (!target_ip || !vm_name) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing: target_ip or vm_name");
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
        return;
    }

    gchar *dev_path = NULL;
    GError *err = NULL;
    if (!pcv_iscsi_initiator_connect(target_ip, vm_name, &dev_path, &err)) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            err ? err->message : "connect failed");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp); if (err) g_error_free(err);
        return;
    }

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "connected");
    json_object_set_string_member(res, "device_path", dev_path ? dev_path : "pending");
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp); g_free(dev_path);
}

void handle_iscsi_disconnect(JsonObject *params, const gchar *rpc_id,
                              UdsServer *server, GSocketConnection *connection)
{
    const gchar *target_ip = json_object_has_member(params, "target_ip")
        ? json_object_get_string_member(params, "target_ip") : NULL;
    const gchar *vm_name = json_object_has_member(params, "vm_name")
        ? json_object_get_string_member(params, "vm_name") : NULL;

    if (!target_ip || !vm_name) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing: target_ip or vm_name");
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
        return;
    }

    pcv_iscsi_initiator_disconnect(target_ip, vm_name, NULL);

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "disconnected");
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

#include "modules/network/ovn_manager.h"

void handle_ovn_switch_create(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c) {
    const gchar *name = json_object_has_member(p,"name") ? json_object_get_string_member(p,"name") : NULL;
    const gchar *subnet = json_object_has_member(p,"subnet") ? json_object_get_string_member(p,"subnet") : NULL;
    if (!name) { gchar *r=pure_rpc_build_error_response(id,PURE_RPC_ERR_INVALID_PARAMS,"Missing: name"); pure_uds_server_send_response(s,c,r); g_free(r); return; }
    GError *e=NULL; pcv_ovn_switch_create(name,subnet,&e);
    if (e) { gchar *r=pure_rpc_build_error_response(id,PURE_RPC_ERR_ZFS_OPERATION,e->message); pure_uds_server_send_response(s,c,r); g_free(r); g_error_free(e); return; }
    JsonObject *res=json_object_new(); json_object_set_string_member(res,"status","created");
    JsonNode *n=json_node_new(JSON_NODE_OBJECT); json_node_take_object(n,res);
    gchar *r=pure_rpc_build_success_response(id,n); pure_uds_server_send_response(s,c,r); g_free(r);
}

void handle_ovn_switch_delete(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c) {
    const gchar *name = json_object_has_member(p,"name") ? json_object_get_string_member(p,"name") : NULL;
    if (name) pcv_ovn_switch_delete(name,NULL);
    JsonObject *res=json_object_new(); json_object_set_string_member(res,"status","deleted");
    JsonNode *n=json_node_new(JSON_NODE_OBJECT); json_node_take_object(n,res);
    gchar *r=pure_rpc_build_success_response(id,n); pure_uds_server_send_response(s,c,r); g_free(r);
}

void handle_ovn_switch_list(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c) {
    (void)p; JsonArray *a=pcv_ovn_switch_list(); JsonNode *n=json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(n,a); gchar *r=pure_rpc_build_success_response(id,n);
    pure_uds_server_send_response(s,c,r); g_free(r);
}

void handle_ovn_port_add(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c) {
    const gchar *sw=json_object_has_member(p,"switch")?json_object_get_string_member(p,"switch"):NULL;
    const gchar *port=json_object_has_member(p,"port")?json_object_get_string_member(p,"port"):NULL;
    const gchar *mac=json_object_has_member(p,"mac")?json_object_get_string_member(p,"mac"):NULL;
    const gchar *ip=json_object_has_member(p,"ip")?json_object_get_string_member(p,"ip"):NULL;
    if (!sw||!port) { gchar *r=pure_rpc_build_error_response(id,PURE_RPC_ERR_INVALID_PARAMS,"Missing: switch,port"); pure_uds_server_send_response(s,c,r); g_free(r); return; }
    GError *e=NULL; pcv_ovn_port_add(sw,port,mac,ip,&e);
    if (e) { gchar *r=pure_rpc_build_error_response(id,PURE_RPC_ERR_ZFS_OPERATION,e->message); pure_uds_server_send_response(s,c,r); g_free(r); g_error_free(e); return; }
    JsonObject *res=json_object_new(); json_object_set_string_member(res,"status","added");
    JsonNode *n=json_node_new(JSON_NODE_OBJECT); json_node_take_object(n,res);
    gchar *r=pure_rpc_build_success_response(id,n); pure_uds_server_send_response(s,c,r); g_free(r);
}

void handle_ovn_port_remove(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c) {
    const gchar *sw=json_object_has_member(p,"switch")?json_object_get_string_member(p,"switch"):"";
    const gchar *port=json_object_has_member(p,"port")?json_object_get_string_member(p,"port"):NULL;
    if (port) pcv_ovn_port_remove(sw,port,NULL);
    JsonObject *res=json_object_new(); json_object_set_string_member(res,"status","removed");
    JsonNode *n=json_node_new(JSON_NODE_OBJECT); json_node_take_object(n,res);
    gchar *r=pure_rpc_build_success_response(id,n); pure_uds_server_send_response(s,c,r); g_free(r);
}

void handle_ovn_acl_add(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c) {
    const gchar *sw=json_object_has_member(p,"switch")?json_object_get_string_member(p,"switch"):NULL;
    const gchar *dir=json_object_has_member(p,"direction")?json_object_get_string_member(p,"direction"):"to-lport";
    gint pri=json_object_has_member(p,"priority")?(gint)json_object_get_int_member(p,"priority"):1000;
    const gchar *match=json_object_has_member(p,"match")?json_object_get_string_member(p,"match"):NULL;
    const gchar *action=json_object_has_member(p,"action")?json_object_get_string_member(p,"action"):"allow";
    if (!sw||!match) { gchar *r=pure_rpc_build_error_response(id,PURE_RPC_ERR_INVALID_PARAMS,"Missing: switch,match"); pure_uds_server_send_response(s,c,r); g_free(r); return; }
    GError *e=NULL; pcv_ovn_acl_add(sw,dir,pri,match,action,&e);
    if (e) { gchar *r=pure_rpc_build_error_response(id,PURE_RPC_ERR_ZFS_OPERATION,e->message); pure_uds_server_send_response(s,c,r); g_free(r); g_error_free(e); return; }
    JsonObject *res=json_object_new(); json_object_set_string_member(res,"status","added");
    JsonNode *n=json_node_new(JSON_NODE_OBJECT); json_node_take_object(n,res);
    gchar *r=pure_rpc_build_success_response(id,n); pure_uds_server_send_response(s,c,r); g_free(r);
}

void handle_ovn_acl_list(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c) {
    const gchar *sw=json_object_has_member(p,"switch")?json_object_get_string_member(p,"switch"):NULL;
    JsonArray *a=pcv_ovn_acl_list(sw); JsonNode *n=json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(n,a); gchar *r=pure_rpc_build_success_response(id,n);
    pure_uds_server_send_response(s,c,r); g_free(r);
}

void handle_ovn_router_create(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c) {
    const gchar *name=json_object_has_member(p,"name")?json_object_get_string_member(p,"name"):NULL;
    if (!name) { gchar *r=pure_rpc_build_error_response(id,PURE_RPC_ERR_INVALID_PARAMS,"Missing: name"); pure_uds_server_send_response(s,c,r); g_free(r); return; }
    GError *e=NULL; pcv_ovn_router_create(name,&e);
    if (e) { gchar *r=pure_rpc_build_error_response(id,PURE_RPC_ERR_ZFS_OPERATION,e->message); pure_uds_server_send_response(s,c,r); g_free(r); g_error_free(e); return; }
    JsonObject *res=json_object_new(); json_object_set_string_member(res,"status","created");
    JsonNode *n=json_node_new(JSON_NODE_OBJECT); json_node_take_object(n,res);
    gchar *r=pure_rpc_build_success_response(id,n); pure_uds_server_send_response(s,c,r); g_free(r);
}

void handle_ovn_status(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c) {
    (void)p; JsonObject *st=pcv_ovn_status(); JsonNode *n=json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n,st); gchar *r=pure_rpc_build_success_response(id,n);
    pure_uds_server_send_response(s,c,r); g_free(r);
}

void handle_ovn_router_delete(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c) {
    const gchar *name=json_object_has_member(p,"name")?json_object_get_string_member(p,"name"):NULL;
    if (!name) { gchar *r=pure_rpc_build_error_response(id,PURE_RPC_ERR_INVALID_PARAMS,"Missing: name"); pure_uds_server_send_response(s,c,r); g_free(r); return; }
    GError *e=NULL; pcv_ovn_router_delete(name,&e);
    if (e) { gchar *r=pure_rpc_build_error_response(id,PURE_RPC_ERR_ZFS_OPERATION,e->message); pure_uds_server_send_response(s,c,r); g_free(r); g_error_free(e); return; }
    JsonObject *res=json_object_new(); json_object_set_string_member(res,"status","deleted");
    json_object_set_string_member(res,"name",name);
    JsonNode *n=json_node_new(JSON_NODE_OBJECT); json_node_take_object(n,res);
    gchar *r=pure_rpc_build_success_response(id,n); pure_uds_server_send_response(s,c,r); g_free(r);
}

void handle_ovn_router_list(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c) {
    (void)p; JsonArray *a=pcv_ovn_router_list(); JsonNode *n=json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(n,a); gchar *r=pure_rpc_build_success_response(id,n);
    pure_uds_server_send_response(s,c,r); g_free(r);
}

void handle_ovn_router_add_port(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c) {
    const gchar *router=json_object_has_member(p,"router")?json_object_get_string_member(p,"router"):NULL;
    const gchar *sw=json_object_has_member(p,"switch")?json_object_get_string_member(p,"switch"):NULL;
    const gchar *mac=json_object_has_member(p,"mac")?json_object_get_string_member(p,"mac"):NULL;
    const gchar *cidr=json_object_has_member(p,"cidr")?json_object_get_string_member(p,"cidr"):NULL;
    if (!router||!sw||!mac||!cidr) { gchar *r=pure_rpc_build_error_response(id,PURE_RPC_ERR_INVALID_PARAMS,"Missing: router,switch,mac,cidr"); pure_uds_server_send_response(s,c,r); g_free(r); return; }
    GError *e=NULL; pcv_ovn_router_add_port(router,sw,mac,cidr,&e);
    if (e) { gchar *r=pure_rpc_build_error_response(id,PURE_RPC_ERR_ZFS_OPERATION,e->message); pure_uds_server_send_response(s,c,r); g_free(r); g_error_free(e); return; }
    JsonObject *res=json_object_new(); json_object_set_string_member(res,"status","connected");
    json_object_set_string_member(res,"router",router); json_object_set_string_member(res,"switch",sw);
    JsonNode *n=json_node_new(JSON_NODE_OBJECT); json_node_take_object(n,res);
    gchar *r=pure_rpc_build_success_response(id,n); pure_uds_server_send_response(s,c,r); g_free(r);
}

void handle_ovn_dhcp_enable(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c) {
    const gchar *subnet=json_object_has_member(p,"subnet")?json_object_get_string_member(p,"subnet"):NULL;
    const gchar *gw=json_object_has_member(p,"gateway")?json_object_get_string_member(p,"gateway"):NULL;
    if (!subnet||!gw) { gchar *r=pure_rpc_build_error_response(id,PURE_RPC_ERR_INVALID_PARAMS,"Missing: subnet,gateway"); pure_uds_server_send_response(s,c,r); g_free(r); return; }
    GError *e=NULL; pcv_ovn_dhcp_enable(subnet,gw,&e);
    if (e) { gchar *r=pure_rpc_build_error_response(id,PURE_RPC_ERR_ZFS_OPERATION,e->message); pure_uds_server_send_response(s,c,r); g_free(r); g_error_free(e); return; }
    JsonObject *res=json_object_new(); json_object_set_string_member(res,"status","enabled");
    json_object_set_string_member(res,"subnet",subnet);
    JsonNode *n=json_node_new(JSON_NODE_OBJECT); json_node_take_object(n,res);
    gchar *r=pure_rpc_build_success_response(id,n); pure_uds_server_send_response(s,c,r); g_free(r);
}

void handle_ovn_nat_add(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c) {
    const gchar *router=json_object_has_member(p,"router")?json_object_get_string_member(p,"router"):NULL;
    const gchar *type=json_object_has_member(p,"type")?json_object_get_string_member(p,"type"):NULL;
    const gchar *ext_ip=json_object_has_member(p,"external_ip")?json_object_get_string_member(p,"external_ip"):NULL;
    const gchar *log_ip=json_object_has_member(p,"logical_ip")?json_object_get_string_member(p,"logical_ip"):NULL;
    if (!router||!type||!ext_ip||!log_ip) { gchar *r=pure_rpc_build_error_response(id,PURE_RPC_ERR_INVALID_PARAMS,"Missing: router,type,external_ip,logical_ip"); pure_uds_server_send_response(s,c,r); g_free(r); return; }
    GError *e=NULL; pcv_ovn_nat_add(router,type,ext_ip,log_ip,&e);
    if (e) { gchar *r=pure_rpc_build_error_response(id,PURE_RPC_ERR_ZFS_OPERATION,e->message); pure_uds_server_send_response(s,c,r); g_free(r); g_error_free(e); return; }
    JsonObject *res=json_object_new(); json_object_set_string_member(res,"status","added");
    json_object_set_string_member(res,"router",router); json_object_set_string_member(res,"type",type);
    JsonNode *n=json_node_new(JSON_NODE_OBJECT); json_node_take_object(n,res);
    gchar *r=pure_rpc_build_success_response(id,n); pure_uds_server_send_response(s,c,r); g_free(r);
}

void handle_ovn_nat_list(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c) {
    const gchar *router=json_object_has_member(p,"router")?json_object_get_string_member(p,"router"):NULL;
    if (!router) { gchar *r=pure_rpc_build_error_response(id,PURE_RPC_ERR_INVALID_PARAMS,"Missing: router"); pure_uds_server_send_response(s,c,r); g_free(r); return; }
    JsonArray *a=pcv_ovn_nat_list(router); JsonNode *n=json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(n,a); gchar *r=pure_rpc_build_success_response(id,n);
    pure_uds_server_send_response(s,c,r); g_free(r);
}

void handle_ovn_tenant_create(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c) {
    const gchar *tenant=json_object_has_member(p,"tenant")?json_object_get_string_member(p,"tenant"):NULL;
    const gchar *subnet=json_object_has_member(p,"subnet")?json_object_get_string_member(p,"subnet"):NULL;
    if (!tenant||!subnet) { gchar *r=pure_rpc_build_error_response(id,PURE_RPC_ERR_INVALID_PARAMS,"Missing: tenant,subnet"); pure_uds_server_send_response(s,c,r); g_free(r); return; }
    GError *e=NULL; pcv_ovn_tenant_create(tenant,subnet,&e);
    if (e) { gchar *r=pure_rpc_build_error_response(id,PURE_RPC_ERR_ZFS_OPERATION,e->message); pure_uds_server_send_response(s,c,r); g_free(r); g_error_free(e); return; }
    JsonObject *res=json_object_new(); json_object_set_string_member(res,"status","created");
    json_object_set_string_member(res,"tenant",tenant);

    gchar *sw_name=g_strdup_printf("tenant-%s-ls",tenant);
    json_object_set_string_member(res,"switch",sw_name); g_free(sw_name);
    JsonNode *n=json_node_new(JSON_NODE_OBJECT); json_node_take_object(n,res);
    gchar *r=pure_rpc_build_success_response(id,n); pure_uds_server_send_response(s,c,r); g_free(r);
}

void handle_ovn_switch_detail(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c) {
    const gchar *name=json_object_has_member(p,"name")?json_object_get_string_member(p,"name"):NULL;
    if (!name) { gchar *r=pure_rpc_build_error_response(id,PURE_RPC_ERR_INVALID_PARAMS,"Missing: name"); pure_uds_server_send_response(s,c,r); g_free(r); return; }
    JsonObject *detail=pcv_ovn_switch_detail(name);
    JsonNode *n=json_node_new(JSON_NODE_OBJECT); json_node_take_object(n,detail);
    gchar *r=pure_rpc_build_success_response(id,n); pure_uds_server_send_response(s,c,r); g_free(r);
}

void handle_ovn_router_detail(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c) {
    const gchar *name=json_object_has_member(p,"name")?json_object_get_string_member(p,"name"):NULL;
    if (!name) { gchar *r=pure_rpc_build_error_response(id,PURE_RPC_ERR_INVALID_PARAMS,"Missing: name"); pure_uds_server_send_response(s,c,r); g_free(r); return; }
    JsonObject *detail=pcv_ovn_router_detail(name);
    JsonNode *n=json_node_new(JSON_NODE_OBJECT); json_node_take_object(n,detail);
    gchar *r=pure_rpc_build_success_response(id,n); pure_uds_server_send_response(s,c,r); g_free(r);
}
