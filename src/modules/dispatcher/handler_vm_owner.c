                                                                             
                                            
                                                                                                                 
  
                           
                                                                                               
                                                                                                           
                                                                     
                                                                                             
                                                                                     
                                                                          
  
                       
                                                             
                                                                                
#include "modules/dispatcher/handler_vm_owner.h"

#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>

#include "api/grpc_server.h"                                            
#include "api/ws_server.h"                                              
#include "modules/audit/pcv_audit.h"                                         
#include "modules/auth/pcv_principal_names.h"                             
#include "modules/auth/pcv_rbac.h"
#include "modules/core/vm_state.h"
#include "modules/daemons/vm_diag_sampler.h"                                              
#include "modules/dispatcher/dispatcher_shared.h"
#include "modules/dispatcher/rpc_utils.h"
#include "modules/virt/virt_conn_pool.h"
#include "modules/virt/vm_owner_meta.h"
#include "utils/pcv_log.h"

                                                                                                                       
                                                                                                   
                                         

#define OWNER_INVALID "Invalid params: owner must be an existing user"

                 
static void
_reply_error(const gchar *rpc_id, UdsServer *server, GSocketConnection *connection, gint code, const gchar *msg)
{
    gchar *resp = pure_rpc_build_error_response(rpc_id, code, msg);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

                                                                                    
static const gchar *
_target(JsonObject *params)
{
    static const gchar *const keys[] = { "name", "vm_id", "vm_name", "vm" };
    for (guint i = 0; params && i < G_N_ELEMENTS(keys); i++) {
        JsonNode *n = json_object_get_member(params, keys[i]);
        if (n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING && *json_node_get_string(n))
            return json_node_get_string(n);
    }
    return NULL;
}

                                                      
void
handle_vm_owner_set(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = _target(params);
    if (!name) {
        _reply_error(rpc_id, server, connection, PURE_RPC_ERR_INVALID_PARAMS, "Missing required parameter: name");
        return;
    }
    JsonNode *on = params ? json_object_get_member(params, "owner") : NULL;
    if (!on) {
        _reply_error(rpc_id, server, connection, PURE_RPC_ERR_INVALID_PARAMS, "Missing required parameter: owner");
        return;
    }
    const gchar *owner = (JSON_NODE_HOLDS_VALUE(on) && json_node_get_value_type(on) == G_TYPE_STRING) ? json_node_get_string(on) : NULL;
    if (!owner || !*owner || pcv_principal_name_is_reserved(owner)) {
        _reply_error(rpc_id, server, connection, PURE_RPC_ERR_INVALID_PARAMS, OWNER_INVALID);
        return;
    }
    const PcvUserExistence exists = pcv_rbac_user_exists(owner);
    if (exists == PCV_USER_UNKNOWN) {
        _reply_error(rpc_id, server, connection, PURE_RPC_ERR_ZFS_OPERATION, "Owner check failed: user directory unavailable");
        return;
    }
    if (exists != PCV_USER_PRESENT) {
        _reply_error(rpc_id, server, connection, PURE_RPC_ERR_INVALID_PARAMS, OWNER_INVALID);
        return;
    }
    JsonNode *en = params ? json_object_get_member(params, "expected_owner") : NULL;
    const gboolean has_expected = en != NULL;
    if (en && !JSON_NODE_HOLDS_NULL(en) && !(JSON_NODE_HOLDS_VALUE(en) && json_node_get_value_type(en) == G_TYPE_STRING)) {
        _reply_error(rpc_id, server, connection, PURE_RPC_ERR_INVALID_PARAMS,
                     "Invalid params: expected_owner must be a string or null");
        return;
    }
    const gchar *expected = (en && !JSON_NODE_HOLDS_NULL(en)) ? json_node_get_string(en) : NULL;

    gchar *lock_err = NULL;
    PcvVmLock *lock = pcv_vm_lock_acquire_ex(name, VM_OP_TUNING, &lock_err, NULL);
    if (!lock) {
        _reply_error(rpc_id, server, connection, PURE_RPC_ERR_BUSY, lock_err ? lock_err : "VM is busy");
        g_free(lock_err);
        return;
    }
    virConnectPtr conn = virt_conn_pool_acquire();
    virDomainPtr dom = NULL;
    if (conn) {
        dom = virDomainLookupByName(conn, name);
        if (!dom) {
            virResetLastError();
            dom = virDomainLookupByUUIDString(conn, name);
        }
        if (!dom)
            virResetLastError();
    }
    if (!dom) {
        _reply_error(rpc_id, server, connection, conn ? PURE_RPC_ERR_VM_NOT_FOUND : PURE_RPC_ERR_ZFS_OPERATION,
                     conn ? "VM not found" : "libvirt connection unavailable");
        if (conn) virt_conn_pool_release(conn);
        pcv_vm_lock_release(lock);
        return;
    }
    char uuid[VIR_UUID_STRING_BUFLEN] = { 0 };
    (void)virDomainGetUUIDString(dom, uuid);
    const char *dom_name = virDomainGetName(dom);
    gchar *previous = pcv_vm_owner_metadata_read(dom, FALSE);
    gboolean live_updated = FALSE;
    GError *error = NULL;
    if (has_expected && g_strcmp0(previous, expected) != 0) {                                      
        _reply_error(rpc_id, server, connection, PURE_RPC_ERR_CONFLICT, "Owner changed: expected_owner does not match");
    } else if (!pcv_vm_owner_metadata_set(dom, owner, previous, &live_updated, &error)) {
        gchar *msg = g_strdup_printf("Owner metadata update failed (%s)", error ? error->message : "config");
        _reply_error(rpc_id, server, connection, PURE_RPC_ERR_ZFS_OPERATION, msg);
        g_free(msg);
    } else {
        const gchar *actor = pcv_disp_dispatcher_caller_subject(params, connection);
        PCV_LOG_INFO("vm.owner.set", "[vm.owner.set] vm=%s uuid=%s previous=%s new=%s live=%d actor=%s",
                     dom_name ? dom_name : name, uuid, previous ? previous : "-", owner, live_updated ? 1 : 0,
                     actor ? actor : "-");
        pcv_vm_diag_sampler_invalidate_owners();                                     
        pcv_ws_close_vm_streams(uuid);                                                      
        JsonObject *res = json_object_new();
        json_object_set_string_member(res, "name", dom_name ? dom_name : name);
        json_object_set_string_member(res, "uuid", uuid);
        json_object_set_string_member(res, "owner", owner);
        if (previous) json_object_set_string_member(res, "previous_owner", previous);
        else json_object_set_null_member(res, "previous_owner");
        json_object_set_boolean_member(res, "live_updated", live_updated);
        JsonNode *node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(node, res);
        gchar *resp = pure_rpc_build_success_response(rpc_id, node);
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
    }
    g_clear_error(&error);
    g_free(previous);
    virDomainFree(dom);
    virt_conn_pool_release(conn);
    pcv_vm_lock_release(lock);
}
