   
                 
                                          
  
                           
                                                                                           
                                                                                                                     
                                                                             
  
                                            
   

#include "api/drain.h"
#include "modules/virt/vm_manager.h"
#include "modules/virt/virt_conn_pool.h"
#include "modules/audit/pcv_audit.h"
#include "api/ws_server.h"
#if PCV_CLUSTER_ENABLED
#include "modules/cluster/cluster_manager.h"
#endif
#include "utils/pcv_spawn.h"
#include "utils/pcv_log.h"
#include "modules/dispatcher/rpc_utils.h"
#include "modules/core/vm_state.h"
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <libvirt-gobject/libvirt-gobject.h>
#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>
#include "modules/virt/disk_image_guard.h"
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include "modules/virt/vm_manager_internal.h"

                                                                             
                            
                                              
                                                                                

                                                         
                                                        
static void lifecycle_task_data_free(LifecycleTaskData *data) {
    if (data->manager) g_object_unref(data->manager);
    g_free(data->name);
    g_free(data);
}

   
                                                             
  
                                                      
  
                                                                              
                                                                            
                                                 
                                                                     
                                                                                 
                                                                      
   
static void start_vm_thread_impl(GTask *task,
                                 gpointer source_object __attribute__((unused)),
                                 gpointer task_data,
                                 GCancellable *cancellable __attribute__((unused))) {
    LifecycleTaskData *data = (LifecycleTaskData *)task_data;

                                                   
    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Failed to acquire libvirt connection");
        return;
    }

    virDomainPtr dom = virDomainLookupByName(conn, data->name);
    if (!dom) {
        virErrorPtr e = virGetLastError();
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                "VM not found: %s", e ? e->message : data->name);
        virt_conn_pool_release(conn);
        return;
    }

    int rc = virDomainCreate(dom);
    if (rc != 0) {
        virErrorPtr e = virGetLastError();
        PCV_LOG_WARN("vm_manager", "virDomainCreate failed for '%s': %s",
                     data->name, e ? e->message : "unknown error");
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "VM operation failed — check server logs for details");
    } else {
        g_task_return_boolean(task, TRUE);
    }

    virDomainFree(dom);
    virt_conn_pool_release(conn);
}

   
                                        
                                                    
  
                                               
  
                                  
                            
                        
                              
   
void purecvisor_vm_manager_start_vm_async(PureCVisorVmManager *self,
                                          const gchar *name,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data) {
    GTask *task = pcv_drain_task_new(self, NULL, callback, user_data);
    LifecycleTaskData *data = g_new0(LifecycleTaskData, 1);
    data->manager = g_object_ref(self);
    data->name = g_strdup(name);

    g_task_set_task_data(task, data, (GDestroyNotify)lifecycle_task_data_free);
    g_task_run_in_thread(task, start_vm_thread_impl);
    g_object_unref(task);
}

                                                                             
                            
                                                           
                                                                                

   
                                                          
  
                                                    
                                                 
  
                                                                             
                                          
                                              
                                                   
                                               
                                                  
                                                   
                                              
                                               
   
static void stop_vm_thread_impl(GTask *task,
                                gpointer source_object __attribute__((unused)),
                                gpointer task_data,
                                GCancellable *cancellable __attribute__((unused))) {
    LifecycleTaskData *data = (LifecycleTaskData *)task_data;

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Failed to acquire libvirt connection");
        return;
    }

    virDomainPtr dom = virDomainLookupByName(conn, data->name);
    if (!dom) {
        virErrorPtr e = virGetLastError();
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                "VM not found: %s", e ? e->message : data->name);
        virt_conn_pool_release(conn);
        return;
    }

                                                                  
                                                 
                                                    
                                              
      
                                                        
                                           
      
                                               
                                          
                                               
    int rc = virDomainShutdown(dom);
    if (rc != 0) {
                                    
        if (virDomainDestroy(dom) < 0) {
            PCV_LOG_WARN("vm_manager", "virDomainDestroy failed for '%s'", data->name);
                                                                          
        }
        goto stop_done;
    }

                                                             
                                                  
                                 
                                                 
    for (int poll_i = 0; poll_i < 30; poll_i++) {                                
        g_usleep(G_USEC_PER_SEC);                                           
        int state = 0;
        if (virDomainGetState(dom, &state, NULL, 0) == 0) {
            if (state == VIR_DOMAIN_SHUTOFF) {                           
                PCV_LOG_INFO("vm_manager", "VM '%s' shut down gracefully after %ds",
                             data->name, poll_i + 1);
                goto stop_done;
            }
        }
    }
    PCV_LOG_WARN("vm_manager", "VM '%s' graceful shutdown timed out (30s) — force destroying",
                 data->name);
    if (virDomainDestroy(dom) < 0) {
        PCV_LOG_WARN("vm_manager", "virDomainDestroy (post-timeout) failed for '%s'", data->name);
    }

stop_done:
                                            
    g_task_return_boolean(task, TRUE);

    virDomainFree(dom);
    virt_conn_pool_release(conn);
}

   
                                       
                                                   
  
                                               
   
void purecvisor_vm_manager_stop_vm_async(PureCVisorVmManager *self,
                                         const gchar *name,
                                         GAsyncReadyCallback callback,
                                         gpointer user_data) {
    GTask *task = pcv_drain_task_new(self, NULL, callback, user_data);
    LifecycleTaskData *data = g_new0(LifecycleTaskData, 1);
    data->manager = g_object_ref(self);
    data->name = g_strdup(name);

    g_task_set_task_data(task, data, (GDestroyNotify)lifecycle_task_data_free);
    g_task_run_in_thread(task, stop_vm_thread_impl);
    g_object_unref(task);
}

                                                                            
                                   
  
                                       
                                                         
                                                         
                                        
                                                                               

   
                      
                                 
                                             
   
typedef struct {
    gchar *vm_name;                     
    guint target_value;                                
} ResourceTuningData;

                             
                                                          
static void resource_tuning_data_free(ResourceTuningData *data) {
    if (data) {
        g_free(data->vm_name);
        g_free(data);
    }
}

   
                          
                                   
                                                           
  
                                                
                                         
  
                                        
   
static void set_memory_thread_impl(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    ResourceTuningData *data = (ResourceTuningData *)task_data;

                                  
    virConnectPtr raw_conn = virt_conn_pool_acquire();
    if (!raw_conn) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to open raw libvirt connection");
        return;
    }

                         
    virDomainPtr raw_domain = virDomainLookupByName(raw_conn, data->vm_name);
    if (!raw_domain) {
        virt_conn_pool_release(raw_conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "VM '%s' not found", data->vm_name);
        return;
    }

                                                 
      
                               
                                                          
                                                        
      
                                 
                                                
                                                    
      
              
                                            
                                 
                                     
    guint memory_kb = data->target_value * 1024;                                     
    int ret = virDomainSetMemoryFlags(raw_domain, memory_kb, VIR_DOMAIN_AFFECT_LIVE | VIR_DOMAIN_AFFECT_CONFIG);
    
    if (ret < 0) {
        virErrorPtr vir_err = virGetLastError();
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, 
                                "Memory tuning failed: %s", vir_err ? vir_err->message : "Unknown error");
    } else {
        g_task_return_boolean(task, TRUE);
    }

                  
    virDomainFree(raw_domain);
    virt_conn_pool_release(raw_conn);
}

   
                                          
                       
  
                                                      
  
                                  
   
void purecvisor_vm_manager_set_memory_async(PureCVisorVmManager *self, const gchar *name, guint memory_mb, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data) {
    GTask *task = pcv_drain_task_new(self, cancellable, callback, user_data);
    ResourceTuningData *data = g_new0(ResourceTuningData, 1);
    data->vm_name = g_strdup(name);
    data->target_value = memory_mb;
    
    g_task_set_task_data(task, data, (GDestroyNotify)resource_tuning_data_free);
    g_task_run_in_thread(task, set_memory_thread_impl);
    g_object_unref(task);
}

                
                                       
gboolean purecvisor_vm_manager_set_memory_finish(PureCVisorVmManager *self, GAsyncResult *res, GError **error) {
    return g_task_propagate_boolean(G_TASK(res), error);
}

   
                        
                                      
                                               
  
                                                
                                      
  
                                          
                               
   
static void set_vcpu_thread_impl(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    ResourceTuningData *data = (ResourceTuningData *)task_data;

                                  
    virConnectPtr raw_conn = virt_conn_pool_acquire();
    if (!raw_conn) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to open raw libvirt connection");
        return;
    }

    virDomainPtr raw_domain = virDomainLookupByName(raw_conn, data->vm_name);
    if (!raw_domain) {
        virt_conn_pool_release(raw_conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "VM '%s' not found", data->vm_name);
        return;
    }

                                          
    int ret = virDomainSetVcpusFlags(raw_domain, data->target_value, VIR_DOMAIN_AFFECT_LIVE | VIR_DOMAIN_AFFECT_CONFIG);
    
    if (ret < 0) {
        virErrorPtr vir_err = virGetLastError();
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, 
                                "vCPU tuning failed: %s", vir_err ? vir_err->message : "Unknown error");
    } else {
        g_task_return_boolean(task, TRUE);
    }

    virDomainFree(raw_domain);
    virt_conn_pool_release(raw_conn);
}

   
                                        
                          
  
                                                    
  
                              
   
void purecvisor_vm_manager_set_vcpu_async(PureCVisorVmManager *self, const gchar *name, guint vcpu_count, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data) {
    GTask *task = pcv_drain_task_new(self, cancellable, callback, user_data);
    ResourceTuningData *data = g_new0(ResourceTuningData, 1);
    data->vm_name = g_strdup(name);
    data->target_value = vcpu_count;
    
    g_task_set_task_data(task, data, (GDestroyNotify)resource_tuning_data_free);
    g_task_run_in_thread(task, set_vcpu_thread_impl);
    g_object_unref(task);
}

                 
                                          
gboolean purecvisor_vm_manager_set_vcpu_finish(PureCVisorVmManager *self, GAsyncResult *res, GError **error) {
    return g_task_propagate_boolean(G_TASK(res), error);
}

                                                                             
                  
  
                               
                                                        
                                           
                                                                                

                         
typedef struct {
    gchar   *name;                     
    gint     new_size_gb;                      
    gchar   *target;                                   
    PcvVmLock *lock;                                                            
} ResizeDiskData;

                                                                 
                                                                  
static void resize_disk_data_free(ResizeDiskData *d) {
                                                                 
                                                   
                                                      
    pcv_vm_lock_release(d->lock);
    g_free(d->name);
    g_free(d->target);
    g_free(d);
}

   
                                                                 
                                                    
                          
                                                              
   
static void
audit_resize_disk_success(ResizeDiskData *d, const gchar *actor)
{
    gchar *target = g_strdup_printf("%s:%s", d->name, d->target ? d->target : "vda");
    gchar *job_id = g_strdup_printf("vm.resize_disk:%s", target);
    pcv_audit_log(actor, "vm.resize_disk", target, "ok", 0, 0, "local");
    pcv_ws_broadcast_job_complete_mt(job_id, "vm.resize_disk",
                                     "completed", NULL);
    g_free(job_id);
    g_free(target);
}

   
                                                                 
                                                    
                          
                                                                       
                                                      
   
static void
audit_resize_disk_failure(ResizeDiskData *d, const gchar *error_msg, const gchar *actor)
{
    gchar *target = g_strdup_printf("%s:%s", d->name, d->target ? d->target : "vda");
    gchar *job_id = g_strdup_printf("vm.resize_disk:%s", target);
    pcv_audit_log(actor, "vm.resize_disk", target, "fail", PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
    pcv_ws_broadcast_job_complete_mt(job_id, "vm.resize_disk",
                                     "failed", error_msg ? error_msg : "unknown");
    g_free(job_id);
    g_free(target);
}

                                                                        
                                                               
static xmlNodePtr
_vm_disk_child(xmlNodePtr disk, const gchar *name)
{
    for (xmlNodePtr child = disk ? disk->children : NULL; child; child = child->next) {
        if (child->type == XML_ELEMENT_NODE && xmlStrcmp(child->name, BAD_CAST name) == 0)
            return child;
    }
    return NULL;
}

                                                                       
  
                           
                                                                                 
                                                            
                                                                       
                                                                         
                                                                
                                                      
                                                                  
                                                                         
             
                                                                              
                                                                  
  
                                                     
                                                 
gboolean
pcv_vm_disk_source_for_target(const gchar *domain_xml, const gchar *target_dev,
                              gchar **source_out, PcvVmDiskSourceKind *kind_out,
                              GError **error)
{
    if (source_out) *source_out = NULL;
    if (!domain_xml || !target_dev || !*target_dev || !source_out || !kind_out) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "domain XML and disk target are required");
        return FALSE;
    }

    xmlDocPtr doc = xmlReadMemory(domain_xml, (int)strlen(domain_xml), "domain.xml", NULL,
                                  XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    if (!doc) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "Cannot parse domain XML");
        return FALSE;
    }

    xmlNodePtr root = xmlDocGetRootElement(doc);
    xmlNodePtr devices = NULL;
    for (xmlNodePtr child = root ? root->children : NULL; child; child = child->next) {
        if (child->type == XML_ELEMENT_NODE && xmlStrcmp(child->name, BAD_CAST "devices") == 0) {
            devices = child;
            break;
        }
    }

    gboolean matched = FALSE;
    gchar *source = NULL;
    PcvVmDiskSourceKind kind = PCV_VM_DISK_SOURCE_FILE;
    const gchar *reject = NULL;                                   
    for (xmlNodePtr disk = devices ? devices->children : NULL; disk; disk = disk->next) {
        if (disk->type != XML_ELEMENT_NODE || xmlStrcmp(disk->name, BAD_CAST "disk") != 0)
            continue;
        xmlNodePtr target = _vm_disk_child(disk, "target");
        xmlChar *dev = target ? xmlGetProp(target, BAD_CAST "dev") : NULL;
        gboolean same = dev && xmlStrcmp(dev, BAD_CAST target_dev) == 0;
        if (dev) xmlFree(dev);
        if (!same) continue;

        matched = TRUE;
                                                                                  
        xmlChar *device = xmlGetProp(disk, BAD_CAST "device");
        gboolean is_disk = !device || xmlStrcmp(device, BAD_CAST "disk") == 0;
        if (device) xmlFree(device);
        if (!is_disk) {
            reject = "is not a disk device";
            break;
        }

        xmlNodePtr src = _vm_disk_child(disk, "source");
        xmlChar *dev_path = src ? xmlGetProp(src, BAD_CAST "dev") : NULL;
        xmlChar *file_path = src ? xmlGetProp(src, BAD_CAST "file") : NULL;
        if (dev_path && dev_path[0]) {
            source = g_strdup((const gchar *)dev_path);
            kind = g_str_has_prefix(source, "/dev/zvol/") ? PCV_VM_DISK_SOURCE_ZVOL
                                                         : PCV_VM_DISK_SOURCE_BLOCK;
        } else if (file_path && file_path[0]) {
            source = g_strdup((const gchar *)file_path);
            kind = PCV_VM_DISK_SOURCE_FILE;
        } else {
            reject = "has no local file or block source";
        }
        if (dev_path) xmlFree(dev_path);
        if (file_path) xmlFree(file_path);
        break;                                                       
    }
    xmlFreeDoc(doc);

                                                                      
    if (source && kind == PCV_VM_DISK_SOURCE_ZVOL) {
        const gchar *dataset = source + strlen("/dev/zvol/");
        if (!*dataset || dataset[0] == '-' || dataset[0] == '/')
            reject = "has an invalid zvol path";
    }

    if (!matched || reject) {
        g_free(source);
        if (!matched)
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                        "Disk target '%s' not found", target_dev);
        else
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                        "Disk target '%s' %s", target_dev, reject);
        return FALSE;
    }

    *source_out = source;
    *kind_out = kind;
    return TRUE;
}

                                                    
                                                                                 
                                                                        
                                                       
                                               
                                                        
gboolean
pcv_vm_disk_resize_check(guint64 current_bytes, gint new_size_gb, GError **error)
{
    if (new_size_gb <= 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "new_size_gb must be > 0");
        return FALSE;
    }
    guint64 new_bytes = (guint64)new_size_gb << 30;
    if (new_bytes < current_bytes) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Shrinking a disk is not allowed (current %" G_GUINT64_FORMAT
                    " bytes, requested %d GiB)", current_bytes, new_size_gb);
        return FALSE;
    }
    return TRUE;
}

                                                                         
                                                    
                                                        
static void
resize_disk_fail(GTask *task, ResizeDiskData *d, const gchar *msg)
{
    audit_resize_disk_failure(d, msg, pcv_drain_task_actor(task));
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", msg);
}

   
                      
                                  
  
                                                
                                                      
                                                    
                      
  
      
                    
                                                                 
                                                                    
                                                                                                
                                                         
   
static void resize_disk_thread(GTask *task, gpointer source_object __attribute__((unused)),
                                gpointer task_data, GCancellable *cancel __attribute__((unused)))
{
    ResizeDiskData *d = (ResizeDiskData *)task_data;
    GError *error = nullptr;

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        audit_resize_disk_failure(d, "Failed to acquire libvirt connection", pcv_drain_task_actor(task));
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
            "Failed to acquire libvirt connection");
        return;
    }

    virDomainPtr dom = virDomainLookupByName(conn, d->name);
    if (!dom) {
        gchar *msg = g_strdup_printf("VM '%s' not found", d->name);
        audit_resize_disk_failure(d, msg, pcv_drain_task_actor(task));
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
            "VM '%s' not found", d->name);
        virt_conn_pool_release(conn);
        g_free(msg);
        return;
    }

                                                             
      
                        
                                                            
                                                            
      
                     
                                                                             
                                                            
                               
      
                   
                                                       
                                                                               
                                                 
                                                       
    gchar *xml = virDomainGetXMLDesc(dom, 0);
    gchar *disk_source = nullptr;
    PcvVmDiskSourceKind kind = PCV_VM_DISK_SOURCE_FILE;
    GError *select_err = nullptr;
    gboolean selected = pcv_vm_disk_source_for_target(xml, d->target, &disk_source,
                                                      &kind, &select_err);
    g_autofree gchar *disk_format = pcv_disk_image_driver_format(xml, d->target);
    free(xml);                                              
    if (!selected || kind == PCV_VM_DISK_SOURCE_BLOCK) {
        gchar *msg = selected
            ? g_strdup_printf("Disk '%s' of VM '%s' is a non-zvol block device (%s); "
                              "storage resize is not supported", d->target, d->name, disk_source)
            : g_strdup_printf("Cannot resolve disk '%s' of VM '%s': %s", d->target, d->name,
                              select_err ? select_err->message : "unknown");
        resize_disk_fail(task, d, msg);
        g_free(msg);
        g_clear_error(&select_err);
        g_free(disk_source);
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        return;
    }

                                                        
                                                           
    {
        virDomainBlockInfo binfo;
        GError *size_err = nullptr;
        gchar *msg = nullptr;
        if (virDomainGetBlockInfo(dom, d->target, &binfo, 0) < 0) {
            virErrorPtr e = virGetLastError();
            msg = g_strdup_printf("Cannot read current size of disk '%s' of VM '%s': %s",
                                  d->target, d->name, e && e->message ? e->message : "unknown");
        } else if (!pcv_vm_disk_resize_check(binfo.capacity, d->new_size_gb, &size_err)) {
            msg = g_strdup_printf("Disk '%s' of VM '%s': %s", d->target, d->name,
                                  size_err ? size_err->message : "resize rejected");
        }
        g_clear_error(&size_err);
        if (msg) {
            resize_disk_fail(task, d, msg);
            g_free(msg);
            g_free(disk_source);
            virDomainFree(dom);
            virt_conn_pool_release(conn);
            return;
        }
    }
    gboolean is_zvol = (kind == PCV_VM_DISK_SOURCE_ZVOL);

                      
    if (is_zvol) {
                                                                      
        const gchar *dataset = disk_source + strlen("/dev/zvol/");
        gchar *size_str = g_strdup_printf("%dG", d->new_size_gb);
        gchar *prop_str = g_strdup_printf("volsize=%s", size_str);
        const gchar *argv[] = {"zfs", "set", prop_str, (gchar *)dataset, NULL};
        gchar *std_err = nullptr;

        if (!pcv_spawn_sync(argv, NULL, &std_err, &error)) {
            const gchar *err_msg = error ? error->message : (std_err ? std_err : "unknown");
            gchar *msg = g_strdup_printf("zfs set volsize failed: %s", err_msg);
            audit_resize_disk_failure(d, msg, pcv_drain_task_actor(task));
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                "zfs set volsize failed: %s",
                err_msg);
            if (error) g_error_free(error);
            g_free(std_err);
            g_free(size_str);
            g_free(prop_str);
            g_free(disk_source);
            virDomainFree(dom);
            virt_conn_pool_release(conn);
            g_free(msg);
            return;
        }
        g_free(std_err);
        g_free(size_str);
        g_free(prop_str);
    } else {
                                                                                        
        if (!pcv_disk_image_resize_file(disk_source, disk_format, d->new_size_gb, &error)) {
            gchar *msg = g_strdup_printf("qemu-img resize failed: %s", error->message);
            audit_resize_disk_failure(d, msg, pcv_drain_task_actor(task));
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                "qemu-img resize failed: %s",
                error->message);
            g_error_free(error);
            g_free(disk_source);
            virDomainFree(dom);
            virt_conn_pool_release(conn);
            g_free(msg);
            return;
        }
    }

                                                          
      
                                 
                                               
                                                    
                                          
                                                 
      
                    
                                                          
                                  
      
                
                                             
                              
                                      
    virDomainInfo info;
    if (virDomainGetInfo(dom, &info) == 0 && info.state == VIR_DOMAIN_RUNNING) {
        const gchar *target = d->target ? d->target : "vda";
        unsigned long long new_size_kb = (unsigned long long)d->new_size_gb * 1024ULL * 1024ULL;
        int rc = virDomainBlockResize(dom, target, new_size_kb, 0);
        if (rc < 0) {
            virErrorPtr e = virGetLastError();
            PCV_LOG_WARN("vm_manager", "virDomainBlockResize failed for '%s': %s",
                         d->name, e ? e->message : "unknown");
                                                      
        }
    }

    PCV_LOG_INFO("vm_manager", "VM '%s': disk resized to %dG (%s)",
                  d->name, d->new_size_gb, is_zvol ? "zvol" : "qcow2");

    g_free(disk_source);
    virDomainFree(dom);
    virt_conn_pool_release(conn);
    audit_resize_disk_success(d, pcv_drain_task_actor(task));
    g_task_return_boolean(task, TRUE);
}

   
                             
                                        
                        
  
                                                 
                                            
                                                                  
   
void purecvisor_vm_resize_disk(const gchar *name, gint new_size_gb, const gchar *target,
                                PcvVmLock *op_lock) {
    ResizeDiskData *d = g_new0(ResizeDiskData, 1);
    d->name = g_strdup(name);
    d->new_size_gb = new_size_gb;
    d->target = target ? g_strdup(target) : g_strdup("vda");
    d->lock = op_lock;                                            

    GTask *task = pcv_drain_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(task, d, (GDestroyNotify)resize_disk_data_free);
    g_task_run_in_thread(task, resize_disk_thread);
    g_object_unref(task);
}
