   
                            
                                         
  
                           
                                                                                                           
                                                                                                                             
                                                                             
  
                                                              
   

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
#include "api/uds_server.h"
#include "modules/dispatcher/rpc_utils.h"
#include "modules/dispatcher/rpc_completion.h"
#include "modules/core/vm_state.h"
#include "modules/dispatcher/handler_vm_lifecycle.h"
#include "modules/dispatcher/guest_agent_exec.h"
#include "modules/virt/vm_manager.h"
#include "modules/network/vpc/vpc_manager.h"
#include "utils/pcv_spawn.h"
#include "utils/pcv_validate.h"
#include "utils/pcv_log.h"
#include "modules/virt/virt_conn_pool.h"
#include "purecvisor/pcv_handler_util.h"
#include "modules/network/security_group.h"
#include "modules/dispatcher/handler_vm_internal.h"


                                                  
                                                               
typedef enum {
    PCV_VM_RENAME_DISK_NONE = 0,                 
    PCV_VM_RENAME_DISK_ZVOL,                                           
    PCV_VM_RENAME_DISK_FILE,                                            
} PcvVmRenameDiskKind;

                                                          
                                                       
                                                                              
typedef struct {
    PcvVmRenameDiskKind disk_kind;                           
    gchar *old_disk_path;                          
    gchar *new_disk_path;                            
    gchar *old_dataset;                                           
    gchar *new_dataset;                                             
    gchar *old_nvram_path;                                        
    gchar *new_nvram_path;                                          
} PcvVmRenamePlan;

   
                                                          
                                                             
                                                             
   
static void
_vm_rename_plan_clear(PcvVmRenamePlan *plan)
{
    if (!plan) return;
    g_free(plan->old_disk_path);
    g_free(plan->new_disk_path);
    g_free(plan->old_dataset);
    g_free(plan->new_dataset);
    g_free(plan->old_nvram_path);
    g_free(plan->new_nvram_path);
    memset(plan, 0, sizeof(*plan));                                                       
}

   
                                                                   
                                                    
                                                                                  
                                    
   
void
pcv_disp_send_rpc_error(UdsServer *server, GSocketConnection *connection,
                const gchar *rpc_id, gint code, const gchar *message)
{
    gchar *err = pure_rpc_build_error_response(rpc_id, code,
                    message ? message : "Unknown error");                        
    pure_uds_server_send_response(server, connection, err);
    g_free(err);
}

   
                                                                 
                                                                       
                                                  
   
static const gchar *
_vm_rename_get_new_name(JsonObject *params)
{
    if (!params) return NULL;
                                                                 
    if (json_object_has_member(params, "new_name"))
        return json_object_get_string_member(params, "new_name");
    if (json_object_has_member(params, "target_name"))
        return json_object_get_string_member(params, "target_name");
    if (json_object_has_member(params, "target"))
        return json_object_get_string_member(params, "target");
    return NULL;
}

   
                                                                         
                                                       
                                                                     
   
xmlNodePtr
pcv_disp_xml_direct_child(xmlNodePtr parent, const gchar *name)
{
    if (!parent || !name) return NULL;
    for (xmlNodePtr cur = parent->children; cur; cur = cur->next) {
        if (cur->type == XML_ELEMENT_NODE &&                                      
            xmlStrcmp(cur->name, BAD_CAST name) == 0)
            return cur;
    }
    return NULL;
}

   
                                                                           
                                                    
                                                           
                                                                 
                                                             
   
static xmlNodePtr
_xml_find_primary_disk_source(xmlNodePtr node)
{
    for (xmlNodePtr cur = node; cur; cur = cur->next) {                        
        if (cur->type == XML_ELEMENT_NODE &&
            xmlStrcmp(cur->name, BAD_CAST "disk") == 0) {
            xmlChar *device = xmlGetProp(cur, BAD_CAST "device");                          
            gboolean is_disk = device && xmlStrcmp(device, BAD_CAST "disk") == 0;
            if (device) xmlFree(device);                                                   
            if (is_disk) {
                xmlNodePtr source = pcv_disp_xml_direct_child(cur, "source");
                if (source)
                    return source;                                                      
            }
        }

        xmlNodePtr child = _xml_find_primary_disk_source(cur->children);                   
        if (child)
            return child;
    }
    return NULL;
}

   
                                                                             
                                                            
  
                                                           
                                      
  
                                                        
                                                                               
                                                                     
                                                         
   
static gboolean
_vm_rename_build_zvol_target(const gchar *old_name, const gchar *new_name,
                             const gchar *old_path, PcvVmRenamePlan *plan,
                             gchar **error_msg)
{
    if (!g_str_has_prefix(old_path, "/dev/zvol/")) {                             
        if (error_msg)
            *error_msg = g_strdup("vm.rename supports only ZFS zvol or standard qcow2/raw primary disks");
        return FALSE;
    }

    gchar *dataset = g_strdup(old_path + strlen("/dev/zvol/"));                        
    gchar *slash = strrchr(dataset, '/');                                         
                                            
    if (!slash || slash == dataset || *(slash + 1) == '\0') {
        if (error_msg)
            *error_msg = g_strdup("vm.rename could not parse source zvol dataset path");
        g_free(dataset);
        return FALSE;
    }

    const gchar *leaf = slash + 1;                                                      
    if (g_strcmp0(leaf, old_name) != 0) {                                             
        if (error_msg) {
            *error_msg = g_strdup_printf(
                "vm.rename requires the primary zvol leaf '%s' to match VM name '%s'",
                leaf, old_name);
        }
        g_free(dataset);
        return FALSE;
    }

    gchar *parent = g_strndup(dataset, (gsize)(slash - dataset));                         
    gchar *target_dataset = g_strdup_printf("%s/%s", parent, new_name);                   
    gchar *target_path = g_strdup_printf("/dev/zvol/%s", target_dataset);                      

    plan->disk_kind = PCV_VM_RENAME_DISK_ZVOL;
    plan->old_disk_path = g_strdup(old_path);
    plan->new_disk_path = target_path;                                                    
    plan->old_dataset = dataset;                                                     
    plan->new_dataset = target_dataset;

    g_free(parent);                                                           
    return TRUE;
}

   
                                                                                 
                                                  
                                                            
                                                                     
   
static gboolean
_vm_rename_file_ext_allowed(const gchar *base, const gchar *old_name,
                            const gchar **ext_out)
{
    static const gchar *exts[] = { ".qcow2", ".raw", ".img", NULL };                 
    for (gint i = 0; exts[i]; i++) {
        gchar *expected = g_strdup_printf("%s%s", old_name, exts[i]);                           
        gboolean match = g_strcmp0(base, expected) == 0;
        g_free(expected);
        if (match) {
            if (ext_out) *ext_out = exts[i];                                
            return TRUE;
        }
    }
    return FALSE;
}

   
                                                                 
                                                 
                                                     
                                                                             
   
static gboolean
_vm_rename_build_file_target(const gchar *old_name, const gchar *new_name,
                             const gchar *old_path, PcvVmRenamePlan *plan,
                             gchar **error_msg)
{
    gchar *base = g_path_get_basename(old_path);                          
    gchar *dir = g_path_get_dirname(old_path);                             
    const gchar *ext = NULL;

    if (!_vm_rename_file_ext_allowed(base, old_name, &ext)) {                    
        if (error_msg) {
            *error_msg = g_strdup_printf(
                "vm.rename requires primary file disk basename to be %s.qcow2, %s.raw, or %s.img",
                old_name, old_name, old_name);
        }
        g_free(base);
        g_free(dir);
        return FALSE;
    }

    gchar *new_base = g_strdup_printf("%s%s", new_name, ext);                         
    gchar *target_path = g_build_filename(dir, new_base, NULL);                     

    plan->disk_kind = PCV_VM_RENAME_DISK_FILE;
    plan->old_disk_path = g_strdup(old_path);
    plan->new_disk_path = target_path;                                      

    g_free(new_base);
    g_free(base);
    g_free(dir);
    return TRUE;
}

   
                                                                         
                                                        
  
                                                            
                                                               
  
                 
                                                          
                                                             
                                                                
                                                              
   
static gboolean
_vm_rename_prepare_nvram(xmlNodePtr root, const gchar *old_name,
                         const gchar *new_name, PcvVmRenamePlan *plan,
                         gchar **error_msg)
{
    xmlNodePtr os = pcv_disp_xml_direct_child(root, "os");
    xmlNodePtr nvram = os ? pcv_disp_xml_direct_child(os, "nvram") : NULL;
    if (!nvram)
        return TRUE;                                                                       

    xmlChar *content = xmlNodeGetContent(nvram);                                
    if (!content || !*content) {                                           
        if (content) xmlFree(content);
        return TRUE;
    }

    gchar *old_path = g_strdup((const gchar *)content);
    xmlFree(content);
    g_strstrip(old_path);                                                          
    if (!*old_path) {                                                  
        g_free(old_path);
        return TRUE;
    }

    gchar *base = g_path_get_basename(old_path);
    gchar *expected = g_strdup_printf("%s_VARS.fd", old_name);                       
    if (g_strcmp0(base, expected) != 0) {                                          
        if (error_msg) {
            *error_msg = g_strdup_printf(
                "vm.rename found non-standard NVRAM path '%s'; expected basename '%s'",
                old_path, expected);
        }
        g_free(base);
        g_free(expected);
        g_free(old_path);
        return FALSE;
    }

    gchar *dir = g_path_get_dirname(old_path);
    gchar *new_base = g_strdup_printf("%s_VARS.fd", new_name);                          
    gchar *new_path = g_build_filename(dir, new_base, NULL);                          
    xmlNodeSetContent(nvram, BAD_CAST new_path);                                       

    plan->old_nvram_path = old_path;                                              
    plan->new_nvram_path = new_path;

    g_free(new_base);
    g_free(dir);
    g_free(base);
    g_free(expected);
    return TRUE;
}

   
                                                                            
                                                     
                                       
  
                                                             
                                                   
  
                                                                      
                                               
                                                                         
                                                       
   
static gchar *
_vm_rename_build_patched_xml(const gchar *xml,
                             const gchar *old_name,
                             const gchar *new_name,
                             PcvVmRenamePlan *plan,
                             gchar **error_msg)
{
                                                                            
    xmlDocPtr doc = xmlReadMemory(xml, (int)strlen(xml), "pcv-vm-rename.xml",
                                  NULL, XML_PARSE_NONET | XML_PARSE_NOERROR |
                                                XML_PARSE_NOWARNING);
    if (!doc) {
        if (error_msg)
            *error_msg = g_strdup("vm.rename failed to parse domain XML");
        return NULL;                                                                     
    }

    gchar *patched = NULL;                                                               
    xmlNodePtr root = xmlDocGetRootElement(doc);
    xmlNodePtr name_node = root ? pcv_disp_xml_direct_child(root, "name") : NULL;
    if (!name_node) {
        if (error_msg)
            *error_msg = g_strdup("vm.rename could not find domain <name> node");
        goto cleanup;
    }
    xmlNodeSetContent(name_node, BAD_CAST new_name);                            

    xmlNodePtr source = root ? _xml_find_primary_disk_source(root) : NULL;
    if (!source) {                                                                      
        if (error_msg)
            *error_msg = g_strdup("vm.rename requires a primary disk source");
        goto cleanup;
    }

                                                            
    xmlChar *dev = xmlGetProp(source, BAD_CAST "dev");
    xmlChar *file = dev ? NULL : xmlGetProp(source, BAD_CAST "file");
    if (dev) {
                                                         
        if (!_vm_rename_build_zvol_target(old_name, new_name, (const gchar *)dev,
                                          plan, error_msg)) {
            xmlFree(dev);
            goto cleanup;
        }
        xmlSetProp(source, BAD_CAST "dev", BAD_CAST plan->new_disk_path);                     
        xmlFree(dev);
    } else if (file) {
                                                         
        if (!_vm_rename_build_file_target(old_name, new_name, (const gchar *)file,
                                          plan, error_msg)) {
            xmlFree(file);
            goto cleanup;
        }
        xmlSetProp(source, BAD_CAST "file", BAD_CAST plan->new_disk_path);
        xmlFree(file);
    } else {
        if (error_msg)                                                                  
            *error_msg = g_strdup("vm.rename could not find primary disk dev/file source");
        goto cleanup;
    }

    if (!_vm_rename_prepare_nvram(root, old_name, new_name, plan, error_msg))
        goto cleanup;                                                         

                                 
    xmlChar *out = NULL;
    int out_len = 0;
    xmlDocDumpMemory(doc, &out, &out_len);
    if (!out || out_len <= 0) {
        if (error_msg)
            *error_msg = g_strdup("vm.rename failed to serialize patched XML");
        if (out) xmlFree(out);
        goto cleanup;
    }
    patched = g_strdup((const gchar *)out);                                     
    xmlFree(out);                                                                     

cleanup:
    xmlFreeDoc(doc);                                                            
    if (!patched)
        _vm_rename_plan_clear(plan);                                                  
    return patched;
}

   
                                                                    
                                                
                                                               
   
static gboolean
_vm_rename_zfs_exists(const gchar *dataset)
{
    const gchar *argv[] = {"zfs", "list", "-H", "-o", "name", dataset, NULL};
    return pcv_spawn_sync(argv, NULL, NULL, NULL);                                
}

   
                                                                                 
                                                        
                                                         
   
static gboolean
_vm_rename_zfs_rename(const gchar *from, const gchar *to, gchar **error_msg)
{
    gchar *stderr_s = NULL;
    const gchar *argv[] = {"zfs", "rename", from, to, NULL};
    gboolean ok = pcv_spawn_sync(argv, NULL, &stderr_s, NULL);
    if (!ok && error_msg) {                                                   
        *error_msg = g_strdup_printf("zfs rename %s -> %s failed: %s",
                                     from, to, stderr_s ? stderr_s : "unknown error");
    }
    g_free(stderr_s);
    return ok;
}

   
                                                                          
                                                 
  
                                                             
                                                                                    
                                          
   
static gboolean
_vm_rename_storage_apply(PcvVmRenamePlan *plan, gchar **error_msg)
{
    if (plan->disk_kind == PCV_VM_RENAME_DISK_ZVOL) {
        if (!_vm_rename_zfs_exists(plan->old_dataset)) {                        
            if (error_msg)
                *error_msg = g_strdup_printf("source zvol dataset not found: %s",
                                             plan->old_dataset);
            return FALSE;
        }
        if (_vm_rename_zfs_exists(plan->new_dataset)) {                             
            if (error_msg)
                *error_msg = g_strdup_printf("target zvol dataset already exists: %s",
                                             plan->new_dataset);
            return FALSE;
        }
        return _vm_rename_zfs_rename(plan->old_dataset, plan->new_dataset, error_msg);
    }

    if (plan->disk_kind == PCV_VM_RENAME_DISK_FILE) {
        if (!g_file_test(plan->old_disk_path, G_FILE_TEST_EXISTS)) {                     
            if (error_msg)
                *error_msg = g_strdup_printf("source disk file not found: %s",
                                             plan->old_disk_path);
            return FALSE;
        }
        if (g_file_test(plan->new_disk_path, G_FILE_TEST_EXISTS)) {                         
            if (error_msg)
                *error_msg = g_strdup_printf("target disk file already exists: %s",
                                             plan->new_disk_path);
            return FALSE;
        }
        if (g_rename(plan->old_disk_path, plan->new_disk_path) != 0) {                
            if (error_msg)
                *error_msg = g_strdup_printf("disk file rename failed: %s",
                                             g_strerror(errno));
            return FALSE;
        }
        return TRUE;
    }

    if (error_msg)                                                                     
        *error_msg = g_strdup("vm.rename has no storage plan");
    return FALSE;
}

   
                                                                
                                                   
                                                                 
   
static void
_vm_rename_storage_rollback(PcvVmRenamePlan *plan)
{
    if (!plan) return;
    if (plan->disk_kind == PCV_VM_RENAME_DISK_ZVOL) {
                                                 
        if (plan->new_dataset && plan->old_dataset &&
            _vm_rename_zfs_exists(plan->new_dataset)) {
            gchar *ignored = NULL;                                          
            if (!_vm_rename_zfs_rename(plan->new_dataset, plan->old_dataset, &ignored)) {
                PCV_LOG_ERROR("vm_rename", "storage rollback failed: %s",
                              ignored ? ignored : "unknown error");
            }
            g_free(ignored);
        }
    } else if (plan->disk_kind == PCV_VM_RENAME_DISK_FILE) {
        if (plan->new_disk_path && plan->old_disk_path &&
            g_file_test(plan->new_disk_path, G_FILE_TEST_EXISTS)) {
            if (g_rename(plan->new_disk_path, plan->old_disk_path) != 0) {
                PCV_LOG_ERROR("vm_rename", "disk rollback failed: %s",
                              g_strerror(errno));
            }
        }
    }
}

   
                                                         
                                                 
                                                                    
   
static gboolean
_vm_rename_nvram_apply(PcvVmRenamePlan *plan, gchar **error_msg)
{
    if (!plan->old_nvram_path || !plan->new_nvram_path)
        return TRUE;                                                                   

    if (g_file_test(plan->new_nvram_path, G_FILE_TEST_EXISTS)) {                         
        if (error_msg)
            *error_msg = g_strdup_printf("target NVRAM file already exists: %s",
                                         plan->new_nvram_path);
        return FALSE;
    }

    if (!g_file_test(plan->old_nvram_path, G_FILE_TEST_EXISTS))
        return TRUE;                                                               

    if (g_rename(plan->old_nvram_path, plan->new_nvram_path) != 0) {                
        if (error_msg)
            *error_msg = g_strdup_printf("NVRAM file rename failed: %s",
                                         g_strerror(errno));
        return FALSE;
    }
    return TRUE;
}

   
                                                                            
                                                       
   
static void
_vm_rename_nvram_rollback(PcvVmRenamePlan *plan)
{
    if (!plan || !plan->old_nvram_path || !plan->new_nvram_path)
        return;
    if (g_file_test(plan->new_nvram_path, G_FILE_TEST_EXISTS)) {
        if (g_rename(plan->new_nvram_path, plan->old_nvram_path) != 0) {
            PCV_LOG_ERROR("vm_rename", "NVRAM rollback failed: %s",
                          g_strerror(errno));
        }
    }
}

                                                                    
                                           
                                                                       

                                                                                         
static void
_vm_rename_reject(virDomainPtr dom, virConnectPtr conn, UdsServer *server, GSocketConnection *connection,
                  const gchar *rpc_id, PureRpcErrorCode code, const gchar *message)
{
    virDomainFree(dom);
    virt_conn_pool_release(conn);
    pcv_disp_send_rpc_error(server, connection, rpc_id, code, message);
}

   
                                                                       
                                                            
  
                                                        
                                          
                                                                                       
                                      
                                                                                             
  
                                                                         
                                                                                                                               
                                                                            
   
void
handle_vm_rename_request(JsonObject *params, const gchar *rpc_id,
                         UdsServer *server, GSocketConnection *connection)
{
    const gchar *old_name = pcv_disp_guest_get_vm_name(params);                        
    const gchar *new_name = _vm_rename_get_new_name(params);                                     

    if (!old_name || !new_name) {                                       
        pcv_disp_send_rpc_error(server, connection, rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                        "Invalid params: 'name' and 'new_name' are required");
        return;
    }
                                           
    if (!pcv_validate_vm_name(old_name) || !pcv_validate_vm_name(new_name)) {
        pcv_disp_send_rpc_error(server, connection, rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                        "Invalid VM name: only [A-Za-z0-9_-], max 64 chars");
        return;
    }
                                                                               
    if (!pcv_validate_new_vm_name(new_name)) {
        pcv_disp_send_rpc_error(server, connection, rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                        "VM name must not look like a UUID");
        return;
    }
    if (g_strcmp0(old_name, new_name) == 0) {                                    
        pcv_disp_send_rpc_error(server, connection, rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                        "new_name must be different from current name");
        return;
    }
                                                                                
    g_autofree gchar *lock_err = NULL;
    g_autoptr(PcvVmLock) old_lock = pcv_vm_lock_acquire(old_name, VM_OP_TUNING, &lock_err);
    g_autoptr(PcvVmLock) new_lock = old_lock ? pcv_vm_lock_acquire(new_name, VM_OP_CREATING, &lock_err) : NULL;
    if (!old_lock || !new_lock) {
        pcv_disp_send_rpc_error(server, connection, rpc_id, PURE_RPC_ERR_BUSY, lock_err ? lock_err : "VM busy (operation in progress)");
        return;
    }
    if (pcv_vpc_vm_is_attached(old_name)) {
        pcv_disp_send_rpc_error(server, connection, rpc_id, PURE_RPC_ERR_CONFLICT,
                        "VM has a Local VPC attachment; detach it before vm.rename");
        return;
    }

    virConnectPtr conn = virt_conn_pool_acquire();                             
    if (!conn) {
        pcv_disp_send_rpc_error(server, connection, rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
                        "Failed to connect to Libvirt.");
        return;
    }

    virDomainPtr dom = pure_virt_get_domain(conn, old_name);                           
    if (!dom) {
        virt_conn_pool_release(conn);                                         
        pcv_disp_send_rpc_error(server, connection, rpc_id, PURE_RPC_ERR_VM_NOT_FOUND, "VM not found.");
        return;
    }

                                                                     
                                                            
                                                               
                           
    {
        gchar *dpdk_bridge = NULL;
        PcvDpdkMetaResult dpdk_meta = pcv_vm_dpdk_metadata_read(dom, &dpdk_bridge);
        g_free(dpdk_bridge);
        if (dpdk_meta == PCV_DPDK_META_OK || dpdk_meta == PCV_DPDK_META_INVALID) {
            _vm_rename_reject(dom, conn, server, connection, rpc_id, PURE_RPC_ERR_CONFLICT,
                dpdk_meta == PCV_DPDK_META_OK
                    ? "vm.rename is not supported for managed DPDK VMs; recreate the VM with the new name"
                    : "VM DPDK metadata is unreadable; vm.rename blocked");
            return;
        }
    }
                                                                             
                                                                                
    const char *bound_name = virDomainGetName(dom);
    if (pcv_security_group_vm_is_bound(bound_name ? bound_name : old_name)) {
        _vm_rename_reject(dom, conn, server, connection, rpc_id, PURE_RPC_ERR_CONFLICT,
                          "VM has a security group binding; detach it before vm.rename");
        return;
    }

                                               
    virDomainPtr existing = virDomainLookupByName(conn, new_name);
    if (existing) {
        virDomainFree(existing);
        _vm_rename_reject(dom, conn, server, connection, rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Target VM name already exists.");
        return;
    }
    virResetLastError();                                                                    

    int active = virDomainIsActive(dom);                                         
    if (active < 0) {
        _vm_rename_reject(dom, conn, server, connection, rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "Could not verify VM power state.");
        return;
    }
    if (active == 1) {                                                             
        _vm_rename_reject(dom, conn, server, connection, rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "vm.rename requires the VM to be shut off.");
        return;
    }

    int snap_count = virDomainSnapshotNum(dom, 0);                        
    if (snap_count > 0) {                                                                
        _vm_rename_reject(dom, conn, server, connection, rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
                          "vm.rename is blocked while libvirt snapshot metadata exists.");
        return;
    }
    if (snap_count < 0)
        virResetLastError();                                                              

                                                  
    char *xml = virDomainGetXMLDesc(dom, VIR_DOMAIN_XML_INACTIVE);
    if (!xml)
        xml = virDomainGetXMLDesc(dom, 0);
    if (!xml) {
        _vm_rename_reject(dom, conn, server, connection, rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "Failed to read domain XML.");
        return;
    }

    PcvVmRenamePlan plan = {0};                                                
    gchar *err_msg = NULL;
                                                                    
    gchar *new_xml = _vm_rename_build_patched_xml(xml, old_name, new_name,
                                                  &plan, &err_msg);
    if (!new_xml) {
        free(xml);                                                                         
        _vm_rename_reject(dom, conn, server, connection, rpc_id, PURE_RPC_ERR_ZFS_OPERATION, err_msg);
        g_free(err_msg);
        return;
    }

                                                    
    gboolean storage_moved = FALSE;
    gboolean nvram_moved = FALSE;
    gboolean old_undefined = FALSE;

    if (!_vm_rename_storage_apply(&plan, &err_msg))                       
        goto fail;
    storage_moved = TRUE;

    if (!_vm_rename_nvram_apply(&plan, &err_msg))                           
        goto fail;
                                            
    nvram_moved = plan.old_nvram_path && plan.new_nvram_path &&
                  g_file_test(plan.new_nvram_path, G_FILE_TEST_EXISTS);

                                                                      
    int undef_rc = virDomainUndefineFlags(dom, VIR_DOMAIN_UNDEFINE_KEEP_NVRAM);
    if (undef_rc < 0) {
        virResetLastError();
        undef_rc = virDomainUndefine(dom);                                        
    }
    if (undef_rc < 0) {
        const gchar *vir_err = virGetLastErrorMessage();
        err_msg = g_strdup_printf("Failed to undefine old VM: %s",
                                  vir_err ? vir_err : "unknown error");
        virResetLastError();
        goto fail;
    }
    old_undefined = TRUE;                                                              
    virDomainFree(dom);
    dom = NULL;                                                                            

                               
    virDomainPtr new_dom = virDomainDefineXML(conn, new_xml);
    if (!new_dom) {
        const gchar *vir_err = virGetLastErrorMessage();
        err_msg = g_strdup_printf("Failed to define renamed VM: %s",
                                  vir_err ? vir_err : "unknown error");
        virResetLastError();
        goto fail;
    }
    virDomainFree(new_dom);                                                     

#if PCV_CLUSTER_ENABLED
                                                         
    pcv_cluster_remove_vm_xml(old_name);
    pcv_cluster_sync_vm_xml(new_name);
#endif

    pcv_audit_log(pcv_rpc_completion_actor(), "vm.rename", new_name, "ok", 0, 0, "local");                 

                                              
    JsonObject *res_obj = json_object_new();
    json_object_set_string_member(res_obj, "status", "renamed");
    json_object_set_string_member(res_obj, "old_name", old_name);
    json_object_set_string_member(res_obj, "new_name", new_name);
    json_object_set_string_member(res_obj, "storage_type",
        plan.disk_kind == PCV_VM_RENAME_DISK_ZVOL ? "zvol" : "file");
    json_object_set_string_member(res_obj, "old_disk", plan.old_disk_path);
    json_object_set_string_member(res_obj, "new_disk", plan.new_disk_path);
    json_object_set_boolean_member(res_obj, "disk_renamed", storage_moved);
    json_object_set_boolean_member(res_obj, "nvram_renamed", nvram_moved);

    JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(res_node, res_obj);
    gchar *resp = pure_rpc_build_success_response(rpc_id, res_node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);

                                      
    _vm_rename_plan_clear(&plan);
    g_free(new_xml);
    free(xml);
    virt_conn_pool_release(conn);
    return;

fail:
                                                        
    pcv_audit_log(pcv_rpc_completion_actor(), "vm.rename", old_name, "fail", PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
    if (nvram_moved)
        _vm_rename_nvram_rollback(&plan);                                
    if (storage_moved)
        _vm_rename_storage_rollback(&plan);                            
    if (old_undefined) {
                                                      
        virDomainPtr rollback_dom = virDomainDefineXML(conn, xml);
        if (rollback_dom)
            virDomainFree(rollback_dom);
        else
            PCV_LOG_ERROR("vm_rename", "failed to restore original XML for '%s'",
                          old_name);                                               
    }

    if (dom)
        virDomainFree(dom);                                                           
    pcv_disp_send_rpc_error(server, connection, rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
                    err_msg ? err_msg : "vm.rename failed");
    g_free(err_msg);
    _vm_rename_plan_clear(&plan);
    g_free(new_xml);
    free(xml);
    virt_conn_pool_release(conn);
}
