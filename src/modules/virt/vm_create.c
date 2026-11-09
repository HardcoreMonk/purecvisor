   
                    
                                                  
  
                           
                                                                                           
                                                                                                                     
                                                                             
  
                                                     
   

#include "api/drain.h"
#include "modules/virt/vm_manager.h"
#include "modules/virt/virt_conn_pool.h"
#include "modules/virt/vm_config_builder.h"
#include "utils/pcv_config.h"
#include "modules/network/pcv_qos.h"
#include "modules/network/network_manager.h"
#include "modules/network/dpdk_manager.h"
#include "modules/network/vpc/vpc_manager.h"
#if PCV_CLUSTER_ENABLED
#include "modules/cluster/cluster_manager.h"
#endif
#include "utils/pcv_spawn.h"
#include "utils/pcv_zfs_exec.h"
#include "utils/pcv_log.h"
#include "utils/pcv_validate.h"
#include "modules/resource/resource_registry.h"
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


                                                                             
                              
  
         
                                      
                                                
                                            
                                       
                                       
                                                                                

   
                    
                             
                                        
   
typedef struct {
    PureCVisorVmManager *manager;                                   
    gchar *name;                                                     
    gint vcpu;                                       
    gint ram_mb;                                       
    gint disk_size_gb;                                                   
    gchar *disk_path;                                          
    gchar *iso_path;                                                     
    gchar *network_bridge;                                         
    gint   vlan_id;                                                   
    gchar *nic_type;                                                               
    gchar *pci_addr;                                                      
    gint boot_mode;                                                          
    gboolean tpm;                                                
    gint cpu_mode;                                                                                              
    gboolean hugepages;                                           
    gchar *storage_type;                                              
    gchar *storage_pool;                                                         
    gchar *image_dir;                                               
    gchar *base_image;                                                                     
    PcvResPin base_pin;                                                                             
    gchar *owner;                                                    
    gchar *network_mode;                                                                                      
    gchar *tenant;                                                                                    
    gboolean  qos_required;                                                                   
    PcvQosSla qos_sla;                                                                    
} CreateVmTaskData;

                                                        
                                                        
                                                                   
static void create_vm_task_data_free(CreateVmTaskData *data) {
    if (data->manager) g_object_unref(data->manager);
    g_free(data->name);
    g_free(data->disk_path);
    g_free(data->iso_path);
    g_free(data->network_bridge);
    g_free(data->nic_type);
    g_free(data->pci_addr);
    g_free(data->storage_type);
    g_free(data->storage_pool);
    g_free(data->image_dir);
    g_free(data->base_image);
    g_free(data->owner);
    g_free(data->network_mode);
    g_free(data->tenant);
    g_free(data);
}

                                                                      
                                                    
                                                                   
                                                    
                                                  
                                                                  
                           
gboolean
purecvisor_vm_sriov_pci_parse(const gchar *pci, guint out[4])
{
    out[0] = out[1] = out[2] = out[3] = 0;
    if (!pci || !pcv_validate_pci_addr(pci))
        return FALSE;
    guint f[4] = {0, 0, 0, 0};
    if (sscanf(pci, "%x:%x:%x.%x", &f[0], &f[1], &f[2], &f[3]) != 4)
        return FALSE;
    for (guint i = 0; i < 4; i++)
        out[i] = f[i];
    return TRUE;
}

                                                                     
                                                                
                                                                       
                                                            
                                                                      
                                                             
                                                
                                                         
                                                          
                                                 
                        
gboolean
purecvisor_vm_sriov_nic_check(const gchar *nic_type, const gchar *network_bridge,
                              const gchar *network_mode, GError **error)
{
    if (g_strcmp0(nic_type, "sriov") != 0)
        return TRUE;
    if (!network_bridge || !*network_bridge) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            PCV_VM_SRIOV_NO_BRIDGE_MSG);
        return FALSE;
    }
    if (g_strcmp0(network_mode, "tenant-overlay") == 0) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            PCV_VM_SRIOV_OVERLAY_MSG);
        return FALSE;
    }
    return TRUE;
}

                                                                       
                                                                            
                                               
                                
static gboolean
_create_vm_rollback_disk(const gchar *disk_path, const gchar *vm_name,
                         const gchar *reason)
{
    if (!disk_path || !*disk_path)
        return TRUE;
    if (g_str_has_prefix(disk_path, "/dev/zvol/")) {
        g_autoptr(GError) rollback_error = NULL;
        const gboolean removed = pcv_zfs_destroy_settling("-f", disk_path + strlen("/dev/zvol/"), &rollback_error);
        PCV_LOG_WARN("vm_manager", "VM '%s': %s; zvol rollback %s: %s%s%s",
                     vm_name, reason, removed ? "completed" : "failed", disk_path,
                     rollback_error ? " — " : "", rollback_error ? rollback_error->message : "");
        return removed;
    }
    if (g_file_test(disk_path, G_FILE_TEST_EXISTS)) {
        gboolean removed = g_unlink(disk_path) == 0;
        PCV_LOG_WARN("vm_manager", "VM '%s': %s; disk rollback %s: %s",
                     vm_name, reason, removed ? "completed" : "failed", disk_path);
        return removed;
    }
    return TRUE;
}

   
                                                                             
  
                           
                                                                     
                                                         
                                                            
                                                                   
                                                            
                                      
  
                                                      
                                                    
   
gboolean
purecvisor_vm_provision_file_disk(const gchar *format,
                                  const gchar *disk_path,
                                  gint size_gb,
                                  const gchar *base_image, const PcvResPin *base_pin,
                                  GError **error)
{
    if (!format || (g_strcmp0(format, "qcow2") != 0 &&
                    g_strcmp0(format, "raw") != 0) ||
        !disk_path || !*disk_path || size_gb < 1) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Invalid file disk provisioning arguments");
        return FALSE;
    }

    const gboolean has_base = base_image && *base_image;
    if (has_base && (!g_file_test(base_image, G_FILE_TEST_IS_REGULAR) || !pcv_res_pin_matches(base_image, base_pin))) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "Base image '%s' is missing, not a regular file, or changed after admission",
                    base_image);
        return FALSE;
    }
    g_autofree gchar *base_format = NULL;
    if (has_base && !pcv_disk_image_inspect(base_image, PCV_DISK_IMAGE_BASE, &base_format, error))
        return FALSE;

                                                                 
                                                       
    gint reserve_fd = g_open(disk_path, O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (reserve_fd < 0) {
        const gint saved_errno = errno;
        if (saved_errno == EEXIST) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                        "Disk image '%s' already exists — delete the VM first",
                        disk_path);
        } else {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Cannot reserve disk image '%s': %s",
                        disk_path, g_strerror(saved_errno));
        }
        return FALSE;
    }
    if (close(reserve_fd) != 0) {
        const gint saved_errno = errno;
        (void)g_unlink(disk_path);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Cannot close reserved disk image '%s': %s",
                    disk_path, g_strerror(saved_errno));
        return FALSE;
    }

    g_autofree gchar *size_str = g_strdup_printf("%dG", size_gb);
    const gchar *create_argv[] = {
        "qemu-img", "create", "-f", format, disk_path, size_str, NULL
    };
    g_autofree gchar *create_stderr = NULL;
    GError *spawn_error = NULL;

    if (!pcv_spawn_sync(create_argv, NULL, &create_stderr, &spawn_error)) {
        const gchar *detail = spawn_error ? spawn_error->message
            : (create_stderr && *create_stderr ? g_strstrip(create_stderr)
                                                : "unknown qemu-img error");
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "%s disk creation failed: %s", format, detail);
        g_clear_error(&spawn_error);
        (void)g_unlink(disk_path);
        return FALSE;
    }

    if (!has_base)
        return TRUE;

                                                                                    
    if (!pcv_disk_image_convert_into(base_image, base_format, format, disk_path, &spawn_error)) {
        const gboolean removed = g_unlink(disk_path) == 0;
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Base image conversion to %s failed: %s", format, spawn_error->message);
        PCV_LOG_WARN("vm_manager",
                     "base image conversion failed; target rollback %s: %s",
                     removed ? "completed" : "failed", disk_path);
        g_clear_error(&spawn_error);
        return FALSE;
    }

    return TRUE;
}

   
                                                                                    
  
                           
                                                                    
                                                                           
                                                                              
  
                                                              
   
gboolean
purecvisor_vm_provision_zvol_disk(const gchar *zvol_name,
                                  gint size_gb,
                                  const gchar *base_image, const PcvResPin *base_pin,
                                  GError **error)
{
    const gboolean has_base = base_image && *base_image;
    if (has_base && (!g_file_test(base_image, G_FILE_TEST_IS_REGULAR) || !pcv_res_pin_matches(base_image, base_pin))) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "Base image '%s' is missing, not a regular file, or changed after admission",
                    base_image);
        return FALSE;
    }
    g_autofree gchar *base_format = NULL;
    if (has_base && !pcv_disk_image_inspect(base_image, PCV_DISK_IMAGE_BASE, &base_format, error))
        return FALSE;

    g_autofree gchar *size_str = g_strdup_printf("%dG", size_gb);
    const gchar *zfs_options[] = {"-V", size_str, NULL};
    g_autofree gchar *create_stderr = NULL;
    GError *step_error = NULL;
    if (!pcv_zfs_create_locked(zfs_options, zvol_name, &create_stderr, &step_error)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "ZFS Provisioning Failed: %s",
                    step_error ? step_error->message : (create_stderr ? g_strstrip(create_stderr) : "Unknown ZFS error"));
        g_clear_error(&step_error);
        return FALSE;
    }
    if (!has_base)
        return TRUE;

    g_autofree gchar *disk_path = g_strdup_printf("/dev/zvol/%s", zvol_name);
    const gchar *udev_argv[] = {"udevadm", "settle", "--timeout=5", NULL};
    (void)pcv_spawn_sync(udev_argv, NULL, NULL, NULL);
                                                                                                 
    if (!pcv_res_pin_matches(base_image, base_pin) ||
        !pcv_disk_image_recheck(base_image, base_format, &step_error)) {
        const gboolean removed = _create_vm_rollback_disk(disk_path, zvol_name, "base image changed while waiting for the pool");
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Base image '%s' changed while waiting for the storage pool%s%s%s",
                    base_image, step_error ? ": " : "", step_error ? step_error->message : "",
                    removed ? "" : "; rollback of the zvol failed, remove it manually");
        g_clear_error(&step_error);
        return FALSE;
    }
    if (!pcv_disk_image_convert_into(base_image, base_format, "raw", disk_path, &step_error)) {
        const gboolean removed = _create_vm_rollback_disk(disk_path, zvol_name, "base image conversion failed");
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Base image conversion to raw failed: %s%s",
                    step_error->message, removed ? "" : "; rollback of the zvol failed, remove it manually");
        g_clear_error(&step_error);
        return FALSE;
    }
    PCV_LOG_INFO("vm_manager", "zvol '%s': base image '%s' written", zvol_name, base_image);
    return TRUE;
}

   
                                                  
  
                                                        
                                                     
                                                   
                                                    
  
                                                                               
                                                                   
                                                                           
                                            
                                             
                                                   
                                                
                                              
  
                              
                                                 
                                                     
   
static void create_vm_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    CreateVmTaskData *data = (CreateVmTaskData *)task_data;
    PCV_LOG_INFO("vm_manager", "VM '%s' creation worker started (vcpu=%d ram=%dMB disk=%dGB stype=%s)",
                 data ? data->name : "(null)",
                 data ? data->vcpu : -1,
                 data ? data->ram_mb : -1,
                 data ? data->disk_size_gb : -1,
                 (data && data->storage_type) ? data->storage_type : "(auto)");

                                                                      
    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                "vm.create cancelled before start");
        return;
    }

                                                                         
                                                                 
                                     
    if (data->network_bridge && pcv_vpc_bridge_is_managed(data->network_bridge)) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
            "VPC-managed NICs must be attached through vpc.attachment.create");
        return;
    }

                                                                        
                                                             
    guint sriov_pci[4] = {0, 0, 0, 0};
    if (g_strcmp0(data->nic_type, "sriov") == 0 &&
        !purecvisor_vm_sriov_pci_parse(data->pci_addr, sriov_pci)) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
            "Invalid SR-IOV pci_addr '%s': expected DDDD:BB:SS.F",
            data->pci_addr ? data->pci_addr : "");
        return;
    }

                                                                                
                                                                  
    GError *nic_err = NULL;
    if (!purecvisor_vm_sriov_nic_check(data->nic_type, data->network_bridge,
                                       data->network_mode, &nic_err)) {
        g_task_return_error(task, nic_err);
        return;
    }

                                                                   
                                                                        
                                                            
    if (g_strcmp0(data->nic_type, "dpdk") == 0) {
        g_autofree gchar *vhost_path = pcv_dpdk_vhost_socket_path(data->name);
        GError *preflight_error = NULL;
        if (!vhost_path ||
            !pcv_dpdk_vhost_runtime_preflight(&preflight_error)) {
            g_task_return_new_error(
                task, G_IO_ERROR, G_IO_ERROR_FAILED,
                "DPDK vhost runtime preflight failed: %s",
                preflight_error ? preflight_error->message
                                : "invalid or overlong canonical socket path");
            g_clear_error(&preflight_error);
            return;
        }
    }

                                                       
      
                                                                
                                                     
                                                                     
                                                 
      
                                                             
                                              
      
                                                     
                                                       
      
                                                         
                                                                
                                                   
                                  
    if (data->hugepages) {
        gchar *free_s = NULL;
        if (g_file_get_contents("/sys/kernel/mm/hugepages/hugepages-2048kB/free_hugepages",
                                &free_s, NULL, NULL) && free_s) {
            const gint64 free_pages = g_ascii_strtoll(g_strstrip(free_s), NULL, 10);
            const gint64 need_pages = _hugepage_preflight_need_pages(data->ram_mb);

            if (free_pages < need_pages) {
                g_free(free_s);
                g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "hugepages=true 이지만 호스트 2MB hugepage 가 부족합니다 "
                    "(필요 %" G_GINT64_FORMAT "개 = %d MB, 여유 %" G_GINT64_FORMAT "개). "
                    "`sysctl vm.nr_hugepages=<N>` 으로 사전 할당하거나 hugepages 를 끄십시오.",
                    need_pages, data->ram_mb, free_pages);
                return;
            }
        }
        g_free(free_s);
    }

                                       
      
          
                                        
                                                    
                                                            
      
                    
                                                                         
                                                                            
       
                  
                                        
                                               
                                         
                                                  
    gint final_disk_size = (data->disk_size_gb > 0) ? data->disk_size_gb : 50;
    if (final_disk_size > 2048) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "disk_size_gb (%d) exceeds 2TB limit", final_disk_size);
        return;
    }
    const gchar *zvol_pool = (data->storage_pool && *data->storage_pool)
        ? data->storage_pool
        : pcv_config_get_zvol_pool();
    const gchar *image_dir = (data->image_dir && *data->image_dir)
        ? data->image_dir
        : pcv_config_get_image_dir();
    gchar *disk_path = nullptr;
    gboolean use_zvol = FALSE;

                                       
                                              
                               
                             
                                                       
      
                                                        
                                                      
                                  
       
    const gchar *st = data->storage_type;
    gboolean use_file_raw = FALSE;                    

    if (st && g_strcmp0(st, "qcow2") == 0) {
        use_zvol = FALSE;
    } else if (st && g_strcmp0(st, "raw") == 0) {
        use_zvol = FALSE;
        use_file_raw = TRUE;
    } else if (st && g_strcmp0(st, "zvol") == 0) {
                                                
        const gchar *pool_chk_argv[] = {"zfs", "list", "-H",
                                         zvol_pool, NULL};
        gchar *chk_err = nullptr;
        GError *chk_e = nullptr;
        use_zvol = pcv_spawn_sync(pool_chk_argv, NULL, &chk_err, &chk_e);
        g_free(chk_err);
        if (chk_e) g_error_free(chk_e);
        if (!use_zvol) {
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                "storage_type 'zvol' requested but ZFS pool '%s' not found",
                zvol_pool);
            return;
        }
    } else {
                                                      
        const gchar *pool_chk_argv[] = {"zfs", "list", "-H",
                                         zvol_pool, NULL};
        gchar *chk_err = nullptr;
        GError *chk_e = nullptr;
        use_zvol = pcv_spawn_sync(pool_chk_argv, NULL, &chk_err, &chk_e);
        g_free(chk_err);
        if (chk_e) g_error_free(chk_e);
    }

                                                                
#if PCV_CLUSTER_ENABLED
    if (use_zvol && !pcv_cluster_check_zvol_fence()) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
            "zvol I/O fence: this node does not hold zvol pool ownership (ADR-0011). "
            "Only the cluster leader with confirmed ownership can create zvol-backed VMs.");
        return;
    }
#endif

    if (use_zvol) {
                                          
        gchar *zvol_name = g_strdup_printf("%s/%s",
                                            zvol_pool, data->name);
        gchar *zvol_dev  = g_strdup_printf("/dev/zvol/%s", zvol_name);

                                
        {
            const gchar *chk_argv[] = {"zfs", "list", "-H", "-t", "volume",
                                        zvol_name, NULL};
            gchar *chk_err = nullptr;
            GError *chk_e = nullptr;
            gboolean exists = pcv_spawn_sync(chk_argv, NULL, &chk_err, &chk_e);
            g_free(chk_err);
            if (chk_e) g_error_free(chk_e);
            if (exists) {
                g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_EXISTS,
                    "ZFS dataset '%s' already exists — delete the VM first",
                    zvol_name);
                g_free(zvol_name); g_free(zvol_dev);
                return;
            }
        }

        GError *zvol_error = NULL;
        if (!purecvisor_vm_provision_zvol_disk(zvol_name, final_disk_size, data->base_image,
                                               &data->base_pin, &zvol_error)) {
            PCV_LOG_WARN("vm_manager", "VM '%s' zvol provisioning failed: %s",
                         data->name, zvol_error ? zvol_error->message : "unknown error");
            g_task_return_error(task, zvol_error ? zvol_error :
                g_error_new(G_IO_ERROR, G_IO_ERROR_FAILED, "zvol provisioning failed"));
            g_free(zvol_name); g_free(zvol_dev);
            return;
        }
        disk_path = zvol_dev;
        g_free(zvol_name);
        PCV_LOG_INFO("vm_manager", "VM '%s': zvol disk created at %s (%dG, base_image=%s)",
                     data->name, zvol_pool, final_disk_size,
                     (data->base_image && *data->base_image) ? "applied" : "none");
    } else {
                                             
        const gchar *fmt = use_file_raw ? "raw" : "qcow2";
        const gchar *ext = use_file_raw ? "img" : "qcow2";
                            
        if (!g_file_test(image_dir, G_FILE_TEST_IS_DIR)) {
            const gchar *mkdir_argv[] = {"mkdir", "-p", image_dir, NULL};
            (void)pcv_spawn_sync(mkdir_argv, NULL, NULL, NULL);
        }

        disk_path = g_strdup_printf("%s/%s.%s", image_dir, data->name, ext);

        GError *file_error = NULL;
        if (!purecvisor_vm_provision_file_disk(fmt, disk_path, final_disk_size,
                                               data->base_image, &data->base_pin, &file_error)) {
            PCV_LOG_WARN("vm_manager", "VM '%s' file disk provisioning failed: %s",
                         data->name,
                         file_error ? file_error->message : "unknown error");
            g_task_return_error(task, file_error ? file_error :
                g_error_new(G_IO_ERROR, G_IO_ERROR_FAILED,
                            "%s provisioning failed", fmt));
            g_free(disk_path);
            return;
        }
        PCV_LOG_INFO("vm_manager",
                     "VM '%s': %s disk provisioned at %s (%dG, base_image=%s)",
                     data->name, fmt, disk_path, final_disk_size,
                     (data->base_image && *data->base_image) ? "applied" : "none");
    }

                                                           
    PureCVisorVmConfig *config = purecvisor_vm_config_new(data->name,
                                                          data->vcpu,
                                                          data->ram_mb);
    purecvisor_vm_config_set_disk(config, disk_path);

    if (data->iso_path) purecvisor_vm_config_set_iso(config, data->iso_path);
    if (data->cpu_mode > 0) purecvisor_vm_config_set_cpu_mode(config, data->cpu_mode);
    if (data->hugepages) purecvisor_vm_config_set_hugepages(config, TRUE);
    if (data->boot_mode > 0) purecvisor_vm_config_set_boot_mode(config, data->boot_mode);
    if (data->tpm) purecvisor_vm_config_set_tpm(config, TRUE);
                                                                    
                                                              
                                            
    if (data->network_mode) purecvisor_vm_config_set_network_mode(config, data->network_mode);
    if (data->tenant) purecvisor_vm_config_set_tenant(config, data->tenant);
                                                      

    GVirConfigDomain *domain_config = purecvisor_vm_config_build(config);
    
    gchar *base_xml = gvir_config_object_to_xml(GVIR_CONFIG_OBJECT(domain_config));

                                                                   
      
                                                                       
                                                           
                                                               
                                        
      
                            
                        
              
                                     
                     
                                                                              
                                                                           
      
                           
                                                           
                                                                   
                                                       
                                                         
      
                  
                                                                 
                                                            
                                                                          
      
                  
                                                     
                                                    
                                                      
                                       
      
                                                   
                                                        
                                                          
                     
                                                                              
    gchar *final_xml = nullptr;
    if (g_strcmp0(data->network_mode, "tenant-overlay") == 0) {
                                                                       
                                                                      
                                                      
                                                    
                                                         
        gchar *iface_xml = _overlay_ethernet_iface_xml();
        const gchar *insert_point = strstr(base_xml, "</devices>");
        if (insert_point) {
            gsize prefix_len = (gsize)(insert_point - base_xml);
            final_xml = g_strdup_printf("%.*s%s%s",
                                        (gint)prefix_len, base_xml,
                                        iface_xml,
                                        insert_point);
        } else {
                                                
            final_xml = g_strdup(base_xml);
        }
        g_free(iface_xml);
        g_free(base_xml);
    } else if (data->network_bridge && strlen(data->network_bridge) > 0) {
        gchar *iface_xml = nullptr;
        const gchar *nic_type = data->nic_type ? data->nic_type : "bridge";

        if (g_strcmp0(nic_type, "dpdk") == 0) {
                                                                  
                                                  
                                                                                  
            iface_xml = _build_dpdk_iface_xml(data->name);
        } else if (g_strcmp0(nic_type, "sriov") == 0) {
                                                                      
                                                               
                                                                                  
                                                                                          
                                                                       
            iface_xml = g_strdup_printf(
                "    <hostdev mode='subsystem' type='pci' managed='yes'>\n"
                "      <source>\n"
                "        <address domain='0x%04x' bus='0x%02x' slot='0x%02x' function='0x%x'/>\n"
                "      </source>\n"
                "    </hostdev>\n",
                sriov_pci[0], sriov_pci[1], sriov_pci[2], sriov_pci[3]);
        } else {
                                                                 
            gchar *vlan_xml = (data->vlan_id >= 1 && data->vlan_id <= 4094)
                ? g_strdup_printf("      <vlan><tag id=\"%d\"/></vlan>\n", data->vlan_id)
                : g_strdup("");

                                                      
                                                      
                                                       
            const gchar *ovs_argv[] = {"ovs-vsctl", "br-exists", data->network_bridge, NULL};
            gboolean is_ovs = pcv_spawn_sync(ovs_argv, NULL, NULL, NULL);

            gchar *uplink_mode = NULL;
            GError *uplink_error = NULL;
            if (!pcv_network_bridge_uplink_mode(
                    data->network_bridge, &uplink_mode, &uplink_error)) {
                                                                   
                                                                           
                PCV_LOG_WARN("vm_manager", "uplink mode lookup failed for %s: %s",
                             data->network_bridge,
                             uplink_error ? uplink_error->message : "unknown");
                g_clear_error(&uplink_error);
                uplink_mode = g_strdup("shared");
            }
            gboolean shared_uplink = g_strcmp0(uplink_mode, "shared") == 0;

                                                                
                                                      
                                                               
            gchar *virtualport_xml = is_ovs
                ? g_strdup("      <virtualport type='openvswitch'/>\n")
                : shared_uplink
                    ? g_strdup("      <filterref filter='no-mac-spoofing'/>\n")
                    : g_strdup("");

                                                         
                                              
            gchar *safe_bridge = g_markup_escape_text(data->network_bridge, -1);
                                                                          
                                                                   
                                                             
                                                          
            gint bridge_mtu = pcv_bridge_mtu_read(data->network_bridge, NULL);
            iface_xml = _build_bridge_iface_xml(safe_bridge, virtualport_xml, vlan_xml, bridge_mtu);
            g_free(safe_bridge);
            g_free(virtualport_xml);
            g_free(uplink_mode);
            g_free(vlan_xml);
        }

                               
        gchar *insert_point = strstr(base_xml, "</devices>");
        if (insert_point) {
            gsize prefix_len = (gsize)(insert_point - base_xml);
            final_xml = g_strdup_printf("%.*s%s%s",
                                        (gint)prefix_len, base_xml,
                                        iface_xml,
                                        insert_point);
        } else {
                                                
            final_xml = g_strdup(base_xml);
        }
        g_free(iface_xml);
        g_free(base_xml);
    } else {
                                
        final_xml = base_xml;
    }

    if (data->owner && *data->owner) {
        gchar *owned_xml = pcv_vm_xml_with_owner_metadata(final_xml, data->owner);
        g_free(final_xml);
        final_xml = owned_xml;
    }

                                                                   
                                                                   
                                                                      
                                                        
    {
        gchar *overlay_meta_xml = _overlay_metadata_xml(data->network_mode, data->tenant);
        if (overlay_meta_xml && *overlay_meta_xml) {
            gchar *merged_xml = pcv_vm_xml_inject_metadata_child(final_xml, overlay_meta_xml);
            g_free(final_xml);
            final_xml = merged_xml;
        }
        g_free(overlay_meta_xml);
    }

                                                                  
                                                                             
    {
        gchar *dpdk_meta_xml = _dpdk_metadata_xml(data->nic_type, data->network_bridge);
        if (dpdk_meta_xml && *dpdk_meta_xml) {
            gchar *merged_xml = pcv_vm_xml_inject_metadata_child(final_xml, dpdk_meta_xml);
            g_free(final_xml);
            final_xml = merged_xml;
        }
        g_free(dpdk_meta_xml);
    }

                                     
                                                      
                                                            
      
                                   
                                                         
                                                
                        
       
    {
        const gchar *old_video = "<model type=\"virtio\"/>";
        const gchar *new_video =
            "<model type=\"virtio\" vram=\"65536\" heads=\"1\">\n"
            "          <resolution x=\"1024\" y=\"768\"/>\n"
            "        </model>";
        gchar *pos = strstr(final_xml, old_video);
        if (pos) {
            gchar *patched = g_strdup_printf("%.*s%s%s",
                (gint)(pos - final_xml), final_xml,
                new_video, pos + strlen(old_video));
            g_free(final_xml);
            final_xml = patched;
        }
    }

                                                                              
                                                                    
                                                                                 
      
                                                                            
                                                                    
    {
        gchar *cpu_patched = _vm_xml_apply_cpu_invtsc(final_xml);
        g_free(final_xml);
        final_xml = cpu_patched;
    }

                                           
                                               
                                          
                                                                          
                                                                  
                                                          
                                                                        
                                                              
                                                                
                                              
                                                                     
                                                                             
                                                                          
                                   
                                                              
                                                
    {
        const gboolean want_shared =
            (g_strcmp0(data->nic_type ? data->nic_type : "bridge", "dpdk") == 0);
        gchar *mb_xml = nullptr;

                                                                          
                                                                    
          
                                                           
                                                              
                                                      
                                                                
                                                                     
          
                                                         
                                                            
                          
          
                                                             
                                                    
                                                                            
                                                                    
        mb_xml = _build_memory_backing_xml(data->hugepages, want_shared);

        if (mb_xml) {
            gchar *end = strstr(final_xml, "</domain>");
            if (end) {
                gchar *patched = g_strdup_printf("%.*s%s%s",
                                                 (gint)(end - final_xml), final_xml, mb_xml, end);
                g_free(final_xml);
                final_xml = patched;
            }
            g_free(mb_xml);
        }
    }

                                         
                                                       
                                                  
      
                                                             
                                                                            
      
                                                        
                                                                
                                                                     
      
               
                                     
                                            
                                                                    
      
                   
                                             
                                                   
                                                  
                                                  
      
                 
                                                       
                                                           
                                         
      
                     
                                     
                                                                 
                                      
                                                                              
    if (data->boot_mode >= 1) {
        const gchar *old_os = "<type machine=\"q35\">hvm</type>";

                                                      
        const gchar *loader_path = nullptr;
        const gchar *nvram_tpl = nullptr;

        if (data->boot_mode == 2) {
                                                          
            static const gchar *sb_loaders[] = {
                "/usr/share/OVMF/OVMF_CODE_4M.ms.fd",                         
                "/usr/share/OVMF/OVMF_CODE.secboot.fd",                        
                "/usr/share/edk2/ovmf/OVMF_CODE.secboot.fd",                   
                NULL
            };
            static const gchar *sb_nvrams[] = {
                "/usr/share/OVMF/OVMF_VARS_4M.ms.fd",
                "/usr/share/OVMF/OVMF_VARS.fd",
                "/usr/share/edk2/ovmf/OVMF_VARS.fd",
                NULL
            };
            for (gint i = 0; sb_loaders[i]; i++) {
                if (g_file_test(sb_loaders[i], G_FILE_TEST_EXISTS)) {
                    loader_path = sb_loaders[i];
                    nvram_tpl = sb_nvrams[i];
                    break;
                }
            }
            if (!loader_path) {
                loader_path = sb_loaders[0];                               
                nvram_tpl = sb_nvrams[0];
            }
        } else {
                                    
            static const gchar *uefi_loaders[] = {
                "/usr/share/OVMF/OVMF_CODE_4M.fd",                             
                "/usr/share/OVMF/OVMF_CODE.fd",                                 
                "/usr/share/edk2/ovmf/OVMF_CODE.fd",                            
                NULL
            };
            static const gchar *uefi_nvrams[] = {
                "/usr/share/OVMF/OVMF_VARS_4M.fd",
                "/usr/share/OVMF/OVMF_VARS.fd",
                "/usr/share/edk2/ovmf/OVMF_VARS.fd",
                NULL
            };
            for (gint i = 0; uefi_loaders[i]; i++) {
                if (g_file_test(uefi_loaders[i], G_FILE_TEST_EXISTS)) {
                    loader_path = uefi_loaders[i];
                    nvram_tpl = uefi_nvrams[i];
                    break;
                }
            }
            if (!loader_path) {
                loader_path = uefi_loaders[0];
                nvram_tpl = uefi_nvrams[0];
            }
        }

                                                                       
        gchar *nvram_path = g_strdup_printf(
            "/var/lib/libvirt/qemu/nvram/%s_VARS.fd", data->name);

                                                               
        const gchar *secure_attr = data->boot_mode == 2 ? " secure=\"yes\"" : "";
        gchar *new_os = g_strdup_printf(
            "<type machine=\"q35\">hvm</type>\n"
            "      <loader readonly=\"yes\" type=\"pflash\"%s>%s</loader>\n"
            "      <nvram template=\"%s\">%s</nvram>",
            secure_attr, loader_path, nvram_tpl, nvram_path);

        gchar *pos = strstr(final_xml, old_os);
        if (pos) {
            gchar *patched = g_strdup_printf("%.*s%s%s",
                (gint)(pos - final_xml), final_xml,
                new_os, pos + strlen(old_os));
            g_free(final_xml);
            final_xml = patched;
        }
        g_free(new_os);
        g_free(nvram_path);
    }

                                       
                                        
                                                   
                                                                
    if (data->tpm) {
        const gchar *tpm_xml =
            "    <tpm model='tpm-tis'>\n"
            "      <backend type='emulator' version='2.0'/>\n"
            "    </tpm>\n";
        gchar *insert = strstr(final_xml, "</devices>");
        if (insert) {
            gchar *patched = g_strdup_printf("%.*s%s%s",
                (gint)(insert - final_xml), final_xml,
                tpm_xml, insert);
            g_free(final_xml);
            final_xml = patched;
        }
    }

                                                 
                                                           
                                                    
    {
        const gchar *wd_cfg = pcv_config_get_string("vm", "watchdog_enabled", "true");
        if (g_ascii_strcasecmp(wd_cfg, "true") == 0 ||
            g_ascii_strcasecmp(wd_cfg, "1") == 0 ||
            g_ascii_strcasecmp(wd_cfg, "yes") == 0) {
            const gchar *wd_xml =
                "    <watchdog model='i6300esb' action='reset'/>\n";
            gchar *insert = strstr(final_xml, "</devices>");
            if (insert) {
                gchar *patched = g_strdup_printf("%.*s%s%s",
                    (gint)(insert - final_xml), final_xml,
                    wd_xml, insert);
                g_free(final_xml);
                final_xml = patched;
            }
        }
    }

                                                                           
                                                                            
                                                                 
                                                                        
                                                                      
    {
        gchar *mb_patched = _vm_xml_ensure_memballoon_stats(final_xml);
        g_free(final_xml);
        final_xml = mb_patched;

        gchar *rng_patched = _vm_xml_insert_virtio_rng(final_xml);
        g_free(final_xml);
        final_xml = rng_patched;

        gchar *ports_patched = _vm_xml_ensure_q35_hotplug_ports(final_xml);
        g_free(final_xml);
        final_xml = ports_patched;
    }

                                                                         
      
                                          
                                                             
                                            
                                           
                                                      
      
                           
                                                            
                                                             
                                                          
      
                 
                                                             
                                                       
      
                                                      
                                                       
                                                       
                                                       
                                                                              
                                                                      
                                                 
                                        
    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
                                     
        (void)_create_vm_rollback_disk(disk_path, data->name, "cancelled create");
        PCV_LOG_INFO("vm_manager", "VM '%s' creation cancelled before define", data->name);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
            "VM creation cancelled");
        g_free(final_xml);
        g_object_unref(domain_config);
        purecvisor_vm_config_free(config);
        g_free(disk_path);
        return;
    }

    virConnectPtr conn = virt_conn_pool_acquire();
    virDomainPtr dom = virDomainDefineXML(conn, final_xml);

    if (!dom) {
                             
                                                     
                                                            
                                                    
                                           
        virErrorPtr libvirt_err = virGetLastError();
        PCV_LOG_WARN("vm_manager", "virDomainDefineXML failed: %s",
                     libvirt_err ? libvirt_err->message : "unknown");

                                               
        (void)_create_vm_rollback_disk(disk_path, data->name, "failed define");

        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
            "VM operation failed — check server logs for details");
    } else {
                                                                 
          
                                                              
                                                    
                                                                   
                                                         
                              
        gchar *stored = virDomainGetXMLDesc(dom, 0);
        if (stored && final_xml) {
                                                                       
            const gchar *expected_br = strstr(final_xml, "<source bridge='");
            const gchar *actual_br   = strstr(stored,    "<source bridge='");
            gboolean bridge_match = TRUE;
            if (expected_br && actual_br) {
                expected_br += strlen("<source bridge='");
                actual_br   += strlen("<source bridge='");
                const gchar *e_end = strchr(expected_br, '\'');
                const gchar *a_end = strchr(actual_br,   '\'');
                if (e_end && a_end) {
                    gsize el = (gsize)(e_end - expected_br);
                    gsize al = (gsize)(a_end - actual_br);
                    if (el != al || strncmp(expected_br, actual_br, el) != 0) {
                        bridge_match = FALSE;
                        PCV_LOG_WARN("vm_manager",
                            "Post-define bridge mismatch for '%s' "
                            "(expected=%.*s, stored=%.*s) — redefining",
                            data->name, (int)el, expected_br, (int)al, actual_br);
                    }
                }
            }
            if (!bridge_match) {
                                                             
                virDomainUndefine(dom);
                virDomainFree(dom);
                dom = virDomainDefineXML(conn, final_xml);
                if (!dom) {
                    PCV_LOG_WARN("vm_manager",
                        "Redefine after mismatch failed for '%s'", data->name);
                }
            }
        }
        free(stored);                                              
        if (dom) {
                                                                                  
                                                               
                                             
            if (g_strcmp0(data->nic_type, "dpdk") == 0) {
                GError *dpdk_error = NULL;
                char *dpdk_domain_xml = virDomainGetXMLDesc(
                    dom, VIR_DOMAIN_XML_INACTIVE);
                gboolean dpdk_ready = dpdk_domain_xml &&
                    pcv_vm_dpdk_vhost_source_classify(
                        dpdk_domain_xml, data->name) ==
                            PCV_DPDK_VHOST_SOURCE_CANONICAL;
                if (!dpdk_ready) {
                    virErrorPtr vir_error = virGetLastError();
                    g_set_error(&dpdk_error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "stored domain does not contain one canonical DPDK vhost source%s%s",
                        (!dpdk_domain_xml && vir_error && vir_error->message) ? ": " : "",
                        (!dpdk_domain_xml && vir_error && vir_error->message)
                            ? vir_error->message : "");
                }
                free(dpdk_domain_xml);
                if (dpdk_ready)
                    dpdk_ready = pcv_dpdk_vm_port_ensure(
                        data->network_bridge, data->name, &dpdk_error);
                if (!dpdk_ready) {
                    int undef_rc = virDomainUndefineFlags(
                        dom, VIR_DOMAIN_UNDEFINE_NVRAM |
                             VIR_DOMAIN_UNDEFINE_SNAPSHOTS_METADATA);
                    if (undef_rc < 0)
                        undef_rc = virDomainUndefine(dom);
                    if (undef_rc == 0)
                        _create_vm_rollback_disk(
                            disk_path, data->name, "DPDK vhost provisioning failure");
                    else
                        PCV_LOG_ERROR("vm_manager",
                            "VM '%s': DPDK vhost provisioning and domain rollback both failed; "
                            "disk preserved for manual recovery", data->name);
                    virDomainFree(dom);
                    dom = NULL;
                    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "DPDK vhost provisioning failed: %s%s",
                        dpdk_error ? dpdk_error->message : "unknown error",
                        undef_rc == 0 ? " (domain and disk rolled back)" :
                                        " (domain rollback failed; manual recovery required)");
                    g_clear_error(&dpdk_error);
                    goto create_vm_cleanup;
                }
            }

                                                                         
                                                                
                                                                 
            if (data->qos_required) {
                GError *qos_meta_err = NULL;
                if (!pcv_vm_qos_metadata_write(dom, &data->qos_sla, &qos_meta_err)) {
                    PCV_LOG_WARN("vm_manager",
                        "VM '%s': qos metadata write 실패(WARN, create 는 계속 진행): %s",
                        data->name, qos_meta_err ? qos_meta_err->message : "unknown");
                    g_clear_error(&qos_meta_err);
                }
            }
            virDomainFree(dom);
                                             
#if PCV_CLUSTER_ENABLED
            pcv_cluster_sync_vm_xml(data->name);
#endif
            g_task_return_boolean(task, TRUE);
        } else {
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                "VM redefine failed after parameter mismatch");
        }
    }

create_vm_cleanup:
                                                                      
      
                 
                                                     
                                                        
      
                    
                                                                 
                                               
                                                                 
                                                                    
      
                                      
                                                   
                                                            
    virt_conn_pool_release(conn);
    g_free(final_xml);
    g_object_unref(domain_config);
    purecvisor_vm_config_free(config);
    g_free(disk_path);
}
 

   
                                         
                       
  
                                                                 
                                                              
  
                                        
                                                
                                  
                                    
                                                 
                                            
                                         
                                            
                                                              
                                                                          
                                            
                                   
   
void purecvisor_vm_manager_create_vm_async(PureCVisorVmManager *self,
                                           const gchar *name,
                                           gint vcpu,
                                           gint ram_mb,
                                           gint disk_size_gb,
                                           const gchar *iso_path,
                                           const gchar *network_bridge,
                                           gint         vlan_id,
                                           gint         boot_mode,
                                           gboolean     tpm,
                                           gint         cpu_mode,
                                           gboolean     hugepages,
                                           const gchar *storage_type,
                                           const gchar *storage_pool,
                                           const gchar *image_dir,
                                           const gchar *nic_type,
                                           const gchar *pci_addr,
                                           const gchar *base_image, const PcvResPin *base_image_pin,                                            
                                           const gchar *owner,
                                           const gchar *network_mode,                                                                
                                           const gchar *tenant,                                                                 
                                           gboolean     qos_required,                                                        
                                           const PcvQosSla *qos_sla,                                                        
                                           GCancellable *cancellable,
                                           GAsyncReadyCallback callback,
                                           gpointer user_data) {
                                                                    
      
                   
                                                       
                                                     
                                             
                                             
                                     
      
               
                                                     
                                      
      
                           
                                                        
                                                              
      
                             
                                 
                                                           
      
                             
                                             
                               
      
                             
                               
                                                   
                                                
                                                                              
                                                                                                  
    GTask *task = pcv_drain_task_new(self, cancellable, callback, user_data);
    if (g_strcmp0(nic_type, "dpdk") == 0 &&
        (!network_bridge || !*network_bridge ||
         !pcv_validate_bridge_name(network_bridge))) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                "nic_type=dpdk requires an explicit valid network_bridge");
        g_object_unref(task);
        return;
    }
    CreateVmTaskData *data = g_new0(CreateVmTaskData, 1);

    data->manager = g_object_ref(self);
    data->name = g_strdup(name);
    data->vcpu = vcpu;
    data->ram_mb = ram_mb;
    data->disk_size_gb = disk_size_gb;           
    data->iso_path = iso_path ? g_strdup(iso_path) : NULL;
                                                                        
                                                                          
    data->network_bridge = purecvisor_vm_resolve_network_bridge(network_bridge);
    data->vlan_id = vlan_id;              
    data->boot_mode = boot_mode;
    data->tpm = tpm;
    data->cpu_mode = cpu_mode;
    data->hugepages = hugepages;
    data->storage_type = storage_type ? g_strdup(storage_type) : NULL;
    data->storage_pool = storage_pool ? g_strdup(storage_pool) : NULL;
    data->image_dir = image_dir ? g_strdup(image_dir) : NULL;
    data->nic_type = nic_type ? g_strdup(nic_type) : NULL;
    data->pci_addr = pci_addr ? g_strdup(pci_addr) : NULL;
    data->base_image = base_image ? g_strdup(base_image) : NULL;
    if (base_image_pin) data->base_pin = *base_image_pin;
    data->owner = owner && *owner ? g_strdup(owner) : NULL;
                                                            
                                                                   
    data->network_mode = network_mode && *network_mode ? g_strdup(network_mode) : NULL;
    data->tenant = tenant && *tenant ? g_strdup(tenant) : NULL;
                                                                  
                                                        
    data->qos_required = qos_required;
    if (qos_required && qos_sla)
        data->qos_sla = *qos_sla;
    else
        memset(&data->qos_sla, 0, sizeof data->qos_sla);

    g_task_set_task_data(task, data, (GDestroyNotify)create_vm_task_data_free);
    g_task_run_in_thread(task, create_vm_thread);
    g_object_unref(task);
}

   
                                          
                           
                             
  
                                                         
  
                                             
   
gboolean purecvisor_vm_manager_create_vm_finish(PureCVisorVmManager *manager __attribute__((unused)),
                                                GAsyncResult *res,
                                                GError **error) {
    return g_task_propagate_boolean(G_TASK(res), error);
}
