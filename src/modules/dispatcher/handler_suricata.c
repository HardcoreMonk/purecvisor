   
                           
                                                                 
  
                           
                                                                           
                                                                      
                                                                
                                                   
  
                                                                                   
   

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
#include "api/dispatcher_internal.h"


                                                                     
                                                                     
  
                                                          
                                                      
                                                         
                        
  
                                                             
                                                                
                                                                                   
  
                                                                   
                                                              
                                                          
                                                          
                                                           
                                                            
                                    
  
                                                                 
                                                                    
                                                             
                                                                  
                                                                   
                                                           
                                                             
                                                                        

                                                         
                                                       
                                                       
                                                   
                                                    
static gboolean
_json_opt_bool_member(JsonObject *params, const gchar *key, gboolean *out)
{
    if (!params || !key || !json_object_has_member(params, key))
        return FALSE;
    JsonNode *node = json_object_get_member(params, key);
    if (!node || !JSON_NODE_HOLDS_VALUE(node) ||
        json_node_get_value_type(node) != G_TYPE_BOOLEAN)
        return FALSE;
    if (out) *out = json_node_get_boolean(node);
    return TRUE;
}

                                                             
                                                                  
                                                             
                                                                          
                                                            
                                                     
                                                             
                                                                     
                                                                 
                                                    
static struct {
    GMutex   mu;
    gboolean running;                                                    
    gchar   *last_op;                                                                          
    gchar   *last_result;                            
    gchar   *last_error;                         
    gint64   last_ts;                       
} g_suricata_ips_job;

                                                          
                                                            
                                                         
static struct {
    GMutex   mu;
    gboolean running;                         
    gchar   *last_url;                      
    gchar   *last_result;                                  
    gchar   *last_error;                                   
    gint64   last_ts;                            
} g_suricata_rules_job;

typedef struct {
    gchar *url;
    gchar *admin;
} SuricataRulesUpdateCtx;

                                                                      
                                                   
static void
_suricata_rules_update_ctx_free(gpointer data)
{
    SuricataRulesUpdateCtx *ctx = data;
    if (!ctx) return;
    g_free(ctx->url);
    g_free(ctx->admin);
    g_free(ctx);
}

                                                                             
                                                          
                                                  
                                                                           
                                                
                                               
static void
_suricata_rules_update_worker(GTask *task, gpointer src, gpointer task_data,
                              GCancellable *cancellable)
{
    (void)task; (void)src; (void)cancellable;
    SuricataRulesUpdateCtx *ctx = task_data;

    GError *error = NULL;
                                                            
    gboolean ok = pcv_suricata_rules_update(ctx->url, ctx->admin, &error);

                                                            
                                               
      
                                                                  
                                                       
                                                           
                                                       
      
                                                  
                                                        
                                                       
                                                   
      
                                                                    
                                                   
                                                     
                                                   
                                                                  
                                                            
                                                  
      
                                                        
                                                    
                                                      
                                  
      
                                                             
                                                   
    if (ok && g_file_test(PCV_IPS_RULES_PATH, G_FILE_TEST_EXISTS)) {
        gboolean acquired = FALSE;
        gchar *saved_op = NULL;
        g_mutex_lock(&g_suricata_ips_job.mu);
        if (!g_suricata_ips_job.running) {
            g_suricata_ips_job.running = TRUE;
            saved_op = g_suricata_ips_job.last_op;                        
            g_suricata_ips_job.last_op = g_strdup("rules.regen");
            acquired = TRUE;
        }
        g_mutex_unlock(&g_suricata_ips_job.mu);

        if (!acquired) {
            PCV_LOG_WARN("suricata_ips",
                "원본 룰 갱신 후 IPS 파생본 재생성 생략 — 다른 IPS 작업 진행 중"
                "(파생본은 이전 원본 기준으로 남는다)");
        } else {
            GHashTable *sids = pcv_suricata_policy_drop_sids_snapshot();
            GError *rerr = NULL;
            gboolean regen = pcv_suricata_ips_rules_apply(sids, ctx->admin, &rerr);
            if (!regen) {
                PCV_LOG_WARN("suricata_ips",
                    "원본 룰 갱신 후 IPS 파생본 재생성 실패 — 파생본은 이전 원본 기준으로 "
                    "남는다(원본 갱신은 성공 유지): %s", rerr ? rerr->message : "unknown");
            }
            g_hash_table_destroy(sids);

            g_mutex_lock(&g_suricata_ips_job.mu);
            g_suricata_ips_job.running = FALSE;
            g_free(g_suricata_ips_job.last_result);
            g_suricata_ips_job.last_result = g_strdup(regen ? "ok" : "fail");
            g_free(g_suricata_ips_job.last_error);
            g_suricata_ips_job.last_error = regen ? NULL
                : g_strdup(rerr ? rerr->message : "unknown error");
            g_suricata_ips_job.last_ts = g_get_real_time() / G_USEC_PER_SEC;
            g_mutex_unlock(&g_suricata_ips_job.mu);

            g_clear_error(&rerr);
        }
        g_free(saved_op);                                         
    }

    g_mutex_lock(&g_suricata_rules_job.mu);
    g_suricata_rules_job.running = FALSE;
    g_free(g_suricata_rules_job.last_result);
    g_suricata_rules_job.last_result = g_strdup(ok ? "ok" : "fail");
    g_free(g_suricata_rules_job.last_error);
    g_suricata_rules_job.last_error = ok ? NULL
        : g_strdup(error ? error->message : "unknown error");
    g_suricata_rules_job.last_ts = g_get_real_time() / G_USEC_PER_SEC;
    g_mutex_unlock(&g_suricata_rules_job.mu);

    g_clear_error(&error);
}

                                                                             
                                                           
                                               
void
handle_suricata_policy_set(JsonObject *params, const gchar *rpc_id,
                            UdsServer *server, GSocketConnection *connection)
{
    const gchar *tenant       = pcv_disp_json_string_member(params, "tenant");                      
    const gchar *profile      = pcv_disp_json_string_member(params, "profile");                     
    const gchar *auto_isolate = pcv_disp_json_string_member(params, "auto_isolate");                
    gboolean inspect_val = FALSE;
    gboolean has_inspect = _json_opt_bool_member(params, "inspect", &inspect_val);

    const gchar *admin = pcv_disp_dispatcher_caller_subject(params, connection);

    GError *error = NULL;
    gboolean ok = pcv_suricata_policy_set(tenant, has_inspect ? &inspect_val : NULL,
                                          profile, auto_isolate, &error);

                                                                 
    pcv_audit_log(admin ? admin : "system", "suricata.policy.set",
                  tenant ? tenant : (auto_isolate ? "auto_isolate" : "?"),
                  ok ? "ok" : "fail", ok ? 0 : (error ? error->code : -1), 0, "local");

    if (!ok) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            error ? error->message : "suricata.policy.set failed");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        g_clear_error(&error);
        return;
    }

                                    
    JsonObject *res = pcv_suricata_policy_get_json(tenant);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

                                             
                                                     
void
handle_suricata_policy_get(JsonObject *params, const gchar *rpc_id,
                            UdsServer *server, GSocketConnection *connection)
{
    const gchar *tenant = pcv_disp_json_string_member(params, "tenant");                
    JsonObject *res = pcv_suricata_policy_get_json(tenant);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

                                                                     
                                                            
                                                                    
void
handle_suricata_rules_update(JsonObject *params, const gchar *rpc_id,
                              UdsServer *server, GSocketConnection *connection)
{
    const gchar *url = pcv_disp_json_string_member(params, "url");
    if (!url || !*url) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing required parameter: url");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }
                                                       
                             
    if (!g_str_has_prefix(url, "http://") && !g_str_has_prefix(url, "https://")) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "url must use http/https scheme");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

                                                    
    g_mutex_lock(&g_suricata_rules_job.mu);
    if (g_suricata_rules_job.running) {
        g_mutex_unlock(&g_suricata_rules_job.mu);
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
            "suricata rules update already in progress");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }
    g_suricata_rules_job.running = TRUE;
    g_free(g_suricata_rules_job.last_url);
    g_suricata_rules_job.last_url = g_strdup(url);
    g_mutex_unlock(&g_suricata_rules_job.mu);

    const gchar *admin = pcv_disp_dispatcher_caller_subject(params, connection);
    SuricataRulesUpdateCtx *ctx = g_new0(SuricataRulesUpdateCtx, 1);
    ctx->url = g_strdup(url);
    ctx->admin = g_strdup(admin ? admin : "system");

    GTask *task = pcv_drain_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(task, ctx, _suricata_rules_update_ctx_free);
    g_task_run_in_thread(task, _suricata_rules_update_worker);
    g_object_unref(task);

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "started");
    json_object_set_string_member(res, "url", url);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

                                                             
                                                        
                                                                   
void
handle_suricata_status(JsonObject *params, const gchar *rpc_id,
                        UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    JsonObject *res = json_object_new();

                                                              
    PcvSuricataState state = PCV_SURICATA_INACTIVE;
    gboolean binary_present = FALSE;
    pcv_suricata_engine_status_cached(&state, &binary_present);
    JsonObject *engine = json_object_new();
    json_object_set_string_member(engine, "state", pcv_suricata_state_str(state));
    json_object_set_boolean_member(engine, "binary_present", binary_present);
    json_object_set_object_member(res, "engine", engine);

                                                            
    guint64 crit = 0, warn = 0, info = 0, dropped = 0;
    pcv_suricata_get_stats(&crit, &warn, &info, &dropped);
    JsonObject *eve = json_object_new();
    json_object_set_boolean_member(eve, "running", pcv_suricata_eve_tail_running());
    json_object_set_int_member(eve, "alerts_crit", (gint64)crit);
    json_object_set_int_member(eve, "alerts_warn", (gint64)warn);
    json_object_set_int_member(eve, "alerts_info", (gint64)info);
    json_object_set_int_member(eve, "dropped", (gint64)dropped);
    json_object_set_object_member(res, "eve_tail", eve);

                                             
    JsonObject *rules = json_object_new();
    json_object_set_int_member(rules, "count", (gint64)pcv_suricata_rules_count());
    JsonObject *update = json_object_new();
    g_mutex_lock(&g_suricata_rules_job.mu);
    json_object_set_boolean_member(update, "running", g_suricata_rules_job.running);
    if (g_suricata_rules_job.last_result)
        json_object_set_string_member(update, "last_result", g_suricata_rules_job.last_result);
    else
        json_object_set_null_member(update, "last_result");
    if (g_suricata_rules_job.last_url)
        json_object_set_string_member(update, "last_url", g_suricata_rules_job.last_url);
    else
        json_object_set_null_member(update, "last_url");
    if (g_suricata_rules_job.last_error)
        json_object_set_string_member(update, "last_error", g_suricata_rules_job.last_error);
    else
        json_object_set_null_member(update, "last_error");
    json_object_set_int_member(update, "last_ts", g_suricata_rules_job.last_ts);
    g_mutex_unlock(&g_suricata_rules_job.mu);
    json_object_set_object_member(rules, "update", update);
    json_object_set_object_member(res, "rules", rules);

                                           
    gboolean enforce = FALSE;
    guint tenant_count = 0;
    pcv_suricata_policy_summary(&enforce, &tenant_count);
    JsonObject *policy = json_object_new();
    json_object_set_string_member(policy, "auto_isolate", enforce ? "enforce" : "dry_run");
    json_object_set_int_member(policy, "tenants", (gint64)tenant_count);
    json_object_set_object_member(res, "policy", policy);

    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

typedef struct {
    gboolean enable;
    guint    queue_num;
    gboolean fail_open;
    gchar   *admin;
} SuricataIpsToggleCtx;

                                                                        
                                                
static void
_suricata_ips_toggle_ctx_free(gpointer data)
{
    SuricataIpsToggleCtx *ctx = data;
    if (!ctx) return;
    g_free(ctx->admin);
    g_free(ctx);
}

                                                                
                                                            
                                                         
                                                              
                                                                 
                                                           
static void
_suricata_ips_toggle_worker(GTask *task, gpointer src, gpointer task_data,
                            GCancellable *cancellable)
{
    (void)task; (void)src; (void)cancellable;
    SuricataIpsToggleCtx *ctx = task_data;
    const gchar *op = ctx->enable ? "enable" : "disable";

    GError *error = NULL;
    gboolean ok = ctx->enable
        ? pcv_suricata_ips_enable(ctx->queue_num, ctx->fail_open, &error)
        : pcv_suricata_ips_disable(&error);

                                                                           
                                                                            
                                                                           
                                                             
                               
                                                                                            
    gchar method[32];
    g_snprintf(method, sizeof method, "suricata.ips.%s", op);
    pcv_audit_log(ctx->admin ? ctx->admin : "system", method, "suricata-ips",
                  ok ? "ok" : "fail", ok ? 0 : (error ? error->code : -1), 0, "local");

    g_mutex_lock(&g_suricata_ips_job.mu);
    g_suricata_ips_job.running = FALSE;
                                                                          
    g_free(g_suricata_ips_job.last_result); g_suricata_ips_job.last_result = g_strdup(ok ? "ok" : "fail");
    g_free(g_suricata_ips_job.last_error);  g_suricata_ips_job.last_error = ok ? NULL
        : g_strdup(error ? error->message : "unknown error");
    g_suricata_ips_job.last_ts = g_get_real_time() / G_USEC_PER_SEC;
    g_mutex_unlock(&g_suricata_ips_job.mu);

    g_clear_error(&error);
}

                                                                           
                                                              
                                        
                                                                             
static void
_suricata_ips_toggle_async(gboolean enable, JsonObject *params, const gchar *rpc_id,
                           UdsServer *server, GSocketConnection *connection)
{
    const gchar *op = enable ? "enable" : "disable";

    g_mutex_lock(&g_suricata_ips_job.mu);
    if (g_suricata_ips_job.running) {
        g_mutex_unlock(&g_suricata_ips_job.mu);
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
            "suricata ips toggle already in progress");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }
    g_suricata_ips_job.running = TRUE;
                                                                        
                                                                          
    g_free(g_suricata_ips_job.last_op);
    g_suricata_ips_job.last_op = g_strdup(op);
    g_mutex_unlock(&g_suricata_ips_job.mu);

    const gchar *admin = pcv_disp_dispatcher_caller_subject(params, connection);
    SuricataIpsToggleCtx *ctx = g_new0(SuricataIpsToggleCtx, 1);
    ctx->enable    = enable;
    ctx->queue_num = (guint)pcv_config_get_ips_queue_num();
    ctx->fail_open = pcv_config_get_ips_fail_open();
    ctx->admin     = g_strdup(admin ? admin : "system");

    GTask *task = pcv_drain_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(task, ctx, _suricata_ips_toggle_ctx_free);
    g_task_run_in_thread(task, _suricata_ips_toggle_worker);
    g_object_unref(task);

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "started");
    json_object_set_string_member(res, "op", op);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

                                                                              
                                                      
void
handle_suricata_ips_enable(JsonObject *params, const gchar *rpc_id,
                            UdsServer *server, GSocketConnection *connection)
{
    _suricata_ips_toggle_async(TRUE, params, rpc_id, server, connection);
}

                                                                                
                                                  
void
handle_suricata_ips_disable(JsonObject *params, const gchar *rpc_id,
                             UdsServer *server, GSocketConnection *connection)
{
    _suricata_ips_toggle_async(FALSE, params, rpc_id, server, connection);
}

                                                                     
                                      
                                                         
                                                                 
void
handle_suricata_ips_status(JsonObject *params, const gchar *rpc_id,
                            UdsServer *server, GSocketConnection *connection)
{
    (void)params; (void)connection;
    JsonObject *res = json_object_new();

    PcvSuricataState st = PCV_SURICATA_INACTIVE;
    gboolean binary_present = FALSE;
    pcv_suricata_ips_engine_status_cached(&st, &binary_present);
    JsonObject *engine = json_object_new();
    json_object_set_string_member(engine, "state", pcv_suricata_state_str(st));
    json_object_set_boolean_member(engine, "binary_present", binary_present);
    json_object_set_object_member(res, "engine", engine);

    json_object_set_boolean_member(res, "enabled", pcv_suricata_ips_is_enabled());
    json_object_set_int_member(res, "queue_num", (gint64)pcv_config_get_ips_queue_num());
    json_object_set_boolean_member(res, "fail_open", pcv_config_get_ips_fail_open());
    json_object_set_string_member(res, "mode", pcv_config_get_ips_mode());
                                                                          
                                                           
                                                               
                                                
    json_object_set_boolean_member(res, "degraded", pcv_suricata_ips_is_degraded());

                                           
    g_mutex_lock(&g_suricata_ips_job.mu);
    JsonObject *lt = json_object_new();
    json_object_set_boolean_member(lt, "running", g_suricata_ips_job.running);
    if (g_suricata_ips_job.last_op)
        json_object_set_string_member(lt, "op", g_suricata_ips_job.last_op);
    if (g_suricata_ips_job.last_result)
        json_object_set_string_member(lt, "result", g_suricata_ips_job.last_result);
    if (g_suricata_ips_job.last_error)
        json_object_set_string_member(lt, "error", g_suricata_ips_job.last_error);
    json_object_set_int_member(lt, "ts", g_suricata_ips_job.last_ts);
    g_mutex_unlock(&g_suricata_ips_job.mu);
    json_object_set_object_member(res, "last_toggle", lt);

    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

                                                                     
                                                              
  
                                                    
                                                         
                                         
  
                                                               
                                                
                                                     
                                                   
                                     
                                                  
                   
  
                                             
                                                   
                                                                       
                                                    
                                                       
  
                                                     
                                                         
                                        
                                                     
                                                           
                           
  
                                                        
                                                    
                                 
  
                                                              
                                                               
                                                
                                                     
                                      
                                                                       
                                                 
                                                                        

typedef struct {
    gboolean    add;                                                        
    GHashTable *desired;                       
    GHashTable *prev;                               
    gchar      *admin;
} SuricataIpsDropCtx;

                                                                 
                                                      
static void
_suricata_ips_drop_ctx_free(gpointer data)
{
    SuricataIpsDropCtx *ctx = data;
    if (!ctx) return;
    if (ctx->desired) g_hash_table_destroy(ctx->desired);
    if (ctx->prev)    g_hash_table_destroy(ctx->prev);
    g_free(ctx->admin);
    g_free(ctx);
}

                                                              
                                                   
                                                 
                                              
                                                                
                                                                 
static void
_suricata_ips_drop_worker(GTask *task, gpointer src, gpointer task_data,
                          GCancellable *cancellable)
{
    (void)task; (void)src; (void)cancellable;
    SuricataIpsDropCtx *ctx = task_data;
    const gchar *method = ctx->add ? "suricata.ips.drop.add" : "suricata.ips.drop.remove";

    GError *error = NULL;
                                                    
                             
    gboolean ok = pcv_suricata_ips_rules_apply(ctx->desired, ctx->admin, &error);

    if (ok) {
                                          
        GError *serr = NULL;
        if (!pcv_suricata_policy_drop_sids_commit(&serr)) {
            ok = FALSE;
            PCV_LOG_WARN("suricata_ips",
                "drop_sids 정책 저장 실패 — 파생 룰셋은 이미 갱신됨(다음 성공적 "
                "drop 변경 또는 rules.update 재생성에서 정합 복구): %s",
                serr ? serr->message : "unknown");
            if (!error) error = serr; else g_clear_error(&serr);
        }
    }
    if (!ok) {
                                                
        pcv_suricata_policy_drop_sids_rollback(ctx->prev);
    }

                                                         
                                                                   
                                             
                                                                           
    pcv_audit_log(ctx->admin ? ctx->admin : "system", method, "suricata-ips-drop",
                  ok ? "ok" : "fail", ok ? 0 : (error ? error->code : -1), 0, "local");

    g_mutex_lock(&g_suricata_ips_job.mu);
    g_suricata_ips_job.running = FALSE;
                                                                   
    g_free(g_suricata_ips_job.last_result);
    g_suricata_ips_job.last_result = g_strdup(ok ? "ok" : "fail");
    g_free(g_suricata_ips_job.last_error);
    g_suricata_ips_job.last_error = ok ? NULL
        : g_strdup(error ? error->message : "unknown error");
    g_suricata_ips_job.last_ts = g_get_real_time() / G_USEC_PER_SEC;
    g_mutex_unlock(&g_suricata_ips_job.mu);

    g_clear_error(&error);
}

                                                                          
                                                  
                                                      
                                 
                                                 
                                                        
static void
_suricata_ips_drop_mutate(gboolean add, JsonObject *params, const gchar *rpc_id,
                          UdsServer *server, GSocketConnection *connection)
{
    const gchar *op = add ? "drop.add" : "drop.remove";

                                   
    JsonArray *sids = NULL;
    if (params && json_object_has_member(params, "sids")) {
        JsonNode *n = json_object_get_member(params, "sids");
        if (n && JSON_NODE_HOLDS_ARRAY(n))
            sids = json_node_get_array(n);
    }
    if (!sids) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing required parameter: sids (array of positive integers)");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

                                                     
    g_mutex_lock(&g_suricata_ips_job.mu);
    if (g_suricata_ips_job.running) {
        g_mutex_unlock(&g_suricata_ips_job.mu);
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
            "suricata ips operation already in progress");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }
    g_suricata_ips_job.running = TRUE;
                                                               
                                                  
                                                    
                                                                 
                                                            
                                                         
    gchar *saved_op = g_suricata_ips_job.last_op;
    g_suricata_ips_job.last_op = g_strdup(op);
    g_mutex_unlock(&g_suricata_ips_job.mu);

                                                
    GHashTable *prev = pcv_suricata_policy_drop_sids_snapshot();
    GError *error = NULL;
    if (!pcv_suricata_policy_set_drop_sids_global(sids, add, &error)) {
        g_hash_table_destroy(prev);
                                                    
                                                               
                                      
        g_mutex_lock(&g_suricata_ips_job.mu);
        g_free(g_suricata_ips_job.last_op);
        g_suricata_ips_job.last_op = saved_op;               
        g_suricata_ips_job.running = FALSE;
        g_mutex_unlock(&g_suricata_ips_job.mu);

        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            error ? error->message : "invalid sids");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        g_clear_error(&error);
        return;
    }
    g_free(saved_op);                                 

    const gchar *admin = pcv_disp_dispatcher_caller_subject(params, connection);
    SuricataIpsDropCtx *ctx = g_new0(SuricataIpsDropCtx, 1);
    ctx->add     = add;
    ctx->prev    = prev;
    ctx->desired = pcv_suricata_policy_drop_sids_snapshot();                   
    ctx->admin   = g_strdup(admin ? admin : "system");

    GTask *task = pcv_drain_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(task, ctx, _suricata_ips_drop_ctx_free);
    g_task_run_in_thread(task, _suricata_ips_drop_worker);
    g_object_unref(task);

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "started");
    json_object_set_string_member(res, "op", op);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

                                                      
                                                        
void
handle_suricata_ips_drop_list(JsonObject *params, const gchar *rpc_id,
                               UdsServer *server, GSocketConnection *connection)
{
    (void)params;
                                                 
                                                                
    JsonObject *full = pcv_suricata_policy_get_json(NULL);
    JsonNode   *arr  = json_object_dup_member(full, "drop_sids");
    json_object_unref(full);

    JsonObject *res = json_object_new();
    if (arr)
        json_object_set_member(res, "sids", arr);
    else
        json_object_set_array_member(res, "sids", json_array_new());           

    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

                                                                            
                                                       
void
handle_suricata_ips_drop_add(JsonObject *params, const gchar *rpc_id,
                              UdsServer *server, GSocketConnection *connection)
{
    _suricata_ips_drop_mutate(TRUE, params, rpc_id, server, connection);
}

                                                      
                                                      
void
handle_suricata_ips_drop_remove(JsonObject *params, const gchar *rpc_id,
                                 UdsServer *server, GSocketConnection *connection)
{
    _suricata_ips_drop_mutate(FALSE, params, rpc_id, server, connection);
}
