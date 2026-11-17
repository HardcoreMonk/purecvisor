   
                          
                                                                        
  
                           
                                                                    
                                                                       
                                  
                                                                           
                                                                                         
                                                                             
                                                            
                                                                        
                                                                              
                                                                       
                              
  
                       
                                                              
                                                         
                                                   
  
                                                                 
                                                                    
                                     
                                                              
                                                              
                                               
   
#include "vm_diag_sampler.h"

#include <json-glib/json-glib.h>
#include <libvirt/libvirt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cgroup_psi.h"
#include "modules/dispatcher/host_hardware_inventory.h"
#include "modules/resource/resource_domain_refs.h"
#include "modules/virt/pcv_virt_open.h"
#include "prometheus_exporter.h"
#include "utils/pcv_log.h"
#include "vm_diag.h"
#include "vm_diag_view.h"

#define VM_DIAG_LOG_DOM "vm_diag"

                                                        
#define VM_DIAG_LOG_INTERVAL_SEC 300

                             
typedef struct {
    gchar   *owner;                                     
    gint     domain_id;                                 
    guint    refreshed_cycle;                            
    gint     epoch;                                                             
    gboolean ok;                                                     
} VmOwnerEntry;

                                              
static struct {
    PcvVmDiagTable  *table;                             
    GHashTable      *owners;                                    
    GPtrArray       *configs;                                                
    GPtrArray       *running;                                              
    guint            cycle;                           
    PcvVmDiagError   error;                         
    gint64           obs_mono_us;                        
    gint64           obs_wall_ms;                    
    gboolean         have_psi[PCV_CGROUP_PSI_RES_COUNT][2];
    gdouble          psi[PCV_CGROUP_PSI_RES_COUNT][2];
    gboolean         cores_tried;                           
    guint            physical_cores;              
    PcvVmDiagError   last_err_logged;
    gint64           last_err_log_us;
    gint64           last_trunc_log_us;
} S;

static void
_owner_entry_free(gpointer p)
{
    VmOwnerEntry *e = p;
    if (!e) return;
    g_free(e->owner);
    g_free(e);
}

                                                                        

                                                               
static guint64
_mem_total_kb(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return 0;
    char line[128];
    guint64 kb = 0;
    while (fgets(line, sizeof(line), f)) {
        unsigned long long v = 0;
        if (sscanf(line, "MemTotal: %llu kB", &v) == 1) { kb = v; break; }
    }
    fclose(f);
    return kb;
}

                                                                        
                                                         
                                           
static guint
_physical_cores_once(void)
{
    if (S.cores_tried) return S.physical_cores;
    S.cores_tried = TRUE;
    JsonObject *inv = pcv_host_hardware_inventory_collect("/");
    if (!inv) return 0;
    if (json_object_has_member(inv, "cpu_topology")) {
        JsonNode *n = json_object_get_member(inv, "cpu_topology");
        if (JSON_NODE_HOLDS_OBJECT(n)) {
            JsonObject *cpu = json_node_get_object(n);
            if (json_object_has_member(cpu, "physical_cores")) {
                gint64 v = json_object_get_int_member(cpu, "physical_cores");
                if (v > 0 && v <= G_MAXUINT) S.physical_cores = (guint)v;
            }
        }
    }
    json_object_unref(inv);
    return S.physical_cores;
}

                                        
static void
_fill_host(PcvVmDiagHostIn *h)
{
    memset(h, 0, sizeof(*h));
    h->observed_mono_us = S.obs_mono_us;
    h->observed_wall_ms = S.obs_wall_ms;
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);                             
    h->logical_cpus = ncpu > 0 ? (guint)ncpu : 0;
    h->physical_cores = _physical_cores_once();
    h->mem_total_kb = _mem_total_kb();
    memcpy(h->have_psi, S.have_psi, sizeof(h->have_psi));
    memcpy(h->psi, S.psi, sizeof(h->psi));
}

                                                               
                                            
void
pcv_vm_diag_sampler_note_host_psi(const gchar *resource, gboolean have_some,
                                  gdouble some_avg10, gboolean have_full, gdouble full_avg10)
{
    gint r;
    if (g_strcmp0(resource, "cpu") == 0)         r = PCV_CGROUP_PSI_RES_CPU;
    else if (g_strcmp0(resource, "memory") == 0) r = PCV_CGROUP_PSI_RES_MEMORY;
    else if (g_strcmp0(resource, "io") == 0)     r = PCV_CGROUP_PSI_RES_IO;
    else return;
    if (have_some) { S.have_psi[r][0] = TRUE; S.psi[r][0] = some_avg10; }
    if (have_full) { S.have_psi[r][1] = TRUE; S.psi[r][1] = full_avg10; }
}

                                                                        

                                                            
static gint
_domain_id(virDomainPtr dom)
{
    unsigned int id = virDomainGetID(dom);
    return (id == (unsigned int)-1 || id > (unsigned int)G_MAXINT) ? -1 : (gint)id;
}

                                                                                                      
static gint g_owner_epoch;

                                                                                          
void
pcv_vm_diag_sampler_invalidate_owners(void)
{
    g_atomic_int_inc(&g_owner_epoch);
}

                                                       
                                                           
static const gchar *
_owner_for(virDomainPtr dom, const gchar *uuid, gint domain_id)
{
    const gint epoch = g_atomic_int_get(&g_owner_epoch);
    VmOwnerEntry *e = g_hash_table_lookup(S.owners, uuid);
    gboolean stale = !e || !e->ok || e->domain_id != domain_id ||
        S.cycle - e->refreshed_cycle >= PCV_VM_DIAG_OWNER_REFRESH_CYCLES || e->epoch != epoch;
    if (!stale) return e->owner;

    if (!e) {
        e = g_new0(VmOwnerEntry, 1);
        g_hash_table_insert(S.owners, g_strdup(uuid), e);
    }
    char *xml = virDomainGetXMLDesc(dom, VIR_DOMAIN_XML_INACTIVE);
    if (!xml) {
        e->ok = FALSE;                                  
        return e->owner;
    }
    PcvResDomainRefs refs = { 0 };
    gboolean parsed = pcv_res_domain_refs_parse(xml, &refs, NULL);
    free(xml);
    g_free(e->owner);
    e->owner = parsed ? g_strdup(refs.owner) : NULL;                             
    pcv_res_domain_refs_clear(&refs);
    e->ok = parsed;
    e->domain_id = domain_id;
    e->refreshed_cycle = S.cycle;
    e->epoch = epoch;
    return e->owner;
}

                                       
static gboolean
_owner_unseen(gpointer key, gpointer value, gpointer ud)
{
    (void)value;
    return !g_hash_table_contains((GHashTable *)ud, key);
}

                                                
                                                      
void
pcv_vm_diag_sampler_collect(void)
{
    if (!S.table) S.table = pcv_vm_diag_table_new();
    if (!S.owners) S.owners = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, _owner_entry_free);
    S.cycle++;
    S.error = PCV_VM_DIAG_E_NONE;
    memset(S.have_psi, 0, sizeof(S.have_psi));                                            
    if (S.running) { g_ptr_array_unref(S.running); S.running = NULL; }

    virConnectPtr conn = pcv_virt_open_bounded("qemu:///system", FALSE, PCV_VIRT_OPEN_TIMEOUT_SEC, NULL);
    if (!conn) { S.error = PCV_VM_DIAG_E_CONNECT_FAILED; return; }

                                                                
                               
    virDomainStatsRecordPtr *recs = NULL;
    gint n = virConnectGetAllDomainStats(conn,
        VIR_DOMAIN_STATS_STATE | VIR_DOMAIN_STATS_CPU_TOTAL | VIR_DOMAIN_STATS_VCPU |
        VIR_DOMAIN_STATS_BALLOON | VIR_DOMAIN_STATS_BLOCK | VIR_DOMAIN_STATS_INTERFACE,
        &recs, VIR_CONNECT_GET_ALL_DOMAINS_STATS_RUNNING | VIR_CONNECT_GET_ALL_DOMAINS_STATS_NOWAIT);
    S.obs_mono_us = g_get_monotonic_time();
    S.obs_wall_ms = g_get_real_time() / 1000;
    if (n < 0) {
        S.error = PCV_VM_DIAG_E_RUNNING_STATS_FAILED;
        virConnectClose(conn);
        return;
    }
    S.running = g_ptr_array_new_with_free_func(pcv_vm_diag_sample_free);
    for (gint i = 0; i < n; i++) {
        const gchar *name = virDomainGetName(recs[i]->dom);
        char uuid[VIR_UUID_STRING_BUFLEN];
        if (!name || virDomainGetUUIDString(recs[i]->dom, uuid) < 0) continue;
        PcvVmDiagSample *s = g_new0(PcvVmDiagSample, 1);
        if (!pcv_vm_diag_parse(recs[i]->params, recs[i]->nparams, name, uuid,
                               _domain_id(recs[i]->dom), TRUE, TRUE, s)) {
            pcv_vm_diag_sample_free(s);
            continue;
        }
        s->mono_us = S.obs_mono_us;
        s->wall_ms = S.obs_wall_ms;
        g_ptr_array_add(S.running, s);
    }
    virDomainStatsRecordListFree(recs);

                                                                 
                                                                 
    recs = NULL;
    gint m = virConnectGetAllDomainStats(conn,
        VIR_DOMAIN_STATS_STATE | VIR_DOMAIN_STATS_VCPU | VIR_DOMAIN_STATS_BALLOON,
        &recs, VIR_CONNECT_GET_ALL_DOMAINS_STATS_NOWAIT);
    if (m < 0) {
        S.error = PCV_VM_DIAG_E_DEFINED_STATS_FAILED;                     
    } else {
        GPtrArray *cfgs = g_ptr_array_new_with_free_func(pcv_vm_diag_config_free);
        g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        for (gint i = 0; i < m; i++) {
            const gchar *name = virDomainGetName(recs[i]->dom);
            char uuid[VIR_UUID_STRING_BUFLEN];
            if (!name || virDomainGetUUIDString(recs[i]->dom, uuid) < 0) continue;
            gint id = _domain_id(recs[i]->dom);
            PcvVmDiagConfig *c = g_new0(PcvVmDiagConfig, 1);
            if (!pcv_vm_diag_parse_config(recs[i]->params, recs[i]->nparams, name, uuid, id, c)) {
                pcv_vm_diag_config_free(c);
                continue;
            }
            c->owner = g_strdup(_owner_for(recs[i]->dom, uuid, id));
            g_hash_table_add(seen, g_strdup(uuid));
            g_ptr_array_add(cfgs, c);
        }
        g_hash_table_foreach_remove(S.owners, _owner_unseen, seen);
        if (S.configs) g_ptr_array_unref(S.configs);
        S.configs = cfgs;
    }
    if (recs) virDomainStatsRecordListFree(recs);
    virConnectClose(conn);
}

                                                                        

                                                 
                                             
static void
_log_error(PcvVmDiagError code)
{
    if (code == PCV_VM_DIAG_E_NONE) { S.last_err_logged = PCV_VM_DIAG_E_NONE; return; }
    gint64 now = g_get_monotonic_time();
    if (code == S.last_err_logged &&
        now - S.last_err_log_us < (gint64)VM_DIAG_LOG_INTERVAL_SEC * G_USEC_PER_SEC)
        return;
    S.last_err_logged = code;
    S.last_err_log_us = now;
    PCV_LOG_WARN(VM_DIAG_LOG_DOM, "[vm-diag] sampler error code=%s (repeats suppressed 300s)",
                 pcv_vm_diag_error_name(code));
}

                                                       
                                     
                                                    
static void
_publish_error(PcvVmDiagError code)
{
    PcvVmDiagSnapshot *snap = pcv_vm_diag_store_copy();
    if (!snap) {
        snap = g_new0(PcvVmDiagSnapshot, 1);
        snap->records = g_ptr_array_new();
        _fill_host(&snap->host);
        snap->host.observed_mono_us = 0;                   
        snap->host.observed_wall_ms = 0;
    }
    snap->state = PCV_VM_DIAG_S_ERROR;
    snap->error = code;
    pcv_vm_diag_store_publish(snap);
}

                                                                         
                                                                  
static void
_prom_cb(const gchar *name, const gchar *labels, gdouble value, gint kind, gpointer ud)
{
    (void)ud;
    if (kind == PCV_VM_DIAG_PROM_SOURCE_COUNTER)
        pcv_prom_source_counter_set_labels(name, labels, value);
    else
        pcv_prom_gauge_set_labels(name, labels, value);
}

                                                              
                                                          
void
pcv_vm_diag_sampler_finish(void)
{
                                                                     
    GPtrArray *cg = g_ptr_array_new_with_free_func(pcv_cgroup_vm_stat_free);
    pcv_cgroup_psi_collect(NULL, cg);

    if (!S.running || !S.table) {                      
        _log_error(S.error);
        _publish_error(S.error);
        g_ptr_array_unref(cg);
        return;
    }

    PcvVmDiagHostIn host;
    _fill_host(&host);
    PcvVmDiagSnapshot *snap = g_new0(PcvVmDiagSnapshot, 1);
    pcv_vm_diag_table_step(S.table, S.running, S.configs, cg, &host, snap);
    if (S.error != PCV_VM_DIAG_E_NONE) {                              
        snap->state = PCV_VM_DIAG_S_ERROR;
        snap->error = S.error;
    }
    _log_error(S.error);

    if (snap->truncated > 0) {
        gint64 now = g_get_monotonic_time();
        if (now - S.last_trunc_log_us >= (gint64)VM_DIAG_LOG_INTERVAL_SEC * G_USEC_PER_SEC) {
            S.last_trunc_log_us = now;
            PCV_LOG_WARN(VM_DIAG_LOG_DOM, "[vm-diag] tracked=%u truncated=%u",
                         snap->tracked, snap->truncated);
        }
    }

    pcv_vm_diag_prom_series(snap, PCV_PROM_VM_SERIES_MAX, _prom_cb, NULL, NULL);
    pcv_vm_diag_store_publish(snap);               

    g_ptr_array_unref(S.running);
    S.running = NULL;
    g_ptr_array_unref(cg);
}

                                                                      
void
pcv_vm_diag_sampler_shutdown(void)
{
    if (S.running) g_ptr_array_unref(S.running);
    if (S.configs) g_ptr_array_unref(S.configs);
    if (S.owners) g_hash_table_destroy(S.owners);
    pcv_vm_diag_table_free(S.table);
    memset(&S, 0, sizeof(S));
    pcv_vm_diag_store_publish(NULL);
}
