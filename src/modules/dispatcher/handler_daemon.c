   
                         
                                                                                                         
  
                           
                                                                           
                                                                      
                                                                
                                                   
  
                                                                  
   

#include "api/dispatcher.h"
#include "api/uds_server.h"
#include "api/snapshot_verify_probe.h"
#include "api/ova_import_xml.h"
#include "api/vm_batch_policy.h"
#include "api/vm_batch_queue.h"
#include "api/daemon_config_policy.h"
#include "bootstrap/pcv_bootstrap.h"
#include "modules/virt/vm_manager.h"
#include "modules/virt/cancellable_map.h"
#include "modules/virt/vm_clone_plan.h"
#include "modules/daemons/prometheus_exporter.h"
#include "modules/daemons/pcv_webpush.h"
#include "modules/audit/pcv_audit.h"
#include "modules/plugin/pcv_plugin_manager.h"
#include "modules/core/cpu_allocator.h"
#include "utils/pcv_secure.h"
#include "purecvisor/version.h"
#include <json-glib/json-glib.h>
#include <glib.h>
#include <glib/gstdio.h>
#include "modules/dispatcher/handler_snapshot.h"
#include "modules/dispatcher/rpc_utils.h"
#include "modules/dispatcher/rpc_completion.h"
#include "modules/dispatcher/handler_vnc.h"
#include "modules/dispatcher/handler_vm_start.h"
#include "modules/dispatcher/handler_vm_lifecycle.h"
#include "modules/dispatcher/handler_vm_inspect.h"
#include "modules/dispatcher/handler_vm_hotplug.h"
#include "modules/network/network_manager.h"
#include "modules/network/vpc/vpc_manager.h"
#include "modules/network/pcv_qos.h"
#include "modules/network/pcv_qos_chaos.h"
#include "modules/daemons/pcv_trace.h"
#include "modules/security/pcv_suricata.h"
#include "modules/security/pcv_suricata_rules.h"
#include "modules/security/pcv_suricata_ips.h"
#include "modules/security/pcv_suricata_ips_rules.h"
#include "modules/dispatcher/handler_storage.h"
#include "modules/dispatcher/handler_monitor.h"
#include "modules/dispatcher/handler_container.h"
#include "modules/dispatcher/handler_overlay.h"
#include "modules/dispatcher/handler_accel.h"
#include "modules/accel/gpu_manager.h"
#include "modules/dispatcher/handler_template.h"
#include "modules/dispatcher/handler_auth.h"
#include "modules/dispatcher/handler_backup.h"
#include "modules/dispatcher/handler_security.h"
#include "modules/dispatcher/handler_tenant_overlay.h"
#include "modules/dispatcher/handler_vpc.h"
#include "modules/daemons/alert_engine.h"
#include "modules/daemons/process_monitor.h"
#include "modules/daemons/update_check.h"
#include "modules/virt/virt_conn_pool.h"
#include "utils/pcv_spawn.h"
#include "utils/pcv_config.h"
#include "utils/pcv_jwt.h"
#include "modules/auth/pcv_rbac.h"
#include "api/drain.h"
#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <sys/stat.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "modules/lxc/lxc_driver.h"
#include "modules/storage/zfs_driver.h"
#include "modules/backup/backup_scheduler.h"
#include "modules/ai/workload_predict.h"
#include "utils/pcv_worker_pool.h"
#include "utils/pcv_log.h"
#include "utils/pcv_validate.h"
#include "utils/pcv_job_queue.h"
#include "modules/core/vm_state.h"
#include "api/ws_server.h"
#include <sqlite3.h>
#include <errno.h>
#include "api/dispatcher_policy.h"
#include "api/dispatcher_internal.h"
#include "modules/dispatcher/dispatcher_shared.h"
#include "api/dispatcher_owner_scope.h"
#include "modules/dispatcher/handler_vm_ova.h"
#include "modules/dispatcher/handler_vm_clone.h"
#include "modules/dispatcher/handler_vm_create.h"
#include "modules/dispatcher/handler_vm_admin.h"
#include "modules/dispatcher/handler_suricata.h"
#include "modules/dispatcher/handler_alert.h"
#include "modules/dispatcher/handler_daemon.h"
#include "api/dispatcher_internal.h"


                                     
                                                   
void handle_config_reload(JsonObject *params, const gchar *rpc_id,
                                   UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    PcvAlertConfigSetResult reload_result =
        pcv_alert_engine_reload_daemon_config();
    if (reload_result != PCV_ALERT_CONFIG_SET_OK) {
        gchar *r = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid alert daemon config");
        pure_uds_server_send_response(server, connection, r);
        g_free(r);
        return;
    }
    JsonNode *n = json_node_new(JSON_NODE_NULL);
    gchar *r = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, r); g_free(r);
    g_message("[CONFIG] Configuration reloaded via RPC");
}

                                               
                                                            
                         
void handle_health_deep(JsonObject *params, const gchar *rpc_id,
                                 UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    JsonObject *res = json_object_new();

                     
    const gchar *zpool_argv[] = {"zpool", "status", "-x", NULL};
    gchar *zpool_out = nullptr;
    gboolean zpool_ok = pcv_spawn_sync(zpool_argv, &zpool_out, NULL, NULL);
    json_object_set_string_member(res, "zfs_pool",
        (zpool_ok && zpool_out && strstr(zpool_out, "all pools are healthy")) ? "ok" : "degraded");
    g_free(zpool_out);

                       
    const gchar *nft_argv[] = {"nft", "list", "ruleset", NULL};
    gchar *nft_out = nullptr;
    pcv_spawn_sync(nft_argv, &nft_out, NULL, NULL);
    gint rule_count = 0;
    if (nft_out) {
        gchar **lines = g_strsplit(nft_out, "\n", -1);
        for (int i = 0; lines[i]; i++) rule_count++;
        g_strfreev(lines);
    }
    json_object_set_int_member(res, "nftables_rules", rule_count);
    g_free(nft_out);

    json_object_set_string_member(res, "status", "ok");
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res);
    gchar *r = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, r); g_free(r);
}

                                       
                                                           
void handle_db_migration_status(JsonObject *params, const gchar *rpc_id,
                                         UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    JsonObject *res = json_object_new();
    json_object_set_int_member(res, "schema_version", 1);
    json_object_set_string_member(res, "status", "up_to_date");
    json_object_set_string_member(res, "rbac_db", "/var/lib/purecvisor/rbac.db");
    json_object_set_string_member(res, "audit_db", "/var/lib/purecvisor/pcv_audit.db");
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res);
    gchar *r = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, r); g_free(r);
}

                                                                       
                                                                     
void handle_daemon_update_check(JsonObject *params, const gchar *rpc_id,
                                        UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    PcvUpdateStatus st = pcv_update_check_get();
    JsonObject *obj = json_object_new();
    json_object_set_boolean_member(obj, "enabled", st.enabled);
    json_object_set_string_member(obj, "current", st.current);
    json_object_set_string_member(obj, "latest", st.latest);
    json_object_set_boolean_member(obj, "update_available", st.update_available);
    json_object_set_string_member(obj, "url", st.url);
    json_object_set_int_member(obj, "checked_at", st.checked_at);
    json_object_set_string_member(obj, "state", st.state);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, obj);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}

                                                                        
                                          
  
                                                        
                                                                    
  
              
                                          
                                
                                
                                  
  
                                   
                                                                                    
                                                                      
                                                                          
                                                                            
                                                                                 
                                                                    
  
                                                                                  
                                                                     
                                                                     
                                                                                
                                                             
                                                                        
                                                                          
void handle_node_drain(JsonObject *params, const gchar *rpc_id,
                                UdsServer *server, GSocketConnection *connection)
{
    guint timeout_sec = 30;
    if (!pcv_rpc_params_get_uint_in_range(params, "timeout_sec", 1, 86400, 30, &timeout_sec)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                         "timeout_sec must be an integer between 1 and 86400");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }
    pcv_drain_begin(NULL, timeout_sec);
    JsonNode *ok_node = json_node_new(JSON_NODE_VALUE);
    json_node_set_boolean(ok_node, TRUE);
    gchar *resp = pure_rpc_build_success_response(rpc_id, ok_node);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}

                                                                      
                                                              
void handle_node_resume(JsonObject *params, const gchar *rpc_id,
                                 UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    if (pcv_drain_is_terminating()) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
                         "Process shutdown cannot be resumed");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

    pcv_drain_cancel();
    JsonNode *ok_node = json_node_new(JSON_NODE_VALUE);
    json_node_set_boolean(ok_node, TRUE);
    gchar *resp = pure_rpc_build_success_response(rpc_id, ok_node);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}


                                                                         
                     
  
                   
                                                           
                                           
  
                  
                                                 
  
                     
                                                             
  
                 
                                          
                         
                                                                            
                                                                      
                                                                   
void handle_config_history(JsonObject *params, const gchar *rpc_id,
                                    UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    JsonArray *arr = json_array_new();
    GDir *dir = g_dir_open("/var/lib/purecvisor/", 0, NULL);
    if (dir) {
        const gchar *name;
        while ((name = g_dir_read_name(dir))) {
            if (g_str_has_prefix(name, "daemon.conf.")) {
                JsonObject *entry = json_object_new();
                json_object_set_string_member(entry, "file", name);
                gchar *path = g_strdup_printf("/var/lib/purecvisor/%s", name);
                struct stat st;
                if (stat(path, &st) == 0)
                    json_object_set_int_member(entry, "mtime", (gint64)st.st_mtime);
                g_free(path);
                json_array_add_object_element(arr, entry);
            }
        }
        g_dir_close(dir);
    }
    JsonNode *node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(node, arr);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}

                                                                      
                                                          
void handle_config_backup(JsonObject *params, const gchar *rpc_id,
                                   UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    gchar *ts = g_strdup_printf("%ld", (long)time(NULL));
    gchar *dst = g_strdup_printf("/var/lib/purecvisor/daemon.conf.%s", ts);
    const gchar *cp_argv[] = {"cp", "/etc/purecvisor/daemon.conf", dst, NULL};
    pcv_spawn_sync(cp_argv, NULL, NULL, NULL);
    JsonNode *node = json_node_new(JSON_NODE_VALUE);
    json_node_set_string(node, dst);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp); g_free(dst); g_free(ts);
}


                                                              
                                                        
void handle_daemon_config_get(JsonObject *params, const gchar *rpc_id,
                                       UdsServer *server, GSocketConnection *connection)
{
    const gchar *section = (params && json_object_has_member(params, "section"))
        ? json_object_get_string_member(params, "section") : NULL;

    JsonObject *result = json_object_new();

    if (!section || g_strcmp0(section, "storage") == 0) {
        JsonObject *stg = json_object_new();
        json_object_set_string_member(stg, "zvol_pool",       pcv_config_get_zvol_pool());
        json_object_set_string_member(stg, "container_pool",  pcv_config_get_container_pool());
        json_object_set_string_member(stg, "image_dir",       pcv_config_get_image_dir());
        json_object_set_string_member(stg, "iso_dirs",        pcv_config_get_iso_dirs());
        json_object_set_object_member(result, "storage", stg);
    }
    if (!section || g_strcmp0(section, "container") == 0) {
        JsonObject *ctr = json_object_new();
        json_object_set_string_member(ctr, "lxc_path",        pcv_config_get_container_path());
        json_object_set_string_member(ctr, "rootless", pcv_config_get_string("container", "rootless", PCV_LXC_ROOTLESS_DEFAULT));
        json_object_set_object_member(result, "container", ctr);
    }

    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, result);
    gchar *resp = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}

                                                           
                                                         
                                                        
void handle_daemon_config_set(JsonObject *params, const gchar *rpc_id,
                                       UdsServer *server, GSocketConnection *connection)
{
    const gchar *section = (params && json_object_has_member(params, "section"))
        ? json_object_get_string_member(params, "section") : NULL;
    const gchar *key = (params && json_object_has_member(params, "key"))
        ? json_object_get_string_member(params, "key") : NULL;
    const gchar *value = (params && json_object_has_member(params, "value"))
        ? json_object_get_string_member(params, "value") : NULL;
    gboolean secret_value =
        key && g_strcmp0(key, "webhook_secret") == 0;

    if (!section || !key || !value) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Required: section, key, value");
        pure_uds_server_send_response(server, connection, err); g_free(err);
        if (secret_value)
            pcv_disp_wipe_json_string_member(params, "value");
        return;
    }

    if (g_strcmp0(section, "storage") != 0 &&
        g_strcmp0(section, "container") != 0 &&
        g_strcmp0(section, "alert") != 0 &&
        g_strcmp0(section, "backup") != 0) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Section not editable (allowed: storage, container, alert, backup)");
        pure_uds_server_send_response(server, connection, err); g_free(err);
        if (secret_value)
            pcv_disp_wipe_json_string_member(params, "value");
        return;
    }

    const gchar *conf_path = pcv_config_get_file_path();
    GError *error = nullptr;
    PcvAlertConfigSetResult set_result =
        pcv_daemon_config_set_value(
            conf_path, section, key, value, &error);

    if (error) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, error->message);
        pure_uds_server_send_response(server, connection, err);
        g_free(err); g_error_free(error);
        if (secret_value)
            pcv_disp_wipe_json_string_member(params, "value");
        return;
    }

    if (set_result != PCV_ALERT_CONFIG_SET_OK) {
        gchar *err = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid alert daemon config");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        if (secret_value)
            pcv_disp_wipe_json_string_member(params, "value");
        return;
    }

    JsonObject *ok = json_object_new();
    json_object_set_boolean_member(ok, "success", TRUE);
    json_object_set_string_member(ok, "section", section);
    json_object_set_string_member(ok, "key", key);
    if (secret_value) {
        json_object_set_boolean_member(ok, "value_configured",
                                       value[0] != '\0');
        pcv_disp_wipe_json_string_member(params, "value");
    } else {
        json_object_set_string_member(ok, "value", value);
    }
    JsonNode *nn = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(nn, ok);
    gchar *resp = pure_rpc_build_success_response(rpc_id, nn);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}

                                                                     
                                                                 
  
                                                         
                                                         
                                                                     
  
                                                               
                                                        
                                              
                                                       
                                          
  
                                                                   
                                                
                                                                 
                                                     
                                                   
                                                               
                                            
  
                                                                          
                                                     
                                                                        

                                                                
                                                         
                                                                                    
static gint
_trace_err_code(const GError *e)
{
    if (!e || e->domain != G_IO_ERROR) return PURE_RPC_ERR_ZFS_OPERATION;
    switch (e->code) {
        case G_IO_ERROR_INVALID_ARGUMENT: return PURE_RPC_ERR_INVALID_PARAMS;              
        case G_IO_ERROR_NOT_FOUND:        return PURE_RPC_ERR_NOT_FOUND;                   
        case G_IO_ERROR_EXISTS:                                                         
        case G_IO_ERROR_BUSY:             return PURE_RPC_ERR_BUSY;                    
        case G_IO_ERROR_TIMED_OUT:        return PURE_RPC_ERR_TIMEOUT;                     
        default:                          return PURE_RPC_ERR_ZFS_OPERATION;                      
    }
}

                                                                                
                                                                           
                                                              
                                            
                                                          
                                                             
void
handle_debug_trace_start(JsonObject *params, const gchar *rpc_id,
                          UdsServer *server, GSocketConnection *connection)
{
    if (!json_object_has_member(params, "timebox_sec")) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing required parameter: timebox_sec");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    PcvTraceFilter f = {0};
    const gchar *s;
    if ((s = pcv_disp_json_string_member(params, "vm")))     g_strlcpy(f.vm,     s, sizeof(f.vm));
    if ((s = pcv_disp_json_string_member(params, "tenant"))) g_strlcpy(f.tenant, s, sizeof(f.tenant));
    if ((s = pcv_disp_json_string_member(params, "proto")))  g_strlcpy(f.proto,  s, sizeof(f.proto));
    if ((s = pcv_disp_json_string_member(params, "src_ip"))) g_strlcpy(f.src_ip, s, sizeof(f.src_ip));
    if ((s = pcv_disp_json_string_member(params, "dst_ip"))) g_strlcpy(f.dst_ip, s, sizeof(f.dst_ip));

    gint iv;
    if (pcv_disp_json_int_member(params, "src_port", &iv))    f.src_port    = (guint)iv;
    if (pcv_disp_json_int_member(params, "dst_port", &iv))    f.dst_port    = (guint)iv;
    if (pcv_disp_json_int_member(params, "timebox_sec", &iv)) f.timebox_sec = (guint)iv;

    if (json_object_has_member(params, "overlay"))
        f.overlay = json_object_get_boolean_member(params, "overlay");

    const gchar *admin = pcv_disp_dispatcher_caller_subject(params, connection);
    GError *error = NULL;
    gchar *trace_id = pcv_trace_start(&f, admin, &error);
    if (!trace_id) {
        gchar *err = pure_rpc_build_error_response(rpc_id, _trace_err_code(error),
            error ? error->message : "debug.trace.start failed");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        g_clear_error(&error);
        return;
    }

    gchar *out_dir = g_build_filename(PCV_TRACE_OUT_ROOT, trace_id, NULL);
    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "trace_id", trace_id);
    json_object_set_string_member(res, "out_dir", out_dir);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
    g_free(out_dir);
    g_free(trace_id);
}

                                
                                                                   
void
handle_debug_trace_stop(JsonObject *params, const gchar *rpc_id,
                         UdsServer *server, GSocketConnection *connection)
{
    const gchar *trace_id = pcv_disp_json_string_member(params, "trace_id");
    if (!trace_id || !*trace_id) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing required parameter: trace_id");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    GError *error = NULL;
    if (!pcv_trace_stop(trace_id, &error)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, _trace_err_code(error),
            error ? error->message : "debug.trace.stop failed");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        g_clear_error(&error);
        return;
    }

    JsonObject *res = json_object_new();
                                                               
    json_object_set_boolean_member(res, "stopped", TRUE);
    json_object_set_boolean_member(res, "stop_requested", TRUE);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

                                                         
                                                      
void
handle_debug_trace_status(JsonObject *params, const gchar *rpc_id,
                           UdsServer *server, GSocketConnection *connection)
{
    const gchar *trace_id = pcv_disp_json_string_member(params, "trace_id");               
    JsonObject *res = pcv_trace_status(trace_id);                   
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

                                      
                                                  
void
handle_debug_trace_list(JsonObject *params, const gchar *rpc_id,
                         UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    JsonArray *traces = pcv_trace_list();
    JsonObject *res = json_object_new();
    json_object_set_array_member(res, "traces", traces);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

                                                       
  
                                                         
                                                           
                           
  
                                              
                                                         
                                                        
                                                 
                                                                 
                                                       
                                          
                                              
                                                       
                                                   
                                            
                                                            
                                                   
                                                       
                                        
void
handle_debug_trace_report(JsonObject *params, const gchar *rpc_id,
                           UdsServer *server, GSocketConnection *connection)
{
    const gchar *trace_id = pcv_disp_json_string_member(params, "trace_id");
    if (!trace_id || !*trace_id) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing required parameter: trace_id");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    GError *error = NULL;
    JsonObject *res = pcv_trace_report(trace_id, &error);
    if (!res) {
        gchar *err = pure_rpc_build_error_response(rpc_id, _trace_err_code(error),
            error ? error->message : "debug.trace.report failed");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        g_clear_error(&error);
        return;
    }

    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}
