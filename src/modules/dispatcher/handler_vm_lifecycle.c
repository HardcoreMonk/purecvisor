   
                               
                                                      
  
                           
                                                   
                                                    
                                        
  
          
                                                        
                                                           
                                                                
                                                       
                                    
  
            
                                                            
                                                           
  
        
                                                                         
                                                                         
                                                                
                                                   
                                                                
                                     
  
         
                                            
                                                           
                                                      
  
        
                                                                    
                                                           
  
        
                                                                               
                                                                                
                                                           
  
                                                                                        
                                                                              
  
            
                                                               
                                                                                                   
                                                                                   
  
                                                                                                  
                                                                                                                                     
                                                         
                                               
                                                                                                      
  
          
                                                                                       
                                                               
                                               
  
         
                                                          
                                        
                                                                    
                                                               
                                              
  
          
                               
                             
                               
   
#include "api/drain.h"
#include <glib.h>
#include <gio/gio.h>
#include <libvirt/libvirt.h>
#include <libvirt/libvirt-qemu.h>
#include <libvirt/virterror.h>
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include "../audit/pcv_audit.h"
#include <string.h>
#include "api/uds_server.h"
#include "modules/dispatcher/vnc_port_xml.h"
#include "modules/dispatcher/rpc_utils.h"
#include "modules/core/vm_state.h"
#include "modules/dispatcher/handler_vm_lifecycle.h"
#include "modules/dispatcher/guest_agent_exec.h"                                               
#include "modules/virt/vm_manager.h"                                                       
#include "utils/pcv_spawn.h"
#include "modules/dispatcher/vm_limit_policy.h"
#include "utils/pcv_validate.h"
#include "modules/virt/virt_conn_pool.h"
#include "purecvisor/pcv_handler_util.h"
#include "../network/security_group.h"
#include "modules/dispatcher/handler_vm_internal.h"

   
                               
                                         
                                                     
  
                                                         
  
                                                                  
                                                              
                                                                 
                                                                                
   
void pcv_disp_free_lifecycle_ctx(gpointer data) {
    if (!data) return;                                                                  
    VmLifecycleCtx *ctx = (VmLifecycleCtx *)data;
    g_free(ctx->vm_id);
    g_free(ctx->action);
    g_free(ctx->rpc_id);
    if (ctx->server) g_object_unref(ctx->server);                                      
    if (ctx->connection) g_object_unref(ctx->connection);
    g_free(ctx);                                                           
}

                                                             
                                                                                 
                                                               

                                                                  
                                                             
                                          
                                                               
static gboolean _cancel_source_cb(gpointer c) {
    g_cancellable_cancel(G_CANCELLABLE(c));                                  
    return G_SOURCE_REMOVE;
}

                                                                    
                                 
  
                                    
                                                    
                                                 
                        
                                                                       

   
                  
                        
                                                 
  
                                                          
                                                           
                                                                           
  
                                                               
                                         
   
static void vm_list_worker(GTask *task, gpointer source_obj, gpointer task_data, GCancellable *cancellable) {
    (void)source_obj; (void)task_data;
                    
    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                                "vm.list timed out (30s)");
        return;
    }
                                          
    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to connect to Libvirt.");
        return;
    }

                                                       
    virDomainPtr *domains;
    int ret = virConnectListAllDomains(conn, &domains, 0);
    if (ret < 0) {
        virt_conn_pool_release(conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to list domains.");
        return;
    }

                                     
    JsonArray *array = json_array_new();
    for (int i = 0; i < ret; i++) {
        JsonObject *vm_obj = json_object_new();
        char uuid[VIR_UUID_STRING_BUFLEN];
        virDomainGetUUIDString(domains[i], uuid);

        json_object_set_string_member(vm_obj, "uuid", uuid);
        json_object_set_string_member(vm_obj, "name", virDomainGetName(domains[i]));

                                                       
        virDomainInfo info;
        if (virDomainGetInfo(domains[i], &info) < 0) {
                                               
            virDomainFree(domains[i]);
            json_object_unref(vm_obj);
            continue;
        }
        const char *state_str = (info.state == VIR_DOMAIN_RUNNING)    ? "running" :
                                (info.state == VIR_DOMAIN_SHUTOFF)    ? "shutoff" :
                                (info.state == VIR_DOMAIN_PAUSED)     ? "paused" :
                                (info.state == VIR_DOMAIN_SHUTDOWN)   ? "shutdown" :
                                (info.state == VIR_DOMAIN_CRASHED)    ? "crashed" :
                                (info.state == VIR_DOMAIN_PMSUSPENDED)? "pmsuspended" : "unknown";
        json_object_set_string_member(vm_obj, "state", state_str);
        json_object_set_int_member(vm_obj, "vcpu", (gint64)info.nrVirtCpu);
        json_object_set_int_member(vm_obj, "memory_mb", (gint64)(info.maxMem / 1024));

                                                 
        char *xml = virDomainGetXMLDesc(domains[i], VIR_DOMAIN_XML_INACTIVE);
        if (xml) {
                                                              
            const char *storage_type = "unknown";
            if (strstr(xml, "<disk type='block'"))
                storage_type = "zvol";
            else if (strstr(xml, "<disk type='file'"))
                storage_type = "qcow2";
            json_object_set_string_member(vm_obj, "storage_type", storage_type);

                                               
            const char *boot_mode = "bios";
            const char *loader_pos = strstr(xml, "<loader");
            if (loader_pos) {
                boot_mode = strstr(loader_pos, "secure='yes'") ? "uefi-secureboot" : "uefi";
            }
            json_object_set_string_member(vm_obj, "boot_mode", boot_mode);

                                                              
            const char *disk_format = "unknown";
            const char *drv = strstr(xml, "<driver");
            if (drv) {
                const char *drv_type = strstr(drv, "type='qcow2'");
                if (drv_type)
                    disk_format = "qcow2";
                else if (strstr(drv, "type='raw'"))
                    disk_format = "raw";
            }
            json_object_set_string_member(vm_obj, "disk_format", disk_format);

                                                                      
            const char *disk_path = NULL;
            const char *src_dev = strstr(xml, "<source dev='");
            const char *src_file = strstr(xml, "<source file='");
            if (src_dev) {
                src_dev += 13;                              
                const char *end = strchr(src_dev, '\'');
                if (end) {
                    gchar *path = g_strndup(src_dev, (gsize)(end - src_dev));
                    json_object_set_string_member(vm_obj, "disk_path", path);
                    g_free(path);
                    disk_path = "set";
                }
            } else if (src_file) {
                src_file += 14;                               
                const char *end = strchr(src_file, '\'');
                if (end) {
                    gchar *path = g_strndup(src_file, (gsize)(end - src_file));
                    json_object_set_string_member(vm_obj, "disk_path", path);
                    g_free(path);
                    disk_path = "set";
                }
            }
            if (!disk_path)
                json_object_set_string_member(vm_obj, "disk_path", "");

                                                       
            int net_count = 0;
            const char *p = xml;
            while ((p = strstr(p, "<interface type=")) != NULL) {
                net_count++;
                p++;
            }
            json_object_set_int_member(vm_obj, "network_count", (gint64)net_count);

            free(xml);                               
        }

                                                 
        int autostart_val = 0;
        virDomainGetAutostart(domains[i], &autostart_val);
        json_object_set_boolean_member(vm_obj, "auto_start", autostart_val ? TRUE : FALSE);

                                                    
        int snap_count = virDomainSnapshotNum(domains[i], 0);
        json_object_set_int_member(vm_obj, "snapshot_count", (gint64)(snap_count >= 0 ? snap_count : 0));

        json_array_add_object_element(array, vm_obj);
        virDomainFree(domains[i]);                            
    }
    free(domains);                                                        
                                                                         
                                                                                                     
                                                                                         
                                                                                      
                     
                                                                                 
    if (virConnectIsAlive(conn) != 1) {
        json_array_unref(array);
        virt_conn_pool_release(conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to list domains.");
        return;
    }
    virt_conn_pool_release(conn);                                 

                                                  
    JsonNode *root_node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(root_node, array);                                 
    g_task_return_pointer(task, root_node, (GDestroyNotify)json_node_free);
}

   
                    
                                     
  
                                                      
                                            
  
                                      
                                 
                                
                                                                             
   
static void vm_list_callback(GObject *source_obj, GAsyncResult *res, gpointer user_data) {
    GTask *task = G_TASK(res);
    VmLifecycleCtx *ctx = (VmLifecycleCtx *)user_data;
    GError *error = NULL;

                                    
    guint tid = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(task), "timeout_id"));
    pcv_source_remove_if_attached(tid);

                                                       
    JsonNode *result_node = g_task_propagate_pointer(task, &error);
                                                         
    pcv_audit_log(pcv_drain_task_actor(res), "vm.list", "", error ? "fail" : "ok",
                  error ? PURE_RPC_ERR_ZFS_OPERATION : 0, 0, "local");
    if (error) {
        gchar *err_resp = pure_rpc_build_error_response(ctx->rpc_id, PURE_RPC_ERR_ZFS_OPERATION, error->message);
        pure_uds_server_send_response(ctx->server, ctx->connection, err_resp);
        g_free(err_resp);
        g_error_free(error);                         
    } else if (ctx->page_limit > 0 && JSON_NODE_HOLDS_ARRAY(result_node)) {
                                               
        JsonArray *full = json_node_get_array(result_node);
        gint total = (gint)json_array_get_length(full);
        gint off = ctx->page_offset < 0 ? 0 : ctx->page_offset;
        if (off > total) off = total;

        JsonArray *paged = json_array_new();
        for (gint i = off; i < total && i < off + ctx->page_limit; i++)
            json_array_add_element(paged, json_array_dup_element(full, (guint)i));

        JsonObject *pg = json_object_new();
        json_object_set_array_member(pg, "items", paged);
        json_object_set_int_member(pg, "total", total);
        json_object_set_int_member(pg, "offset", off);
        json_object_set_int_member(pg, "limit", ctx->page_limit);
        json_object_set_boolean_member(pg, "has_more", off + ctx->page_limit < total);

        JsonNode *pg_node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(pg_node, pg);
        gchar *succ_resp = pure_rpc_build_success_response(ctx->rpc_id, pg_node);
        pure_uds_server_send_response(ctx->server, ctx->connection, succ_resp);
        g_free(succ_resp);
        json_node_free(result_node);
    } else {
        gchar *succ_resp = pure_rpc_build_success_response(ctx->rpc_id, result_node);
        pure_uds_server_send_response(ctx->server, ctx->connection, succ_resp);
        g_free(succ_resp);
                                                                             
    }
}

   
                          
                                     
  
                                                        
                                    
  
                                                   
                      
                                                                  
                                                           
                                                                           
   
void handle_vm_list_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    VmLifecycleCtx *ctx = g_new0(VmLifecycleCtx, 1);
    ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = g_object_ref(server);
    ctx->connection = g_object_ref(connection);

                                               
    ctx->page_offset = (params && json_object_has_member(params, "offset"))
        ? (gint)json_object_get_int_member(params, "offset") : 0;
    ctx->page_limit = (params && json_object_has_member(params, "limit"))
        ? (gint)json_object_get_int_member(params, "limit") : 0;

                           
    if (ctx->page_offset < 0 || ctx->page_offset > 100000 ||
        (ctx->page_limit != 0 && (ctx->page_limit < 0 || ctx->page_limit > 10000))) {
        gchar *err = pure_rpc_build_error_response(rpc_id,
            PURE_RPC_ERR_INVALID_PARAMS,
            "Pagination out of range: offset 0-100000, limit 1-10000");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        g_free(ctx->rpc_id); g_object_unref(ctx->server);
        g_object_unref(ctx->connection); g_free(ctx);
        return;
    }

                                                                                 
                                                                                      
                             
    GCancellable *cancel = g_cancellable_new();
    GTask *task = pcv_drain_task_new(NULL, cancel, vm_list_callback, ctx);
    g_task_set_task_data(task, ctx, pcv_disp_free_lifecycle_ctx);
                                                      
    guint timeout_id = g_timeout_add_seconds(30, _cancel_source_cb, cancel);
    g_object_set_data(G_OBJECT(task), "timeout_id", GUINT_TO_POINTER(timeout_id));
    g_task_run_in_thread(task, vm_list_worker);
    g_object_unref(task);
    g_object_unref(cancel);
}

                                                                    
                                                      
  
                                                           
                                
                                                  
                                                                       

   
                        
                       
                                 
  
                                                            
                                                       
  
                                                   
  
                                                     
                                                    
  
          
                                                      
                                               
                                            
  
                                                                         
   
virDomainPtr pure_virt_get_domain(virConnectPtr conn, const gchar *identifier) {
                                                      
    virDomainPtr dom = virDomainLookupByUUIDString(conn, identifier);
    if (!dom) {
                                               
        virResetLastError();
                                               
        dom = virDomainLookupByName(conn, identifier);
    }
    return dom;
}
   
                    
                                       
  
                                             
                                                           
                                                       
                                                                         
  
                                                     
                                                                    
                                                                
                                                                    
                                                                     
  
                                                                                                  
  
                                        
                                                      
  
                                                         
   
static void vm_action_worker(GTask *task, gpointer source_obj, gpointer task_data, GCancellable *cancellable) {
    (void)source_obj;
    VmLifecycleCtx *ctx = (VmLifecycleCtx *)task_data;
    GError *error = NULL;

                    
    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                                "vm.%s timed out (30s)", ctx->action ? ctx->action : "unknown");
        return;
    }

                                 
    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to connect to Libvirt.");
        return;
    }

      
                                         
                                            
                                                            
       
    virDomainPtr dom = pure_virt_get_domain(conn, ctx->vm_id);

                                                
                                                                   
                                                                
                                                  
    if (!dom) {
        g_set_error(&error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "VM not found: %s", ctx->vm_id);
        virt_conn_pool_release(conn);
        g_task_return_error(task, error);
        return;
    }

                                                    
      
                                                    
                                                  
                                                   

                                                                     
    if (g_strcmp0(ctx->action, "stop") == 0) {
        if (!virDomainIsActive(dom)) {
            g_print("VM '%s' is already shut off. Skipping stop sequence.\n", ctx->vm_id);
        } else if (virDomainDestroy(dom) < 0) {
            g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to stop VM: %s", ctx->vm_id);
            virDomainFree(dom); virt_conn_pool_release(conn); g_task_return_error(task, error); return;
        }
    }
                                                       
                                                              
    else if (g_strcmp0(ctx->action, "pause") == 0) {
        if (!virDomainIsActive(dom)) {
            g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "VM '%s' is not running, cannot pause.", ctx->vm_id);
            virDomainFree(dom); virt_conn_pool_release(conn); g_task_return_error(task, error); return;
        }
        if (virDomainSuspend(dom) < 0) {
            g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to pause VM: %s", ctx->vm_id);
            virDomainFree(dom); virt_conn_pool_release(conn); g_task_return_error(task, error); return;
        }
    }
                                   
                                                                
                                                                  
                                                             
    else if (g_strcmp0(ctx->action, "resume") == 0) {
        virDomainInfo info;
        if (virDomainGetInfo(dom, &info) < 0) {
            g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to query VM state: %s", ctx->vm_id);
            virDomainFree(dom); virt_conn_pool_release(conn); g_task_return_error(task, error); return;
        }
        if (info.state != VIR_DOMAIN_PAUSED) {
            g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "VM '%s' is not paused.", ctx->vm_id);
            virDomainFree(dom); virt_conn_pool_release(conn); g_task_return_error(task, error); return;
        }
        if (virDomainResume(dom) < 0) {
            g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to resume VM: %s", ctx->vm_id);
            virDomainFree(dom); virt_conn_pool_release(conn); g_task_return_error(task, error); return;
        }
    }
                                                      
                                                             
      
                                                                
                                                          
    else if (g_strcmp0(ctx->action, "limit") == 0) {
        if (!virDomainIsActive(dom)) {
            g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Entity '%s' is not active. Cannot apply live limits.", ctx->vm_id);
            virDomainFree(dom); virt_conn_pool_release(conn); g_task_return_error(task, error); return;
        }

          
                                         
                                                                        
                                                                             
                                                                                 
           
        gint64 cpu_quota_us = 0;
        if (pcv_vm_limit_cpu_quota_us(ctx->cpu_quota, &cpu_quota_us)) {
            virTypedParameter params[1];
                                                                                
            strncpy(params[0].field, VIR_DOMAIN_SCHEDULER_VCPU_QUOTA, VIR_TYPED_PARAM_FIELD_LENGTH);
            params[0].type = VIR_TYPED_PARAM_LLONG;
            params[0].value.l = cpu_quota_us;

            if (virDomainSetSchedulerParametersFlags(dom, params, 1, VIR_DOMAIN_AFFECT_LIVE) < 0) {
                g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to inject cgroup limits to kernel.");
                virDomainFree(dom); virt_conn_pool_release(conn); g_task_return_error(task, error); return;
            }
        }

          
                                                                              
          
                                  
                                                         
                                                                                       
                                                                           
                                                                       
                                                            
                                                             
          
                                           
                                                                     
                                                           
                                                                     
                                                                             
                                                                            
                                                                       
                                                 
           
        if (ctx->mem_quota_mb != 0) {
            virTypedParameter mem_params[1];
                                                                 
            strncpy(mem_params[0].field, VIR_DOMAIN_MEMORY_HARD_LIMIT, VIR_TYPED_PARAM_FIELD_LENGTH);
            mem_params[0].type = VIR_TYPED_PARAM_ULLONG;

            if (ctx->mem_quota_mb < 0) {
                                                       
                mem_params[0].value.ul = VIR_DOMAIN_MEMORY_PARAM_UNLIMITED;
            } else {
                                                                 
                virDomainInfo info;
                if (virDomainGetInfo(dom, &info) == 0 && info.maxMem > 0) {
                    guint64 guest_ram_mb = (guint64)info.maxMem / 1024;                
                    guint64 floor_mb = pcv_memtune_safe_floor_mb(guest_ram_mb);
                    if ((guint64)ctx->mem_quota_mb < floor_mb) {
                        g_warning("[vm.limit] Rejecting unsafe hard_limit %d MB for '%s': "
                                  "below safe floor %" G_GUINT64_FORMAT " MB "
                                  "(guest RAM %" G_GUINT64_FORMAT " MB + overhead headroom).",
                                  ctx->mem_quota_mb, ctx->vm_id, floor_mb, guest_ram_mb);
                        g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Unsafe memtune hard_limit %d MB for '%s': below safe floor "
                            "%" G_GUINT64_FORMAT " MB (guest RAM %" G_GUINT64_FORMAT
                            " MB + overhead headroom). hard_limit caps the whole QEMU "
                            "process; a value below this floor triggers host cgroup "
                            "OOM-kill. Use a value >= floor, or -1 for unlimited.",
                            ctx->mem_quota_mb, ctx->vm_id, floor_mb, guest_ram_mb);
                        virDomainFree(dom); virt_conn_pool_release(conn);
                        g_task_return_error(task, error); return;
                    }
                } else {
                                                                     
                    g_warning("[vm.limit] Could not query guest RAM for '%s' "
                              "(virDomainGetInfo failed) — skipping hard_limit headroom "
                              "safety check; applying requested %d MB as-is.",
                              ctx->vm_id, ctx->mem_quota_mb);
                }
                mem_params[0].value.ul = (unsigned long long)ctx->mem_quota_mb * 1024;            
            }

            if (virDomainSetMemoryParameters(dom, mem_params, 1, VIR_DOMAIN_AFFECT_LIVE) < 0) {
                g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to inject memory limits to kernel.");
                virDomainFree(dom); virt_conn_pool_release(conn); g_task_return_error(task, error); return;
            }
        }
    }


                                            
    virDomainFree(dom);
    virt_conn_pool_release(conn);

                                                      
                                                                
                                              
    if (g_strcmp0(ctx->action, "stop") == 0)
        pcv_security_group_sync_vm(ctx->vm_id);

    g_task_return_boolean(task, TRUE);

}

   
                      
                                             
  
                                                        
                                                       
  
                                                  
                                                             
  
                                                          
                                                            
                                                              
                                                 
   
static void vm_action_callback(GObject *source_obj, GAsyncResult *res, gpointer user_data) {
    GTask *task = G_TASK(res);
    VmLifecycleCtx *ctx = (VmLifecycleCtx *)user_data;
    GError *error = NULL;

                    
    guint action_tid = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(task), "timeout_id"));
    pcv_source_remove_if_attached(action_tid);

    gboolean success = g_task_propagate_boolean(task, &error);
                                                                                
    g_clear_pointer(&ctx->lock, pcv_vm_lock_release);

                                                                                                 
    {
        gchar *audit_method = g_strdup_printf("vm.%s", ctx->action ? ctx->action : "unknown");
        pcv_audit_log(pcv_drain_task_actor(res), audit_method, ctx->vm_id,
                      success ? "ok" : "fail",
                      success ? 0 : PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
        g_free(audit_method);
    }

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

                                  
typedef struct {
    gchar           *vm_id;
    gchar           *origin;
    PcvVmStopDoneFn  done;
    gpointer         done_data;
    PcvVmLock       *lock;                                        
    gchar           *actor;                                                         
} GracefulStopCtx;

                                                                     
static void _graceful_stop_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
    GracefulStopCtx *c = user_data;
    GError *err = NULL;
    gboolean ok = purecvisor_vm_manager_stop_vm_finish(PURECVISOR_VM_MANAGER(source), res, &err);
    g_clear_pointer(&c->lock, pcv_vm_lock_release);
    pcv_audit_log(c->actor, "vm.stop", c->vm_id, ok ? "ok" : "fail",
                  ok ? 0 : PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
    if (!ok)
        g_warning("[vm.stop] graceful stop failed for '%s' (origin=%s): %s",
                  c->vm_id, c->origin, err ? err->message : "unknown");
    if (c->done)
        c->done(c->vm_id, ok, err, c->done_data);
    if (err) g_error_free(err);
    g_free(c->vm_id); g_free(c->origin); g_free(c->actor);
    g_free(c);
}

gboolean pcv_vm_graceful_stop_submit(PureCVisorVmManager *mgr, const gchar *vm_id, const gchar *origin,
                                     const gchar *actor, PcvVmStopDoneFn done, gpointer user_data, gchar **err_msg)
{
    if (!mgr || !vm_id || !*vm_id) {
        if (err_msg) *err_msg = g_strdup("vm manager unavailable");
        return FALSE;
    }
    PcvVmLock *op_lock = pcv_vm_lock_acquire(vm_id, VM_OP_STOPPING, err_msg);
    if (!op_lock)
        return FALSE;
    g_message("[vm.stop] graceful submit '%s' (origin=%s)", vm_id, origin ? origin : "vm.batch");
    GracefulStopCtx *c = g_new0(GracefulStopCtx, 1);
    c->vm_id = g_strdup(vm_id);
    c->origin = g_strdup(origin ? origin : "vm.batch");
    c->actor = g_strdup(actor);
    c->done = done;
    c->done_data = user_data;
    c->lock = op_lock;
    purecvisor_vm_manager_stop_vm_async(mgr, vm_id, _graceful_stop_done, c);
    return TRUE;
}

                                                                                  
void pcv_vm_stop_report_rejected(const gchar *vm_id, const gchar *origin, const gchar *reason, const gchar *actor)
{
    const gchar *why = (reason && *reason) ? reason : "stop rejected";
    pcv_audit_log(actor, "vm.stop", vm_id, "fail", PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
    g_warning("[vm.stop] batch stop rejected '%s' (origin=%s): %s", vm_id, origin ? origin : "vm.batch", why);
}

   
                          
                                           
  
                                                           
                                          
                                                                 
  
          
                          
                                                             
                                                      
                          
  
                                                                  
                               
   
void handle_vm_stop_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    if (!params || !json_object_has_member(params, "vm_id")) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid params: 'vm_id' missing");
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
        return;
    }
    const gchar *vm_id = json_object_get_string_member(params, "vm_id");

                                            
                                                           
                                                              
                                                                                                
                                              
    gchar *err_msg = NULL;
    PcvVmLock *op_lock = pcv_vm_lock_acquire(vm_id, VM_OP_STOPPING, &err_msg);
    if (!op_lock) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, err_msg);
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp); g_free(err_msg); return;
    }

    VmLifecycleCtx *ctx = g_new0(VmLifecycleCtx, 1);
    ctx->vm_id = g_strdup(vm_id); ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = g_object_ref(server); ctx->connection = g_object_ref(connection);
                                                   
    ctx->action = g_strdup("stop");
    ctx->lock = op_lock;                                          

    GCancellable *cancel = g_cancellable_new();
    GTask *task = pcv_drain_task_new(NULL, cancel, vm_action_callback, ctx);
    g_task_set_task_data(task, ctx, pcv_disp_free_lifecycle_ctx);
    g_object_set_data(G_OBJECT(task), "is_delete", GINT_TO_POINTER(FALSE));
    guint tid = g_timeout_add_seconds(30, _cancel_source_cb, cancel);
    g_object_set_data(G_OBJECT(task), "timeout_id", GUINT_TO_POINTER(tid));
    g_task_run_in_thread(task, vm_action_worker);
    g_object_unref(task);
    g_object_unref(cancel);
}


   
                           
                                                 
                                 
  
                                                         
                                                              
  
                                 
                                             
                                                                 
   
void handle_vm_pause_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    if (!params || !json_object_has_member(params, "vm_id")) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid params: 'vm_id' missing");
        pure_uds_server_send_response(server, connection, err_resp); g_free(err_resp); return;
    }
    const gchar *vm_id = json_object_get_string_member(params, "vm_id");
    VmLifecycleCtx *ctx = g_new0(VmLifecycleCtx, 1);
    ctx->vm_id = g_strdup(vm_id); ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = g_object_ref(server); ctx->connection = g_object_ref(connection);
    ctx->action = g_strdup("pause");
    GTask *task = pcv_drain_task_new(NULL, NULL, vm_action_callback, ctx);
    g_task_set_task_data(task, ctx, pcv_disp_free_lifecycle_ctx);
    g_task_run_in_thread(task, vm_action_worker);
    g_object_unref(task);
}

   
                            
                                               
                                                     
  
                                               
                                                              
   
void handle_vm_resume_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    if (!params || !json_object_has_member(params, "vm_id")) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid params: 'vm_id' missing");
        pure_uds_server_send_response(server, connection, err_resp); g_free(err_resp); return;
    }
    const gchar *vm_id = json_object_get_string_member(params, "vm_id");
    VmLifecycleCtx *ctx = g_new0(VmLifecycleCtx, 1);
    ctx->vm_id = g_strdup(vm_id); ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = g_object_ref(server); ctx->connection = g_object_ref(connection);
    ctx->action = g_strdup("resume");
    GTask *task = pcv_drain_task_new(NULL, NULL, vm_action_callback, ctx);
    g_task_set_task_data(task, ctx, pcv_disp_free_lifecycle_ctx);
    g_task_run_in_thread(task, vm_action_worker);
    g_object_unref(task);
}

   
                           
                                                         
  
                                                          
                                                              
                                                                 
  
        
                             
                                                    
                                                     
  
                                               
                                       
   
void handle_vm_limit_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
                                                        
    if (!params || !json_object_has_member(params, "vm_id")) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid params: 'vm_id' missing");
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
        return;
    }

                                         
    if (json_object_has_member(params, "cpu")) {
        gint64 cpu_val = json_object_get_int_member(params, "cpu");
        if (cpu_val != -1 && (cpu_val <= 0 || cpu_val >= 10000000)) {
            gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                "Invalid params: 'cpu' must be -1 (unlimited) or 1..9999999 (percent)");
            pure_uds_server_send_response(server, connection, err_resp);
            g_free(err_resp);
            return;
        }
    }
    if (json_object_has_member(params, "mem")) {
        gint64 mem_val = json_object_get_int_member(params, "mem");
        if (mem_val != -1 && (mem_val <= 0 || mem_val > 1048576)) {
            gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                "Invalid params: 'mem' must be -1 (unlimited) or 1..1048576 MB");
            pure_uds_server_send_response(server, connection, err_resp);
            g_free(err_resp);
            return;
        }
    }

    const gchar *vm_id_str = json_object_get_string_member_with_default(params, "vm_id", NULL);
    if (!vm_id_str || !*vm_id_str) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid params: 'vm_id' must be non-empty string");
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
        return;
    }
    VmLifecycleCtx *ctx = g_new0(VmLifecycleCtx, 1);
    ctx->vm_id = g_strdup(vm_id_str);
    ctx->action = g_strdup("limit");
    
    if (json_object_has_member(params, "cpu")) {
        ctx->cpu_quota = json_object_get_int_member(params, "cpu");
    }

                                                
    if (json_object_has_member(params, "mem")) {
        ctx->mem_quota_mb = json_object_get_int_member(params, "mem");
    }
    ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = g_object_ref(server);
    ctx->connection = g_object_ref(connection);

    GTask *task = pcv_drain_task_new(NULL, NULL, vm_action_callback, ctx);
    g_task_set_task_data(task, ctx, pcv_disp_free_lifecycle_ctx);
    
                                                                  
    g_object_set_data(G_OBJECT(task), "is_delete", GINT_TO_POINTER(FALSE)); 
    
    g_task_run_in_thread(task, vm_action_worker);
    g_object_unref(task);
}

                                                                    
                                
  
                                                             
                                                    
  
                                                             
                                         
                                                                       
static void vm_metrics_callback(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GTask *task = G_TASK(res);
    VmLifecycleCtx *ctx = (VmLifecycleCtx *)user_data;
    GError *error = NULL;
    gboolean _success = g_task_propagate_boolean(task, &error);
                                                 
    pcv_audit_log(pcv_drain_task_actor(res), "vm.metrics", ctx->vm_id ?: "",
                  _success ? "ok" : "fail",
                  _success ? 0 : PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
    if (_success) {
                                     
        JsonObject *result_obj = json_object_new();
        json_object_set_int_member(result_obj, "cpu", ctx->out_cpu_pct);
        json_object_set_int_member(result_obj, "mem", ctx->out_mem_pct);
        json_object_set_int_member(result_obj, "vcpu", ctx->out_vcpu);
        json_object_set_int_member(result_obj, "memory_mb", ctx->out_memory_mb);
        json_object_set_int_member(result_obj, "disk_rd", ctx->out_disk_rd);
        json_object_set_int_member(result_obj, "disk_wr", ctx->out_disk_wr);
        json_object_set_int_member(result_obj, "net_rx", ctx->out_net_rx);
        json_object_set_int_member(result_obj, "net_tx", ctx->out_net_tx);
        json_object_set_int_member(result_obj, "disk_rd_req", ctx->out_disk_rd_req);
        json_object_set_int_member(result_obj, "disk_wr_req", ctx->out_disk_wr_req);
        json_object_set_int_member(result_obj, "net_rx_pkts", ctx->out_net_rx_pkts);
        json_object_set_int_member(result_obj, "net_tx_pkts", ctx->out_net_tx_pkts);

        JsonNode *result_node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(result_node, result_obj);

        gchar *resp = pure_rpc_build_success_response(ctx->rpc_id, result_node);
        pure_uds_server_send_response(ctx->server, ctx->connection, resp);
        g_free(resp);                                    
    } else {
        gchar *err_resp = pure_rpc_build_error_response(ctx->rpc_id, PURE_RPC_ERR_ZFS_OPERATION, error->message);
        pure_uds_server_send_response(ctx->server, ctx->connection, err_resp);
        g_free(err_resp);
        g_error_free(error);
    }
}

                                                                    
                                    
  
                                        
                                       
  
                                                              
                                                              
                                                                              
  
                                                              
                                         
                                                                       
static void vm_metrics_worker(GTask *task, gpointer source_obj, gpointer task_data, GCancellable *cancellable) {
    VmLifecycleCtx *ctx = (VmLifecycleCtx *)task_data;

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
                                                                             
        g_warning("vm_metrics_worker: connection pool exhausted for VM '%s'",
                  ctx->vm_id ? ctx->vm_id : "(null)");
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Hypervisor connection pool exhausted");
        return;
    }
    virDomainPtr dom = pure_virt_get_domain(conn, ctx->vm_id);

                                                  
    if (!dom || !virDomainIsActive(dom)) {
        ctx->out_cpu_pct = 0;
        ctx->out_mem_pct = 0;
    } else {
        virDomainInfo info1, info2;

          
                      
                                        
                                   
                                   
                                                                        
          
                                                             
                                                  
                             
           
        if (virDomainGetInfo(dom, &info1) < 0) {
            ctx->out_cpu_pct = 0; ctx->out_mem_pct = 0;
            goto metrics_cleanup;
        }
        g_usleep(100000);                                   
        if (virDomainGetInfo(dom, &info2) < 0) {
            ctx->out_cpu_pct = 0; ctx->out_mem_pct = 0;
            goto metrics_cleanup;
        }

        unsigned long long time_diff = info2.cpuTime - info1.cpuTime;
        unsigned long long wall_diff = 100000000ULL * info1.nrVirtCpu;                            
        ctx->out_cpu_pct = (wall_diff > 0) ? (int)((time_diff * 100) / wall_diff) : 0;
        if (ctx->out_cpu_pct > 100) ctx->out_cpu_pct = 100;

          
                      
                                                                     
                                                                  
                                       
           
        virDomainMemoryStatStruct mem_stats[VIR_DOMAIN_MEMORY_STAT_NR];
        int nr_stats = virDomainMemoryStats(dom, mem_stats, VIR_DOMAIN_MEMORY_STAT_NR, 0);
        unsigned long long mem_actual = info2.memory;
        unsigned long long mem_usable = 0;
        unsigned long long mem_unused = 0;
        unsigned long long mem_rss = 0;
        for (int i = 0; i < nr_stats; i++) {
            switch (mem_stats[i].tag) {
            case VIR_DOMAIN_MEMORY_STAT_ACTUAL_BALLOON:
                mem_actual = mem_stats[i].val;
                break;
            case VIR_DOMAIN_MEMORY_STAT_USABLE:
                mem_usable = mem_stats[i].val;
                break;
            case VIR_DOMAIN_MEMORY_STAT_UNUSED:
                mem_unused = mem_stats[i].val;
                break;
            case VIR_DOMAIN_MEMORY_STAT_RSS:
                mem_rss = mem_stats[i].val;
                break;
            default:
                break;
            }
        }

        unsigned long long mem_used = info2.memory / 5;                         
                                                              
                                                           
        if (mem_actual > 0 && mem_usable > 0 && mem_actual > mem_usable)
            mem_used = mem_actual - mem_usable;                                    
        else if (mem_actual > 0 && mem_unused > 0 && mem_actual > mem_unused)
            mem_used = mem_actual - mem_unused;                                   
        else if (mem_actual > 0 && mem_rss > 0)
            mem_used = mem_rss < mem_actual ? mem_rss : mem_actual;                              

        ctx->out_mem_pct = (mem_actual > 0) ? (int)((mem_used * 100) / mem_actual) : 0;
        if (ctx->out_mem_pct > 100) ctx->out_mem_pct = 100;                         

                                
        ctx->out_vcpu = (gint)info2.nrVirtCpu;
        ctx->out_memory_mb = (gint64)(info2.maxMem / 1024);

                                          
        virDomainBlockStatsStruct blk_stats;
        if (virDomainBlockStats(dom, "vda", &blk_stats, sizeof(blk_stats)) == 0) {
            ctx->out_disk_rd = blk_stats.rd_bytes;
            ctx->out_disk_wr = blk_stats.wr_bytes;
            ctx->out_disk_rd_req = blk_stats.rd_req;
            ctx->out_disk_wr_req = blk_stats.wr_req;
        }

                                                  
        gchar *xml = virDomainGetXMLDesc(dom, 0);
        if (xml) {
                                                        
            gchar *tgt = strstr(xml, "<target dev='");
            if (tgt) {
                tgt += 13;                              
                gchar *end = strchr(tgt, '\'');
                if (end) {
                    gchar *iface = g_strndup(tgt, end - tgt);
                    virDomainInterfaceStatsStruct if_stats;
                    if (virDomainInterfaceStats(dom, iface, &if_stats, sizeof(if_stats)) == 0) {
                        ctx->out_net_rx = if_stats.rx_bytes;
                        ctx->out_net_tx = if_stats.tx_bytes;
                        ctx->out_net_rx_pkts = if_stats.rx_packets;
                        ctx->out_net_tx_pkts = if_stats.tx_packets;
                    }
                    g_free(iface);
                }
            }
            free(xml);
        }
    }

metrics_cleanup:
    if (dom) virDomainFree(dom);
    if (conn) virt_conn_pool_release(conn);

    g_task_return_boolean(task, TRUE);
}

                                                                    
                                               
  
                               
                             
  
                                                         
                               
  
                                                   
                                                   
                                                                       
void handle_vm_metrics_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    const gchar *vm_id;
                                                                   
    PCV_REQUIRE_PARAM(params, "vm_id", vm_id, rpc_id, server, connection);

    VmLifecycleCtx *ctx = g_new0(VmLifecycleCtx, 1);
    ctx->vm_id = g_strdup(vm_id);
    ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = g_object_ref(server);
    ctx->connection = g_object_ref(connection);

    GTask *task = pcv_drain_task_new(NULL, NULL, vm_metrics_callback, ctx);
    g_task_set_task_data(task, ctx, pcv_disp_free_lifecycle_ctx);                
    g_task_run_in_thread(task, vm_metrics_worker);
    g_object_unref(task);
}

                                                                    
                                   
  
                               
                           
  
                                                          
                                                          
  
                                                                       
                                            
  
                                                    
                                                                       
void handle_vm_vnc_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    const gchar *vm_id;
    PCV_REQUIRE_PARAM(params, "vm_id", vm_id, rpc_id, server, connection);

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "Hypervisor connection failed");
        pure_uds_server_send_response(server, connection, err); g_free(err); return;
    }
    virDomainPtr dom = pure_virt_get_domain(conn, vm_id);
    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "Entity not found");
        pure_uds_server_send_response(server, connection, err); g_free(err); virt_conn_pool_release(conn); return;
    }
    virDomainInfo info;
    if (virDomainGetInfo(dom, &info) < 0) {
        virErrorPtr verr = virGetLastError();
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, verr && verr->message ? verr->message : "Failed to read VM state");
        pure_uds_server_send_response(server, connection, err); g_free(err); virDomainFree(dom); virt_conn_pool_release(conn); return;
    }
    if (info.state != VIR_DOMAIN_RUNNING) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "VM is not running. No VNC port active.");
        pure_uds_server_send_response(server, connection, err); g_free(err); virDomainFree(dom); virt_conn_pool_release(conn); return;
    }
    gchar *xml = virDomainGetXMLDesc(dom, 0);
    if (!xml) {
        virErrorPtr verr = virGetLastError();
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, verr && verr->message ? verr->message : "Failed to read domain XML");
        pure_uds_server_send_response(server, connection, err); g_free(err); virDomainFree(dom); virt_conn_pool_release(conn); return;
    }
                                                                      
    GError *port_error = NULL;
    gchar *port_str = pcv_vnc_port_from_domain_xml(xml, &port_error);
    if (port_str) {
        JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
        JsonObject *res_obj = json_object_new();
        json_object_set_string_member(res_obj, "vnc_port", port_str);
        json_node_take_object(res_node, res_obj);
        gchar *resp = pure_rpc_build_success_response(rpc_id, res_node);
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp); g_free(port_str);
    } else {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
                         port_error ? port_error->message : "VNC Graphics adapter not found in XML");
        pure_uds_server_send_response(server, connection, err); g_free(err);
        g_clear_error(&port_error);
    }
    g_free(xml); virDomainFree(dom); virt_conn_pool_release(conn);
}
