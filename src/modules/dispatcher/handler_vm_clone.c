   
                           
                                           
  
                           
                                                                           
                                                                      
                                                                
                                                   
  
                                                                 
   

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
#include "utils/pcv_zfs_exec.h"
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
#include "modules/dispatcher/dispatcher_shared.h"
#include "api/dispatcher_owner_scope.h"
#include "modules/dispatcher/handler_vm_ova.h"
#include "modules/dispatcher/handler_vm_clone.h"


                                                             
                                             
  
          
                                                            
                                                 
                                                     
                                                            
                                                       
                                                      
                                                 
                                    
                                
  
         
                                                              
                                                             
                                                                            
                                                                    
                 
  
                                                   
                                                                      
typedef struct {
    gchar    *source;                           
    gchar    *target;                           
    gboolean  full_copy;                                                 
    gboolean  guest_reset;                                              
    PcvVmCloneDiskKind disk_kind;                      
    gchar    *source_disk_path;                                  
    gchar    *target_disk_path;                            
    gchar    *source_dataset;                                        
    gchar    *target_dataset;                                   
    gchar    *zfs_pool;                                                
    gchar    *source_zvol_name;                               
    PcvVmLock *source_lock;                                                 
    PcvVmLock *target_lock;                                                             
    GThread  *lock_renew_thread;                                             
    gint      lock_renew_stop;                                          
} VmCloneCtx;

                                                                         
                                                             
                                                               
                           
#define VM_CLONE_LOCK_RENEW_TICK_MS   250
#define VM_CLONE_LOCK_RENEW_TICKS     240                          

                                                                
                                                                 
                                                                      
                                                 
                                                     
                                                  
                                  
                                                                        
                                                                                                   
static gpointer
_vm_clone_lock_renew_worker(gpointer data)
{
    VmCloneCtx *ctx = (VmCloneCtx *)data;
    while (!g_atomic_int_get(&ctx->lock_renew_stop)) {                     
        for (gint i = 0; i < VM_CLONE_LOCK_RENEW_TICKS; i++) {                           
            if (g_atomic_int_get(&ctx->lock_renew_stop)) return NULL;                    
            g_usleep(VM_CLONE_LOCK_RENEW_TICK_MS * 1000);
        }
        if (g_atomic_int_get(&ctx->lock_renew_stop)) break;
        if (ctx->source_lock) (void)pcv_vm_lock_renew(ctx->source_lock);                 
        if (ctx->target_lock) (void)pcv_vm_lock_renew(ctx->target_lock);
    }
    return NULL;
}

                                                            
                                                      
                                                  
                                  
                                                                      
static void
_vm_clone_ctx_free(gpointer data)
{
    VmCloneCtx *ctx = (VmCloneCtx *)data;
    if (!ctx) return;
                                                        
                                                      
    if (ctx->lock_renew_thread) {
        g_atomic_int_set(&ctx->lock_renew_stop, 1);
        g_thread_join(ctx->lock_renew_thread);
        ctx->lock_renew_thread = NULL;
    }
                                                                    
                                                               
    pcv_vm_lock_release(ctx->source_lock);
    pcv_vm_lock_release(ctx->target_lock);
    g_free(ctx->source);
    g_free(ctx->target);
    g_free(ctx->source_disk_path);
    g_free(ctx->target_disk_path);
    g_free(ctx->source_dataset);
    g_free(ctx->target_dataset);
    g_free(ctx->zfs_pool);
    g_free(ctx->source_zvol_name);
    g_free(ctx);
}

                                                                    
                                                      
                                                      
                                                                                   
static gboolean
_vm_clone_template_prepared_ack(JsonObject *params)
{
    if (!params)
        return FALSE;

    if (json_object_has_member(params, "template_prepared")) {
        JsonNode *node = json_object_get_member(params, "template_prepared");
        if (node && JSON_NODE_HOLDS_VALUE(node) &&
            json_node_get_value_type(node) == G_TYPE_BOOLEAN &&
            json_node_get_boolean(node))
            return TRUE;
    }

    const gchar *ack = pcv_disp_json_string_member(params, "clone_safety_ack");
    return g_strcmp0(ack, "template-prepared") == 0;
}

                                                               
                                                     
                                                                       
static gboolean
_vm_clone_bool_member(JsonObject *params, const gchar *name, gboolean fallback)
{
    if (!params || !name || !json_object_has_member(params, name))
        return fallback;

    JsonNode *node = json_object_get_member(params, name);
    if (!node || !JSON_NODE_HOLDS_VALUE(node) ||
        json_node_get_value_type(node) != G_TYPE_BOOLEAN)
        return fallback;

    return json_node_get_boolean(node);
}

                                                            
                                                                        
static gchar *
_vm_clone_job_target(VmCloneCtx *ctx)
{
    return g_strdup_printf("%s:%s", ctx->source, ctx->target);
}

                                                              
                                                                              
static gchar *
_vm_clone_job_id(VmCloneCtx *ctx)
{
    gchar *target = _vm_clone_job_target(ctx);
    gchar *job_id = g_strdup_printf("vm.clone:%s", target);
    g_free(target);
    return job_id;
}

                                                               
                                                               
static void
_audit_vm_clone_success(VmCloneCtx *ctx, const gchar *actor)
{
    gchar *target = _vm_clone_job_target(ctx);
    gchar *job_id = _vm_clone_job_id(ctx);
    pcv_audit_log(actor, "vm.clone", target, "ok", 0, 0, "local");
    pcv_ws_broadcast_job_complete_mt(job_id, "vm.clone", "completed", NULL);
    g_free(job_id);
    g_free(target);
}

                                                               
                                             
                                               
static void
_audit_vm_clone_failure(VmCloneCtx *ctx, const gchar *error_msg, const gchar *actor)
{
    gchar *target = _vm_clone_job_target(ctx);
    gchar *job_id = _vm_clone_job_id(ctx);
    pcv_audit_log(actor, "vm.clone", target, "fail", PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
    pcv_ws_broadcast_job_complete_mt(job_id, "vm.clone",
                                     "failed", error_msg ? error_msg : "unknown");
    g_free(job_id);
    g_free(target);
}

                                                                     
                                                                      
static gchar *
_vm_clone_source_snapshot(VmCloneCtx *ctx, const gchar *snap_tag)
{
    if (!ctx || !ctx->source_dataset || !snap_tag)
        return NULL;
    return g_strdup_printf("%s@%s", ctx->source_dataset, snap_tag);
}

                                                                        
                                                               
                                                                         
static gboolean
_vm_clone_destroy_dataset_recursive(const gchar *dataset)
{
    if (!dataset || !*dataset)
        return TRUE;

    GError *cleanup_err = NULL;
    gchar *cleanup_stderr = NULL;
    gboolean ok = pcv_zfs_destroy_locked("-R", dataset, &cleanup_stderr, &cleanup_err);
    if (!ok) {
        if (cleanup_stderr)
            g_strstrip(cleanup_stderr);
        PCV_LOG_WARN("vm_clone", "Target clone dataset cleanup failed for '%s': %s (zfs stderr: %s)",
                     dataset,
                     cleanup_err ? cleanup_err->message : "unknown",
                     (cleanup_stderr && *cleanup_stderr) ? cleanup_stderr : "(none)");
    }
    g_free(cleanup_stderr);
    g_clear_error(&cleanup_err);
    return ok;
}

                                                               
                                                 
                                                
static gboolean
_vm_clone_destroy_source_snapshot(VmCloneCtx *ctx, const gchar *snap_tag)
{
    gchar *snap_full = _vm_clone_source_snapshot(ctx, snap_tag);
    if (!snap_full)
        return TRUE;

    GError *cleanup_err = NULL;
    gchar *cleanup_stderr = NULL;
    gboolean ok = pcv_zfs_destroy_locked(NULL, snap_full, &cleanup_stderr, &cleanup_err);
    if (!ok) {
        if (cleanup_stderr)
            g_strstrip(cleanup_stderr);
        PCV_LOG_WARN("vm_clone", "Source clone snapshot cleanup failed for '%s': %s (zfs stderr: %s)",
                     snap_full,
                     cleanup_err ? cleanup_err->message : "unknown",
                     (cleanup_stderr && *cleanup_stderr) ? cleanup_stderr : "(none)");
    }
    g_free(cleanup_stderr);
    g_clear_error(&cleanup_err);
    g_free(snap_full);
    return ok;
}

                                                                     
                                                        
                                                
static gboolean
_vm_clone_remove_target_file(const gchar *path)
{
    if (!path || !*path)
        return TRUE;

    if (!g_file_test(path, G_FILE_TEST_EXISTS))
        return TRUE;

    if (g_remove(path) == 0)
        return TRUE;

    PCV_LOG_WARN("vm_clone", "Target clone file cleanup failed for '%s': %s",
                 path, g_strerror(errno));
    return FALSE;
}

                                                                        
                                                             
                                                        
static void
_vm_clone_cleanup_failed_artifacts(VmCloneCtx *ctx, const gchar *snap_tag)
{
    if (!ctx)
        return;

    if (ctx->disk_kind == PCV_VM_CLONE_DISK_ZVOL)
        (void)_vm_clone_destroy_dataset_recursive(ctx->target_dataset);
    else
        (void)_vm_clone_remove_target_file(ctx->target_disk_path);
    (void)_vm_clone_destroy_source_snapshot(ctx, snap_tag);
}

                                                                    
                                                               
                                                                     
static PcvVmCloneDiskPlan
_vm_clone_disk_plan_from_ctx(VmCloneCtx *ctx)
{
    PcvVmCloneDiskPlan plan = {
        .kind = ctx->disk_kind,
        .source_disk_path = ctx->source_disk_path,
        .target_disk_path = ctx->target_disk_path,
        .source_dataset = ctx->source_dataset,
        .target_dataset = ctx->target_dataset,
        .zfs_pool = ctx->zfs_pool,
        .source_zvol_name = ctx->source_zvol_name,
    };
    return plan;
}

   
                        
                                                    
                                                    
                                                   
                                                            
   
static void
_generate_random_mac(gchar *out_mac, gsize out_len)
{
    guint8 bytes[3];
    for (int i = 0; i < 3; i++)
        bytes[i] = (guint8)g_random_int_range(0, 256);
    g_snprintf(out_mac, out_len, "52:54:00:%02x:%02x:%02x",
               bytes[0], bytes[1], bytes[2]);
}

                                                                
                                                      
                                         
                                                                  
static gboolean
_xml_replace_mac_eval(const GMatchInfo *match_info, GString *result, gpointer user_data)
{
    (void)user_data;

    gchar *old_mac_tag = g_match_info_fetch(match_info, 0);
    if (!old_mac_tag)
        return FALSE;

    gchar new_mac[18];
    _generate_random_mac(new_mac, sizeof(new_mac));
    gboolean self_closing = g_str_has_suffix(old_mac_tag, "/>");
    g_string_append_printf(result,
                           self_closing ? "<mac address='%s'/>"
                                        : "<mac address='%s'>",
                           new_mac);
    g_free(old_mac_tag);
    return FALSE;
}

   
                         
                                     
  
                                                        
                                             
                                                               
  
          
                                             
                                                     
                                          
   
static gchar *
_xml_replace_all_macs(gchar *xml)
{
    GRegex *mac_re = g_regex_new("<mac address='[0-9a-fA-F:]+'/?>",
                                  G_REGEX_CASELESS, 0, NULL);
    if (!mac_re) return xml;

    GError *error = NULL;
    gchar *result = g_regex_replace_eval(mac_re,
                                         xml,
                                         -1,
                                         0,
                                         0,
                                         _xml_replace_mac_eval,
                                         NULL,
                                         &error);
    g_regex_unref(mac_re);
    if (!result) {
        PCV_LOG_WARN("vm_clone", "MAC rewrite failed: %s",
                     error ? error->message : "unknown");
        if (error)
            g_error_free(error);
        return xml;
    }

    g_free(xml);
    return result;
}

                                                                            
  
                                                         
                                                              
                                                             
                                                     
                                 
  
                                                 
                                                                                          
                                                                           
                                                                                         
static void
_vm_clone_thread(GTask *task, gpointer source_obj,
                  gpointer task_data, GCancellable *cancellable)
{
    (void)source_obj; (void)cancellable;
    VmCloneCtx *ctx = (VmCloneCtx *)task_data;
    GError *err = nullptr;

    gboolean is_zvol = (ctx->disk_kind == PCV_VM_CLONE_DISK_ZVOL);
    gboolean is_file_disk = (ctx->disk_kind == PCV_VM_CLONE_DISK_QCOW2 ||
                             ctx->disk_kind == PCV_VM_CLONE_DISK_RAW);
    if ((is_zvol && (!ctx->source_dataset || !ctx->target_dataset ||
                     !ctx->zfs_pool || !ctx->source_zvol_name)) ||
        (is_file_disk && (!ctx->source_disk_path || !ctx->target_disk_path)) ||
        (!is_zvol && !is_file_disk)) {
        const gchar *err_msg = "vm.clone internal error: missing clone disk plan";
        PCV_LOG_WARN("vm_clone", "%s", err_msg);
        _audit_vm_clone_failure(ctx, err_msg, pcv_drain_task_actor(task));
        g_task_return_boolean(task, FALSE);
        return;
    }

    PCV_LOG_INFO("vm_clone", "Starting clone '%s' -> '%s' (mode=%s type=%s guest_reset=%s source=%s target=%s)",
                 ctx->source, ctx->target, ctx->full_copy ? "full" : "cow",
                 pcv_vm_clone_disk_kind_to_string(ctx->disk_kind),
                 ctx->guest_reset ? "yes" : "no",
                 is_zvol ? ctx->source_dataset : ctx->source_disk_path,
                 is_zvol ? ctx->target_dataset : ctx->target_disk_path);

                                                                            
                                                                
                                                                
                                           
    ctx->lock_renew_thread = g_thread_new("clone-lock-renew",
                                          _vm_clone_lock_renew_worker, ctx);

    gchar *snap_tag = NULL;
    gboolean source_snapshot_exists = FALSE;

    if (is_zvol) {
                           
        snap_tag = g_strdup_printf("clone-%s", ctx->target);
        {
            gchar *snap_full = _vm_clone_source_snapshot(ctx, snap_tag);
            const gchar *argv[] = {"zfs", "snapshot", snap_full, NULL};
            gboolean ok = pcv_spawn_sync(argv, NULL, NULL, &err);
            g_free(snap_full);
            if (!ok) {
                const gchar *err_msg = err ? err->message : "unknown";
                PCV_LOG_WARN("vm_clone", "ZFS snapshot failed for '%s': %s",
                              ctx->source, err_msg);
                _audit_vm_clone_failure(ctx, err_msg, pcv_drain_task_actor(task));
                if (err) g_error_free(err);
                g_free(snap_tag);
                g_task_return_boolean(task, FALSE);
                return;
            }
            source_snapshot_exists = TRUE;
        }

                                         
        {
            gboolean ok;
            if (ctx->full_copy) {
                ok = purecvisor_zfs_full_copy(ctx->zfs_pool, ctx->source_zvol_name,
                                               snap_tag, ctx->target, &err);
            } else {
                ok = purecvisor_zfs_clone_volume(ctx->zfs_pool, ctx->source_zvol_name,
                                                  snap_tag, ctx->target, &err);
            }
            if (!ok) {
                const gchar *err_msg = err ? err->message : "unknown";
                PCV_LOG_WARN("vm_clone", "ZFS %s failed for '%s': %s",
                              ctx->full_copy ? "full copy" : "clone",
                              ctx->target, err_msg);
                _audit_vm_clone_failure(ctx, err_msg, pcv_drain_task_actor(task));
                _vm_clone_cleanup_failed_artifacts(ctx,
                                                   source_snapshot_exists ? snap_tag : NULL);
                if (err) g_error_free(err);
                g_free(snap_tag);
                g_task_return_boolean(task, FALSE);
                return;
            }
        }

                                                                              
                                                                          
                                                                       
                                                                        
                                                         
                                                       
                                                                                   
        {
            const gchar *udevadm_argv[] = {"udevadm", "settle", "--timeout=5", NULL};
            (void)pcv_spawn_sync(udevadm_argv, NULL, NULL, NULL);

                                                                     
                                                                      
            const int node_wait_max_ms  = 10000;
            const int node_wait_step_ms = 100;
            int node_waited_ms = 0;
            while (!g_file_test(ctx->target_disk_path, G_FILE_TEST_EXISTS)) {
                if (node_waited_ms >= node_wait_max_ms) {
                    gchar *err_msg = g_strdup_printf(
                        "zvol device node '%s' did not appear within %ds after clone",
                        ctx->target_disk_path, node_wait_max_ms / 1000);
                    PCV_LOG_WARN("vm_clone", "%s", err_msg);
                    _audit_vm_clone_failure(ctx, err_msg, pcv_drain_task_actor(task));
                    _vm_clone_cleanup_failed_artifacts(ctx,
                                                       source_snapshot_exists ? snap_tag : NULL);
                    g_free(err_msg);
                    g_free(snap_tag);
                    g_task_return_boolean(task, FALSE);
                    return;
                }
                g_usleep((gulong)node_wait_step_ms * 1000);
                node_waited_ms += node_wait_step_ms;
            }
        }

        if (ctx->full_copy) {
                                                                     
                                                                      
            if (_vm_clone_destroy_source_snapshot(ctx, snap_tag))
                source_snapshot_exists = FALSE;
        }
    } else {
        PcvVmCloneDiskPlan file_plan = _vm_clone_disk_plan_from_ctx(ctx);
        if (!pcv_vm_clone_copy_file_disk(&file_plan, &err)) {
            const gchar *err_msg = err ? err->message : "unknown";
            PCV_LOG_WARN("vm_clone", "File disk clone failed for '%s': %s",
                         ctx->target, err_msg);
            _audit_vm_clone_failure(ctx, err_msg, pcv_drain_task_actor(task));
            _vm_clone_cleanup_failed_artifacts(ctx, NULL);
            if (err) g_error_free(err);
            g_task_return_boolean(task, FALSE);
            return;
        }
    }

    if (ctx->guest_reset) {
        PcvVmCloneDiskPlan reset_plan = _vm_clone_disk_plan_from_ctx(ctx);
        if (!pcv_vm_clone_reset_guest_identity(&reset_plan, ctx->target, &err)) {
            const gchar *err_msg = err ? err->message : "unknown";
            PCV_LOG_WARN("vm_clone", "Guest identity reset failed for '%s': %s",
                         ctx->target, err_msg);
            _audit_vm_clone_failure(ctx, err_msg, pcv_drain_task_actor(task));
            _vm_clone_cleanup_failed_artifacts(ctx,
                                               source_snapshot_exists ? snap_tag : NULL);
            if (err) g_error_free(err);
            g_free(snap_tag);
            g_task_return_boolean(task, FALSE);
            return;
        }
    }

                                      
    gchar *xml = nullptr;
    {
        virConnectPtr conn = virt_conn_pool_acquire();
        if (!conn) {
            PCV_LOG_WARN("vm_clone", "Failed to acquire libvirt connection");
            _audit_vm_clone_failure(ctx, "Failed to acquire libvirt connection", pcv_drain_task_actor(task));
            _vm_clone_cleanup_failed_artifacts(ctx,
                                               source_snapshot_exists ? snap_tag : NULL);
            g_free(snap_tag);
            g_task_return_boolean(task, FALSE);
            return;
        }
        virDomainPtr dom = virDomainLookupByName(conn, ctx->source);
        if (!dom) {
            PCV_LOG_WARN("vm_clone", "Source VM '%s' not found in libvirt",
                          ctx->source);
            _audit_vm_clone_failure(ctx, "Source VM not found in libvirt", pcv_drain_task_actor(task));
            _vm_clone_cleanup_failed_artifacts(ctx,
                                               source_snapshot_exists ? snap_tag : NULL);
            virt_conn_pool_release(conn);
            g_free(snap_tag);
            g_task_return_boolean(task, FALSE);
            return;
        }
        xml = virDomainGetXMLDesc(dom, VIR_DOMAIN_XML_INACTIVE);
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        if (!xml || !*xml) {
            PCV_LOG_WARN("vm_clone", "Empty XML for source VM '%s'",
                          ctx->source);
            _audit_vm_clone_failure(ctx, "Empty XML for source VM", pcv_drain_task_actor(task));
            _vm_clone_cleanup_failed_artifacts(ctx,
                                               source_snapshot_exists ? snap_tag : NULL);
            free(xml);
            g_free(snap_tag);
            g_task_return_boolean(task, FALSE);
            return;
        }
    }

                    
      
                                                     
                                                                    
                                                            
             
                                                 
                                               
                                                    
                                                 
                                                   
    {
        gchar *old_tag = g_strdup_printf("<name>%s</name>", ctx->source);
        gchar *new_tag = g_strdup_printf("<name>%s</name>", ctx->target);
        if (g_strstr_len(xml, -1, old_tag)) {
            gchar **parts = g_strsplit(xml, old_tag, 2);
            gchar *replaced = g_strjoinv(new_tag, parts);
            g_strfreev(parts);
            free(xml);                                          
            xml = replaced;                                        
        } else {
                                                                   
                                                     
            gchar *dup = g_strdup(xml);
            free(xml);                         
            xml = dup;                  
        }
        g_free(old_tag);
        g_free(new_tag);
    }

                                                
    {
        GRegex *uuid_re = g_regex_new("\\s*<uuid>[^<]*</uuid>\\s*\n?",
                                        0, 0, NULL);
        if (uuid_re) {
            gchar *no_uuid = g_regex_replace(uuid_re, xml, -1, 0, "", 0, NULL);
            g_regex_unref(uuid_re);
            if (no_uuid) { g_free(xml); xml = no_uuid; }
        }
    }

                                       
    xml = _xml_replace_all_macs(xml);

                     
      
                                                    
                                                           
                                                               
    if (ctx->source_disk_path && ctx->target_disk_path &&
        g_strstr_len(xml, -1, ctx->source_disk_path)) {
        gchar **parts = g_strsplit(xml, ctx->source_disk_path, -1);
        gchar *tmp = g_strjoinv(ctx->target_disk_path, parts);
        g_strfreev(parts);
        g_free(xml);
        xml = tmp;
    } else {
        const gchar *err_msg = "Source disk path not found in current VM XML";
        PCV_LOG_WARN("vm_clone", "%s for '%s'", err_msg, ctx->source);
        _audit_vm_clone_failure(ctx, err_msg, pcv_drain_task_actor(task));
        _vm_clone_cleanup_failed_artifacts(ctx,
                                           source_snapshot_exists ? snap_tag : NULL);
        g_free(xml);
        g_free(snap_tag);
        g_task_return_boolean(task, FALSE);
        return;
    }

                                         
    {
        virConnectPtr conn = virt_conn_pool_acquire();
        if (!conn) {
            PCV_LOG_WARN("vm_clone", "Failed to acquire libvirt connection");
            _audit_vm_clone_failure(ctx, "Failed to acquire libvirt connection", pcv_drain_task_actor(task));
            _vm_clone_cleanup_failed_artifacts(ctx,
                                               source_snapshot_exists ? snap_tag : NULL);
            g_free(xml);
            g_free(snap_tag);
            g_task_return_boolean(task, FALSE);
            return;
        }

        virDomainPtr dom = virDomainDefineXML(conn, xml);
        if (dom) {
            PCV_LOG_INFO("vm_clone", "Defined cloned VM '%s' from '%s'",
                          ctx->target, ctx->source);
            virDomainFree(dom);
            virt_conn_pool_release(conn);
        } else {
            virErrorPtr vir_err = virGetLastError();
            const gchar *err_msg = vir_err ? vir_err->message : "unknown";
            PCV_LOG_WARN("vm_clone", "virDomainDefineXML failed for '%s': %s",
                          ctx->target, err_msg);
            _audit_vm_clone_failure(ctx, err_msg, pcv_drain_task_actor(task));
            _vm_clone_cleanup_failed_artifacts(ctx,
                                               source_snapshot_exists ? snap_tag : NULL);
            virt_conn_pool_release(conn);
            g_free(xml);
            g_free(snap_tag);
            g_task_return_boolean(task, FALSE);
            return;
        }
    }

                         

    if (ctx->full_copy && source_snapshot_exists) {
                                                              
        if (_vm_clone_destroy_source_snapshot(ctx, snap_tag))
            source_snapshot_exists = FALSE;
    }

                                             
    _audit_vm_clone_success(ctx, pcv_drain_task_actor(task));

    g_free(xml);
    g_free(snap_tag);
    g_task_return_boolean(task, TRUE);
}

                                                                        
                                          
  
                                                           
                                                             
                                                        
                                                           
                                                                        
                                                                   
  
                                                 
                                                
                                       
                                                   
                                                 
  
                                 
                                       
                                                           
                                                        
  
            
                                                               
                                                           
                                                                         
void handle_vm_clone(JsonObject *params, const gchar *rpc_id,
                              UdsServer *server, GSocketConnection *connection)
{
    const gchar *source = json_object_has_member(params, "source")
        ? json_object_get_string_member(params, "source")
        : (json_object_has_member(params, "name")
           ? json_object_get_string_member(params, "name")
           : (json_object_has_member(params, "vm_id")
              ? json_object_get_string_member(params, "vm_id") : NULL));
    const gchar *clone_name = json_object_has_member(params, "clone_name")
        ? json_object_get_string_member(params, "clone_name")
        : (json_object_has_member(params, "target")
           ? json_object_get_string_member(params, "target")
           : (json_object_has_member(params, "new_name")
              ? json_object_get_string_member(params, "new_name") : NULL));
    const gchar *mode = json_object_has_member(params, "mode")
        ? json_object_get_string_member(params, "mode") : NULL;

    if (!source || !clone_name) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing required parameters: source and clone_name (or target)");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }

               
    if (!pcv_validate_vm_name(source) || !pcv_validate_vm_name(clone_name)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid VM name — must be [a-zA-Z0-9_-], 1-63 chars");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }
                                                                                    
    if (!pcv_validate_new_vm_name(clone_name)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "VM name must not look like a UUID");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }
    if (g_strcmp0(source, clone_name) == 0) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "source and clone_name must be different");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }
    if (pcv_vpc_vm_is_attached(source)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_CONFLICT,
            "VPC-attached VMs must detach before vm.clone to avoid copying managed NIC identity");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }

    if (mode && g_strcmp0(mode, "cow") != 0 && g_strcmp0(mode, "full") != 0) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid clone mode: use cow or full");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }

    gint caller_role = pcv_disp_dispatcher_caller_role(params, connection);
    if (caller_role < PCV_ROLE_OPERATOR) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_FORBIDDEN,
            "vm.clone requires operator role or higher");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }

    gboolean template_prepared = _vm_clone_template_prepared_ack(params);
    gboolean guest_reset = !template_prepared;
    guest_reset = _vm_clone_bool_member(params, "guest_reset", guest_reset);
    guest_reset = _vm_clone_bool_member(params, "guest_identity_reset", guest_reset);

    if (!template_prepared && !guest_reset) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
            "vm.clone requires either template_prepared=true or guest_reset=true");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }

    if (guest_reset && !pcv_vm_clone_guest_reset_available()) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_CONFLICT,
            "vm.clone guest reset requires libguestfs-tools (virt-sysprep, virt-customize, virt-filesystems, guestfish) or template_prepared=true for a prepared template");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }

    PcvVmCloneDiskInfo disk_info = {0};
    PcvVmCloneDiskPlan disk_plan = {0};
    gchar *preflight_error = NULL;
    VmCloneCtx *clone_ctx = g_new0(VmCloneCtx, 1);
    clone_ctx->source = g_strdup(source);
    clone_ctx->target = g_strdup(clone_name);
    clone_ctx->guest_reset = guest_reset;

                                                    
                                                                    
                                                                        
                                                                  
                                                           
                                                             
                                                                        
    {
        gchar *lock_err = NULL;
        clone_ctx->source_lock = pcv_vm_lock_acquire(source, VM_OP_SNAPSHOT, &lock_err);
        if (!clone_ctx->source_lock) {
            gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
                lock_err ? lock_err : "Source VM busy (another operation in progress)");
            pure_uds_server_send_response(server, connection, e);
            g_free(e); g_free(lock_err);
            _vm_clone_ctx_free(clone_ctx);                                  
            return;
        }
        clone_ctx->target_lock = pcv_vm_lock_acquire(clone_name, VM_OP_CREATING, &lock_err);
        if (!clone_ctx->target_lock) {
            gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
                lock_err ? lock_err : "Target VM name busy (another operation in progress)");
            pure_uds_server_send_response(server, connection, e);
            g_free(e); g_free(lock_err);
            _vm_clone_ctx_free(clone_ctx);                                       
            return;
        }
    }

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        _vm_clone_ctx_free(clone_ctx);
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_CONFLICT,
            "Failed to acquire libvirt connection");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }

    virDomainPtr target_dom = virDomainLookupByName(conn, clone_name);
    if (target_dom) {
        virDomainFree(target_dom);
        virt_conn_pool_release(conn);
        _vm_clone_ctx_free(clone_ctx);
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
            "Target VM already exists");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }
    virResetLastError();

    virDomainPtr dom = virDomainLookupByName(conn, source);
    if (!dom) {
        virt_conn_pool_release(conn);
        _vm_clone_ctx_free(clone_ctx);
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_NOT_FOUND,
            "Source VM not found");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }

                                                                       
                                                                   
                                                             
                           
    {
        gchar *dpdk_bridge = NULL;
        PcvDpdkMetaResult dpdk_meta = pcv_vm_dpdk_metadata_read(dom, &dpdk_bridge);
        g_free(dpdk_bridge);
        if (dpdk_meta == PCV_DPDK_META_OK || dpdk_meta == PCV_DPDK_META_INVALID) {
            virDomainFree(dom);
            virt_conn_pool_release(conn);
            _vm_clone_ctx_free(clone_ctx);
            gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_CONFLICT,
                dpdk_meta == PCV_DPDK_META_OK
                    ? "vm.clone is not supported for managed DPDK VMs; create a new DPDK VM instead"
                    : "Source VM DPDK metadata is unreadable; vm.clone blocked");
            pure_uds_server_send_response(server, connection, e); g_free(e);
            return;
        }
    }

    int source_active_state = virDomainIsActive(dom);
    if (source_active_state < 0) {
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        _vm_clone_ctx_free(clone_ctx);
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_CONFLICT,
            "vm.clone could not verify source VM power state");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }
                                                      
                                                              
    if (source_active_state == 1) {
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        _vm_clone_ctx_free(clone_ctx);
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
            "vm.clone requires the source VM to be shut off");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }

    char *xml = virDomainGetXMLDesc(dom, VIR_DOMAIN_XML_INACTIVE);
    virDomainFree(dom);
    virt_conn_pool_release(conn);

    if (!pcv_vm_clone_extract_disk_info(xml, &disk_info, &preflight_error) ||
        !pcv_vm_clone_build_disk_plan(clone_name, &disk_info, &disk_plan,
                                      &preflight_error) ||
        !pcv_vm_clone_disk_plan_beta_allowed(&disk_plan, &preflight_error)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
            preflight_error ? preflight_error : "vm.clone beta guard failed");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        free(xml);
        g_free(preflight_error);
        pcv_vm_clone_disk_plan_clear(&disk_plan);
        pcv_vm_clone_disk_info_clear(&disk_info);
        _vm_clone_ctx_free(clone_ctx);
        return;
    }
    free(xml);
    g_free(preflight_error);
    pcv_vm_clone_disk_info_clear(&disk_info);

    gboolean full_copy = mode
        ? (g_strcmp0(mode, "full") == 0)
        : (disk_plan.kind != PCV_VM_CLONE_DISK_ZVOL);
    clone_ctx->full_copy = full_copy;

    if (disk_plan.kind != PCV_VM_CLONE_DISK_ZVOL && !full_copy) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
            "vm.clone cow mode is only supported for ZFS zvol disks; use mode=full for qcow2/raw");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        pcv_vm_clone_disk_plan_clear(&disk_plan);
        _vm_clone_ctx_free(clone_ctx);
        return;
    }

    if ((disk_plan.kind == PCV_VM_CLONE_DISK_QCOW2 ||
         disk_plan.kind == PCV_VM_CLONE_DISK_RAW) &&
        !pcv_vm_clone_file_copy_available()) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_CONFLICT,
            "vm.clone qcow2/raw file disk clone requires qemu-img");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        pcv_vm_clone_disk_plan_clear(&disk_plan);
        _vm_clone_ctx_free(clone_ctx);
        return;
    }

    clone_ctx->disk_kind = disk_plan.kind;
    clone_ctx->source_disk_path = disk_plan.source_disk_path;
    clone_ctx->target_disk_path = disk_plan.target_disk_path;
    clone_ctx->source_dataset = disk_plan.source_dataset;
    clone_ctx->target_dataset = disk_plan.target_dataset;
    clone_ctx->zfs_pool = disk_plan.zfs_pool;
    clone_ctx->source_zvol_name = disk_plan.source_zvol_name;
    memset(&disk_plan, 0, sizeof(disk_plan));

    if (clone_ctx->target_dataset) {
        const gchar *zfs_list_argv[] = {"zfs", "list", "-H",
                                         clone_ctx->target_dataset, NULL};
        if (pcv_spawn_sync(zfs_list_argv, NULL, NULL, NULL)) {
            gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
                "Target zvol dataset already exists");
            pure_uds_server_send_response(server, connection, e); g_free(e);
            _vm_clone_ctx_free(clone_ctx);
            return;
        }
    }

    if (clone_ctx->disk_kind == PCV_VM_CLONE_DISK_QCOW2 ||
        clone_ctx->disk_kind == PCV_VM_CLONE_DISK_RAW) {
        if (g_file_test(clone_ctx->target_disk_path, G_FILE_TEST_EXISTS)) {
            gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
                "Target disk file already exists");
            pure_uds_server_send_response(server, connection, e); g_free(e);
            _vm_clone_ctx_free(clone_ctx);
            return;
        }
    }

                                   
    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "status", "accepted");
    json_object_set_string_member(obj, "source", source);
    json_object_set_string_member(obj, "clone_name", clone_name);
    json_object_set_string_member(obj, "mode", full_copy ? "full" : "cow");
    gchar *job_id = _vm_clone_job_id(clone_ctx);
    json_object_set_string_member(obj, "job_id", job_id);
    g_free(job_id);
    json_object_set_boolean_member(obj, "guest_reset", guest_reset);
    json_object_set_string_member(obj, "storage_type",
                                  pcv_vm_clone_disk_kind_to_string(clone_ctx->disk_kind));
    json_object_set_string_member(obj, "source_disk", clone_ctx->source_disk_path);
    json_object_set_string_member(obj, "target_disk", clone_ctx->target_disk_path);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, obj);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);

                             
      
               
                                                          
                                            
                                                                      
                                                     
    GTask *clone_task = pcv_drain_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(clone_task, clone_ctx, (GDestroyNotify)_vm_clone_ctx_free);
    pcv_worker_pool_push(clone_task, _vm_clone_thread);
    g_object_unref(clone_task);                                          
}
