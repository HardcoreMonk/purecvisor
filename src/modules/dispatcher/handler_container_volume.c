   
                                   
                                                                   
  
                           
                                                                                  
                                                                                                          
                                                                                            
                                                                                                 
                                                                                                        
                                                                                                                        
                                                                                                  
                                                                                   
                                                                                               
  
                                                                                   
                             
   

#include "handler_container.h"
#include "modules/lxc/lxc_driver.h"
#include "modules/lxc/lxc_volume_mount.h"                                                               
#include "modules/lxc/lxc_volume_detach.h"                                           
#include "rpc_utils.h"
#include "modules/dispatcher/handler_container_mode.h"
#include "purecvisor/pcv_validate.h"
#include "modules/core/vm_state.h"
#include "api/uds_server.h"

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <unistd.h>                              

                                                                             
                                                             
  
                                                      
                                                    
                                                               
  
                                                                                         
                        
  
                                                                                                   
                                                                               
                                                                             
                                                                 
                                                                                                                  
                                                                                                              
                                                                                                                                           
                                                                                        
                                                                                      
                                                                                                                
                                                                               

                                                                                                               
                                                                                                              
                                                                                                                
static void
_send_volume_mount_error(const gchar *rpc_id, UdsServer *server, GSocketConnection *conn, GError *err,
                         gint fallback_code, const gchar *fallback_msg)
{
    PcvLxcMountErrorClass cls = pcv_lxc_mount_error_classify(err);
    gboolean bad = cls == PCV_LXC_MOUNT_CLASS_BAD_REQUEST, old_kernel = cls == PCV_LXC_MOUNT_CLASS_NO_KERNEL_SUPPORT;
    gint code = bad ? PURE_RPC_ERR_INVALID_PARAMS : old_kernel ? PURE_RPC_ERR_CONFLICT : fallback_code;
    const gchar *msg = bad ? err->message : !old_kernel ? fallback_msg
                     : fallback_code == PURE_RPC_ERR_INVALID_PARAMS ? "host_path check needs openat2 (kernel 5.6+)"
                                                                     : "stop the container to attach a volume on this kernel";
    gchar *e = pure_rpc_build_error_response(rpc_id, code, msg);
    pure_uds_server_send_response(server, conn, e);
    g_free(e);
    g_clear_error(&err);
}

                                                                                                    
                                                                                            
                              
static PcvVmLock *
_volume_lock_or_reply(const gchar *name, const gchar *rpc_id, UdsServer *server, GSocketConnection *conn)
{
    gchar *lock_err = NULL;
    PcvVmLock *lock = pcv_vm_lock_acquire(name, VM_OP_TUNING, &lock_err);
    if (!lock) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY, lock_err ? lock_err : "Container is busy");
        pure_uds_server_send_response(server, conn, e);
        g_free(e);
    }
    g_free(lock_err);
    return lock;
}

void
handle_container_volume_attach(JsonObject *params, const gchar *rpc_id,
                                UdsServer *server, GSocketConnection *conn)
{
                                                                 
                                                                  
                       
    const gchar *name           = pcv_rpc_params_get_string(params, "name");
    const gchar *host_path      = pcv_rpc_params_get_string(params, "host_path");
    const gchar *container_path = pcv_rpc_params_get_string(params, "container_path");
    if (!name || !host_path || !container_path) {
        gchar *e = pure_rpc_build_error_response(
                       rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Missing or non-string required: name, host_path, container_path");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }
    gboolean readonly = TRUE;                                                     
    if (!pcv_rpc_params_get_bool(params, "readonly", TRUE, &readonly)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "readonly must be a boolean");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }

    if (!pcv_validate_vm_name(name)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Invalid container name");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }

                                                                                   
    if (container_path[0] != '/' || strstr(container_path, "..")) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "container_path must be absolute with no '..'");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }
                                                                                                                    
    if (!pcv_container_config_token_safe(container_path, FALSE) ||
        !pcv_container_config_token_safe(host_path, FALSE)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "host_path/container_path must not contain whitespace, control characters or backslashes");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }

                                                                                                         
    g_autoptr(PcvVmLock) op_lock = _volume_lock_or_reply(name, rpc_id, server, conn);
    if (!op_lock)
        return;

                                                                                                   
    g_autofree gchar *canon = NULL;
    GError *run_err = NULL;
    int src_fd = pcv_lxc_volume_source_open(host_path, &canon, &run_err);
    if (src_fd < 0) {
        _send_volume_mount_error(rpc_id, server, conn, run_err, PURE_RPC_ERR_INVALID_PARAMS,
                                 "host_path does not exist or is invalid");
        return;
    }
    if (!pcv_container_volume_hook_ready(rpc_id, server, conn)) { close(src_fd); return; }                                              

                                                                                           
                                                                                                      
                                              
    GError *cfg_err = NULL;
    if (!pcv_container_config_ready(name, NULL, &cfg_err)) {
        close(src_fd);
        _send_volume_mount_error(rpc_id, server, conn, cfg_err, PURE_RPC_ERR_INTERNAL_ERROR,
                                 cfg_err ? cfg_err->message : "container config unavailable");
        return;
    }
    gchar *state = pcv_lxc_get_state(name);
    gboolean running = state && g_strcmp0(state, "RUNNING") == 0;
    g_free(state);
                                                                                              
    gboolean ok = pcv_lxc_volume_attach(PCV_LXC_PATH, name, src_fd, canon, container_path, readonly, running, &run_err);
    close(src_fd);
    if (!ok) {                                                                                             
                                                             
        const gboolean dup = g_error_matches(run_err, G_IO_ERROR, G_IO_ERROR_EXISTS);
        const gboolean occupied = g_error_matches(run_err, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_CONFLICT);
        _send_volume_mount_error(rpc_id, server, conn, run_err,
                                 dup ? PURE_RPC_ERR_INVALID_PARAMS : occupied ? PURE_RPC_ERR_CONFLICT : PURE_RPC_ERR_INTERNAL_ERROR,
                                 run_err ? run_err->message : "container volume attach failed");
        return;
    }

    JsonObject *res = json_object_new();
    json_object_set_boolean_member(res, "success", TRUE);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, conn, resp);
    g_free(resp);
}

                                                                             
                                                                         
  
                                                                             
                                                                          
                                                         
  
                               
                        
                                                                                             
                                 
  
                                                                                                                
                                                                                               
                                                                               

                                                                                                                   
                                                                          
                                                                        
static void
_send_volume_detach_error(const gchar *rpc_id, UdsServer *server, GSocketConnection *conn, GError *err)
{
    gint code = g_error_matches(err, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_CONFLICT) ? PURE_RPC_ERR_CONFLICT
                                                                                          : PURE_RPC_ERR_INTERNAL_ERROR;
    gchar *e = pure_rpc_build_error_response(rpc_id, code, err ? err->message : "container volume detach failed");
    pure_uds_server_send_response(server, conn, e);
    g_free(e);
    g_clear_error(&err);
}

void
handle_container_volume_detach(JsonObject *params, const gchar *rpc_id,
                                UdsServer *server, GSocketConnection *conn)
{
                                                             
    const gchar *name           = pcv_rpc_params_get_string(params, "name");
    const gchar *container_path = pcv_rpc_params_get_string(params, "container_path");
    if (!name || !container_path) {
        gchar *e = pure_rpc_build_error_response(
                       rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Missing or non-string required: name, container_path");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }

    if (!pcv_validate_vm_name(name)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Invalid container name");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }
    if (container_path[0] != '/' || strstr(container_path, "..")) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "container_path must be absolute with no '..'");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }

                                                                                  
    g_autoptr(PcvVmLock) op_lock = _volume_lock_or_reply(name, rpc_id, server, conn);
    if (!op_lock)
        return;

                                                                                                     
                                                                                  
    GError *err = NULL;
    if (!pcv_container_config_ready(name, NULL, &err)) {
        _send_volume_detach_error(rpc_id, server, conn, err);
        return;
    }
    gboolean drop_line = FALSE, was_running = FALSE;
    if (!pcv_lxc_volume_detach_run(PCV_LXC_PATH, name, container_path, &drop_line, &was_running, &err) ||
        (drop_line && !pcv_lxc_volume_config_drop(PCV_LXC_PATH, name, container_path, was_running, &err))) {
        _send_volume_detach_error(rpc_id, server, conn, err);
        return;
    }

    JsonObject *res = json_object_new();
    json_object_set_boolean_member(res, "success", TRUE);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, conn, resp);
    g_free(resp);
}

                                                                             
                                              
  
                                                   
                                                                
  
              
                                                                                               
                                                                               

void
handle_container_volume_list(JsonObject *params, const gchar *rpc_id,
                              UdsServer *server, GSocketConnection *conn)
{
    if (!params || !json_object_has_member(params, "name")) {
        gchar *e = pure_rpc_build_error_response(
                       rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Missing parameter: 'name'");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }
    const gchar *name = json_object_get_string_member(params, "name");

    if (!pcv_validate_vm_name(name)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Invalid container name");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }

    gchar *config_path = NULL;
    GError *cfg_err = NULL;
    if (!pcv_container_config_ready(name, &config_path, &cfg_err)) {
        gchar *e = pure_rpc_build_error_response(
                       rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
                       cfg_err ? cfg_err->message : "container config unavailable");
        pure_uds_server_send_response(server, conn, e);
        g_free(e);
        if (cfg_err) g_error_free(cfg_err);
        return;
    }
    gchar *contents = NULL;
    JsonArray *arr = json_array_new();

    if (g_file_get_contents(config_path, &contents, NULL, NULL)) {
        gchar **lines = g_strsplit(contents, "\n", -1);
        for (gint i = 0; lines[i]; i++) {
                                                                           
            if (!g_str_has_prefix(lines[i], "lxc.mount.entry"))
                continue;
            gchar *eq = strchr(lines[i], '=');                           
            if (!eq) continue;
            gchar *val = g_strstrip(g_strdup(eq + 1));
                                                                           
            gchar **parts = g_strsplit(val, " ", 6);
            g_free(val);
            if (parts[0] && parts[1] && parts[2] && parts[3]) {                         
                                                   
                gboolean ro = strstr(parts[3], "ro") != NULL;                                 
                gchar *cpath = g_strdup_printf("/%s", parts[1]);                           
                JsonObject *obj = json_object_new();
                json_object_set_string_member(obj, "host_path", parts[0]);
                json_object_set_string_member(obj, "container_path", cpath);
                json_object_set_boolean_member(obj, "readonly", ro);
                json_array_add_object_element(arr, obj);
                g_free(cpath);
            }
            g_strfreev(parts);
        }
        g_strfreev(lines);
        g_free(contents);
    }
    g_free(config_path);

    JsonNode *node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(node, arr);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, conn, resp);
    g_free(resp);
}
