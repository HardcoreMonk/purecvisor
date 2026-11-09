   
                         
                                                               
  
                           
                                                                           
                                                                      
                                                                
                                                   
  
                                                          
   

#include "api/dispatcher.h"
#include "api/uds_server.h"
#include "api/snapshot_verify_probe.h"
#include "api/ova_import_xml.h"
#include "modules/virt/disk_image_guard.h"
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
#include "modules/resource/resource_gate.h"


                                                                         
                                                      
  
                                                               
                                                     
                                                    
  
                                                                   
                                     
  
                     
                             
                                  
                        
                            
                             
                
  
         
                            
                                       
                                                                            

typedef struct {
    gchar *vm_name;
    gchar *output_dir;
    gchar *job_id;
    PcvJobRef *job_ref;                                     
} OvaExportCtx;

                                                       
                                                       
static void _free_ova_ctx(gpointer data) {
    if (!data) return;
    OvaExportCtx *ctx = data;
    g_free(ctx->vm_name);
    g_free(ctx->output_dir);
    pcv_job_unref(ctx->job_ref);
    g_free(ctx->job_id);
    g_free(ctx);
}

   
                                       
                                                           
                  
                                              
   
static gchar *_ova_sha256_file(const gchar *path) {
    const gchar *argv[] = {"sha256sum", path, NULL};
    gchar *stdout_buf = nullptr;
    GError *error = nullptr;
    if (!pcv_spawn_sync(argv, &stdout_buf, NULL, &error)) {
        if (error) g_error_free(error);
        g_free(stdout_buf);
        return NULL;
    }
                                          
    if (stdout_buf) {
        gchar *sp = strchr(stdout_buf, ' ');
        if (sp) *sp = '\0';
        gchar *hash = g_strdup(stdout_buf);
        g_free(stdout_buf);
        return hash;
    }
    return NULL;
}

                                                                      
                                                      
                                     
static gchar *
_ova_export_result_json(OvaExportCtx *ctx, gboolean ok,
                        const gchar *ova_path, const gchar *error_msg)
{
    JsonObject *result = json_object_new();
    json_object_set_string_member(result, "vm", ctx->vm_name ?: "");
    json_object_set_string_member(result, "format", "ova");
    json_object_set_string_member(result, "output_dir", ctx->output_dir ?: "");
    if (ok) {
        json_object_set_string_member(result, "ova_path", ova_path ?: "");
    } else {
        json_object_set_string_member(result, "error",
                                      error_msg ?: "OVA export failed");
    }

    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, result);
    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, node);
    gchar *json = json_generator_to_data(gen, NULL);
    g_object_unref(gen);
    json_node_free(node);
    return json;
}

                                                                     
                                                     
                                                                           
static void
_ova_export_record_result(OvaExportCtx *ctx, gboolean ok,
                          const gchar *ova_path, const gchar *error_msg, const gchar *actor)
{
    gchar *result_json = _ova_export_result_json(ctx, ok, ova_path, error_msg);
    pcv_audit_log(actor, "vm.export.ova", ctx->vm_name ?: "",
                  ok ? "ok" : "fail", ok ? 0 : PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
    pcv_ws_finish_job(ctx->job_ref, ok ? PCV_JOB_COMPLETED : PCV_JOB_FAILED,
                       result_json, ok ? NULL : (error_msg ?: "OVA export failed"));
    g_free(result_json);
}

                                                           
                                                           
                                                             
                                  
                                                           
                                                                                       
                                                        
static void _ova_export_worker(GTask *task, gpointer source_obj,
                                gpointer task_data, GCancellable *cancellable)
{
    (void)source_obj; (void)cancellable;
    OvaExportCtx *ctx = task_data;
    gboolean audit_ok = FALSE;
    const gchar *audit_error = NULL;
    gchar *audit_error_owned = NULL;
    gchar *xml = NULL;
    gchar *disk_path = NULL;
    gchar *disk_format = NULL;
    gchar *tmpdir = NULL;
    gchar *vmdk_name = NULL;
    gchar *vmdk_path = NULL;
    gchar *ovf_name = NULL;
    gchar *ovf_path = NULL;
    gchar *mf_name = NULL;
    gchar *mf_path = NULL;
    gchar *ova_path = NULL;

                                                                    
    pcv_job_progress(ctx->job_ref, 5,
                          "Reading VM metadata");

                                  
    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        g_warning("[OVA] Failed to connect to libvirt for %s", ctx->vm_name);
        audit_error = "failed to connect to libvirt";
        goto ova_cleanup;
    }

    extern virDomainPtr pure_virt_get_domain(virConnectPtr conn, const gchar *identifier);
    virDomainPtr dom = pure_virt_get_domain(conn, ctx->vm_name);
    if (!dom) {
        g_warning("[OVA] VM '%s' not found", ctx->vm_name);
        virt_conn_pool_release(conn);
        audit_error = "VM not found";
        goto ova_cleanup;
    }

    xml = virDomainGetXMLDesc(dom, VIR_DOMAIN_XML_INACTIVE);
    virDomainInfo info = {0};
    int vcpus = 1;
    int mem_mb = 1024;
    if (virDomainGetInfo(dom, &info) == 0) {
        vcpus = info.nrVirtCpu > 0 ? (int)info.nrVirtCpu : 1;
        mem_mb = info.maxMem > 0 ? (int)(info.maxMem / 1024) : 1024;
    }
    virDomainFree(dom);
    virt_conn_pool_release(conn);

    if (!xml) {
        g_warning("[OVA] Failed to get XML for %s", ctx->vm_name);
        audit_error = "failed to get VM XML";
        goto ova_cleanup;
    }

                                                                   
                                                     
                                                                  
    {
        GError *error = NULL;
        if (!pcv_ova_export_disk_source(xml, &disk_path, &disk_format, &error)) {
            audit_error_owned = g_strdup(error ? error->message : "no disk source found");
            g_clear_error(&error);
        }
    }
    g_free(xml);

    if (!disk_path) {
        g_warning("[OVA] No disk source found for %s", ctx->vm_name);
        audit_error = audit_error_owned ? audit_error_owned : "no disk source found";
        goto ova_cleanup;
    }

                                              
    tmpdir = g_strdup_printf("%s/ova-%s-%ld",
                             ctx->output_dir, ctx->vm_name, (long)time(NULL));
    if (g_mkdir_with_parents(tmpdir, 0755) != 0) {
        audit_error_owned = g_strdup_printf("failed to create temporary directory: %s",
                                            g_strerror(errno));
        audit_error = audit_error_owned;
        goto ova_cleanup;
    }

    vmdk_name = g_strdup_printf("%s.vmdk", ctx->vm_name);
    vmdk_path = g_strdup_printf("%s/%s", tmpdir, vmdk_name);

    g_message("[OVA] Converting %s (%s) → %s", disk_path, disk_format, vmdk_path);
    pcv_job_progress(ctx->job_ref, 35,
                          "Converting disk to VMDK");
    {
        const gchar *argv[] = {"qemu-img", "convert", "-f", disk_format,
            "-O", "vmdk", disk_path, vmdk_path, NULL};
        gchar *std_err = nullptr;
        GError *error = nullptr;
        if (!pcv_spawn_sync(argv, NULL, &std_err, &error)) {
            g_warning("[OVA] qemu-img convert failed: %s",
                error ? error->message : (std_err ? std_err : "unknown"));
            audit_error_owned = g_strdup_printf("qemu-img convert failed: %s",
                error ? error->message : (std_err ? std_err : "unknown"));
            audit_error = audit_error_owned;
            if (error) g_error_free(error);
            g_free(std_err);
            goto ova_cleanup;
        }
        g_free(std_err);
    }

                       
    gint64 vmdk_size = 0;
    {
        GFile *f = g_file_new_for_path(vmdk_path);
        GFileInfo *fi = g_file_query_info(f, G_FILE_ATTRIBUTE_STANDARD_SIZE,
            G_FILE_QUERY_INFO_NONE, NULL, NULL);
        if (fi) {
            vmdk_size = g_file_info_get_size(fi);
            g_object_unref(fi);
        }
        g_object_unref(f);
    }

                             
    pcv_job_progress(ctx->job_ref, 65,
                          "Generating OVF manifest");
    {
        ovf_name = g_strdup_printf("%s.ovf", ctx->vm_name);
        ovf_path = g_strdup_printf("%s/%s", tmpdir, ovf_name);
        gint disk_gb = (gint)(vmdk_size / (1024LL * 1024 * 1024)) + 1;

        gchar *ovf_content = g_strdup_printf(
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<Envelope xmlns=\"http://schemas.dmtf.org/ovf/envelope/1\"\n"
            "  xmlns:ovf=\"http://schemas.dmtf.org/ovf/envelope/1\"\n"
            "  xmlns:rasd=\"http://schemas.dmtf.org/wbem/wscim/1/cim-schema/2/CIM_ResourceAllocationSettingData\"\n"
            "  xmlns:vssd=\"http://schemas.dmtf.org/wbem/wscim/1/cim-schema/2/CIM_VirtualSystemSettingData\">\n"
            "  <References>\n"
            "    <File ovf:href=\"%s\" ovf:id=\"file1\" ovf:size=\"%ld\"/>\n"
            "  </References>\n"
            "  <DiskSection>\n"
            "    <Info>Virtual disk information</Info>\n"
            "    <Disk ovf:capacity=\"%d\" ovf:capacityAllocationUnits=\"byte * 2^30\"\n"
            "      ovf:diskId=\"vmdisk1\" ovf:fileRef=\"file1\"\n"
            "      ovf:format=\"http://www.vmware.com/interfaces/specifications/vmdk.html#streamOptimized\"/>\n"
            "  </DiskSection>\n"
            "  <VirtualSystem ovf:id=\"%s\">\n"
            "    <Info>PureCVisor exported VM</Info>\n"
            "    <Name>%s</Name>\n"
            "    <VirtualHardwareSection>\n"
            "      <Info>Virtual hardware requirements</Info>\n"
            "      <System>\n"
            "        <vssd:ElementName>Virtual Hardware Family</vssd:ElementName>\n"
            "        <vssd:VirtualSystemType>vmx-13</vssd:VirtualSystemType>\n"
            "      </System>\n"
            "      <Item>\n"
            "        <rasd:Description>Number of Virtual CPUs</rasd:Description>\n"
            "        <rasd:ElementName>%d virtual CPU(s)</rasd:ElementName>\n"
            "        <rasd:ResourceType>3</rasd:ResourceType>\n"
            "        <rasd:VirtualQuantity>%d</rasd:VirtualQuantity>\n"
            "      </Item>\n"
            "      <Item>\n"
            "        <rasd:AllocationUnits>byte * 2^20</rasd:AllocationUnits>\n"
            "        <rasd:Description>Memory Size</rasd:Description>\n"
            "        <rasd:ElementName>%d MB of memory</rasd:ElementName>\n"
            "        <rasd:ResourceType>4</rasd:ResourceType>\n"
            "        <rasd:VirtualQuantity>%d</rasd:VirtualQuantity>\n"
            "      </Item>\n"
            "    </VirtualHardwareSection>\n"
            "  </VirtualSystem>\n"
            "</Envelope>\n",
            vmdk_name, (long)vmdk_size, disk_gb,
            ctx->vm_name, ctx->vm_name,
            vcpus, vcpus,
            mem_mb, mem_mb
        );

        GError *write_error = NULL;
        if (!g_file_set_contents(ovf_path, ovf_content, -1, &write_error)) {
            audit_error_owned = g_strdup_printf("failed to write OVF manifest: %s",
                write_error ? write_error->message : "unknown");
            audit_error = audit_error_owned;
            g_clear_error(&write_error);
            g_free(ovf_content);
            goto ova_cleanup;
        }
        g_free(ovf_content);

                                 
        mf_name = g_strdup_printf("%s.mf", ctx->vm_name);
        mf_path = g_strdup_printf("%s/%s", tmpdir, mf_name);
        gchar *ovf_hash  = _ova_sha256_file(ovf_path);
        gchar *vmdk_hash = _ova_sha256_file(vmdk_path);
        if (!ovf_hash || !vmdk_hash) {
            audit_error = "failed to calculate OVA manifest checksums";
            g_free(ovf_hash);
            g_free(vmdk_hash);
            goto ova_cleanup;
        }
        gchar *mf_content = g_strdup_printf(
            "SHA256(%s)= %s\nSHA256(%s)= %s\n",
            ovf_name, ovf_hash,
            vmdk_name, vmdk_hash
        );
        write_error = NULL;
        if (!g_file_set_contents(mf_path, mf_content, -1, &write_error)) {
            audit_error_owned = g_strdup_printf("failed to write OVA manifest: %s",
                write_error ? write_error->message : "unknown");
            audit_error = audit_error_owned;
            g_clear_error(&write_error);
            g_free(mf_content); g_free(ovf_hash); g_free(vmdk_hash);
            goto ova_cleanup;
        }
        g_free(mf_content); g_free(ovf_hash); g_free(vmdk_hash);

                                 
        ova_path = g_strdup_printf("%s/%s.ova", ctx->output_dir, ctx->vm_name);
        pcv_job_progress(ctx->job_ref, 85,
                              "Creating OVA archive");
        {
            const gchar *argv[] = {"tar", "-cf", ova_path, "-C", tmpdir,
                ovf_name, vmdk_name, mf_name, NULL};
            gchar *std_err = nullptr;
            GError *error = nullptr;
            if (!pcv_spawn_sync(argv, NULL, &std_err, &error)) {
                g_warning("[OVA] tar failed: %s",
                    error ? error->message : (std_err ? std_err : "unknown"));
                audit_error_owned = g_strdup_printf("tar failed: %s",
                    error ? error->message : (std_err ? std_err : "unknown"));
                audit_error = audit_error_owned;
                if (error) g_error_free(error);
                g_free(std_err);
                goto ova_cleanup;
            }
            g_free(std_err);
        }

        if (!g_file_test(ova_path, G_FILE_TEST_IS_REGULAR)) {
            audit_error = "OVA archive was not created";
            goto ova_cleanup;
        }
        g_message("[OVA] Export complete: %s", ova_path);
        audit_ok = TRUE;
    }

ova_cleanup:
    if (!audit_ok && !audit_error)
        audit_error = "OVA export failed";
    pcv_job_progress(ctx->job_ref, 95,
                          "Cleaning up temporary export files");

    if (ovf_path) g_remove(ovf_path);
    if (mf_path) g_remove(mf_path);
    if (vmdk_path) g_remove(vmdk_path);
    if (tmpdir) g_rmdir(tmpdir);

    _ova_export_record_result(ctx, audit_ok, ova_path, audit_error, pcv_drain_task_actor(task));

    g_free(disk_path); g_free(disk_format);
    g_free(vmdk_name); g_free(vmdk_path);
    g_free(ovf_name); g_free(ovf_path);
    g_free(mf_name); g_free(mf_path);
    g_free(ova_path);
    g_free(tmpdir);
    g_free(audit_error_owned);
    g_task_return_boolean(task, audit_ok);
}

                                                                           
                                                            
                                                  
                                                         
void handle_vm_export_ova(JsonObject *params, const gchar *rpc_id,
                                   UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;
    if (!name || !name[0]) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing required parameter: name");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }

    const gchar *output_dir = json_object_has_member(params, "output_dir")
        ? json_object_get_string_member(params, "output_dir") : "/tmp";

                                        
    gchar *real_out = realpath(output_dir, NULL);
    if (!real_out) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid output_dir — directory does not exist");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }

                  
    PcvJobRef *job_ref = pcv_job_admit("ova_export", name, NULL, "vm.export.ova");
                                                                
    if (!job_ref) {
        free(real_out);
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
                                                 "Job persistence unavailable — OVA export rejected");
        pure_uds_server_send_response(server, connection, e);
        g_free(e);
        return;
    }

                                           
    gchar *job_id = g_strdup(pcv_job_id(job_ref));
    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "status", "accepted");
    json_object_set_string_member(obj, "vm", name);
    json_object_set_string_member(obj, "output_dir", real_out);
    json_object_set_string_member(obj, "format", "ova");
    json_object_set_string_member(obj, "job_id", job_id);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, obj);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);

                      
    OvaExportCtx *ctx = g_new0(OvaExportCtx, 1);
    ctx->vm_name = g_strdup(name);
    ctx->output_dir = g_strdup(real_out);
    ctx->job_id = g_strdup(job_id);
    ctx->job_ref = job_ref;             
    g_free(job_id);
    free(real_out);

    GTask *task = pcv_drain_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(task, ctx, _free_ova_ctx);
    g_task_run_in_thread(task, _ova_export_worker);
    g_object_unref(task);
}

                                                                             
                                                    
  
                                                              
                                                          
                       
  
                                                                                   
  
             
                         
                                  
                                           
                                                                            
                  
                                                                                

typedef struct {
    gchar *ova_path;
    gchar *vm_name;
    gchar *pool;
    gchar *image_dir;
    gchar *job_id;
    PcvJobRef *job_ref;                                     
    PcvVmLock *lock;                                                               
    PcvResPin ova_pin;                                                                   
} OvaImportCtx;

                                                                       
                                                      
                                   
                                                                              
                                                                   
                                                      
                                 
static void _free_ova_import_ctx(gpointer data) {
    if (!data) return;
    OvaImportCtx *ctx = data;
    g_clear_pointer(&ctx->lock, pcv_vm_lock_release);
    g_free(ctx->ova_path);
    g_free(ctx->vm_name);
    g_free(ctx->pool);
    g_free(ctx->image_dir);
    pcv_job_unref(ctx->job_ref);
    g_free(ctx->job_id);
    g_free(ctx);
}

   
                                                      
                                                           
                                                            
                       
                                           
                                        
                                           
   
static gchar *
_ovf_extract_value(const gchar *xml, const gchar *tag, const gchar *after)
{
    const gchar *start = after ? after : xml;
    gchar *open_tag = g_strdup_printf("<%s>", tag);
    gchar *close_tag = g_strdup_printf("</%s>", tag);
    const gchar *p = strstr(start, open_tag);
    gchar *result = nullptr;
    if (p) {
        p += strlen(open_tag);
        const gchar *e = strstr(p, close_tag);
        if (e && e > p)
            result = g_strndup(p, (gsize)(e - p));
    }
    g_free(open_tag);
    g_free(close_tag);
    return result;
}

   
                                            
                                                                     
                    
                         
                             
                                        
   
static gchar *
_ovf_extract_attr(const gchar *xml, const gchar *tag, const gchar *attr)
{
    gchar *search = g_strdup_printf("<%s ", tag);
    const gchar *p = strstr(xml, search);
    g_free(search);
    if (!p) return NULL;

    gchar *attr_search = g_strdup_printf("%s=\"", attr);
    const gchar *a = strstr(p, attr_search);
    g_free(attr_search);
    if (!a) return NULL;

    a = strchr(a, '\"');
    if (!a) return NULL;
    a++;
    const gchar *e = strchr(a, '\"');
    if (!e || e <= a) return NULL;
    return g_strndup(a, (gsize)(e - a));
}

                                                                   
                                                                                             
                                                                  
                                                
static gboolean
_ova_import_destroy_zvol(const gchar *dataset)
{
    if (!dataset || !*dataset)
        return TRUE;

    GError *error = NULL;
    const gboolean ok = pcv_zfs_destroy_settling("-R", dataset, &error);
    if (!ok) {
        PCV_LOG_WARN("ova_import", "ZFS zvol cleanup failed for '%s': %s",
                     dataset, error ? error->message : "unknown");
    }
    g_clear_error(&error);
    return ok;
}

                                                          
                                                                
                                                               
                                                            
                                                               
                                                                          
                                                         
static void _ova_import_worker(GTask *task, gpointer source_obj,
                                gpointer task_data, GCancellable *cancellable)
{
    (void)source_obj; (void)cancellable;
    OvaImportCtx *ctx = task_data;
    gchar *tmpdir = nullptr;
    gchar *disk_path = nullptr;
    gchar *vmdk_path = nullptr;
    gchar *created_zvol_dataset = nullptr;
    gboolean audit_ok = FALSE;
    gchar *terminal_result = NULL;                                    
    const gchar *audit_error = NULL;
    gchar *audit_error_owned = NULL;

    pcv_job_progress(ctx->job_ref, 5, "Extracting OVA archive");

                       
    gchar tmpl[] = "/tmp/pcv-ova-import-XXXXXX";
    tmpdir = g_strdup(mkdtemp(tmpl));
    if (!tmpdir) {
        terminal_result = g_strdup("\"mkdtemp failed\"");
        g_warning("[OVA-Import] mkdtemp failed for %s", ctx->vm_name);
        audit_error = "mkdtemp failed";
        goto import_cleanup;
    }

                                                                                
    if (!pcv_res_pin_matches(ctx->ova_path, &ctx->ova_pin)) {
        g_warning("[OVA-Import] %s: OVA file changed after admission", ctx->vm_name);
        terminal_result = g_strdup("\"OVA file changed after admission\"");
        audit_error = "OVA file changed after admission";
        goto import_cleanup;
    }

                   
    {
        const gchar *argv[] = {"tar", "-xf", ctx->ova_path, "-C", tmpdir, NULL};
        gchar *std_err = nullptr;
        GError *error = nullptr;
        if (!pcv_spawn_sync(argv, NULL, &std_err, &error)) {
            g_warning("[OVA-Import] tar extraction failed: %s",
                error ? error->message : (std_err ? std_err : "unknown"));
            terminal_result = g_strdup("\"tar extraction failed\"");
            audit_error = "tar extraction failed";
            if (error) g_error_free(error);
            g_free(std_err);
            goto import_cleanup;
        }
        g_free(std_err);
    }
    pcv_job_progress(ctx->job_ref, 20, "Parsing OVF metadata");

                       
    gchar *ovf_path = nullptr;
    {
        GDir *dir = g_dir_open(tmpdir, 0, NULL);
        if (dir) {
            const gchar *entry;
            while ((entry = g_dir_read_name(dir)) != nullptr) {
                if (g_str_has_suffix(entry, ".ovf")) {
                    ovf_path = g_strdup_printf("%s/%s", tmpdir, entry);
                    break;
                }
            }
            g_dir_close(dir);
        }
    }
    if (!ovf_path) {
        g_warning("[OVA-Import] No .ovf file found in OVA for %s", ctx->vm_name);
        terminal_result = g_strdup("\"no .ovf file in OVA\"");
        audit_error = "no .ovf file in OVA";
        goto import_cleanup;
    }

                                                 
    if (!pcv_ova_import_member_is_regular(ovf_path)) {
        g_warning("[OVA-Import] %s: .ovf is not a regular file", ctx->vm_name);
        terminal_result = g_strdup("\"OVA member refused: .ovf is not a regular file\"");
        audit_error = "OVA member refused: .ovf is not a regular file";
        g_free(ovf_path);
        goto import_cleanup;
    }

                   
    gchar *ovf_content = nullptr;
    gsize ovf_len = 0;
    if (!g_file_get_contents(ovf_path, &ovf_content, &ovf_len, NULL) || !ovf_content) {
        g_warning("[OVA-Import] Failed to read OVF: %s", ovf_path);
        terminal_result = g_strdup("\"failed to read OVF\"");
        audit_error = "failed to read OVF";
        g_free(ovf_path);
        goto import_cleanup;
    }

                                                                          
    gint vcpus = 2;           
    {
        const gchar *cpu_marker = strstr(ovf_content, "<rasd:ResourceType>3</rasd:ResourceType>");
        if (cpu_marker) {
            gchar *val = _ovf_extract_value(ovf_content, "rasd:VirtualQuantity", cpu_marker);
            if (val) { vcpus = atoi(val); g_free(val); }
        }
        if (vcpus < 1) vcpus = 2;
    }

                                                                             
    gint memory_mb = 2048;           
    {
        const gchar *mem_marker = strstr(ovf_content, "<rasd:ResourceType>4</rasd:ResourceType>");
        if (mem_marker) {
            gchar *val = _ovf_extract_value(ovf_content, "rasd:VirtualQuantity", mem_marker);
            if (val) { memory_mb = atoi(val); g_free(val); }
        }
        if (memory_mb < 256) memory_mb = 2048;
    }

                                             
    gchar *vmdk_name = _ovf_extract_attr(ovf_content, "File", "ovf:href");
    g_free(ovf_content);
    g_free(ovf_path);

    if (!vmdk_name) {
        g_warning("[OVA-Import] No disk reference in OVF for %s", ctx->vm_name);
        terminal_result = g_strdup("\"no disk file in OVF\"");
        audit_error = "no disk file in OVF";
        goto import_cleanup;
    }

                                                                              
                                                                   
    if (!pcv_ova_import_member_name_is_safe(vmdk_name)) {
        g_warning("[OVA-Import] %s: disk reference is not a plain file name", ctx->vm_name);
        terminal_result = g_strdup("\"OVA member refused: disk reference is not a plain file name\"");
        audit_error = "OVA member refused: disk reference is not a plain file name";
        g_free(vmdk_name);
        goto import_cleanup;
    }
    vmdk_path = g_strdup_printf("%s/%s", tmpdir, vmdk_name);
    g_free(vmdk_name);
    if (!pcv_ova_import_member_is_regular(vmdk_path)) {
        g_warning("[OVA-Import] VMDK missing or not a regular file: %s", vmdk_path);
        terminal_result = g_strdup("\"OVA member refused: VMDK file not found or not a regular file\"");
        audit_error = "OVA member refused: VMDK file not found or not a regular file";
        goto import_cleanup;
    }

                                                                                     
                                                           
    {
        g_autofree gchar *vmdk_format = NULL;
        GError *guard_error = NULL;
        if (!pcv_disk_image_inspect(vmdk_path, PCV_DISK_IMAGE_OVA_VMDK, &vmdk_format, &guard_error)) {
            g_warning("[OVA-Import] %s: %s", ctx->vm_name, guard_error->message);
            terminal_result = g_strdup("\"OVA disk image refused\"");
            audit_error = "OVA disk image refused";
            g_clear_error(&guard_error);
            goto import_cleanup;
        }
    }

    pcv_job_progress(ctx->job_ref, 30,
        "Converting disk image (vmdk → target format)");

                                          
    gboolean use_zvol = FALSE;
    {
        const gchar *pool_argv[] = {"zfs", "list", "-H", "-o", "name",
                                    ctx->pool, NULL};
        use_zvol = pcv_spawn_sync(pool_argv, NULL, NULL, NULL);
    }

    if (use_zvol) {
                                                                               
                                      
        gchar *zvol_path = g_strdup_printf("/dev/zvol/%s/%s", ctx->pool, ctx->vm_name);
        gchar *zvol_name = g_strdup_printf("%s/%s", ctx->pool, ctx->vm_name);

                                     
        gchar *stdout_buf = nullptr;
        const gchar *info_argv[] = {"qemu-img", "info", "--output=json", vmdk_path, NULL};
        GError *error = nullptr;
        gint64 disk_bytes = 10LL * 1024 * 1024 * 1024;              
        if (pcv_spawn_sync(info_argv, &stdout_buf, NULL, &error)) {
                                                            
                                                            
            gint64 virtual_size = pcv_ova_import_virtual_size(stdout_buf);
            if (virtual_size > 0) disk_bytes = virtual_size;
        }
        if (error) g_error_free(error);
        g_free(stdout_buf);

        gchar *size_str = g_strdup_printf("%ldG", (long)(disk_bytes / (1024 * 1024 * 1024)) + 1);
        const gchar *zfs_options[] = {"-V", size_str, NULL};
        error = nullptr;
        gchar *std_err = nullptr;
        if (!pcv_zfs_create_locked(zfs_options, zvol_name, &std_err, &error)) {
            g_warning("[OVA-Import] zfs create failed: %s",
                error ? error->message : (std_err ? std_err : "unknown"));
            audit_error_owned = g_strdup_printf("zfs create failed: %s",
                error ? error->message : (std_err ? std_err : "unknown"));
            terminal_result = g_strdup("\"zfs create failed\"");
            audit_error = audit_error_owned;
            if (error) g_error_free(error);
            g_free(std_err);
            g_free(size_str);
            g_free(zvol_name);
            g_free(zvol_path);
            goto import_cleanup;
        }
        if (error) g_error_free(error);
        g_free(std_err);
        g_free(size_str);
        created_zvol_dataset = g_strdup(zvol_name);
        g_free(zvol_name);

                                                                  
                                                            
                                                            
        gboolean zvol_ready = FALSE;
        for (guint attempt = 0; attempt < 200; attempt++) {
            if (g_file_test(zvol_path, G_FILE_TEST_EXISTS)) {
                zvol_ready = TRUE;
                break;
            }
            g_usleep(50 * G_TIME_SPAN_MILLISECOND);
        }
        if (!zvol_ready) {
            g_warning("[OVA-Import] zvol device did not appear: %s", zvol_path);
            terminal_result = g_strdup("\"zvol device did not appear\"");
            audit_error = "zvol device did not appear";
            (void)_ova_import_destroy_zvol(created_zvol_dataset);
            g_clear_pointer(&created_zvol_dataset, g_free);
            g_free(zvol_path);
            goto import_cleanup;
        }

                           
          
                                                                   
                                                               
                                          
                                                   
                                          
        const gchar *conv_argv[] = {
            "qemu-img", "convert", "-n", "-f", "vmdk", "-O", "raw",
            vmdk_path, zvol_path, NULL
        };
        error = nullptr;
        std_err = nullptr;
        if (!pcv_spawn_sync(conv_argv, NULL, &std_err, &error)) {
            g_warning("[OVA-Import] qemu-img convert to zvol failed: %s",
                error ? error->message : (std_err ? std_err : "unknown"));
            terminal_result = g_strdup("\"disk conversion failed\"");
            audit_error = "disk conversion failed";
            if (created_zvol_dataset) {
                (void)_ova_import_destroy_zvol(created_zvol_dataset);
                g_clear_pointer(&created_zvol_dataset, g_free);
            }
            if (error) g_error_free(error);
            g_free(std_err);
            g_free(zvol_path);
            goto import_cleanup;
        }
        g_free(std_err);
        disk_path = zvol_path;
    } else {
                      
        disk_path = g_strdup_printf("%s/%s.qcow2", ctx->image_dir, ctx->vm_name);
        const gchar *conv_argv[] = {
            "qemu-img", "convert", "-f", "vmdk", "-O", "qcow2",
            vmdk_path, disk_path, NULL
        };
        GError *error = nullptr;
        gchar *std_err = nullptr;
        if (!pcv_spawn_sync(conv_argv, NULL, &std_err, &error)) {
            g_warning("[OVA-Import] qemu-img convert to qcow2 failed: %s",
                error ? error->message : (std_err ? std_err : "unknown"));
            terminal_result = g_strdup("\"disk conversion failed\"");
            audit_error = "disk conversion failed";
            if (error) g_error_free(error);
            g_free(std_err);
            goto import_cleanup;
        }
        g_free(std_err);
    }
    g_clear_pointer(&vmdk_path, g_free);

    pcv_job_progress(ctx->job_ref, 80,
                          "Generating and normalizing libvirt domain XML");

                                                            
                                                               
                                                                    
                                                                         
                                                              
                                                    
                                               
    {
        gchar *vcpu_str = g_strdup_printf("%d", vcpus);
        gchar *mem_str = g_strdup_printf("%d", memory_mb);
        const gchar *disk_format = use_zvol ? "raw" : "qcow2";
        gchar *disk_arg = g_strdup_printf("path=%s,format=%s,bus=virtio",
                                          disk_path, disk_format);
        const gchar *argv[] = {
            "virt-install", "--connect", pcv_config_get_libvirt_uri(),
            "--name", ctx->vm_name,
            "--vcpus", vcpu_str, "--memory", mem_str,
            "--disk", disk_arg, "--import",
            "--os-variant", "generic",
            "--noautoconsole", "--nographics", "--print-xml", NULL
        };
        GError *error = nullptr;
        gchar *domain_xml_generated = nullptr;
        gchar *std_err = nullptr;
        if (!pcv_spawn_sync(argv, &domain_xml_generated, &std_err, &error)) {
            g_warning("[OVA-Import] virt-install XML generation failed for %s: %s", ctx->vm_name,
                error ? error->message : (std_err ? std_err : "unknown"));
            terminal_result = g_strdup("\"virt-install failed\"");
            audit_error = "virt-install XML generation failed";
            goto import_define_fail;
        }

        gchar *domain_xml = pcv_ova_import_normalize_domain_xml(
            domain_xml_generated, disk_path, use_zvol, disk_format, &error);
        if (!domain_xml) {
            g_warning("[OVA-Import] domain XML normalization failed for %s: %s",
                      ctx->vm_name, error ? error->message : "unknown");
            terminal_result = g_strdup("\"domain XML normalization failed\"");
            audit_error = "domain XML normalization failed";
            goto import_define_fail;
        }

        pcv_job_progress(ctx->job_ref, 90,
                              "Defining imported VM as shutoff domain");
        virConnectPtr conn = virt_conn_pool_acquire();
        virDomainPtr imported = conn ? virDomainDefineXML(conn, domain_xml) : NULL;
        if (!imported) {
            virErrorPtr verr = virGetLastError();
            audit_error_owned = g_strdup_printf("libvirt define failed: %s",
                verr && verr->message ? verr->message : "unknown");
            audit_error = audit_error_owned;
            g_warning("[OVA-Import] %s", audit_error_owned);
            terminal_result = g_strdup("\"libvirt define failed\"");
            if (conn) virt_conn_pool_release(conn);
            g_free(domain_xml);
            goto import_define_fail;
        }
        virDomainFree(imported);
        virt_conn_pool_release(conn);
        g_free(domain_xml);

        g_clear_error(&error);
        g_free(vcpu_str);
        g_free(mem_str);
        g_free(disk_arg);
        g_free(domain_xml_generated);
        g_free(std_err);
        goto import_define_done;

import_define_fail:
        g_clear_error(&error);
        g_free(vcpu_str);
        g_free(mem_str);
        g_free(disk_arg);
        g_free(domain_xml_generated);
        g_free(std_err);
        if (created_zvol_dataset) {
            (void)_ova_import_destroy_zvol(created_zvol_dataset);
            g_clear_pointer(&created_zvol_dataset, g_free);
        } else if (disk_path) {
            g_remove(disk_path);
        }
        goto import_cleanup;

import_define_done:
        ;
    }

    pcv_job_progress(ctx->job_ref, 95, "Cleaning up temporary files");
    g_message("[OVA-Import] Successfully imported %s (vcpus=%d, mem=%dMB)",
        ctx->vm_name, vcpus, memory_mb);

    {
        gchar *result = g_strdup_printf(
            "{\"vm\":\"%s\",\"vcpus\":%d,\"memory_mb\":%d,\"disk\":\"%s\"}",
            ctx->vm_name, vcpus, memory_mb, disk_path ? disk_path : "");
        terminal_result = result;
        audit_ok = TRUE;
    }

import_cleanup:
    if (!audit_ok && !audit_error)
        audit_error = "OVA import failed";
    pcv_audit_log(pcv_drain_task_actor(task), "vm.import.ova", ctx->vm_name,
                  audit_ok ? "ok" : "fail", audit_ok ? 0 : PURE_RPC_ERR_ZFS_OPERATION,
                  0, "local");
    pcv_ws_finish_job(ctx->job_ref, audit_ok ? PCV_JOB_COMPLETED : PCV_JOB_FAILED,
                       terminal_result, audit_ok ? NULL : audit_error);
    g_free(terminal_result);
    g_free(disk_path);
    g_free(vmdk_path);
    g_free(created_zvol_dataset);
    g_free(audit_error_owned);
                    
    if (tmpdir) {
        gchar *rm_tmpdir = g_strdup(tmpdir);
        const gchar *rm_argv[] = {"rm", "-rf", rm_tmpdir, NULL};
        pcv_spawn_fire(rm_argv);
        g_free(rm_tmpdir);
        g_free(tmpdir);
    }
    g_task_return_boolean(task, audit_ok);
}

                                                                    
                                                            
                                                                    
                                                                 
                                                
                                                     
                                  
                                                             
                                                                       
                                                                   
                                                                                   
                                                           
                                                             
                                                           
                                                                            
                                                                                            
                                                                                             
                                                                                     
                                                                                          
                                                                                       
                       
void handle_vm_import_ova(JsonObject *params, const gchar *rpc_id,
                                   UdsServer *server, GSocketConnection *connection)
{
    const gchar *ova_path = json_object_has_member(params, "ova_path")
        ? json_object_get_string_member(params, "ova_path") : NULL;
    const gchar *name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;

    if (!ova_path || !ova_path[0] || !name || !name[0]) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing required parameters: ova_path, name");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }

                  
    if (!pcv_validate_vm_name(name)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid VM name — must be alphanumeric/hyphen/underscore, 1-63 chars");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }

                                                      
    if (!pcv_validate_new_vm_name(name)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "VM name must not look like a UUID");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }

                                                                             
    g_autofree gchar *ova_canon = NULL;                                            
    PcvResPin ova_pin = { 0 };                                                                   
    if (!pcv_res_gate(params, connection, "vm.import.ova", "ova_path", PCV_RES_IMAGE, ova_path, &ova_canon, &ova_pin,
                      rpc_id, server))
        return;
    ova_path = ova_canon;

                                       
    gchar *real_ova = realpath(ova_path, NULL);
    if (!real_ova) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "OVA file not found or invalid path");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }
    if (!g_str_has_prefix(real_ova, "/tmp/") &&
        !g_str_has_prefix(real_ova, "/pcvpool/") &&
        !g_str_has_prefix(real_ova, "/var/lib/")) {
        free(real_ova);
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "OVA path not in allowed directories (/tmp, /pcvpool, /var/lib)");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }
    if (!g_file_test(real_ova, G_FILE_TEST_IS_REGULAR)) {
        free(real_ova);
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "OVA path is not a regular file");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }

    const gchar *pool = json_object_has_member(params, "pool")
        ? json_object_get_string_member(params, "pool") : NULL;
                                                                    
                                        
    if (pool && pool[0] && !pcv_disp_valid_vm_storage_pool(pool)) {
        free(real_ova);
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid pool — must be a relative ZFS dataset path ([A-Za-z0-9_.-] segments)");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }
    g_autofree gchar *pool_canon = NULL;                                        
    if (!pcv_res_gate(params, connection, "vm.import.ova", "pool", PCV_RES_STORAGE, pool, &pool_canon, NULL, rpc_id,
                      server)) {
        free(real_ova);
        return;
    }
    if (pool_canon)
        pool = pool_canon;
    if (!pool || !pool[0])
        pool = pcv_config_get_zvol_pool();
    const gchar *image_dir = pcv_config_get_image_dir();

                                                               
                                                                       
                                                  
    PcvVmLock *op_lock = NULL;
    {
        gchar *import_lock_err = NULL;
        op_lock = pcv_vm_lock_acquire(name, VM_OP_CREATING, &import_lock_err);
        if (!op_lock) {
            free(real_ova);
            gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
                import_lock_err ? import_lock_err : "VM is busy (another operation in progress)");
            pure_uds_server_send_response(server, connection, e);
            g_free(e); g_free(import_lock_err);
            return;
        }
    }

                                                                     
    {
        virConnectPtr conn = virt_conn_pool_acquire();
        if (!conn) {
            pcv_vm_lock_release(op_lock);
            free(real_ova);
            gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_CONFLICT,
                "Failed to acquire libvirt connection");
            pure_uds_server_send_response(server, connection, e); g_free(e);
            return;
        }

        virDomainPtr existing = virDomainLookupByName(conn, name);
        if (existing) {
            virDomainFree(existing);
            virt_conn_pool_release(conn);
            pcv_vm_lock_release(op_lock);
            free(real_ova);
            gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
                "Target VM already exists");
            pure_uds_server_send_response(server, connection, e); g_free(e);
            return;
        }
        virResetLastError();
        virt_conn_pool_release(conn);

        gboolean disk_exists = FALSE;
        gchar *dataset = g_strdup_printf("%s/%s", pool, name);
        const gchar *zfs_argv[] = {"zfs", "list", "-H", "-o", "name", dataset, NULL};
        disk_exists = pcv_spawn_sync(zfs_argv, NULL, NULL, NULL);
        g_free(dataset);

        if (!disk_exists) {
            gchar *qcow2_path = g_strdup_printf("%s/%s.qcow2", image_dir, name);
            gchar *raw_img_path = g_strdup_printf("%s/%s.img", image_dir, name);
            gchar *raw_path = g_strdup_printf("%s/%s.raw", image_dir, name);
            disk_exists = g_file_test(qcow2_path, G_FILE_TEST_EXISTS) ||
                          g_file_test(raw_img_path, G_FILE_TEST_EXISTS) ||
                          g_file_test(raw_path, G_FILE_TEST_EXISTS);
            g_free(qcow2_path);
            g_free(raw_img_path);
            g_free(raw_path);
        }

        if (disk_exists) {
            pcv_vm_lock_release(op_lock);
            free(real_ova);
            gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
                "Target VM disk already exists");
            pure_uds_server_send_response(server, connection, e); g_free(e);
            return;
        }
    }

                 
    PcvJobRef *job_ref = pcv_job_admit("ova_import", name, NULL, "vm.import.ova");
                                                                     
    if (!job_ref) {
        pcv_vm_lock_release(op_lock);
        free(real_ova);
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
                                                 "Job persistence unavailable — OVA import rejected");
        pure_uds_server_send_response(server, connection, e);
        g_free(e);
        return;
    }

                                           
    gchar *job_id = g_strdup(pcv_job_id(job_ref));
    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "status", "accepted");
    json_object_set_string_member(obj, "vm", name);
    json_object_set_string_member(obj, "ova_path", real_ova);
    json_object_set_string_member(obj, "job_id", job_id);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, obj);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);

                      
    OvaImportCtx *ctx = g_new0(OvaImportCtx, 1);
    ctx->ova_path = g_strdup(real_ova);
    ctx->vm_name = g_strdup(name);
    ctx->pool = g_strdup(pool);
    ctx->image_dir = g_strdup(image_dir);
    ctx->job_id = g_strdup(job_id);
    ctx->job_ref = job_ref;             
    ctx->lock = op_lock;                                                    
    op_lock = NULL;
    ctx->ova_pin = ova_pin;                               
    free(real_ova);
    g_free(job_id);

    GTask *itask = pcv_drain_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(itask, ctx, _free_ova_import_ctx);
    g_task_run_in_thread(itask, _ova_import_worker);
    g_object_unref(itask);
}
