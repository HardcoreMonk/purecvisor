   
                            
                                
  
                           
                                                                                                           
                                                                                                                             
                                                                             
  
                                                   
   

#include "api/drain.h"
#include <unistd.h>
#include <glib.h>
#include <gio/gio.h>
#include <libvirt/libvirt.h>
#include <libvirt/libvirt-qemu.h>
#include <libvirt/virterror.h>
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include "modules/audit/pcv_audit.h"
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "api/uds_server.h"
#include "modules/dispatcher/rpc_utils.h"
#include "modules/core/vm_state.h"
#include "modules/dispatcher/handler_vm_lifecycle.h"
#include "modules/dispatcher/guest_agent_exec.h"
#include "modules/virt/cancellable_map.h"
#include "api/ws_server.h"
#include "modules/dispatcher/vm_delete_status.h"
#include "modules/dispatcher/vm_delete_plan.h"
#include "modules/virt/vm_manager.h"
#include "modules/network/vpc/vpc_manager.h"
#include "modules/network/dpdk_manager.h"
#include "utils/pcv_config.h"
#include "utils/pcv_spawn.h"
#include "utils/pcv_zfs_exec.h"
#include "utils/pcv_validate.h"
#include "utils/pcv_log.h"
#include "modules/virt/virt_conn_pool.h"
#include "purecvisor/pcv_handler_util.h"
#include "modules/network/security_group.h"
#include "modules/dispatcher/handler_vm_internal.h"


                                                                                                      
                                                                     
  
       
                                                                    
                                                                             
                                                                                    
  
                
                                                             
                                                                                                         

                                                                     
                                                                
                                                                                       
typedef struct {
    gchar        *vm_id;                                         
    gchar        *target;                                                               
    gchar        *rpc_id;                                       
    UdsServer    *server;                            
    GSocketConnection *connection;                                      
    PcvVmLock    *lock;                                                          
    PcvJobRef    *job_ref;                                                         
    GPtrArray    *warnings;                                            
    gboolean      already_absent;                                    
} VmDeleteCtx;

   
                                                       
                                  
                                                               
   
static void
_vm_delete_ctx_free(VmDeleteCtx *ctx)
{
    g_free(ctx->vm_id);
    g_free(ctx->target);
    g_free(ctx->rpc_id);
    g_object_unref(ctx->server);
    g_object_unref(ctx->connection);
    pcv_job_unref(ctx->job_ref);
    g_clear_pointer(&ctx->warnings, g_ptr_array_unref);
    g_free(ctx);
}

                                                                  
                                                         
static void
_vm_delete_restore_dpdk_port(gboolean removed, const gchar *bridge,
                             const gchar *vm_name, const gchar *stage)
{
    if (!removed || !bridge || !vm_name)
        return;
    GError *restore_error = NULL;
    if (!pcv_dpdk_vm_port_ensure(bridge, vm_name, &restore_error)) {
        PCV_LOG_ERROR("vm_delete", "VM '%s': %s 뒤 DPDK port 복원 실패: %s",
                      vm_name, stage,
                      restore_error ? restore_error->message : "unknown");
    }
    g_clear_error(&restore_error);
}

                                                                   
                                                           
                                                           
static gboolean
_vm_delete_prepare_nvram(const gchar *xml, gchar **path, GError **error)
{
    *path = NULL;
    xmlDocPtr doc = xml ? xmlReadMemory(xml, (int)strlen(xml), "vm-delete.xml",
        NULL, XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING) : NULL;
    xmlNodePtr root = doc ? xmlDocGetRootElement(doc) : NULL;
    if (!root || xmlStrcmp(root->name, BAD_CAST "domain") != 0) {
        if (doc) xmlFreeDoc(doc);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
            "VM XML unavailable or invalid; undefine/storage delete blocked");
        return FALSE;
    }
    xmlNodePtr os = pcv_disp_xml_direct_child(root, "os");
    xmlNodePtr nvram = pcv_disp_xml_direct_child(os, "nvram");
    xmlNodePtr varstore = pcv_disp_xml_direct_child(os, "varstore");
    gboolean valid = varstore == NULL;
    if (nvram) {
        xmlChar *type = xmlGetProp(nvram, BAD_CAST "type");
        xmlNodePtr source = pcv_disp_xml_direct_child(nvram, "source");
        xmlChar *value = source ? xmlGetProp(source, BAD_CAST "file")
                               : xmlNodeGetContent(nvram);
        valid = valid && (!type || xmlStrcmp(type, BAD_CAST "file") == 0);
        if (value) {
            gchar *candidate = g_strdup((const gchar *)value);
            g_strstrip(candidate);
            if (*candidate && g_path_is_absolute(candidate)) *path = candidate;
            else {
                valid = valid && !*candidate && !source;
                g_free(candidate);
            }
        } else if (source) valid = FALSE;
        xmlFree(value);
        xmlFree(type);
    }
    xmlFreeDoc(doc);
    if (!valid) {
        g_clear_pointer(path, g_free);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
            "Unsupported NVRAM storage; undefine/storage delete blocked");
    }
    return valid;
}

                                                                         
#define VM_DELETE_QUERY_TIMEOUT_SEC 60
                             
#define VM_DELETE_REFUSAL_MAX_NAMES 8

                                                                                                   
                                                    
                                         
static gboolean
_vm_delete_same_storage(const gchar *a, const gchar *b)
{
    if (g_strcmp0(a, b) == 0)
        return TRUE;
    struct stat sa, sb;
    if (stat(a, &sa) != 0 || stat(b, &sb) != 0)
        return FALSE;
    if (S_ISBLK(sa.st_mode) && S_ISBLK(sb.st_mode))
        return sa.st_rdev == sb.st_rdev;
    return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

                                                                                                     
                                                                                             
                                    
                                                             
static gboolean
_vm_delete_refuse_clones(const PcvVmDeleteStoragePlan *plan, GError **error)
{
    const gchar *argv[] = { "zfs", "list", "-H", "-t", "snapshot", "-o", "name,clones", "-r", plan->dataset, NULL };
    g_autofree gchar *out = NULL;
    GError *query_error = NULL;
    if (!pcv_spawn_sync_timeout(argv, &out, NULL, VM_DELETE_QUERY_TIMEOUT_SEC, &query_error)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "VM delete blocked: could not read clones of ZFS dataset '%s': %s. Nothing was changed.",
                    plan->dataset, query_error ? query_error->message : "unknown error");
        g_clear_error(&query_error);
        return FALSE;
    }
    g_autoptr(GPtrArray) clones = pcv_vm_delete_parse_snapshot_clones(out);
    if (clones->len == 0)
        return TRUE;
    g_autoptr(GString) names = g_string_new(NULL);
    for (guint i = 0; i < clones->len && i < VM_DELETE_REFUSAL_MAX_NAMES; i++)
        g_string_append_printf(names, "%s%s", i ? ", " : "", (const gchar *)g_ptr_array_index(clones, i));
    if (clones->len > VM_DELETE_REFUSAL_MAX_NAMES)
        g_string_append_printf(names, " and %u more", clones->len - VM_DELETE_REFUSAL_MAX_NAMES);
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "VM delete refused: ZFS dataset '%s' has dependent clones: %s. "
                "Delete those clones first, or make them independent with 'zfs promote', then retry. Nothing was changed.",
                plan->dataset, names->str);
    return FALSE;
}

                                                                                                
                                                                                               
                                                      
static gboolean
_vm_delete_refuse_backing(const PcvVmDeleteDiskRef *ref, const gchar *other, const PcvVmDeleteStoragePlan *plan, GError **error)
{
    if (access(ref->path, F_OK) != 0 && errno == ENOENT)
        return TRUE;
    const gchar *argv[] = { "qemu-img", "info", "--force-share", "--output=json", "--backing-chain", ref->path, NULL };
    g_autofree gchar *out = NULL;
    GError *query_error = NULL;
    if (!pcv_spawn_sync_timeout(argv, &out, NULL, VM_DELETE_QUERY_TIMEOUT_SEC, &query_error)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "VM delete blocked: could not read the backing chain of disk '%s' of VM '%s': %s. Nothing was changed.",
                    ref->path, other, query_error ? query_error->message : "unknown error");
        g_clear_error(&query_error);
        return FALSE;
    }
    g_autoptr(GPtrArray) files = pcv_vm_delete_parse_backing_chain(out, &query_error);
    if (!files) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "VM delete blocked: could not parse the backing chain of disk '%s' of VM '%s': %s. Nothing was changed.",
                    ref->path, other, query_error ? query_error->message : "unknown error");
        g_clear_error(&query_error);
        return FALSE;
    }
    for (guint i = 0; i < files->len; i++) {
        if (_vm_delete_same_storage(g_ptr_array_index(files, i), plan->path)) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "VM delete refused: disk '%s' is a backing file of disk '%s' of VM '%s'. "
                        "Delete that VM or flatten its disk first, then retry. Nothing was changed.",
                        plan->path, ref->path, other);
            return FALSE;
        }
    }
    return TRUE;
}

                                                                                                       
                                             
static gboolean
_vm_delete_refuse_other_domain(virDomainPtr other, const gchar *other_name, const PcvVmDeleteStoragePlan *plan, GError **error)
{
    const unsigned int xml_flags[] = { VIR_DOMAIN_XML_INACTIVE, 0 };
    g_autoptr(GHashTable) probed = g_hash_table_new(g_str_hash, g_str_equal);
    g_autoptr(GPtrArray) kept_refs = g_ptr_array_new_with_free_func((GDestroyNotify)g_ptr_array_unref);
    for (guint pass = 0; pass < G_N_ELEMENTS(xml_flags); pass++) {
        if (pass == 1 && virDomainIsActive(other) != 1)
            break;
        char *xml = virDomainGetXMLDesc(other, xml_flags[pass]);
        if (!xml) {
            virErrorPtr e = virGetLastError();
            if (e && e->code == VIR_ERR_NO_DOMAIN) {
                virResetLastError();
                return TRUE;
            }
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "VM delete blocked: could not read the definition of VM '%s': %s. Nothing was changed.",
                        other_name, e && e->message ? e->message : "unknown error");
            virResetLastError();
            return FALSE;
        }
        GError *parse_error = NULL;
        GPtrArray *refs = pcv_vm_delete_xml_disk_refs(xml, &parse_error);
        free(xml);                                
        if (!refs) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "VM delete blocked: could not parse the definition of VM '%s': %s. Nothing was changed.",
                        other_name, parse_error ? parse_error->message : "unknown error");
            g_clear_error(&parse_error);
            return FALSE;
        }
        g_ptr_array_add(kept_refs, refs);                                             
        for (guint i = 0; i < refs->len; i++) {
            const PcvVmDeleteDiskRef *ref = g_ptr_array_index(refs, i);
            if (_vm_delete_same_storage(ref->path, plan->path)) {
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "VM delete refused: disk '%s' is also used by VM '%s'. "
                            "Remove it from that VM's definition (detach it with 'pcvctl device disk detach <vm> --target <dev>') "
                            "or delete that VM first, then retry. Nothing was changed.",
                            plan->path, other_name);
                return FALSE;
            }
            if (ref->format && g_strcmp0(ref->format, "raw") != 0 && g_hash_table_add(probed, ref->path) &&
                !_vm_delete_refuse_backing(ref, other_name, plan, error))
                return FALSE;
        }
    }
    return TRUE;
}

                                                                                                   
                                                                                
                          
                                                                             
static gboolean
_vm_delete_refuse_dependents(virConnectPtr conn, const gchar *self_name, const PcvVmDeleteStoragePlan *plan, GError **error)
{
    if (plan->kind == PCV_VM_DELETE_STORAGE_ZVOL && !_vm_delete_refuse_clones(plan, error))
        return FALSE;
    virDomainPtr *domains = NULL;
    int count = virConnectListAllDomains(conn, &domains, 0);
    if (count < 0) {
        const gchar *message = virGetLastErrorMessage();
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "VM delete blocked: could not list other VMs: %s. Nothing was changed.",
                    message ? message : "unknown error");
        virResetLastError();
        return FALSE;
    }
    gboolean clear = TRUE;
    for (int i = 0; i < count; i++) {
        const char *name = virDomainGetName(domains[i]);
        if (clear && g_strcmp0(name, self_name) != 0)
            clear = _vm_delete_refuse_other_domain(domains[i], name, plan, error);
        virDomainFree(domains[i]);
    }
    free(domains);                                
    return clear;
}

   
                     
                                   
  
                                                        
                                                                 
                                                  
                                                     
                                       
  
                                                                                                  
  
                               
                                           
                                                
                                          
                                         
  
          
                                                                                         
                                                                                     
                                                       
                                                  
                                               
                             
                             
                                                      
                                 
   
static void
_vm_delete_worker(GTask *task, gpointer src __attribute__((unused)),
                  gpointer task_data, GCancellable *cancel)
{
    VmDeleteCtx *ctx = task_data;
    const gchar  *vm_id = ctx->vm_id;

                                                        
    if (cancel && g_cancellable_is_cancelled(cancel)) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                "vm.delete cancelled before start");
        return;
    }

                                                                                            
                                                                     
    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "VM delete blocked: libvirt connection is unavailable. Nothing was changed.");
        return;
    }
    virDomainPtr  dom  = pure_virt_get_domain(conn, vm_id);
    if (!dom) {
        virErrorPtr lookup_error = virGetLastError();
        if (!lookup_error || lookup_error->code != VIR_ERR_NO_DOMAIN) {
            gchar *reason = g_strdup_printf("VM delete blocked: VM lookup failed: %s. Nothing was changed.",
                                            lookup_error && lookup_error->message ? lookup_error->message : "unknown error");
            virResetLastError();
            virt_conn_pool_release(conn);
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", reason);
            g_free(reason);
            return;
        }
        virResetLastError();
    }
    g_autofree gchar *canonical_name = dom && virDomainGetName(dom)
        ? g_strdup(virDomainGetName(dom)) : g_strdup(vm_id);

                                                                                          
                                                            
                                                                                                     
    PcvVmDeleteStoragePlan plan = {0};
    gchar *saved_xml      = NULL;                                 
    g_autofree gchar *nvram_path = NULL;
    g_autofree gchar *dpdk_bridge = NULL;
    gboolean dpdk_port_removed = FALSE;
    gboolean zfs_exists = FALSE;
    if (dom) {
        char *xml = virDomainGetXMLDesc(dom, VIR_DOMAIN_XML_INACTIVE);
        if (xml) {
            saved_xml = g_strdup(xml);                             
            free(xml);                                                                           
        }

        GError *nvram_error = NULL;
        if (!_vm_delete_prepare_nvram(saved_xml, &nvram_path, &nvram_error)) {
            g_free(saved_xml);
            virDomainFree(dom);
            virt_conn_pool_release(conn);
            g_task_return_error(task, nvram_error);
            return;
        }
        GError *plan_error = NULL;
        gboolean planned = pcv_vm_delete_plan_from_xml(saved_xml, canonical_name, &plan, &plan_error);
        if (planned && virDomainIsActive(dom) == 1) {
                                                                           
            char *live_xml = virDomainGetXMLDesc(dom, 0);
            planned = pcv_vm_delete_plan_add_kept_from_xml(live_xml, &plan, &plan_error);
            free(live_xml);
        }
        if (!planned) {
            pcv_vm_delete_plan_clear(&plan);
            g_free(saved_xml);
            virDomainFree(dom);
            virt_conn_pool_release(conn);
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                    "VM delete blocked: could not read the disks of this VM: %s. Nothing was changed.",
                                    plan_error ? plan_error->message : "unknown error");
            g_clear_error(&plan_error);
            return;
        }
    } else {
        pcv_vm_delete_plan_for_absent(pcv_config_get_zvol_pool(), vm_id, &plan);
    }
    const gchar *zvol_path = plan.kind == PCV_VM_DELETE_STORAGE_ZVOL ? plan.path : NULL;
    const gchar *zfs_dataset = plan.dataset;
    const gchar *file_disk_path = plan.kind == PCV_VM_DELETE_STORAGE_FILE ? plan.path : NULL;
    zfs_exists = zvol_path && access(zvol_path, F_OK) == 0;                       
                                                          
    gboolean file_present = file_disk_path && (access(file_disk_path, F_OK) == 0 || errno != ENOENT);

                                                     
                                                                             
    if (zfs_exists || file_present) {
        GError *refuse_error = NULL;
        if (!_vm_delete_refuse_dependents(conn, canonical_name, &plan, &refuse_error)) {
            pcv_vm_delete_plan_clear(&plan);
            g_free(saved_xml);
            if (dom) virDomainFree(dom);
            virt_conn_pool_release(conn);
            g_task_return_error(task, refuse_error);
            return;
        }
    }

                                                      
                                                               
    if (!dom && !zfs_exists) {
        pcv_vm_delete_plan_clear(&plan);
        g_free(saved_xml);
        virt_conn_pool_release(conn);
        PCV_LOG_INFO("vm_delete", "VM '%s': already absent (idempotent success)", vm_id);
        ctx->already_absent = TRUE;
        g_task_return_boolean(task, TRUE);
        return;
    }

    if (dom) {
                                                                           
                                                                           
                                                                  
        PcvDpdkMetaResult dpdk_meta = pcv_vm_dpdk_metadata_read(dom, &dpdk_bridge);
        if (dpdk_meta == PCV_DPDK_META_INVALID) {
            pcv_vm_delete_plan_clear(&plan);
            g_free(saved_xml);
            virDomainFree(dom);
            virt_conn_pool_release(conn);
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                    "DPDK metadata is invalid or unreadable; VM delete blocked");
            return;
        }
        if (dpdk_meta == PCV_DPDK_META_OK) {
            GError *dpdk_error = NULL;
            if (!pcv_dpdk_vm_port_delete(canonical_name, &dpdk_error)) {
                pcv_vm_delete_plan_clear(&plan);
                g_free(saved_xml);
                virDomainFree(dom);
                virt_conn_pool_release(conn);
                g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "DPDK vhost port cleanup failed; VM delete blocked: %s",
                    dpdk_error ? dpdk_error->message : "unknown error");
                g_clear_error(&dpdk_error);
                return;
            }
            dpdk_port_removed = TRUE;
        }
    }

      
                                      
      
                                                            
                                                           
                                                             
                                                          
       
    if (dom) {
        virDomainInfo info = {0};
        if (virDomainGetInfo(dom, &info) < 0) {
            const gchar *vir_message = virGetLastErrorMessage();
            gchar *reason = g_strdup_printf(
                "VM state query failed; undefine/storage delete blocked: %s",
                vir_message ? vir_message : "unknown error");
            virResetLastError();
            _vm_delete_restore_dpdk_port(dpdk_port_removed, dpdk_bridge,
                                         canonical_name, "state query 실패");
            virDomainFree(dom);
            virt_conn_pool_release(conn);
            pcv_vm_delete_plan_clear(&plan);
            g_free(saved_xml);
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                    "%s", reason);
            g_free(reason);
            return;
        }
        if ((info.state == VIR_DOMAIN_RUNNING || info.state == VIR_DOMAIN_PAUSED) &&
            virDomainDestroy(dom) < 0) {
            const gchar *vir_message = virGetLastErrorMessage();
            gchar *reason = g_strdup_printf(
                "VM destroy failed; undefine/storage delete blocked: %s",
                vir_message ? vir_message : "unknown error");
            virResetLastError();
            _vm_delete_restore_dpdk_port(dpdk_port_removed, dpdk_bridge,
                                         canonical_name, "destroy 실패");
            virDomainFree(dom);
            virt_conn_pool_release(conn);
            pcv_vm_delete_plan_clear(&plan);
            g_free(saved_xml);
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                    "%s", reason);
            g_free(reason);
            return;
        }
        int undef_rc = virDomainUndefineFlags(dom,
                VIR_DOMAIN_UNDEFINE_SNAPSHOTS_METADATA |
                VIR_DOMAIN_UNDEFINE_MANAGED_SAVE |
                VIR_DOMAIN_UNDEFINE_KEEP_NVRAM);
        if (undef_rc < 0) {
            if (!nvram_path)
                undef_rc = virDomainUndefine(dom);
            if (undef_rc < 0) {
                virErrorPtr e = virGetLastError();
                PCV_LOG_WARN("vm_delete", "VM '%s': undefine failed: %s",
                             vm_id, e ? e->message : "unknown");
            }
        }
        virDomainFree(dom);
        if (undef_rc < 0) {
            _vm_delete_restore_dpdk_port(dpdk_port_removed, dpdk_bridge,
                                         canonical_name, "undefine 실패");
            virt_conn_pool_release(conn);
            pcv_vm_delete_plan_clear(&plan);
            g_free(saved_xml);
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                    "VM undefine failed; storage delete blocked");
            return;
        }
    }
    virt_conn_pool_release(conn);

                                                              
    pcv_security_group_sync_vm(vm_id);

      
                                                
      
                                                       
                                                         
      
                      
                                                                              
                                                                 
                                             
                                                           
                                              
                                             
                                         
                                                     
                                            
      
                     
                                                   
                                                         
                                                  
       
    gboolean  zfs_success = TRUE;
    gchar    *zfs_err_msg = g_strdup("Success");
    gboolean  exorcism_partial = FALSE;                    

    if (zfs_exists) {
                                                            
                                             
        GError *spawn_e = NULL;

        const gchar *fuser_argv[] = {"fuser", "-k", "-9", zvol_path, NULL};
        g_autofree gchar *fuser_err = NULL;
        gint fuser_rc = -1;
        if (!pcv_spawn_sync_status(fuser_argv, NULL, &fuser_err, &fuser_rc, &spawn_e) || fuser_rc > 1 ||
            (fuser_rc == 1 && fuser_err && *g_strstrip(fuser_err))) {                                    
            PCV_LOG_WARN("vm_delete", "VM '%s': fuser partial (exit %d): %s",
                         vm_id, fuser_rc, spawn_e ? spawn_e->message : fuser_err);
            exorcism_partial = TRUE;
        }
        if (spawn_e) { g_error_free(spawn_e); spawn_e = NULL; }

        const gchar *wipefs_argv[] = {"wipefs", "-a", zvol_path, NULL};
        if (!pcv_spawn_sync(wipefs_argv, NULL, NULL, &spawn_e)) {
            PCV_LOG_WARN("vm_delete", "VM '%s': wipefs partial: %s",
                         vm_id, spawn_e ? spawn_e->message : "nonzero exit");
            exorcism_partial = TRUE;
        }
        if (spawn_e) { g_error_free(spawn_e); spawn_e = NULL; }

        gchar *dd_of = g_strdup_printf("of=%s", zvol_path);
        const gchar *dd_argv[] = {"dd", "if=/dev/zero", dd_of,
                                   "bs=1M", "count=10", "status=none", NULL};
        if (!pcv_spawn_sync(dd_argv, NULL, NULL, &spawn_e)) {
            PCV_LOG_WARN("vm_delete", "VM '%s': dd zero partial: %s",
                         vm_id, spawn_e ? spawn_e->message : "nonzero exit");
            exorcism_partial = TRUE;
        }
        g_free(dd_of);
        if (spawn_e) { g_error_free(spawn_e); spawn_e = NULL; }

        const gchar *partx_argv[] = {"partx", "-d", zvol_path, NULL};
        (void)pcv_spawn_sync(partx_argv, NULL, NULL, NULL);                       
        const gchar *kpartx_argv[] = {"kpartx", "-d", zvol_path, NULL};
        (void)pcv_spawn_sync(kpartx_argv, NULL, NULL, NULL);
        const gchar *partprobe_argv[] = {"partprobe", NULL};
        (void)pcv_spawn_sync(partprobe_argv, NULL, NULL, NULL);
        const gchar *udevadm_argv[] = {"udevadm", "settle", "--timeout=5", NULL};
        (void)pcv_spawn_sync(udevadm_argv, NULL, NULL, NULL);

        g_usleep(1 * G_USEC_PER_SEC);

                                                                                      
                                                                
        gchar *zfs_stderr = NULL;
        GError *zfs_error = NULL;
        if (!pcv_zfs_destroy_locked("-r", zfs_dataset, &zfs_stderr, &zfs_error)) {
            zfs_success = FALSE;
            g_free(zfs_err_msg);
            zfs_err_msg = g_strdup(zfs_stderr ? zfs_stderr : (zfs_error ? zfs_error->message : "zfs destroy failed"));
        }
        g_clear_error(&zfs_error);
        g_free(zfs_stderr);
    }

    if (!zfs_success) {
                                                         
                                                         
                                                     
        gboolean redefined = FALSE;
        if (saved_xml) {
            virConnectPtr rc = virt_conn_pool_acquire();
            if (rc) {
                virDomainPtr rdom = virDomainDefineXML(rc, saved_xml);
                if (rdom) {
                    virDomainFree(rdom);
                    redefined = TRUE;
                    PCV_LOG_WARN("vm_delete",
                        "VM '%s': ZFS destroy failed — definition restored from saved XML. "
                        "Delete can be retried.", vm_id);
                    _vm_delete_restore_dpdk_port(dpdk_port_removed, dpdk_bridge,
                                                 canonical_name, "XML rollback");
                } else {
                    virErrorPtr e = virGetLastError();
                    PCV_LOG_ERROR("vm_delete",
                        "VM '%s': ZFS destroy failed AND redefine failed: %s. "
                        "Manual recovery required: restore XML and investigate zvol state.",
                        vm_id, e ? e->message : "unknown");
                }
                virt_conn_pool_release(rc);
            }
        }

        gchar *reason = g_strdup_printf(
            "ZFS destroy failed%s: %s",
            redefined ? " (VM definition rolled back)" : " (VM XML gone — manual recovery)",
            zfs_err_msg);
        g_free(zfs_err_msg);
        pcv_vm_delete_plan_clear(&plan);
        g_free(saved_xml);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", reason);
        g_free(reason);
        return;
    }
    g_free(zfs_err_msg);
                                                                  
                                                  

      
                               
      
                                             
                                       
                                              
      
                                                      
       
    if (file_disk_path && !g_str_has_prefix(file_disk_path, "/dev/")) {
        if (access(file_disk_path, F_OK) == 0 || errno != ENOENT) {
            if (unlink(file_disk_path) == 0) {
                PCV_LOG_INFO("vm_delete", "VM '%s': disk file deleted: %s",
                             vm_id, file_disk_path);
            } else {
                int err = errno;
                PCV_LOG_ERROR("vm_delete", "VM '%s': failed to delete disk file '%s': %s",
                              vm_id, file_disk_path, g_strerror(err));
                gboolean redefined = FALSE;
                if (!zfs_exists && saved_xml) {
                    virConnectPtr rc = virt_conn_pool_acquire();
                    if (rc) {
                        virDomainPtr rdom = virDomainDefineXML(rc, saved_xml);
                        if (rdom) {
                            virDomainFree(rdom);
                            redefined = TRUE;
                            PCV_LOG_WARN("vm_delete",
                                "VM '%s': file disk cleanup failed — definition restored. "
                                "Delete can be retried.", vm_id);
                            _vm_delete_restore_dpdk_port(
                                dpdk_port_removed, dpdk_bridge, canonical_name,
                                "file disk rollback");
                        } else {
                            virErrorPtr e = virGetLastError();
                            PCV_LOG_ERROR("vm_delete",
                                "VM '%s': file disk cleanup failed AND redefine failed: %s. "
                                "Manual recovery required.", vm_id,
                                e ? e->message : "unknown");
                        }
                        virt_conn_pool_release(rc);
                    }
                }
                gchar *reason = g_strdup_printf(
                    "Disk file cleanup failed%s: %s (%s). %s",
                    redefined ? " (VM definition rolled back)" : "",
                    file_disk_path, g_strerror(err),
                    redefined ? "Delete can be retried."
                              : "VM definition is absent; manual cleanup is required.");
                pcv_vm_delete_plan_clear(&plan);
                g_free(saved_xml);
                g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", reason);
                g_free(reason);
                return;
            }
        }
    }
    g_free(saved_xml);

                                                                            
                                                                                         
    if (nvram_path && unlink(nvram_path) < 0 && errno != ENOENT) {
        gchar *warning = g_strdup_printf("NVRAM cleanup failed: %s (%s); manual cleanup is required",
                                         nvram_path, g_strerror(errno));
        PCV_LOG_WARN("vm_delete", "VM '%s': %s", vm_id, warning);
        g_ptr_array_add(ctx->warnings, warning);
    }

    if (exorcism_partial) {
        PCV_LOG_WARN("vm_delete", "VM '%s': delete succeeded with partial exorcism (check above warnings)", vm_id);
        g_ptr_array_add(ctx->warnings, g_strdup("storage exorcism was partial; see daemon log"));
    }
                                                                          
    for (guint i = 0; i < plan.kept->len; i++) {
        gchar *warning = g_strdup_printf("Disk not deleted (not owned by this VM): %s",
                                         (const gchar *)g_ptr_array_index(plan.kept, i));
        PCV_LOG_WARN("vm_delete", "VM '%s': %s", vm_id, warning);
        g_ptr_array_add(ctx->warnings, warning);
    }
    pcv_vm_delete_plan_clear(&plan);
    g_task_return_boolean(task, TRUE);
}

   
                       
                                                                            
  
                                                                       
                                                                                      
                                                                        
   
static void
_vm_delete_callback(GObject *src __attribute__((unused)), GAsyncResult *res,
                    gpointer user_data)
{
    VmDeleteCtx *ctx = user_data;
    GError      *err = NULL;

                                                              
    g_clear_pointer(&ctx->lock, pcv_vm_lock_release);

    gboolean ok = g_task_propagate_boolean(G_TASK(res), &err);
    const gchar *emsg = ok ? NULL : (err ? err->message : "unknown");
    gchar *result = pcv_vm_delete_job_result(ctx->target, ctx->already_absent, ctx->warnings, emsg);
    if (ok) {                                                                
        pcv_audit_log(pcv_drain_task_actor(res), "vm.delete", ctx->target, "ok", 0, 0, "local");
        pcv_ws_finish_job(ctx->job_ref, PCV_JOB_COMPLETED, result, NULL);
        g_message("[vm.delete] complete for '%s' (warnings: %u)", ctx->target, ctx->warnings->len);
    } else {
        pcv_audit_log(pcv_drain_task_actor(res), "vm.delete", ctx->target, "fail", PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
        pcv_ws_finish_job(ctx->job_ref, PCV_JOB_FAILED, result, emsg);
        g_warning("[vm.delete] background worker failed for '%s': %s", ctx->target, emsg);
    }
    g_free(result);
    g_clear_error(&err);
                                                                   
    cmap_remove(ctx->vm_id);
    _vm_delete_ctx_free(ctx);
}

   
                            
                                                
  
                                                               
                                                       
                                                                                  
                                                                                            
  
                                                                             
                                                                                              
  
                                                
   
void handle_vm_delete_request(JsonObject *params, const gchar *rpc_id,
                              UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm_id = json_object_get_string_member(params, "vm_id");
    if (!vm_id || !pcv_validate_vm_name(vm_id)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing or invalid param: vm_id (alphanumeric, -, _ only)");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }
    if (pcv_vpc_vm_is_attached(vm_id)) {
        gchar *err = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_CONFLICT,
            "VM has a Local VPC attachment; detach it explicitly before vm.delete");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

                                                                    
                                                                        
                                                            
    gchar *lock_err = NULL;
    PcvVmLock *op_lock = pcv_vm_lock_acquire(vm_id, VM_OP_DELETING, &lock_err);
    if (!op_lock) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_CONFLICT,
                       lock_err ? lock_err : "VM is busy (another operation in progress)");
        pure_uds_server_send_response(server, connection, e);
        g_free(e); g_free(lock_err);
        return;
    }

                                                                   
                                                                            
    PcvJobRef *job_ref = pcv_job_admit("vm.delete", pcv_vm_lock_key(op_lock), NULL, "vm.delete");
    if (!job_ref) {
        pcv_vm_lock_release(op_lock);
        pcv_disp_send_rpc_error(server, connection, rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
                        "Job persistence unavailable — VM deletion rejected");
        return;
    }
    pcv_job_progress(job_ref, 0, "VM deletion started");

                                                                                           
    JsonObject *acc = json_object_new();
    json_object_set_boolean_member(acc, "accepted", TRUE);
    json_object_set_string_member(acc, "name", pcv_vm_lock_key(op_lock));
    json_object_set_string_member(acc, "job_id", pcv_job_id(job_ref));
    json_object_set_string_member(acc, "status", "accepted");
    JsonNode *acc_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(acc_node, acc);
    gchar *acc_resp = pure_rpc_build_success_response(rpc_id, acc_node);
    pure_uds_server_send_response(server, connection, acc_resp);                     
    g_free(acc_resp);

    VmDeleteCtx *ctx = g_new0(VmDeleteCtx, 1);
    ctx->vm_id      = g_strdup(vm_id);
    ctx->target     = g_strdup(pcv_vm_lock_key(op_lock));
    ctx->rpc_id     = g_strdup(rpc_id);
    ctx->server     = g_object_ref(server);
    ctx->connection = g_object_ref(connection);
    ctx->lock       = op_lock;
    ctx->job_ref    = job_ref;                                                    
    ctx->warnings   = g_ptr_array_new_with_free_func(g_free);

                                                                                        
                                                                       
    GCancellable *cancel = g_cancellable_new();
    cmap_register(vm_id, cancel);

    GTask *task = pcv_drain_task_new(NULL, cancel, _vm_delete_callback, ctx);
    g_task_set_task_data(task, ctx, NULL);                                                                     
    g_task_run_in_thread(task, _vm_delete_worker);
    g_object_unref(task);
    g_object_unref(cancel);                             
}
