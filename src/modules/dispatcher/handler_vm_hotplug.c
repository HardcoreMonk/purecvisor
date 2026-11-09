   
                             
                                                                
  
                           
                                                   
                                                    
                                        
  
                                                        
                                                  
                                                          
                                                           
  
            
                                                                         
                                                                                                 
                                                                                                        
  
                                                                                                
                                                                                                                    
                                                                                         
                                                           
  
                             
                                                     
                                                                   
                                                             
                                                                   
  
         
                                                                        
                                                                       
                                                                      
  
          
                                                         
                             
                                
  
                                                               
                                                                                
                                
                                                                              
                                                                            
                                                            
                                                              
                                                               
                                            
                                                                       
                                   
                                                                                        
                                                                                    
                                                                          
                                                                              
                                             
   
#include "api/drain.h"
#include <glib.h>
#include <gio/gio.h>
#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>
#include <json-glib/json-glib.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "api/uds_server.h"
#include "modules/dispatcher/rpc_utils.h"
#include "modules/dispatcher/handler_vm_hotplug.h"
#include "modules/dispatcher/hotplug_affect_policy.h"
#include "modules/dispatcher/guest_agent_exec.h"                              
#include "modules/audit/pcv_audit.h"
#include "modules/virt/virt_conn_pool.h"
#include "modules/core/vm_state.h"                                                  
#include "purecvisor/pcv_handler_util.h"
#include "utils/pcv_spawn.h"

  
                                                 
                                         
                                           
   
extern virDomainPtr pure_virt_get_domain(virConnectPtr conn, const gchar *identifier);


                                                                    
              
  
                                                         
                                                          
                          
  
                                          
                                                      
                                                      
  
            
                                                                     
                                                            
                                                 
                                                                       
typedef struct {
    gchar *vm_id;                                                               
    gint target_value;                                           
    gchar *rpc_id;                                         
    UdsServer *server;                                       
    GSocketConnection *connection;                                  
    PcvVmLock *lock;                                                              
    gboolean config_only;                                                                                                              
    const gchar *method;                                                                              
} VmHotplugCtx;

   
                    
                                   
                                                     
  
                                                      
                               
                                                                           
                                                                               
                                                                 
   
static void free_hotplug_ctx(gpointer data) {
    if (!data) return;                                                            
    VmHotplugCtx *ctx = (VmHotplugCtx *)data;
    g_free(ctx->vm_id);
    g_free(ctx->rpc_id);
    if (ctx->server) g_object_unref(ctx->server);
    if (ctx->connection) g_object_unref(ctx->connection);
    g_free(ctx);
}

                                                                 
                                                          
  
                                                         
                                                                 
                                                          
                                                                  
                                                                
                                                                         
                                                              
                                                                       
static gboolean hotplug_get_affect_flags(virDomainPtr dom, unsigned int *flags, gboolean config_only, GError **error) {
    int active = virDomainIsActive(dom);                              
    if (active < 0) {
        virErrorPtr err = virGetLastError();
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Failed to read VM active state: %s",
                    err ? err->message : "Unknown");
        return FALSE;
    }

                                                                   
                                                                      
    *flags = pcv_hotplug_compute_affect_flags(active == 1, config_only);

    return TRUE;
}

                                                                    
                     
  
                        
                                                     
  
          
                  
                             
                                               
                                             
  
                                                   
                                          
  
                                                     
                                     
                                                                                  
                                                                        
                                                                       
static void vm_set_memory_worker(GTask *task, gpointer source_obj, gpointer task_data, GCancellable *cancellable) {
    VmHotplugCtx *ctx = (VmHotplugCtx *)task_data;
    virConnectPtr conn = virt_conn_pool_acquire();                        
    
    if (!conn) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to connect to Libvirt.");
        return;
    }

                                                                 
    virDomainPtr dom = pure_virt_get_domain(conn, ctx->vm_id);
    if (!dom) {
        virt_conn_pool_release(conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "VM %s not found.", ctx->vm_id);
        return;
    }

                                              
    unsigned long memory_kb = (unsigned long)ctx->target_value * 1024;

    unsigned int flags = 0;
    GError *state_error = NULL;
    if (!hotplug_get_affect_flags(dom, &flags, ctx->config_only, &state_error)) {
        g_task_return_error(task, state_error);
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        return;
    }

    if (virDomainSetMemoryFlags(dom, memory_kb, flags) < 0) {
        virErrorPtr err = virGetLastError();
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Memory hotplug failed: %s", err ? err->message : "Unknown");
    } else {
        g_task_return_boolean(task, TRUE);
    }

    virDomainFree(dom);
    virt_conn_pool_release(conn);
}

                                                                    
                      
  
                        
                                                    
  
          
                  
                             
                                             
  
                                               
                                          
  
                                                       
                                            
                                                                 
                                          
                                                                       
static void vm_set_vcpu_worker(GTask *task, gpointer source_obj, gpointer task_data, GCancellable *cancellable) {
    VmHotplugCtx *ctx = (VmHotplugCtx *)task_data;
    virConnectPtr conn = virt_conn_pool_acquire();
    
    if (!conn) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to connect to Libvirt.");
        return;
    }

    virDomainPtr dom = pure_virt_get_domain(conn, ctx->vm_id);
    if (!dom) {
        virt_conn_pool_release(conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "VM %s not found.", ctx->vm_id);
        return;
    }

    unsigned int flags = 0;
    GError *state_error = NULL;
    if (!hotplug_get_affect_flags(dom, &flags, ctx->config_only, &state_error)) {
        g_task_return_error(task, state_error);
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        return;
    }

    if (virDomainSetVcpusFlags(dom, ctx->target_value, flags) < 0) {
        virErrorPtr err = virGetLastError();
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "vCPU hotplug failed: %s", err ? err->message : "Unknown");
    } else {
        g_task_return_boolean(task, TRUE);
    }

    virDomainFree(dom);
    virt_conn_pool_release(conn);
}

                                                                    
              
  
                                        
                                        
  
                                      
                                           
                                                                
  
                                                       
                                                         
                
                                                               
                                                           
                                                                       
static void hotplug_callback(GObject *source_obj, GAsyncResult *res, gpointer user_data) {
    GTask *task = G_TASK(res);
    VmHotplugCtx *ctx = (VmHotplugCtx *)user_data;
    GError *error = NULL;

                                                         
                                                                  
    g_clear_pointer(&ctx->lock, pcv_vm_lock_release);

    gboolean success = g_task_propagate_boolean(task, &error);
    pcv_audit_log(pcv_drain_task_actor(res), ctx->method, ctx->vm_id, success ? "ok" : "fail",                                                   
                  success ? 0 : PURE_RPC_ERR_ZFS_OPERATION, 0, "local");                                                    

    if (!success) {
                                         
        gchar *err_resp = pure_rpc_build_error_response(ctx->rpc_id, PURE_RPC_ERR_ZFS_OPERATION, error->message);
        pure_uds_server_send_response(ctx->server, ctx->connection, err_resp);
        g_free(err_resp);
        g_error_free(error);
    } else {
                                               
        gchar *succ_resp = pure_rpc_build_success_response(ctx->rpc_id, json_node_new(JSON_NODE_NULL));
        pure_uds_server_send_response(ctx->server, ctx->connection, succ_resp);
        g_free(succ_resp);
    }
}

                                                                    
                       
                                                                       

   
                                
                                                  
  
                                                                
                                
                             
                                 
                                                                        
  
                                                           
                                
                                                  
                                            
   
void handle_vm_set_memory_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
                                                                                    
    if (!params || !json_object_has_member(params, "vm_id") || !json_object_has_member(params, "memory_mb")) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid params: 'vm_id' or 'memory_mb' missing");
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
        return;
    }

                            
                                     
                                                  
                                                                  
                                                                         
                                                               
                                 
    const gchar *_lock_vm = json_object_get_string_member(params, "vm_id");   gchar *lock_err = NULL;
    PcvVmLock *op_lock = pcv_vm_lock_acquire(_lock_vm, VM_OP_TUNING, &lock_err);
    if (!op_lock) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
                       lock_err ? lock_err : "VM busy (operation in progress)");
        pure_uds_server_send_response(server, connection, e);
        g_free(e); g_free(lock_err);
        return;
    }
    VmHotplugCtx *ctx = g_new0(VmHotplugCtx, 1);   ctx->lock = op_lock;                                      
    ctx->vm_id = g_strdup(json_object_get_string_member(params, "vm_id"));
    ctx->target_value = json_object_get_int_member(params, "memory_mb");   ctx->method = "vm.set_memory";
    ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = g_object_ref(server);
    ctx->connection = g_object_ref(connection);
                                                                         
                                                               
    const gchar *_apply = json_object_has_member(params, "apply")
        ? json_object_get_string_member(params, "apply") : NULL;
    ctx->config_only = (_apply && g_strcmp0(_apply, "config") == 0);

    GTask *task = pcv_drain_task_new(NULL, NULL, hotplug_callback, ctx);
    g_task_set_task_data(task, ctx, free_hotplug_ctx);
    g_task_run_in_thread(task, vm_set_memory_worker);
    g_object_unref(task);
}

   
                              
                                                
  
                                                                                         
                                
                             
                                 
                                                                        
  
                                                                   
                                                
                                                  
                                             
   
void handle_vm_set_vcpu_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
                                                                    
                                                                     
    gint vcpu_count = 0;
    gboolean has_vcpu_count = pcv_rpc_params_get_int_alias(params, "vcpu_count", "vcpu", &vcpu_count);
    if (!has_vcpu_count) {
        has_vcpu_count = pcv_rpc_params_get_int_alias(params, "count", NULL, &vcpu_count);
    }

    if (!params || !json_object_has_member(params, "vm_id") || !has_vcpu_count) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid params: 'vm_id' and one of 'vcpu_count', 'vcpu', or 'count' required");
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
        return;
    }

                                                                         
                                                               
                                 
    const gchar *_lock_vm = json_object_get_string_member(params, "vm_id");   gchar *lock_err = NULL;
    PcvVmLock *op_lock = pcv_vm_lock_acquire(_lock_vm, VM_OP_TUNING, &lock_err);
    if (!op_lock) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
                       lock_err ? lock_err : "VM busy (operation in progress)");
        pure_uds_server_send_response(server, connection, e);
        g_free(e); g_free(lock_err);
        return;
    }
    VmHotplugCtx *ctx = g_new0(VmHotplugCtx, 1);   ctx->lock = op_lock;                                      
    ctx->vm_id = g_strdup(json_object_get_string_member(params, "vm_id"));
    ctx->target_value = vcpu_count;   ctx->method = "vm.set_vcpu";
    ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = g_object_ref(server);
    ctx->connection = g_object_ref(connection);
                                                                            
                                                                  
                                                
    const gchar *_apply = json_object_has_member(params, "apply")
        ? json_object_get_string_member(params, "apply") : NULL;
    ctx->config_only = (_apply && g_strcmp0(_apply, "config") == 0);

    GTask *task = pcv_drain_task_new(NULL, NULL, hotplug_callback, ctx);
    g_task_set_task_data(task, ctx, free_hotplug_ctx);
    g_task_run_in_thread(task, vm_set_vcpu_worker);
    g_object_unref(task);
}

                                                                    
                                      
  
                                            
  
                                                                    
                               
                                     
                                                               
  
                 
                      
                                        
                                              
  
                                                           
                                                        
                                                                              
                                                          
                                                                       
void handle_vm_pin_vcpu(JsonObject *params, const gchar *rpc_id,
                         UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm_id = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;
    if (!vm_id) vm_id = json_object_has_member(params, "vm_id")
        ? json_object_get_string_member(params, "vm_id") : NULL;

    const gchar *cpuset = json_object_has_member(params, "cpuset")
        ? json_object_get_string_member(params, "cpuset") : NULL;

    if (!vm_id || !cpuset || !json_object_has_member(params, "vcpu")) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing: name/vm_id, vcpu, or cpuset");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    gint vcpu_id = (gint)json_object_get_int_member(params, "vcpu");

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "libvirt connection unavailable");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

                                         
    virNodeInfo node_info;
    if (virNodeGetInfo(conn, &node_info) < 0) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "Failed to get host CPU info");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        virt_conn_pool_release(conn);
        return;
    }

                                
                                                          
                                                      
                                                
                                                   
                                                              
    int max_cpus = VIR_NODEINFO_MAXCPUS(node_info);
    int maplen = VIR_CPU_MAPLEN(max_cpus);
    unsigned char *cpumap = g_malloc0((gsize)maplen);

      
                                           
                                    
                                                             
                                                                       
       
    gboolean parse_ok = TRUE;
    gchar **parts = g_strsplit(cpuset, ",", -1);
    for (int i = 0; parts[i] && parse_ok; i++) {
        gchar *part = g_strstrip(parts[i]);
        if (strlen(part) == 0) continue;

        gchar *dash = strchr(part, '-');
        if (dash) {
                           
            *dash = '\0';
            gint start = atoi(part);
            gint end   = atoi(dash + 1);
            if (start < 0 || end < start || end >= max_cpus) {
                parse_ok = FALSE;
                break;
            }
            for (gint c = start; c <= end; c++) {
                VIR_USE_CPU(cpumap, c);
            }
        } else {
                             
            gint cpu = atoi(part);
            if (cpu < 0 || cpu >= max_cpus) {
                parse_ok = FALSE;
                break;
            }
            VIR_USE_CPU(cpumap, cpu);
        }
    }
    g_strfreev(parts);

    if (!parse_ok) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid cpuset range (exceeds host CPU count or malformed)");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        g_free(cpumap);
        virt_conn_pool_release(conn);
        return;
    }

    virDomainPtr dom = pure_virt_get_domain(conn, vm_id);
    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "VM not found");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        g_free(cpumap);
        virt_conn_pool_release(conn);
        return;
    }

                                                             
      
                               
                                              
                                               
                                                    
                                                 
                                                          
    gchar *lock_err = NULL;
    PcvVmLock *op_lock = pcv_vm_lock_acquire(vm_id, VM_OP_TUNING, &lock_err);
    if (!op_lock) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
                       lock_err ? lock_err : "VM busy (operation in progress)");
        pure_uds_server_send_response(server, connection, e);
        g_free(e); g_free(lock_err);
        g_free(cpumap); virDomainFree(dom); virt_conn_pool_release(conn);
        return;
    }

    int rc = virDomainPinVcpu(dom, (unsigned int)vcpu_id, cpumap, maplen);

    if (rc < 0) {
        virErrorPtr e = virGetLastError();
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            e ? e->message : "vCPU pinning failed");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
    } else {
        JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
        JsonObject *res_obj = json_object_new();
        json_object_set_boolean_member(res_obj, "pinned", TRUE);
        json_object_set_int_member(res_obj, "vcpu", vcpu_id);
        json_object_set_string_member(res_obj, "cpuset", cpuset);
        json_node_take_object(res_node, res_obj);

        gchar *resp = pure_rpc_build_success_response(rpc_id, res_node);
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
    }

    pcv_vm_lock_release(op_lock);                               
    g_free(cpumap);
    virDomainFree(dom);
    virt_conn_pool_release(conn);
}

                                                                    
                                              
  
                                                             
                          
  
         
                               
                                                
                                                
  
       
                                                      
                                                               
                                        
  
                                                     
                                              
                                                                                
                                                     
                                                                       
void handle_vm_set_bandwidth(JsonObject *params, const gchar *rpc_id,
                              UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;

    if (!name) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing required parameter: name");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    gint inbound_kbps = json_object_has_member(params, "inbound_kbps")
        ? (gint)json_object_get_int_member(params, "inbound_kbps") : 0;
    gint outbound_kbps = json_object_has_member(params, "outbound_kbps")
        ? (gint)json_object_get_int_member(params, "outbound_kbps") : 0;

    if (inbound_kbps <= 0 && outbound_kbps <= 0) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "At least one of inbound_kbps or outbound_kbps must be > 0");
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

    virDomainPtr dom = pure_virt_get_domain(conn, name);
    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_VM_NOT_FOUND,
            "VM not found");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        virt_conn_pool_release(conn);
        return;
    }

                                        
      
                                      
                                                        
                                                        
                                                                 
      
                                                       
                                                               
    gchar *iface = NULL;
    {
        const gchar *argv2[] = {"virsh", "domiflist", name, NULL};
        gchar *stdout_buf = NULL;
        GError *spawn_err = NULL;
        if (pcv_spawn_sync(argv2, &stdout_buf, NULL, &spawn_err) && stdout_buf) {
            gchar **lines = g_strsplit(stdout_buf, "\n", -1);
            for (int i = 0; lines[i]; i++) {
                gchar *line = g_strstrip(lines[i]);
                if (i >= 2 && line[0] && line[0] != '-') {
                    gchar **cols = g_strsplit_set(line, " \t", -1);
                    if (cols[0] && cols[0][0]) {
                        iface = g_strdup(cols[0]);
                    }
                    g_strfreev(cols);
                    break;
                }
            }
            g_strfreev(lines);
        }
        g_free(stdout_buf);
        if (spawn_err) g_error_free(spawn_err);
    }

    if (!iface) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            "No network interface found on VM");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        return;
    }

                                                        
      
                                                                     
                                                                       
      
             
                                                           
                                                            
                                                               
      
                                                 
    virTypedParameter typed_params[2];
    int nparams = 0;

    if (inbound_kbps > 0) {
        strncpy(typed_params[nparams].field, "inbound.average",
                VIR_TYPED_PARAM_FIELD_LENGTH);
        typed_params[nparams].type = VIR_TYPED_PARAM_UINT;
        typed_params[nparams].value.ui = (unsigned int)inbound_kbps;
        nparams++;
    }
    if (outbound_kbps > 0) {
        strncpy(typed_params[nparams].field, "outbound.average",
                VIR_TYPED_PARAM_FIELD_LENGTH);
        typed_params[nparams].type = VIR_TYPED_PARAM_UINT;
        typed_params[nparams].value.ui = (unsigned int)outbound_kbps;
        nparams++;
    }

                                                          
    gchar *lock_err = NULL;
    PcvVmLock *op_lock = pcv_vm_lock_acquire(name, VM_OP_TUNING, &lock_err);
    if (!op_lock) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
                       lock_err ? lock_err : "VM busy (operation in progress)");
        pure_uds_server_send_response(server, connection, e);
        g_free(e); g_free(lock_err);
        g_free(iface); virDomainFree(dom); virt_conn_pool_release(conn);
        return;
    }

    int rc = virDomainSetInterfaceParameters(dom, iface, typed_params, nparams,
                 VIR_DOMAIN_AFFECT_LIVE);

    if (rc < 0) {
        virErrorPtr e = virGetLastError();
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            e ? e->message : "Failed to set bandwidth limits");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
    } else {
        JsonObject *obj = json_object_new();
        json_object_set_string_member(obj, "vm", name);
        json_object_set_string_member(obj, "interface", iface);
        json_object_set_int_member(obj, "inbound_kbps", inbound_kbps);
        json_object_set_int_member(obj, "outbound_kbps", outbound_kbps);
        JsonNode *node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(node, obj);
        gchar *resp = pure_rpc_build_success_response(rpc_id, node);
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
    }

    pcv_vm_lock_release(op_lock);                               
    g_free(iface);
    virDomainFree(dom);
    virt_conn_pool_release(conn);
}

                                                                    
                                                   
  
                                                  
                                                         
  
         
                           
  
          
                                                        
                                 
  
                                                           
                                        
                                                                     
                                                                
                                                                       
void handle_vm_memory_stats_request(JsonObject *params, const gchar *rpc_id,
                                     UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;

    if (!name) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing required parameter: name");
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

    virDomainPtr dom = pure_virt_get_domain(conn, name);
    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_VM_NOT_FOUND,
            "VM not found");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        virt_conn_pool_release(conn);
        return;
    }

                                                   
      
                                     
                                          
                                                
                                                              
                                                               
    virDomainSetMemoryStatsPeriod(dom, 5, VIR_DOMAIN_AFFECT_LIVE);

                           
    virDomainMemoryStatStruct stats[VIR_DOMAIN_MEMORY_STAT_NR];
    int nr_stats = virDomainMemoryStats(dom, stats, VIR_DOMAIN_MEMORY_STAT_NR, 0);

    if (nr_stats < 0) {
        virErrorPtr e = virGetLastError();
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            e ? e->message : "Failed to get memory stats");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        return;
    }

                                    
    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "vm", name);

    for (int i = 0; i < nr_stats; i++) {
        switch (stats[i].tag) {
        case VIR_DOMAIN_MEMORY_STAT_ACTUAL_BALLOON:
            json_object_set_int_member(obj, "actual_balloon_kb", (gint64)stats[i].val);
            break;
        case VIR_DOMAIN_MEMORY_STAT_RSS:
            json_object_set_int_member(obj, "rss_kb", (gint64)stats[i].val);
            break;
        case VIR_DOMAIN_MEMORY_STAT_UNUSED:
            json_object_set_int_member(obj, "unused_kb", (gint64)stats[i].val);
            break;
        case VIR_DOMAIN_MEMORY_STAT_AVAILABLE:
            json_object_set_int_member(obj, "available_kb", (gint64)stats[i].val);
            break;
        case VIR_DOMAIN_MEMORY_STAT_USABLE:
            json_object_set_int_member(obj, "usable_kb", (gint64)stats[i].val);
            break;
        case VIR_DOMAIN_MEMORY_STAT_SWAP_IN:
            json_object_set_int_member(obj, "swap_in", (gint64)stats[i].val);
            break;
        case VIR_DOMAIN_MEMORY_STAT_SWAP_OUT:
            json_object_set_int_member(obj, "swap_out", (gint64)stats[i].val);
            break;
        default:
            break;
        }
    }

    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, obj);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);

    virDomainFree(dom);
    virt_conn_pool_release(conn);
}

                                                                    
                                          
  
                                                              
  
         
                           
  
       
                            
                               
                                    
                                                                   
  
                                                            
               
                                                                     
                                                             
                                                                       
void handle_vm_cpu_stats_request(JsonObject *params, const gchar *rpc_id,
                                  UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;

    if (!name) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing required parameter: name");
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

    virDomainPtr dom = pure_virt_get_domain(conn, name);
    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_VM_NOT_FOUND,
            "VM not found");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        virt_conn_pool_release(conn);
        return;
    }

                      
    virDomainInfo info;
    if (virDomainGetInfo(dom, &info) < 0) {
        virErrorPtr e = virGetLastError();
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            e ? e->message : "Failed to get domain info");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        return;
    }

                              
    int max_vcpu = virDomainGetVcpusFlags(dom,
        VIR_DOMAIN_AFFECT_CONFIG | VIR_DOMAIN_VCPU_MAXIMUM);
    if (max_vcpu < 0) max_vcpu = info.nrVirtCpu;

                           
    int nr_vcpus = info.nrVirtCpu;
    virVcpuInfoPtr vcpuinfo = g_new0(virVcpuInfo, nr_vcpus);

                                    
      
                                       
                                           
                                                  
                                          
                                                                  
                                                 
    int host_cpus = virNodeGetCPUMap(conn, NULL, NULL, 0);
    if (host_cpus <= 0) host_cpus = 64;                                
    int maplen = VIR_CPU_MAPLEN(host_cpus);
    unsigned char *cpumaps = g_new0(unsigned char, nr_vcpus * maplen);

    int got = virDomainGetVcpus(dom, vcpuinfo, nr_vcpus, cpumaps, maplen);

                    
    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "vm", name);
    json_object_set_int_member(obj, "vcpu_count", nr_vcpus);
    json_object_set_int_member(obj, "max_vcpu", max_vcpu);
    json_object_set_int_member(obj, "cpu_time_ns", (gint64)info.cpuTime);

    JsonArray *arr = json_array_new();

    if (got > 0) {
        for (int i = 0; i < got; i++) {
            JsonObject *vcpu_obj = json_object_new();
            json_object_set_int_member(vcpu_obj, "number", vcpuinfo[i].number);
            json_object_set_int_member(vcpu_obj, "state", vcpuinfo[i].state);
            json_object_set_int_member(vcpu_obj, "cpu_time", (gint64)vcpuinfo[i].cpuTime);

                                                     
            GString *aff_str = g_string_new(NULL);
            for (int c = 0; c < host_cpus; c++) {
                if (VIR_CPU_USABLE(cpumaps, maplen, i, c)) {
                    if (aff_str->len > 0) g_string_append_c(aff_str, ',');
                    g_string_append_printf(aff_str, "%d", c);
                }
            }
            json_object_set_string_member(vcpu_obj, "cpu_affinity", aff_str->str);
            g_string_free(aff_str, TRUE);

            json_array_add_object_element(arr, vcpu_obj);
        }
    }

    json_object_set_array_member(obj, "vcpus", arr);

    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, obj);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);

    g_free(vcpuinfo);
    g_free(cpumaps);
    virDomainFree(dom);
    virt_conn_pool_release(conn);
}
