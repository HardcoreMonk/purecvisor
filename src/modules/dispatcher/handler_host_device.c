   
                              
                                                                              
  
                           
                                                                                                                
                                                                                                                 
                                                                                            
                                                              
                                                                                                                  
                                                                                          
                                                                                
                                                                              
  
                                                                                 
                              
   
#include "modules/dispatcher/handler_host_device.h"

#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>
#include <stdlib.h>

#include "modules/dispatcher/host_device_scan.h"
#include "modules/dispatcher/host_device_view.h"
#include "modules/dispatcher/rpc_utils.h"
#include "modules/resource/resource_domain_refs.h"
#include "modules/resource/resource_gate.h"
#include "modules/virt/virt_conn_pool.h"
#include "purecvisor/pcv_handler_util.h"

                                                                                                 
                                                
static gboolean _host_visible(gpointer ctx, PcvResKind kind, const gchar *id)
{
    return pcv_res_visible(ctx, kind, id);
}

                                 
static gboolean _host_no_domain(void)
{
    return virGetLastErrorCode() == VIR_ERR_NO_DOMAIN;
}

                                          
static PcvResDomState _host_active_state(virDomainPtr dom)
{
    int rc = virDomainIsActive(dom);
    return pcv_res_domain_active_state(rc, rc < 0 && _host_no_domain());
}

                                                                                                 
                                                                             
static PcvHostReadOutcome _host_read_domain(virDomainPtr dom, PcvHostVmDef **out)
{
    *out = NULL;
    PcvResDomState state = _host_active_state(dom);
    if (state == PCV_RES_DOM_GONE || state == PCV_RES_DOM_UNKNOWN)
        return pcv_host_device_read_outcome(FALSE, FALSE, state, FALSE);                                  
    int persistent = virDomainIsPersistent(dom);
    if (persistent < 0)                                                                                       
        return pcv_host_device_read_outcome(FALSE, _host_no_domain(), state, FALSE);

    const char *dn = virDomainGetName(dom);
    PcvHostVmDef *def = g_new0(PcvHostVmDef, 1);
    def->name = g_strdup(dn ? dn : "");
    def->persistent = persistent == 1;
    def->active = state == PCV_RES_DOM_ACTIVE;
    PcvHostReadOutcome oc = PCV_HOST_READ_USE;
    if (def->persistent) {
        char *xml = virDomainGetXMLDesc(dom, VIR_DOMAIN_XML_INACTIVE);
        oc = pcv_host_device_read_outcome(xml != NULL, xml == NULL && _host_no_domain(), state, FALSE);
        def->config_xml = xml ? g_strdup(xml) : NULL;
        free(xml);
    }
    if (oc == PCV_HOST_READ_USE && def->active) {
        char *xml = virDomainGetXMLDesc(dom, 0);
        gboolean gone = xml == NULL && _host_no_domain();
        PcvResDomState now = xml ? state : gone ? PCV_RES_DOM_GONE : _host_active_state(dom);
        oc = pcv_host_device_read_outcome(xml != NULL, gone, now, TRUE);
        def->live_xml = xml ? g_strdup(xml) : NULL;
        free(xml);
        if (oc == PCV_HOST_READ_INACTIVE_ONLY) {                                                           
            def->active = FALSE;
            oc = PCV_HOST_READ_USE;
        }
    }
    if (oc == PCV_HOST_READ_USE) {
        PcvResDomainRefs refs = { 0 };                                                                               
        const gchar *src = def->config_xml ? def->config_xml : def->live_xml;
        if (src && pcv_res_domain_refs_parse(src, &refs, NULL))
            def->owner = g_strdup(refs.owner);
        else if (src)
            oc = PCV_HOST_READ_FAIL;                                                                       
        pcv_res_domain_refs_clear(&refs);
    }
    if (oc == PCV_HOST_READ_USE)
        *out = def;
    else
        pcv_host_vm_def_free(def);
    return oc;
}

                        
static void _host_reply_error(const gchar *rpc_id, UdsServer *server, GSocketConnection *connection, const gchar *msg)
{
    gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, msg);
    pure_uds_server_send_response(server, connection, err);
    g_free(err);
}

                                                                          
void handle_host_device_list(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection)
{
    virConnectPtr conn;
    PCV_REQUIRE_VIRT_CONN(conn, rpc_id, server, connection);
    PcvResCaller rc = { 0 };
    pcv_res_caller_init(&rc, params, connection);
    PcvHostViewCaller caller = { rc.role, rc.subject, _host_visible, &rc };

    virDomainPtr *doms = NULL;
    int n = virConnectListAllDomains(conn, &doms, 0);
    if (n < 0) {
        virt_conn_pool_release(conn);
        _host_reply_error(rpc_id, server, connection, "cannot list VMs");
        pcv_res_caller_clear(&rc);
        return;
    }
    GPtrArray *vms = g_ptr_array_new_with_free_func(pcv_host_vm_def_free);
    gchar *fail_msg = NULL;
    for (int i = 0; i < n; i++) {                                                                        
        if (!fail_msg) {
            PcvHostVmDef *def = NULL;
            if (_host_read_domain(doms[i], &def) == PCV_HOST_READ_FAIL) {
                const char *dn = virDomainGetName(doms[i]);
                fail_msg = pcv_host_device_read_error_message(&caller, dn);
            } else if (def) {
                g_ptr_array_add(vms, def);
            }
        }
        virDomainFree(doms[i]);
    }
    free(doms);
    virt_conn_pool_release(conn);
    if (fail_msg) {                                                                                        
        _host_reply_error(rpc_id, server, connection, fail_msg);
        g_free(fail_msg);
        g_ptr_array_unref(vms);
        pcv_res_caller_clear(&rc);
        return;
    }

    PcvHostScan *scan = NULL;
    GError *error = NULL;
    JsonObject *view = pcv_host_device_scan("/", &scan, &error)
                       ? pcv_host_device_view_build(scan, vms, &caller, PCV_HOST_DEVICE_MAX, &error) : NULL;
    if (view) {
        JsonNode *node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(node, view);
        gchar *resp = pure_rpc_build_success_response(rpc_id, node);
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
    } else {
        _host_reply_error(rpc_id, server, connection, error ? error->message : "cannot read host devices");
    }
    g_clear_error(&error);
    pcv_host_device_scan_free(scan);
    g_ptr_array_unref(vms);
    pcv_res_caller_clear(&rc);
}
