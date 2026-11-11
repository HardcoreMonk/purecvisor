   
                            
                                              
  
                           
                                                                                                         
                                                                                                                           
                                                                             
  
                                                
   

#include <glib.h>
#include <gio/gio.h>
#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include "api/uds_server.h"
#include "modules/dispatcher/rpc_utils.h"
#include "modules/dispatcher/handler_vm_hotplug.h"
#include "modules/dispatcher/hotplug_affect_policy.h"
#include "modules/dispatcher/hotplug_nic_xml.h"
#include "modules/dispatcher/hotplug_disk_xml.h"
#include "modules/dispatcher/hotplug_usb_xml.h"
#include "modules/dispatcher/guest_ip.h"
#include "modules/dispatcher/guest_agent_exec.h"
#include "modules/virt/virt_conn_pool.h"
#include "modules/core/vm_state.h"
#include "modules/network/network_manager.h"
#include "modules/network/vpc/vpc_manager.h"
#include "modules/resource/resource_gate.h"
#include "purecvisor/pcv_handler_util.h"
#include "utils/pcv_spawn.h"
#include "utils/pcv_validate.h"
#include "modules/dispatcher/handler_vm_internal.h"


                                                                    
                                                     
  
                                                                   
                                                             
  
                                                                                     
                                                                          
                                
  
                                                         
                                                     
                                                 
  
                                                                            
                                                                         
                                                                      
                                                    
  
                                                                     
                                                                               
                                                      
                                                             
  
                                                                 
                                                                                  
                                                                      
  
                                                                   
                                                                       
                                                       
  
                                                                  
                                                  
                                                          
                                                                        
                                                              
                                                                       
void handle_device_disk_attach(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    const gchar *vm_id = json_object_get_string_member(params, "vm_id");
    const gchar *source_dev = json_object_get_string_member(params, "source");
    const gchar *target_dev = json_object_get_string_member(params, "target");

    if (!vm_id || !source_dev || !target_dev) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing vm_id, source, or target");
        pure_uds_server_send_response(server, connection, err); g_free(err); return;
    }

                                                                       
                                                            
                                                               
                                              
    const gchar *bus = json_object_has_member(params, "bus")
        ? json_object_get_string_member(params, "bus") : "virtio";
    if (g_strcmp0(bus, "virtio") != 0 && g_strcmp0(bus, "scsi") != 0 &&
        g_strcmp0(bus, "sata")   != 0 && g_strcmp0(bus, "ide")  != 0) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid bus: must be virtio, scsi, sata, or ide");
        pure_uds_server_send_response(server, connection, err); g_free(err); return;
    }

                                                                      
                                                                                  
                                                                            
    if (!*vm_id) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "Entity not found");
        pure_uds_server_send_response(server, connection, err); g_free(err); return;
    }

                                                     
                                         
                                                                       
                                                               
                                                                                 
                                                                            
    gchar *xml_payload = g_strdup_printf(
        "<disk type='block' device='disk'>\n"
        "  <driver name='qemu' type='raw' cache='none' io='native'/>\n"
        "  <source dev='%s'/>\n"
        "  <target dev='%s' bus='%s'/>\n"
        "</disk>", source_dev, target_dev, bus);

                                                                        
                                           
    gchar *lock_err = NULL;
    PcvVmLock *op_lock = pcv_vm_lock_acquire(vm_id, VM_OP_TUNING, &lock_err);
    if (!op_lock) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
                       lock_err ? lock_err : "VM busy (operation in progress)");
        pure_uds_server_send_response(server, connection, e);
        g_free(e); g_free(lock_err);
        g_free(xml_payload);
        return;
    }

                                                       
    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        pcv_vm_lock_release(op_lock);
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "Hypervisor connection pool exhausted");
        pure_uds_server_send_response(server, connection, err); g_free(err);
        g_free(xml_payload);
        return;
    }
    virDomainPtr dom = pure_virt_get_domain(conn, vm_id);

    if (!dom) {
        pcv_vm_lock_release(op_lock);
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "Entity not found");
        pure_uds_server_send_response(server, connection, err); g_free(err);
        g_free(xml_payload); virt_conn_pool_release(conn); return;
    }

                                                                           
                                                                 
    gboolean is_active = virDomainGetID(dom) != (unsigned int)-1;
    unsigned int flags = pcv_hotplug_compute_affect_flags(is_active, FALSE);

    if (virDomainAttachDeviceFlags(dom, xml_payload, flags) < 0) {
        virErrorPtr libvirt_err = virGetLastError();
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, libvirt_err ? libvirt_err->message : "Attach failed");
        pure_uds_server_send_response(server, connection, err); g_free(err);
    } else {
        JsonObject *res = json_object_new();
        json_object_set_boolean_member(res, "live", is_active);
        json_object_set_boolean_member(res, "config", TRUE);
        JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(res_node, res);
        gchar *resp = pure_rpc_build_success_response(rpc_id, res_node);
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
    }

    pcv_vm_lock_release(op_lock);                               
    g_free(xml_payload);
    virDomainFree(dom);
    virt_conn_pool_release(conn);
}

                                                                    
                                                         
  
                                                  
                                                                   
                                                         
  
                                                           
                                
  
                    
                                                           
                                                                             
                                                                                 
                                                               
                       
  
                                                                      
                                                                                 
                                
  
                                                                       
                                                                    
                                                                 
  
                                                       
                                                       
                                                           
                                                                                         
                                                                                 
                                                                       
void handle_device_disk_detach(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    const gchar *vm_id = pcv_rpc_params_get_string(params, "vm_id");
    const gchar *target_dev = pcv_rpc_params_get_string(params, "target");

    if (!vm_id || !*vm_id || !target_dev || !*target_dev) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing vm_id or target");
        pure_uds_server_send_response(server, connection, err); g_free(err); return;
    }

                                                          
    gchar *lock_err = NULL;
    PcvVmLock *op_lock = pcv_vm_lock_acquire(vm_id, VM_OP_TUNING, &lock_err);
    if (!op_lock) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
                       lock_err ? lock_err : "VM busy (operation in progress)");
        pure_uds_server_send_response(server, connection, e);
        g_free(e); g_free(lock_err);
        return;
    }

                                                       
    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        pcv_vm_lock_release(op_lock);
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "Hypervisor connection pool exhausted");
        pure_uds_server_send_response(server, connection, err); g_free(err);
        return;
    }
    virDomainPtr dom = pure_virt_get_domain(conn, vm_id);

    if (!dom) {
        pcv_vm_lock_release(op_lock);
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "Entity not found");
        pure_uds_server_send_response(server, connection, err); g_free(err); virt_conn_pool_release(conn); return;
    }

                                                                     
      
                                              
                                                                               
                                                        
                                                   
                                                                 
    gboolean is_active = virDomainGetID(dom) != (unsigned int)-1;
    gchar *live_xml = is_active ? virDomainGetXMLDesc(dom, 0) : NULL;
    if (is_active && !live_xml) {
        pcv_vm_lock_release(op_lock);
        virErrorPtr xml_err = virGetLastError();
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
                         xml_err && xml_err->message ? xml_err->message : "Failed to read domain XML");
        pure_uds_server_send_response(server, connection, err); g_free(err);
        virDomainFree(dom); virt_conn_pool_release(conn); return;
    }
    gchar *config_xml = virDomainGetXMLDesc(dom, VIR_DOMAIN_XML_INACTIVE);
    if (!config_xml) {
        pcv_vm_lock_release(op_lock);
        virErrorPtr xml_err = virGetLastError();
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
                         xml_err && xml_err->message ? xml_err->message : "Failed to read domain XML");
        pure_uds_server_send_response(server, connection, err); g_free(err);
        g_free(live_xml); virDomainFree(dom); virt_conn_pool_release(conn); return;
    }

                                                                         
                                                                  
                                       
    GError *select_error = NULL;
    gboolean want_live = FALSE, want_config = FALSE;
    gchar *exact_xml = NULL;
    if (!pcv_hotplug_disk_detach_plan(live_xml, config_xml, target_dev, is_active,
                                      &want_live, &want_config, &exact_xml, &select_error)) {
        pcv_vm_lock_release(op_lock);
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
                         select_error ? select_error->message : "Device not found in live or persistent XML");
        pure_uds_server_send_response(server, connection, err); g_free(err);
        g_clear_error(&select_error);
        g_free(config_xml); g_free(live_xml); virDomainFree(dom); virt_conn_pool_release(conn); return;
    }

                                                   
    unsigned int flags = (want_live ? VIR_DOMAIN_AFFECT_LIVE : 0) |
                         (want_config ? VIR_DOMAIN_AFFECT_CONFIG : 0);
    if (virDomainDetachDeviceFlags(dom, exact_xml, flags) < 0) {
        virErrorPtr libvirt_err = virGetLastError();
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, libvirt_err ? libvirt_err->message : "Detach failed");
        pure_uds_server_send_response(server, connection, err); g_free(err);
    } else {
        JsonObject *res = json_object_new();
        json_object_set_boolean_member(res, "live", want_live);
        json_object_set_boolean_member(res, "config", want_config);
        JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(res_node, res);
        gchar *resp = pure_rpc_build_success_response(rpc_id, res_node);
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
    }

    pcv_vm_lock_release(op_lock);                               
                                     
    g_free(exact_xml);
    g_free(config_xml);
    g_free(live_xml);
    virDomainFree(dom);
    virt_conn_pool_release(conn);
}

                                                                    
                                       
  
                                           
  
                                                                                 
                                
  
                                     
                                                                  
                                                   
  
                                                                              
                                                                          
                                               
                                                                                     
                                                                       
void handle_vm_mount_iso(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    const gchar *vm_id = json_object_has_member(params, "vm_id")
        ? json_object_get_string_member(params, "vm_id") : NULL;
    if ((!vm_id || *vm_id == '\0') && json_object_has_member(params, "name"))
        vm_id = json_object_get_string_member(params, "name");
    const gchar *iso_path = json_object_has_member(params, "iso_path")
        ? json_object_get_string_member(params, "iso_path") : NULL;

    if (!vm_id || *vm_id == '\0' || !iso_path || *iso_path == '\0') {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing: vm_id or iso_path");
        pure_uds_server_send_response(server, connection, err); g_free(err); return;
    }

    if (!pcv_validate_iso_path(iso_path)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid iso_path: must be absolute, non-empty, and must not contain '..'");
        pure_uds_server_send_response(server, connection, err); g_free(err); return;
    }
    g_autofree gchar *iso_canon = NULL;
    if (!pcv_res_gate(params, connection, "vm.mount_iso", "iso_path", PCV_RES_ISO, iso_path, &iso_canon, NULL, rpc_id, server)) return;
    iso_path = iso_canon;
    if (!g_file_test(iso_path, G_FILE_TEST_IS_REGULAR)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid iso_path: file does not exist or is not a regular file");
        pure_uds_server_send_response(server, connection, err); g_free(err); return;
    }

    virConnectPtr conn;
    PCV_REQUIRE_VIRT_CONN(conn, rpc_id, server, connection);
    virDomainPtr dom = pure_virt_get_domain(conn, vm_id);

    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "VM not found");
        pure_uds_server_send_response(server, connection, err); g_free(err);
        virt_conn_pool_release(conn);
        return;
    }

    gchar *iso_escaped = g_markup_escape_text(iso_path, -1);
    gchar *mount_xml = g_strdup_printf(
        "<disk type='file' device='cdrom'>\n"
        "  <driver name='qemu' type='raw'/>\n"
        "  <source file='%s'/>\n"
        "  <target dev='sda' bus='sata'/>\n"
        "  <readonly/>\n"
        "</disk>", iso_escaped);

    int flags = VIR_DOMAIN_AFFECT_LIVE | VIR_DOMAIN_AFFECT_CONFIG;

                                                          
    gchar *lock_err = NULL;
    PcvVmLock *op_lock = pcv_vm_lock_acquire(vm_id, VM_OP_TUNING, &lock_err);
    if (!op_lock) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
                       lock_err ? lock_err : "VM busy (operation in progress)");
        pure_uds_server_send_response(server, connection, e);
        g_free(e); g_free(lock_err);
        g_free(mount_xml); g_free(iso_escaped);
        virDomainFree(dom); virt_conn_pool_release(conn);
        return;
    }

    if (virDomainUpdateDeviceFlags(dom, mount_xml, flags) < 0) {
        virErrorPtr libvirt_err = virGetLastError();
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            libvirt_err ? libvirt_err->message : "Failed to mount ISO");
        pure_uds_server_send_response(server, connection, err); g_free(err);
    } else {
        JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
        JsonObject *res_obj = json_object_new();
        json_object_set_boolean_member(res_obj, "mounted", TRUE);
        json_object_set_string_member(res_obj, "iso_path", iso_path);
        json_node_take_object(res_node, res_obj);

        gchar *resp = pure_rpc_build_success_response(rpc_id, res_node);
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
    }

    pcv_vm_lock_release(op_lock);                               
    g_free(mount_xml);
    g_free(iso_escaped);
    virDomainFree(dom); virt_conn_pool_release(conn);
}

                                                                    
                                  
  
                                       
  
                                          
                                
  
          
                                                     
                                            
                                           
  
                                                           
                                        
                                                                     
                                                                       
void handle_vm_eject_iso(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    const gchar *vm_id;
    PCV_REQUIRE_PARAM(params, "vm_id", vm_id, rpc_id, server, connection);

    virConnectPtr conn;
    PCV_REQUIRE_VIRT_CONN(conn, rpc_id, server, connection);
    virDomainPtr dom = pure_virt_get_domain(conn, vm_id);
    
    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "Entity not found");
        pure_uds_server_send_response(server, connection, err); g_free(err); virt_conn_pool_release(conn); return;
    }

      
                                           
                                                                           
                                           
      
                                     
                                                                
                                                         
                                                 
                                                            
                                                  
    const gchar *eject_xml =
        "<disk type='file' device='cdrom'>\n"
        "  <target dev='sda' bus='sata'/>\n"
        "</disk>";

                                                              
    int flags = VIR_DOMAIN_AFFECT_LIVE | VIR_DOMAIN_AFFECT_CONFIG;
    
                                                          
    gchar *lock_err = NULL;
    PcvVmLock *op_lock = pcv_vm_lock_acquire(vm_id, VM_OP_TUNING, &lock_err);
    if (!op_lock) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
                       lock_err ? lock_err : "VM busy (operation in progress)");
        pure_uds_server_send_response(server, connection, e);
        g_free(e); g_free(lock_err);
        virDomainFree(dom); virt_conn_pool_release(conn);
        return;
    }

    if (virDomainUpdateDeviceFlags(dom, eject_xml, flags) < 0) {
        virErrorPtr libvirt_err = virGetLastError();
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, libvirt_err ? libvirt_err->message : "Failed to eject ISO");
        pure_uds_server_send_response(server, connection, err); g_free(err);
    } else {
                                      
        JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
        JsonObject *res_obj = json_object_new();
        json_object_set_boolean_member(res_obj, "ejected", TRUE);
        json_node_take_object(res_node, res_obj);

        gchar *resp = pure_rpc_build_success_response(rpc_id, res_node);
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
    }

    pcv_vm_lock_release(op_lock);                               
    virDomainFree(dom); virt_conn_pool_release(conn);
}
                                                                    
                                                                     
                                         
  
                                                              
                                                             
                                               
                                                                       

                                                                                 
                                                  
                                                                            
                                                        
                                                                    
static gchar *_pcv_xml_attr_dup(const gchar *start, const gchar *limit, const gchar *attr)
{
    if (!start || !attr) return NULL;                                                

                                                           
    gssize len = (limit && limit > start) ? (gssize)(limit - start) : -1;
    gchar *needle = g_strdup_printf("%s='", attr);                                   
    const gchar *hit = g_strstr_len(start, len, needle);
    gchar quote = '\'';                                                             

    if (!hit) {                                                                             
        g_free(needle);
        needle = g_strdup_printf("%s=\"", attr);
        hit = g_strstr_len(start, len, needle);
        quote = '"';
    }

    if (!hit) {
        g_free(needle);
        return NULL;
    }

    hit += strlen(needle);                                                                  
    const gchar *end = strchr(hit, quote);                                       
    if (!end || (limit && end > limit)) {                                                        
        g_free(needle);
        return NULL;
    }

    gchar *value = g_strndup(hit, (gsize)(end - hit));                              
    g_free(needle);
    return value;
}

                                                            
                                                           
                   
                                                                       
                                                            
static gchar *_pcv_dns_for_bridge(const gchar *bridge)
{
    if (!bridge || !*bridge)
        return g_strdup("");                                          

    gchar *conf_path = g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-%s.conf", bridge);
    gchar *content = NULL;
    gchar *result = NULL;

    if (g_file_get_contents(conf_path, &content, NULL, NULL) && content) {
                                                                
        if (g_strstr_len(content, -1, "\nport=0") ||
            g_str_has_prefix(content, "port=0")) {
            result = g_strdup("off");
        } else {
            gchar **lines = g_strsplit(content, "\n", -1);
            for (guint i = 0; lines[i] && !result; i++) {
                gchar *line = g_strstrip(lines[i]);
                if (g_str_has_prefix(line, "server="))
                    result = g_strdup(line + 7);                                       
            }
            g_strfreev(lines);
        }
    }

    g_free(content);
    g_free(conf_path);
    return result ? result : g_strdup("");
}

                                                                                        
                                                                                
                                                                     
                                                                                   
                                                                          
                                                                                            
                                      
                                                                 
                                                             
static gchar *_pcv_nic_ip_for_mac(virDomainPtr dom, const gchar *mac,
                                  const gchar *bridge, const gchar **source_out)
{
    int state = VIR_DOMAIN_NOSTATE;
    int reason = 0;
    gboolean running = virDomainGetState(dom, &state, &reason, 0) == 0 &&
                       state == VIR_DOMAIN_RUNNING;
    char uuid[VIR_UUID_STRING_BUFLEN] = "";
    GPtrArray *lease = NULL, *agent = NULL;
    if (running) {
        virDomainInterfacePtr *ifaces = NULL;
        int n = virDomainInterfaceAddresses(dom, &ifaces, VIR_DOMAIN_INTERFACE_ADDRESSES_SRC_LEASE, 0);
        lease = pcv_guest_ip_from_virt_ifaces(ifaces, n);
        for (int i = 0; i < n; i++)
            if (ifaces[i]) virDomainInterfaceFree(ifaces[i]);
        free(ifaces);                                                     
        if (virDomainGetUUIDString(dom, uuid) == 0)
            agent = pcv_guest_agent_cache_lookup(uuid);
    }

    PcvGuestIpInputs in = { .lease_ifaces = lease, .agent_ifaces = agent, .arp_require_complete = FALSE };
    const gchar *src = "";
    gchar *ip = pcv_guest_ip_resolve(&in, mac, PCV_GUEST_IP_MAC_IPV4_THEN_ANY, &src);
    gchar *lease_text = NULL, *arp_text = NULL;
    if (!ip) {                               
        if (bridge && *bridge) {
            gchar *path = g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-%s.leases", bridge);
            (void)g_file_get_contents(path, &lease_text, NULL, NULL);
            g_free(path);
        }
        if (running)
            (void)g_file_get_contents("/proc/net/arp", &arp_text, NULL, NULL);
        in.lease_file_text = lease_text;
        in.proc_net_arp_text = arp_text;
        ip = pcv_guest_ip_resolve(&in, mac, PCV_GUEST_IP_MAC_IPV4_THEN_ANY, &src);
    }
    if (running && uuid[0] && !agent && g_strcmp0(src, "lease") != 0)
        pcv_guest_agent_probe_request(uuid, virDomainGetName(dom));

    g_free(lease_text);
    g_free(arp_text);
    if (agent)
        g_ptr_array_unref(agent);
    if (lease)
        g_ptr_array_unref(lease);
    if (source_out) *source_out = src;
    return ip;
}

   
                          
                                                   
  
                                          
                                                                  
                                                                  
                                                    
  
              
                                                                 
                                                             
                                                                      
  
                                                            
                                                                    
                                                
   
void handle_device_nic_list(JsonObject *params, const gchar *rpc_id,
                             UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm_id = json_object_has_member(params, "vm_id")
        ? json_object_get_string_member(params, "vm_id") : NULL;
    if (!vm_id) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing: vm_id");
        pure_uds_server_send_response(server, connection, err); g_free(err); return;
    }
    virConnectPtr conn;
    PCV_REQUIRE_VIRT_CONN(conn, rpc_id, server, connection);
    virDomainPtr dom = pure_virt_get_domain(conn, vm_id);
    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "VM not found");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); virt_conn_pool_release(conn); return;
    }

                                                   
                                                                               
    char *xml = virDomainGetXMLDesc(dom, 0);
    JsonArray *arr = json_array_new();

    if (xml) {
        const gchar *iface = xml;
                                                                       
        while ((iface = strstr(iface, "<interface ")) != NULL) {
            const gchar *tag_end = strchr(iface, '>');                            
            const gchar *block_end = strstr(iface, "</interface>");                  
            if (!tag_end || !block_end)
                break;                                  

            const gchar *mac_tag = g_strstr_len(iface, block_end - iface, "<mac ");
            const gchar *source_tag = g_strstr_len(iface, block_end - iface, "<source ");
            const gchar *model_tag = g_strstr_len(iface, block_end - iface, "<model ");
            const gchar *target_tag = g_strstr_len(iface, block_end - iface, "<target ");

            gchar *type = _pcv_xml_attr_dup(iface, tag_end, "type");
            gchar *mac = mac_tag ? _pcv_xml_attr_dup(mac_tag, block_end, "address") : NULL;
            gchar *source = NULL;
            const gchar *source_key = "";
            if (source_tag) {
                const gchar *source_end = strchr(source_tag, '>');
                if (!source_end || source_end > block_end)
                    source_end = block_end;
                                                                 
                                                                                
                                                          
                const gchar *keys[] = {"bridge", "network", "dev", "port", "path", NULL};
                for (guint k = 0; keys[k] && !source; k++) {
                    source = _pcv_xml_attr_dup(source_tag, source_end, keys[k]);
                    if (source)
                        source_key = keys[k];
                }
            }
            gchar *model = model_tag ? _pcv_xml_attr_dup(model_tag, block_end, "type") : NULL;
            gchar *target = target_tag ? _pcv_xml_attr_dup(target_tag, block_end, "dev") : NULL;
            const gchar *ip_source = "";
            gchar *ip = (mac && *mac) ? _pcv_nic_ip_for_mac(dom, mac, source, &ip_source) : NULL;
            gchar *dns = (source && g_strcmp0(source_key, "bridge") == 0)
                ? _pcv_dns_for_bridge(source)
                : g_strdup("");

            JsonObject *nic = json_object_new();
            json_object_set_string_member(nic, "mac", mac ? mac : "");
            json_object_set_string_member(nic, "type", type ? type : "unknown");
            json_object_set_string_member(nic, "source", source ? source : "");
            json_object_set_string_member(nic, "source_type", source_key);
            if (g_strcmp0(source_key, "bridge") == 0)
                json_object_set_string_member(nic, "bridge", source ? source : "");
            json_object_set_string_member(nic, "model", model ? model : "virtio");
            json_object_set_string_member(nic, "target", target ? target : "");
            json_object_set_string_member(nic, "ip", ip ? ip : "");
            json_object_set_string_member(nic, "ip_source", ip_source ? ip_source : "");
            json_object_set_string_member(nic, "dns", dns ? dns : "");
            json_array_add_object_element(arr, nic);

            g_free(type);
            g_free(mac);
            g_free(source);
            g_free(model);
            g_free(target);
            g_free(ip);
            g_free(dns);

            iface = block_end + strlen("</interface>");
        }
        free(xml);                                                    
    }

    JsonNode *res = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(res, arr);
    gchar *resp = pure_rpc_build_success_response(rpc_id, res);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
    virDomainFree(dom); virt_conn_pool_release(conn);
}

   
                            
                                                         
  
                                                                                             
                     
  
                  
                                                           
                                                
                                            
  
                                                         
                                                           
                                                
                                                                  
                                              
                                                     
   
void handle_device_nic_attach(JsonObject *params, const gchar *rpc_id,
                               UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm_id = json_object_has_member(params, "vm_id")
        ? json_object_get_string_member(params, "vm_id") : NULL;
    const gchar *bridge = json_object_has_member(params, "bridge")
        ? json_object_get_string_member(params, "bridge") : "virbr0";
    const gchar *model  = json_object_has_member(params, "model")
        ? json_object_get_string_member(params, "model")  : "virtio";

    if (!vm_id) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing: vm_id");
        pure_uds_server_send_response(server, connection, err); g_free(err); return;
    }
    if (!pcv_validate_bridge_name(bridge)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid bridge name");
        pure_uds_server_send_response(server, connection, err); g_free(err); return;
    }
    if (pcv_vpc_bridge_is_managed(bridge)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_CONFLICT,
            "VPC-managed NICs must be attached through vpc.attachment.create");
        pure_uds_server_send_response(server, connection, err); g_free(err); return;
    }
    if (!pcv_res_gate(params, connection, "device.nic.attach", "bridge", PCV_RES_BRIDGE, bridge, NULL, NULL, rpc_id, server)) return;

                                                               
                                                      
                                                                
                                 
    if (g_strcmp0(model, "virtio") != 0 && g_strcmp0(model, "e1000") != 0 &&
        g_strcmp0(model, "e1000e") != 0 && g_strcmp0(model, "rtl8139") != 0) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid model: must be virtio, e1000, e1000e, or rtl8139");
        pure_uds_server_send_response(server, connection, err); g_free(err); return;
    }

    virConnectPtr conn;
    PCV_REQUIRE_VIRT_CONN(conn, rpc_id, server, connection);
    virDomainPtr dom = pure_virt_get_domain(conn, vm_id);
    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "VM not found");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); virt_conn_pool_release(conn); return;
    }

                                                             
                                                            
                                                    
                               
    gint bridge_mtu = pcv_bridge_mtu_read(bridge, NULL);
    gchar *mtu_xml = (bridge_mtu > 0)
        ? g_strdup_printf("  <mtu size='%d'/>\n", bridge_mtu)
        : g_strdup("");
                                                                    
                                                                        
                                         
    gchar *safe_bridge = g_markup_escape_text(bridge, -1);
    gchar *uplink_mode = NULL;
    GError *uplink_error = NULL;
    if (!pcv_network_bridge_uplink_mode(bridge, &uplink_mode, &uplink_error)) {
        g_warning("[device.nic.attach] uplink mode lookup failed for %s: %s",
                  bridge, uplink_error ? uplink_error->message : "unknown");
        g_clear_error(&uplink_error);
        uplink_mode = g_strdup("shared");
    }
    const gchar *filter_xml = g_strcmp0(uplink_mode, "shared") == 0
        ? "  <filterref filter='no-mac-spoofing'/>\n" : "";
    gchar *xml = g_strdup_printf(
        "<interface type='bridge'>\n"
        "  <source bridge='%s'/>\n"
        "  <model type='%s'/>\n"
        "%s"
        "%s"
        "</interface>", safe_bridge, model, mtu_xml, filter_xml);
    g_free(safe_bridge);
    g_free(mtu_xml);
    g_free(uplink_mode);

                                                          
    gchar *lock_err = NULL;
    PcvVmLock *op_lock = pcv_vm_lock_acquire(vm_id, VM_OP_TUNING, &lock_err);
    if (!op_lock) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
                       lock_err ? lock_err : "VM busy (operation in progress)");
        pure_uds_server_send_response(server, connection, e);
        g_free(e); g_free(lock_err);
        g_free(xml); virDomainFree(dom); virt_conn_pool_release(conn);
        return;
    }

    int rc = virDomainAttachDeviceFlags(dom, xml,
                 VIR_DOMAIN_AFFECT_LIVE | VIR_DOMAIN_AFFECT_CONFIG);
    g_free(xml);

    if (rc < 0) {
        virErrorPtr e = virGetLastError();
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            e ? e->message : "NIC attach failed");
        pure_uds_server_send_response(server, connection, err); g_free(err);
    } else {
        JsonNode *res = json_node_new(JSON_NODE_VALUE);
        json_node_set_boolean(res, TRUE);
        gchar *resp = pure_rpc_build_success_response(rpc_id, res);
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
    }
    pcv_vm_lock_release(op_lock);                               
    virDomainFree(dom); virt_conn_pool_release(conn);
}

   
                            
                                                         
  
                                                                      
                     
  
              
                                                          
                                                              
  
                                                      
                                                                         
                                                     
   
void handle_device_nic_detach(JsonObject *params, const gchar *rpc_id,
                               UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm_id = json_object_has_member(params, "vm_id")
        ? json_object_get_string_member(params, "vm_id") : NULL;
    const gchar *mac   = json_object_has_member(params, "mac")
        ? json_object_get_string_member(params, "mac")   : NULL;

    if (!vm_id || !mac) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing: vm_id or mac");
        pure_uds_server_send_response(server, connection, err); g_free(err); return;
    }
    if (!pcv_validate_mac(mac)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid MAC address");
        pure_uds_server_send_response(server, connection, err); g_free(err); return;
    }
    if (pcv_vpc_mac_is_managed(mac)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_CONFLICT,
            "VPC-managed NICs must be detached through vpc.attachment.delete");
        pure_uds_server_send_response(server, connection, err); g_free(err); return;
    }

    virConnectPtr conn;
    PCV_REQUIRE_VIRT_CONN(conn, rpc_id, server, connection);
    virDomainPtr dom = pure_virt_get_domain(conn, vm_id);
    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "VM not found");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); virt_conn_pool_release(conn); return;
    }

    char *domain_xml = virDomainGetXMLDesc(dom, VIR_DOMAIN_XML_INACTIVE);
    GError *select_error = NULL;
    gchar *xml = domain_xml
        ? pcv_hotplug_select_nic_xml(domain_xml, mac, &select_error)
        : NULL;
    free(domain_xml);
    if (!xml) {
        virErrorPtr libvirt_error = virGetLastError();
        const gchar *message = select_error ? select_error->message
            : (libvirt_error ? libvirt_error->message
                             : "Failed to read persistent VM configuration");
        gchar *err = pure_rpc_build_error_response(rpc_id,
            PURE_RPC_ERR_ZFS_OPERATION, message);
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        g_clear_error(&select_error);
        virDomainFree(dom);
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
        g_free(xml); virDomainFree(dom); virt_conn_pool_release(conn);
        return;
    }

    int rc = virDomainDetachDeviceFlags(dom, xml,
                 VIR_DOMAIN_AFFECT_LIVE | VIR_DOMAIN_AFFECT_CONFIG);
    g_free(xml);

    if (rc < 0) {
        virErrorPtr e = virGetLastError();
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            e ? e->message : "NIC detach failed");
        pure_uds_server_send_response(server, connection, err); g_free(err);
    } else {
        JsonNode *res = json_node_new(JSON_NODE_VALUE);
        json_node_set_boolean(res, TRUE);
        gchar *resp = pure_rpc_build_success_response(rpc_id, res);
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
    }
    pcv_vm_lock_release(op_lock);                               
    virDomainFree(dom); virt_conn_pool_release(conn);
}

                                                                    
                                                                
  
                                                                    
                                                                                 
                                                           
                                                        
                                                     
                                                                       

   
                       
                                                    
                          
  
                                                            
                                               
                                                       
   
static gboolean pcv_validate_usb_id(const gchar *id) {
    if (!id || strlen(id) != 6) return FALSE;                                         
    if (id[0] != '0' || (id[1] != 'x' && id[1] != 'X')) return FALSE;                  
    for (int i = 2; i < 6; i++) {                                                  
        if (!g_ascii_isxdigit(id[i])) return FALSE;
    }
    return TRUE;
}

  
                                                                              
                                                                                   
                                                                               
                                                                           
                                                                  
                                                                     
   
static gboolean _usb_read_defs(virDomainPtr dom, gboolean is_active, gboolean *has_persistent,
                               gchar **live_xml, gchar **config_xml, GError **error)
{
    int persistent = virDomainIsPersistent(dom);
    gboolean ok = persistent >= 0;
    *has_persistent = persistent == 1;
    *live_xml = *config_xml = NULL;
    if (ok && is_active)
        ok = (*live_xml = virDomainGetXMLDesc(dom, 0)) != NULL;
    if (ok && *has_persistent)
        ok = (*config_xml = virDomainGetXMLDesc(dom, VIR_DOMAIN_XML_INACTIVE)) != NULL;
    if (!ok) {
        virErrorPtr e = virGetLastError();
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            e && e->message ? e->message : "Failed to read domain XML");
        g_clear_pointer(live_xml, g_free);
    }
    return ok;
}

  
                                                                                                            
                                                                                                          
                
                                                      
   
static void _usb_reply_gerror(const gchar *rpc_id, UdsServer *server, GSocketConnection *connection, GError *error)
{
    PureRpcErrorCode code = g_error_matches(error, PCV_USB_ERROR, PCV_USB_ERROR_CONFLICT)
        ? PURE_RPC_ERR_CONFLICT : PURE_RPC_ERR_ZFS_OPERATION;
    gchar *err = pure_rpc_build_error_response(rpc_id, code, error->message);
    pure_uds_server_send_response(server, connection, err);
    g_free(err);
    g_error_free(error);
}

   
                        
                                                                                                 
  
                                                                           
                                                                                                                 
                                                                                                           
                                     
                                                                                            
                                                                                                       
                                                                                                      
                                                                                            
                                                                                         
                                                         
  
                                                                               
                                               
                                                                                                                          
                                                                                                                  
   
void handle_vm_usb_attach(JsonObject *params, const gchar *rpc_id,
                           UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm_id = pcv_rpc_params_get_string(params, "vm_id");
    const gchar *vendor_id = pcv_rpc_params_get_string(params, "vendor_id");
    const gchar *product_id = pcv_rpc_params_get_string(params, "product_id");
    PcvUsbPolicy policy = PCV_USB_POLICY_OPTIONAL;
    gboolean persist = TRUE;                                                                           
    const gchar *bad = NULL;

    if (!vm_id || !vendor_id || !product_id)
        bad = "Missing required parameters: vm_id, vendor_id, product_id";
    else if (!pcv_validate_usb_id(vendor_id) || !pcv_validate_usb_id(product_id))
        bad = "vendor_id and product_id must be hex format: 0xNNNN";
    else if (json_object_has_member(params, "startup_policy") &&                                
             !pcv_usb_policy_parse(pcv_rpc_params_get_string(params, "startup_policy"), &policy))
        bad = "startup_policy must be one of: mandatory, requisite, optional";
    else if (!pcv_rpc_params_get_bool(params, "persistent", TRUE, &persist))                       
        bad = "persistent must be a boolean";
    else if (!persist && json_object_has_member(params, "startup_policy"))                        
        bad = "startup_policy applies only when persistent is true";
    if (bad) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, bad);
        pure_uds_server_send_response(server, connection, err);
        g_free(err); return;
    }
    g_autofree gchar *usb_ref = g_strdup_printf("%s:%s", vendor_id, product_id);
    if (!pcv_res_gate(params, connection, "vm.usb.attach", "vendor_id/product_id", PCV_RES_USB, usb_ref, NULL, NULL, rpc_id, server)) return;

                                                                                           
    if (!*vm_id) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_VM_NOT_FOUND, "VM not found");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); return;
    }

                                                                                 
    gchar *lock_err = NULL;
    PcvVmLock *op_lock = pcv_vm_lock_acquire(vm_id, VM_OP_TUNING, &lock_err);
    if (!op_lock) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
                       lock_err ? lock_err : "VM busy (operation in progress)");
        pure_uds_server_send_response(server, connection, e);
        g_free(e); g_free(lock_err);
        return;
    }
    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        pcv_vm_lock_release(op_lock);
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "Hypervisor connection pool exhausted");
        pure_uds_server_send_response(server, connection, err); g_free(err);
        return;
    }
    virDomainPtr dom = pure_virt_get_domain(conn, vm_id);
    if (!dom) {
        pcv_vm_lock_release(op_lock);
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_VM_NOT_FOUND, "VM not found");
        pure_uds_server_send_response(server, connection, err); g_free(err);
        virt_conn_pool_release(conn); return;
    }

    guint16 v = pcv_usb_id_value(vendor_id), p = pcv_usb_id_value(product_id);
    gboolean is_active = virDomainGetID(dom) != (unsigned int)-1, has_persistent = FALSE;
    g_autofree gchar *live_xml = NULL;
    g_autofree gchar *config_xml = NULL;
    g_autofree gchar *saved_policy = NULL;
    PcvUsbAttachKind kind = PCV_USB_ATTACH_NOOP;
    GError *error = NULL;
    if (!_usb_read_defs(dom, is_active, &has_persistent, &live_xml, &config_xml, &error) ||
        !pcv_usb_attach_plan(live_xml, config_xml, is_active, has_persistent, v, p, policy, persist, vm_id,
                             &kind, &saved_policy, &error)) {
        pcv_vm_lock_release(op_lock);
        _usb_reply_gerror(rpc_id, server, connection, error);
        virDomainFree(dom); virt_conn_pool_release(conn); return;
    }

                                                     
    gboolean want_live = kind == PCV_USB_ATTACH_TWO_STEP || kind == PCV_USB_ATTACH_LIVE_ONLY;
    gboolean want_config = kind == PCV_USB_ATTACH_TWO_STEP || kind == PCV_USB_ATTACH_CONFIG_LEGACY ||
                           kind == PCV_USB_ATTACH_CONFIG_STOPPED;
    g_autofree gchar *dev = g_strdup_printf("0x%04x:0x%04x", v, p);
    g_autofree gchar *plain_xml = pcv_usb_hostdev_xml(v, p, NULL);
    g_autofree gchar *policy_xml = pcv_usb_hostdev_xml(v, p, &policy);
    g_autofree gchar *fail = NULL;
    gboolean live_attached = FALSE;
    if (want_live) {
        if (virDomainAttachDeviceFlags(dom, plain_xml, VIR_DOMAIN_AFFECT_LIVE) < 0)
            fail = g_strdup_printf("USB device %s could not be attached to the running VM '%s'; nothing was changed: %s",
                                   dev, vm_id, virGetLastErrorMessage());
        else
            live_attached = TRUE;
    }
    if (!fail && want_config && virDomainAttachDeviceFlags(dom, policy_xml, VIR_DOMAIN_AFFECT_CONFIG) < 0) {
        g_autofree gchar *cfg_err = g_strdup(virGetLastErrorMessage());
        if (live_attached) {                                                             
            if (virDomainDetachDeviceFlags(dom, plain_xml, VIR_DOMAIN_AFFECT_LIVE) == 0)
                fail = g_strdup_printf("USB device %s could not be saved to the persistent definition of VM '%s'; "
                                       "the live attachment was rolled back: %s", dev, vm_id, cfg_err);
            else
                fail = g_strdup_printf("USB device %s is attached to the running VM '%s' but not saved to its persistent "
                                       "definition, and rolling back failed (detach it with 'pcvctl vm usb-detach'): "
                                       "%s; rollback: %s", dev, vm_id, cfg_err, virGetLastErrorMessage());
        } else if (kind == PCV_USB_ATTACH_CONFIG_LEGACY) {
            fail = g_strdup_printf("USB device %s could not be saved to the persistent definition of VM '%s'; "
                                   "the live attachment was left as it was: %s", dev, vm_id, cfg_err);
        } else {
            fail = g_strdup_printf("USB device %s could not be saved to the persistent definition of VM '%s': %s",
                                   dev, vm_id, cfg_err);
        }
    }

    if (fail) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, fail);
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
    } else {
        JsonObject *res_obj = json_object_new();
        json_object_set_boolean_member(res_obj, "attached", TRUE);
        json_object_set_string_member(res_obj, "vendor_id", vendor_id);
        json_object_set_string_member(res_obj, "product_id", product_id);
        json_object_set_boolean_member(res_obj, "live", want_live);
        json_object_set_boolean_member(res_obj, "config", want_config);
        if (saved_policy)
            json_object_set_string_member(res_obj, "startup_policy", saved_policy);
        else
            json_object_set_null_member(res_obj, "startup_policy");
        JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(res_node, res_obj);
        gchar *usb_resp = pure_rpc_build_success_response(rpc_id, res_node);
        pure_uds_server_send_response(server, connection, usb_resp);
        g_free(usb_resp);
    }

    pcv_vm_lock_release(op_lock);                               
    virDomainFree(dom);
    virt_conn_pool_release(conn);
}

   
                        
                                                                                
  
                                                                                         
                                                                                            
                                                                                                          
                                                                                         
                                                                                              
  
                                                                                 
                                                                                                                
                                                                             
   
void handle_vm_usb_detach(JsonObject *params, const gchar *rpc_id,
                           UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm_id = pcv_rpc_params_get_string(params, "vm_id");
    const gchar *vendor_id = pcv_rpc_params_get_string(params, "vendor_id");
    const gchar *product_id = pcv_rpc_params_get_string(params, "product_id");
    const gchar *bad = NULL;

    if (!vm_id || !vendor_id || !product_id)
        bad = "Missing required parameters: vm_id, vendor_id, product_id";
    else if (!pcv_validate_usb_id(vendor_id) || !pcv_validate_usb_id(product_id))
        bad = "vendor_id and product_id must be hex format: 0xNNNN";
    if (bad) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, bad);
        pure_uds_server_send_response(server, connection, err);
        g_free(err); return;
    }
    if (!*vm_id) {                                                          
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_VM_NOT_FOUND, "VM not found");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); return;
    }

    gchar *lock_err = NULL;
    PcvVmLock *op_lock = pcv_vm_lock_acquire(vm_id, VM_OP_TUNING, &lock_err);
    if (!op_lock) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
                       lock_err ? lock_err : "VM busy (operation in progress)");
        pure_uds_server_send_response(server, connection, e);
        g_free(e); g_free(lock_err);
        return;
    }
    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        pcv_vm_lock_release(op_lock);
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "Hypervisor connection pool exhausted");
        pure_uds_server_send_response(server, connection, err); g_free(err);
        return;
    }
    virDomainPtr dom = pure_virt_get_domain(conn, vm_id);
    if (!dom) {
        pcv_vm_lock_release(op_lock);
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_VM_NOT_FOUND, "VM not found");
        pure_uds_server_send_response(server, connection, err); g_free(err);
        virt_conn_pool_release(conn); return;
    }

    guint16 v = pcv_usb_id_value(vendor_id), p = pcv_usb_id_value(product_id);
    gboolean is_active = virDomainGetID(dom) != (unsigned int)-1, has_persistent = FALSE;
    gboolean want_live = FALSE, want_config = FALSE;
    g_autofree gchar *live_xml = NULL;
    g_autofree gchar *config_xml = NULL;
    GError *error = NULL;
    if (!_usb_read_defs(dom, is_active, &has_persistent, &live_xml, &config_xml, &error) ||
        !pcv_usb_detach_plan(live_xml, config_xml, is_active, has_persistent, v, p, vm_id,
                             &want_live, &want_config, &error)) {
        pcv_vm_lock_release(op_lock);
        _usb_reply_gerror(rpc_id, server, connection, error);
        virDomainFree(dom); virt_conn_pool_release(conn); return;
    }

    g_autofree gchar *xml = pcv_usb_hostdev_xml(v, p, NULL);
    unsigned int flags = (want_live ? VIR_DOMAIN_AFFECT_LIVE : 0) | (want_config ? VIR_DOMAIN_AFFECT_CONFIG : 0);
    if (virDomainDetachDeviceFlags(dom, xml, flags) < 0) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, virGetLastErrorMessage());
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
    } else {
        JsonObject *res_obj = json_object_new();
        json_object_set_boolean_member(res_obj, "detached", TRUE);
        json_object_set_string_member(res_obj, "vendor_id", vendor_id);
        json_object_set_string_member(res_obj, "product_id", product_id);
        json_object_set_boolean_member(res_obj, "live", want_live);
        json_object_set_boolean_member(res_obj, "config", want_config);
        JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(res_node, res_obj);
        gchar *usb_resp = pure_rpc_build_success_response(rpc_id, res_node);
        pure_uds_server_send_response(server, connection, usb_resp);
        g_free(usb_resp);
    }

    pcv_vm_lock_release(op_lock);                               
    virDomainFree(dom);
    virt_conn_pool_release(conn);
}

   
                      
                                                                             
  
                                                                                                           
                                                                             
                                                                                                
                                                                                          
                                                                          
  
                                                               
                                                        
                                                                                     
   
void handle_vm_usb_list(JsonObject *params, const gchar *rpc_id,
                         UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm_id = json_object_has_member(params, "vm_id")
        ? json_object_get_string_member(params, "vm_id") : NULL;

    if (!vm_id) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing: vm_id");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); return;
    }

    virConnectPtr conn;
    PCV_REQUIRE_VIRT_CONN(conn, rpc_id, server, connection);
    virDomainPtr dom = pure_virt_get_domain(conn, vm_id);

    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_VM_NOT_FOUND, "VM not found");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); virt_conn_pool_release(conn); return;
    }

    gboolean is_active = virDomainGetID(dom) != (unsigned int)-1, has_persistent = FALSE;
    g_autofree gchar *live_xml = NULL;
    g_autofree gchar *config_xml = NULL;
    GPtrArray *entries = NULL;
    GError *error = NULL;
    if (!_usb_read_defs(dom, is_active, &has_persistent, &live_xml, &config_xml, &error) ||
        !pcv_usb_hostdev_list(live_xml, config_xml, &entries, &error)) {
        _usb_reply_gerror(rpc_id, server, connection, error);
        virDomainFree(dom); virt_conn_pool_release(conn); return;
    }

    JsonArray *arr = json_array_new();
    for (guint i = 0; i < entries->len; i++) {
        const PcvUsbHostdevEntry *u = g_ptr_array_index(entries, i);
        if (!pcv_res_request_can_see_usb(params, connection, u->vendor_id, u->product_id))
            continue;                                                                  
        JsonObject *usb = json_object_new();
        json_object_set_string_member(usb, "vendor_id", u->vendor_id);
        json_object_set_string_member(usb, "product_id", u->product_id);
        json_object_set_boolean_member(usb, "live", u->live);
        json_object_set_boolean_member(usb, "persistent", u->persistent);
        json_object_set_string_member(usb, "startup_policy", u->startup_policy);
        json_object_set_boolean_member(usb, "missing", u->missing);
        json_array_add_object_element(arr, usb);
    }
    g_ptr_array_unref(entries);

    JsonNode *res = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(res, arr);
    gchar *usb_list_resp = pure_rpc_build_success_response(rpc_id, res);
    pure_uds_server_send_response(server, connection, usb_list_resp);
    g_free(usb_list_resp);
    virDomainFree(dom);
    virt_conn_pool_release(conn);
}
