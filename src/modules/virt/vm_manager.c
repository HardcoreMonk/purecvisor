   
                     
                                                    
  
                           
                                                   
                                                    
                                        
  
                   
                                                                         
                                                   
  
          
                                                       
                                                
                                                         
                                                            
                                                  
                                                   
                                                      
  
                                                          
                                                                          
                                                          
                                                     
                                                                 
                                                      
                                                  
                                             
                                                    
                                                                   
                                                                        
                                         
                                                                       
                                                                   
                                                                         
  
                          
                                                          
                                             
                                          
                                                                   
                                                            
  
                          
                                                             
                                                    
                                                                    
                                               
                                                          
                                       
                                                  
                               
                                                            
                                                         
                                             
                                          
  
                          
                                      
                                     
                                                           
                                                    
  
                   
                                                            
                                           
  
                                
                                             
                                            
                                                       
  
             
                                                                     
                                                             
                                                                          
                                                            
                                                           
   
  
                                        
                                           
  
                
                                                                  
                                                      
                                            
                               
   

                                                                       
  
           
                                                                    
                                                                   
                                                  
                                                                             
                                                                    
                                                          
                                                                      
                                             
  
                     
                                                               
                                         
                                                                 
                                                     
                                           
                                                   
                                                                              
#include "vm_manager.h"
#include "virt_conn_pool.h"
#include "vm_config_builder.h"
#include "../storage/zfs_driver.h"
#include "../audit/pcv_audit.h"
#include "utils/pcv_config.h"
#include "modules/network/pcv_qos.h"                                                       
#include "modules/network/tenant_overlay.h"                                                           
#include "modules/network/vm_iface.h"                                                                        
#include "../network/network_manager.h"                                                          
#include "modules/network/vpc/vpc_manager.h"                                        
#include "api/ws_server.h"
#if PCV_CLUSTER_ENABLED
#include "modules/cluster/cluster_manager.h"
#endif
#include "../../utils/pcv_spawn.h"                              
#include "../../utils/pcv_log.h"                                     
#include "modules/dispatcher/rpc_utils.h"                               
#include "modules/core/vm_state.h"                                                       

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <libvirt-gobject/libvirt-gobject.h>
#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>

#define PCV_VM_METADATA_URI "urn:purecvisor:metadata"

   
                                
  
                                     
                                                       
                                                            
  
                          
                                                    
                                                  
                                                      
                                                                        
                                                                       
                                
  
                                     
                                            
                                               
                                                    
                                                     
  
                                                 
                                                                      
   
struct _PureCVisorVmManager {
    GObject parent_instance;
    GVirConnection *conn;                                        
};

                        
                                                                              
G_DEFINE_TYPE(PureCVisorVmManager, purecvisor_vm_manager, G_TYPE_OBJECT)

                                                                             
                        
                                      
                                          
                                                         
                                                                                
typedef enum {
    SIGNAL_VM_STARTED = 0,                          
    SIGNAL_VM_STOPPED,                              
    SIGNAL_VM_METRICS_UPDATED,                              
    N_SIGNALS
} PcvVmManagerSignalId;

static guint signals[N_SIGNALS] = { 0 };

                                                                             
                                     
                                                                                

   
                                 
                                         
  
                                                     
                                                  
                                                     
  
                                                     
                                                       
                                        
  
                                                           
                                                  
  
                                        
                                                  
   
static gint _extract_vnc_port_from_domain(GVirDomain *dom) {
    GError *err = nullptr;
    GVirConfigDomain *config = nullptr;
    gchar *xml_data = nullptr;
    gint port = -1;

                                                            
    config = gvir_domain_get_config(dom, 0, &err);
    if (err) {
                                           
        g_error_free(err);
        return -1;
    }

    xml_data = gvir_config_object_to_xml(GVIR_CONFIG_OBJECT(config));
    g_object_unref(config);

    if (!xml_data) return -1;

                               
                                                                 
                                                                   
                                                                        
                                                                       
                               
    
                                      
    GRegex *regex = g_regex_new("<graphics[^>]+port='(\\d+)'",
                                G_REGEX_CASELESS | G_REGEX_MULTILINE, 0, NULL);
    
    GMatchInfo *match_info;
    if (g_regex_match(regex, xml_data, 0, &match_info)) {
        gchar *port_str = g_match_info_fetch(match_info, 1);
        if (port_str) {
            port = (gint)g_ascii_strtoll(port_str, NULL, 10);
            g_free(port_str);
        }
    }

    g_match_info_free(match_info);
    g_regex_unref(regex);
    g_free(xml_data);

    return port;
}

                                                                             
                      
                                              
                                         
                              
                                                                                

   
                                  
                                         
                                           
  
                                                      
  
                                                            
                                                                        
   
static void purecvisor_vm_manager_finalize(GObject *object) {
    PureCVisorVmManager *self = PURECVISOR_VM_MANAGER(object);
    if (self->conn) {
        g_object_unref(self->conn);
    }
    G_OBJECT_CLASS(purecvisor_vm_manager_parent_class)->finalize(object);
}

   
                                    
                                            
                                              
  
                                                       
                                                      
                  
  
                                                        
                                                                      
   
static void purecvisor_vm_manager_class_init(PureCVisorVmManagerClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = purecvisor_vm_manager_finalize;

                                                                         
                    
                                                                            

       
                                       
                                   
                                       
      
                                                              
                         
       
    signals[SIGNAL_VM_STARTED] =
        g_signal_new(PCV_VM_SIGNAL_STARTED,
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_LAST,
                     0,                                               
                     NULL, NULL,                                     
                     NULL,                                            
                     G_TYPE_NONE,                         
                     1,                                
                     G_TYPE_STRING);                         

       
                                       
                                   
                                       
      
                                                             
                         
       
    signals[SIGNAL_VM_STOPPED] =
        g_signal_new(PCV_VM_SIGNAL_STOPPED,
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_LAST,
                     0,
                     NULL, NULL,
                     NULL,
                     G_TYPE_NONE,
                     1,
                     G_TYPE_STRING);                         

       
                                               
                                 
                                                            
      
                                   
                                                    
                                            
                                               
       
    signals[SIGNAL_VM_METRICS_UPDATED] =
        g_signal_new(PCV_VM_SIGNAL_METRICS_UPDATED,
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_LAST,
                     0,
                     NULL, NULL,
                     NULL,
                     G_TYPE_NONE,
                     1,
                     G_TYPE_POINTER);                                 
}

                               
                                               
static void purecvisor_vm_manager_init(PureCVisorVmManager *self) {
    self->conn = nullptr;
}

   
                             
                        
  
                                                       
                                
  
                                                                           
                                                           
   
PureCVisorVmManager *purecvisor_vm_manager_new(GVirConnection *conn) {
    PureCVisorVmManager *self = g_object_new(PURECVISOR_TYPE_VM_MANAGER, NULL);
    if (conn) {
        self->conn = g_object_ref(conn);
    }
    return self;
}

                                                                             
                              
  
         
                                      
                                                
                                            
                                       
                                       
                                                                                

   
                    
                             
                                        
   
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

   
                                                                         
  
                                                                       
                                                     
                                 
  
                                                        
                                                    
                                                            
                                  
  
                                                           
                                                 
                                                
                                               
  
                                                     
                                               
  
                                               
                                                    
                                                                   
                                                
   
static gchar *
_vm_xml_inject_metadata_child(const gchar *xml, const gchar *child_xml)
{
    if (!xml)
        return NULL;                                              
    if (!child_xml || !*child_xml)
        return g_strdup(xml);                                         

                                                                  
                                                         
                                                   
    const gchar *meta_close = strstr(xml, "</metadata>");
    if (meta_close) {
        gsize prefix_len = (gsize)(meta_close - xml);                            
        return g_strdup_printf("%.*s    %s%s",
                               (gint)prefix_len, xml,
                               child_xml, meta_close);
    }

                                                                
    gchar *metadata = g_strdup_printf(
        "  <metadata>\n"
        "    %s"
        "  </metadata>\n",
        child_xml);

                                                           
                                                         
                                                      
    const gchar *insert = strstr(xml, "</name>");
    gchar *patched = NULL;
    if (insert) {
        insert += strlen("</name>");                                              
        patched = g_strdup_printf("%.*s%s%s",
                                  (gint)(insert - xml), xml,
                                  metadata, insert);
    } else {
        insert = strstr(xml, "<devices>");                    
        if (insert) {
            patched = g_strdup_printf("%.*s%s%s",
                                      (gint)(insert - xml), xml,
                                      metadata, insert);
        } else {
            const gchar *end = strstr(xml, "</domain>");                    
            patched = end
                ? g_strdup_printf("%.*s%s%s", (gint)(end - xml), xml, metadata, end)
                : g_strdup(xml);                                  
        }
    }
    g_free(metadata);
    return patched;
}

                                                           
                                                                      
                                                         
                                                                 
                        

   
                                                                                  
  
                                                                           
                                                             
                                                                         
                                                        
                               
  
                                                             
                                                   
               
  
                                                 
                                                    
                                                  
                                 
  
                                                  
                                                                   
                                                         
                                                            
                                                      
   
gchar *
_overlay_metadata_xml(const gchar *network_mode, const gchar *tenant)
{
    if (g_strcmp0(network_mode, "tenant-overlay") != 0)
        return g_strdup("");

    gchar *safe_mode = g_markup_escape_text(network_mode, -1);
    gchar *safe_tenant = g_markup_escape_text(tenant ? tenant : "", -1);
    gchar *xml = g_strdup_printf(
        "<pcv:overlay xmlns:pcv='%s' network_mode='%s' tenant='%s'/>\n",
        PCV_OVERLAY_METADATA_URI, safe_mode, safe_tenant);
    g_free(safe_mode);
    g_free(safe_tenant);
    return xml;
}

   
                                                                           
  
                                                             
                                                        
                                       
  
                                               
                                                                                
                                                                     
                 
                                                                       
                                                                          
                                                             
                     
                                                              
                                          
  
                                                   
                                                  
                                                 
                                       
  
                                                                 
                                                                     
                                                               
                                                 
   
gboolean
_overlay_metadata_parse(const gchar *metadata_xml, gchar **mode_out, gchar **tenant_out)
{
    if (!metadata_xml || !mode_out || !tenant_out)
        return FALSE;

    gchar *mode = NULL, *tenant = NULL;
                                                                          
                                                                          
    GRegex *mode_re = g_regex_new(
        "<(?:[\\w.-]+:)?overlay\\b[^>]*\\bnetwork_mode\\s*=\\s*[\"']([^\"']*)[\"']", 0, 0, NULL);
    GRegex *tenant_re = g_regex_new(
        "<(?:[\\w.-]+:)?overlay\\b[^>]*\\btenant\\s*=\\s*[\"']([^\"']*)[\"']", 0, 0, NULL);
    GMatchInfo *match = NULL;

                                                                
    if (mode_re && g_regex_match(mode_re, metadata_xml, 0, &match))
        mode = g_match_info_fetch(match, 1);
    g_clear_pointer(&match, g_match_info_free);                                 

                                                           
    if (tenant_re && g_regex_match(tenant_re, metadata_xml, 0, &match))
        tenant = g_match_info_fetch(match, 1);
    g_clear_pointer(&match, g_match_info_free);

    if (mode_re) g_regex_unref(mode_re);
    if (tenant_re) g_regex_unref(tenant_re);

                                                                  
    if (!mode || !tenant) {
        g_free(mode);
        g_free(tenant);
        return FALSE;
    }

    *mode_out = mode;
    *tenant_out = tenant;
    return TRUE;
}

   
                                                               
  
                                                    
                                                   
  
                                                                
                                                 
                                                       
  
                                                           
                                                     
                                                                           
                                                                     
                                                           
                                                      
                                                          
                                               
                                                     
                                                          
                                                                    
                                               
                                                                     
                                                                
                                                               
                                                            
                                                      
                                                 
                                                
                                                         
                                                                
                                                         
  
                                             
                                                           
                                                           
   
gchar *
_qos_metadata_xml(const PcvQosSla *sla)
{
    if (!sla)
        return g_strdup("");

    return g_strdup_printf(
        "<qos min_mbps='%u' max_mbps='%u' burst_kb='%u'/>\n",
        sla->min_mbps, sla->max_mbps, sla->burst_kb);
}

   
                                                                                  
  
                                                            
                                                      
                                                       
                                                 
                                                  
                                    
  
                                                     
                                                        
  
                                           
                                                                    
                                                 
   
gboolean
_qos_metadata_parse(const gchar *metadata_xml, PcvQosSla *out)
{
    if (!metadata_xml || !out)
        return FALSE;

    GRegex *min_re = g_regex_new(
        "<(?:[\\w.-]+:)?qos\\b[^>]*\\bmin_mbps\\s*=\\s*[\"']([0-9]+)[\"']", 0, 0, NULL);
    GRegex *max_re = g_regex_new(
        "<(?:[\\w.-]+:)?qos\\b[^>]*\\bmax_mbps\\s*=\\s*[\"']([0-9]+)[\"']", 0, 0, NULL);
    GRegex *burst_re = g_regex_new(
        "<(?:[\\w.-]+:)?qos\\b[^>]*\\bburst_kb\\s*=\\s*[\"']([0-9]+)[\"']", 0, 0, NULL);
    GMatchInfo *match = NULL;
    gchar *min_s = NULL, *max_s = NULL, *burst_s = NULL;

    if (min_re && g_regex_match(min_re, metadata_xml, 0, &match))
        min_s = g_match_info_fetch(match, 1);
    g_clear_pointer(&match, g_match_info_free);

    if (max_re && g_regex_match(max_re, metadata_xml, 0, &match))
        max_s = g_match_info_fetch(match, 1);
    g_clear_pointer(&match, g_match_info_free);

    if (burst_re && g_regex_match(burst_re, metadata_xml, 0, &match))
        burst_s = g_match_info_fetch(match, 1);
    g_clear_pointer(&match, g_match_info_free);

    if (min_re) g_regex_unref(min_re);
    if (max_re) g_regex_unref(max_re);
    if (burst_re) g_regex_unref(burst_re);

                                                                
    if (!min_s || !max_s) {
        g_free(min_s);
        g_free(max_s);
        g_free(burst_s);
        return FALSE;
    }

    memset(out, 0, sizeof *out);                                             
    out->min_mbps = (guint32)g_ascii_strtoull(min_s, NULL, 10);
    out->max_mbps = (guint32)g_ascii_strtoull(max_s, NULL, 10);
    out->burst_kb = burst_s ? (guint32)g_ascii_strtoull(burst_s, NULL, 10) : 256;                         

    g_free(min_s);
    g_free(max_s);
    g_free(burst_s);
    return TRUE;
}

   
                                                                     
  
                                                    
                                             
  
                                                                     
                                                                    
  
                                                        
                                                                    
                                                   
                            
   
gboolean
pcv_vm_qos_metadata_write(virDomainPtr dom, const PcvQosSla *sla, GError **error)
{
    if (!dom || !sla) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "pcv_vm_qos_metadata_write: dom/sla required");
        return FALSE;
    }

                                                              
    int active = virDomainIsActive(dom);
    if (active < 0) {
        virErrorPtr vir_err = virGetLastError();
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Failed to read VM active state: %s",
                    vir_err ? vir_err->message : "unknown");
        return FALSE;
    }
                                                         
    unsigned int flags = VIR_DOMAIN_AFFECT_CONFIG;
    if (active == 1)
        flags |= VIR_DOMAIN_AFFECT_LIVE;

    gchar *xml = _qos_metadata_xml(sla);
    int ret = virDomainSetMetadata(dom, VIR_DOMAIN_METADATA_ELEMENT, xml,
                                   "pcv", PCV_QOS_METADATA_URI, flags);
    g_free(xml);

    if (ret != 0) {
        virErrorPtr vir_err = virGetLastError();
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "virDomainSetMetadata(qos) failed: %s",
                    vir_err ? vir_err->message : "unknown");
        return FALSE;
    }
    return TRUE;
}

   
                                                                   
                                  
  
                                                     
                                                     
                                                         
                                     
  
                                                                       
                             
  
                                                                            
                                              
                                     
  
                                                            
                                                               
                                                    
                                         
                                                     
                                                       
                                           
   
PcvQosMetaResult
pcv_vm_qos_metadata_read(virDomainPtr dom, PcvQosSla *out)
{
    if (!dom || !out)
        return PCV_QOS_META_INVALID;

    char *meta = virDomainGetMetadata(dom, VIR_DOMAIN_METADATA_ELEMENT,
                                      PCV_QOS_METADATA_URI, 0);
    if (!meta) {
                                                                  
                                                            
        virErrorPtr vir_err = virGetLastError();
        gboolean truly_absent = vir_err && vir_err->code == VIR_ERR_NO_DOMAIN_METADATA;
        virResetLastError();                                            
        return truly_absent ? PCV_QOS_META_ABSENT : PCV_QOS_META_INVALID;
    }

                                                        
    gboolean parsed = _qos_metadata_parse(meta, out);
    free(meta);                                                            
    return parsed ? PCV_QOS_META_OK : PCV_QOS_META_INVALID;
}

   
                                                           
                                                                         
                                               
                   
  
                                                           
                                                
                                 
  
                                                  
                                                        
                                       
  
                                                                       
                         
   
static PcvQosSla
_qos_default_tier_sla(void)
{
    gint uplink_cfg = pcv_config_get_int("qos", "uplink_mbps", 1000);
    guint32 default_max_mbps;
    if (uplink_cfg <= 0) {
        PCV_LOG_WARN("vm_manager",
            "[qos] qos.uplink_mbps 설정값(%d)이 비합리(<=0) — 기본값 1000Mbit 로 폴백",
            uplink_cfg);
        default_max_mbps = 1000;
    } else {
        default_max_mbps = (guint32)uplink_cfg;
    }

    PcvQosSla sla;
    memset(&sla, 0, sizeof sla);
    sla.min_mbps = 0;
    sla.max_mbps = default_max_mbps;
    sla.burst_kb = 256;
    return sla;
}

   
                                                                        
                                                                 
  
                                                      
                                                    
                                                 
  
                                                                           
                                                                                
                                                                  
                                                      
  
                                                          
                                               
                                                    
                                                 
                                                  
   
void
pcv_qos_backfill_existing(void)
{
    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        PCV_LOG_WARN("vm_manager", "[qos-migrate] libvirt 연결 실패 — backfill 건너뜀");
        return;
    }

    virDomainPtr *domains = NULL;
    int n = virConnectListAllDomains(conn, &domains, 0);
    if (n < 0) {
        PCV_LOG_WARN("vm_manager", "[qos-migrate] virConnectListAllDomains 실패");
        virt_conn_pool_release(conn);
        return;
    }

    PcvQosSla default_sla = _qos_default_tier_sla();

    guint backfilled = 0;
    guint skipped_invalid = 0;
                                                               
    for (int i = 0; i < n; i++) {
        PcvQosSla existing;
        PcvQosMetaResult meta_state = pcv_vm_qos_metadata_read(domains[i], &existing);

                                                         
        if (meta_state == PCV_QOS_META_ABSENT) {
            GError *werr = NULL;
            if (pcv_vm_qos_metadata_write(domains[i], &default_sla, &werr)) {
                backfilled++;
            } else {
                const char *name = virDomainGetName(domains[i]);
                PCV_LOG_WARN("vm_manager",
                    "[qos-migrate] VM '%s' 기본 SLA backfill 실패: %s",
                    name ? name : "?", werr ? werr->message : "unknown");
                g_clear_error(&werr);
            }
        } else if (meta_state == PCV_QOS_META_INVALID) {
                                                         
                                                  
                              
            const char *name = virDomainGetName(domains[i]);
                                                                     
                                                               
                                                              
                         
            PCV_LOG_WARN("vm_manager",
                "[qos-migrate] VM '%s' qos 메타데이터 읽기/파싱 실패(부재 아님 — "
                "덮어쓰기 회피) — 기본 티어로 덮어쓰지 않음, 수동 확인 필요", name ? name : "?");
            skipped_invalid++;
        }
                                                    

        virDomainFree(domains[i]);
    }
    free(domains);                                             
    virt_conn_pool_release(conn);

    if (backfilled > 0) {
        PCV_LOG_INFO("vm_manager", "[qos-migrate] backfilled default SLA to %u VMs", backfilled);
    }
    if (skipped_invalid > 0) {
        PCV_LOG_WARN("vm_manager",
            "[qos-migrate] skipped %u VMs with unparseable qos metadata (manual repair needed)",
            skipped_invalid);
    }
}

                                                                             
                                                                    
  
                                                              
                                                  
                                                                
                                                                   
                                                  
                                                      
                                                 
  
                                                    
                                                             
                                                              
                                                     
                                                      
                                                              
                          
                                                                                

                                                        
                                                   
                                                        
                                                          
  
                                                              
                                                              
                    
  
                                                                   
                                                                               
                                                            
                                                  
                                              
                                                                          
                                                                  
                                              
  
                                                                        
                                                                   
static gboolean
_qos_derive_tenant_iface(virDomainPtr dom, const gchar *vm_name,
                          gchar **tenant_out, gchar **iface_out)
{
    *tenant_out = NULL;
    *iface_out  = NULL;

    char *meta = virDomainGetMetadata(dom, VIR_DOMAIN_METADATA_ELEMENT,
                                      PCV_OVERLAY_METADATA_URI, 0);
    gchar *mode = NULL, *tenant = NULL;
    gboolean is_overlay = FALSE;
    if (meta) {
        if (_overlay_metadata_parse(meta, &mode, &tenant) &&
            g_strcmp0(mode, "tenant-overlay") == 0)
            is_overlay = TRUE;
        free(meta);                                                             
    } else {
                                                                      
                                                     
                                                                       
        virResetLastError();
    }

    if (is_overlay) {
        gchar *ep = NULL;
        if (pcv_tenant_overlay_get_member_ep(tenant, vm_name, &ep) && ep && *ep) {
            *tenant_out = tenant;               
            *iface_out  = g_strdup_printf("%s-h", ep);
            g_free(ep);
            g_free(mode);
            return TRUE;
        }
        g_free(ep);
        PCV_LOG_WARN("vm_manager",
            "[qos] VM '%s' overlay metadata 존재하나 mesh 미조인 — iface 발견 불가"
            "(경합 또는 구 도메인, reconcile 이 배선되면 다음 주기가 재시도)", vm_name);
        g_free(tenant);
        g_free(mode);
        return FALSE;
    }
    g_free(mode);
    g_free(tenant);

                                                                   
                                 
    GPtrArray *ifaces = pcv_vm_iface_list(vm_name);
    gchar *vnet = (ifaces->len > 0) ? g_strdup(g_ptr_array_index(ifaces, 0)) : NULL;
    g_ptr_array_unref(ifaces);
    if (!vnet)
        return FALSE;                                          

    *tenant_out = g_strdup("default");                                      
    *iface_out  = vnet;
    return TRUE;
}

   
                                                                                      
  
                                                    
                                                    
                           
  
                                                                                  
                                                                        
                                                                  
                                                                      
   
gboolean
pcv_vm_qos_derive_context(virDomainPtr dom, const gchar *vm_name,
                          gchar **tenant_out, gchar **iface_out,
                          PcvQosSla *sla_out)
{
    if (tenant_out) *tenant_out = NULL;
    if (iface_out)  *iface_out  = NULL;

    if (!dom || !vm_name || !*vm_name || !sla_out)
        return FALSE;

    gchar *tenant = NULL, *iface = NULL;
    if (!_qos_derive_tenant_iface(dom, vm_name, &tenant, &iface)) {
        g_free(tenant);
        g_free(iface);
        return FALSE;
    }

                                                          
                                                    
                                                        
                                         
    PcvQosMetaResult mstate = pcv_vm_qos_metadata_read(dom, sla_out);
    if (mstate != PCV_QOS_META_OK) {
        if (mstate == PCV_QOS_META_INVALID)
            PCV_LOG_WARN("vm_manager",
                "[qos] VM '%s' qos 메타데이터 읽기/파싱 실패(부재 아님 — 덮어쓰기 회피) — "
                "런타임 기본 티어만 적용(영속 미변경)", vm_name);
        *sla_out = _qos_default_tier_sla();
    }
    g_strlcpy(sla_out->tenant, tenant, sizeof sla_out->tenant);
    g_strlcpy(sla_out->vm, vm_name, sizeof sla_out->vm);

    if (tenant_out) *tenant_out = tenant; else g_free(tenant);
    if (iface_out)  *iface_out  = iface;  else g_free(iface);
    return TRUE;
}

   
                                                                                
                                                                      
  
                                                             
                                                    
                                                               
                                                       
                
  
                                                     
                                                       
                               
  
                                                                    
                                                
   
GPtrArray *
pcv_vm_qos_expected_provider(void)
{
    GPtrArray *entries = g_ptr_array_new_with_free_func(g_free);

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        PCV_LOG_WARN("vm_manager", "[qos] expected-provider: libvirt 연결 실패 — 빈 목록 반환");
        return entries;
    }

    virDomainPtr *domains = NULL;
    int n = virConnectListAllDomains(conn, &domains, VIR_CONNECT_LIST_DOMAINS_ACTIVE);
    if (n < 0) {
        PCV_LOG_WARN("vm_manager", "[qos] expected-provider: virConnectListAllDomains 실패");
        virt_conn_pool_release(conn);
        return entries;
    }

    for (int i = 0; i < n; i++) {
        const char *name = virDomainGetName(domains[i]);
        if (name && *name) {
            gchar *tenant = NULL, *iface = NULL;
            PcvQosSla sla;
            if (pcv_vm_qos_derive_context(domains[i], name, &tenant, &iface, &sla)) {
                PcvQosExpectedEntry *e = g_new0(PcvQosExpectedEntry, 1);
                g_strlcpy(e->tenant, tenant, sizeof e->tenant);
                g_strlcpy(e->vm, name, sizeof e->vm);
                g_strlcpy(e->iface, iface, sizeof e->iface);
                e->sla = sla;                                                                
                g_ptr_array_add(entries, e);
            }
            g_free(tenant);
            g_free(iface);
        }
        virDomainFree(domains[i]);
    }
    free(domains);                                             
    virt_conn_pool_release(conn);

    return entries;
}

   
                                                                             
                                      
  
                                                                        
                                                                          
                                                      
      
  
                                                                       
                                                                  
                                                                       
                                                                
  
                                                             
                                                     
                                                   
                                 
  
                                                             
                                                                    
                                                                
                                                         
   
gboolean
_overlay_live_iface_parse(const gchar *domain_xml, gchar **tap_out, gchar **mac_out)
{
    if (!domain_xml || !tap_out || !mac_out)
        return FALSE;
    *tap_out = NULL;
    *mac_out = NULL;

                                                                      
                                                        
                                                                    
    GRegex *iface_re = g_regex_new(
        "<interface\\b[^>]*\\btype='ethernet'[^>]*>(.*?)</interface>",
        G_REGEX_DOTALL, 0, NULL);
    if (!iface_re)
        return FALSE;

    GMatchInfo *mi = NULL;
    gchar *block = NULL;
    guint eth_count = 0;
    if (g_regex_match(iface_re, domain_xml, 0, &mi)) {
        while (g_match_info_matches(mi)) {
            eth_count++;
            if (!block)
                block = g_match_info_fetch(mi, 1);                          
            g_match_info_next(mi, NULL);
        }
    }
    g_clear_pointer(&mi, g_match_info_free);
    g_regex_unref(iface_re);

    if (!block)
        return FALSE;                                                  
    if (eth_count > 1)
        PCV_LOG_WARN("vm_manager",
                     "tenant-overlay 도메인에 ethernet iface 가 %u 개 — 첫 번째만 배선",
                     eth_count);

    gchar *tap = NULL, *mac = NULL;
    GRegex *tap_re = g_regex_new("<target\\b[^>]*\\bdev='([^']*)'", 0, 0, NULL);
    GRegex *mac_re = g_regex_new("<mac\\b[^>]*\\baddress='([^']*)'", 0, 0, NULL);
    GMatchInfo *m2 = NULL;
    if (tap_re && g_regex_match(tap_re, block, 0, &m2))
        tap = g_match_info_fetch(m2, 1);
    g_clear_pointer(&m2, g_match_info_free);
    if (mac_re && g_regex_match(mac_re, block, 0, &m2))
        mac = g_match_info_fetch(m2, 1);
    g_clear_pointer(&m2, g_match_info_free);
    if (tap_re) g_regex_unref(tap_re);
    if (mac_re) g_regex_unref(mac_re);
    g_free(block);

                                                           
    if (!tap || !*tap || !mac || !*mac) {
        g_free(tap);
        g_free(mac);
        return FALSE;
    }
    *tap_out = tap;
    *mac_out = mac;
    return TRUE;
}

   
                                                                       
                               
  
                                                                      
                                                                
                                                                   
                                                    
  
                                                         
                                                                
  
                                    
                                                           
   
gchar *
_overlay_gw_cidr_from_subnet(const gchar *subnet_cidr)
{
    if (!subnet_cidr)
        return NULL;

    gchar **cidr_parts = g_strsplit(subnet_cidr, "/", 2);                            
                                           
    if (!cidr_parts[0] || !cidr_parts[1] || !*cidr_parts[1]) {
        g_strfreev(cidr_parts);
        return NULL;
    }

    gchar **octets = g_strsplit(cidr_parts[0], ".", -1);                          
    if (g_strv_length(octets) != 4) {                                   
        g_strfreev(octets);
        g_strfreev(cidr_parts);
        return NULL;
    }

    gchar *gw = g_strdup_printf("%s.%s.%s.1/%s",
                                octets[0], octets[1], octets[2], cidr_parts[1]);
    g_strfreev(octets);
    g_strfreev(cidr_parts);
    return gw;
}

   
                                                                                      
  
                                   
                                                            
                                                        
                                                    
                                                             
                                                         
                                                
  
                                                        
                                                 
                             
  
                                                    
                                                      
  
                                                            
   
gchar *
_overlay_ethernet_iface_xml(void)
{
    return g_strdup(
        "    <interface type='ethernet'>\n"
        "      <model type='virtio'/>\n"
        "    </interface>\n");
}

   
                                                                             
  
                                                  
                                                           
  
                                                          
                                                                           
                                                                         
   
static gchar *
_vm_xml_with_owner_metadata(const gchar *xml, const gchar *owner)
{
                           
                                                    
                                                 
                                                  
      
               
                                           
                                                                              
                                                                 
                                   
    if (!xml || !owner || !*owner)
        return g_strdup(xml);

    gchar *safe_owner = g_markup_escape_text(owner, -1);
    gchar *owner_child = g_strdup_printf(
        "<pcv:owner xmlns:pcv='%s'>%s</pcv:owner>\n",
        PCV_VM_METADATA_URI, safe_owner);
    g_free(safe_owner);

    gchar *patched = _vm_xml_inject_metadata_child(xml, owner_child);
    g_free(owner_child);

    return patched;
}

   
                                                                                
                                                         
                                                                      
                                                       
                                                                  
                                                         
   

   
                                                                       
  
                                                    
                                          
  
                                                                     
                                                           
                 
  
                            
                             
   
gint64
_hugepage_preflight_need_pages(gint ram_mb)
{
    return ((gint64)ram_mb + 1) / 2;                     
}

   
                                                                             
  
                                                    
                                                     
  
                                                               
                                                                               
                                                                                       
                                                    
                                    
                                                          
                                                           
  
                                         
                                                            
                                                    
   
gchar *
_build_memory_backing_xml(gboolean hugepages, gboolean want_shared)
{
    if (hugepages && want_shared) {
        return g_strdup("  <memoryBacking><hugepages><page size='2048' unit='KiB'/></hugepages>"
                        "<nosharepages/><access mode='shared'/></memoryBacking>\n");
    } else if (want_shared) {
        return g_strdup("  <memoryBacking><nosharepages/><source type='memfd'/>"
                        "<access mode='shared'/></memoryBacking>\n");
    } else if (hugepages) {
        return g_strdup("  <memoryBacking><hugepages><page size='2048' unit='KiB'/>"
                        "</hugepages><nosharepages/></memoryBacking>\n");
    } else {
        return g_strdup("  <memoryBacking><nosharepages/></memoryBacking>\n");
    }
}

   
                                                            
                                                                         
                                                                                   
                                                                            
                                                                       
                                                                           
                                                        
   

   
                                                                              
  
                                                         
                                                  
                          
  
                                                                           
                                                                   
                                                                
                                                           
                                                          
  
                                                              
                                                           
                                                                      
                                                    
                                                        
                                                          
  
                                             
                                                                  
                                                       
                                                                               
                                                             
   
gchar *
_build_bridge_iface_xml(const gchar *safe_bridge,
                        const gchar *virtualport_xml,
                        const gchar *vlan_xml,
                        gint mtu)
{
    gchar *mtu_xml = (mtu > 0)
        ? g_strdup_printf("      <mtu size='%d'/>\n", mtu)
        : g_strdup("");
    gchar *xml = g_strdup_printf(
        "    <interface type='bridge'>\n"
        "      <source bridge='%s'/>\n"
        "%s"
        "      <model type='virtio'/>\n"
        "      <driver name='vhost'/>\n"
        "%s"
        "%s"
        "    </interface>\n",
        safe_bridge, virtualport_xml, mtu_xml, vlan_xml);
    g_free(mtu_xml);
    return xml;
}

   
                                                                                
  
                                                          
                                                            
  
                                                                   
                                                                     
                                                                              
                                                              
                                                               
                                                                  
                                              
                                                           
  
                                         
                                                          
   
gchar *
_vm_xml_ensure_memballoon_stats(const gchar *xml)
{
    if (!xml) return NULL;
    if (strstr(xml, "<stats period=")) return g_strdup(xml);                  

    const gchar *mb = strstr(xml, "<memballoon");
    if (mb) {
        const gchar *gt = strchr(mb, '>');
        if (!gt) return g_strdup(xml);                        
        if (*(gt - 1) == '/') {
                                                                            
            gsize head = (gsize)(gt - 1 - xml);
            return g_strdup_printf("%.*s><stats period='10'/></memballoon>%s",
                                   (gint)head, xml, gt + 1);
        }
                                                                    
        gsize head = (gsize)(gt + 1 - xml);
        return g_strdup_printf("%.*s<stats period='10'/>%s", (gint)head, xml, gt + 1);
    }

                                                        
    const gchar *dev = strstr(xml, "</devices>");
    if (!dev) return g_strdup(xml);
    gsize head = (gsize)(dev - xml);
    return g_strdup_printf("%.*s    <memballoon model='virtio'>\n"
                           "      <stats period='10'/>\n"
                           "    </memballoon>\n%s",
                           (gint)head, xml, dev);
}

   
                                                             
  
                                                       
                                          
  
                                                           
                                                                  
                                                           
                                   
  
                           
                                                          
   
gchar *
_vm_xml_insert_virtio_rng(const gchar *xml)
{
    if (!xml) return NULL;
    if (strstr(xml, "<rng")) return g_strdup(xml);                  
    const gchar *dev = strstr(xml, "</devices>");
    if (!dev) return g_strdup(xml);
    gsize head = (gsize)(dev - xml);
    return g_strdup_printf("%.*s    <rng model='virtio'>\n"
                           "      <backend model='random'>/dev/urandom</backend>\n"
                           "    </rng>\n%s",
                           (gint)head, xml, dev);
}

   
                                                                              
  
                                                    
                                                
  
                                                           
                                                               
                                                               
                                                             
                                                                
                                                                   
                                                                      
                                                         
  
                           
                                                      
   
gchar *
_vm_xml_ensure_q35_hotplug_ports(const gchar *xml)
{
    constexpr guint required_ports = 8;
    if (!xml) return NULL;

    xmlDocPtr doc = xmlReadMemory(xml, (int)strlen(xml), "domain.xml", NULL,
                                  XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    if (!doc) return g_strdup(xml);

    xmlNodePtr root = xmlDocGetRootElement(doc);
    xmlNodePtr devices = NULL;
    gboolean has_devices = FALSE;
    gboolean is_q35 = FALSE;
    gboolean has_pcie_root = FALSE;
    guint ports = 0;
    for (xmlNodePtr child = root ? root->children : NULL; child; child = child->next) {
        if (child->type != XML_ELEMENT_NODE) continue;
        if (xmlStrcmp(child->name, BAD_CAST "os") == 0) {
            for (xmlNodePtr os_child = child->children; os_child; os_child = os_child->next) {
                if (os_child->type != XML_ELEMENT_NODE ||
                    xmlStrcmp(os_child->name, BAD_CAST "type") != 0) continue;
                xmlChar *machine = xmlGetProp(os_child, BAD_CAST "machine");
                if (machine && strstr((const gchar *)machine, "q35")) is_q35 = TRUE;
                if (machine) xmlFree(machine);
            }
        } else if (xmlStrcmp(child->name, BAD_CAST "devices") == 0) {
            devices = child;
            has_devices = TRUE;
        }
    }

    if (is_q35 && devices) {
        for (xmlNodePtr child = devices->children; child; child = child->next) {
            if (child->type != XML_ELEMENT_NODE ||
                xmlStrcmp(child->name, BAD_CAST "controller") != 0) continue;
            xmlChar *type = xmlGetProp(child, BAD_CAST "type");
            xmlChar *model = xmlGetProp(child, BAD_CAST "model");
            if (type && model && xmlStrcmp(type, BAD_CAST "pci") == 0 &&
                xmlStrcmp(model, BAD_CAST "pcie-root-port") == 0)
                ports++;
            if (type && model && xmlStrcmp(type, BAD_CAST "pci") == 0 &&
                xmlStrcmp(model, BAD_CAST "pcie-root") == 0)
                has_pcie_root = TRUE;
            if (type) xmlFree(type);
            if (model) xmlFree(model);
        }
    }
    xmlFreeDoc(doc);

    if (!is_q35 || !has_devices || (has_pcie_root && ports >= required_ports))
        return g_strdup(xml);
    const gchar *insert = g_strrstr(xml, "</devices>");
    if (!insert) return g_strdup(xml);

    GString *controllers = g_string_new(NULL);
    if (!has_pcie_root) {
                                                                        
                                                        
        g_string_append(controllers,
            "    <controller type='pci' index='0' model='pcie-root'/>\n");
    }
    for (guint i = ports; i < required_ports; i++) {
        g_string_append(controllers,
            "    <controller type='pci' model='pcie-root-port'/>\n");
    }
    gchar *patched = g_strdup_printf("%.*s%s%s", (gint)(insert - xml), xml,
                                     controllers->str, insert);
    g_string_free(controllers, TRUE);
    return patched;
}

   
                                                                                 
  
                                                         
                                                         
                                            
  
                                                                            
                                                                                       
                                                                         
                                                               
                     
  
                                                  
                                                                       
                                                       
                                                                                     
                                                                 
  
                                                                           
                                                                               
                                                   
  
                           
                                                          
   
gchar *
_vm_xml_apply_cpu_invtsc(const gchar *xml)
{
    if (!xml) return NULL;
    if (strstr(xml, "invtsc")) return g_strdup(xml);                  
    const gchar *p = strstr(xml, "<cpu mode=\"");
    if (!p) return g_strdup(xml);                                 
    const gchar *gt = strchr(p, '>');
    if (!gt || *(gt - 1) != '/') return g_strdup(xml);                                    
    gsize head = (gsize)(gt - 1 - xml);                                  
    return g_strdup_printf(
        "%.*s migratable=\"off\"><feature policy=\"require\" name=\"invtsc\"/></cpu>%s",
        (gint)head, xml, gt + 1);
}

   
                                                  
  
                                                        
                                                     
                                                   
                                                    
  
                                                                               
                                                                   
                                                                           
                                            
                                             
                                                   
                                                
                                              
  
                              
                                                 
                                                     
   
static void create_vm_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    CreateVmTaskData *data = (CreateVmTaskData *)task_data;
    GError *error = nullptr;
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

        gchar *size_str = g_strdup_printf("%dG", final_disk_size);
        const gchar *zfs_argv[] = {"zfs", "create", "-V", size_str,
                                    zvol_name, NULL};
        gchar *std_err = nullptr;

        if (!pcv_spawn_sync(zfs_argv, NULL, &std_err, &error)) {
            gchar *err_msg = error ? error->message
                                   : (std_err ? g_strstrip(std_err)
                                              : "Unknown ZFS error");
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                    "ZFS Provisioning Failed: %s", err_msg);
            if (error) g_error_free(error);
            g_free(std_err); g_free(size_str);
            g_free(zvol_name); g_free(zvol_dev);
            return;
        }
        g_free(std_err); g_free(size_str);
        disk_path = zvol_dev;
        g_free(zvol_name);
        PCV_LOG_INFO("vm_manager", "VM '%s': zvol disk created at %s (%dG)",
                     data->name, zvol_pool, final_disk_size);

                                                                      
                                                            
                                                        
        if (data->base_image && *data->base_image &&
            g_file_test(data->base_image, G_FILE_TEST_EXISTS)) {
            const gchar *udev_argv[] = {"udevadm", "settle", "--timeout=5", NULL};
            (void)pcv_spawn_sync(udev_argv, NULL, NULL, NULL);
            const gchar *conv_argv[] = {
                "qemu-img", "convert", "-f", "qcow2", "-O", "raw",
                data->base_image, disk_path, NULL
            };
            gchar *conv_err = NULL;
            gboolean conv_ok = pcv_spawn_sync(conv_argv, NULL, &conv_err, NULL);
            if (conv_ok) {
                PCV_LOG_INFO("vm_manager", "VM '%s': base image '%s' written to zvol",
                             data->name, data->base_image);
            } else {
                PCV_LOG_WARN("vm_manager", "VM '%s': base image write failed: %s",
                             data->name, conv_err ? conv_err : "unknown");
            }
            g_free(conv_err);
        }
    } else {
                                             
        const gchar *fmt = use_file_raw ? "raw" : "qcow2";
        const gchar *ext = use_file_raw ? "img" : "qcow2";
                            
        if (!g_file_test(image_dir, G_FILE_TEST_IS_DIR)) {
            const gchar *mkdir_argv[] = {"mkdir", "-p", image_dir, NULL};
            (void)pcv_spawn_sync(mkdir_argv, NULL, NULL, NULL);
        }

        disk_path = g_strdup_printf("%s/%s.%s", image_dir, data->name, ext);

                         
        if (g_file_test(disk_path, G_FILE_TEST_EXISTS)) {
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_EXISTS,
                "Disk image '%s' already exists — delete the VM first",
                disk_path);
            g_free(disk_path);
            return;
        }

        gchar *size_str = g_strdup_printf("%dG", final_disk_size);
        const gchar *qimg_argv[] = {"qemu-img", "create", "-f", fmt,
                                     disk_path, size_str, NULL};
        gchar *std_err = nullptr;

        if (!pcv_spawn_sync(qimg_argv, NULL, &std_err, &error)) {
            gchar *err_msg = error ? error->message
                                   : (std_err ? g_strstrip(std_err)
                                              : "Unknown qemu-img error");
            PCV_LOG_WARN("vm_manager", "VM '%s' qemu-img FAILED: %s (stderr=%s)",
                         data->name, err_msg, std_err ? std_err : "(none)");
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                    "%s Provisioning Failed: %s", fmt, err_msg);
            g_unlink(disk_path);
            if (error) g_error_free(error);
            g_free(std_err); g_free(size_str); g_free(disk_path);
            return;
        }
        g_free(std_err); g_free(size_str);
        PCV_LOG_INFO("vm_manager", "VM '%s': %s disk created at %s (%dG)",
                      data->name, fmt, disk_path, final_disk_size);
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
                                                                  
                                                  
                                                                         
            iface_xml = g_strdup_printf(
                "    <interface type='vhostuser'>\n"
                "      <source type='unix' path='/var/run/purecvisor/vhost-%s.sock' mode='server'/>\n"
                "      <model type='virtio'/>\n"
                "      <driver queues='2'/>\n"
                "    </interface>\n",
                data->name);
        } else if (g_strcmp0(nic_type, "sriov") == 0 && data->pci_addr) {
                                                                      
                                                               
                                                                                  
                                                                         
                                                                       
            guint dom = 0, bus = 0, slot = 0, func = 0;
            sscanf(data->pci_addr, "%x:%x:%x.%x", &dom, &bus, &slot, &func);
            iface_xml = g_strdup_printf(
                "    <hostdev mode='subsystem' type='pci' managed='yes'>\n"
                "      <source>\n"
                "        <address domain='0x%04x' bus='0x%02x' slot='0x%02x' function='0x%x'/>\n"
                "      </source>\n"
                "    </hostdev>\n",
                dom, bus, slot, func);
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
        gchar *owned_xml = _vm_xml_with_owner_metadata(final_xml, data->owner);
        g_free(final_xml);
        final_xml = owned_xml;
    }

                                                                   
                                                                   
                                                                   
                                                        
    {
        gchar *overlay_meta_xml = _overlay_metadata_xml(data->network_mode, data->tenant);
        if (overlay_meta_xml && *overlay_meta_xml) {
            gchar *merged_xml = _vm_xml_inject_metadata_child(final_xml, overlay_meta_xml);
            g_free(final_xml);
            final_xml = merged_xml;
        }
        g_free(overlay_meta_xml);
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
                                     
        if (disk_path && *disk_path) {
            if (g_str_has_prefix(disk_path, "/dev/zvol/")) {
                gchar *zvol_name = g_strdup(disk_path + strlen("/dev/zvol/"));
                const gchar *zfs_argv[] = {"zfs", "destroy", "-f", zvol_name, NULL};
                (void)pcv_spawn_sync(zfs_argv, NULL, NULL, NULL);
                g_free(zvol_name);
            } else if (g_file_test(disk_path, G_FILE_TEST_EXISTS)) {
                g_unlink(disk_path);
            }
        }
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

                                               
        if (disk_path && *disk_path) {
            if (g_str_has_prefix(disk_path, "/dev/zvol/")) {
                                 
                gchar *zvol_name = g_strdup(disk_path + strlen("/dev/zvol/"));
                const gchar *zfs_argv[] = {"zfs", "destroy", "-f", zvol_name, NULL};
                (void)pcv_spawn_sync(zfs_argv, NULL, NULL, NULL);
                g_free(zvol_name);
                PCV_LOG_WARN("vm_manager", "Rolled back zvol for failed VM define: %s", disk_path);
            } else {
                                     
                if (g_file_test(disk_path, G_FILE_TEST_EXISTS)) {
                    g_unlink(disk_path);
                    PCV_LOG_WARN("vm_manager", "Rolled back disk file for failed VM define: %s", disk_path);
                }
            }
        }

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
        g_free(stored);
        if (dom) {
                                                                         
                                                                
                                                                 
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
                                           const gchar *base_image,                                 
                                           const gchar *owner,
                                           const gchar *network_mode,                                                                
                                           const gchar *tenant,                                                                 
                                           gboolean     qos_required,                                                        
                                           const PcvQosSla *qos_sla,                                                        
                                           GCancellable *cancellable,
                                           GAsyncReadyCallback callback,
                                           gpointer user_data) {
                                                                    
      
                   
                                                       
                                                     
                                             
                                             
                                     
      
               
                                                     
                                      
      
                           
                                                        
                                                              
      
                             
                                 
                                                           
      
                             
                                             
                               
      
                             
                               
                                                   
                                                
                                                                              
                                                                                                  
    GTask *task = g_task_new(self, cancellable, callback, user_data);
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

   
                                        
                                    
                                                                               
                             
  
                                                   
                                                       
   
gchar *
purecvisor_vm_resolve_network_bridge(const gchar *requested)
{
    if (!requested || !*requested)
        return g_strdup(pcv_config_get_string("network", "default_bridge", "pcvnat0"));
    if (g_strcmp0(requested, "none") == 0)
        return NULL;
    return g_strdup(requested);
}


                                                                             
                               
                                                     
                                                                                

   
                     
                                              
   
typedef struct {
    PureCVisorVmManager *manager;
    gchar *name;
} LifecycleTaskData;

                                                         
                                                        
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
    GTask *task = g_task_new(self, NULL, callback, user_data);
    LifecycleTaskData *data = g_new0(LifecycleTaskData, 1);
    data->manager = g_object_ref(self);
    data->name = g_strdup(name);

    g_task_set_task_data(task, data, (GDestroyNotify)lifecycle_task_data_free);
    g_task_run_in_thread(task, start_vm_thread_impl);
    g_object_unref(task);
}

   
                                         
                                   
                                          
  
                                                            
                                                        
  
                              
   
gboolean purecvisor_vm_manager_start_vm_finish(PureCVisorVmManager *manager,
                                               GAsyncResult *res,
                                               GError **error) {
    gboolean ok = g_task_propagate_boolean(G_TASK(res), error);
    if (ok) {
                                      
                                                       
                                                           
                                    
        LifecycleTaskData *data = g_task_get_task_data(G_TASK(res));
        g_signal_emit(manager, signals[SIGNAL_VM_STARTED], 0, data->name);
    }
    return ok;
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
    GTask *task = g_task_new(self, NULL, callback, user_data);
    LifecycleTaskData *data = g_new0(LifecycleTaskData, 1);
    data->manager = g_object_ref(self);
    data->name = g_strdup(name);

    g_task_set_task_data(task, data, (GDestroyNotify)lifecycle_task_data_free);
    g_task_run_in_thread(task, stop_vm_thread_impl);
    g_object_unref(task);
}

   
                                        
                                   
  
                                                  
                                                        
  
                              
   
gboolean purecvisor_vm_manager_stop_vm_finish(PureCVisorVmManager *manager,
                                              GAsyncResult *res,
                                              GError **error) {
    gboolean ok = g_task_propagate_boolean(G_TASK(res), error);
    if (ok) {
                                        
        LifecycleTaskData *data = g_task_get_task_data(G_TASK(res));
        g_signal_emit(manager, signals[SIGNAL_VM_STOPPED], 0, data->name);
    }
    return ok;
}

                                                                             
                              
  
               
                                                                       
                                              
                                               
                                    
  
                       
                                                            
                                           
                                                                                
                                                                   
                                                              
                                                  
                                                                              
                                 
typedef struct {
    gchar *vm_name;                                
} ZfsDestroyData;

                                                           
                                                       
static void _zfs_destroy_data_free(gpointer p) {
    ZfsDestroyData *d = p;
    if (!d) return;
    g_free(d->vm_name);
    g_free(d);
}

                        
                                                          
constexpr int ZFS_RETRY_MAX = 5;
static const guint ZFS_RETRY_MS[ZFS_RETRY_MAX] = {500, 1000, 2000, 4000, 8000};

                                                 
                                              
                                                    
  
                                                 
                    
                                                                  
            
                                             
                                         
                                                     
                                                               
static GHashTable *g_delete_status = nullptr;                                     
static GMutex      g_delete_status_mu;

                                
                                                             
                                                                        
static void _delete_status_set(const gchar *vm, const gchar *status) {
    g_mutex_lock(&g_delete_status_mu);
    if (!g_delete_status)
        g_delete_status = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_hash_table_replace(g_delete_status, g_strdup(vm), g_strdup(status));
    g_mutex_unlock(&g_delete_status_mu);
}

   
                            
                          
                                    
  
                                                        
  
         
                                         
                                                            
                       
                                               
                                 
                               
  
                  
                                                  
   
const gchar *pcv_vm_delete_status_get(const gchar *vm) {
    if (!vm) return "unknown";
    g_mutex_lock(&g_delete_status_mu);
    const gchar *st = g_delete_status
        ? g_hash_table_lookup(g_delete_status, vm) : NULL;
    g_mutex_unlock(&g_delete_status_mu);
    return st ? st : "not_found";
}

                                         
                                                      
void pcv_vm_manager_cleanup(void) {
    g_mutex_lock(&g_delete_status_mu);
    if (g_delete_status) {
        g_hash_table_destroy(g_delete_status);
        g_delete_status = nullptr;
    }
    g_mutex_unlock(&g_delete_status_mu);
}

   
                                                                     
  
                                                        
                                                        
                                                         
                                   
  
                                                                               
                                    
                                       
                                                         
                                                          
                                        
                                                         
                                                                  
                                                         
                                                    
                                     
   
static void
_zfs_destroy_thread(GTask    *task __attribute__((unused)),
                    gpointer  source_object __attribute__((unused)),
                    gpointer  task_data,
                    GCancellable *cancellable __attribute__((unused)))
{
    ZfsDestroyData *d = (ZfsDestroyData *)task_data;
    GError *err = nullptr;
    _delete_status_set(d->vm_name, "deleting");

                                           
    gchar *qcow2_path = g_strdup_printf("%s/%s.qcow2",
                                         pcv_config_get_image_dir(), d->vm_name);
    if (g_file_test(qcow2_path, G_FILE_TEST_EXISTS)) {
        if (g_unlink(qcow2_path) == 0) {
            PCV_LOG_INFO("vm_manager",
                         "qcow2 disk removed: %s", qcow2_path);
        } else {
            PCV_LOG_WARN("vm_manager",
                         "qcow2 disk remove failed: %s", qcow2_path);
        }
        g_free(qcow2_path);
                                                                      
    } else {
        g_free(qcow2_path);
    }

                               
    gchar *zfs_dataset = g_strdup_printf("%s/%s", pcv_config_get_zvol_pool(), d->vm_name);
    const gchar *check_argv[] = {"zfs", "list", "-H", "-o", "name",
                                 zfs_dataset, NULL};
    gboolean exists = pcv_spawn_sync(check_argv, NULL, NULL, NULL);

    if (!exists) {
        PCV_LOG_WARN("vm_manager",
                     "ZFS dataset '%s' not found — skipped", zfs_dataset);
        g_free(zfs_dataset);
        goto cleanup;
    }

    PCV_LOG_INFO("vm_manager", "ZFS destroy (bg): %s", zfs_dataset);

                                                                  
                                                       
                                                   
                                                         
                                                                         
    for (guint attempt = 0; attempt < ZFS_RETRY_MAX; attempt++) {

        if (attempt > 0) {
            PCV_LOG_INFO("vm_manager",
                         "ZFS destroy retry %u/%u for '%s' (wait %ums)",
                         attempt, ZFS_RETRY_MAX - 1,
                         zfs_dataset, ZFS_RETRY_MS[attempt - 1]);
            g_usleep((gulong)ZFS_RETRY_MS[attempt - 1] * 1000UL);
        }

        g_clear_error(&err);
        gboolean ok = purecvisor_zfs_destroy_volume(pcv_config_get_zvol_pool(),
                                                     d->vm_name, &err);
        if (ok) {
            PCV_LOG_INFO("vm_manager",
                         "ZFS dataset removed: %s (attempt %u)",
                         zfs_dataset, attempt + 1);
            goto zfs_cleanup;
        }

                                                             
                                                             
        gboolean is_busy = err &&
            (strstr(err->message, "dataset is busy") != nullptr ||
             strstr(err->message, "busy")             != nullptr ||
             strstr(err->message, "EBUSY")            != nullptr);

        if (!is_busy) {
            PCV_LOG_WARN("vm_manager",
                         "ZFS destroy failed (non-retryable) for %s: %s",
                         zfs_dataset, err ? err->message : "unknown");
            goto zfs_cleanup;
        }

        PCV_LOG_WARN("vm_manager",
                     "ZFS destroy: device busy for '%s', will retry",
                     zfs_dataset);
    }

                   
    PCV_LOG_WARN("vm_manager",
                 "ZFS destroy gave up after %u attempts for '%s': %s",
                 ZFS_RETRY_MAX, zfs_dataset,
                 err ? err->message : "unknown");

zfs_cleanup:
    g_free(zfs_dataset);
cleanup:
                    
    _delete_status_set(d->vm_name, err ? "failed" : "done");
                          
#if PCV_CLUSTER_ENABLED
    pcv_cluster_remove_vm_xml(d->vm_name);
#endif
    if (err) g_error_free(err);
                                                                        
}

   
                                                       
  
                                                      
                                                  
                                                        
                                             
  
                                                                               
                                                                       
                                                          
                                                                    
                                                
                                             
                                                      
                                                 
                                                     
                                                               
                                              
   
static void delete_vm_thread_impl(GTask *task,
                                  gpointer source_object __attribute__((unused)),
                                  gpointer task_data,
                                  GCancellable *cancellable __attribute__((unused))) {
    LifecycleTaskData *data = (LifecycleTaskData *)task_data;
    GError *err __attribute__((unused)) = nullptr;

                                                                     
                                                                
                                                        
                                                      
    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Failed to acquire libvirt connection");
        return;
    }

    virDomainPtr dom = virDomainLookupByName(conn, data->name);
    if (dom) {
                                                                  
                                           
                                                             
                                                           
                                                            
                                                                      
        virDomainState state = VIR_DOMAIN_NOSTATE;
        int reason = 0;
        virDomainGetState(dom, (int *)&state, &reason, 0);
        if (state == VIR_DOMAIN_RUNNING || state == VIR_DOMAIN_PAUSED) {
            virDomainDestroy(dom);                             
        }

                                                                    
                                                             
                                                               
                                                             
                                                          
                                                                  
        int rc = virDomainUndefineFlags(dom, VIR_DOMAIN_UNDEFINE_NVRAM |
                                             VIR_DOMAIN_UNDEFINE_SNAPSHOTS_METADATA);
        if (rc != 0) {
                                                          
            rc = virDomainUndefine(dom);
        }
        virDomainFree(dom);

        if (rc != 0) {
            virErrorPtr e = virGetLastError();
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                    "virDomainUndefine failed: %s",
                                    e ? e->message : "unknown error");
            virt_conn_pool_release(conn);
            return;
        }
    }
                                                             

    virt_conn_pool_release(conn);

                                                                     
                                                         
                                                 
                                                   
                                                                         
      
              
                                      
                                                                   
                                              
      
                   
                                                          
                                                          
                                                      
      
                      
                                                     
                                               
                                                
      
                                 
                                                                 
                                         
                                                                              
    _delete_status_set(data->name, "pending");
    ZfsDestroyData *zd = g_new0(ZfsDestroyData, 1);
    zd->vm_name = g_strdup(data->name);

    GTask *zfs_task = g_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(zfs_task, zd, _zfs_destroy_data_free);
    g_task_run_in_thread(zfs_task, _zfs_destroy_thread);
    g_object_unref(zfs_task);

    g_task_return_boolean(task, TRUE);
}
   
                                         
                                                     
                                                                     
  
                                                
   
void purecvisor_vm_manager_delete_vm_async(PureCVisorVmManager *self,
                                           const gchar *name,
                                           GAsyncReadyCallback callback,
                                           gpointer user_data) {
    GTask *task = g_task_new(self, NULL, callback, user_data);
    LifecycleTaskData *data = g_new0(LifecycleTaskData, 1);
    data->manager = g_object_ref(self);
    data->name = g_strdup(name);

    g_task_set_task_data(task, data, (GDestroyNotify)lifecycle_task_data_free);
    g_task_run_in_thread(task, delete_vm_thread_impl);
    g_object_unref(task);
}

                                                               
                                                                      
gboolean purecvisor_vm_manager_delete_vm_finish(PureCVisorVmManager *manager __attribute__((unused)),
                                                GAsyncResult *res,
                                                GError **error) {
    return g_task_propagate_boolean(G_TASK(res), error);
}

                                                                             
                                
  
                                                             
                                                        
                               
  
                           
                                          
                                                                                

   
                                                     
  
                                                       
                                           
  
                                                                              
                                                           
                                          
                                                              
                                
                                                            
                                                           
                                                           
                                                                 
   
static void list_vms_thread(GTask *task,
                            gpointer source_object __attribute__((unused)), 
                            gpointer task_data, 
                            GCancellable *cancellable __attribute__((unused))) {
    PureCVisorVmManager *self = PURECVISOR_VM_MANAGER(task_data);
    GList *domains, *l;
    JsonBuilder *builder = json_builder_new();
    GError *err = nullptr;

    json_builder_begin_array(builder);

                                  
                                        
    if (!gvir_connection_fetch_domains(self->conn, NULL, &err)) {
        if (err) g_error_free(err);
    }

                              
    domains = gvir_connection_get_domains(self->conn);

                            
      
                 
                                                          
                                                   
    for (l = domains; l != nullptr; l = l->next) {
        GVirDomain *dom = GVIR_DOMAIN(l->data);
        const gchar *name = gvir_domain_get_name(dom);
        const gchar *uuid = gvir_domain_get_uuid(dom);

                                                      
                                     
                                            
                                                                
        gint dom_id = gvir_domain_get_id(dom, NULL);

        const gchar *state_str = "shutoff";
        gboolean is_active = FALSE;

        if (dom_id > 0) {
            state_str = "running";
            is_active = TRUE;
        }

                                                       
        gint vnc_port = -1;
        if (is_active) {
            vnc_port = _extract_vnc_port_from_domain(dom);
        }

                        
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "name");
        json_builder_add_string_value(builder, name);
        
        json_builder_set_member_name(builder, "uuid");
        json_builder_add_string_value(builder, uuid);
        
        json_builder_set_member_name(builder, "state");
        json_builder_add_string_value(builder, state_str);
        
        json_builder_set_member_name(builder, "vnc_port");
        if (vnc_port > 0) {
            json_builder_add_int_value(builder, vnc_port);
        } else {
            json_builder_add_null_value(builder);
        }
        json_builder_end_object(builder);
    }

                                      
                                                        
                                                 
                                              
    g_list_free_full(domains, (GDestroyNotify)g_object_unref);
    json_builder_end_array(builder);

                                       
      
                                       
                                                 
                                                             
                                                                
    JsonNode *root = json_builder_get_root(builder);
    g_object_unref(builder);

    g_task_return_pointer(task, root, (GDestroyNotify)json_node_free);
}

   
                                        
                                                 
  
                                              
   
void purecvisor_vm_manager_list_vms_async(PureCVisorVmManager *self,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data) {
    GTask *task = g_task_new(self, NULL, callback, user_data);
    
                                                                 
                              
    g_task_set_task_data(task, g_object_ref(self), (GDestroyNotify)g_object_unref);
    
    g_task_run_in_thread(task, list_vms_thread);
    g_object_unref(task);
}

   
                                         
                  
  
                                                      
  
                                                        
   
JsonNode *purecvisor_vm_manager_list_vms_finish(PureCVisorVmManager *manager __attribute__((unused)),
                                                GAsyncResult *res,
                                                GError **error) {
    return g_task_propagate_pointer(G_TASK(res), error);
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
    GTask *task = g_task_new(self, cancellable, callback, user_data);
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
    GTask *task = g_task_new(self, cancellable, callback, user_data);
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
    gboolean holds_lock;                                             
} ResizeDiskData;

                                                                 
                                                                  
static void resize_disk_data_free(ResizeDiskData *d) {
                                                                 
                                                           
                                                                         
    if (d->holds_lock) unlock_vm_operation(d->name);
    g_free(d->name);
    g_free(d->target);
    g_free(d);
}

   
                                                                 
                                                    
                          
                                                              
   
static void
audit_resize_disk_success(ResizeDiskData *d)
{
    gchar *target = g_strdup_printf("%s:%s", d->name, d->target ? d->target : "vda");
    gchar *job_id = g_strdup_printf("vm.resize_disk:%s", target);
    pcv_audit_log(NULL, "vm.resize_disk", target, "ok", 0, 0, "local");
    pcv_ws_broadcast_job_complete_mt(job_id, "vm.resize_disk",
                                     "completed", NULL);
    g_free(job_id);
    g_free(target);
}

   
                                                                 
                                                    
                          
                                                                       
                                                      
   
static void
audit_resize_disk_failure(ResizeDiskData *d, const gchar *error_msg)
{
    gchar *target = g_strdup_printf("%s:%s", d->name, d->target ? d->target : "vda");
    gchar *job_id = g_strdup_printf("vm.resize_disk:%s", target);
    pcv_audit_log(NULL, "vm.resize_disk", target, "fail", PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
    pcv_ws_broadcast_job_complete_mt(job_id, "vm.resize_disk",
                                     "failed", error_msg ? error_msg : "unknown");
    g_free(job_id);
    g_free(target);
}

   
                      
                                  
  
                                                
                                                      
                                                    
                      
  
      
                    
                              
                                                                   
                                                         
   
static void resize_disk_thread(GTask *task, gpointer source_object __attribute__((unused)),
                                gpointer task_data, GCancellable *cancel __attribute__((unused)))
{
    ResizeDiskData *d = (ResizeDiskData *)task_data;
    GError *error = nullptr;

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        audit_resize_disk_failure(d, "Failed to acquire libvirt connection");
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
            "Failed to acquire libvirt connection");
        return;
    }

    virDomainPtr dom = virDomainLookupByName(conn, d->name);
    if (!dom) {
        gchar *msg = g_strdup_printf("VM '%s' not found", d->name);
        audit_resize_disk_failure(d, msg);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
            "VM '%s' not found", d->name);
        virt_conn_pool_release(conn);
        g_free(msg);
        return;
    }

                                                                
      
                        
                                                            
                                                            
      
                  
                                                                    
                                                                        
      
                   
                                                       
                                          
                                                       
    gchar *xml = virDomainGetXMLDesc(dom, 0);
    gboolean is_zvol = FALSE;
    gchar *disk_source = nullptr;

    if (xml) {
                                                      
        gchar *dev_tag = strstr(xml, "<source dev='");
        if (dev_tag) {
            dev_tag += strlen("<source dev='");
            gchar *end = strchr(dev_tag, '\'');
            if (end) {                                 
                disk_source = g_strndup(dev_tag, (gsize)(end - dev_tag));
                                                             
                is_zvol = g_str_has_prefix(disk_source, "/dev/zvol/") ||
                          g_str_has_prefix(disk_source, "/dev/zd");
            }
        }
                                                         
        if (!disk_source) {
            gchar *file_tag = strstr(xml, "<source file='");
            if (file_tag) {
                file_tag += strlen("<source file='");
                gchar *end = strchr(file_tag, '\'');
                if (end) {
                    disk_source = g_strndup(file_tag, (gsize)(end - file_tag));
                    is_zvol = FALSE;
                }
            }
        }
        g_free(xml);
    }

    if (!disk_source) {
        gchar *msg = g_strdup_printf("Cannot determine disk source for VM '%s'", d->name);
        audit_resize_disk_failure(d, msg);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
            "Cannot determine disk source for VM '%s'", d->name);
        g_free(msg);
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        return;
    }

                      
    if (is_zvol) {
                                                                      
        const gchar *dataset = disk_source + strlen("/dev/zvol/");
        gchar *size_str = g_strdup_printf("%dG", d->new_size_gb);
        gchar *prop_str = g_strdup_printf("volsize=%s", size_str);
        const gchar *argv[] = {"zfs", "set", prop_str, (gchar *)dataset, NULL};
        gchar *std_err = nullptr;

        if (!pcv_spawn_sync(argv, NULL, &std_err, &error)) {
            const gchar *err_msg = error ? error->message : (std_err ? std_err : "unknown");
            gchar *msg = g_strdup_printf("zfs set volsize failed: %s", err_msg);
            audit_resize_disk_failure(d, msg);
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
                                    
        gchar *size_str = g_strdup_printf("%dG", d->new_size_gb);
        const gchar *argv[] = {"qemu-img", "resize", disk_source, size_str, NULL};
        gchar *std_err = nullptr;

        if (!pcv_spawn_sync(argv, NULL, &std_err, &error)) {
            const gchar *err_msg = error ? error->message : (std_err ? std_err : "unknown");
            gchar *msg = g_strdup_printf("qemu-img resize failed: %s", err_msg);
            audit_resize_disk_failure(d, msg);
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                "qemu-img resize failed: %s",
                err_msg);
            if (error) g_error_free(error);
            g_free(std_err);
            g_free(size_str);
            g_free(disk_source);
            virDomainFree(dom);
            virt_conn_pool_release(conn);
            g_free(msg);
            return;
        }
        g_free(std_err);
        g_free(size_str);
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
    audit_resize_disk_success(d);
    g_task_return_boolean(task, TRUE);
}

   
                             
                                        
                        
  
                                                 
                                            
                                                          
   
void purecvisor_vm_resize_disk(const gchar *name, gint new_size_gb, const gchar *target,
                                gboolean holds_lock) {
    ResizeDiskData *d = g_new0(ResizeDiskData, 1);
    d->name = g_strdup(name);
    d->new_size_gb = new_size_gb;
    d->target = target ? g_strdup(target) : g_strdup("vda");
    d->holds_lock = holds_lock;                                            

    GTask *task = g_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(task, d, (GDestroyNotify)resize_disk_data_free);
    g_task_run_in_thread(task, resize_disk_thread);
    g_object_unref(task);
}

                                                                             
                                                   
  
                                                   
                                                    
                                                                                
   
                                              
                                         
  
                                                    
                                                       
  
            
                                                          
                                   
                                                
  
                         
                            
                                                         
                                                
  
                                                            
   
void
purecvisor_vm_manager_emit_metrics_updated(PureCVisorVmManager *self,
                                           GHashTable          *cache)
{
    g_return_if_fail(PURECVISOR_IS_VM_MANAGER(self));
    g_signal_emit(self, signals[SIGNAL_VM_METRICS_UPDATED], 0, cache);
}
