   
                               
                                                          
  
                           
                                                                                         
                                                                                            
                                                                                             
                                                                                           
                                                                               
  
                                                                                 
                                                         
   
#include "modules/dispatcher/handler_monitor_diag.h"

#include <string.h>

#include "modules/daemons/vm_diag.h"
#include "modules/daemons/vm_diag_view.h"
#include "modules/dispatcher/rpc_utils.h"
#include "modules/resource/resource_gate.h"
#include "modules/resource/vm_read_scope.h"

                                                                    
#define VM_DIAG_VM_PARAM_MAX 255

                                                                                         
                                                    
static gboolean
_vm_diag_read_visible(const PcvVmDiagConfig *cfg, gpointer ud)
{
    return cfg && pcv_vm_read_visible(ud, cfg->owner);
}

                
static void
_vm_diag_reply_invalid(const gchar *rpc_id, UdsServer *server, GSocketConnection *connection,
                       const gchar *msg)
{
    gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, msg);
    pure_uds_server_send_response(server, connection, err);
    g_free(err);
}

                                                      
void
handle_monitor_vm_diag(JsonObject *params, const gchar *rpc_id, UdsServer *server,
                       GSocketConnection *connection)
{
    const gchar *vm = NULL;
    if (params && json_object_has_member(params, "vm")) {
        JsonNode *n = json_object_get_member(params, "vm");
        if (!n || !JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_STRING) {
            _vm_diag_reply_invalid(rpc_id, server, connection, "vm must be a string");
            return;
        }
        vm = json_node_get_string(n);
        if (!vm || strlen(vm) > VM_DIAG_VM_PARAM_MAX) {
            _vm_diag_reply_invalid(rpc_id, server, connection, "vm is too long");
            return;
        }
    }

    PcvResCaller rc = { 0 };
    pcv_res_caller_init(&rc, params, connection);
    PcvVmReadScope *scope = pcv_vm_read_scope_new(&rc);                                    
    PcvVmDiagViewer viewer = {
        .all = pcv_vm_read_scope_all(scope),
        .visible = _vm_diag_read_visible,
        .ud = scope,
    };

    PcvVmDiagSnapshot *snap = pcv_vm_diag_store_copy();
    JsonNode *node = pcv_vm_diag_view_json(snap, &viewer, vm, g_get_monotonic_time());
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);                    
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);

    pcv_vm_diag_snapshot_free(snap);
    pcv_vm_read_scope_free(scope);
    pcv_res_caller_clear(&rc);
}
