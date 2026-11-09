   
                            
                                         
  
                           
                                                                                                         
                                                                                                                           
                                                                             
  
                                                     
   

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
#include "modules/dispatcher/guest_agent_exec.h"
#include "modules/audit/pcv_audit.h"
#include "modules/virt/virt_conn_pool.h"
#include "modules/virt/vm_manager.h"
#include "modules/core/vm_state.h"
#include "purecvisor/pcv_handler_util.h"
#include "api/ws_server.h"
#include "utils/pcv_spawn.h"
#include "modules/dispatcher/handler_vm_internal.h"


                                                                    
                                                                   
  
                                                         
                                                
  
                                                                
                                                       
                                                     
  
         
                            
                                           
                                   
  
          
                                  
                 
                                                                     
                                                                      
                                                                                   
  
          
                        
                           
                             
                                                                       

                                
typedef struct {
    gchar *vm_name;                  
    gchar *target;                                   
    gint   new_size_gb;                  
    PcvVmLock *lock;                                                
} DiskLiveResizeCtx;

                                                                              
                                                         
                                                                   
static void free_disk_live_resize_ctx(gpointer data) {
    if (!data) return;
    DiskLiveResizeCtx *ctx = (DiskLiveResizeCtx *)data;
    g_clear_pointer(&ctx->lock, pcv_vm_lock_release);
    g_free(ctx->vm_name);
    g_free(ctx->target);
    g_free(ctx);
}

                                                                      
                                                            
                                                                 
static void
audit_disk_live_resize_success(DiskLiveResizeCtx *ctx, const gchar *actor)
{
    gchar *target = g_strdup_printf("%s:%s", ctx->vm_name, ctx->target);
    gchar *job_id = g_strdup_printf("vm.disk.live_resize:%s", target);
    pcv_audit_log(actor, "vm.disk.live_resize", target, "ok", 0, 0, "local");
    pcv_ws_broadcast_job_complete_mt(job_id, "vm.disk.live_resize",
                                     "completed", NULL);
    g_free(job_id);
    g_free(target);
}

                                                                         
                                                              
static void
audit_disk_live_resize_failure(DiskLiveResizeCtx *ctx, const gchar *error_msg, const gchar *actor)
{
    gchar *target = g_strdup_printf("%s:%s", ctx->vm_name, ctx->target);
    gchar *job_id = g_strdup_printf("vm.disk.live_resize:%s", target);
    pcv_audit_log(actor, "vm.disk.live_resize", target, "fail", PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
    pcv_ws_broadcast_job_complete_mt(job_id, "vm.disk.live_resize",
                                     "failed", error_msg ? error_msg : "unknown");
    g_free(job_id);
    g_free(target);
}

   
                                                                              
                                                    
                                                             
                                    
                                  
                                                                      
                                                                      
                                                                 
                                                                                
                                                           
                                                                        
                     
   
static void vm_disk_live_resize_worker(GTask *task, gpointer source_obj,
                                        gpointer task_data, GCancellable *cancellable)
{
    (void)source_obj;
    (void)cancellable;

    DiskLiveResizeCtx *ctx = (DiskLiveResizeCtx *)task_data;

                                                     
    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        audit_disk_live_resize_failure(ctx, "Failed to connect to libvirt", pcv_drain_task_actor(task));
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
            "Failed to connect to libvirt");
        return;
    }

    virDomainPtr dom = pure_virt_get_domain(conn, ctx->vm_name);
    if (!dom) {
        gchar *msg = g_strdup_printf("VM '%s' not found", ctx->vm_name);
        audit_disk_live_resize_failure(ctx, msg, pcv_drain_task_actor(task));
        virt_conn_pool_release(conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
            "VM '%s' not found", ctx->vm_name);
        g_free(msg);
        return;
    }

                                                                     
    gchar *fail_msg = NULL;
    gchar *disk_source = NULL;
    PcvVmDiskSourceKind kind = PCV_VM_DISK_SOURCE_FILE;
    {
        GError *gerr = NULL;
        char *xml = virDomainGetXMLDesc(dom, 0);
        if (!pcv_vm_disk_source_for_target(xml, ctx->target, &disk_source, &kind, &gerr)) {
            fail_msg = g_strdup_printf("Cannot resolve disk '%s': %s", ctx->target,
                                       gerr ? gerr->message : "unknown");
        } else {
            virDomainBlockInfo binfo;
            if (virDomainGetBlockInfo(dom, ctx->target, &binfo, 0) < 0) {
                virErrorPtr e = virGetLastError();
                fail_msg = g_strdup_printf("Cannot read current size of disk '%s': %s",
                                           ctx->target,
                                           e && e->message ? e->message : "unknown");
            } else if (!pcv_vm_disk_resize_check(binfo.capacity, ctx->new_size_gb, &gerr)) {
                fail_msg = g_strdup_printf("Disk '%s': %s", ctx->target,
                                           gerr ? gerr->message : "resize rejected");
            }
        }
        free(xml);
        g_clear_error(&gerr);
    }

                                                          
                                                                      
    if (!fail_msg && kind == PCV_VM_DISK_SOURCE_ZVOL) {
        const gchar *dataset = disk_source + strlen("/dev/zvol/");
        gchar *volsize_arg = g_strdup_printf("volsize=%dG", ctx->new_size_gb);
        const gchar *zfs_argv[] = {"zfs", "set", volsize_arg, dataset, NULL};
        GError *zfs_err = NULL;
        gchar *zfs_stderr = NULL;
        if (!pcv_spawn_sync(zfs_argv, NULL, &zfs_stderr, &zfs_err)) {
            fail_msg = g_strdup_printf("zfs set volsize failed for '%s': %s", dataset,
                                       zfs_err ? zfs_err->message
                                               : (zfs_stderr ? zfs_stderr : "unknown"));
        }
        g_clear_error(&zfs_err);
        g_free(zfs_stderr);
        g_free(volsize_arg);
    }
    g_free(disk_source);

    if (fail_msg) {
        audit_disk_live_resize_failure(ctx, fail_msg, pcv_drain_task_actor(task));
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", fail_msg);
        g_free(fail_msg);
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        return;
    }

                                      
                                                             
                                                                          
                                                                   
    unsigned long long new_size_bytes =
        (unsigned long long)ctx->new_size_gb * 1024ULL * 1024ULL * 1024ULL;

    if (virDomainBlockResize(dom, ctx->target, new_size_bytes,
                             VIR_DOMAIN_BLOCK_RESIZE_BYTES) < 0) {
        virErrorPtr e = virGetLastError();
        const gchar *err_msg = e ? e->message : "Unknown";
        audit_disk_live_resize_failure(ctx, err_msg, pcv_drain_task_actor(task));
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
            "Block resize failed: %s", err_msg);
    } else {
        audit_disk_live_resize_success(ctx, pcv_drain_task_actor(task));
        g_task_return_boolean(task, TRUE);
    }

    virDomainFree(dom);
    virt_conn_pool_release(conn);
}

   
                                                                                    
  
                                                                  
                                                         
                                                                     
                          
                                                                  
                                              
  
                                                       
                                                      
                                                    
                                                 
                                                   
                  
  
                                                                       
                                                     
                                                                      
                                                                    
                                                                       
                                               
                                             
                                                                
                                                                 
                                                        
                                                                 
                                                                  
                                            
                                                        
                                                            
                                                                   
                                     
   
void handle_vm_disk_live_resize_request(JsonObject *params, const gchar *rpc_id,
                                         UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;
    const gchar *target = json_object_has_member(params, "target")
        ? json_object_get_string_member(params, "target") : NULL;
    gint new_size_gb = json_object_has_member(params, "new_size_gb")
        ? (gint)json_object_get_int_member(params, "new_size_gb") : 0;

    if (!name || !target || new_size_gb <= 0) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing or invalid params: name, target required, new_size_gb must be > 0");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

                                                                           
                                                                            
    gchar *lock_err = NULL;
    PcvVmLock *op_lock = pcv_vm_lock_acquire(name, VM_OP_TUNING, &lock_err);
    if (!op_lock) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
                       lock_err ? lock_err : "VM busy (operation in progress)");
        pure_uds_server_send_response(server, connection, e);
        g_free(e); g_free(lock_err);
        return;
    }

                                           
      
                                              
                                                       
                                                         
                                               
                                              
    JsonNode *accepted = json_node_new(JSON_NODE_OBJECT);
    JsonObject *accepted_obj = json_object_new();
    json_object_set_string_member(accepted_obj, "status", "accepted");
    json_object_set_string_member(accepted_obj, "vm", name);
    json_object_set_string_member(accepted_obj, "target", target);
    json_object_set_int_member(accepted_obj, "new_size_gb", new_size_gb);
    json_node_take_object(accepted, accepted_obj);

    gchar *resp = pure_rpc_build_success_response(rpc_id, accepted);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);

                                                
    DiskLiveResizeCtx *ctx = g_new0(DiskLiveResizeCtx, 1);
    ctx->vm_name = g_strdup(name);
    ctx->target = g_strdup(target);
    ctx->new_size_gb = new_size_gb;
    ctx->lock = op_lock;                                               

    GTask *task = pcv_drain_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(task, ctx, free_disk_live_resize_ctx);
    g_task_run_in_thread(task, vm_disk_live_resize_worker);
    g_object_unref(task);
}

                                                                    
                                          
  
                                                          
  
         
                                      
                                                
                                                       
                                                       
                                             
                                             
  
          
                                  
                    
                           
  
                                                         
                                          
                                                                          
                          
                                                                       
void handle_vm_blkio_set(JsonObject *params, const gchar *rpc_id,
                          UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;
    const gchar *device = json_object_has_member(params, "device")
        ? json_object_get_string_member(params, "device") : NULL;

    if (!name || !device) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing required parameters: name, device");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    gint64 read_bytes_sec = json_object_has_member(params, "read_bytes_sec")
        ? json_object_get_int_member(params, "read_bytes_sec") : 0;
    gint64 write_bytes_sec = json_object_has_member(params, "write_bytes_sec")
        ? json_object_get_int_member(params, "write_bytes_sec") : 0;
    gint64 read_iops_sec = json_object_has_member(params, "read_iops_sec")
        ? json_object_get_int_member(params, "read_iops_sec") : 0;
    gint64 write_iops_sec = json_object_has_member(params, "write_iops_sec")
        ? json_object_get_int_member(params, "write_iops_sec") : 0;

    if (read_bytes_sec <= 0 && write_bytes_sec <= 0 &&
        read_iops_sec <= 0 && write_iops_sec <= 0) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "At least one I/O limit (read/write bytes_sec or iops_sec) must be > 0");
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

                                                          
    virTypedParameter typed_params[4];
    int nparams = 0;

    if (read_bytes_sec > 0) {
        strncpy(typed_params[nparams].field, VIR_DOMAIN_BLOCK_IOTUNE_READ_BYTES_SEC,
                VIR_TYPED_PARAM_FIELD_LENGTH);
        typed_params[nparams].type = VIR_TYPED_PARAM_ULLONG;
        typed_params[nparams].value.ul = (unsigned long long)read_bytes_sec;
        nparams++;
    }
    if (write_bytes_sec > 0) {
        strncpy(typed_params[nparams].field, VIR_DOMAIN_BLOCK_IOTUNE_WRITE_BYTES_SEC,
                VIR_TYPED_PARAM_FIELD_LENGTH);
        typed_params[nparams].type = VIR_TYPED_PARAM_ULLONG;
        typed_params[nparams].value.ul = (unsigned long long)write_bytes_sec;
        nparams++;
    }
    if (read_iops_sec > 0) {
        strncpy(typed_params[nparams].field, VIR_DOMAIN_BLOCK_IOTUNE_READ_IOPS_SEC,
                VIR_TYPED_PARAM_FIELD_LENGTH);
        typed_params[nparams].type = VIR_TYPED_PARAM_ULLONG;
        typed_params[nparams].value.ul = (unsigned long long)read_iops_sec;
        nparams++;
    }
    if (write_iops_sec > 0) {
        strncpy(typed_params[nparams].field, VIR_DOMAIN_BLOCK_IOTUNE_WRITE_IOPS_SEC,
                VIR_TYPED_PARAM_FIELD_LENGTH);
        typed_params[nparams].type = VIR_TYPED_PARAM_ULLONG;
        typed_params[nparams].value.ul = (unsigned long long)write_iops_sec;
        nparams++;
    }

                                                          
    gchar *lock_err = NULL;
    PcvVmLock *op_lock = pcv_vm_lock_acquire(name, VM_OP_TUNING, &lock_err);
    if (!op_lock) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
                       lock_err ? lock_err : "VM busy (operation in progress)");
        pure_uds_server_send_response(server, connection, e);
        g_free(e); g_free(lock_err);
        virDomainFree(dom); virt_conn_pool_release(conn);
        return;
    }

    int rc = virDomainSetBlockIoTune(dom, device, typed_params, nparams,
                 VIR_DOMAIN_AFFECT_LIVE | VIR_DOMAIN_AFFECT_CONFIG);

    if (rc < 0) {
        virErrorPtr e = virGetLastError();
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            e ? e->message : "Failed to set block I/O tune");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
    } else {
        JsonObject *obj = json_object_new();
        json_object_set_string_member(obj, "vm", name);
        json_object_set_string_member(obj, "device", device);
        json_object_set_int_member(obj, "read_bytes_sec", read_bytes_sec);
        json_object_set_int_member(obj, "write_bytes_sec", write_bytes_sec);
        json_object_set_int_member(obj, "read_iops_sec", read_iops_sec);
        json_object_set_int_member(obj, "write_iops_sec", write_iops_sec);
        JsonNode *node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(node, obj);
        gchar *resp = pure_rpc_build_success_response(rpc_id, node);
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
    }

    pcv_vm_lock_release(op_lock);                               
    virDomainFree(dom);
    virt_conn_pool_release(conn);
}

                                                                    
                                          
  
                                                     
  
         
                             
                                       
  
          
                                                              
                                                  
  
                                                          
                                                                    
                                                    
                                                                          
                                                                       
void handle_vm_blkio_get(JsonObject *params, const gchar *rpc_id,
                          UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;
    const gchar *device = json_object_has_member(params, "device")
        ? json_object_get_string_member(params, "device") : NULL;

    if (!name || !device) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing required parameters: name, device");
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

                                                  
    int nparams = 0;
    if (virDomainGetBlockIoTune(dom, device, NULL, &nparams, 0) < 0) {
        virErrorPtr e = virGetLastError();
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            e ? e->message : "Failed to query block I/O tune params count");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        return;
    }

    if (nparams <= 0) {
                                                       
        JsonObject *obj = json_object_new();
        json_object_set_string_member(obj, "device", device);
        json_object_set_string_member(obj, "status", "no_iotune_params");
        JsonNode *node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(node, obj);
        gchar *resp = pure_rpc_build_success_response(rpc_id, node);
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        return;
    }

    virTypedParameterPtr blk_params = g_new0(virTypedParameter, nparams);

    int rc = virDomainGetBlockIoTune(dom, device, blk_params, &nparams, 0);
    if (rc < 0) {
        virErrorPtr e = virGetLastError();
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            e ? e->message : "Failed to get block I/O tune");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        g_free(blk_params);
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        return;
    }

                                      
    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "vm", name);
    json_object_set_string_member(obj, "device", device);

    for (int i = 0; i < nparams; i++) {
        if (blk_params[i].type == VIR_TYPED_PARAM_ULLONG) {
            json_object_set_int_member(obj, blk_params[i].field,
                                        (gint64)blk_params[i].value.ul);
        } else if (blk_params[i].type == VIR_TYPED_PARAM_UINT) {
            json_object_set_int_member(obj, blk_params[i].field,
                                        (gint64)blk_params[i].value.ui);
        } else if (blk_params[i].type == VIR_TYPED_PARAM_LLONG) {
            json_object_set_int_member(obj, blk_params[i].field,
                                        blk_params[i].value.l);
        }
    }

    g_free(blk_params);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, obj);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);

    virDomainFree(dom);
    virt_conn_pool_release(conn);
}
