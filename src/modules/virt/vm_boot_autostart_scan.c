   
                                 
                                                                       
  
                           
                                                                                                 
                                                                                                 
                                                                                                          
                                                                                              
                                                                                 
                                                
                                                                               
  
                                                                               
                                                   
   
#include "modules/virt/vm_boot_autostart.h"

#include <gio/gio.h>
#include <libvirt/virterror.h>
#include <stdlib.h>

                           
                                                             
GPtrArray *pcv_boot_autostart_scan(virConnectPtr conn, const PcvBootBridgeOracle *o, PcvBootScanCounts *counts,
                                   GError **error)
{
    PcvBootScanCounts local = { 0 };
    PcvBootScanCounts *c = counts ? counts : &local;
    *c = (PcvBootScanCounts){ 0 };
    if (!conn) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED, "no libvirt connection");
        return NULL;
    }
    virDomainPtr *doms = NULL;
    int n = virConnectListAllDomains(conn, &doms, VIR_CONNECT_LIST_DOMAINS_INACTIVE |
                                                  VIR_CONNECT_LIST_DOMAINS_PERSISTENT |
                                                  VIR_CONNECT_LIST_DOMAINS_AUTOSTART);
    if (n < 0) {
        const char *msg = virGetLastErrorMessage();
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "list autostart domains: %s", msg ? msg : "unknown");
        return NULL;
    }
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    for (int i = 0; i < n; i++) {
        virDomainPtr d = doms[i];
        const char *name = virDomainGetName(d);
        int state = -1, reason = -1;
        char *xml = NULL;
        if (!name || virDomainGetState(d, &state, &reason, 0) < 0
            || !(xml = virDomainGetXMLDesc(d, VIR_DOMAIN_XML_INACTIVE))) {
            const char *msg = virGetLastErrorMessage();
            c->query_error++;
            g_warning("[boot-autostart] skip '%s': query failed (%s)", name ? name : "?", msg ? msg : "unknown");
            virDomainFree(d);
            continue;
        }
        g_autofree gchar *detail = NULL;
        PcvBootClass k = pcv_boot_autostart_classify(state, reason, xml, o, &detail);
        free(xml);
        const gchar *kn = pcv_boot_autostart_class_name(k);
        switch (k) {
        case PCV_BOOT_CLASS_CANDIDATE:
            g_ptr_array_add(out, g_strdup(name));
            g_message("[boot-autostart] candidate '%s'", name);
            break;
        case PCV_BOOT_CLASS_NOT_PCV:
            c->not_pcv++;
            g_debug("[boot-autostart] skip '%s': %s (%s)", name, kn, detail ? detail : "-");
            break;
        case PCV_BOOT_CLASS_BRIDGE_MISSING:
            c->bridge_missing++;
            g_message("[boot-autostart] skip '%s': %s (%s)", name, kn, detail ? detail : "-");
            break;
        case PCV_BOOT_CLASS_STATE_REASON:
            c->state_reason++;
            g_message("[boot-autostart] skip '%s': %s (%s)", name, kn, detail ? detail : "-");
            break;
        case PCV_BOOT_CLASS_ALREADY_ACTIVE:
                                                                            
            g_message("[boot-autostart] skip '%s': %s (%s)", name, kn, detail ? detail : "-");
            break;
        case PCV_BOOT_CLASS_XML_ERROR:
        default:
            c->query_error++;
            g_warning("[boot-autostart] skip '%s': %s (%s)", name, kn, detail ? detail : "-");
            break;
        }
        virDomainFree(d);
    }
    free(doms);
    return out;
}
