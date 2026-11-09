   
                            
                                                                                            
  
                           
                                                   
                                                    
                                        
  
          
                                                      
                                             
                                                     
                                                         
                                                       
            
  
         
                                                                    
                                                     
                                                             
                                                          
                                                                    
        
                                                                        
                                                     
                                                              
                                                        
                                                                                 
         
                                            
                                                              
                          
            
                                                                      
                                                                           
                                                              
                                                                      
                                          
                                                   
  
            
                                                                              
                                                                                             
                                                                                                
  
                                                                   
                                                                                    
                                                                                
                                                                                
                                                                                
                                                                                   
                                                                                         
                                                                                      
                                                                                   
                                                                                   
                                                                                   
                                                                                  
  
              
                                                   
                                                                          
                                                   
                                                             
                                           
  
                       
                                                                
                                                            
                     
  
         
                                                                    
                               
                                                                                          
                                                                         
                                              
  
          
                                                     
                           
   

#include "handler_container.h"
#include "modules/lxc/lxc_driver.h"
#include "modules/lxc/lxc_unit.h"                                                         
#include "rpc_utils.h"
#include "poll_async.h"                                               
#include "modules/dispatcher/handler_container_mode.h"
#include "modules/dispatcher/rpc_completion.h"                                                  
#include "purecvisor/pcv_validate.h"
#include "modules/core/vm_state.h"
#include "modules/audit/pcv_audit.h"
#include "api/drain.h"                                                       
#include "api/uds_server.h"
#include "api/ws_server.h"

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>                                    
#include "utils/pcv_spawn.h"

                                                                             
             
  
                                                       
                                                  
                                                 
  
                                        
                                                
  
                                                     
                                                           
                                                      
                                                                          
                                                         
                                                                  
                                                       
                                                                  
                                         
  
                      
                                 
                                        
                                                
  
                                                         
                                                                               

typedef struct {
    gchar            *name;                                 
    gchar            *rpc_id;                                    
    UdsServer        *server;                                      
    GSocketConnection *conn;                                       
                              
    gchar            *str_param;                                                    
    gboolean          bool_param;                                  
    PcvVmLock        *lock;                                                       
} ContainerCtx;

   
            
                                    
                                               
                                              
  
                                                      
                                                        
                                                 
                                                                               
                                                                                          
                                                         
   
static ContainerCtx *
_ctx_new(const gchar *name, const gchar *rpc_id,
         UdsServer *server, GSocketConnection *conn)
{
    ContainerCtx *ctx = g_new0(ContainerCtx, 1);                                                            
    ctx->name   = g_strdup(name);                                                
    ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = server ? g_object_ref(server) : NULL;                                               
    ctx->conn   = conn ? g_object_ref(conn) : NULL;
    return ctx;
}

   
             
                                   
                                         
                                          
  
                                                 
                                            
                                           
   
static void
_ctx_free(ContainerCtx *ctx)
{
    if (!ctx) return;                                                    
    g_free(ctx->name);
    g_free(ctx->rpc_id);
    g_free(ctx->str_param);                                                   
    g_clear_object(&ctx->server);                                                               
    g_clear_object(&ctx->conn);
    g_free(ctx);
}

   
                                  
                                                    
  
                                                    
                                                         
                                                  
                                                   
                                             
  
       
                                                                    
                                                             
                                                              
                                                                  
  
                                         
                                                                         
                                                  
                                                                           
                                                                   
                                                             
   
static gboolean
_ensure_container_config_ready(const gchar *name,
                               gchar      **out_config_path,
                               GError     **error)
{
    gchar *config_path = g_strdup_printf("%s/%s/config", PCV_LXC_PATH, name);
                                                 
    if (g_file_test(config_path, G_FILE_TEST_EXISTS)) {
        if (out_config_path) *out_config_path = config_path;              
        else g_free(config_path);                                                 
        return TRUE;
    }

                                                                   
    gchar *dataset = g_strdup_printf("%s/%s", PCV_LXC_ZFS_BASE, name);
    const gchar *mount_argv[] = { "zfs", "mount", dataset, NULL };
    GError *mount_err = NULL;

    if (!pcv_spawn_sync(mount_argv, NULL, NULL, &mount_err) && mount_err) {
                                                                               
                                                         
                                                             
        if (!g_strrstr(mount_err->message, "already mounted") &&
            !g_file_test(config_path, G_FILE_TEST_EXISTS)) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Failed to mount container dataset for '%s': %s",
                        name, mount_err->message);
            g_error_free(mount_err);
            g_free(dataset);
            g_free(config_path);
            return FALSE;
        }
        g_error_free(mount_err);
    }
    g_free(dataset);

                                                                
    if (!g_file_test(config_path, G_FILE_TEST_EXISTS)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "Container '%s' config is not visible under %s",
                    name, config_path);
        g_free(config_path);
        return FALSE;
    }

    if (out_config_path) *out_config_path = config_path;
    else g_free(config_path);
    return TRUE;
}

                                                                                                         
                                                                           
gboolean
pcv_container_config_ready(const gchar *name, gchar **out_config_path, GError **error)
{
    return _ensure_container_config_ready(name, out_config_path, error);
}

                                                                         

   
            
                                         
                                                                    
  
                                      
                                                                
                                                                   
   
static void
_send_ok(ContainerCtx *ctx)
{
    JsonObject *res = json_object_new();
    json_object_set_boolean_member(res, "success", TRUE);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(ctx->rpc_id, node);
    pure_uds_server_send_response(ctx->server, ctx->conn, resp);
    g_free(resp);
}

   
               
                       
                                                         
                                      
  
                                              
                                                   
   
static void
_send_error(ContainerCtx *ctx, PureRpcErrorCode code, const gchar *msg)
{
    gchar *resp = pure_rpc_build_error_response(ctx->rpc_id, code, msg);
    pure_uds_server_send_response(ctx->server, ctx->conn, resp);
    g_free(resp);
}

                                                                             
                   
  
                                                    
                                      
  
        
                            
                                         
                                    
                                  
                                       
                                                                               

                                                              
                                           
typedef struct {
    ContainerCtx *base;                                                              
    gchar        *image;
    guint         memory_mb;
    guint         vcpu_count;
    gchar        *bridge;
    gint          rootless;                                                          
    gchar        *owner_sub;                                                                 
} CreateCtx;

                                                     
                                    
static void
_create_ctx_free(CreateCtx *c)
{
    _ctx_free(c->base);                                                    
    g_free(c->image);
    g_free(c->bridge);
    g_free(c->owner_sub);
    g_free(c);
}

   
                   
                                          
  
                                                 
                                                  
                                                
  
             
                                                        
                                                
                                           
  
                                                                      
                                                    
   
static void
_on_create_done(GObject *src __attribute__((unused)), GAsyncResult *res, gpointer user_data)
{
    CreateCtx *ctx = (CreateCtx *)user_data;
    GError    *error = NULL;

                              
                                                            
                                                             
                            
    g_clear_pointer(&ctx->base->lock, pcv_vm_lock_release);                              
    gchar *job_id = g_strdup_printf("container.create:%s", ctx->base->name);
    if (!pcv_lxc_create_finish(res, &error)) {
        const gchar *err_msg = error ? error->message : "unknown error";
                                                  
        g_warning("container.create failed for '%s': %s",
                  ctx->base->name, err_msg);
        pcv_audit_log(pcv_drain_task_actor(res), "container.create", ctx->base->name, "fail",
                      PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
        pcv_ws_broadcast_job_complete(job_id, "container.create",
                                      "failed", err_msg);
        if (error) g_error_free(error);
    } else {
        g_info("container.create succeeded for '%s'", ctx->base->name);
                                                                     
                                                                       
                                                            
        pcv_lxc_stamp_owner(ctx->base->name, ctx->owner_sub);
        pcv_audit_log(pcv_drain_task_actor(res), "container.create", ctx->base->name, "ok",
                      0, 0, "local");
        pcv_ws_broadcast_job_complete(job_id, "container.create",
                                      "completed", NULL);
    }
    g_free(job_id);
    _create_ctx_free(ctx);
}

   
                                                                    
  
                                                             
                                                          
                                                             
                                                 
                                                                      
  
                                                    
                                                    
                                                   
                                                 
                    
  
                                                          
                                                                 
                                 
                                                                                                   
                                                                       
                                                            
                                                                           
                                                                  
                                                             
                                                       
                                                      
                                                            
                                   
                                                                                                                                                  
                                                                                                                 
                                                    
                                        
                                                   
                                                        
                                                      
   
void
handle_container_create(JsonObject *params, const gchar *rpc_id,
                         UdsServer *server, GSocketConnection *conn)
{
                                                                                       
    g_autofree gchar *alias_msg = NULL;
    if (!pcv_container_create_bridge_alias(params, &alias_msg)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, alias_msg);
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }
    if (!params || !json_object_has_member(params, "name")) {
        gchar *e = pure_rpc_build_error_response(
                       rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Missing parameter: 'name'");
        pure_uds_server_send_response(server, conn, e);
        g_free(e); return;
    }

                                                                
    const gchar *name   = json_object_get_string_member(params, "name");
    const gchar *image  = json_object_has_member(params, "image")
                          ? json_object_get_string_member(params, "image")
                          : "ubuntu:22.04";
    guint memory_mb   = json_object_has_member(params, "memory_mb")
                        ? (guint)json_object_get_int_member(params, "memory_mb") : 512;
    guint vcpu_count  = json_object_has_member(params, "vcpu_count")
                        ? (guint)json_object_get_int_member(params, "vcpu_count") : 1;
    const gchar *bridge = json_object_has_member(params, "network_bridge")
                          ? json_object_get_string_member(params, "network_bridge")
                          : NULL;

      
                          
      
                                           
                                                       
      
                                                                             
                                                            
                                                          
      
                                            
       
    if (!pcv_validate_new_container_name(name)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Invalid container name: 1-64 chars [a-zA-Z0-9_-], must not start with '-'");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }
    if (!pcv_validate_container_image(image)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Invalid image: use 'distro:release' format (e.g. ubuntu:22.04)");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }
    if (bridge && !pcv_validate_bridge_name(bridge)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Invalid network_bridge: 1-16 chars [a-zA-Z0-9_-]");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }

                                                                                  
                                                                                                       
    PcvContainerCreateMode mode = {0};
    if (!pcv_container_create_mode_check(params, conn, NULL, PCV_LXC_SUBUID_PATH, PCV_LXC_SUBGID_PATH, &mode)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, mode.code, mode.message);
        pure_uds_server_send_response(server, conn, e); g_free(e); g_free(mode.message); return;
    }

                                                                   
                                                                            
                                                      
    gchar *lock_err = NULL;
    PcvVmLock *op_lock = pcv_vm_lock_acquire(name, VM_OP_CREATING, &lock_err);
    if (!op_lock) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
                       lock_err ? lock_err : "Container is busy");
        pure_uds_server_send_response(server, conn, e);
        g_free(e); g_free(lock_err); return;
    }

                                                       
                                              
    {
        JsonObject *accepted = json_object_new();
        json_object_set_string_member(accepted, "status", "accepted");
        json_object_set_string_member(accepted, "name", name);
        json_object_set_string_member(accepted, "message", "Container creation started");
        JsonNode *node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(node, accepted);
        gchar *resp = pure_rpc_build_success_response(rpc_id, node);
        pure_uds_server_send_response(server, conn, resp);
        g_free(resp);
    }

                                                                              
                                                                        
                                                        
    const gchar *owner_sub =
        json_object_get_string_member_with_default(params, "_pcv_caller_sub", NULL);

                                           
    CreateCtx *ctx  = g_new0(CreateCtx, 1);
    ctx->base       = _ctx_new(name, rpc_id, server, conn);
    ctx->base->lock = op_lock;                                         
    ctx->image      = g_strdup(image);
    ctx->memory_mb  = memory_mb;
    ctx->vcpu_count = vcpu_count;
    ctx->bridge     = g_strdup(bridge ? bridge : PCV_LXC_DEFAULT_BRIDGE);
    ctx->rootless   = mode.rootless;
    ctx->owner_sub  = g_strdup(owner_sub);

                                              
    pcv_lxc_create_async_full(name, ctx->image, memory_mb, vcpu_count,
                               ctx->bridge, mode.rootless,
                               NULL, _on_create_done, ctx);
}

                                                                             
                    
  
                                                    
  
                                        
                             
  
                        
                        
                                                                               

   
                    
                                 
                                                
  
                                                   
  
                                                                           
                                                       
   
static void
_on_destroy_done(GObject *src __attribute__((unused)), GAsyncResult *res, gpointer user_data)
{
    ContainerCtx *ctx = (ContainerCtx *)user_data;
    GError       *error = NULL;

    g_clear_pointer(&ctx->lock, pcv_vm_lock_release);                                       
    gchar *job_id = g_strdup_printf("container.destroy:%s", ctx->name);
    if (!pcv_lxc_destroy_finish(res, &error)) {
        const gchar *err_msg = error ? error->message : "unknown";
        g_warning("container.destroy failed for '%s': %s",
                  ctx->name, err_msg);
        pcv_audit_log(pcv_drain_task_actor(res), "container.destroy", ctx->name, "fail",
                      PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
        pcv_ws_broadcast_job_complete(job_id, "container.destroy",
                                      "failed", err_msg);
        if (error) g_error_free(error);
    } else {
        g_info("container.destroy succeeded for '%s'", ctx->name);
        pcv_audit_log(pcv_drain_task_actor(res), "container.destroy", ctx->name, "ok",
                      0, 0, "local");
        pcv_ws_broadcast_job_complete(job_id, "container.destroy",
                                      "completed", NULL);
    }
    g_free(job_id);
    _ctx_free(ctx);
}

   
                                                                    
  
                                                            
                                                                                   
                                                     
                                                
                                                                 
                                                      
  
                                                    
                                                 
                                                   
                             
  
                                                                     
                                                              
                                                                     
                                       
                                                        
                                                          
                                                                   
                                                                    
                                                
                                        
                                            
                                                        
                                                
   
void
handle_container_destroy(JsonObject *params, const gchar *rpc_id,
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
                       "Invalid container name: 1-64 chars [a-zA-Z0-9_-]");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }

                      
    gchar *lock_err = NULL;
    PcvVmLock *op_lock = pcv_vm_lock_acquire(name, VM_OP_DELETING, &lock_err);
    if (!op_lock) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
                       lock_err ? lock_err : "Container is busy");
        pure_uds_server_send_response(server, conn, e);
        g_free(e); g_free(lock_err); return;
    }

                                         
    {
        JsonObject *accepted = json_object_new();
        json_object_set_string_member(accepted, "status", "accepted");
        json_object_set_string_member(accepted, "name", name);
        json_object_set_string_member(accepted, "message", "Container deletion started");
        JsonNode *node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(node, accepted);
        gchar *resp = pure_rpc_build_success_response(rpc_id, node);
        pure_uds_server_send_response(server, conn, resp);
        g_free(resp);
    }

    ContainerCtx *ctx = _ctx_new(name, rpc_id, server, conn);
    ctx->lock = op_lock;
    pcv_lxc_destroy_async(name, NULL, _on_destroy_done, ctx);
}

                                                                             
                  
  
                             
  
                                                     
  
                        
                        
                                                                               

   
                  
                               
                                   
  
                                                                               
                                                       
   
static void
_on_start_done(GObject *src __attribute__((unused)), GAsyncResult *res, gpointer user_data)
{
    ContainerCtx *ctx = (ContainerCtx *)user_data;
    GError       *error = NULL;

    g_clear_pointer(&ctx->lock, pcv_vm_lock_release);
    gchar *job_id = g_strdup_printf("container.start:%s", ctx->name);
    if (!pcv_lxc_start_finish(res, &error)) {
        const gchar *err_msg = error ? error->message : "unknown error";
        g_warning("container.start failed for '%s': %s",
                  ctx->name, err_msg);
        pcv_audit_log(pcv_drain_task_actor(res), "container.start", ctx->name, "fail", g_error_matches(error, G_IO_ERROR,
                      G_IO_ERROR_NOT_SUPPORTED) ? PURE_RPC_ERR_INTERNAL_ERROR : PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
        pcv_ws_broadcast_job_complete(job_id, "container.start",
                                      "failed", err_msg);
        if (error) g_error_free(error);
    } else {
        g_info("container.start succeeded for '%s'", ctx->name);
        pcv_audit_log(pcv_drain_task_actor(res), "container.start", ctx->name, "ok",
                      0, 0, "local");
        pcv_ws_broadcast_job_complete(job_id, "container.start",
                                      "completed", NULL);
    }
    g_free(job_id);
    _ctx_free(ctx);
}

                                                                                             
                                                                        
PcvCtrStartSubmit
pcv_container_start_submit(const gchar *name, const gchar *actor, gchar **msg_out)
{
    g_autofree gchar *why = NULL;
    if (!pcv_container_volume_start_check(name, &why)) {                                                      
        if (msg_out) *msg_out = g_steal_pointer(&why);
        return PCV_CTR_START_DENIED;
    }
    gchar *lock_err = NULL;
    PcvVmLock *op_lock = pcv_vm_lock_acquire(name, VM_OP_STARTING, &lock_err);
    if (!op_lock) {
        if (msg_out) *msg_out = lock_err ? lock_err : g_strdup("Container is busy");
        else g_free(lock_err);
        return PCV_CTR_START_BUSY;
    }
    ContainerCtx *ctx = _ctx_new(name, NULL, NULL, NULL);                                                     
    ctx->lock = op_lock;                                        
    pcv_lxc_start_async(name, actor, NULL, _on_start_done, ctx);
    return PCV_CTR_START_SUBMITTED;
}

                                                                                                              
                                                                         
void
pcv_container_start_report_rejected(const gchar *name, PcvCtrStartSubmit why, const gchar *msg, const gchar *actor)
{
    const gboolean denied = why == PCV_CTR_START_DENIED;                                                                                      
    const gchar *reason = (msg && *msg) ? msg : "start rejected";
    pcv_audit_log(actor, "container.start", name, denied ? "denied" : "fail",
                  denied ? PURE_RPC_ERR_CONFLICT : PURE_RPC_ERR_BUSY, 0, "local");
    g_autofree gchar *job_id = g_strdup_printf("container.start:%s", name);
    pcv_ws_broadcast_job_complete(job_id, "container.start", "failed", reason);
    g_warning("container.start submit rejected for '%s': %s", name, reason);
}

   
                                                                
  
                                                                 
                                                                                
                                                                 
                                                               
                                                       
                                                                                 
                                                                                 
  
                                                  
                                                
                                                 
                                     
  
                                                                 
                                     
                                                                    
                                           
                                             
                                                                     
                           
                                        
                                 
                                                  
                                                         
   
void
handle_container_start(JsonObject *params, const gchar *rpc_id,
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
                       "Invalid container name: 1-64 chars [a-zA-Z0-9_-]");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }
                                                                                               
    g_autofree gchar *why = NULL;
    const PcvCtrStartSubmit r = pcv_container_start_submit(name, NULL, &why);
    if (r == PCV_CTR_START_DENIED) {
                                                                                              
        g_autofree gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_CONFLICT, why);
        pure_uds_server_send_response(server, conn, e);
        pcv_audit_log(pcv_rpc_completion_actor(), "container.start", name, "denied", PURE_RPC_ERR_CONFLICT, 0, "local");
        return;
    }
    if (r == PCV_CTR_START_BUSY) {                                                                   
        g_autofree gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY, why);
        pure_uds_server_send_response(server, conn, e);
        return;
    }

                                                                                       
                                                                             
    JsonObject *ok = json_object_new();
    json_object_set_boolean_member(ok, "success", TRUE);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, ok);
    g_autofree gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, conn, resp);
}

                                                                             
                 
  
                                                            
                              
  
                     
                                                                 
  
                                                
                        
                                                                               

   
                 
                              
                                   
  
                                                
                                                       
   
static void
_on_stop_done(GObject *src __attribute__((unused)), GAsyncResult *res, gpointer user_data)
{
    ContainerCtx *ctx = (ContainerCtx *)user_data;
    GError       *error = NULL;

    g_clear_pointer(&ctx->lock, pcv_vm_lock_release);
    gchar *job_id = g_strdup_printf("container.stop:%s", ctx->name);
    if (!pcv_lxc_stop_finish(res, &error)) {
        const gchar *err_msg = error ? error->message : "unknown error";
        g_warning("container.stop failed for '%s': %s",
                  ctx->name, err_msg);
        pcv_audit_log(pcv_drain_task_actor(res), "container.stop", ctx->name, "fail",
                      PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
        pcv_ws_broadcast_job_complete(job_id, "container.stop",
                                      "failed", err_msg);
        if (error) g_error_free(error);
    } else {
        g_info("container.stop succeeded for '%s'", ctx->name);
        pcv_audit_log(pcv_drain_task_actor(res), "container.stop", ctx->name, "ok",
                      0, 0, "local");
        pcv_ws_broadcast_job_complete(job_id, "container.stop",
                                      "completed", NULL);
    }
    g_free(job_id);
    _ctx_free(ctx);
}

   
                                                                 
  
                                                            
                                                                 
                                                            
                                                     
                         
  
                                                  
                                                 
                                                  
                                                
  
                                                                
                                     
                                                            
                                        
                                          
                                       
                                                          
                                        
                                            
                                                  
                                                         
   
void
handle_container_stop(JsonObject *params, const gchar *rpc_id,
                       UdsServer *server, GSocketConnection *conn)
{
    if (!params || !json_object_has_member(params, "name")) {
        gchar *e = pure_rpc_build_error_response(
                       rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Missing parameter: 'name'");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }
    const gchar *name  = json_object_get_string_member(params, "name");
    gboolean     force = json_object_has_member(params, "force")
                         ? json_object_get_boolean_member(params, "force")
                         : FALSE;

    if (!pcv_validate_vm_name(name)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Invalid container name: 1-64 chars [a-zA-Z0-9_-]");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }

    gchar *lock_err = NULL;
    PcvVmLock *op_lock = pcv_vm_lock_acquire(name, VM_OP_STOPPING, &lock_err);
    if (!op_lock) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
                       lock_err ? lock_err : "Container is busy");
        pure_uds_server_send_response(server, conn, e);
        g_free(e); g_free(lock_err); return;
    }

                                          
                                                           
    {
        JsonObject *ok = json_object_new();
        json_object_set_boolean_member(ok, "success", TRUE);
        JsonNode *node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(node, ok);
        gchar *resp = pure_rpc_build_success_response(rpc_id, node);
        pure_uds_server_send_response(server, conn, resp);
        g_free(resp);
    }

    ContainerCtx *ctx  = _ctx_new(name, rpc_id, server, conn);
    ctx->bool_param    = force;
    ctx->lock          = op_lock;
    pcv_lxc_stop_async(name, force, NULL, _on_stop_done, ctx);
}

                                                                             
                                                       
  
                                                               
                                                                                     
  
                          
                                                                                 
                                                                         
  
                         
                                                                                                                                   
  
                                                                                            
                                                                               

                                                                                                         
                                                                                  
                                                                 
static JsonNode *
pcv_container_list_compute(JsonObject *params, PcvPollGuard *guard, GError **error)
{
    GError    *lerr = NULL;
    GPtrArray *list = pcv_lxc_list_guarded(guard, &lerr);
    if (!list) {
        g_set_error_literal(error, g_quark_from_static_string("pcv-container-list"), PURE_RPC_ERR_INTERNAL_ERROR,
                            lerr ? lerr->message : "container.list failed");
        g_clear_error(&lerr);
        return NULL;
    }
    g_clear_error(&lerr);
                                            
    JsonArray *arr = json_array_new();
    for (guint i = 0; i < list->len; i++)
        json_array_add_object_element(arr, pcv_container_info_json(g_ptr_array_index(list, i)));
    g_ptr_array_unref(list);                                   

                                                     
                                                                 
    gint pg_offset = (params && json_object_has_member(params, "offset"))
        ? (gint)json_object_get_int_member(params, "offset") : 0;
    gint pg_limit = (params && json_object_has_member(params, "limit"))
        ? (gint)json_object_get_int_member(params, "limit") : 0;

    if (pg_limit > 0) {                                                 
                                                                        
                                                          
        if (pg_limit > 10000) pg_limit = 10000;
        gint total = (gint)json_array_get_length(arr);
        if (pg_offset < 0) pg_offset = 0;                                        
        if (pg_offset > total) pg_offset = total;                                              
        JsonArray *paged = json_array_new();
                                                                       
                                                      
        for (gint i = pg_offset; i < total && i < pg_offset + pg_limit; i++)
            json_array_add_element(paged, json_array_dup_element(arr, (guint)i));
        JsonObject *pg = json_object_new();
        json_object_set_array_member(pg, "items", paged);
        json_object_set_int_member(pg, "total", total);
        json_object_set_int_member(pg, "offset", pg_offset);
        json_object_set_int_member(pg, "limit", pg_limit);
        json_object_set_boolean_member(pg, "has_more", pg_offset + pg_limit < total);
        json_array_unref(arr);
        JsonNode *result_node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(result_node, pg);
        return result_node;
    }
    JsonNode *result_node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(result_node, arr);
    return result_node;
}

                                                                                    
void
handle_container_list(JsonObject *params, const gchar *rpc_id,
                       UdsServer *server, GSocketConnection *conn)
{
    pcv_poll_async_submit("container.list", params, rpc_id, server, conn, pcv_container_list_compute);
}

                                                                             
                             
  
                                                         
                                                 
  
                          
                                                                                                                     
  
                        
                                                     
                                                                 
                                                                    
                                                        
                                                                 
  
                                                       
                                                                               

void
handle_container_metrics(JsonObject *params, const gchar *rpc_id,
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
                       "Invalid container name: 1-64 chars [a-zA-Z0-9_-]");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }

    GError *error = NULL;
    PcvLxcMetrics *m = pcv_lxc_get_metrics(name, &error);
    if (!m) {
        gchar *e = pure_rpc_build_error_response(
                       rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
                       error ? error->message : "metrics unavailable");
        pure_uds_server_send_response(server, conn, e); g_free(e);
        if (error) g_error_free(error);
        return;
    }

                                                         
    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "name",         m->name);
    json_object_set_string_member(obj, "state",        m->state_str);
    json_object_set_double_member(obj, "mem_used_mb",  (gdouble)m->mem_used_bytes  / (1024.0 * 1024.0));
    json_object_set_double_member(obj, "mem_limit_mb", (gdouble)m->mem_limit_bytes / (1024.0 * 1024.0));
    json_object_set_double_member(obj, "cpu_percent",  m->cpu_percent);
    json_object_set_int_member   (obj, "cpu_time_ns",  (gint64)m->cpu_time_ns);                                       
    json_object_set_double_member(obj, "net_rx_mb",    (gdouble)m->net_rx_bytes / (1024.0 * 1024.0));
    json_object_set_double_member(obj, "net_tx_mb",    (gdouble)m->net_tx_bytes / (1024.0 * 1024.0));
    json_object_set_string_member(obj, "ip_addr",      m->ip_addr);
    json_object_set_int_member   (obj, "init_pid",     (gint64)m->init_pid);
    pcv_lxc_metrics_free(m);

    JsonNode *result_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(result_node, obj);
    gchar *resp = pure_rpc_build_success_response(rpc_id, result_node);
    pure_uds_server_send_response(server, conn, resp);
    g_free(resp);
}

                                                                             
                 
  
                                                   
                                                            
  
                                                      
                                        
                                                                               

                                                                                                            
                                                                          
static void
_on_exec_done(GObject *src __attribute__((unused)), GAsyncResult *res, gpointer user_data)
{
    ContainerCtx *ctx = (ContainerCtx *)user_data;
    GError       *error = NULL;

                                   
    gchar *output = pcv_lxc_exec_finish(res, &error);
    pcv_audit_log(pcv_drain_task_actor(res), "container.exec", ctx->name, output ? "ok" : "fail", output ? 0 : PURE_RPC_ERR_INTERNAL_ERROR, 0, "local");
    if (!output) {
        _send_error(ctx, PURE_RPC_ERR_INTERNAL_ERROR,
                    error ? error->message : "container.exec failed");
        if (error) g_error_free(error);
        _ctx_free(ctx); return;
    }

    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "output", output);
    g_free(output);

    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, obj);
    gchar *resp = pure_rpc_build_success_response(ctx->rpc_id, node);
    pure_uds_server_send_response(ctx->server, ctx->conn, resp);
    g_free(resp);
    _ctx_free(ctx);
}

   
                         
                                                
  
                                              
  
            
                                                                     
                                          
                                                                                   
                                                          
  
              
                                                      
                                          
                                   
   
void
handle_container_exec(JsonObject *params, const gchar *rpc_id,
                       UdsServer *server, GSocketConnection *conn)
{
    if (!params
        || !json_object_has_member(params, "name")
        || !json_object_has_member(params, "cmd")) {
        gchar *e = pure_rpc_build_error_response(
                       rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Missing parameter: 'name' and/or 'cmd'");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }
    const gchar *name = json_object_get_string_member(params, "name");
    const gchar *cmd  = json_object_get_string_member(params, "cmd");

                                    
    if (!pcv_validate_vm_name(name)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Invalid container name: 1-64 chars [a-zA-Z0-9_-]");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }
    if (!pcv_validate_exec_cmd(cmd)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Invalid cmd: must be 1-1024 chars with no null bytes");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }

                                                            
    const gchar *argv[] = { "/bin/sh", "-c", cmd, NULL };

    ContainerCtx *ctx = _ctx_new(name, rpc_id, server, conn);
    pcv_lxc_exec_async(name, argv, NULL, _on_exec_done, ctx);
}

                                                                             
                            
  
                                                       
                                      
  
                        
                                                 
  
                                             
                        
  
                                                         
                                                         
                                                                               

                                                                                                               
static void
_on_snap_create_done(GObject *src __attribute__((unused)), GAsyncResult *res,
                     gpointer user_data)
{
    ContainerCtx *ctx = (ContainerCtx *)user_data;
    GError       *error = NULL;
    gboolean ok = pcv_lxc_snapshot_create_finish(res, &error);
    g_autofree gchar *target = g_strdup_printf("%s:%s", ctx->name, ctx->str_param ? ctx->str_param : "");
    pcv_audit_log(pcv_drain_task_actor(res), "container.snapshot.create", target, ok ? "ok" : "fail", ok ? 0 : PURE_RPC_ERR_INTERNAL_ERROR, 0, "local");
    if (!ok)
        _send_error(ctx, PURE_RPC_ERR_INTERNAL_ERROR,
                    error ? error->message : "snapshot.create failed");
    else
        _send_ok(ctx);
    if (error) g_error_free(error);
    _ctx_free(ctx);
}

   
                                                                   
  
                                                  
                                                           
                                                                   
                                                                    
                                          
  
                                                   
                                                    
                                              
  
                                                               
                                                                 
                                                           
                                                       
                                                  
                                       
                                           
                                        
                                                              
                                               
                                                    
   
void
handle_container_snapshot_create(JsonObject *params, const gchar *rpc_id,
                                   UdsServer *server, GSocketConnection *conn)
{
    if (!params
        || !json_object_has_member(params, "name")
        || !json_object_has_member(params, "snap_name")) {
        gchar *e = pure_rpc_build_error_response(
                       rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Missing parameter: 'name' and/or 'snap_name'");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }
    const gchar  *name      = json_object_get_string_member(params, "name");
    const gchar  *snap_name = json_object_get_string_member(params, "snap_name");

                    
    if (!pcv_validate_vm_name(name)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Invalid container name: 1-64 chars [a-zA-Z0-9_-]");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }
    if (!pcv_validate_snap_name(snap_name)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Invalid snap_name: 1-128 chars [a-zA-Z0-9_-]");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }

    ContainerCtx *ctx  = _ctx_new(name, rpc_id, server, conn);
    ctx->str_param     = g_strdup(snap_name);
    pcv_lxc_snapshot_create_async(name, snap_name, NULL, _on_snap_create_done, ctx);
}

                                                                             
                              
  
                                                    
                         
  
                                    
                                                                          
                                                            
                                                
  
                                                 
                        
  
                                       
                                                                               

                                                                                                  
                                                                                                          
                                                                                                                 
                                                              
static void
_on_snap_rollback_done(GObject *src __attribute__((unused)), GAsyncResult *res,
                       gpointer user_data)
{
    ContainerCtx *ctx = (ContainerCtx *)user_data;
    GError       *error = NULL;
    gboolean      restart = FALSE;                                                               
    gboolean ok = pcv_lxc_snapshot_rollback_finish(res, &restart, &error);
    g_autofree gchar *target = g_strdup_printf("%s:%s", ctx->name, ctx->str_param ? ctx->str_param : "");
    pcv_audit_log(pcv_drain_task_actor(res), "container.snapshot.rollback", target, ok ? "ok" : "fail", ok ? 0 : PURE_RPC_ERR_INTERNAL_ERROR, 0, "local");
    if (!ok) {
        g_autofree gchar *msg = g_strconcat(error ? error->message : "snapshot.rollback failed",
                                            restart ? " (container restart requested; see container.start result)" : "", NULL);
        _send_error(ctx, PURE_RPC_ERR_INTERNAL_ERROR, msg);
    } else {
        _send_ok(ctx);
    }
    if (error) g_error_free(error);
    g_clear_pointer(&ctx->lock, pcv_vm_lock_release);                                                        
    if (restart) {                                                                      
        g_autofree gchar *why = NULL;
        const PcvCtrStartSubmit r = pcv_container_start_submit(ctx->name, pcv_drain_task_actor(res), &why);
        if (r != PCV_CTR_START_SUBMITTED)
            pcv_container_start_report_rejected(ctx->name, r, why, pcv_drain_task_actor(res));
    }
    _ctx_free(ctx);
}

   
                                                                           
  
                                                      
                                                          
                                                                     
                                                            
                                                                                                      
                                               
  
                                                
                                                 
                                                                                      
                                                   
  
                                                                
                                                        
                                  
                                                       
                                                           
                                                              
                                                                
                                                   
                                        
                                     
                                               
                                                       
   
void
handle_container_snapshot_rollback(JsonObject *params, const gchar *rpc_id,
                                    UdsServer *server, GSocketConnection *conn)
{
    if (!params
        || !json_object_has_member(params, "name")
        || !json_object_has_member(params, "snap_name")) {
        gchar *e = pure_rpc_build_error_response(
                       rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Missing parameter: 'name' and/or 'snap_name'");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }
    const gchar  *name      = json_object_get_string_member(params, "name");
    const gchar  *snap_name = json_object_get_string_member(params, "snap_name");

                    
    if (!pcv_validate_vm_name(name)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Invalid container name: 1-64 chars [a-zA-Z0-9_-]");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }
    if (!pcv_validate_snap_name(snap_name)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Invalid snap_name: 1-128 chars [a-zA-Z0-9_-]");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }

                                                                                          
    gchar *lock_err = NULL;
    PcvVmLock *lock = pcv_vm_lock_acquire(name, VM_OP_SNAPSHOT, &lock_err);
    if (!lock) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY, lock_err ? lock_err : "Container is busy");
        pure_uds_server_send_response(server, conn, e); g_free(e); g_free(lock_err); return;
    }
    ContainerCtx *ctx  = _ctx_new(name, rpc_id, server, conn);
    ctx->str_param     = g_strdup(snap_name);
    ctx->lock          = lock;                                                               
    pcv_lxc_snapshot_rollback_async(name, snap_name, NULL, _on_snap_rollback_done, ctx);
}

                                                                             
                            
  
                                                                  
                                                         
  
                                                                                          
                                                                                
                                     
  
                                                 
                        
                                                                               

                                                                                                  
static void
_on_snap_delete_done(GObject *src __attribute__((unused)), GAsyncResult *res,
                     gpointer user_data)
{
    ContainerCtx *ctx = (ContainerCtx *)user_data;
    GError       *error = NULL;
    gboolean ok = pcv_lxc_snapshot_delete_finish(res, &error);
    g_autofree gchar *target = g_strdup_printf("%s:%s", ctx->name, ctx->str_param ? ctx->str_param : "");
    const gint fail_code = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT) ? PURE_RPC_ERR_BUSY : PURE_RPC_ERR_INTERNAL_ERROR;
    pcv_audit_log(pcv_drain_task_actor(res), "container.snapshot.delete", target, ok ? "ok" : "fail", ok ? 0 : fail_code, 0, "local");
    if (!ok)
        _send_error(ctx, fail_code, error ? error->message : "snapshot.delete failed");
    else
        _send_ok(ctx);
    if (error) g_error_free(error);
    _ctx_free(ctx);
}

   
                                                                       
  
                                              
                                                      
                                                
                      
  
                                                  
                                                    
                                      
  
                                                                
                                                                 
                                                                             
             
                                                  
                                        
                                     
                                               
                                                       
   
void
handle_container_snapshot_delete(JsonObject *params, const gchar *rpc_id,
                                   UdsServer *server, GSocketConnection *conn)
{
    if (!params
        || !json_object_has_member(params, "name")
        || !json_object_has_member(params, "snap_name")) {
        gchar *e = pure_rpc_build_error_response(
                       rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Missing parameter: 'name' and/or 'snap_name'");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }
    const gchar  *name      = json_object_get_string_member(params, "name");
    const gchar  *snap_name = json_object_get_string_member(params, "snap_name");

                    
    if (!pcv_validate_vm_name(name)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Invalid container name: 1-64 chars [a-zA-Z0-9_-]");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }
    if (!pcv_validate_snap_name(snap_name)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Invalid snap_name: 1-128 chars [a-zA-Z0-9_-]");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }

    ContainerCtx *ctx  = _ctx_new(name, rpc_id, server, conn);
    ctx->str_param     = g_strdup(snap_name);
    pcv_lxc_snapshot_delete_async(name, snap_name, NULL, _on_snap_delete_done, ctx);
}

                                                                             
                          
  
                                                         
  
                              
  
                        
                                            
                                                                               

                                                                                                   
                                                      
static void
_on_snap_list_done(GObject *src __attribute__((unused)), GAsyncResult *res,
                   gpointer user_data)
{
    ContainerCtx *ctx = (ContainerCtx *)user_data;
    GError       *error = NULL;

    GPtrArray *snaps = pcv_lxc_snapshot_list_finish(res, &error);
    pcv_audit_log(pcv_drain_task_actor(res), "container.snapshot.list", ctx->name, snaps ? "ok" : "fail", snaps ? 0 : PURE_RPC_ERR_INTERNAL_ERROR, 0, "local");
    if (!snaps) {
        _send_error(ctx, PURE_RPC_ERR_INTERNAL_ERROR,
                    error ? error->message : "snapshot.list failed");
        if (error) g_error_free(error);
        _ctx_free(ctx); return;
    }

    JsonArray *arr = json_array_new();
    for (guint i = 0; i < snaps->len; i++)
        json_array_add_string_element(arr, (const gchar *)g_ptr_array_index(snaps, i));
    g_ptr_array_unref(snaps);

    JsonNode *node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(node, arr);
    gchar *resp = pure_rpc_build_success_response(ctx->rpc_id, node);
    pure_uds_server_send_response(ctx->server, ctx->conn, resp);
    g_free(resp);
    _ctx_free(ctx);
}

   
                                                          
  
                                                         
                                             
  
                                                     
                                 
  
                                                            
                                                   
                                                                 
                                  
                           
                                        
                                     
                                               
                                                    
   
void
handle_container_snapshot_list(JsonObject *params, const gchar *rpc_id,
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
                       "Invalid container name: 1-64 chars [a-zA-Z0-9_-]");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }

    ContainerCtx *ctx = _ctx_new(name, rpc_id, server, conn);
    pcv_lxc_snapshot_list_async(name, NULL, _on_snap_list_done, ctx);
}

                                                                             
                                                            
  
                                                            
                                                                   
  
                                                     
                                                                           
  
                                                                                      
                                                                  
                               
                                                                            
                                                                               

   
                                                        
  
                                                 
                                                         
                                                            
                                                        
                                                                 
                                                                                          
                                                                          
                                                    
  
                                                          
                                                       
                                                       
  
                                                          
                                                   
                                           
                                               
                                                           
                                        
                                              
                                                  
                                                         
   
void
handle_container_logs(JsonObject *params, const gchar *rpc_id,
                       UdsServer *server, GSocketConnection *conn)
{
    if (!params || !json_object_has_member(params, "name")) {
        gchar *e = pure_rpc_build_error_response(
                       rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Missing parameter: 'name'");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }
    const gchar *name = json_object_get_string_member(params, "name");
    gint n_lines = json_object_has_member(params, "lines")
                   ? (gint)json_object_get_int_member(params, "lines") : 50;

    if (!pcv_validate_vm_name(name)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Invalid container name: 1-64 chars [a-zA-Z0-9_-]");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }
    if (n_lines < 1) n_lines = 1;                                   
    if (n_lines > 10000) n_lines = 10000;                                        

    gint total_lines = 0;
    GPtrArray *lines = pcv_lxc_unit_log_tail(PCV_LXC_PATH, name, "/var/log/lxc", n_lines, &total_lines, NULL);
    if (!lines) {                                 
        gchar *e = pure_rpc_build_error_response(
                       rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
                       "No log file found for container");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }

    JsonArray *arr = json_array_new();
    for (guint i = 0; i < lines->len; i++)
        json_array_add_string_element(arr, (const gchar *)g_ptr_array_index(lines, i));
    g_ptr_array_unref(lines);

    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "name", name);
    json_object_set_array_member(obj, "lines", arr);
    json_object_set_int_member(obj, "total", total_lines);

    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, obj);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, conn, resp);
    g_free(resp);
}

                                                                             
                                                           
  
                                                       
                                                      
               
  
                            
                                                    
  
                                           
                                                                               

void
handle_container_env_set(JsonObject *params, const gchar *rpc_id,
                          UdsServer *server, GSocketConnection *conn)
{
    if (!params
        || !json_object_has_member(params, "name")
        || !json_object_has_member(params, "key")
        || !json_object_has_member(params, "value")) {
        gchar *e = pure_rpc_build_error_response(
                       rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Missing required: name, key, value");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }
    const gchar *name  = pcv_rpc_params_get_string(params, "name");
    const gchar *key   = pcv_rpc_params_get_string(params, "key");
    const gchar *value = pcv_rpc_params_get_string(params, "value");
                                                                          
                                                               
    if (!name || !key || !*key || !value ||
        (*value && !pcv_container_config_token_safe(value, TRUE))) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "name, key, value must be strings; key non-empty; value without control characters");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }

    if (!pcv_validate_vm_name(name)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Invalid container name");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }
                                            
                                                               
    for (const gchar *p = key; *p; p++) {                               
        if (!g_ascii_isalnum(*p) && *p != '_') {
            gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                           "Invalid env key: alphanumeric and '_' only");
            pure_uds_server_send_response(server, conn, e); g_free(e); return;
        }
    }

                                                                                                
                                                                                   
                                                      
    gchar *lock_err = NULL;
    PcvVmLock *op_lock = pcv_vm_lock_acquire(name, VM_OP_TUNING, &lock_err);
    if (!op_lock) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY, lock_err ? lock_err : "Container is busy");
        pure_uds_server_send_response(server, conn, e); g_free(e); g_free(lock_err); return;
    }

                                    
    gchar *config_path = NULL;
    GError *cfg_err = NULL;
    if (!_ensure_container_config_ready(name, &config_path, &cfg_err)) {
        gchar *e = pure_rpc_build_error_response(
                       rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
                       cfg_err ? cfg_err->message : "container config unavailable");
        pcv_vm_lock_release(op_lock);                  
        pure_uds_server_send_response(server, conn, e);
        g_free(e);
        if (cfg_err) g_error_free(cfg_err);
        return;
    }
    gchar *contents = NULL;
                                                                 
    gchar *search_prefix = g_strdup_printf("lxc.environment = %s=", key);
                                                                             
    g_autoptr(GMutexLocker) cfg_lock = g_mutex_locker_new(pcv_lxc_config_mutex());

                                                      
    if (g_file_get_contents(config_path, &contents, NULL, NULL)) {
        gchar **lines = g_strsplit(contents, "\n", -1);
        GString *out = g_string_new(NULL);
        for (gint i = 0; lines[i]; i++) {
            gchar *trimmed = g_strstrip(g_strdup(lines[i]));                           
            if (g_str_has_prefix(trimmed, search_prefix)) {
                g_free(trimmed);
                continue;               
            }
            g_free(trimmed);
            g_string_append(out, lines[i]);                        
            if (lines[i + 1]) g_string_append_c(out, '\n');
        }
        g_strfreev(lines);
        g_free(contents);
        contents = g_string_free(out, FALSE);                                      
    }

                                                                   
    gchar *entry = g_strdup_printf("lxc.environment = %s=%s\n", key, value);
    gchar *full = g_strconcat(contents ? contents : "", entry, NULL);
    gboolean saved = g_file_set_contents(config_path, full, -1, NULL);
    g_clear_pointer(&cfg_lock, g_mutex_locker_free);
    g_free(full); g_free(contents); g_free(entry); g_free(search_prefix); g_free(config_path);
    pcv_vm_lock_release(op_lock);                  
    if (!saved) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INTERNAL_ERROR, "Failed to persist container environment");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }

    JsonObject *res = json_object_new();
    json_object_set_boolean_member(res, "success", TRUE);
    json_object_set_string_member(res, "note", "restart required");
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, conn, resp);
    g_free(resp);
}

                                                                             
                                                             
  
                                                      
  
              
                                                                               
                                                                               

void
handle_container_env_list(JsonObject *params, const gchar *rpc_id,
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
    if (!_ensure_container_config_ready(name, &config_path, &cfg_err)) {
        gchar *e = pure_rpc_build_error_response(
                       rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
                       cfg_err ? cfg_err->message : "container config unavailable");
        pure_uds_server_send_response(server, conn, e);
        g_free(e);
        if (cfg_err) g_error_free(cfg_err);
        return;
    }
    gchar *contents = NULL;
    JsonObject *env_obj = json_object_new();

    if (g_file_get_contents(config_path, &contents, NULL, NULL)) {
        gchar **lines = g_strsplit(contents, "\n", -1);
        for (gint i = 0; lines[i]; i++) {
            gchar *trimmed = g_strstrip(g_strdup(lines[i]));
            if (!g_str_has_prefix(trimmed, "lxc.environment")) {
                g_free(trimmed); continue;
            }
            gchar *eq = strchr(trimmed, '=');                                       
            if (!eq) { g_free(trimmed); continue; }
                                                                             
            gchar *kv = g_strstrip(g_strdup(eq + 1));                             
            gchar *sep = strchr(kv, '=');                                            
            if (sep) {
                *sep = '\0';                                              
                json_object_set_string_member(env_obj, kv, sep + 1);
            }
            g_free(kv);
            g_free(trimmed);
        }
        g_strfreev(lines);
        g_free(contents);
    }
    g_free(config_path);

    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, env_obj);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, conn, resp);
    g_free(resp);
}

                                                                             
                                                               
  
                                                      
                                             
  
                    
                                                    
                                                                               

void
handle_container_env_delete(JsonObject *params, const gchar *rpc_id,
                             UdsServer *server, GSocketConnection *conn)
{
    if (!params
        || !json_object_has_member(params, "name")
        || !json_object_has_member(params, "key")) {
        gchar *e = pure_rpc_build_error_response(
                       rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Missing required: name, key");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }
    const gchar *name = pcv_rpc_params_get_string(params, "name");
    const gchar *key  = pcv_rpc_params_get_string(params, "key");
                                                               
                                                  
    gboolean key_ok = key && *key;
    for (const gchar *p = key; key_ok && *p; p++)
        key_ok = g_ascii_isalnum(*p) || *p == '_';
    if (!name || !key_ok) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "name and key must be strings; key alphanumeric and '_' only");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }

    if (!pcv_validate_vm_name(name)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Invalid container name");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }

                                                                                                
                                                                                   
                                                      
    gchar *lock_err = NULL;
    PcvVmLock *op_lock = pcv_vm_lock_acquire(name, VM_OP_TUNING, &lock_err);
    if (!op_lock) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY, lock_err ? lock_err : "Container is busy");
        pure_uds_server_send_response(server, conn, e); g_free(e); g_free(lock_err); return;
    }

    gchar *config_path = NULL;
    GError *cfg_err = NULL;
    if (!_ensure_container_config_ready(name, &config_path, &cfg_err)) {
        gchar *e = pure_rpc_build_error_response(
                       rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
                       cfg_err ? cfg_err->message : "container config unavailable");
        pcv_vm_lock_release(op_lock);                  
        pure_uds_server_send_response(server, conn, e);
        g_free(e);
        if (cfg_err) g_error_free(cfg_err);
        return;
    }
    gchar *contents = NULL;
    gchar *search_prefix = g_strdup_printf("lxc.environment = %s=", key);
    g_autoptr(GMutexLocker) cfg_lock = g_mutex_locker_new(pcv_lxc_config_mutex());                             
    if (g_file_get_contents(config_path, &contents, NULL, NULL)) {
        gchar **lines = g_strsplit(contents, "\n", -1);
        GString *out = g_string_new(NULL);
        for (gint i = 0; lines[i]; i++) {
            gchar *trimmed = g_strstrip(g_strdup(lines[i]));
            if (g_str_has_prefix(trimmed, search_prefix)) {
                g_free(trimmed);
                continue;
            }
            g_free(trimmed);
            g_string_append(out, lines[i]);
            if (lines[i + 1]) g_string_append_c(out, '\n');
        }
        g_file_set_contents(config_path, out->str, -1, NULL);
        g_string_free(out, TRUE);
        g_strfreev(lines);
        g_free(contents);
    }
    g_clear_pointer(&cfg_lock, g_mutex_locker_free);                  
    g_free(search_prefix);
    g_free(config_path);
    pcv_vm_lock_release(op_lock);                  

    JsonObject *res_env = json_object_new();
    json_object_set_boolean_member(res_env, "success", TRUE);
    json_object_set_string_member(res_env, "note", "restart required");
    JsonNode *node_env = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node_env, res_env);
    gchar *resp_env = pure_rpc_build_success_response(rpc_id, node_env);
    pure_uds_server_send_response(server, conn, resp_env);
    g_free(resp_env);
}

