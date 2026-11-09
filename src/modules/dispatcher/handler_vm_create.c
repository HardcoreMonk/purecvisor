   
                            
                                                                      
  
                           
                                                                           
                                                                      
                                                                
                                                   
  
                                                              
   

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
#include "api/dispatcher_internal.h"
#include "modules/resource/resource_gate.h"


                                                           
                                                    
                                                   
                                                                       
static gboolean
_path_at_or_under(const gchar *path, const gchar *root)
{
    gsize n = strlen(root);
    return g_strcmp0(path, root) == 0 ||
           (g_str_has_prefix(path, root) && path[n] == '/');
}

                                                                
                                                      
                                              
                                                                  
                                                     
static gboolean
_valid_vm_image_dir(const gchar *dir)
{
    if (!dir || !*dir || strlen(dir) > 511)
        return FALSE;
    if (dir[0] != '/' || g_strcmp0(dir, "/") == 0 ||
        strstr(dir, "..") || strstr(dir, "//"))
        return FALSE;

    static const gchar *blocked_roots[] = {
        "/bin", "/boot", "/dev", "/etc", "/lib", "/lib64",
        "/proc", "/root", "/run", "/sbin", "/sys", "/usr", NULL
    };
    for (guint i = 0; blocked_roots[i]; i++) {
        if (_path_at_or_under(dir, blocked_roots[i]))
            return FALSE;
    }
    return TRUE;
}

                                                   
                                                                
                                                       
                                                               
                                                  
                                                         
                                                          
                                                  
                                                
                                                         
                                                                   
static gboolean
qos_applicable(const gchar *network_mode, const gchar *nic_type)
{
    return pcv_qos_vm_requires_sla(network_mode, nic_type);
}

                                                                                      
                                                                                                     
                                                                            
                                   
                                                             
                                                                     
static void
_res_deny(GSocketConnection *connection, const gchar *rpc_id, UdsServer *server, PcvResDecision d,
          const gchar *param, const gchar *value)
{
    g_autofree gchar *msg = pcv_res_decision_message(d, param);
    g_autofree gchar *response = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_FORBIDDEN, msg);
    pure_uds_server_send_response(server, connection, response);
    pcv_res_audit_denied("vm.create", param, value);
}

                                                                                        
                                                                                                         
                                                                                                      
                                                                                 
                                                                                                                                                    
                                                                                 
                                                                                                
                                                                          
                                                                            
                                        
                                                                            
                                                                    
static gboolean
_gate_create_refs(JsonObject *params, GSocketConnection *connection, const gchar *rpc_id, UdsServer *server,
                  const gchar **iso_path, const gchar **base_image, const gchar **storage_pool,
                  const gchar **image_dir, const gchar **pci_addr, const gchar *bridge, GPtrArray *keep,
                  PcvResPin *base_pin_out)
{
    g_autofree gchar *resolved_bridge = purecvisor_vm_resolve_network_bridge(bridge);
    *base_pin_out = (PcvResPin){ 0 };
    const struct {
        const gchar  *param;
        PcvResKind    kind;
        const gchar **slot;                             
        PcvResPin    *pin;                                                                   
    } refs[] = {
        { "iso_path", PCV_RES_ISO, iso_path, NULL },
        { "base_image", PCV_RES_IMAGE, base_image, base_pin_out },
        { "storage_pool", PCV_RES_STORAGE, storage_pool, NULL },
        { "image_dir", PCV_RES_STORAGE, image_dir, NULL },
        { "pci_addr", PCV_RES_PCI, pci_addr, NULL },
        { "network_bridge", PCV_RES_BRIDGE, NULL, NULL },
    };
    PcvResCaller c = { 0 };
    pcv_res_caller_init(&c, params, connection);
    for (gsize i = 0; i < G_N_ELEMENTS(refs); i++) {
        const gchar *value = refs[i].slot ? *refs[i].slot : resolved_bridge;
        if (!value || (!*value && refs[i].kind != PCV_RES_PCI))
            continue;                                                                       
        gchar *effective = NULL;
        PcvResDecision d = pcv_res_check(&c, refs[i].kind, value, &effective, refs[i].pin);
        if (d != PCV_RES_ALLOW) {
            pcv_res_caller_clear(&c);
            _res_deny(connection, rpc_id, server, d, refs[i].param, value);
            return FALSE;
        }
        if (refs[i].slot) {
            g_ptr_array_add(keep, effective);               
            *refs[i].slot = effective;
        } else {
            g_free(effective);
        }
    }
    pcv_res_caller_clear(&c);
    return TRUE;
}

                                                                                                    
                                                                                   
                                                                                     
                                                     
                                                               
static gboolean
_gate_overlay_tenant(JsonObject *params, GSocketConnection *connection, const gchar *tenant, const gchar *rpc_id,
                     UdsServer *server)
{
    if (!tenant || !*tenant)
        return TRUE;
    PcvResCaller c = { 0 };
    pcv_res_caller_init(&c, params, connection);
    PcvResDecision d = pcv_res_check_tenant_name(&c, tenant);
    pcv_res_caller_clear(&c);
    if (d == PCV_RES_ALLOW)
        return TRUE;
    _res_deny(connection, rpc_id, server, d, "tenant", tenant);
    return FALSE;
}

                                                            
  
                                                           
                                             
  
                                                  
                                                
                                                                        

                                 
  
                        
  
                       
                                                                     
                                          
                                                            
                                           
        
                                                                                      
                                             
                                 
              
                                                                          
                                                
                                                               
                                                 
  
            
                                                              
                                                                   
                                                   
                                                   
   
typedef struct {
    gchar *job_id;                                        
    PcvJobRef *job_ref;                                     
    gchar *vm_name;                                                
    PcvVmLock *lock;                                                      
} VmCreateJobCtx;

                                                        
                                                                 
                                                      
static void _vm_create_job_ctx_free(gpointer data)
{
    if (!data) return;
    VmCreateJobCtx *ctx = (VmCreateJobCtx *)data;
    pcv_job_unref(ctx->job_ref);
    g_free(ctx->job_id);
    g_free(ctx->vm_name);
    g_free(ctx);
}

   
                          
                                                          
  
                                                     
                                                               
                                                    
  
           
                                                         
                                                                             
  
             
                                                      
                                                                  
  
                                              
                                                           
   
static void
_on_vm_create_finished(GObject *source_object,
                       GAsyncResult *res,
                       gpointer user_data)
{
    VmCreateJobCtx *ctx = (VmCreateJobCtx *)user_data;
    GError *error = NULL;

                                                             
                                                 
    g_clear_pointer(&ctx->lock, pcv_vm_lock_release);

    gboolean ok = purecvisor_vm_manager_create_vm_finish(
        PURECVISOR_VM_MANAGER(source_object), res, &error);

    if (ok) {
                                               
        pcv_audit_log(pcv_drain_task_actor(res), "vm.create", ctx->vm_name, "ok", 0, 0, "local");
        pcv_ws_finish_job(ctx->job_ref, PCV_JOB_COMPLETED, NULL, NULL);
        PCV_LOG_INFO("dispatcher",
                     "vm.create job %s completed for '%s'",
                     ctx->job_id, ctx->vm_name);
    } else {
        const gchar *err_msg = error ? error->message : "Unknown error";
                                
        pcv_audit_log(pcv_drain_task_actor(res), "vm.create", ctx->vm_name, "fail", PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
        pcv_ws_finish_job(ctx->job_ref, PCV_JOB_FAILED, err_msg, err_msg);
        PCV_LOG_WARN("dispatcher",
                     "vm.create job %s FAILED for '%s': %s",
                     ctx->job_id, ctx->vm_name, err_msg);
        if (error) g_error_free(error);
    }

                                                                    
    cmap_remove(ctx->vm_name);
    _vm_create_job_ctx_free(ctx);
}

   
                                                    
  
                                                      
                                                          
                                                           
                                                       
                           
  
                               
                                            
                               
  
        
                     
                                                                
                                                                  
                                                                   
                                                                            
                                                                                
                                                                    
                                                                     
  
                                               
                                       
                                       
                                
                                
   
void handle_vm_create(PureCVisorDispatcher *self, JsonObject *params,
                              const gchar *rpc_id, UdsServer *server,
                              GSocketConnection *connection) {
    if (!json_object_has_member(params, "name")) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing parameter: name");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    const gchar *name = json_object_get_string_member(params, "name");

                                                                                   
    if (pcv_validate_vm_name(name) && !pcv_validate_new_vm_name(name)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "VM name must not look like a UUID");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

                                         
                                                  
                                                   
    {
        gboolean exists = FALSE;
        virConnectPtr conn = virt_conn_pool_acquire();
        if (conn) {
            virDomainPtr dom = virDomainLookupByName(conn, name);
            if (dom) {
                exists = TRUE;
                virDomainFree(dom);
            }
            virt_conn_pool_release(conn);
        }

        if (!exists) {
            gchar *dataset = g_strdup_printf("%s/%s", pcv_config_get_zvol_pool(), name);
            const gchar *zfs_argv[] = {"zfs", "list", "-H", "-o", "name", dataset, NULL};
                                                                 
                                                                 
                                                                
            exists = pcv_spawn_sync_timeout(zfs_argv, NULL, NULL, 10, NULL);
            g_free(dataset);
        }

        if (!exists) {
            const gchar *image_dir = pcv_config_get_image_dir();
            gchar *qcow2_path = g_strdup_printf("%s/%s.qcow2", image_dir, name);
            gchar *raw_img_path = g_strdup_printf("%s/%s.img", image_dir, name);
            gchar *raw_path = g_strdup_printf("%s/%s.raw", image_dir, name);
            exists = g_file_test(qcow2_path, G_FILE_TEST_EXISTS) ||
                     g_file_test(raw_img_path, G_FILE_TEST_EXISTS) ||
                     g_file_test(raw_path, G_FILE_TEST_EXISTS);
            g_free(qcow2_path);
            g_free(raw_img_path);
            g_free(raw_path);
        }

        if (exists) {
            gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
                "VM already exists — delete the VM first");
            pure_uds_server_send_response(server, connection, err);
            g_free(err);
            return;
        }
    }

                                                 
    if (!pcv_validate_vm_name(name)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid VM name — must be 1-64 chars [a-zA-Z0-9_-]");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    gint vcpu = 1;
    gint memory_mb = 1024;
    gint disk_size_gb = 50;
    gint vlan_id = 0;                                          
    const gchar *iso_path = nullptr;
    const gchar *bridge = nullptr;
    const gchar *storage_type = nullptr;                                  
    const gchar *storage_pool = nullptr;                                    
    const gchar *image_dir = nullptr;                                   
    const gchar *nic_type = nullptr;                                          
    const gchar *pci_addr = nullptr;                                      

    if (json_object_has_member(params, "vcpu")) vcpu = json_object_get_int_member(params, "vcpu");
    if (json_object_has_member(params, "memory_mb")) memory_mb = json_object_get_int_member(params, "memory_mb");
                                                      
    if (json_object_has_member(params, "disk_size_gb")) disk_size_gb = json_object_get_int_member(params, "disk_size_gb");
    else if (json_object_has_member(params, "disk_gb")) disk_size_gb = json_object_get_int_member(params, "disk_gb");
    if (json_object_has_member(params, "vlan_id")) vlan_id = json_object_get_int_member(params, "vlan_id");

                                                              
                                                      
                                      
                                           
                                                                       
                                               
    if (vcpu < 1 || vcpu > 256) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid vcpu — must be between 1 and 256");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }
    if (memory_mb < 256 || memory_mb > 1048576) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid memory_mb — must be between 256 and 1048576 (1TB)");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }
    if (disk_size_gb < 0 || disk_size_gb > 65536) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid disk_size_gb — must be between 0 and 65536 (64TB)");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }
    if (vlan_id < 0 || vlan_id > 4094) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid vlan_id — must be between 0 and 4094");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }
    if (json_object_has_member(params, "iso_path")) iso_path = json_object_get_string_member(params, "iso_path");
    if (json_object_has_member(params, "network_bridge")) {
        bridge = json_object_get_string_member(params, "network_bridge");
    }
                                                                    
                                              
                                                              
                                                              
                                                                
                                                                            
                                                                      
                                                        
    {
        gint         v_disk   = (disk_size_gb > 0) ? disk_size_gb : PCV_MIN_DISK_GB;                      
        const gchar *v_bridge = (bridge && *bridge) ? bridge : NULL;                                    
        GError      *v_err    = NULL;
        if (!pcv_validate_vm_create_params(name, vcpu, memory_mb, v_disk,
                                           iso_path, v_bridge, &v_err)) {
            gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                (v_err && v_err->message) ? v_err->message : "Invalid vm.create parameters");
            pure_uds_server_send_response(server, connection, err);
            g_free(err);
            if (v_err) g_error_free(v_err);
            return;
        }
    }
    {
        g_autofree gchar *resolved_bridge = purecvisor_vm_resolve_network_bridge(bridge);
        if (resolved_bridge && pcv_vpc_bridge_is_managed(resolved_bridge)) {
            gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_CONFLICT,
                "VPC-managed NICs must be attached through vpc.attachment.create");
            pure_uds_server_send_response(server, connection, err);
            g_free(err);
            return;
        }
    }
    if (json_object_has_member(params, "storage_type")) {
        storage_type = json_object_get_string_member(params, "storage_type");
                                       
        if (storage_type &&
            g_strcmp0(storage_type, "zvol") != 0 &&
            g_strcmp0(storage_type, "qcow2") != 0 &&
            g_strcmp0(storage_type, "raw") != 0) {
            gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                "Invalid storage_type — must be 'zvol', 'qcow2', or 'raw'");
            pure_uds_server_send_response(server, connection, err);
            g_free(err);
            return;
        }
    }
    if (json_object_has_member(params, "storage_pool")) {
        storage_pool = json_object_get_string_member(params, "storage_pool");
    }
    if (json_object_has_member(params, "image_dir")) {
        image_dir = json_object_get_string_member(params, "image_dir");
    }
    if (json_object_has_member(params, "storage_location")) {
        const gchar *storage_location = json_object_get_string_member(params, "storage_location");
        if (storage_location && *storage_location) {
            if (!storage_type && storage_location[0] == '/' && !image_dir) {
                image_dir = storage_location;
            } else if (!storage_type && !storage_pool) {
                storage_pool = storage_location;
            } else if (g_strcmp0(storage_type, "zvol") == 0 && !storage_pool) {
                storage_pool = storage_location;
            } else if ((g_strcmp0(storage_type, "qcow2") == 0 ||
                        g_strcmp0(storage_type, "raw") == 0) && !image_dir) {
                image_dir = storage_location;
            }
        }
    }
    if (storage_pool && *storage_pool && !pcv_disp_valid_vm_storage_pool(storage_pool)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid storage_pool — use a relative ZFS dataset path such as 'tank/vms'");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }
    if (image_dir && *image_dir && !_valid_vm_image_dir(image_dir)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid image_dir — use a safe absolute directory outside system roots");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }
                                                           
    if (json_object_has_member(params, "nic_type")) {
        nic_type = json_object_get_string_member(params, "nic_type");
        if (nic_type &&
            g_strcmp0(nic_type, "bridge") != 0 &&
            g_strcmp0(nic_type, "dpdk") != 0 &&
            g_strcmp0(nic_type, "sriov") != 0) {
            gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                "Invalid nic_type — must be 'bridge', 'dpdk', or 'sriov'");
            pure_uds_server_send_response(server, connection, err);
            g_free(err);
            return;
        }
    }
                                                                        
                                                                      
    if (g_strcmp0(nic_type, "dpdk") == 0 &&
        (!bridge || !*bridge || g_strcmp0(bridge, "none") == 0 ||
         !pcv_validate_bridge_name(bridge))) {
        gchar *err = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "nic_type=dpdk requires an explicit valid network_bridge");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }
                                        
                                                                                          
    pci_addr = pcv_disp_json_string_member(params, "pci_addr");
                                                                    
    const gchar *base_image = nullptr;
    if (json_object_has_member(params, "base_image")) {
        base_image = json_object_get_string_member(params, "base_image");
    }
                                                                                    
                                                                             
                                                                  
                                                                  
    if (base_image && *base_image && !pcv_validate_base_image_path(base_image)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid base_image: must be an absolute .qcow2/.qcow/.img/.raw path without '..'");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }
                                                                                          
    g_autoptr(GPtrArray) res_keep = g_ptr_array_new_with_free_func(g_free);
    PcvResPin base_pin = { 0 };                                                                      
    if (!_gate_create_refs(params, connection, rpc_id, server, &iso_path, &base_image, &storage_pool, &image_dir,
                           &pci_addr, bridge, res_keep, &base_pin))
        return;
                                                                                             
                                                               
                                                   
                                                                              
    if (g_strcmp0(nic_type, "sriov") == 0) {
        if (!pci_addr) {
            gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                "nic_type=sriov requires pci_addr");
            pure_uds_server_send_response(server, connection, err);
            g_free(err);
            return;
        }
        PcvResNorm pci_norm = { 0 };
        if (!pcv_res_normalize(PCV_RES_PCI, pci_addr, &pci_norm, NULL)) {
            gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                "Invalid pci_addr: must be a PCI address in DDDD:BB:SS.F or BB:SS.F form");
            pure_uds_server_send_response(server, connection, err);
            g_free(err);
            return;
        }
        g_ptr_array_add(res_keep, pci_norm.ref);               
        pci_addr = pci_norm.ref;
                                                                                           
                                                                              
                                                                 
                                                             
        g_autofree gchar *sriov_bridge = purecvisor_vm_resolve_network_bridge(bridge);
        if (!sriov_bridge || !*sriov_bridge) {
            gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, PCV_VM_SRIOV_NO_BRIDGE_MSG);
            pure_uds_server_send_response(server, connection, err);
            g_free(err);
            return;
        }
    }
    const gchar *owner = pcv_disp_json_string_member(params, "_pcv_caller_sub");

                                                                            
                                                                          
                                                                           
                                                                         
                                    
                                                    
                                                                   
    const gchar *network_mode = nullptr;
    const gchar *tenant = nullptr;
    if (json_object_has_member(params, "network_mode")) {
        network_mode = json_object_get_string_member(params, "network_mode");
    }
    if (json_object_has_member(params, "tenant")) {
        tenant = json_object_get_string_member(params, "tenant");
    }
    {
        GError *nmt_err = nullptr;
        if (!pcv_vm_validate_network_mode_tenant(network_mode, tenant, &nmt_err)) {
            gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                nmt_err ? nmt_err->message : "Invalid network_mode/tenant combination");
            pure_uds_server_send_response(server, connection, err);
            g_free(err);
            g_clear_error(&nmt_err);
            return;
        }
    }
                                                                                              
                                                                             
                                                                                    
                                                  
                                                       
    if (g_strcmp0(nic_type, "sriov") == 0 &&
        g_strcmp0(network_mode, "tenant-overlay") == 0) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, PCV_VM_SRIOV_OVERLAY_MSG);
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }
    if (!_gate_overlay_tenant(params, connection, tenant, rpc_id, server))                 
        return;

                                                          
                                                           
                                                               
                                                                      
                                                                   
    gboolean vm_qos_required = qos_applicable(network_mode, nic_type);
    PcvQosSla vm_qos_sla;
    memset(&vm_qos_sla, 0, sizeof vm_qos_sla);
    if (vm_qos_required) {
        GError *qos_err = nullptr;
        if (!pcv_qos_sla_from_json(params, &vm_qos_sla, &qos_err)) {
            gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                qos_err ? qos_err->message :
                "qos_min_mbps/qos_max_mbps required for bridge/tenant-overlay VMs");
            pure_uds_server_send_response(server, connection, err);
            g_free(err);
            g_clear_error(&qos_err);
            return;
        }
    }

                                            
    gint boot_mode = 0;                                          
    gboolean tpm = FALSE;
    gint cpu_mode = 0;                                                                              
    gboolean hugepages = FALSE;

    if (json_object_has_member(params, "boot_mode")) boot_mode = json_object_get_int_member(params, "boot_mode");
                                                                           
                                                  
    if (json_object_has_member(params, "firmware")) {
        const gchar *fw = json_object_get_string_member(params, "firmware");
        if (g_strcmp0(fw, "uefi") == 0) boot_mode = 1;
        else if (g_strcmp0(fw, "uefi-secureboot") == 0) boot_mode = 2;
        else boot_mode = 0;                                   
    }
    if (json_object_has_member(params, "tpm")) tpm = json_object_get_boolean_member(params, "tpm");
    if (json_object_has_member(params, "cpu_mode")) cpu_mode = json_object_get_int_member(params, "cpu_mode");
    if (json_object_has_member(params, "hugepages")) hugepages = json_object_get_boolean_member(params, "hugepages");

                                                 
                                                                  
                                                            
    {
        gboolean exists = FALSE;
        const gchar *effective_pool = (storage_pool && *storage_pool)
            ? storage_pool : pcv_config_get_zvol_pool();
        const gchar *effective_image_dir = (image_dir && *image_dir)
            ? image_dir : pcv_config_get_image_dir();

        if (!storage_type || g_strcmp0(storage_type, "zvol") == 0) {
            gchar *dataset = g_strdup_printf("%s/%s", effective_pool, name);
            const gchar *zfs_argv[] = {"zfs", "list", "-H", "-o", "name", dataset, NULL};
                                                                 
                                                                 
                                                                
            exists = pcv_spawn_sync_timeout(zfs_argv, NULL, NULL, 10, NULL);
            g_free(dataset);
        }

        if (!exists && (!storage_type ||
                        g_strcmp0(storage_type, "qcow2") == 0 ||
                        g_strcmp0(storage_type, "raw") == 0)) {
            gchar *qcow2_path = g_strdup_printf("%s/%s.qcow2", effective_image_dir, name);
            gchar *raw_img_path = g_strdup_printf("%s/%s.img", effective_image_dir, name);
            gchar *raw_path = g_strdup_printf("%s/%s.raw", effective_image_dir, name);
            exists = g_file_test(qcow2_path, G_FILE_TEST_EXISTS) ||
                     g_file_test(raw_img_path, G_FILE_TEST_EXISTS) ||
                     g_file_test(raw_path, G_FILE_TEST_EXISTS);
            g_free(qcow2_path);
            g_free(raw_img_path);
            g_free(raw_path);
        }

        if (exists) {
            gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
                "VM already exists in selected storage location — delete the VM first");
            pure_uds_server_send_response(server, connection, err);
            g_free(err);
            return;
        }
    }

                                                       
                                                              
                                                             
                                                                    
                                                           
                                                         
                                                               
    gchar *create_lock_err = NULL;
    PcvVmLock *op_lock = pcv_vm_lock_acquire(name, VM_OP_CREATING, &create_lock_err);
    if (!op_lock) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
                       create_lock_err ? create_lock_err : "VM is busy (another operation in progress)");
        pure_uds_server_send_response(server, connection, e);
        g_free(e); g_free(create_lock_err);
        return;
    }

                                                                    
                                               
                                                            
      
                                     
                                                             
                                                    
                                                           
    PcvJobRef *job_ref = pcv_job_admit("vm.create", name, NULL, "vm.create");
                                                                    
    if (!job_ref) {
        pcv_vm_lock_release(op_lock);
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
                                                 "Job persistence unavailable — VM creation rejected");
        pure_uds_server_send_response(server, connection, e);
        g_free(e);
        return;
    }
    pcv_job_progress(job_ref, 0, "VM creation started");

    gchar *job_id = g_strdup(pcv_job_id(job_ref));
    JsonObject *accepted = json_object_new();
    json_object_set_boolean_member(accepted, "accepted", TRUE);
    json_object_set_string_member(accepted, "name", name);
    json_object_set_string_member(accepted, "job_id", job_id);
    json_object_set_string_member(accepted, "status", "accepted");

                                                        
                                                                    
                                                            
                                                                          
    gchar *warn_br = purecvisor_vm_resolve_network_bridge(bridge);
    if (warn_br
        && g_strcmp0(warn_br, "virbr0") != 0
        && g_strcmp0(warn_br, "lxcbr0") != 0) {
        gchar *dhcp_conf = g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-%s.conf", warn_br);
        gboolean has_dhcp = g_file_test(dhcp_conf, G_FILE_TEST_EXISTS);
        g_free(dhcp_conf);
        if (!has_dhcp) {
            json_object_set_string_member(accepted, "network_warning",
                "bridge has no managed DHCP — guests need static IP or external DHCP "
                "(hint: pcvctl network dhcp --enable <bridge>)");
        }
    }
    g_free(warn_br);

    JsonNode *an = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(an, accepted);
    gchar *resp = pure_rpc_build_success_response(rpc_id, an);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);

                                                 
      
                                                                 
                                                                  
                                                  
    VmCreateJobCtx *job_ctx = g_new0(VmCreateJobCtx, 1);
    job_ctx->job_id  = job_id;                                                   
    job_ctx->job_ref = job_ref;             
    job_ctx->vm_name = g_strdup(name);                              
    job_ctx->lock = op_lock;                                    

                                                    
                                                                   
                                                                 
    GCancellable *cancel = g_cancellable_new();
    cmap_register(name, cancel);

                                           
    purecvisor_vm_manager_create_vm_async(self->vm_manager,
                                          name,
                                          vcpu,
                                          memory_mb,
                                          disk_size_gb,
                                          iso_path,
                                          bridge,
                                          vlan_id,
                                          boot_mode,
                                          tpm,
                                          cpu_mode,
                                          hugepages,
                                          storage_type,
                                          storage_pool,
                                          image_dir,
                                          nic_type,
                                          pci_addr,
                                          base_image, &base_pin,
                                          owner,
                                          network_mode,                      
                                          tenant,                            
                                          vm_qos_required,                
                                          &vm_qos_sla,                    
                                          cancel,
                                          _on_vm_create_finished,
                                          job_ctx);
    g_object_unref(cancel);                            
}
