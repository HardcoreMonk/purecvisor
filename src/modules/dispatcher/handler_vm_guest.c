   
                           
                                                     
  
                           
                                                                                                           
                                                                                                                             
                                                                             
  
                                                      
   

#include "api/drain.h"
#include <unistd.h>
#include <glib.h>
#include <gio/gio.h>
#include <libvirt/libvirt.h>
#include <libvirt/libvirt-qemu.h>
#include <libvirt/virterror.h>
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include "modules/audit/pcv_audit.h"
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include "api/uds_server.h"
#include "modules/dispatcher/rpc_utils.h"
#include "modules/dispatcher/handler_vm_lifecycle.h"
#include "modules/dispatcher/guest_agent_exec.h"
#include "modules/dispatcher/guest_exec_request.h"
#include "utils/pcv_spawn.h"
#include "modules/virt/virt_conn_pool.h"
#include "purecvisor/pcv_handler_util.h"
#include "modules/dispatcher/handler_vm_internal.h"


                                                                    
                              
  
                                           
                                                               
  
                             
                                         
                                                                       

   
                              
                                       
                       
  
                                                                     
                                       
   
const gchar *
pcv_disp_guest_get_vm_name(JsonObject *params)
{
    if (!params) return NULL;
    if (json_object_has_member(params, "name"))
        return json_object_get_string_member(params, "name");
    if (json_object_has_member(params, "vm_id"))
        return json_object_get_string_member(params, "vm_id");
    return NULL;
}

                                                                    
                                              
                                                                       

   
                                                                                    
                                                                
  
                                                                     
                                                                  
                                                                                                  
  
                                                                                         
   
void
handle_vm_guest_agent_status_request(JsonObject *params, const gchar *rpc_id,
                                     UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm_name = pcv_disp_guest_get_vm_name(params);
    if (!vm_name) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                         "Invalid params: 'name' missing");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }
    pcv_guest_agent_status_submit(vm_name, rpc_id, server, connection);
}

                                                                    
                                                              
                                                                       

   
                                                                                
                                                                
  
                                                         
                                                       
                                               
  
                                                                           
                                                               
   
void
handle_vm_guest_agent_ensure_channel_request(JsonObject *params, const gchar *rpc_id,
                                             UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm_name = pcv_disp_guest_get_vm_name(params);
    if (!vm_name) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                         "Invalid params: 'name' missing");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
                         "Failed to connect to Libvirt.");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    virDomainPtr dom = pure_virt_get_domain(conn, vm_name);
    if (!dom) {
        virt_conn_pool_release(conn);
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_VM_NOT_FOUND,
                         "VM not found.");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    gboolean running = virDomainIsActive(dom) == 1;
    gboolean persistent = virDomainIsPersistent(dom) == 1;
    char *config_xml = virDomainGetXMLDesc(dom, VIR_DOMAIN_XML_INACTIVE);
    char *live_xml = virDomainGetXMLDesc(dom, 0);
    gboolean channel_configured = pcv_guest_agent_xml_has_channel(config_xml);
    gboolean channel_live = pcv_guest_agent_xml_has_channel(live_xml);
    gboolean config_changed = FALSE;
    gboolean live_changed = FALSE;
    gchar *config_error = NULL;
    gchar *live_error = NULL;

                                                      
    const gchar *channel_xml =
        "<channel type='unix'>"
          "<target type='virtio' name='org.qemu.guest_agent.0'/>"
        "</channel>";

                                                                               
    if (!channel_configured && persistent) {
        if (virDomainAttachDeviceFlags(dom, channel_xml, VIR_DOMAIN_AFFECT_CONFIG) == 0) {
            channel_configured = TRUE;
            config_changed = TRUE;
        } else {
            const char *vir_err = virGetLastErrorMessage();
            config_error = g_strdup(vir_err ? vir_err : "unknown error");                       
            virResetLastError();
        }
    }

                                                                  
    if (running && !channel_live) {
        if (virDomainAttachDeviceFlags(dom, channel_xml, VIR_DOMAIN_AFFECT_LIVE) == 0) {
            channel_live = TRUE;
            live_changed = TRUE;
        } else {
            const char *vir_err = virGetLastErrorMessage();
            live_error = g_strdup(vir_err ? vir_err : "unknown error");
            virResetLastError();
        }
    }

    if (!channel_configured && !channel_live) {
        gchar *msg = g_strdup_printf(
            "Failed to add guest agent channel.%s%s%s%s",
            config_error ? " config: " : "", config_error ? config_error : "",
            live_error ? " live: " : "", live_error ? live_error : "");
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, msg);
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        g_free(msg);
    } else {
        JsonObject *res_obj = json_object_new();
        json_object_set_string_member(res_obj, "name", vm_name);
        json_object_set_string_member(res_obj, "status",
            (config_changed || live_changed) ? "updated" : "already_configured");
        json_object_set_boolean_member(res_obj, "running", running);
        json_object_set_boolean_member(res_obj, "persistent", persistent);
        json_object_set_boolean_member(res_obj, "changed", config_changed || live_changed);
        json_object_set_boolean_member(res_obj, "channel_configured", channel_configured);
        json_object_set_boolean_member(res_obj, "channel_live", channel_live);
        json_object_set_boolean_member(res_obj, "reboot_required", channel_configured && running && !channel_live);
        json_object_set_boolean_member(res_obj, "install_required", TRUE);
        if (config_error)
            json_object_set_string_member(res_obj, "config_warning", config_error);
        if (live_error)
            json_object_set_string_member(res_obj, "live_warning", live_error);
        pcv_guest_agent_add_install_commands(res_obj);

        JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(res_node, res_obj);
        gchar *resp = pure_rpc_build_success_response(rpc_id, res_node);
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
    }

    g_free(config_error);
    g_free(live_error);
    if (config_xml) free(config_xml);
    if (live_xml) free(live_xml);
    virDomainFree(dom);
    virt_conn_pool_release(conn);
}

                                                                    
                                                  
                                                                       

   
                                                                        
                                                       
                                                       
   
static gboolean
_guest_fsinfo_get_int64(JsonObject *obj, const gchar *member, gint64 *out)
{
    if (!obj || !member || !json_object_has_member(obj, member))
        return FALSE;
    *out = json_object_get_int_member(obj, member);
    return TRUE;
}

   
                                                                 
                                                         
                                        
                                                          
   
static gboolean
_guest_fsinfo_should_count(const gchar *type)
{
                                 
    static const gchar *skip[] = {
        "tmpfs", "devtmpfs", "proc", "sysfs", "devpts",
        "cgroup", "cgroup2", "squashfs", "overlay", NULL
    };

    if (!type || !*type)
        return TRUE;                                                              
    for (guint i = 0; skip[i]; i++) {
        if (g_strcmp0(type, skip[i]) == 0)
            return FALSE;                                                      
    }
    return TRUE;
}

   
                                                                       
                                                        
                                           
                                                                    
                                                                   
   
static void
_guest_fsinfo_worker(GTask *task, gpointer source_obj __attribute__((unused)),
                     gpointer task_data, GCancellable *cancellable __attribute__((unused)))
{
    VmLifecycleCtx *ctx = (VmLifecycleCtx *)task_data;

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Failed to connect to Libvirt.");
        return;
    }

    virDomainPtr dom = pure_virt_get_domain(conn, ctx->vm_id);
    if (!dom) {
        virt_conn_pool_release(conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                "VM not found: %s", ctx->vm_id);
        return;
    }

    if (!virDomainIsActive(dom)) {
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "VM '%s' is not running.", ctx->vm_id);
        return;
    }

    char *agent_result = virDomainQemuAgentCommand(
        dom, "{\"execute\":\"guest-get-fsinfo\"}", 10, 0);
                                                    
                                                          
    g_autofree gchar *agent_error = !agent_result
        ? g_utf8_make_valid(virGetLastErrorMessage() ?: "unknown error", -1) : NULL;
    virDomainFree(dom);
    virt_conn_pool_release(conn);

    if (!agent_result) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "guest-get-fsinfo failed on '%s': %s",
                                ctx->vm_id, agent_error);
        return;
    }

    GError *parse_error = NULL;
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, agent_result, -1, &parse_error)) {
        free(agent_result);
        g_object_unref(parser);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Failed to parse guest-get-fsinfo response: %s",
                                parse_error ? parse_error->message : "invalid JSON");
        g_clear_error(&parse_error);
        return;
    }
    free(agent_result);

    JsonNode *root = json_parser_get_root(parser);
    JsonObject *root_obj = root && JSON_NODE_HOLDS_OBJECT(root)
        ? json_node_get_object(root) : NULL;
    if (!root_obj || !json_object_has_member(root_obj, "return")) {
        g_object_unref(parser);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "guest-get-fsinfo response missing return array.");
        return;
    }

                                                                       
    JsonArray *raw_filesystems = json_object_get_array_member(root_obj, "return");
    JsonArray *filesystems = json_array_new();
    gint64 total_bytes = 0;                                                   
    gint64 used_bytes = 0;                                                   

    for (guint i = 0; raw_filesystems && i < json_array_get_length(raw_filesystems); i++) {
        JsonObject *raw = json_array_get_object_element(raw_filesystems, i);
        if (!raw)
            continue;                                                   

        const gchar *name = json_object_has_member(raw, "name")
            ? json_object_get_string_member(raw, "name") : "";
        const gchar *mountpoint = json_object_has_member(raw, "mountpoint")
            ? json_object_get_string_member(raw, "mountpoint") : "";
        const gchar *type = json_object_has_member(raw, "type")
            ? json_object_get_string_member(raw, "type") : "";

        gint64 fs_total = 0;
        gint64 fs_used = 0;
        gboolean has_total = _guest_fsinfo_get_int64(raw, "total-bytes", &fs_total);
        gboolean has_used = _guest_fsinfo_get_int64(raw, "used-bytes", &fs_used);

        JsonObject *fs_obj = json_object_new();
        json_object_set_string_member(fs_obj, "name", name);
        json_object_set_string_member(fs_obj, "mountpoint", mountpoint);
        json_object_set_string_member(fs_obj, "type", type);
        if (has_total)
            json_object_set_int_member(fs_obj, "total_bytes", fs_total);
        if (has_used)
            json_object_set_int_member(fs_obj, "used_bytes", fs_used);
                                                              
        if (has_total && has_used && fs_total >= fs_used) {
            json_object_set_int_member(fs_obj, "available_bytes", fs_total - fs_used);
            if (fs_total > 0) {                                       
                double pct = ((double)fs_used * 100.0) / (double)fs_total;
                json_object_set_double_member(fs_obj, "usage_percent", pct);
            }
        }

        if (json_object_has_member(raw, "disk")) {
            JsonArray *disks = json_object_get_array_member(raw, "disk");
            if (disks && json_array_get_length(disks) > 0) {
                JsonObject *disk = json_array_get_object_element(disks, 0);
                if (disk && json_object_has_member(disk, "dev"))
                    json_object_set_string_member(fs_obj, "device",
                                                  json_object_get_string_member(disk, "dev"));
            }
        }

                                                          
        if (has_total && has_used && fs_total > 0 && _guest_fsinfo_should_count(type)) {
            total_bytes += fs_total;
            used_bytes += fs_used;
        }

        json_array_add_object_element(filesystems, fs_obj);                          
    }

    JsonObject *res_obj = json_object_new();
    json_object_set_string_member(res_obj, "name", ctx->vm_id);
    json_object_set_string_member(res_obj, "status", "ok");
    json_object_set_int_member(res_obj, "total_bytes", total_bytes);
    json_object_set_int_member(res_obj, "used_bytes", used_bytes);
    if (total_bytes > 0)
        json_object_set_double_member(res_obj, "usage_percent",
                                      ((double)used_bytes * 100.0) / (double)total_bytes);
    json_object_set_array_member(res_obj, "filesystems", filesystems);

    JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(res_node, res_obj);
    g_object_unref(parser);
    g_task_return_pointer(task, res_node, (GDestroyNotify)json_node_free);
}

   
                                                                   
                                                     
   
static void
_guest_fsinfo_callback(GObject *source_obj __attribute__((unused)),
                       GAsyncResult *res, gpointer user_data)
{
    GTask *task = G_TASK(res);
    VmLifecycleCtx *ctx = (VmLifecycleCtx *)user_data;
    GError *error = NULL;
    JsonNode *result_node = g_task_propagate_pointer(task, &error);

    pcv_audit_log(pcv_drain_task_actor(res), "vm.guest.fsinfo", ctx->vm_id ?: "",
                  error ? "fail" : "ok",
                  error ? PURE_RPC_ERR_ZFS_OPERATION : 0, 0, "local");

    if (error) {
        gchar *err_resp = pure_rpc_build_error_response(ctx->rpc_id,
            PURE_RPC_ERR_ZFS_OPERATION, error->message);
        pure_uds_server_send_response(ctx->server, ctx->connection, err_resp);
        g_free(err_resp);
        g_error_free(error);
    } else {
        gchar *resp = pure_rpc_build_success_response(ctx->rpc_id, result_node);
        pure_uds_server_send_response(ctx->server, ctx->connection, resp);
        g_free(resp);
    }
}

   
                                                                              
                                                               
                                                                     
   
void
handle_vm_guest_fsinfo_request(JsonObject *params, const gchar *rpc_id,
                               UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm_name = pcv_disp_guest_get_vm_name(params);
    if (!vm_name) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                         "Invalid params: 'name' missing");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

                                                                                         
    VmLifecycleCtx *ctx = g_new0(VmLifecycleCtx, 1);
    ctx->vm_id = g_strdup(vm_name);
    ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = g_object_ref(server);
    ctx->connection = g_object_ref(connection);

    GTask *task = pcv_drain_task_new(NULL, NULL, _guest_fsinfo_callback, ctx);
    g_task_set_task_data(task, ctx, pcv_disp_free_lifecycle_ctx);
    g_task_run_in_thread(task, _guest_fsinfo_worker);                            
    g_object_unref(task);
}

                                                                    
                                         
  
                                           
                                                  
                              
  
                                           
                                                                       

   
                                                                  
                                                          
                                                                 
   
static void
_guest_ping_worker(GTask *task, gpointer source_obj __attribute__((unused)),
                   gpointer task_data, GCancellable *cancellable __attribute__((unused)))
{
    VmLifecycleCtx *ctx = (VmLifecycleCtx *)task_data;

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Failed to connect to Libvirt.");
        return;
    }

    virDomainPtr dom = pure_virt_get_domain(conn, ctx->vm_id);
    if (!dom) {
        virt_conn_pool_release(conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                "VM not found: %s", ctx->vm_id);
        return;
    }

    if (!virDomainIsActive(dom)) {
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "VM '%s' is not running.", ctx->vm_id);
        return;
    }

                                          
    char *result = virDomainQemuAgentCommand(dom, "{\"execute\":\"guest-ping\"}",
                                              5, 0);
    g_autofree gchar *agent_error = !result
        ? g_utf8_make_valid(virGetLastErrorMessage() ?: "unknown error", -1) : NULL;
    virDomainFree(dom);
    virt_conn_pool_release(conn);

    if (!result) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Guest agent not available on '%s': %s",
                                ctx->vm_id, agent_error);
        return;
    }

    free(result);                                                         
                                                                                  
    g_task_return_boolean(task, TRUE);
}

   
                                                                              
                                            
   
static void
_guest_ping_callback(GObject *source_obj __attribute__((unused)), GAsyncResult *res, gpointer user_data)
{
    GTask *task = G_TASK(res);
    VmLifecycleCtx *ctx = (VmLifecycleCtx *)user_data;
    GError *error = NULL;
    gboolean _ok = g_task_propagate_boolean(task, &error);
                               
    pcv_audit_log(pcv_drain_task_actor(res), "vm.guest.ping", ctx->vm_id ?: "",
                  _ok ? "ok" : "fail",
                  _ok ? 0 : PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
    if (!_ok) {
        gchar *err_resp = pure_rpc_build_error_response(ctx->rpc_id, PURE_RPC_ERR_ZFS_OPERATION, error->message);
        pure_uds_server_send_response(ctx->server, ctx->connection, err_resp);
        g_free(err_resp);
        g_error_free(error);
    } else {
        JsonObject *res_obj = json_object_new();
        json_object_set_string_member(res_obj, "agent", "connected");
        json_object_set_string_member(res_obj, "name", ctx->vm_id);
        JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(res_node, res_obj);
        gchar *resp = pure_rpc_build_success_response(ctx->rpc_id, res_node);
        pure_uds_server_send_response(ctx->server, ctx->connection, resp);
        g_free(resp);
    }
}

   
                                                                        
                                                           
                                                                     
   
void
handle_vm_guest_ping_request(JsonObject *params, const gchar *rpc_id,
                             UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm_name = pcv_disp_guest_get_vm_name(params);
    if (!vm_name) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                         "Invalid params: 'name' missing");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    VmLifecycleCtx *ctx = g_new0(VmLifecycleCtx, 1);
    ctx->vm_id = g_strdup(vm_name);
    ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = g_object_ref(server);
    ctx->connection = g_object_ref(connection);

    GTask *task = pcv_drain_task_new(NULL, NULL, _guest_ping_callback, ctx);
    g_task_set_task_data(task, ctx, pcv_disp_free_lifecycle_ctx);
    g_task_run_in_thread(task, _guest_ping_worker);
    g_object_unref(task);
}

                                                                    
                                                                  
  
                                             
       
                                                                                                 
                                                                                                       
                                                                
                                                              
                                                                                                            
                                                                                                      
  
                                                                                          
                                                     
                                                                       

   
                
                                     
                                                              
   
typedef struct {
    gchar *vm_id;                              
    gchar *command;                                                           
    gchar *rpc_id;                                      
    UdsServer *server;                          
    GSocketConnection *connection;              
} GuestExecCtx;

   
                                                                     
                                                 
   
static void
_guest_exec_ctx_free(gpointer data)
{
    if (!data) return;
    GuestExecCtx *ctx = (GuestExecCtx *)data;
    g_free(ctx->vm_id);
    g_free(ctx->command);
    g_free(ctx->rpc_id);
    if (ctx->server) g_object_unref(ctx->server);
    if (ctx->connection) g_object_unref(ctx->connection);
    g_free(ctx);
}

   
                                                               
                                          
  
                                                        
                                                                   
                                                              
                                       
  
                                                                                  
                                                                          
                                                                                                      
                  
                                                                                                                 
   
static void
_guest_exec_worker(GTask *task, gpointer source_obj __attribute__((unused)),
                   gpointer task_data, GCancellable *cancellable __attribute__((unused)))
{
    GuestExecCtx *ctx = (GuestExecCtx *)task_data;
    const gint64 start_us = g_get_monotonic_time();                                      

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Failed to connect to Libvirt.");
        return;
    }

    virDomainPtr dom = pure_virt_get_domain(conn, ctx->vm_id);
    if (!dom) {
        virt_conn_pool_release(conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                "VM not found: %s", ctx->vm_id);
        return;
    }

    if (!virDomainIsActive(dom)) {                                             
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "VM '%s' is not running.", ctx->vm_id);
        return;
    }

                                                                                     
                                                                                
    PcvGuestShell shell = PCV_GUEST_SHELL_SH;
    g_autofree gchar *os_id = NULL;
    char *osinfo_result = virDomainQemuAgentCommand(dom, "{\"execute\":\"guest-get-osinfo\"}",
                                                     PCV_GUEST_EXEC_OSINFO_TIMEOUT_SEC, 0);
    if (osinfo_result) {
        shell = pcv_guest_exec_shell_for_osinfo(osinfo_result, &os_id);
        free(osinfo_result);
    } else if (pcv_guest_exec_osinfo_error_is_fatal(virGetLastErrorCode())) {
        g_autofree gchar *vir_err = g_utf8_make_valid(virGetLastErrorMessage() ?: "unknown error", -1);
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "guest agent on '%s' is not responding: %s", ctx->vm_id, vir_err);
        return;
    }

                                                                                             
                                                                                       
                                                                                       
                                                                     
    gchar *exec_json = pcv_guest_exec_request_json(shell, ctx->command);
    if (!exec_json) {                                                                   
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Failed to build guest-exec request for '%s'.", ctx->vm_id);
        return;
    }

    char *exec_result = virDomainQemuAgentCommand(dom, exec_json,
                                                   pcv_guest_exec_call_timeout(start_us, g_get_monotonic_time()), 0);
    g_free(exec_json);

    if (!exec_result) {
        g_autofree gchar *vir_err = g_utf8_make_valid(virGetLastErrorMessage() ?: "unknown error", -1);
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "guest-exec failed on '%s': %s",
                                ctx->vm_id, vir_err ? vir_err : "unknown error");
        return;
    }

                                   
    JsonParser *parser = json_parser_new();
    gint64 pid = -1;
    if (json_parser_load_from_data(parser, exec_result, -1, NULL)) {
        JsonNode *root = json_parser_get_root(parser);
        if (root && JSON_NODE_HOLDS_OBJECT(root)) {
            JsonObject *obj = json_node_get_object(root);
            if (json_object_has_member(obj, "return")) {
                JsonObject *ret_obj = json_object_get_object_member(obj, "return");
                if (ret_obj && json_object_has_member(ret_obj, "pid"))
                    pid = json_object_get_int_member(ret_obj, "pid");
            }
        }
    }
    g_object_unref(parser);
    free(exec_result);

    if (pid < 0) {
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Failed to parse guest-exec PID from response.");
        return;
    }

                                                                              
                                                                     
                                             
                                                                      
                                                       
    gint64 exitcode = -1;
    gboolean exited = FALSE;
    g_autofree guchar *stdout_raw = NULL;                                                    
    g_autofree guchar *stderr_raw = NULL;
    gsize stdout_len = 0, stderr_len = 0;

    gchar *status_json = pcv_guest_exec_status_json(pid);

    for (;;) {
        char *status_result = virDomainQemuAgentCommand(dom, status_json,
                                                         pcv_guest_exec_call_timeout(start_us, g_get_monotonic_time()), 0);
        if (!status_result) {
            g_free(status_json);
            virDomainFree(dom);
            virt_conn_pool_release(conn);
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                    "guest-exec-status failed for PID %" G_GINT64_FORMAT, pid);
            return;
        }

                                                                       
        JsonParser *sp = json_parser_new();
        if (json_parser_load_from_data(sp, status_result, -1, NULL)) {
            JsonNode *sroot = json_parser_get_root(sp);
            if (sroot && JSON_NODE_HOLDS_OBJECT(sroot)) {
                JsonObject *sobj = json_node_get_object(sroot);
                if (json_object_has_member(sobj, "return")) {
                    JsonObject *sret = json_object_get_object_member(sobj, "return");
                    if (sret) {
                        exited = json_object_get_boolean_member_with_default(
                                     sret, "exited", FALSE);
                        if (exited) {
                            if (json_object_has_member(sret, "exitcode"))
                                exitcode = json_object_get_int_member(sret, "exitcode");

                                                                                      
                            const gchar *o64 = json_object_get_string_member_with_default(sret, "out-data", NULL);
                            const gchar *e64 = json_object_get_string_member_with_default(sret, "err-data", NULL);
                            if (o64) stdout_raw = g_base64_decode(o64, &stdout_len);
                            if (e64) stderr_raw = g_base64_decode(e64, &stderr_len);
                        }
                    }
                }
            }
        }
        g_object_unref(sp);
        free(status_result);

        if (exited || !pcv_guest_exec_may_poll(start_us, g_get_monotonic_time()))
            break;                                                                        
        g_usleep(PCV_GUEST_EXEC_POLL_INTERVAL_US);                       
    }
    g_free(status_json);
    virDomainFree(dom);
    virt_conn_pool_release(conn);

                            
    JsonObject *res_obj = json_object_new();
    json_object_set_string_member(res_obj, "name", ctx->vm_id);
    json_object_set_int_member(res_obj, "exitcode", exitcode);
    json_object_set_boolean_member(res_obj, "exited", exited);
    json_object_set_string_member(res_obj, "shell", shell == PCV_GUEST_SHELL_POWERSHELL ? "powershell" : "sh");
    if (os_id)                                                                     
        json_object_set_string_member(res_obj, "os_id", os_id);
                                                                                    
    pcv_guest_exec_set_output(res_obj, "stdout", "stdout_b64", stdout_raw, stdout_len);
    pcv_guest_exec_set_output(res_obj, "stderr", "stderr_b64", stderr_raw, stderr_len);

    JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(res_node, res_obj);
    g_task_return_pointer(task, res_node, (GDestroyNotify)json_node_free);
}

   
                                                                  
                                                             
   
static void
_guest_exec_callback(GObject *source_obj __attribute__((unused)), GAsyncResult *res, gpointer user_data)
{
    GTask *task = G_TASK(res);
    GuestExecCtx *ctx = (GuestExecCtx *)user_data;
    GError *error = NULL;

    JsonNode *result_node = g_task_propagate_pointer(task, &error);
                                               
    pcv_audit_log(pcv_drain_task_actor(res), "vm.guest.exec", ctx->vm_id ?: "",
                  error ? "fail" : "ok",
                  error ? PURE_RPC_ERR_ZFS_OPERATION : 0, 0, "local");
    if (error) {
        gchar *err_resp = pure_rpc_build_error_response(ctx->rpc_id, PURE_RPC_ERR_ZFS_OPERATION, error->message);
        pure_uds_server_send_response(ctx->server, ctx->connection, err_resp);
        g_free(err_resp);
        g_error_free(error);
    } else {
        gchar *resp = pure_rpc_build_success_response(ctx->rpc_id, result_node);
        pure_uds_server_send_response(ctx->server, ctx->connection, resp);
        g_free(resp);
    }
}

   
                                                                          
  
                                                         
                                               
  
                                                                             
                                                               
   
void
handle_vm_guest_exec_request(JsonObject *params, const gchar *rpc_id,
                             UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm_name = pcv_disp_guest_get_vm_name(params);
    if (!vm_name) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                         "Invalid params: 'name' missing");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    const gchar *command = NULL;
    if (params && json_object_has_member(params, "command"))
        command = json_object_get_string_member(params, "command");

    if (!command || strlen(command) == 0) {                          
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                         "Invalid params: 'command' must be non-empty");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    if (strlen(command) > 1024) {                                                
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                         "Invalid params: 'command' exceeds 1024 characters");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    GuestExecCtx *ctx = g_new0(GuestExecCtx, 1);
    ctx->vm_id = g_strdup(vm_name);
    ctx->command = g_strdup(command);
    ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = g_object_ref(server);
    ctx->connection = g_object_ref(connection);

    GTask *task = pcv_drain_task_new(NULL, NULL, _guest_exec_callback, ctx);
    g_task_set_task_data(task, ctx, _guest_exec_ctx_free);
    g_task_run_in_thread(task, _guest_exec_worker);
    g_object_unref(task);
}

                                                                    
                                                  
  
       
                                                       
                                                    
  
                                                                                 
                                                                       

   
                                                                          
                                                         
                                                                   
                                                                  
   
static void
_guest_shutdown_worker(GTask *task, gpointer source_obj __attribute__((unused)),
                       gpointer task_data, GCancellable *cancellable __attribute__((unused)))
{
    VmLifecycleCtx *ctx = (VmLifecycleCtx *)task_data;

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Failed to connect to Libvirt.");
        return;
    }

    virDomainPtr dom = pure_virt_get_domain(conn, ctx->vm_id);
    if (!dom) {
        virt_conn_pool_release(conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                "VM not found: %s", ctx->vm_id);
        return;
    }

    if (!virDomainIsActive(dom)) {
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "VM '%s' is not running.", ctx->vm_id);
        return;
    }

                                  
    const gchar *method_used = "guest-agent";
    int rc = virDomainShutdownFlags(dom, VIR_DOMAIN_SHUTDOWN_GUEST_AGENT);
    if (rc < 0) {
                               
        virResetLastError();
        method_used = "acpi";
        rc = virDomainShutdown(dom);
        if (rc < 0) {
            g_autofree gchar *vir_err = g_utf8_make_valid(virGetLastErrorMessage() ?: "unknown error", -1);
            virDomainFree(dom);
            virt_conn_pool_release(conn);
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                    "Failed to shutdown VM '%s': %s",
                                    ctx->vm_id, vir_err ? vir_err : "unknown error");
            return;
        }
    }

    virDomainFree(dom);
    virt_conn_pool_release(conn);

                                                          
    g_free(ctx->action);
    ctx->action = g_strdup(method_used);
    g_task_return_boolean(task, TRUE);
}

   
                                                                                         
                                                              
                                                                
   
static void
_guest_shutdown_callback(GObject *source_obj __attribute__((unused)), GAsyncResult *res, gpointer user_data)
{
    GTask *task = G_TASK(res);
    VmLifecycleCtx *ctx = (VmLifecycleCtx *)user_data;
    GError *error = NULL;
    gboolean _ok = g_task_propagate_boolean(task, &error);
                               
    pcv_audit_log(pcv_drain_task_actor(res), "vm.guest.shutdown", ctx->vm_id ?: "",
                  _ok ? "ok" : "fail",
                  _ok ? 0 : PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
    if (!_ok) {
        gchar *err_resp = pure_rpc_build_error_response(ctx->rpc_id, PURE_RPC_ERR_ZFS_OPERATION, error->message);
        pure_uds_server_send_response(ctx->server, ctx->connection, err_resp);
        g_free(err_resp);
        g_error_free(error);
    } else {
        JsonObject *res_obj = json_object_new();
        json_object_set_string_member(res_obj, "status", "shutdown_initiated");
        json_object_set_string_member(res_obj, "name", ctx->vm_id);
        json_object_set_string_member(res_obj, "method", ctx->action ? ctx->action : "unknown");
        JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(res_node, res_obj);
        gchar *resp = pure_rpc_build_success_response(ctx->rpc_id, res_node);
        pure_uds_server_send_response(ctx->server, ctx->connection, resp);
        g_free(resp);
    }
}

   
                                                                              
                                                             
                                                       
                                                                     
   
void
handle_vm_guest_shutdown_request(JsonObject *params, const gchar *rpc_id,
                                 UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm_name = pcv_disp_guest_get_vm_name(params);
    if (!vm_name) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                         "Invalid params: 'name' missing");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    VmLifecycleCtx *ctx = g_new0(VmLifecycleCtx, 1);
    ctx->vm_id = g_strdup(vm_name);
    ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = g_object_ref(server);
    ctx->connection = g_object_ref(connection);

    GTask *task = pcv_drain_task_new(NULL, NULL, _guest_shutdown_callback, ctx);
    g_task_set_task_data(task, ctx, pcv_disp_free_lifecycle_ctx);
    g_task_run_in_thread(task, _guest_shutdown_worker);
    g_object_unref(task);
}
