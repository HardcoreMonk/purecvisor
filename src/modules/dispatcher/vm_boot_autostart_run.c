   
                                
                                                                                       
  
                           
                                                                                                              
                                                                                                      
                                                                             
                                                                      
                                                                                  
                                                 
                                                                                                                         
                                                                                           
                                                                                      
                                                   
                                                                                                                 
                                                                                 
                                                                                                          
                                                             
                                                      
  
                                                                     
                                               
   
#include "modules/dispatcher/vm_boot_autostart_run.h"

#include "api/drain.h"
#include "modules/audit/pcv_audit.h"
#include "modules/daemons/prometheus_exporter.h"
#include "modules/dispatcher/handler_vm_start.h"
#include "modules/network/network_manager.h"
#include "modules/network/vpc/vpc_manager.h"
#include "modules/virt/virt_conn_pool.h"
#include "modules/virt/vm_boot_autostart.h"
#include "utils/pcv_config.h"

                                                                             
static void log_same_boot(const PcvBootMarker *m)
{
    guint n = m->candidates ? g_strv_length(m->candidates) : 0;
    g_autofree gchar *list = n ? g_strjoinv(", ", m->candidates) : g_strdup("-");
    g_message("[boot-autostart] already evaluated in this boot (decision=%s, candidates=%u: %s)",
              m->decision, n, list);
}

                                                                                  
                                                          
                                        
static gboolean _iface_exists(const gchar *bridge)
{
    return pcv_boot_autostart_iface_exists_at("/sys/class/net", bridge);
}

                                                                              
typedef struct {
    PcvBootScanCounts scan;
    guint             candidates;
    gint64            t0;
} BootRunCtx;

                                                               
                                                    
static gboolean _rc_recheck(const gchar *vm, int *state_out, int *reason_out, gpointer user_data)
{
    (void)user_data;
    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn)
        return FALSE;
    gboolean ok = FALSE;
    virDomainPtr dom = virDomainLookupByName(conn, vm);
    if (dom) {
        ok = virDomainGetState(dom, state_out, reason_out, 0) == 0;
        virDomainFree(dom);
    }
    virt_conn_pool_release(conn);
    return ok;
}

                                                                       
static void _rc_done(const gchar *vm_id, gboolean ok, const GError *error, gpointer user_data)
{
    if (!ok)
        g_warning("[boot-autostart] '%s' start failed: %s", vm_id, error ? error->message : "unknown");
    pcv_boot_autostart_item_done(user_data, ok);
}

                                                                          
                                                                  
static PcvBootSubmit _rc_submit(const gchar *vm, PcvBootRun *run, gchar **reason_out, gpointer user_data)
{
    (void)user_data;
    PcvVmStartRequest req = { .vm_id = vm, .numa_node = 0, .vcpu_count = 0, .bridge_name = NULL, .origin = PCV_BOOT_AUTOSTART_ORIGIN, .skip_if_active = TRUE, .actor = PCV_AUDIT_ACTOR_SYSTEM };
    gchar *err = NULL;
    gboolean lock_busy = FALSE;
    switch (pcv_vm_start_submit_ex(&req, _rc_done, run, &err, &lock_busy)) {
    case PCV_VM_START_SUBMITTED:
        g_free(err);
        return PCV_BOOT_SUBMIT_ASYNC;
    case PCV_VM_START_ALREADY_ACTIVE:
        g_free(err);
        return PCV_BOOT_SUBMIT_ALREADY_ACTIVE;
    case PCV_VM_START_REJECTED:
    default:
        if (lock_busy) {
            *reason_out = err;                                                 
            return PCV_BOOT_SUBMIT_LOCK_BUSY;
        }
        *reason_out = err;
        return PCV_BOOT_SUBMIT_REJECTED;
    }
}

                                                                                     
static void _rc_report(const gchar *vm, const gchar *reason, gpointer user_data)
{
    (void)user_data;
    pcv_vm_start_report_rejected(vm, PCV_BOOT_AUTOSTART_ORIGIN, reason, PCV_AUDIT_ACTOR_SYSTEM);
}

                                          
static void _rc_count(const gchar *result, gpointer user_data)
{
    (void)user_data;
    pcv_prom_inc("purecvisor_vm_boot_autostart_total", "result", result);
}

                                                                                    
static void _rc_finished(const PcvBootRunTally *t, gpointer user_data)
{
    BootRunCtx *c = user_data;
    guint state_reason = c->scan.state_reason + t->skipped_state_reason;
    g_message("[boot-autostart] summary decision=act started=%u failed=%u rejected=%u already_active=%u lock_busy=%u "
              "skipped_state_reason=%u skipped_bridge_missing=%u not_pcv=%u",
              t->started, t->failed, t->rejected, t->already_active, t->lock_busy, state_reason,
              c->scan.bridge_missing, c->scan.not_pcv);
    const gchar *result = c->candidates == 0 ? "none"
                        : (t->failed == 0 && t->rejected == 0) ? "ok"
                        : t->started == 0 ? "fail" : "partial";
    g_autofree gchar *target = g_strdup_printf("host started=%u failed=%u rejected=%u already_active=%u lock_busy=%u "
                                               "skipped=%u", t->started, t->failed, t->rejected, t->already_active,
                                               t->lock_busy, state_reason + c->scan.bridge_missing);
    pcv_audit_log(PCV_AUDIT_ACTOR_SYSTEM, "vm.boot_autostart", target, result, 0,
                  (g_get_monotonic_time() - c->t0) / 1000, NULL);
    g_free(c);
}

                                                                      
static void _rc_count_scan(const PcvBootScanCounts *s)
{
    for (guint i = 0; i < s->bridge_missing; i++)
        _rc_count("skipped_bridge_missing", NULL);
    for (guint i = 0; i < s->state_reason; i++)
        _rc_count("skipped_state_reason", NULL);
}

                                                                                     
                                                                     
static void run_act(const gchar *boot_id, gint64 t0)
{
    const PcvBootBridgeOracle oracle = {
        pcv_config_get_string("network", "default_bridge", "pcvnat0"),
        pcv_network_bridge_uplink_mode,
        pcv_vpc_bridge_is_managed,
        _iface_exists,
    };
    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        g_warning("[boot-autostart] deferred: libvirt unavailable (marker not written)");
        return;
    }
    PcvBootScanCounts counts = { 0 };
    g_autoptr(GError) err = NULL;
    g_autoptr(GPtrArray) cands = pcv_boot_autostart_scan(conn, &oracle, &counts, &err);
    virt_conn_pool_release(conn);
    if (!cands) {
        g_warning("[boot-autostart] domain list failed: %s", err ? err->message : "unknown");
        g_warning("[boot-autostart] deferred: libvirt unavailable (marker not written)");
        return;
    }
    g_ptr_array_add(cands, NULL);
    const gchar *const *names = (const gchar *const *)cands->pdata;
    guint n = cands->len - 1;

    g_autoptr(GError) werr = NULL;
    if (!pcv_boot_autostart_marker_write_at(PCV_BOOT_AUTOSTART_MARKER_PATH, boot_id, PCV_BOOT_DECIDE_ACT, names,
                                            &werr)) {
        g_warning("[boot-autostart] marker write failed — no VM started: %s", werr->message);
        pcv_audit_log(PCV_AUDIT_ACTOR_SYSTEM, "vm.boot_autostart", "host marker-write", "fail", 0,
                      (g_get_monotonic_time() - t0) / 1000, NULL);
        return;
    }
    g_message("[boot-autostart] %u candidate(s); skipped_state_reason=%u skipped_bridge_missing=%u not_pcv=%u "
              "query_error=%u", n, counts.state_reason, counts.bridge_missing, counts.not_pcv, counts.query_error);

    _rc_count_scan(&counts);
    BootRunCtx *ctx = g_new0(BootRunCtx, 1);
    ctx->scan = counts;
    ctx->candidates = n;
    ctx->t0 = t0;
                                                                                
    const PcvBootRunOps ops = { _rc_recheck, _rc_submit, _rc_report, pcv_drain_is_terminating, _rc_count, _rc_finished, ctx };
    pcv_boot_autostart_run_queue(names, n, &ops);
}

                               
                                                            
void pcv_vm_boot_autostart_run(void)
{
    const gint64 t0 = g_get_monotonic_time();
    gboolean enabled = pcv_config_get_int("network", "vm_autostart_after_bootstrap", 1) != 0;

    g_autoptr(GError) err = NULL;
    g_autofree gchar *boot_id = NULL;
    if (!pcv_boot_autostart_read_boot_id_at(PCV_BOOT_AUTOSTART_BOOT_ID_PATH, &boot_id, &err))
        g_warning("[boot-autostart] boot_id unreadable — no decision this start: %s", err->message);

    PcvBootMarker marker = { 0 };
    pcv_boot_autostart_marker_read_at(PCV_BOOT_AUTOSTART_MARKER_PATH, &marker);
    if (marker.state == PCV_BOOT_MARKER_CORRUPT)
        g_warning("[boot-autostart] marker %s unreadable or corrupt — recording this boot only",
                  PCV_BOOT_AUTOSTART_MARKER_PATH);

    gboolean write_now = FALSE;
    PcvBootDecision d = pcv_boot_autostart_decide(enabled, boot_id, &marker, &write_now);
    g_message("[boot-autostart] decision=%s boot=%.8s prev=%.8s enabled=%d",
              pcv_boot_autostart_decision_name(d), boot_id ? boot_id : "none",
              marker.state == PCV_BOOT_MARKER_OK ? marker.boot_id : "none", enabled ? 1 : 0);

    if (d == PCV_BOOT_DECIDE_SAME_BOOT) {
        log_same_boot(&marker);
    } else if (d == PCV_BOOT_DECIDE_ACT) {
        run_act(boot_id, t0);
    } else if (write_now) {
        g_autoptr(GError) werr = NULL;
        if (!pcv_boot_autostart_marker_write_at(PCV_BOOT_AUTOSTART_MARKER_PATH, boot_id, d, NULL, &werr))
            g_warning("[boot-autostart] marker write failed — no VM started: %s", werr->message);
    }
    pcv_boot_autostart_marker_clear(&marker);
}
