   
                           
                                                                
  
                           
                                                   
                                                    
                                        
  
                                                       
                                                             
                                                            
                                                             
                                                        
                                                 
  
                                                                    
                                                        
                                                   
  
            
                                                                                
                                                                                           
                                                                                        
  
                 
                                                    
                                                       
                                                   
  
                          
                                                        
                                                 
                                                         
  
         
                                                                      
                                    
                                                                       
                                                                
                                                      
  
          
                               
                                            
   
#include <glib.h>
#include <gio/gio.h>
#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <stdlib.h>

#include "api/uds_server.h"
#include "modules/dispatcher/rpc_utils.h"
#include "modules/core/vm_state.h"
#include "modules/core/cpu_allocator.h"
#include "modules/virt/virt_conn_pool.h"
#include "modules/virt/vm_manager.h"                                                     
                                                                                                    
                                                                                           
#include "modules/audit/pcv_audit.h"
#include "api/ws_server.h"
#include "../network/security_group.h"
#include "../network/tenant_overlay.h"
#include "../network/network_dhcp.h"                                                                     
#include "../network/network_manager.h"                                     
#include "../network/pcv_qos.h"                                                                
#include "utils/pcv_validate.h"                                                                     
                                                                                              

  
                                                         
                                                          
                                           
   
extern virDomainPtr pure_virt_get_domain(virConnectPtr conn, const gchar *identifier);

                                                                
#define MAX_PHYSICAL_CPUS 256

   
                  
                                  
  
                               
                                            
                                    
                                              
                                            
                                                       
  
                                                          
                                                                     
   
typedef struct {
    gchar *vm_id;                                                                      
    gchar *alloc_key;                                                                  
                                                                                
                                                                
    gchar *bridge_name;                                                 
                                                                   
                                                                
                                                                 
    GArray *allocated_cpus;                                             
    gint numa_node;                                                      
    gchar *rpc_id;                                    
    UdsServer *server;                                           
    GSocketConnection *connection;                                  
    gint64 worker_start_us;                                                    
} VmStartContext;

   
                         
                                     
                                                 
                                                         
                                                                                
   
static void free_vm_start_context(gpointer data) {
    if (!data) return;
    VmStartContext *ctx = (VmStartContext *)data;
    g_free(ctx->vm_id);
    g_free(ctx->alloc_key);
    g_free(ctx->bridge_name);
    g_free(ctx->rpc_id);
    if (ctx->allocated_cpus) g_array_unref(ctx->allocated_cpus);
    if (ctx->server) g_object_unref(ctx->server);
    if (ctx->connection) g_object_unref(ctx->connection);
    g_free(ctx);
}

   
                               
                                                            
                                                        
                                                      
                                                    
  
                                                                       
                                                              
                                                       
  
                      
                                                                           
                                                                         
                                                                     
                                            
  
                            
                                                                         
                                                                
                                                                          
                                                                                         
                                                             
                                                                                    
                                                                      
                                                              
                                                                          
                                                    
  
                                
                                                              
                                                             
                                                  
                                                              
            
  
                                                                          
                                                                           
                                                            
                                                                      
                                                                             
  
                                                                    
                                                                          
                                                      
                                                                       
                                                 
                                                 
  
                                            
                                                                
                                                                            
                                       
                                                                   
                                                                     
   
static gboolean
_orchestrate_tenant_overlay(VmStartContext *ctx, virDomainPtr dom,
                            const gchar *canonical_name, GError **error)
{
                                                            
                                                           
                                                    
    char *meta = virDomainGetMetadata(dom, VIR_DOMAIN_METADATA_ELEMENT,
                                      PCV_OVERLAY_METADATA_URI, 0);
    if (!meta) {
                                                                  
                                                                           
                                                               
                                                             
        virResetLastError();
        return TRUE;
    }
    gchar *mode = NULL, *tenant = NULL;
    gboolean parsed = _overlay_metadata_parse(meta, &mode, &tenant);
                                                                     
                                                                  
    free(meta);                                                              
    if (!parsed) {
                                                                         
                                                                      
                                                                  
                                                                  
                                                               
                                                            
                                                                
                                                 
        g_free(mode);
        g_free(tenant);
        g_warning("[vm.start] VM '%s': overlay metadata 존재하나 파싱 불가 "
                  "(writer 버그 또는 XML 변조 의심) — fail-closed", ctx->vm_id);
                                                                
                                                           
        if (virDomainDestroy(dom) != 0) {
            g_warning("[vm.start] VM '%s': fail-closed destroy 실패 — VM이 미배선 상태로 "
                      "실행 중일 수 있음, 수동 조치 필요", ctx->vm_id);
        }
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "tenant-overlay VM '%s': overlay metadata 파싱 실패 "
                    "(존재하나 network_mode/tenant 복원 불가) — fail-closed", ctx->vm_id);
        return FALSE;
    }
    if (g_strcmp0(mode, "tenant-overlay") != 0) {
        g_free(mode);
        g_free(tenant);
        return TRUE;                                                    
    }
    g_free(mode);                          

                                                                
                                                            
                                                                  
    if (!canonical_name || !*canonical_name) {
        g_free(tenant);
                                                                    
                                                     
        if (virDomainDestroy(dom) != 0) {
            g_warning("[vm.start] VM '%s': fail-closed destroy 실패 — VM이 미배선 상태로 "
                      "실행 중일 수 있음, 수동 조치 필요", ctx->vm_id);
        }
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "tenant-overlay VM '%s': canonical domain name 없음 "
                    "(overlay attach 키가 dangling) — fail-closed", ctx->vm_id);
        return FALSE;
    }

                                             
                                                         
                                                           
    GError *lerr = NULL;
    gchar *overlay_ip = NULL, *ep_name = NULL, *subnet = NULL, *gw_cidr = NULL;
    gchar *tap = NULL, *mac = NULL, *live_xml = NULL;
    gboolean attached = FALSE;                                         

                                                             
    overlay_ip = pcv_tenant_overlay_attach_vm(tenant, canonical_name, &lerr);
    if (!overlay_ip)
        goto fail;
    attached = TRUE;

                                          
    if (!pcv_tenant_overlay_get_member_ep(tenant, canonical_name, &ep_name) || !ep_name) {
        g_set_error(&lerr, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "get_member_ep('%s','%s') 실패", tenant, canonical_name);
        goto fail;
    }

                                                      
    live_xml = virDomainGetXMLDesc(dom, 0);
    if (!live_xml) {
        virErrorPtr ve = virGetLastError();
        g_set_error(&lerr, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "virDomainGetXMLDesc 실패: %s", ve ? ve->message : "unknown");
        goto fail;
    }
    if (!_overlay_live_iface_parse(live_xml, &tap, &mac)) {
        g_set_error(&lerr, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "라이브 XML 에서 tenant-overlay ethernet tap/MAC 발견 실패 "
                    "(iface 부재 또는 target dev 미배정)");
        goto fail;
    }

                                                                   
                                                                    
                                                                      
                                                        
                                                                         
                                                                         
    if (!pcv_validate_bridge_name(tap)) {
        g_set_error(&lerr, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "라이브 XML 발견 tap 이름 화이트리스트 검증 실패: %s", tap);
        goto fail;
    }
    if (!pcv_validate_mac(mac)) {
        g_set_error(&lerr, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "라이브 XML 발견 guest MAC 화이트리스트 검증 실패: %s", mac);
        goto fail;
    }

                                           
    if (!pcv_tenant_overlay_get_subnet(tenant, &subnet) || !subnet) {
        g_set_error(&lerr, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "get_subnet('%s') 실패", tenant);
        goto fail;
    }
    gw_cidr = _overlay_gw_cidr_from_subnet(subnet);
    if (!gw_cidr) {
        g_set_error(&lerr, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "게이트웨이 CIDR 파생 실패(subnet='%s')", subnet);
        goto fail;
    }

                                                                    
    if (!pcv_tenant_overlay_wg_attach_tap(ep_name, tap, overlay_ip, gw_cidr, &lerr))
        goto fail;

                                                                
    if (!network_dhcp_reserve_overlay_ip(ep_name, tap, gw_cidr, mac, overlay_ip, &lerr))
        goto fail;

    g_message("[vm.start] VM '%s' tenant-overlay '%s' 배선 완료 "
              "(ep=%s tap=%s overlay_ip=%s gw=%s mac=%s)",
              canonical_name, tenant, ep_name, tap, overlay_ip, gw_cidr, mac);

    g_free(overlay_ip); g_free(ep_name); g_free(subnet); g_free(gw_cidr);
    g_free(tap); g_free(mac); g_free(live_xml); g_free(tenant);
    return TRUE;

fail:
                                                           
                                                        
                                                                
                                                                
                                                      
    if (ep_name)
        network_dhcp_release_overlay_ip(ep_name, NULL);
    if (attached) {
        GError *derr = NULL;
        if (!pcv_tenant_overlay_detach_vm(tenant, canonical_name, &derr)) {
            g_warning("[vm.start] VM '%s' overlay 롤백 detach 실패(dangling 가능): %s",
                      canonical_name, derr && derr->message ? derr->message : "unknown");
            g_clear_error(&derr);
        }
    }
                                                                       
                                                                 
                                                      
    if (virDomainDestroy(dom) != 0) {
        g_warning("[vm.start] VM '%s': fail-closed destroy 실패 — VM이 미배선 상태로 "
                  "실행 중일 수 있음, 수동 조치 필요", canonical_name);
    }

    g_propagate_error(error, lerr);
    g_free(overlay_ip); g_free(ep_name); g_free(subnet); g_free(gw_cidr);
    g_free(tap); g_free(mac); g_free(live_xml); g_free(tenant);
    return FALSE;
}

   
                          
                                   
  
                                                           
                                                                 
                                                            
  
          
                  
                                                   
                                              
                                           
                                          
                                                           
                                                                
                                                                     
  
                       
                                             
                                         
                                             
  
                                                         
   
static void vm_start_worker_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    VmStartContext *ctx = (VmStartContext *)task_data;
    GError *error = NULL;
                                                                   
                                                     
                                                       
                                                
                               
    gchar *canonical_name = NULL;

                                 
    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to connect to Libvirt daemon.");
        g_task_return_error(task, error);
        return;
    }

                                          
    virDomainPtr dom = pure_virt_get_domain(conn, ctx->vm_id);
    if (!dom) {
        g_set_error(&error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Entity '%s' not found.", ctx->vm_id);
        goto cleanup_conn;
    }

                                                                
                                               
                                                         
                              
    canonical_name = g_strdup(virDomainGetName(dom));

                                                                
                                                                         
                                                          
                                                          
                                                      
    {
        virDomainInfo info;
        if (virDomainGetInfo(dom, &info) == 0 &&
            (info.state == VIR_DOMAIN_RUNNING || info.state == VIR_DOMAIN_BLOCKED)) {
            g_message("[vm.start] VM '%s': already running (idempotent no-op)", ctx->vm_id);
            virDomainFree(dom);
            virt_conn_pool_release(conn);
                                                              
            pcv_security_group_sync_vm(ctx->vm_id);
                                                                     
                                                               
                                                              
                                                               
                                                            
                                                       
            g_free(canonical_name);
            g_task_return_boolean(task, TRUE);
            return;
        }
    }

                                              
      
                                                           
                                                         
                                                  
      
                                                              
                                                       
                                               
      
                                                         
                                                   
      
                                                        
       
    if (ctx->numa_node >= 0) {
        char *xml = virDomainGetXMLDesc(dom, VIR_DOMAIN_XML_INACTIVE);
        if (xml) {
                                                         
            if (!strstr(xml, "<numatune>")) {
                gchar *numatune_xml = g_strdup_printf(
                    "  <numatune>\n"
                    "    <memory mode='strict' nodeset='%d'/>\n"
                    "  </numatune>\n", ctx->numa_node);

                                                 
                char *end = strstr(xml, "</domain>");
                if (end) {
                                                                    
                                                                       
                                                                              
                    gchar *patched = g_strdup_printf("%.*s%s%s",
                        (gint)(end - xml), xml, numatune_xml, end);

                    virDomainPtr new_dom = virDomainDefineXML(conn, patched);
                    if (new_dom) {
                        virDomainFree(dom);
                        dom = new_dom;
                        g_message("[vm.start] NUMA memory binding applied: node %d for VM '%s'",
                                  ctx->numa_node, ctx->vm_id);
                    } else {
                        g_warning("[vm.start] Failed to apply numatune for VM '%s', continuing without",
                                  ctx->vm_id);
                    }
                    g_free(patched);
                }
                g_free(numatune_xml);
            }
            free(xml);                                                   
        }
    }

                                                                      
    if (virDomainCreate(dom) < 0) {
        virErrorPtr err = virGetLastError();
        g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to start VM: %s", err ? err->message : "Unknown error");
        goto cleanup_dom;
    }

      
                          
      
                                                                  
                                                                          
                                            
      
                                                              
                                                           
                                             
      
                                                    
                                          
       
    int maplen = VIR_CPU_MAPLEN(MAX_PHYSICAL_CPUS);
    if (ctx->allocated_cpus) {
        for (guint i = 0; i < ctx->allocated_cpus->len; i++) {
            guint pcpu_id = g_array_index(ctx->allocated_cpus, guint, i);
            unsigned char *cpumap = g_malloc0(maplen);                    
            VIR_USE_CPU(cpumap, pcpu_id);                         

                                                                   
            if (virDomainPinVcpuFlags(dom, i, cpumap, maplen, VIR_DOMAIN_AFFECT_LIVE) < 0) {
                g_warning("Failed to pin vCPU %u to pCPU %u. Continuing...", i, pcpu_id);
            }
            g_free(cpumap);
        }
    }

      
                               
      
                                                       
                                        
                                   
      
                                           
                                           
       
                                                         
                                                      
                                  
    if (ctx->bridge_name && strlen(ctx->bridge_name) > 0) {
        GString *net_xml = g_string_new("<interface type='bridge'>\n");
        g_string_append_printf(net_xml, "  <source bridge='%s'/>\n", ctx->bridge_name);
        g_string_append(net_xml, "  <model type='virtio'/>\n");
                          
                                                                   
                                                                   
                                                                    
                                                              
                                          
          
                         
                                                            
                                                         
                                                                     
        g_string_append_printf(net_xml,
            "  <driver name='vhost' queues='%u' rx_queue_size='1024' tx_queue_size='256'/>\n",
            ctx->allocated_cpus ? ctx->allocated_cpus->len : 1);
                                                                   
        gint bridge_mtu = pcv_bridge_mtu_read(ctx->bridge_name, NULL);
        if (bridge_mtu > 0)
            g_string_append_printf(net_xml, "  <mtu size='%d'/>\n", bridge_mtu);
        gchar *uplink_mode = NULL;
        GError *uplink_error = NULL;
        if (!pcv_network_bridge_uplink_mode(
                ctx->bridge_name, &uplink_mode, &uplink_error)) {
            g_warning("[vm.start] uplink mode lookup failed for %s: %s",
                      ctx->bridge_name,
                      uplink_error ? uplink_error->message : "unknown");
            g_clear_error(&uplink_error);
            uplink_mode = g_strdup("shared");
        }
        if (g_strcmp0(uplink_mode, "shared") == 0)
            g_string_append(net_xml, "  <filterref filter='no-mac-spoofing'/>\n");
        g_free(uplink_mode);
        g_string_append(net_xml, "</interface>");

        if (virDomainAttachDeviceFlags(dom, net_xml->str, VIR_DOMAIN_AFFECT_LIVE) < 0) {
            virErrorPtr err = virGetLastError();
            g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Network hotplug failed: %s", err ? err->message : "Unknown");
                                                                      
            if (virDomainDestroy(dom) != 0) {
                g_warning("[vm.start] VM '%s': fail-closed destroy 실패 — VM이 미배선 상태로 "
                          "실행 중일 수 있음, 수동 조치 필요", ctx->vm_id);
            }
            g_string_free(net_xml, TRUE);
            goto cleanup_dom;
        }
        g_string_free(net_xml, TRUE);
    }

                                                                  
                                                              
                                                                 
                                                                  
                                                              
                                                     
                                                
    if (!_orchestrate_tenant_overlay(ctx, dom, canonical_name, &error)) {
                                                               
        goto cleanup_dom;
    }

                              
                                                                   
                                                                 
                                                      
                                                                
                                                                      
                                                             
                                                              
                                                                 
                                                       
                                                              
                                                               
                                                   
                                                   
                          
    {
        gchar *qos_tenant = NULL, *qos_iface = NULL;
        PcvQosSla qos_sla;
        if (pcv_vm_qos_derive_context(dom, canonical_name, &qos_tenant, &qos_iface, &qos_sla)) {
            GError *qerr = NULL;
            if (pcv_qos_apply_vm(qos_iface, &qos_sla, &qerr)) {
                                                               
                                                             
                                                             
                GError *serr = NULL;
                if (!pcv_qos_ids_save(PCV_QOS_IDS_PATH, &serr)) {
                    g_warning("[vm.start] VM '%s': qos_ids.json 저장 실패(WARN — 다음 재시작까지 "
                              "영향 없음): %s", canonical_name, serr ? serr->message : "unknown");
                    g_clear_error(&serr);
                }
            } else {
                g_warning("[vm.start] VM '%s': QoS 적용 실패(WARN, VM 은 계속 실행 — "
                          "reconcile 은 누락 클래스만 재생성, 복구는 vm 재시작 또는 "
                          "qos.vm.set 재적용): %s", canonical_name,
                          qerr ? qerr->message : "unknown");
                g_clear_error(&qerr);
            }
        }
        g_free(qos_tenant);
        g_free(qos_iface);
    }

cleanup_dom:
    if (dom) virDomainFree(dom);
cleanup_conn:
    if (conn) virt_conn_pool_release(conn);

    if (error) {
        g_task_return_error(task, error);
    } else {
                                                                  
                                                                    
                                
        pcv_security_group_sync_vm(ctx->vm_id);
        g_task_return_boolean(task, TRUE);
    }
    g_free(canonical_name);
}

   
                     
                                     
  
                                                       
                                                        
                                                   
  
                          
                                                               
                                                        
  
              
                                               
                                                    
                                  
   
static void vm_start_callback(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GTask *task = G_TASK(res);
    VmStartContext *ctx = (VmStartContext *)user_data;
    GError *error = NULL;

    gboolean success = g_task_propagate_boolean(task, &error);
    unlock_vm_operation(ctx->vm_id);                              

      
                                    
                                       
                           
       
                                     
                                                                         
                                                         
    gint64 worker_dur_ms = (g_get_monotonic_time() - ctx->worker_start_us) / 1000;
    pcv_audit_log(NULL, "vm.start", ctx->vm_id,
                  success ? "ok" : "fail",
                  success ? 0 : PURE_RPC_ERR_ZFS_OPERATION, worker_dur_ms, "local");

                                                              
    {
        gchar *job_id = g_strdup_printf("vm.start:%s", ctx->vm_id);
        pcv_ws_broadcast_job_complete(job_id, "vm.start",
                                       success ? "ok" : "fail",
                                       (success || !error) ? NULL : error->message);
        g_free(job_id);
    }

    if (!success) {
                                            
                                                                                    
                                                                        
        cpu_allocator_free_vm_cores(global_allocator, ctx->alloc_key);
        g_warning("[vm.start] async worker failed for '%s': %s",
                  ctx->vm_id, error ? error->message : "unknown");
        if (error) g_error_free(error);
    } else {
        g_message("[vm.start] VM '%s' started successfully (async)", ctx->vm_id);
    }
}

   
                           
                                      
                                              
  
                                                            
                                                            
                                   
  
                                      
                             
                                                        
                                               
                                                  
                                         
  
        
                                      
                                     
                                     
                                        
  
                                                              
                                                                        
                                                             
                                                                  
                                                 
   
void handle_vm_start_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
                            
    if (!params || !json_object_has_member(params, "vm_id")) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid params");
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
        return;
    }

                                 
    const gchar *vm_id = json_object_get_string_member(params, "vm_id");
    guint numa_node = json_object_has_member(params, "numa_node") ? json_object_get_int_member(params, "numa_node") : 0;
    guint vcpu_count = json_object_has_member(params, "vcpu_count") ? json_object_get_int_member(params, "vcpu_count") : 1;
    const gchar *bridge = json_object_has_member(params, "bridge_name") ? json_object_get_string_member(params, "bridge_name") : "";

                                                              
                                                      
    if (json_object_has_member(params, "tenant")) {
        g_warning("[vm.start] VM '%s': params 'tenant' 는 무시된다 — tenant-overlay 조인은 "
                  "vm.create network_mode/tenant(도메인 메타데이터)가 유일 소스", vm_id);
    }

      
                                              
                                             
                                           
       
                                                          
                                                              
    gchar *err_msg = NULL;
    if (!lock_vm_operation(vm_id, VM_OP_STARTING, &err_msg)) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, err_msg);
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
        g_free(err_msg);
        return;
    }

      
                                     
      
                                  
                                              
                                                   
                                     
      
                                                
                                             
       
                                                                                   
                                                                         
                                                                       
                             
                                                                                
                                                                 
                                                                        
                                                                         
                                                                        
                                                                  
                                                                   
                                                                 
                                                  
                                                                
                                                               
                                                    
    gchar *alloc_key = g_strdup(vm_id);                                                    
    gboolean already_running = FALSE, skip_alloc = FALSE;
    {
        virConnectPtr pc_conn = virt_conn_pool_acquire();
        if (pc_conn) {
            virDomainPtr pdom = pure_virt_get_domain(pc_conn, vm_id);
            if (pdom) {
                const char *cn = virDomainGetName(pdom);                           
                if (cn && *cn) { g_free(alloc_key); alloc_key = g_strdup(cn); }
                virDomainInfo pinfo;
                if (virDomainGetInfo(pdom, &pinfo) == 0 &&
                    (pinfo.state == VIR_DOMAIN_RUNNING || pinfo.state == VIR_DOMAIN_BLOCKED))
                    already_running = TRUE;           
                virDomainFree(pdom);
            } else {
                skip_alloc = TRUE;                                           
            }
            virt_conn_pool_release(pc_conn);
        }
    }

    GArray *allocated_cpus = NULL;
    gint actual_numa_node = -1;
    if (already_running) {
                                                          
                                                                            
        g_message("[vm.start] VM '%s': already running — CPU 할당 선-스킵(F2: 멱등, 유령 할당 방지)", alloc_key);
    } else if (!skip_alloc &&
               !cpu_allocator_allocate_exclusive(global_allocator, alloc_key, numa_node, vcpu_count,
                                                 &allocated_cpus, &actual_numa_node)) {
        g_warning("[vm.start] No isolated cores for '%s' (need %u), starting without CPU pinning",
                  alloc_key, vcpu_count);
        allocated_cpus = NULL;               
        actual_numa_node = -1;                   
    }

                                              
    VmStartContext *ctx = g_new0(VmStartContext, 1);
    ctx->vm_id = g_strdup(vm_id);
    ctx->alloc_key = alloc_key;                                                            
    ctx->bridge_name = g_strdup(bridge);
    ctx->allocated_cpus = allocated_cpus;                               
    ctx->numa_node = actual_numa_node;                           
    ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = g_object_ref(server);                                   
    ctx->connection = g_object_ref(connection);
    ctx->worker_start_us = g_get_monotonic_time();                               

                                                                           
    GTask *task = g_task_new(NULL, NULL, vm_start_callback, ctx);
    g_task_set_task_data(task, ctx, (GDestroyNotify)free_vm_start_context);

      
                                               
      
                                                                   
                                                         
      
                                                    
       
                                                             
                                                       
                                                             
    {
        JsonNode *acc_node = json_node_new(JSON_NODE_VALUE);
        json_node_set_string(acc_node, "accepted");
        gchar *acc_resp = pure_rpc_build_success_response(rpc_id, acc_node);
        pure_uds_server_send_response(server, connection, acc_resp);                
        g_free(acc_resp);
    }

                                                 
    g_task_run_in_thread(task, vm_start_worker_thread);
    g_object_unref(task);                                     
}
