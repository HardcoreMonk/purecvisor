   
                             
                                                                                                             
  
                           
                                                                                                    
                                                                                                      
                                                                                         
                                                                                                                  
                             
                                                                                                          
                                                                                                    
  
                                                                              
   
#include "api/rest_public_render.h"

#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#include <json-glib/json-glib.h>

#include "modules/audit/pcv_audit.h"
#include "modules/daemons/prometheus_exporter.h"
#include "modules/network/dpdk_manager.h"
#include "modules/network/ovn_manager.h"
#include "modules/virt/pcv_virt_open.h"
#include "purecvisor/version.h"
#include "utils/pcv_bpf.h"
#include "utils/pcv_config.h"
#include "utils/pcv_log.h"
#include "utils/pcv_tls.h"

                                                                
#define PCV_REST_HEALTH_PROBE_TIMEOUT_SEC 3
#define PCV_REST_HEALTH_CACHE_USEC (5 * G_USEC_PER_SEC)

                                                                     
#define PCV_REST_INTERNAL_DEADLINE_SEC PCV_VIRT_OPEN_TIMEOUT_SEC
                                                                   
#define PCV_REST_INTERNAL_ABANDONED_MAX 2
#define REST_RENDER_LOG_DOM "rest_render"

                                                        
static struct {
    GMutex  mu;
    gdouble total;
    gdouble idle;
} g_metrics_prev;

                                           
static struct {
    GMutex     mu;
    gint64     cache_ts;                                          
    gchar     *cache_body;                         
    guint      cache_code;                         
    gboolean   refreshing;                   
    GPtrArray *waiters;                                          
} g_health;

static PcvRestHealthOpenFn g_health_open_fn;                                              

                                       
void
pcv_rest_rendered_clear(PcvRestRendered *r)
{
    g_clear_pointer(&r->body, g_free);
    r->status = 0;
    r->content_type = NULL;
}

                                                                                  
                                                        
void
pcv_rest_render_metrics(const gchar *fleet_resp, PcvRestRendered *out)
{
    GString *prom = g_string_new("");
    if (fleet_resp) {
        JsonParser *jp = json_parser_new();
                                                                      
        if (json_parser_load_from_data(jp, fleet_resp, -1, NULL)) {
            JsonObject *root = json_node_get_object(json_parser_get_root(jp));
            if (json_object_has_member(root, "result")) {
                JsonObject *res = json_object_get_object_member(root, "result");

                              
                if (json_object_has_member(res, "host")) {
                    JsonObject *h = json_object_get_object_member(res, "host");
                    {
                        gdouble cpu_pct = 0;
                        if (json_object_has_member(h,"cpu_percent")) {
                            cpu_pct = json_object_get_double_member(h,"cpu_percent");
                        } else if (json_object_has_member(h,"cpu_total_ticks") &&
                                   json_object_has_member(h,"cpu_idle_ticks")) {
                                                                                   
                            gdouble total = json_object_get_double_member(h,"cpu_total_ticks");
                            gdouble idle = json_object_get_double_member(h,"cpu_idle_ticks");
                            g_mutex_lock(&g_metrics_prev.mu);
                            gdouble dt = total - g_metrics_prev.total;
                            gdouble di = idle - g_metrics_prev.idle;
                            if (dt > 0 && g_metrics_prev.total > 0)
                                cpu_pct = 100.0 * (1.0 - di / dt);
                            g_metrics_prev.total = total;
                            g_metrics_prev.idle = idle;
                            g_mutex_unlock(&g_metrics_prev.mu);
                        }
                        g_string_append_printf(prom,
                            "# HELP purecvisor_host_cpu_percent Host CPU utilization\n"
                            "# TYPE purecvisor_host_cpu_percent gauge\n"
                            "purecvisor_host_cpu_percent %.2f\n", cpu_pct);
                    }
                    g_string_append_printf(prom,
                        "# HELP purecvisor_host_memory_percent Host memory utilization\n"
                        "# TYPE purecvisor_host_memory_percent gauge\n"
                        "purecvisor_host_memory_percent %.2f\n",
                        json_object_has_member(h,"mem_percent")
                            ? json_object_get_double_member(h,"mem_percent") : 0);
                    g_string_append_printf(prom,
                        "# HELP purecvisor_host_disk_percent Host disk utilization\n"
                        "# TYPE purecvisor_host_disk_percent gauge\n"
                        "purecvisor_host_disk_percent %.2f\n",
                        json_object_has_member(h,"disk_percent")
                            ? json_object_get_double_member(h,"disk_percent") : 0);
                    if (json_object_has_member(h,"mem_total_gb"))
                        g_string_append_printf(prom,
                            "# HELP purecvisor_host_memory_total_bytes Host total memory\n"
                            "# TYPE purecvisor_host_memory_total_bytes gauge\n"
                            "purecvisor_host_memory_total_bytes %.0f\n",
                            json_object_get_double_member(h,"mem_total_gb") * 1073741824.0);
                    if (json_object_has_member(h,"cpu_temp_c"))
                        g_string_append_printf(prom,
                            "# HELP purecvisor_host_cpu_temp_celsius CPU temperature\n"
                            "# TYPE purecvisor_host_cpu_temp_celsius gauge\n"
                            "purecvisor_host_cpu_temp_celsius %.1f\n",
                            json_object_get_double_member(h,"cpu_temp_c"));
                    if (json_object_has_member(h,"load_1"))
                        g_string_append_printf(prom,
                            "# HELP purecvisor_host_load1 1-minute load average\n"
                            "# TYPE purecvisor_host_load1 gauge\n"
                            "purecvisor_host_load1 %.2f\n",
                            json_object_get_double_member(h,"load_1"));
                }

                             
                if (json_object_has_member(res, "fleet")) {
                    JsonArray *fleet = json_object_get_array_member(res, "fleet");
                    guint n = json_array_get_length(fleet);

                    g_string_append(prom,
                        "# HELP purecvisor_vm_vcpu VM vCPU count\n"
                        "# TYPE purecvisor_vm_vcpu gauge\n");
                    for (guint i = 0; i < n; i++) {
                        JsonObject *vm = json_array_get_object_element(fleet, i);
                        const gchar *vn = json_object_get_string_member(vm, "name");
                        g_string_append_printf(prom,
                            "purecvisor_vm_vcpu{vm=\"%s\"} %ld\n",
                            vn, (long)json_object_get_int_member(vm, "vcpu"));
                    }

                    g_string_append(prom,
                        "# HELP purecvisor_vm_memory_max_mb VM max memory MB\n"
                        "# TYPE purecvisor_vm_memory_max_mb gauge\n");
                    for (guint i = 0; i < n; i++) {
                        JsonObject *vm = json_array_get_object_element(fleet, i);
                        const gchar *vn = json_object_get_string_member(vm, "name");
                        g_string_append_printf(prom,
                            "purecvisor_vm_memory_max_mb{vm=\"%s\"} %.0f\n",
                            vn, json_object_get_double_member(vm, "mem_max_mb"));
                    }

                    g_string_append(prom,
                        "# HELP purecvisor_vm_memory_used_mb VM used memory MB\n"
                        "# TYPE purecvisor_vm_memory_used_mb gauge\n");
                    for (guint i = 0; i < n; i++) {
                        JsonObject *vm = json_array_get_object_element(fleet, i);
                        const gchar *vn = json_object_get_string_member(vm, "name");
                        g_string_append_printf(prom,
                            "purecvisor_vm_memory_used_mb{vm=\"%s\"} %.0f\n",
                            vn, json_object_get_double_member(vm, "mem_used_mb"));
                    }

                    g_string_append(prom,
                        "# HELP purecvisor_vm_running VM running state\n"
                        "# TYPE purecvisor_vm_running gauge\n");
                    for (guint i = 0; i < n; i++) {
                        JsonObject *vm = json_array_get_object_element(fleet, i);
                        const gchar *vn = json_object_get_string_member(vm, "name");
                        const gchar *st = json_object_get_string_member(vm, "state");
                        g_string_append_printf(prom,
                            "purecvisor_vm_running{vm=\"%s\"} %d\n",
                            vn, g_strcmp0(st, "RUNNING") == 0 ? 1 : 0);
                    }
                }
            }
        }
        g_object_unref(jp);
    }

                                            
    {
        gchar *reg_metrics = pcv_prom_render();
        if (reg_metrics) {
            g_string_append(prom, "\n# HELP purecvisor_registry PureCVisor Registry Metrics\n");
            g_string_append(prom, reg_metrics);
            g_free(reg_metrics);
        }
    }

    out->status = 200;
    out->content_type = "text/plain; version=0.0.4; charset=utf-8";
    out->body = g_string_free(prom, FALSE);
}

                                                                                              
                                                    
void
pcv_rest_render_rpc_result(const gchar *resp, const gchar *fail_detail, PcvRestRendered *out)
{
    out->content_type = NULL;
    if (!resp) {
        out->status = 500;
        out->body = g_strdup_printf("{\"error\":{\"code\":\"INTERNAL_ERROR\",\"message\":\"%s\"}}", fail_detail);
        return;
    }
    out->status = 200;
    JsonParser *jp = json_parser_new();
                                                                  
    if (json_parser_load_from_data(jp, resp, -1, NULL)) {
        JsonObject *root = json_node_get_object(json_parser_get_root(jp));
        if (json_object_has_member(root, "result")) {
            JsonNode *rn = json_object_dup_member(root, "result");
            out->body = json_to_string(rn, FALSE);
            json_node_unref(rn);
        } else {
            out->body = g_strdup(resp);
        }
    } else {
        out->body = g_strdup(resp);
    }
    g_object_unref(jp);
}

                                                                                  
                                                                                                          
                                                                                            
                                     
typedef struct {
    JsonArray *(*vms_fn)(void);
    gint       (*count_fn)(void);
    gboolean   is_vms;
    GMutex     mu;
    GCond      cond;
    gboolean   done;                   
    gboolean   abandoned;              
    JsonArray *arr;                                          
    gint       count;                              
} InternalBox;

static gint g_internal_abandoned;              
static JsonArray *(*g_internal_vms_fn)(void);                           
static gint (*g_internal_count_fn)(void);
static guint g_internal_deadline_ms;                                                             

                                            
static void
_internal_box_clear(gpointer p)
{
    InternalBox *b = p;
    if (b->arr)
        json_array_unref(b->arr);
    g_mutex_clear(&b->mu);
    g_cond_clear(&b->cond);
}

                                                                                       
                                         
static JsonArray *
_real_internal_vms(void)
{
    virConnectPtr vconn = pcv_virt_open_bounded("qemu:///system", FALSE, PCV_VIRT_OPEN_TIMEOUT_SEC, NULL);                  
    if (!vconn)
        return NULL;
    JsonArray *arr = json_array_new();
    virDomainPtr *domains = nullptr;
    int n = virConnectListAllDomains(vconn, &domains,
        VIR_CONNECT_LIST_DOMAINS_ACTIVE | VIR_CONNECT_LIST_DOMAINS_INACTIVE);
    for (int i = 0; i < n; i++) {
        JsonObject *vm = json_object_new();
        const gchar *name = virDomainGetName(domains[i]);
        json_object_set_string_member(vm, "name", name ? name : "?");
        int state = 0;
        virDomainGetState(domains[i], &state, NULL, 0);
        json_object_set_string_member(vm, "state",
            state == 1 ? "running" : state == 5 ? "shutoff" : "other");
        virDomainInfo info;
        if (virDomainGetInfo(domains[i], &info) == 0) {
            json_object_set_int_member(vm, "vcpu", info.nrVirtCpu);
            json_object_set_int_member(vm, "memory_mb", (gint64)info.memory / 1024);
        }
        json_array_add_object_element(arr, vm);
        virDomainFree(domains[i]);
    }
    if (domains) free(domains);
    virConnectClose(vconn);
    return arr;
}

                          
                                        
static gint
_real_internal_count(void)
{
    virConnectPtr tc = pcv_virt_open_bounded("qemu:///system", FALSE, PCV_VIRT_OPEN_TIMEOUT_SEC, NULL);
    if (!tc)
        return -1;
    gint n = virConnectNumOfDomains(tc);
    virConnectClose(tc);
    return n;
}

                                                                       
static gpointer
_internal_helper(gpointer data)
{
    InternalBox *b = data;
    JsonArray *arr = NULL;
    gint count = -1;
    if (b->is_vms)
        arr = b->vms_fn();
    else
        count = b->count_fn();

    g_mutex_lock(&b->mu);
    b->done = TRUE;
    gboolean abandoned = b->abandoned;
    if (!abandoned) {
        b->arr = arr;
        b->count = count;
        arr = NULL;
    }
    g_cond_signal(&b->cond);
    g_mutex_unlock(&b->mu);

    if (arr)
        json_array_unref(arr);
    if (abandoned)
        g_atomic_int_add(&g_internal_abandoned, -1);
    g_atomic_rc_box_release_full(b, _internal_box_clear);
    return NULL;
}

                                                                                                          
                                                           
                                                                            
static gboolean
_internal_query(gboolean is_vms, const gchar *what, JsonArray **arr, gint *count)
{
    if (g_atomic_int_get(&g_internal_abandoned) >= PCV_REST_INTERNAL_ABANDONED_MAX)
        return FALSE;                                     

    InternalBox *b = g_atomic_rc_box_new0(InternalBox);
    b->is_vms = is_vms;
    b->vms_fn = g_internal_vms_fn ? g_internal_vms_fn : _real_internal_vms;
    b->count_fn = g_internal_count_fn ? g_internal_count_fn : _real_internal_count;
    b->count = -1;
    g_mutex_init(&b->mu);
    g_cond_init(&b->cond);

    g_atomic_rc_box_acquire(b);              
    GError *err = NULL;
    GThread *th = g_thread_try_new("rest-internal", _internal_helper, b, &err);
    if (!th) {
        PCV_LOG_WARN(REST_RENDER_LOG_DOM, "%s: helper thread: %s", what, err->message);
        g_error_free(err);
        g_atomic_rc_box_release_full(b, _internal_box_clear);
        g_atomic_rc_box_release_full(b, _internal_box_clear);
        return FALSE;
    }
    g_thread_unref(th);

    gint64 wait_us = g_internal_deadline_ms ? (gint64)g_internal_deadline_ms * 1000
                                            : (gint64)PCV_REST_INTERNAL_DEADLINE_SEC * G_USEC_PER_SEC;
    gint64 deadline = g_get_monotonic_time() + wait_us;
    gboolean ok;
    g_mutex_lock(&b->mu);
    while (!b->done && g_cond_wait_until(&b->cond, &b->mu, deadline))
        ;
    ok = b->done;
    if (ok) {
        *arr = b->arr;
        b->arr = NULL;
        *count = b->count;
    } else {
        b->abandoned = TRUE;
        g_atomic_int_inc(&g_internal_abandoned);
    }
    g_mutex_unlock(&b->mu);
    g_atomic_rc_box_release_full(b, _internal_box_clear);
    if (!ok)
        PCV_LOG_WARN(REST_RENDER_LOG_DOM, "%s: libvirt query exceeded %d s; returning the unavailable result", what,
                     PCV_REST_INTERNAL_DEADLINE_SEC);
    return ok;
}

                                                                                               
                                                                  
void
pcv_rest_render_internal_vms(PcvRestRendered *out)
{
    JsonArray *arr = NULL;
    gint unused = -1;
    if (!_internal_query(TRUE, "/internal/vms", &arr, &unused) || !arr)
        arr = json_array_new();
    JsonNode *node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(node, arr);
    gchar *data = json_to_string(node, FALSE);
    json_node_free(node);
    out->status = 200;
    out->content_type = NULL;
    out->body = data;
}

                                                                                                     
                                                             
void
pcv_rest_render_internal_telemetry(PcvRestRendered *out)
{
    JsonObject *obj = json_object_new();
                                                              
    FILE *cpuf = fopen("/proc/stat", "r");
    if (cpuf) {
        char buf[256];
        if (fgets(buf, sizeof(buf), cpuf)) {
            guint64 u,n,s,id,io,ir,si,st;
            if (sscanf(buf,"cpu %lu %lu %lu %lu %lu %lu %lu %lu",&u,&n,&s,&id,&io,&ir,&si,&st)==8) {
                guint64 tot=u+n+s+id+io+ir+si+st, idle=id+io;
                json_object_set_double_member(obj,"cpu_percent",tot>0?100.0*(1.0-(double)idle/(double)tot):0);
            }
        }
        fclose(cpuf);
    }
                
    FILE *memf = fopen("/proc/meminfo", "r");
    if (memf) {
        char line[128]; guint64 val;
        while (fgets(line, sizeof(line), memf)) {
            if (sscanf(line,"MemTotal: %lu kB",&val)==1) json_object_set_int_member(obj,"mem_total_kb",(gint64)val);
            else if (sscanf(line,"MemAvailable: %lu kB",&val)==1) json_object_set_int_member(obj,"mem_avail_kb",(gint64)val);
        }
        fclose(memf);
    }
                  
    JsonArray *unused_arr = NULL;
    gint vm_count = -1;
    if (_internal_query(FALSE, "/internal/telemetry", &unused_arr, &vm_count) && vm_count >= 0)
        json_object_set_int_member(obj,"vm_count",vm_count);
    JsonNode *tn = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(tn, obj);
    gchar *td = json_to_string(tn, FALSE);
    json_node_free(tn);
    out->status = 200;
    out->content_type = NULL;
    out->body = td;
}

                                                                                                     
                                                          
void
pcv_rest_render_health(const PcvRestHealthInput *in, PcvRestRendered *out)
{
    JsonObject *root = json_object_new();
    JsonObject *checks = json_object_new();
    gboolean critical = FALSE;
    gboolean degraded = FALSE;

                                             
      
             
                                                                
                                                 
                                                                    
                                            
    {
        JsonObject *c = json_object_new();
        gint64 t0 = g_get_monotonic_time();                         
                                                                                         
                                                    
        gboolean backlog = pcv_virt_close_detached_pending() >= PCV_VIRT_CLOSE_DETACHED_MAX;
        virConnectPtr conn = backlog ? NULL : (g_health_open_fn ? g_health_open_fn : pcv_virt_open_bounded)(
            "qemu:///system", TRUE, PCV_REST_HEALTH_PROBE_TIMEOUT_SEC, NULL);
        gint64 dt = (g_get_monotonic_time() - t0) / 1000;
        if (conn) {
            json_object_set_boolean_member(c, "ok", TRUE);
            json_object_set_int_member(c, "latency_ms", dt);
            const gchar *sla = (dt < 200) ? "ok" : (dt < 500) ? "warn" : "critical";
            json_object_set_string_member(c, "sla", sla);
                                                  
            pcv_virt_close_detached(conn);
        } else {
            json_object_set_boolean_member(c, "ok", FALSE);
            json_object_set_string_member(c, "error",
                backlog ? "previous libvirt connections are still closing" : "connection failed");
            critical = TRUE;
        }
        json_object_set_object_member(checks, "libvirt", c);
    }

                    
      
                                             
                                        
                                                    
    {
        JsonObject *c = json_object_new();
        json_object_set_boolean_member(c, "ok", TRUE);
        json_object_set_string_member(c, "note", "single_edge build: distributed metadata store not used");
        json_object_set_object_member(checks, "etcd", c);
    }

                                        
      
                                                     
                                              
                                           
                                         
    {
        JsonObject *c = json_object_new();
        const gchar *db_path = pcv_config_get_string("daemon", "db_path",
                                   "/var/lib/purecvisor/vm_state.db");
        gchar *dir = g_path_get_dirname(db_path);
        struct statvfs sv;
        if (statvfs(dir, &sv) == 0) {
            gdouble avail_gb = (gdouble)sv.f_bavail * sv.f_frsize / (1024.0*1024*1024);
            json_object_set_boolean_member(c, "ok", avail_gb > 0.1);
            json_object_set_double_member(c, "avail_gb",
                (double)((gint)(avail_gb * 100)) / 100.0);
            if (avail_gb <= 0.1) critical = TRUE;
        } else {
            json_object_set_boolean_member(c, "ok", FALSE);
            json_object_set_string_member(c, "error", "statvfs failed");
            critical = TRUE;
        }
        g_free(dir);
        json_object_set_object_member(checks, "disk", c);
    }

                           
    {
        JsonObject *c = json_object_new();
        const gchar *audit_path = "/var/lib/purecvisor/pcv_audit.db";
        struct stat st;
        if (stat(audit_path, &st) == 0) {
            json_object_set_boolean_member(c, "ok", TRUE);
            json_object_set_double_member(c, "size_mb",
                (double)((gint)(st.st_size / (1024.0*1024) * 10)) / 10.0);
        } else {
            json_object_set_boolean_member(c, "ok", TRUE);
            json_object_set_string_member(c, "note", "no audit db yet");
        }
        json_object_set_object_member(checks, "audit_db", c);
    }

                                                   
                                                              
                                                           
    {
        JsonObject *c = json_object_new();
        PcvAuditChainHealth audit = pcv_audit_get_chain_health();
        json_object_set_boolean_member(c, "ok", audit.current_ok);
        json_object_set_int_member(c, "active_epoch", audit.active_epoch);
        json_object_set_boolean_member(c, "historical_break",
                                       audit.historical_break);
        json_object_set_int_member(c, "first_break_rowid",
                                   audit.first_break_rowid);
        json_object_set_int_member(c, "last_verified_at",
                                   audit.last_verified_at);
        json_object_set_string_member(c, "reason", audit.reason);
        PcvRestAuditHealthImpact impact =
            pcv_rest_audit_chain_health_impact(audit.current_ok,
                                               audit.historical_break);
        if (impact == PCV_REST_AUDIT_HEALTH_CRITICAL)
            critical = TRUE;
        json_object_set_object_member(checks, "audit_chain", c);
    }

                
    {
        JsonObject *c = json_object_new();
        PcvRestTlsHealth tls = pcv_rest_tls_health(in->mode,
                                                   pcv_tls_is_enabled(),
                                                   in->tls_degraded,
                                                   in->tls_degraded_reason);
        json_object_set_string_member(c, "mode", tls.mode);
        json_object_set_boolean_member(c, "enabled", tls.enabled);
        json_object_set_boolean_member(c, "degraded", tls.degraded);
        json_object_set_string_member(c, "status", tls.status);
        if (tls.reason)
            json_object_set_string_member(c, "reason", tls.reason);
        json_object_set_object_member(checks, "tls", c);
    }

                                                              
    {
        JsonObject *bpf = json_object_new();
        PcvBpfHealth h = pcv_bpf_health();
        json_object_set_string_member(bpf, "status",
            h == PCV_BPF_OK ? "ok" :
            h == PCV_BPF_DEGRADED_NO_BTF ? "degraded_no_btf" : "degraded_no_lsm");
        const char *reason = pcv_bpf_health_reason();
        if (reason) json_object_set_string_member(bpf, "reason", reason);
        json_object_set_object_member(checks, "bpf", bpf);
    }

                               
    {
        JsonObject *c = json_object_new();
        const gchar *vm_db = pcv_config_get_string("daemon", "db_path",
                                  "/var/lib/purecvisor/vm_state.db");
        struct stat st_vm;
        if (stat(vm_db, &st_vm) == 0) {
            json_object_set_boolean_member(c, "ok", TRUE);
            json_object_set_double_member(c, "size_mb",
                (double)((gint)(st_vm.st_size / (1024.0*1024) * 10)) / 10.0);
        } else {
            json_object_set_boolean_member(c, "ok", TRUE);
            json_object_set_string_member(c, "note", "no vm_state db yet");
        }
        json_object_set_object_member(checks, "vm_state_db", c);
    }

                           
    {
        JsonObject *c = json_object_new();
        json_object_set_string_member(c, "mode", "single_edge");
        json_object_set_object_member(checks, "cluster", c);
    }

                         
    {
        JsonObject *cap = json_object_new();
        json_object_set_boolean_member(cap, "ovn", pcv_ovn_is_available());
        json_object_set_boolean_member(cap, "dpdk", pcv_dpdk_is_available());
        json_object_set_boolean_member(cap, "cluster", FALSE);
        json_object_set_object_member(root, "capabilities", cap);
    }

                        
    const gchar *status_str = critical ? "critical" : degraded ? "degraded" : "ok";
    gint http_code = critical ? 503 : 200;

    json_object_set_string_member(root, "status", status_str);
    json_object_set_string_member(root, "service", "purecvisorsd");
    json_object_set_string_member(root, "version", PCV_PRODUCT_VERSION);
    json_object_set_string_member(root, "node_name", g_get_host_name());
    json_object_set_int_member(root, "uptime_sec",
        (gint64)(g_get_monotonic_time() / G_USEC_PER_SEC));
    json_object_set_object_member(root, "checks", checks);

                   
    JsonNode *jn = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(jn, root);
    gchar *body = json_to_string(jn, FALSE);
    json_node_free(jn);

    out->status = (guint)http_code;
    out->content_type = NULL;
    out->body = body;
}

                                                                                             
PcvRestHealthEntry
pcv_rest_health_enter(gint64 now_us, gpointer waiter, gchar **body, guint *code)
{
    PcvRestHealthEntry entry;
    g_mutex_lock(&g_health.mu);
    gboolean fresh = g_health.cache_body && (now_us - g_health.cache_ts) < PCV_REST_HEALTH_CACHE_USEC;
    if (fresh || (g_health.refreshing && g_health.cache_body)) {
        *body = g_strdup(g_health.cache_body);
        *code = g_health.cache_code;
        entry = PCV_REST_HEALTH_CACHED;
    } else {
        if (!g_health.waiters)
            g_health.waiters = g_ptr_array_new();
        g_ptr_array_add(g_health.waiters, waiter);
        entry = g_health.refreshing ? PCV_REST_HEALTH_WAIT : PCV_REST_HEALTH_PROBE;
        g_health.refreshing = TRUE;
    }
    g_mutex_unlock(&g_health.mu);
    return entry;
}

                                                                               
GPtrArray *
pcv_rest_health_complete(const gchar *body, guint code, gint64 now_us)
{
    g_mutex_lock(&g_health.mu);
    g_free(g_health.cache_body);
    g_health.cache_body = g_strdup(body);
    g_health.cache_code = code;
    g_health.cache_ts = now_us;
    g_health.refreshing = FALSE;
    GPtrArray *waiters = g_health.waiters ? g_steal_pointer(&g_health.waiters) : g_ptr_array_new();
    g_mutex_unlock(&g_health.mu);
    return waiters;
}

                                                                                           
void
pcv_rest_render_set_open_fn_for_test(PcvRestHealthOpenFn fn)
{
    g_health_open_fn = fn;
}

                                                                                     
void
pcv_rest_render_set_internal_fns_for_test(JsonArray *(*vms_fn)(void), gint (*vm_count_fn)(void), guint deadline_ms)
{
    g_internal_vms_fn = vms_fn;
    g_internal_count_fn = vm_count_fn;
    g_internal_deadline_ms = deadline_ms;
}

                                                            
void
pcv_rest_render_metrics_reset_for_test(void)
{
    g_mutex_lock(&g_metrics_prev.mu);
    g_metrics_prev.total = g_metrics_prev.idle = 0;
    g_mutex_unlock(&g_metrics_prev.mu);
}

                                                                 
void
pcv_rest_health_reset_for_test(void)
{
    g_mutex_lock(&g_health.mu);
    g_clear_pointer(&g_health.cache_body, g_free);
    g_clear_pointer(&g_health.waiters, g_ptr_array_unref);
    g_health.cache_ts = 0;
    g_health.cache_code = 0;
    g_health.refreshing = FALSE;
    g_mutex_unlock(&g_health.mu);
}
