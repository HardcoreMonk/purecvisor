   
                           
                                                                           
  
                           
                                                   
                                                    
                                        
  
                                                         
                                                               
                                                              
  
                                                                           
  
                                      
                                                                  
                                                                   
                                                        
                                                          
                                                 
                                                           
                                  
  
                               
                                                                    
                                                          
                                                                               
                                                       
                                                     
                                                       
                                                                           
              
   
#include "modules/security/pcv_suricata_ips.h"
#include "utils/pcv_spawn.h"
#include "utils/pcv_log.h"
#include "utils/pcv_config.h"                                         
#include "modules/audit/pcv_audit.h"                               
#include "modules/daemons/alert_engine.h"                              
#include "modules/daemons/prometheus_exporter.h"                                   

#include <string.h>

#define IPS_LOG_DOM              "suricata-ips"
#define IPS_UNIT                 "suricata-ips"
#define IPS_START_TIMEOUT_SEC    15
#define IPS_STOP_TIMEOUT_SEC     25                                               
                                                                               
                                                                       
                                                                                 
#define IPS_PROBE_TIMEOUT_SEC    5
#define IPS_RESTART_TIMEOUT_SEC  30
#define IPS_CHAIN_PRIORITY       "-10"                                                         

                             
                                                                
                                                                     
                                                      
                                                         
#define IPS_READY_TIMEOUT_SEC        60
#define IPS_TICK_READY_TIMEOUT_SEC   20
#define IPS_READY_POLL_MS            500                        

                                                   
                                                                
                                               
#define IPS_NONENFORCE_ALERT_TICKS   3

                                             
  
                       
                                                                        
                                                                          
                       
                               
                                                                        
                                                                               
                         
                                                      
                                                       
                                                   
                        
                                                        
                                                        
                                                
                                                       
                                                      
                                                  
                                                          
#define IPS_NFQUEUE_PROC         "/proc/net/netfilter/nfnetlink_queue"

                                           
#define IPS_RESTART_FAIL_LIMIT   3

                                                                  
static struct {
    GMutex   mu;
    gboolean enabled;
    gboolean degraded;                                                 
    guint    stop_gen;                                                    
                                                                                
                                                                    
                                                
                                                                         
                                                                           
                                                                                
                                                                                   
                                                                   
                                                                   
    gboolean rule_installed;                                                 
    gboolean rule_fail_open;                                                
                                                                                 
                                                                     
                                                                  
                                                                
} G_ips = {0};

                                                                  
                             
                                                                    
                                                            
                               
static struct {
    GMutex   mu;
    guint    restart_fail;                                  
    guint    queue_num;                      
    gboolean fail_open;                    
    gboolean have_snap;                                        
} G_ips_seq = {0};

                                                               

                                     
                                                       
gchar **
pcv_suricata_ips_queue_rule_argv(guint queue_num, gboolean fail_open)
{
                                                                            
                                                                      
    GPtrArray *a = g_ptr_array_new();
    g_ptr_array_add(a, g_strdup("nft"));
    g_ptr_array_add(a, g_strdup("add"));
    g_ptr_array_add(a, g_strdup("rule"));
    g_ptr_array_add(a, g_strdup("inet"));
    g_ptr_array_add(a, g_strdup("purecvisor"));
    g_ptr_array_add(a, g_strdup("ips"));
    g_ptr_array_add(a, g_strdup("queue"));
    g_ptr_array_add(a, g_strdup("num"));
    g_ptr_array_add(a, g_strdup_printf("%u", queue_num));                         
    if (fail_open)
        g_ptr_array_add(a, g_strdup("bypass"));                                                 
    g_ptr_array_add(a, NULL);                     
    return (gchar **)g_ptr_array_free(a, FALSE);                                    
}

                                                        
  
                                                         
                                                    
                                                      
                
  
                                                                  
                                                                  
                                                          
                                                    
              
  
                                              
                                                              
                                                              
                                                            
                                                      
   

                                                        
                                                           
static GMutex        G_ips_hook_mu;
static PcvIpsExecFn  G_ips_exec_hook  = NULL;
static PcvIpsReadyFn G_ips_ready_hook = NULL;
static PcvIpsProbeFn G_ips_probe_hook = NULL;

                                            
                                                    
                                                            
void
pcv_suricata_ips_set_exec_hook(PcvIpsExecFn fn)
{
    g_mutex_lock(&G_ips_hook_mu);
    G_ips_exec_hook = fn;
    g_mutex_unlock(&G_ips_hook_mu);
}

                                                              
                                              
                                                                       
void
pcv_suricata_ips_set_ready_hook(PcvIpsReadyFn fn)
{
    g_mutex_lock(&G_ips_hook_mu);
    G_ips_ready_hook = fn;
    g_mutex_unlock(&G_ips_hook_mu);
}

                                                        
                                                   
                                                                 
                                                           
void
pcv_suricata_ips_set_probe_hook(PcvIpsProbeFn fn)
{
    g_mutex_lock(&G_ips_hook_mu);
    G_ips_probe_hook = fn;
    g_mutex_unlock(&G_ips_hook_mu);
}

                             
static PcvIpsExecFn
_ips_exec_hook(void)
{
    g_mutex_lock(&G_ips_hook_mu);
    PcvIpsExecFn fn = G_ips_exec_hook;
    g_mutex_unlock(&G_ips_hook_mu);
    return fn;
}

                                   
static PcvIpsReadyFn
_ips_ready_hook(void)
{
    g_mutex_lock(&G_ips_hook_mu);
    PcvIpsReadyFn fn = G_ips_ready_hook;
    g_mutex_unlock(&G_ips_hook_mu);
    return fn;
}

                               
static PcvIpsProbeFn
_ips_probe_hook(void)
{
    g_mutex_lock(&G_ips_hook_mu);
    PcvIpsProbeFn fn = G_ips_probe_hook;
    g_mutex_unlock(&G_ips_hook_mu);
    return fn;
}

                                                             
                                          
static gboolean
_ips_exec(const gchar * const *argv, guint timeout_sec, GError **error)
{
    PcvIpsExecFn fn = _ips_exec_hook();
    if (fn)
        return fn(argv, error);
    return pcv_spawn_sync_timeout(argv, NULL, NULL, timeout_sec, error);
}

                                                        
                                       
                                                        
                                                             
                                                  
                                                          
static gboolean
_ips_exec_out(const gchar * const *argv, guint timeout_sec,
              gchar **stdout_out, GError **error)
{
    PcvIpsExecFn fn = _ips_exec_hook();
    if (fn) {
        gboolean ok = fn(argv, error);
        if (stdout_out)
            *stdout_out = g_strdup(ok ? "active" : "failed");
        return ok;
    }
    return pcv_spawn_sync_timeout(argv, stdout_out, NULL, timeout_sec, error);
}

                                                             

                                                              
static struct {
    GMutex           mu;
    gboolean         valid;
    PcvSuricataState state;
} G_ips_pc = {0};

                                        
                                                        
void
pcv_suricata_ips_probe_cache_store(PcvSuricataState s)
{
    g_mutex_lock(&G_ips_pc.mu);
    G_ips_pc.state = s;
    G_ips_pc.valid = TRUE;                                         
    g_mutex_unlock(&G_ips_pc.mu);
}

                                            
                                                          
void
pcv_suricata_ips_engine_status_cached(PcvSuricataState *state, gboolean *binary_present)
{
                                                                 
                                                   
    g_mutex_lock(&G_ips_pc.mu);
    gboolean have = G_ips_pc.valid;
    PcvSuricataState s = have ? G_ips_pc.state : PCV_SURICATA_INACTIVE;
    g_mutex_unlock(&G_ips_pc.mu);

    if (state) *state = s;
    if (binary_present) {
        gchar *bin = g_find_program_in_path("suricata");                           
        *binary_present = (bin != NULL);
        g_free(bin);
    }
}

                                                             
                                                    
                                                                  
static void
_ips_ensure_chain(void)
{
    { const gchar *a[] = {"nft","add","table","inet","purecvisor",NULL};
      _ips_exec(a, 0, NULL); }
                                                                      
    { const gchar *a[] = {"nft","add","chain","inet","purecvisor","ips",
        "{ type filter hook forward priority " IPS_CHAIN_PRIORITY " ; }", NULL};
      _ips_exec(a, 0, NULL); }
}

                                                  
                                                       
                                      
                                                       
static void
_ips_set_rule_state(gboolean installed, gboolean fail_open)
{
    g_mutex_lock(&G_ips.mu);
    G_ips.rule_installed = installed;
    if (installed)
        G_ips.rule_fail_open = fail_open;
    g_mutex_unlock(&G_ips.mu);
}

                                                       
                                                   
                                                 
static void
_ips_get_rule_state(gboolean *installed, gboolean *fail_open)
{
    g_mutex_lock(&G_ips.mu);
    if (installed) *installed = G_ips.rule_installed;
    if (fail_open) *fail_open = G_ips.rule_fail_open;
    g_mutex_unlock(&G_ips.mu);
}

                                                    
                                                  
                                                         
                                                  
                                                    
static gboolean
_ips_rule_installed(void)
{
    gboolean v = FALSE;
    _ips_get_rule_state(&v, NULL);
    return v;
}

                        
                                                             
                                                   
                                                                     
                                          
static GMutex G_ips_watch_mu;
static struct {
    guint    nonenforce_ticks;                                     
    gboolean nonenforce_fired;                                        
    guint    mitigation_ticks;                                                    
    gboolean mitigation_fired;
} G_ips_watch;

                                    
static void
_ips_watch_reset(void)
{
    g_mutex_lock(&G_ips_watch_mu);
    G_ips_watch.nonenforce_ticks = 0;
    G_ips_watch.nonenforce_fired = FALSE;
    G_ips_watch.mitigation_ticks = 0;
    G_ips_watch.mitigation_fired = FALSE;
    g_mutex_unlock(&G_ips_watch_mu);
}

                                         
                                                        
                                                      
                                                      
                                                      
static void
_ips_remove_queue_rule(void)
{
    const gchar *a[] = {"nft","flush","chain","inet","purecvisor","ips",NULL};
    _ips_exec(a, 0, NULL);
    _ips_set_rule_state(FALSE, FALSE);
}

                                                                    
                                                 
                                                              
static gboolean
_ips_install_queue_rule(guint queue_num, gboolean fail_open, GError **error)
{
    _ips_ensure_chain();
    _ips_remove_queue_rule();
    gchar **qa = pcv_suricata_ips_queue_rule_argv(queue_num, fail_open);
    gboolean ok = _ips_exec((const gchar * const *)qa, 0, error);
    g_strfreev(qa);
    _ips_set_rule_state(ok, fail_open);                                     
    return ok;
}

                                                               

                                             
                                                    
                                         
                                                                      
                                                   
                                
gboolean
pcv_suricata_ips_nfqueue_bound(const gchar *proc_text, guint queue_num)
{
    if (!proc_text)
        return FALSE;

    const gchar *p = proc_text;
    while (*p) {
                                                             
        while (*p == ' ' || *p == '\t')
            p++;
        if (g_ascii_isdigit(*p)) {
            guint64 n = 0;
            gboolean overflow = FALSE;
            while (g_ascii_isdigit(*p)) {
                if (n > (G_MAXUINT64 - 9) / 10) overflow = TRUE;                  
                else n = n * 10 + (guint64)(*p - '0');
                p++;
            }
                                                              
            if (!overflow && (*p == '\0' || *p == ' ' || *p == '\t' ||
                              *p == '\n' || *p == '\r') && n == (guint64)queue_num)
                return TRUE;
        }
                    
        while (*p && *p != '\n')
            p++;
        if (*p == '\n')
            p++;
    }
    return FALSE;
}

                                                         
                                                    
                                                                   
                                                        
static gboolean
_ips_scan_u64_field(const gchar **pp, guint64 *out)
{
    const gchar *p = *pp;
    while (*p == ' ' || *p == '\t')
        p++;
    if (!g_ascii_isdigit(*p))
        return FALSE;                         
    guint64 n = 0;
    while (g_ascii_isdigit(*p)) {
        if (n > (G_MAXUINT64 - 9) / 10) n = G_MAXUINT64;                 
        else n = n * 10 + (guint64)(*p - '0');
        p++;
    }
    *out = n;
    *pp  = p;
    return TRUE;
}

                                                                        
             
                                                       
                                                     
                                                     
                                                                             
                                                                            
                                                      
                           
gboolean
pcv_suricata_ips_nfqueue_drops(const gchar *proc_text, guint queue_num,
                               guint64 *queue_dropped, guint64 *user_dropped)
{
    if (!proc_text)
        return FALSE;

    const gchar *p = proc_text;
    while (*p) {
                                             
        while (*p == ' ' || *p == '\t')
            p++;
        if (g_ascii_isdigit(*p)) {
            const gchar *q = p;
            guint64 qn = 0;
            gboolean overflow = FALSE;
            while (g_ascii_isdigit(*q)) {
                if (qn > (G_MAXUINT64 - 9) / 10) overflow = TRUE;
                else qn = qn * 10 + (guint64)(*q - '0');
                q++;
            }
                                                                   
            if (!overflow && (*q == '\0' || *q == ' ' || *q == '\t' ||
                              *q == '\n' || *q == '\r') && qn == (guint64)queue_num) {
                                                                                
                                                                         
                const gchar *r = q;
                guint64 scratch = 0, qd = 0, ud = 0;
                gboolean ok = TRUE;
                for (int i = 0; i < 4 && ok; i++)
                    ok = _ips_scan_u64_field(&r, &scratch);
                if (ok) ok = _ips_scan_u64_field(&r, &qd);           
                if (ok) ok = _ips_scan_u64_field(&r, &ud);           
                if (ok) {
                    if (queue_dropped) *queue_dropped = qd;
                    if (user_dropped)  *user_dropped  = ud;
                    return TRUE;
                }
                return FALSE;                                      
            }
        }
                    
        while (*p && *p != '\n')
            p++;
        if (*p == '\n')
            p++;
    }
    return FALSE;
}

                                                        
                                            
                                                                  
static gboolean
_ips_queue_listener_bound(guint queue_num)
{
    gchar *text = NULL;
    if (!g_file_get_contents(IPS_NFQUEUE_PROC, &text, NULL, NULL))
        return FALSE;
    gboolean bound = pcv_suricata_ips_nfqueue_bound(text, queue_num);
    g_free(text);
    return bound;
}

                            
                                                        
                                                          
static guint
_ips_stop_gen(void)
{
    g_mutex_lock(&G_ips.mu);
    guint g = G_ips.stop_gen;
    g_mutex_unlock(&G_ips.mu);
    return g;
}

                                                           
                                                      
                                                        
                                    
                                                     
                                 
  
                                                               
                                                                             
                                                            
                                                       
                                 
  
                                                 
                                                                              
                                                                
                                                                  
                                                                       
                                            
                                                             
gboolean
pcv_suricata_ips_wait_ready(guint timeout_sec, GError **error)
{
                                                      
    PcvIpsReadyFn rf = _ips_ready_hook();
    if (rf)
        return rf(error);

                                              
    guint queue_num = G_ips_seq.have_snap ? G_ips_seq.queue_num
                                          : (guint)pcv_config_get_ips_queue_num();

    const gchar *sa[] = {"systemctl","is-active",IPS_UNIT,NULL};
    gint64 deadline = g_get_monotonic_time() + (gint64)timeout_sec * G_USEC_PER_SEC;
    gboolean ready = FALSE;
    gboolean aborted = FALSE;
                                                      
    const guint stop_gen0 = _ips_stop_gen();
    PcvSuricataState last = PCV_SURICATA_INACTIVE;

    while (TRUE) {
        gchar *out = NULL;
        _ips_exec_out(sa, IPS_PROBE_TIMEOUT_SEC, &out, NULL);
        last = pcv_suricata_state_from_output(out);
        g_free(out);

        if (last == PCV_SURICATA_FAILED)
            break;                                            

                                                                 
                                                           
                                                       
        if (last == PCV_SURICATA_ACTIVE && _ips_queue_listener_bound(queue_num)) {
            ready = TRUE;
            break;
        }

                                                              
                                                           
                                                         
        if (_ips_stop_gen() != stop_gen0) {
            aborted = TRUE;
            break;
        }

        if (g_get_monotonic_time() >= deadline)
            break;
        g_usleep((gulong)IPS_READY_POLL_MS * 1000);
    }

    if (aborted) {
                                                           
                                              
        PCV_LOG_WARN(IPS_LOG_DOM,
            "readiness 대기 중 종료·disable 신호 관측 — 남은 상한(%us) 포기하고 즉시 이탈"
            "(큐 %u 규칙 미삽입 유지). 종료 시퀀스가 시퀀스 락을 그만큼 빨리 얻는다",
            timeout_sec, queue_num);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                    "suricata-ips readiness aborted by shutdown signal (queue=%u)", queue_num);
        return FALSE;
    }

    if (!ready) {
        PCV_LOG_ERROR(IPS_LOG_DOM,
            "readiness 대기 %us 초과 — 큐 %u 리스너 미확인(유닛 상태 %s). fail-closed 큐 "
            "규칙을 삽입하지 않는다(검사 없는 통과 유지 — 블랙홀 회피)",
            timeout_sec, queue_num, pcv_suricata_state_str(last));
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                    "suricata-ips readiness timeout (%us, queue=%u, unit=%s)",
                    timeout_sec, queue_num, pcv_suricata_state_str(last));
        return FALSE;
    }
    PCV_LOG_INFO(IPS_LOG_DOM,
                 "readiness 확인 — 큐 %u 리스너 바인드됨(큐 규칙 삽입 가능)", queue_num);
    return TRUE;
}

                                                          
                                               
                                                         
                                            
gboolean
pcv_suricata_ips_enable(guint queue_num, gboolean fail_open, GError **error)
{
    g_mutex_lock(&G_ips_seq.mu);

                                                                    
                                                             
                                                             
                                                       
                                                       
                                  
    G_ips_seq.queue_num    = queue_num;
    G_ips_seq.fail_open    = fail_open;
    G_ips_seq.have_snap    = TRUE;
    G_ips_seq.restart_fail = 0;
    g_mutex_lock(&G_ips.mu);
    G_ips.degraded = FALSE;
    g_mutex_unlock(&G_ips.mu);

    if (!fail_open) {
                                                                       
                                                             
                                           
        _ips_ensure_chain();
        _ips_remove_queue_rule();
    }

                                                                          
                                                                  
                                             
    { const gchar *a[] = {"systemctl","start",IPS_UNIT,NULL};
      if (!_ips_exec(a, IPS_START_TIMEOUT_SEC, error)) {
          g_mutex_unlock(&G_ips_seq.mu);
          return FALSE; } }

                                                               
                                                                 
    if (!fail_open && !pcv_suricata_ips_wait_ready(IPS_READY_TIMEOUT_SEC, error)) {
        g_mutex_unlock(&G_ips_seq.mu);
        return FALSE;                                        
    }

                                                                       
    if (!_ips_install_queue_rule(queue_num, fail_open, error)) {
        g_mutex_unlock(&G_ips_seq.mu);
        return FALSE;
    }

                   
    g_mutex_lock(&G_ips.mu);
    G_ips.enabled = TRUE;
    g_mutex_unlock(&G_ips.mu);

    g_mutex_unlock(&G_ips_seq.mu);
    PCV_LOG_INFO(IPS_LOG_DOM, "inline IPS enabled (queue %u, %s)",
                 queue_num, fail_open ? "fail-open" : "fail-closed");
    return TRUE;
}

                                      
                                                     
                                                    
gboolean
pcv_suricata_ips_disable(GError **error)
{
                                                                 
                                                         
                                                       
                                                           
                                        
    pcv_suricata_ips_shutdown_signal();

    g_mutex_lock(&G_ips_seq.mu);
    G_ips_seq.restart_fail = 0;
    G_ips_seq.have_snap    = FALSE;

                                                      
    _ips_remove_queue_rule();

                                                                  
    { const gchar *a[] = {"systemctl","stop",IPS_UNIT,NULL};
      if (!_ips_exec(a, IPS_STOP_TIMEOUT_SEC, error)) {
          g_mutex_unlock(&G_ips_seq.mu);
          return FALSE; } }

    g_mutex_unlock(&G_ips_seq.mu);
    PCV_LOG_INFO(IPS_LOG_DOM, "inline IPS disabled (queue rule flushed, unit stopped)");
    return TRUE;
}

                                                                   
                                                       
                                                        
                                                           
                                                      
                
                                                                    
                                                                
                                                               
                                                                
                                                      
void
pcv_suricata_ips_shutdown_signal(void)
{
    g_mutex_lock(&G_ips.mu);
    G_ips.enabled  = FALSE;
    G_ips.degraded = FALSE;
    G_ips.stop_gen++;                                               
    g_mutex_unlock(&G_ips.mu);
                                                           
                                                       
                                                
    _ips_watch_reset();
}

                                  
                                                       
                                                        
                                             
                                                                    
                                                             
                                                         
                                                              
                                                        
void
pcv_suricata_ips_boot_flush_stale(void)
{
    g_mutex_lock(&G_ips_seq.mu);
    _ips_remove_queue_rule();
    g_mutex_unlock(&G_ips_seq.mu);
    PCV_LOG_INFO(IPS_LOG_DOM,
                 "부팅 스테일 정리 — ips 체인 flush 시도 완료(이전 세션 잔존 큐 규칙 제거)");
}

                                                           
gboolean
pcv_suricata_ips_is_enabled(void)
{
    g_mutex_lock(&G_ips.mu);
    gboolean en = G_ips.enabled;
    g_mutex_unlock(&G_ips.mu);
    return en;
}

                                   
                                                           
gboolean
pcv_suricata_ips_is_degraded(void)
{
    g_mutex_lock(&G_ips.mu);
    gboolean d = G_ips.degraded;
    g_mutex_unlock(&G_ips.mu);
    return d;
}

                                                      

                                                                                 
                                             
                                                    
static PcvSuricataState
_ips_probe(void)
{
                                                        
    PcvIpsProbeFn pf = _ips_probe_hook();
    if (pf)
        return pf();

                                                        
    { gchar *bin = g_find_program_in_path("suricata");
      if (!bin)
          return PCV_SURICATA_ABSENT;
      g_free(bin); }

    const gchar *argv[] = {"systemctl", "is-active", IPS_UNIT, NULL};
    gchar *out = NULL;
    _ips_exec_out(argv, IPS_PROBE_TIMEOUT_SEC, &out, NULL);
    PcvSuricataState s = pcv_suricata_state_from_output(out);
    g_free(out);
    return s;
}

                                                       
                                                               
                                                   
static guint
_ips_current_queue_num(void)
{
    g_mutex_lock(&G_ips_seq.mu);
    guint q = G_ips_seq.have_snap ? G_ips_seq.queue_num
                                  : (guint)pcv_config_get_ips_queue_num();
    g_mutex_unlock(&G_ips_seq.mu);
    return q;
}

                                                             
                                                           
                                                         
                                                              
                                        
static void
_ips_nfqueue_drops_tick(void)
{
    gchar *text = NULL;
    if (!g_file_get_contents(IPS_NFQUEUE_PROC, &text, NULL, NULL))
        return;                                                      

    guint64 qdrop = 0, udrop = 0;
    gboolean ok = pcv_suricata_ips_nfqueue_drops(text, _ips_current_queue_num(),
                                                 &qdrop, &udrop);
    g_free(text);
    if (!ok)
        return;                                          

    pcv_prom_gauge_set_labels("purecvisor_suricata_ips_nfqueue_queue_dropped", "",
                              (gdouble)qdrop);
    pcv_prom_gauge_set_labels("purecvisor_suricata_ips_nfqueue_user_dropped", "",
                              (gdouble)udrop);
}

                                                                                 
                                                             
void
pcv_suricata_ips_metrics_tick(void)
{
    PcvSuricataState st;
    pcv_suricata_ips_engine_status_cached(&st, NULL);
    pcv_prom_gauge_set_labels("purecvisor_suricata_ips_engine_up", "",
                              st == PCV_SURICATA_ACTIVE ? 1.0 : 0.0);
    pcv_prom_gauge_set_labels("purecvisor_suricata_ips_enabled", "",
                              pcv_suricata_ips_is_enabled() ? 1.0 : 0.0);
    pcv_prom_gauge_set_labels("purecvisor_suricata_ips_fail_open", "",
                              pcv_config_get_ips_fail_open() ? 1.0 : 0.0);
                                                                    
                                                   
    pcv_prom_gauge_set_labels("purecvisor_suricata_ips_failopen_degraded", "",
                              pcv_suricata_ips_is_degraded() ? 1.0 : 0.0);
                                                                 
                                                            
    pcv_prom_gauge_set_labels("purecvisor_suricata_ips_queue_rule_installed", "",
                              _ips_rule_installed() ? 1.0 : 0.0);
                                                                   
                                                       
    _ips_nfqueue_drops_tick();
}

                                   
                                                    
                                                              
                                               
  
                                             
                                                                 
                                                  
                                                             
                                                     
                                                                    
                                                  
  
                                                      
                                                                         
                                                                  
  
                                                        
                                              
                                                     
void
pcv_suricata_ips_boot_stop_orphan_unit(void)
{
    PcvSuricataState s = _ips_probe();
    if (s != PCV_SURICATA_ACTIVE)
        return;                                    

    PCV_LOG_WARN(IPS_LOG_DOM,
        "[ips] enabled=false 인데 %s 유닛이 active — 고아 엔진 정지(관측 오도 제거: "
        "규칙은 이미 걷혔으므로 트래픽 영향은 없다)", IPS_UNIT);

    const gchar *a[] = {"systemctl", "stop", IPS_UNIT, NULL};
    GError *e = NULL;
    if (!_ips_exec(a, IPS_STOP_TIMEOUT_SEC, &e)) {
        PCV_LOG_WARN(IPS_LOG_DOM, "고아 %s 정지 실패(무시하고 부팅 계속): %s",
                     IPS_UNIT, e ? e->message : "unknown");
    }
    g_clear_error(&e);

                                                  
                                                                  
    pcv_prom_gauge_set_labels("purecvisor_suricata_ips_engine_up", "", 0.0);
}

                                            
                                                
                                                        
static gboolean
_ips_restart_unit(GError **error)
{
    gint64 t0 = g_get_monotonic_time();
    const gchar *argv[] = {"systemctl", "restart", IPS_UNIT, NULL};
    GError *e = NULL;
    gboolean ok = _ips_exec(argv, IPS_RESTART_TIMEOUT_SEC, &e);
    gint64 dur = (g_get_monotonic_time() - t0) / 1000;

    if (ok) {
        PCV_LOG_WARN(IPS_LOG_DOM, "suricata-ips FAILED — restarted (%" G_GINT64_FORMAT "ms)", dur);
        pcv_audit_log("system", "suricata.ips.restart", IPS_UNIT, "ok", 0, dur, "local");
    } else {
        PCV_LOG_ERROR(IPS_LOG_DOM, "suricata-ips FAILED — restart failed: %s",
                      e ? e->message : "unknown");
        pcv_audit_log("system", "suricata.ips.restart", IPS_UNIT, "fail",
                      e ? e->code : -1, dur, "local");
    }
    if (e) {
        if (error) g_propagate_error(error, e);
        else       g_clear_error(&e);
    }
    return ok;
}

                                                                              
                                                       
                                                
  
                                                                      
                                                      
                                             
                                                        
                                                         
                                                      
static void
_ips_degrade_to_fail_open(guint queue_num)
{
    if (pcv_suricata_ips_is_degraded())
        return;

                                                       
                                                  
                                                 
    GError *e = NULL;
    gboolean ok = _ips_install_queue_rule(queue_num, TRUE, &e);

    g_mutex_lock(&G_ips.mu);
    G_ips.degraded = TRUE;
    g_mutex_unlock(&G_ips.mu);

    PCV_LOG_ERROR(IPS_LOG_DOM,
        "IPS 재기동 %d회 연속 실패 — fail-open 강등(bypass 규칙 재삽입 %s). 자동 재승격 "
        "없음: 엔진 복구 후 suricata.ips.enable 재호출 필요",
        IPS_RESTART_FAIL_LIMIT, ok ? "성공" : "실패");
    pcv_alert_fire_event("suricata-ips", TRUE, 0,
                         "IPS 재기동 3회 실패 — fail-open 강등");
                                                                
                                                
    pcv_audit_log("system", "suricata.ips.degrade", IPS_UNIT, ok ? "ok" : "fail",
                  ok ? 0 : (e ? e->code : -1), 0, "local");
    g_clear_error(&e);
}

                                                                
                                               
                                                
                                                
                                       
static void
_ips_try_restore_rule(guint queue_num, const gchar *stage)
{
    GError *e = NULL;

                                                                 
    if (!pcv_suricata_ips_wait_ready(IPS_TICK_READY_TIMEOUT_SEC, &e)) {
        G_ips_seq.restart_fail++;
        PCV_LOG_ERROR(IPS_LOG_DOM,
            "fail-closed %s 실패 %u/%d — 큐 규칙 미설치 유지(검사 없는 통과): %s",
            stage, G_ips_seq.restart_fail, IPS_RESTART_FAIL_LIMIT,
            e ? e->message : "unknown");
        g_clear_error(&e);
        if (G_ips_seq.restart_fail >= IPS_RESTART_FAIL_LIMIT)
            _ips_degrade_to_fail_open(queue_num);
        return;
    }
    g_clear_error(&e);

                                                                 
                                                           
                                                          
    if (!pcv_suricata_ips_is_enabled()) {
        PCV_LOG_WARN(IPS_LOG_DOM,
            "%s 중 IPS disable 관측 — 큐 규칙 복원 생략(정지 유닛 + fail-closed 규칙 "
            "조합 = 영구 블랙홀 회피)", stage);
        return;
    }

    GError *re = NULL;
    if (_ips_install_queue_rule(queue_num, FALSE, &re)) {
        G_ips_seq.restart_fail = 0;                    
        PCV_LOG_INFO(IPS_LOG_DOM, "fail-closed 큐 규칙 %s 완료 (queue %u)", stage, queue_num);
    } else {
                                                         
        G_ips_seq.restart_fail++;
        PCV_LOG_ERROR(IPS_LOG_DOM,
            "fail-closed 큐 규칙 %s 실패 %u/%d — 규칙 미설치(통과) 유지: %s",
            stage, G_ips_seq.restart_fail, IPS_RESTART_FAIL_LIMIT,
            re ? re->message : "unknown");
        if (G_ips_seq.restart_fail >= IPS_RESTART_FAIL_LIMIT)
            _ips_degrade_to_fail_open(queue_num);
    }
    g_clear_error(&re);
}

                                                             
                                                  
                                                       
                                                      
                                                     
               
static void
_ips_reconcile(PcvSuricataState s)
{
    g_mutex_lock(&G_ips_seq.mu);

                                                      
                   
    if (!pcv_suricata_ips_is_enabled()) {
        g_mutex_unlock(&G_ips_seq.mu);
        return;
    }

                                                              
                                             
    gboolean fail_open = G_ips_seq.have_snap ? G_ips_seq.fail_open
                                             : pcv_config_get_ips_fail_open();
    guint    queue_num = G_ips_seq.have_snap ? G_ips_seq.queue_num
                                             : (guint)pcv_config_get_ips_queue_num();

                                                   
    gboolean rule_on = FALSE, rule_fo = FALSE;
    _ips_get_rule_state(&rule_on, &rule_fo);

                                                              
                                                              
                                                              
                                                            
                                                                
                                                                   
                                                       
    if (rule_on && !rule_fo && s != PCV_SURICATA_ACTIVE) {
        PCV_LOG_WARN(IPS_LOG_DOM,
            "유닛 상태 %s (리스너 부재)인데 경로에 bypass 없는 큐 규칙이 걸려 있다 — "
            "의도(%s)와 무관하게 즉시 제거(전 forward 트래픽 드롭 차단)",
            pcv_suricata_state_str(s), fail_open ? "fail-open" : "fail-closed");
        _ips_remove_queue_rule();
        rule_on = FALSE;
    }

                                                            
                                                              
                                                              
                                                
    if (fail_open || pcv_suricata_ips_is_degraded()) {
        if (s == PCV_SURICATA_FAILED)
            _ips_restart_unit(NULL);
        g_mutex_unlock(&G_ips_seq.mu);
        return;
    }

                           

                                                                   
                                                         
                                                                         
                                                             
                                                        
                                                            
                                                              
                                         
    if (s != PCV_SURICATA_ACTIVE) {
                                                                
                                                                            
                                                               
                                                                      
                                                        
                                                   
        _ips_remove_queue_rule();

                                                          
                                                        
                                                                       
        if (s != PCV_SURICATA_FAILED) {
            g_mutex_unlock(&G_ips_seq.mu);
            return;
        }

                       
        GError *e = NULL;
        gboolean ok = _ips_restart_unit(&e);
        if (!ok) {
            G_ips_seq.restart_fail++;
            PCV_LOG_ERROR(IPS_LOG_DOM,
                "fail-closed 재기동 실패 %u/%d — 큐 규칙 미설치 유지(검사 없는 통과): %s",
                G_ips_seq.restart_fail, IPS_RESTART_FAIL_LIMIT, e ? e->message : "unknown");
            g_clear_error(&e);
            if (G_ips_seq.restart_fail >= IPS_RESTART_FAIL_LIMIT)
                _ips_degrade_to_fail_open(queue_num);
            g_mutex_unlock(&G_ips_seq.mu);
            return;
        }
        g_clear_error(&e);

                                                            
                                                       
        if (!pcv_suricata_ips_is_enabled()) {
            PCV_LOG_WARN(IPS_LOG_DOM,
                "재기동 직후 IPS disable 관측 — readiness 대기·규칙 복원 생략");
            g_mutex_unlock(&G_ips_seq.mu);
            return;
        }

        _ips_try_restore_rule(queue_num, "재기동");
        g_mutex_unlock(&G_ips_seq.mu);
        return;
    }

                                                                  
                                                           
                                                           
                                                                     
                                          
                                                                        
                                                            
                                                               
                                                         
                 
      
                                                            
                                                                 
                                                                    
                                                        
                                                           
                           
    if (s == PCV_SURICATA_ACTIVE && !rule_on) {
        PCV_LOG_WARN(IPS_LOG_DOM,
            "유닛 ACTIVE 이나 fail-closed 큐 규칙 부재 — 재조정 시도 (queue %u)", queue_num);
        _ips_try_restore_rule(queue_num, "재조정");
    }

    g_mutex_unlock(&G_ips_seq.mu);
}

                                                         
                                                 
                                                  
                     
  
                                                        
                                                           
                                                                    
                                                    
                               
  
                                                  
                                                  
                                                           
                                                      
                                                
                                                            
                                                    
                                                        
                                         
  
                                                           
                                                             
                                                     
                                                     
static void
_ips_nonenforce_watch(PcvSuricataState s)
{
    gboolean rule_on = FALSE, rule_fo = FALSE;
    _ips_get_rule_state(&rule_on, &rule_fo);
    gboolean engine_up = (s == PCV_SURICATA_ACTIVE);

                                                        
                                                                 
    g_mutex_lock(&G_ips_seq.mu);
    gboolean fail_open_intent = G_ips_seq.have_snap ? G_ips_seq.fail_open
                                                    : pcv_config_get_ips_fail_open();
    g_mutex_unlock(&G_ips_seq.mu);

    gboolean degraded = pcv_suricata_ips_is_degraded();                         

                                                                
                                                           
    guint fire_nonenforce = 0, fire_mitigation = 0;

    g_mutex_lock(&G_ips_watch_mu);
               
    if (!engine_up || !rule_on) {
        if (++G_ips_watch.nonenforce_ticks >= IPS_NONENFORCE_ALERT_TICKS
            && !G_ips_watch.nonenforce_fired) {
            G_ips_watch.nonenforce_fired = TRUE;
            fire_nonenforce = G_ips_watch.nonenforce_ticks;
        }
    } else {
        G_ips_watch.nonenforce_ticks = 0;
        G_ips_watch.nonenforce_fired = FALSE;                         
    }

                  
    if (fail_open_intent && rule_on && !rule_fo && engine_up) {
        if (++G_ips_watch.mitigation_ticks >= IPS_NONENFORCE_ALERT_TICKS
            && !G_ips_watch.mitigation_fired) {
            G_ips_watch.mitigation_fired = TRUE;
            fire_mitigation = G_ips_watch.mitigation_ticks;
        }
    } else {
        G_ips_watch.mitigation_ticks = 0;
        G_ips_watch.mitigation_fired = FALSE;
    }
    g_mutex_unlock(&G_ips_watch_mu);

    if (fire_nonenforce) {
        gchar *msg = g_strdup_printf(
            "IPS 비집행 %u tick 지속 — enabled=1 engine_up=%d "
            "queue_rule_installed=%d degraded=%d (검사 없이 통과 중). "
            "자동 재기동은 하지 않는 설계이므로 수동 조치 필요: "
            "systemctl status %s 확인 후 suricata.ips.enable 재호출",
            fire_nonenforce, engine_up ? 1 : 0, rule_on ? 1 : 0,
            degraded ? 1 : 0, IPS_UNIT);
        PCV_LOG_ERROR(IPS_LOG_DOM, "%s", msg);
        pcv_alert_fire_event(IPS_LOG_DOM, TRUE, 0, msg);
        g_free(msg);
    }
    if (fire_mitigation) {
        gchar *msg = g_strdup_printf(
            "IPS 완화 미반영 %u tick 지속 — 의도는 fail-open 인데 경로에는 "
            "bypass 없는 규칙이 걸려 있고 엔진이 active 라 그대로 집행 중이다. "
            "suricata.ips.enable(fail_open=true) 재호출로 규칙을 다시 깔 것",
            fire_mitigation);
        PCV_LOG_ERROR(IPS_LOG_DOM, "%s", msg);
        pcv_alert_fire_event(IPS_LOG_DOM, TRUE, 0, msg);
        g_free(msg);
    }
}

                                                
                                                        
void
pcv_suricata_ips_health_tick(void)
{
    if (!pcv_suricata_ips_is_enabled()) {
                                                 
        pcv_prom_gauge_set_labels("purecvisor_suricata_ips_engine_up", "", 0.0);
        pcv_prom_gauge_set_labels("purecvisor_suricata_ips_enabled", "", 0.0);
        pcv_prom_gauge_set_labels("purecvisor_suricata_ips_failopen_degraded", "",
                                  pcv_suricata_ips_is_degraded() ? 1.0 : 0.0);
        pcv_prom_gauge_set_labels("purecvisor_suricata_ips_queue_rule_installed", "",
                                  _ips_rule_installed() ? 1.0 : 0.0);
                                                                
                                                             
                        
        _ips_watch_reset();
        return;
    }

                                                   
    PcvSuricataState s = _ips_probe();
    pcv_suricata_ips_probe_cache_store(s);

                                                                   
    _ips_reconcile(s);

    pcv_suricata_ips_metrics_tick();

                                                              
                                       
    _ips_nonenforce_watch(s);
}
