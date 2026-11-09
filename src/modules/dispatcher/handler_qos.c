   
                      
                                 
  
                           
                                                                           
                                                                      
                                                                
                                                   
  
                                                   
   

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
#include "modules/dispatcher/handler_qos.h"
#include "api/dispatcher_internal.h"


                                                                     
                                                                       
  
                                                              
                                                            
                                                 
                                                             
                                                       
                                                              
                                                             
                                                        
                                             
                                      
  
                                                                                                  
                                                          
                                                       
                                                       
                                                                                
                                        
  
                                                                  
                                                        
                              
                                                                        

   
                                                              
  
                                                           
                                                            
  
                                                        
                                                          
                                               
                                                
                                             
                      
                                                            
                             
   
void
handle_qos_vm_set(JsonObject *params, const gchar *rpc_id,
                    UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm = pcv_disp_json_string_member(params, "vm");
    if (!vm || !*vm || !pcv_validate_vm_name(vm)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing/invalid required parameter: vm");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    PcvQosSla sla;
    GError *verr = NULL;
    if (!pcv_qos_sla_from_json(params, &sla, &verr)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            verr ? verr->message : "invalid qos_min_mbps/qos_max_mbps");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        g_clear_error(&verr);
        return;
    }

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            "Failed to connect to libvirt");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    extern virDomainPtr pure_virt_get_domain(virConnectPtr conn, const gchar *identifier);
    virDomainPtr dom = pure_virt_get_domain(conn, vm);
    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_VM_NOT_FOUND, "VM not found");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        virt_conn_pool_release(conn);
        return;
    }

    GError *werr = NULL;
    if (!pcv_vm_qos_metadata_write(dom, &sla, &werr)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            werr ? werr->message : "qos metadata write failed");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        g_clear_error(&werr);
        pcv_audit_log(pcv_rpc_completion_actor(), "qos.vm.set", vm, "fail", PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        return;
    }

                                                                  
    gboolean live_applied = FALSE;
    if (virDomainIsActive(dom) == 1) {
        gchar *tenant = NULL, *iface = NULL;
        if (pcv_qos_lookup_applied(vm, &tenant, &iface)) {
            PcvQosSla apply_sla = sla;
            g_strlcpy(apply_sla.tenant, tenant, sizeof(apply_sla.tenant));
            g_strlcpy(apply_sla.vm, vm, sizeof(apply_sla.vm));
            GError *aerr = NULL;
            if (pcv_qos_apply_vm(iface, &apply_sla, &aerr)) {
                live_applied = TRUE;
            } else {
                g_warning("[qos.vm.set] live 재적용 실패(vm=%s): %s",
                          vm, aerr ? aerr->message : "unknown");
                g_clear_error(&aerr);
            }
            g_free(tenant);
            g_free(iface);
        }
    }

    virDomainFree(dom);
    virt_conn_pool_release(conn);

    pcv_audit_log(pcv_rpc_completion_actor(), "qos.vm.set", vm, "ok", 0, 0, "local");

    GError *save_err = NULL;
    if (!pcv_qos_ids_save(PCV_QOS_IDS_PATH, &save_err)) {
        g_warning("[qos.vm.set] qos_ids.json 저장 실패(WARN — 결정성에 영향 가능, vm=%s): %s",
                  vm, save_err ? save_err->message : "unknown");
        g_clear_error(&save_err);
    }

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "vm", vm);
    json_object_set_int_member(res, "qos_min_mbps", sla.min_mbps);
    json_object_set_int_member(res, "qos_max_mbps", sla.max_mbps);
    json_object_set_int_member(res, "qos_burst_kb", sla.burst_kb);
    json_object_set_boolean_member(res, "live_applied", live_applied);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

   
                                                                
                                
                                                          
                                          
   
void
handle_qos_vm_get(JsonObject *params, const gchar *rpc_id,
                    UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm = pcv_disp_json_string_member(params, "vm");
    if (!vm || !*vm || !pcv_validate_vm_name(vm)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing/invalid required parameter: vm");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            "Failed to connect to libvirt");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    extern virDomainPtr pure_virt_get_domain(virConnectPtr conn, const gchar *identifier);
    virDomainPtr dom = pure_virt_get_domain(conn, vm);
    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_VM_NOT_FOUND, "VM not found");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        virt_conn_pool_release(conn);
        return;
    }

    PcvQosSla sla;
    PcvQosMetaResult mr = pcv_vm_qos_metadata_read(dom, &sla);
    virDomainFree(dom);
    virt_conn_pool_release(conn);

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "vm", vm);
    switch (mr) {
    case PCV_QOS_META_OK:
        json_object_set_string_member(res, "status", "ok");
        json_object_set_int_member(res, "qos_min_mbps", sla.min_mbps);
        json_object_set_int_member(res, "qos_max_mbps", sla.max_mbps);
        json_object_set_int_member(res, "qos_burst_kb", sla.burst_kb);
        break;
    case PCV_QOS_META_ABSENT:
        json_object_set_string_member(res, "status", "absent");
        break;
    case PCV_QOS_META_INVALID:
    default:
        json_object_set_string_member(res, "status", "invalid");
        break;
    }
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

   
                                                                          
                                                                  
                                                          
                                                                 
  
                                                   
                                                  
                                                      
                                                        
                                                          
                                
   
void
handle_qos_tenant_set(JsonObject *params, const gchar *rpc_id,
                        UdsServer *server, GSocketConnection *connection)
{
    const gchar *tenant = pcv_disp_json_string_member(params, "tenant");
    if (!tenant || !*tenant || !pcv_validate_vm_name(tenant)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing/invalid required parameter: tenant");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }
    gint min_mbps = json_object_has_member(params, "min_mbps")
        ? (gint)json_object_get_int_member(params, "min_mbps") : 0;
    gint max_mbps = json_object_has_member(params, "max_mbps")
        ? (gint)json_object_get_int_member(params, "max_mbps") : 0;
    if (min_mbps < 0 || max_mbps < 0) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "min_mbps/max_mbps must be >= 0");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    GError *error = NULL;
    gboolean live_applied = FALSE;
    gchar *live_error = NULL;
    if (!pcv_qos_tenant_sla_set(tenant, (guint32)min_mbps, (guint32)max_mbps,
                                &live_applied, &live_error, &error)) {
                                                                  
                                                                      
                                                          
                                                           
                                                                  
                                                     
        gint code = (error && error->domain == G_IO_ERROR
                     && error->code == G_IO_ERROR_INVALID_ARGUMENT)
            ? PURE_RPC_ERR_INVALID_PARAMS : PURE_RPC_ERR_ZFS_OPERATION;
        gchar *err = pure_rpc_build_error_response(rpc_id, code,
            error ? error->message : "qos.tenant.set failed");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        g_clear_error(&error);
        g_free(live_error);                                            
        pcv_audit_log(pcv_rpc_completion_actor(), "qos.tenant.set", tenant, "fail", code, 0, "local");
        return;
    }

                                                         
                                                          
                                                 
               
    pcv_audit_log(pcv_rpc_completion_actor(), "qos.tenant.set", tenant, "ok", 0, 0, "local");

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "tenant", tenant);
    json_object_set_int_member(res, "min_mbps", min_mbps);
    json_object_set_int_member(res, "max_mbps", max_mbps);
    json_object_set_boolean_member(res, "live_applied", live_applied);
    if (live_error) {
        json_object_set_string_member(res, "live_error", live_error);
        g_free(live_error);
    }
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

                                     
                                                            
void
handle_qos_tenant_get(JsonObject *params, const gchar *rpc_id,
                        UdsServer *server, GSocketConnection *connection)
{
    const gchar *tenant = pcv_disp_json_string_member(params, "tenant");
    if (!tenant || !*tenant || !pcv_validate_vm_name(tenant)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing/invalid required parameter: tenant");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    PcvQosTenantSla sla;
    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "tenant", tenant);
    if (pcv_qos_tenant_sla_get(tenant, &sla)) {
        json_object_set_string_member(res, "status", "ok");
        json_object_set_int_member(res, "min_mbps", sla.min_mbps);
        json_object_set_int_member(res, "max_mbps", sla.max_mbps);
    } else {
        json_object_set_string_member(res, "status", "absent");
    }
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

   
                                                                   
                                                            
                                                             
                           
   
void
handle_qos_stats(JsonObject *params, const gchar *rpc_id,
                   UdsServer *server, GSocketConnection *connection)
{
    const gchar *tenant_filter = pcv_disp_json_string_member(params, "tenant");
    const gchar *vm_filter     = pcv_disp_json_string_member(params, "vm");

    GPtrArray *stats = pcv_qos_collect_stats();

    JsonArray *arr = json_array_new();
    for (guint i = 0; i < stats->len; i++) {
        PcvQosClassStat *s = g_ptr_array_index(stats, i);
        if (tenant_filter && g_strcmp0(s->tenant, tenant_filter) != 0) continue;
        if (vm_filter && g_strcmp0(s->vm, vm_filter) != 0) continue;

        JsonObject *entry = json_object_new();
        json_object_set_string_member(entry, "classid", s->classid);
        json_object_set_string_member(entry, "tenant", s->tenant ? s->tenant : "");
        json_object_set_string_member(entry, "vm", s->vm ? s->vm : "");
        json_object_set_int_member(entry, "bytes", (gint64)s->bytes);
        json_object_set_int_member(entry, "drops", (gint64)s->drops);
        json_array_add_object_element(arr, entry);
    }
    g_ptr_array_unref(stats);

    JsonObject *res = json_object_new();
    json_object_set_array_member(res, "classes", arr);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

                                                                     
                                                           
  
                                                        
                                                              
                                                            
  
                                                         
                                                          
                                                   
                                                     
                    
  
                                                           
                                                                                 
                                                               
                                            
                                                      
                                               
                                                       

                                                          
                                                                     
                                    
void
handle_qos_chaos_start(JsonObject *params, const gchar *rpc_id,
                         UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm = pcv_disp_json_string_member(params, "vm");
    if (!vm || !*vm || !pcv_validate_vm_name(vm)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing/invalid required parameter: vm");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }
    const gchar *profile = pcv_disp_json_string_member(params, "profile");
    if (!profile || !*profile) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing required parameter: profile");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }
    if (!json_object_has_member(params, "timebox_sec")) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing required parameter: timebox_sec");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }
    gint timebox_i = (gint)json_object_get_int_member(params, "timebox_sec");
    if (timebox_i < 0) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "timebox_sec must be >= 0");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }
    guint timebox_sec = (guint)timebox_i;
    gboolean dry_run = pcv_qos_chaos_resolve_dry_run(params);
    const gchar *admin = pcv_disp_dispatcher_caller_subject(params, connection);

    GError *error = NULL;
    if (!pcv_qos_chaos_start(vm, profile, timebox_sec, admin, dry_run, &error)) {
                                                          
                                                
                                                   
        gint code = (error && g_strstr_len(error->message, -1, "not QoS-tracked"))
            ? PURE_RPC_ERR_VM_NOT_FOUND : PURE_RPC_ERR_ZFS_OPERATION;
        gchar *err = pure_rpc_build_error_response(rpc_id, code,
            error ? error->message : "qos.chaos.start failed");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        g_clear_error(&error);
        return;
    }

                                                                 
                                                        
                                                
    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "vm", vm);
    json_object_set_boolean_member(res, "dry_run", dry_run);
    json_object_set_string_member(res, "profile", profile);
    json_object_set_int_member(res, "timebox_sec", (gint64)timebox_sec);
    json_object_set_string_member(res, "note",
        "chaos targets the VM leaf on " PCV_QOS_IFB_DEV " (cake<->netem swap) — "
        "affects upstream (VM-originated) traffic only");

    gchar *tenant = NULL, *iface = NULL, *cid = NULL;
    if (pcv_qos_lookup_applied(vm, &tenant, &iface)) {
        cid = pcv_qos_classid(tenant, vm);
        json_object_set_string_member(res, "classid", cid);
    }
    g_free(tenant);
    g_free(iface);

    if (dry_run) {
        GError *perr = NULL;
        gchar **tokens = pcv_qos_chaos_profile_validate(profile, &perr);
        if (tokens) {
            JsonArray *argv_arr = json_array_new();
            const gchar *prefix[] = {"tc", "qdisc", "replace", "dev", PCV_QOS_IFB_DEV, "parent"};
            for (guint i = 0; i < G_N_ELEMENTS(prefix); i++)
                json_array_add_string_element(argv_arr, prefix[i]);
            json_array_add_string_element(argv_arr, cid ? cid : "?");
            json_array_add_string_element(argv_arr, "netem");
            for (guint i = 0; tokens[i]; i++)
                json_array_add_string_element(argv_arr, tokens[i]);
            json_object_set_array_member(res, "argv", argv_arr);
            g_strfreev(tokens);
        }
        g_clear_error(&perr);
    } else {
        json_object_set_int_member(res, "expires_at",
            (gint64)(g_get_real_time() / G_USEC_PER_SEC + timebox_sec));
    }
    g_free(cid);

    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

                        
                                                           
void
handle_qos_chaos_stop(JsonObject *params, const gchar *rpc_id,
                        UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm = pcv_disp_json_string_member(params, "vm");
    if (!vm || !*vm || !pcv_validate_vm_name(vm)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing/invalid required parameter: vm");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    const gchar *admin = pcv_disp_dispatcher_caller_subject(params, connection);
    GError *error = NULL;
    if (!pcv_qos_chaos_stop(vm, admin, &error)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            error ? error->message : "qos.chaos.stop failed");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        g_clear_error(&error);
        return;
    }

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "vm", vm);
    json_object_set_boolean_member(res, "stopped", TRUE);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

                                                         
                                                                      
void
handle_qos_chaos_status(JsonObject *params, const gchar *rpc_id,
                          UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    GPtrArray *entries = pcv_qos_chaos_status();

    JsonArray *arr = json_array_new();
    for (guint i = 0; i < entries->len; i++) {
        PcvQosChaosStatusEntry *s = g_ptr_array_index(entries, i);
        JsonObject *o = json_object_new();
        json_object_set_string_member(o, "vm", s->vm);
        json_object_set_string_member(o, "profile", s->profile);
        json_object_set_string_member(o, "admin", s->admin);
        json_object_set_int_member(o, "timebox_sec", s->timebox_sec);
        json_object_set_int_member(o, "expires_at", s->expires_at);
        json_array_add_object_element(arr, o);
    }
    g_ptr_array_unref(entries);

    JsonObject *res = json_object_new();
    json_object_set_array_member(res, "active", arr);
    json_object_set_string_member(res, "note",
        "chaos targets the VM leaf on " PCV_QOS_IFB_DEV " (cake<->netem swap) — "
        "affects upstream (VM-originated) traffic only");
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}
