                                  
                                                                             
  
                           
                                                   
                                                    
                                        
  
                                             
                                                  
                                              
                                              
                                             
                                             
                               
  
                                                  
                                                 
                                                        
                                                         
                                                  
                                                    
                                         
                                                                
                                                    
                                                             
                                           
                              
  
                                                         
                                                                
   
#include "modules/daemons/pcv_trace.h"
#include "modules/audit/pcv_audit.h"                                               
#include "modules/network/tenant_overlay.h"                                   
#include "utils/pcv_validate.h"
#include "utils/pcv_spawn.h"
#include "utils/pcv_log.h"                                            
#include "utils/pcv_config.h"                                                     
#include <gio/gio.h>                                                                             
#include <glib/gstdio.h>                                              
#include <ftw.h>                                                                  
#include <string.h>                                            

                                                         
                                                     
                
                                                                 
                                                                           
                                                            
                                                    
                                                                 
                                                       
                                                                          
                                                       
                                                         
                                                       
                  
#define TRACE_RETIS_COLLECTORS "skb,skb-drop,nft,ct,skb-tracking"
#define TRACE_RETIS_NFT_VERDICTS "drop"

#define TRACE_LOG_DOM "pcv_trace"

                                                          
                                                   
                                                                      
#define TRACE_SELFCHECK_TIMEOUT_SEC 5u

                                                                    
                                                  
                                               
                                                         
                      
#define TRACE_BACKSTOP_GRACE_SEC 10u

   
                                              
  
                                              
                                                  
                                       
                                                                    
                                     
   
gboolean
pcv_trace_filter_validate(const PcvTraceFilter *f, GError **error)
{
    if (!f) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "filter is NULL");
        return FALSE;
    }

                                        
    if (f->timebox_sec < 1 || f->timebox_sec > PCV_TRACE_MAX_TIMEBOX_SEC) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "timebox_sec must be 1..%u (got %u)",
                    (guint)PCV_TRACE_MAX_TIMEBOX_SEC, f->timebox_sec);
        return FALSE;
    }

                                                       
    if (f->vm[0] != '\0' && !pcv_validate_vm_name(f->vm)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "invalid vm name '%s'", f->vm);
        return FALSE;
    }
    if (f->tenant[0] != '\0' && !pcv_validate_vm_name(f->tenant)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "invalid tenant name '%s'", f->tenant);
        return FALSE;
    }

                                                                 
                                                   
    if (f->proto[0] != '\0' && !pcv_validate_l4_proto(f->proto)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "invalid proto '%s' (tcp|udp|icmp only)", f->proto);
        return FALSE;
    }

                                                                  
                                                          
                                              
    if (f->src_ip[0] != '\0' && !pcv_validate_ip_literal(f->src_ip)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "invalid src_ip '%s'", f->src_ip);
        return FALSE;
    }
    if (f->dst_ip[0] != '\0' && !pcv_validate_ip_literal(f->dst_ip)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "invalid dst_ip '%s'", f->dst_ip);
        return FALSE;
    }

                                                      
                                                                             
                                               
    if (f->src_port != 0 && !pcv_validate_port((gint)f->src_port)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "invalid src_port %u (1..65535)", f->src_port);
        return FALSE;
    }
    if (f->dst_port != 0 && !pcv_validate_port((gint)f->dst_port)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "invalid dst_port %u (1..65535)", f->dst_port);
        return FALSE;
    }

    return TRUE;
}

   
                                              
  
                                                    
                                                             
                                                 
                   
                                                                   
   
gchar *
pcv_trace_build_pcap_expr(const PcvTraceFilter *f)
{
    if (!f) return NULL;

                                                                 
                                                  
    gchar *clauses[6] = { NULL };
    guint  n = 0;

    if (f->proto[0] != '\0')
        clauses[n++] = g_strdup(f->proto);
    if (f->src_ip[0] != '\0')
        clauses[n++] = g_strdup_printf("src host %s", f->src_ip);
    if (f->dst_ip[0] != '\0')
        clauses[n++] = g_strdup_printf("dst host %s", f->dst_ip);
    if (f->src_port != 0) {
        clauses[n++] = (f->proto[0] != '\0')
            ? g_strdup_printf("%s src port %u", f->proto, f->src_port)
            : g_strdup_printf("src port %u", f->src_port);
    }
    if (f->dst_port != 0) {
        clauses[n++] = (f->proto[0] != '\0')
            ? g_strdup_printf("%s dst port %u", f->proto, f->dst_port)
            : g_strdup_printf("dst port %u", f->dst_port);
    }

    if (n == 0) return NULL;                                   

    clauses[n] = NULL;                                  
    gchar *expr = g_strjoinv(" and ", clauses);
    for (guint i = 0; i < n; i++) g_free(clauses[i]);
    return expr;
}

   
                                                                   
  
                                               
                                            
                                                 
                                                                        
   
gchar **
pcv_trace_build_argv(const PcvTraceFilter *f, const char *out_dir)
{
    if (!f || !out_dir) return NULL;

    GPtrArray *argv = g_ptr_array_new();
    g_ptr_array_add(argv, g_strdup("retis"));
    g_ptr_array_add(argv, g_strdup("collect"));
    g_ptr_array_add(argv, g_strdup("-c"));
    g_ptr_array_add(argv, g_strdup(TRACE_RETIS_COLLECTORS));
    g_ptr_array_add(argv, g_strdup("--nft-verdicts"));
    g_ptr_array_add(argv, g_strdup(TRACE_RETIS_NFT_VERDICTS));

    gchar *pcap_expr = pcv_trace_build_pcap_expr(f);
    if (pcap_expr) {
        g_ptr_array_add(argv, g_strdup("-f"));
        g_ptr_array_add(argv, pcap_expr);                             
    }

    g_ptr_array_add(argv, g_strdup("--allow-system-changes"));
    g_ptr_array_add(argv, g_strdup("-o"));
    g_ptr_array_add(argv, g_strdup_printf("%s/run.data", out_dir));
                                                         
                                                      
                                               
    g_ptr_array_add(argv, g_strdup("--cmd"));
    g_ptr_array_add(argv, g_strdup_printf("sleep %u", f->timebox_sec));

    g_ptr_array_add(argv, NULL);
    return (gchar **)g_ptr_array_free(argv, FALSE);
}

   
                                                                    
                                                
  
                                              
                                              
                                         
  
                                                              
                                                           
                                                              
                                                        
                                                                  
                                                  
                                                          
                                                                  
                  
                                                            
                                                                      
                                                
                                              
                                                                    
                                                        
                                                     
                        
   
gboolean
pcv_trace_selfcheck(GError **error)
{
    gchar *path = g_find_program_in_path("retis");
    if (!path) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "retis binary not found in PATH — trace capture degraded "
                    "(retis 설치 필요, D10 T5 packaging 참고)");
        return FALSE;
    }
    g_free(path);

    const gchar *argv[] = { "retis", "--version", NULL };
    GError *spawn_err = NULL;
    if (!pcv_spawn_sync_timeout(argv, NULL, NULL, TRACE_SELFCHECK_TIMEOUT_SEC, &spawn_err)) {
                                                           
                                                            
                                  
        g_propagate_prefixed_error(error, spawn_err,
            "retis --version self-check failed — trace capture degraded: ");
        return FALSE;
    }

    return TRUE;
}

                                                                             
                         
  
                                              
                                                                            
                                                            
                                                        
                                                                                

                                                                           
                                                
static gint g_trace_inflight = 0;

                                               
                                              
gboolean
pcv_trace_try_acquire(void)
{
    return g_atomic_int_compare_and_exchange(&g_trace_inflight, 0, 1);
}

                                               
void
pcv_trace_release(void)
{
    g_atomic_int_set(&g_trace_inflight, 0);
}

                                                     
typedef struct {
    gchar       *trace_id;
    gchar       *out_dir;
    gchar       *admin;
    gchar       *vm;                                       
    gchar       *filter_desc;                                         
    guint        timebox_sec;
    gint64       started_at;               
    gboolean     overlay;
    GSubprocess *proc;                             
    guint        backstop_id;                                
    gboolean     finalized;                     
} TraceState;

static TraceState *g_trace = NULL;

                                             
               
static void
_trace_state_free(TraceState *s)
{
    if (!s) return;
    g_free(s->trace_id);
    g_free(s->out_dir);
    g_free(s->admin);
    g_free(s->vm);
    g_free(s->filter_desc);
    g_clear_object(&s->proc);
    g_free(s);
}

                                                                
                                                                       
                                                           
                                           
                                             
                                              
                     
static gchar **
_wrap_nsenter(gchar **retis_argv, const gchar *ep)
{
    GPtrArray *a = g_ptr_array_new();
    g_ptr_array_add(a, g_strdup("nsenter"));
    g_ptr_array_add(a, g_strdup_printf("--net=/run/netns/%s", ep));
    g_ptr_array_add(a, g_strdup("--"));
    for (guint i = 0; retis_argv[i]; i++)
        g_ptr_array_add(a, g_strdup(retis_argv[i]));
    g_ptr_array_add(a, NULL);
    return (gchar **)g_ptr_array_free(a, FALSE);
}

                                                     
                                                     
                                                                
                                                               
                                           
                                               
                                                     
static gchar *
_trace_summarize(const TraceState *s, const gchar *reason, gboolean *ok_out)
{
    gboolean exited = s->proc && g_subprocess_get_if_exited(s->proc);
    gint     code   = exited ? g_subprocess_get_exit_status(s->proc) : -1;
    gboolean ok     = exited && code == 0;

    gchar   *data_path = g_build_filename(s->out_dir, "run.data", NULL);
    GStatBuf st;
    gint64   size = (g_stat(data_path, &st) == 0) ? (gint64)st.st_size : -1;

    gchar *detail = g_strdup_printf(
        "%s reason=%s exited=%d exit=%d data=%s size=%" G_GINT64_FORMAT,
        ok ? "ok" : "incomplete",
        reason, exited, code, data_path, size);
    g_free(data_path);
    if (ok_out) *ok_out = ok;
    return detail;
}

                                                      
                                                                      
                                         
                                              
                                           
                                                    
static void
_trace_finalize(const gchar *method, const gchar *reason)
{
    if (!g_trace || g_trace->finalized) return;
    g_trace->finalized = TRUE;

                                            
                                                             
    if (g_trace->backstop_id) {
        g_source_remove(g_trace->backstop_id);
        g_trace->backstop_id = 0;
    }

    gboolean ok;
    gchar *detail = _trace_summarize(g_trace, reason, &ok);
                                                                      
                                                 
                                                                  
                                                         
               
    pcv_audit_log(g_trace->admin, method, g_trace->vm, ok ? "ok" : "fail",
                  ok ? 0 : -1, 0, "local");
    PCV_LOG_INFO(TRACE_LOG_DOM, "trace %s (%s): %s",
                 g_trace->trace_id, method, detail);
    g_free(detail);

                                                  
                                      
    gchar *marker = g_build_filename(g_trace->out_dir, PCV_TRACE_RUNNING_MARKER, NULL);
    if (g_remove(marker) != 0)
        PCV_LOG_WARN(TRACE_LOG_DOM, "'.running' 마커 제거 실패: %s", marker);
    g_free(marker);

    TraceState *dead = g_trace;
    g_trace = NULL;
    _trace_state_free(dead);
    pcv_trace_release();
}

                                                          
                                         
                                             
                                                   
static void
_trace_wait_done(GObject *src, GAsyncResult *res, gpointer user)
{
    gchar       *id   = user;
    GSubprocess *proc = G_SUBPROCESS(src);
    GError      *werr = NULL;
    g_subprocess_wait_finish(proc, res, &werr);                      
    g_clear_error(&werr);

    if (g_trace && g_strcmp0(g_trace->trace_id, id) == 0 && !g_trace->finalized)
        _trace_finalize("debug.trace.stop", "exit");
    g_free(id);
}

                                                    
                                            
                        
static gboolean
_trace_backstop_cb(gpointer user)
{
    const gchar *id = user;                                                 

    if (g_trace && g_strcmp0(g_trace->trace_id, id) == 0 && !g_trace->finalized) {
        g_trace->backstop_id = 0;                                                         
        PCV_LOG_WARN(TRACE_LOG_DOM,
            "trace %s: 백스톱 발화(timebox+%us 초과) — 강제 종료",
            g_trace->trace_id, TRACE_BACKSTOP_GRACE_SEC);
        if (g_trace->proc) g_subprocess_force_exit(g_trace->proc);               
        _trace_finalize("debug.trace.expire", "timebox");
                                                             
                                         
    }
    return G_SOURCE_REMOVE;           
}

   
                                                         
               
  
                                              
                                                   
                                                 
                                          
  
                                                             
                                                        
   
gchar *
pcv_trace_start(const PcvTraceFilter *f, const char *admin, GError **error)
{
                                                               
                                                         
                                                      
                                                               
    if (!pcv_trace_selfcheck(error)) {                                   
        pcv_audit_log(admin, "debug.trace.start", (f && f->vm[0]) ? f->vm : "",
                      "fail", -1, 0, "local");
        return NULL;
    }
    if (!pcv_trace_filter_validate(f, error)) {                    
        pcv_audit_log(admin, "debug.trace.start", (f && f->vm[0]) ? f->vm : "",
                      "fail", -1, 0, "local");
        return NULL;
    }

                                    
    if (!pcv_trace_try_acquire()) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                    "trace already running (동시 1개)");
        PCV_LOG_WARN(TRACE_LOG_DOM,
            "trace start 거부: 이미 실행 중(동시 1개) — admin=%s vm=%s",
            admin ? admin : "-", f->vm);
                                                               
                                                     
        pcv_audit_log(admin, "debug.trace.start", f->vm[0] ? f->vm : "",
                      "fail", -1, 0, "local");
        return NULL;
    }

                                              
    gchar   *trace_id = g_strdup_printf("%" G_GINT64_FORMAT "-%04x",
                                        g_get_real_time(),
                                        (guint)g_random_int_range(0, 0x10000));
    gchar   *out_dir  = g_build_filename(PCV_TRACE_OUT_ROOT, trace_id, NULL);
    gchar  **retis_argv = NULL;
    gchar  **final_argv = NULL;
    gchar   *ep = NULL;
    gchar   *marker = NULL;

    if (g_mkdir_with_parents(out_dir, 0700) != 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "trace out_dir 생성 실패: %s", out_dir);
        goto fail;
    }

                                                   
    marker = g_build_filename(out_dir, PCV_TRACE_RUNNING_MARKER, NULL);
    if (!g_file_set_contents(marker, "", 0, error))
        goto fail;

    retis_argv = pcv_trace_build_argv(f, out_dir);                           
    if (!retis_argv) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "build_argv 실패");
        goto fail;
    }

                                                                   
                                                               
    if (f->overlay) {
        if (!pcv_tenant_overlay_get_member_ep(f->tenant, f->vm, &ep) || !ep) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                        "overlay 캡처 요청이나 ep netns 해석 실패 "
                        "(tenant='%s' vm='%s' 가 오버레이 멤버가 아님)",
                        f->tenant, f->vm);
            goto fail;
        }
        final_argv = _wrap_nsenter(retis_argv, ep);             
        g_strfreev(retis_argv);
        retis_argv = NULL;
    } else {
        final_argv = retis_argv;
        retis_argv = NULL;                             
    }

                                                                     
                                                         
                                                      
    GError      *serr = NULL;
    GSubprocess *proc = pcv_spawn_newv(
        (const gchar * const *)final_argv,
        G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
        &serr);
    if (!proc) {
        g_propagate_prefixed_error(error, serr, "retis spawn 실패: ");
        goto fail;
    }

                
    gchar *pcap = pcv_trace_build_pcap_expr(f);
    g_trace = g_new0(TraceState, 1);
    g_trace->trace_id    = g_strdup(trace_id);
    g_trace->out_dir     = g_strdup(out_dir);
                                                               
                                                    
                                                
    g_trace->admin       = g_strdup(admin && *admin ? admin : "-");
    g_trace->vm          = g_strdup(f->vm);
    g_trace->filter_desc = pcap ? pcap : g_strdup("(none)");
    g_trace->timebox_sec = f->timebox_sec;
    g_trace->started_at  = g_get_real_time() / G_USEC_PER_SEC;
    g_trace->overlay     = f->overlay;
    g_trace->proc        = proc;              
    g_trace->finalized   = FALSE;

                                                         
                                           
    g_subprocess_wait_async(proc, NULL, _trace_wait_done, g_strdup(trace_id));
    g_trace->backstop_id = g_timeout_add_seconds_full(
        G_PRIORITY_DEFAULT, f->timebox_sec + TRACE_BACKSTOP_GRACE_SEC,
        _trace_backstop_cb, g_strdup(trace_id), g_free);

    gchar *start_detail = g_strdup_printf(
        "ok timebox_sec=%u filter=\"%s\" overlay=%d out_dir=%s",
        f->timebox_sec, g_trace->filter_desc, f->overlay, out_dir);
                                                               
                                           
    pcv_audit_log(admin, "debug.trace.start", f->vm, "ok", 0, 0, "local");
    PCV_LOG_INFO(TRACE_LOG_DOM, "trace %s start: %s", trace_id, start_detail);
    g_free(start_detail);

                                                 
                                                    
                                                             
                                                     
                                                 
    pcv_trace_retention_apply();

    g_free(out_dir);
    g_free(marker);
    g_free(ep);
    g_strfreev(final_argv);
                                 
    return trace_id;

fail:
                                      
    if (marker) { g_remove(marker); g_free(marker); }
    if (out_dir) { g_remove(out_dir); }                         
    pcv_audit_log(admin, "debug.trace.start", f && f->vm[0] ? f->vm : "",
                  "fail", -1, 0, "local");
    g_free(trace_id);
    g_free(out_dir);
    g_free(ep);
    if (retis_argv) g_strfreev(retis_argv);
    if (final_argv) g_strfreev(final_argv);
    pcv_trace_release();
    return NULL;
}

                                                
                                              
                                                                                   
gboolean
pcv_trace_stop(const char *trace_id, GError **error)
{
    if (!trace_id || !*trace_id) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "trace_id must be non-empty");
        return FALSE;
    }
    if (!g_trace || g_strcmp0(g_trace->trace_id, trace_id) != 0 || g_trace->finalized) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "no active trace with id '%s'", trace_id);
        return FALSE;
    }

    if (g_trace->proc) g_subprocess_force_exit(g_trace->proc);               
    _trace_finalize("debug.trace.stop", "stop");
    return TRUE;
}

                                                 
                                                            
                                           
JsonObject *
pcv_trace_status(const char *trace_id)
{
    JsonObject *o = json_object_new();

                                                         
    gboolean match = g_trace &&
        (!trace_id || !*trace_id || g_strcmp0(g_trace->trace_id, trace_id) == 0);

    if (!match) {
        json_object_set_string_member(o, "state", "idle");
        return o;
    }

                                                              
                                                                
                                                  
                                                             
                                                   
    json_object_set_string_member(o, "state", "running");
    json_object_set_string_member(o, "trace_id", g_trace->trace_id);
    json_object_set_string_member(o, "vm", g_trace->vm ? g_trace->vm : "");
    json_object_set_string_member(o, "filter", g_trace->filter_desc);
    json_object_set_string_member(o, "out_dir", g_trace->out_dir);
    json_object_set_int_member(o, "timebox_sec", g_trace->timebox_sec);
    json_object_set_int_member(o, "started_at", g_trace->started_at);
    json_object_set_boolean_member(o, "overlay", g_trace->overlay);
    return o;
}

                                                                             
                                 
  
                                                     
               
                                                                                

                                      
static void
_retention_entry_free(PcvTraceRetentionEntry *e)
{
    if (!e) return;
    g_free(e->id);
    g_free(e);
}

                                                         
                                  
                                            
static gint
_cmp_mtime_asc(gconstpointer a, gconstpointer b)
{
    const PcvTraceRetentionEntry *ea = *(const PcvTraceRetentionEntry * const *)a;
    const PcvTraceRetentionEntry *eb = *(const PcvTraceRetentionEntry * const *)b;
    if (ea->mtime < eb->mtime) return -1;
    if (ea->mtime > eb->mtime) return 1;
    return 0;
}

   
                                                             
                                                  
                                           
            
  
                                                  
                                                
   
GPtrArray *
pcv_trace_retention_evict(GPtrArray *entries, guint max_count, guint64 max_bytes)
{
    GPtrArray *evicted = g_ptr_array_new_with_free_func(g_free);
    if (!entries || entries->len == 0) return evicted;

                                                   
                                               
    GPtrArray *sorted = g_ptr_array_sized_new(entries->len);
    for (guint i = 0; i < entries->len; i++)
        g_ptr_array_add(sorted, g_ptr_array_index(entries, i));
    g_ptr_array_sort(sorted, _cmp_mtime_asc);

                                 
    guint start = 0;
    if (sorted->len > max_count) {
        guint over = sorted->len - max_count;
        for (guint i = 0; i < over; i++) {
            PcvTraceRetentionEntry *e = g_ptr_array_index(sorted, i);
            g_ptr_array_add(evicted, g_strdup(e->id));
        }
        start = over;
    }

                                               
                                                         
    guint64 remaining_bytes = 0;
    for (guint i = start; i < sorted->len; i++) {
        PcvTraceRetentionEntry *e = g_ptr_array_index(sorted, i);
        remaining_bytes += e->bytes;
    }
    guint i = start;
    while (remaining_bytes > max_bytes && i < sorted->len) {
        PcvTraceRetentionEntry *e = g_ptr_array_index(sorted, i);
        g_ptr_array_add(evicted, g_strdup(e->id));
        remaining_bytes -= e->bytes;
        i++;
    }

    g_ptr_array_free(sorted, TRUE);                                    
    return evicted;
}

                                                        
                                             
static guint64 g_trace_scan_bytes = 0;

                                                    
static int
_trace_size_cb(const char *fpath __attribute__((unused)),
               const struct stat *sb,
               int typeflag,
               struct FTW *ftwbuf __attribute__((unused)))
{
    if (typeflag == FTW_F) g_trace_scan_bytes += (guint64)sb->st_size;
    return 0;
}

                                                            
                                      
                                                 
static int
_pcv_trace_recursive_unlink_cb(const char *fpath,
                               const struct stat *sb __attribute__((unused)),
                               int typeflag,
                               struct FTW *ftwbuf __attribute__((unused)))
{
    if (typeflag == FTW_DP || typeflag == FTW_D) {
        return rmdir(fpath);
    }
    return unlink(fpath);
}

                                            
                                                     
                                                   
void
pcv_trace_retention_apply_root(const gchar *root)
{
    if (!root) return;

    GDir *dir = g_dir_open(root, 0, NULL);
    if (!dir) return;                                            

    GPtrArray *entries = g_ptr_array_new_with_free_func((GDestroyNotify)_retention_entry_free);
    const gchar *name;
    while ((name = g_dir_read_name(dir)) != NULL) {
        gchar *path = g_build_filename(root, name, NULL);
        if (!g_file_test(path, G_FILE_TEST_IS_DIR)) { g_free(path); continue; }

                                                  
        if (g_trace && g_strcmp0(g_trace->trace_id, name) == 0) { g_free(path); continue; }

                                                        
                                           
        gchar *marker = g_build_filename(path, PCV_TRACE_RUNNING_MARKER, NULL);
        gboolean running = g_file_test(marker, G_FILE_TEST_EXISTS);
        g_free(marker);
        if (running) { g_free(path); continue; }

        GStatBuf st;
        if (g_stat(path, &st) != 0) { g_free(path); continue; }

        g_trace_scan_bytes = 0;
        nftw(path, _trace_size_cb, 16, FTW_PHYS);

        PcvTraceRetentionEntry *e = g_new0(PcvTraceRetentionEntry, 1);
        e->id    = g_strdup(name);
        e->mtime = (gint64)st.st_mtime;
        e->bytes = g_trace_scan_bytes;
        g_ptr_array_add(entries, e);

        g_free(path);
    }
    g_dir_close(dir);

    gint retain_count_cfg = pcv_config_get_int("trace", "retain_count",
                                                PCV_TRACE_DEFAULT_RETAIN_COUNT);
    if (retain_count_cfg <= 0) {
        static gboolean warned_count = FALSE;
        if (!warned_count) {
            warned_count = TRUE;
            PCV_LOG_WARN(TRACE_LOG_DOM,
                "trace.retain_count=%d 는 유효하지 않음(1 이상 필요) — 기본값 %d 로 대체",
                retain_count_cfg, PCV_TRACE_DEFAULT_RETAIN_COUNT);
        }
        retain_count_cfg = PCV_TRACE_DEFAULT_RETAIN_COUNT;
    }

    gint retain_mb_cfg = pcv_config_get_int("trace", "retain_mb",
                                             PCV_TRACE_DEFAULT_RETAIN_MB);
    if (retain_mb_cfg <= 0) {
        static gboolean warned_mb = FALSE;
        if (!warned_mb) {
            warned_mb = TRUE;
            PCV_LOG_WARN(TRACE_LOG_DOM,
                "trace.retain_mb=%d 는 유효하지 않음(1 이상 필요) — 기본값 %d 로 대체",
                retain_mb_cfg, PCV_TRACE_DEFAULT_RETAIN_MB);
        }
        retain_mb_cfg = PCV_TRACE_DEFAULT_RETAIN_MB;
    }

                                                      
    guint64 max_bytes = (guint64)retain_mb_cfg * 1024 * 1024;

    GPtrArray *evict_ids = pcv_trace_retention_evict(entries, (guint)retain_count_cfg, max_bytes);
    for (guint idx = 0; idx < evict_ids->len; idx++) {
        const gchar *id = g_ptr_array_index(evict_ids, idx);
        gchar *victim = g_build_filename(root, id, NULL);
        if (nftw(victim, _pcv_trace_recursive_unlink_cb, 16, FTW_DEPTH | FTW_PHYS) != 0)
            PCV_LOG_WARN(TRACE_LOG_DOM, "trace retention evict 실패: %s", victim);
        else
            PCV_LOG_INFO(TRACE_LOG_DOM, "trace retention evict: %s", victim);
        g_free(victim);
    }

    g_ptr_array_unref(evict_ids);
    g_ptr_array_unref(entries);
}

                                          
void
pcv_trace_retention_apply(void)
{
    pcv_trace_retention_apply_root(PCV_TRACE_OUT_ROOT);
}

                                                                             
                                            
  
                                                     
                                    
                                                                                

                                                                         

                                             
                                    
JsonArray *
pcv_trace_list_root(const gchar *root)
{
    JsonArray *arr = json_array_new();
    if (!root) return arr;

    GDir *dir = g_dir_open(root, 0, NULL);
    if (!dir) return arr;                               

    const gchar *name;
    while ((name = g_dir_read_name(dir)) != NULL) {
        gchar *path = g_build_filename(root, name, NULL);
        if (!g_file_test(path, G_FILE_TEST_IS_DIR)) { g_free(path); continue; }

        GStatBuf st;
        if (g_stat(path, &st) != 0) { g_free(path); continue; }

                                                            
                                           
        g_trace_scan_bytes = 0;
        nftw(path, _trace_size_cb, 16, FTW_PHYS);

        gchar   *marker  = g_build_filename(path, PCV_TRACE_RUNNING_MARKER, NULL);
        gboolean running = g_file_test(marker, G_FILE_TEST_EXISTS);
        g_free(marker);

                                                     
                                                    
        gboolean active = (g_trace && g_strcmp0(g_trace->trace_id, name) == 0);

        JsonObject *o = json_object_new();
        json_object_set_string_member(o, "trace_id", name);
        json_object_set_int_member(o, "mtime", (gint64)st.st_mtime);
        json_object_set_int_member(o, "bytes", (gint64)g_trace_scan_bytes);
        json_object_set_boolean_member(o, "running", running);
        json_object_set_boolean_member(o, "active", active);
        json_array_add_object_element(arr, o);

        g_free(path);
    }
    g_dir_close(dir);
    return arr;
}

                                              
JsonArray *
pcv_trace_list(void)
{
    return pcv_trace_list_root(PCV_TRACE_OUT_ROOT);
}

                                                                     

#define TRACE_REPORT_TOP_N 10u                              

typedef struct {
    gchar *table;
    gchar *chain;
    gchar *handle;
    guint  count;
} NftDropAgg;

typedef struct {
    gchar *reason;
    guint  count;
} ReasonAgg;

                                   
static void
_nft_agg_free(gpointer p)
{
    NftDropAgg *a = p;
    if (!a) return;
    g_free(a->table);
    g_free(a->chain);
    g_free(a->handle);
    g_free(a);
}

                                       
static void
_reason_agg_free(gpointer p)
{
    ReasonAgg *a = p;
    if (!a) return;
    g_free(a->reason);
    g_free(a);
}

                                                           
                                                                          
                                               
                 
                                                  
                                              
static gchar *
_report_norm_tok(const gchar *t)
{
    while (*t == '(' || *t == '[') t++;
    gsize n = strlen(t);
    while (n > 0 && (t[n - 1] == ')' || t[n - 1] == ']' ||
                     t[n - 1] == ',' || t[n - 1] == '.'))
        n--;
    return g_strndup(t, n);
}

                                                           
                                                 
                                             
static gint
_cmp_nft_agg(gconstpointer a, gconstpointer b)
{
    const NftDropAgg *x = *(const NftDropAgg * const *)a;
    const NftDropAgg *y = *(const NftDropAgg * const *)b;
    if (x->count != y->count) return (x->count < y->count) ? 1 : -1;            
    gint c = g_strcmp0(x->table, y->table);
    if (c) return c;
    c = g_strcmp0(x->chain, y->chain);
    if (c) return c;
    return g_strcmp0(x->handle, y->handle);
}

                                                    
                      
static gint
_cmp_reason_agg(gconstpointer a, gconstpointer b)
{
    const ReasonAgg *x = *(const ReasonAgg * const *)a;
    const ReasonAgg *y = *(const ReasonAgg * const *)b;
    if (x->count != y->count) return (x->count < y->count) ? 1 : -1;            
    return g_strcmp0(x->reason, y->reason);
}

                                                    
                                                        
                                              
                                                                               
JsonObject *
pcv_trace_report_parse(const gchar *print_output)
{
    JsonObject *root = json_object_new();
                                                                        
    GHashTable *nft = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, _nft_agg_free);
    GHashTable *reasons = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, _reason_agg_free);
    guint total_events = 0;

    if (print_output && *print_output) {
        gchar **lines = g_strsplit(print_output, "\n", -1);
        for (guint li = 0; lines[li] != NULL; li++) {
            gchar **raw = g_strsplit_set(lines[li], " \t\r", -1);

                                       
            GPtrArray *norm = g_ptr_array_new_with_free_func(g_free);
            for (guint i = 0; raw[i] != NULL; i++) {
                if (raw[i][0] == '\0') continue;
                gchar *nt = _report_norm_tok(raw[i]);
                if (nt[0] == '\0') { g_free(nt); continue; }
                g_ptr_array_add(norm, nt);
            }
            g_strfreev(raw);

            const gchar *table = NULL, *chain = NULL, *handle = NULL, *reason = NULL;
            gboolean has_drop = FALSE, has_reason = FALSE;
            for (guint i = 0; i < norm->len; i++) {
                const gchar *tok = g_ptr_array_index(norm, i);
                gboolean has_next = (i + 1 < norm->len);
                if (g_ascii_strcasecmp(tok, "table") == 0 && has_next)
                    table = g_ptr_array_index(norm, i + 1);
                else if (g_ascii_strcasecmp(tok, "chain") == 0 && has_next)
                    chain = g_ptr_array_index(norm, i + 1);
                else if (g_ascii_strcasecmp(tok, "handle") == 0 && has_next)
                    handle = g_ptr_array_index(norm, i + 1);
                else if (g_ascii_strcasecmp(tok, "drop") == 0)
                    has_drop = TRUE;
                else if (g_ascii_strcasecmp(tok, "reason") == 0 && has_next) {
                    has_reason = TRUE;
                    reason = g_ptr_array_index(norm, i + 1);
                }
            }

            gboolean matched = FALSE;

                                                                   
            if (table && chain && handle && has_drop) {
                gchar *key = g_strdup_printf("%s\x1f%s\x1f%s", table, chain, handle);
                NftDropAgg *a = g_hash_table_lookup(nft, key);
                if (a) {
                    a->count++;
                    g_free(key);
                } else {
                    a = g_new0(NftDropAgg, 1);
                    a->table  = g_strdup(table);
                    a->chain  = g_strdup(chain);
                    a->handle = g_strdup(handle);
                    a->count  = 1;
                    g_hash_table_insert(nft, key, a);                  
                }
                matched = TRUE;
            }

                                      
            if (has_reason && reason) {
                ReasonAgg *a = g_hash_table_lookup(reasons, reason);
                if (a) {
                    a->count++;
                } else {
                    a = g_new0(ReasonAgg, 1);
                    a->reason = g_strdup(reason);
                    a->count  = 1;
                    g_hash_table_insert(reasons, g_strdup(reason), a);
                }
                matched = TRUE;
            }

            if (matched) total_events++;                     

            g_ptr_array_unref(norm);
        }
        g_strfreev(lines);
    }

    gboolean truncated = FALSE;

                                                           
    GPtrArray *nft_sorted = g_ptr_array_new();                                  
    {
        GHashTableIter it;
        gpointer k, v;
        g_hash_table_iter_init(&it, nft);
        while (g_hash_table_iter_next(&it, &k, &v))
            g_ptr_array_add(nft_sorted, v);
    }
    g_ptr_array_sort(nft_sorted, _cmp_nft_agg);
    if (nft_sorted->len > TRACE_REPORT_TOP_N) truncated = TRUE;
    JsonArray *nft_arr = json_array_new();
    for (guint i = 0; i < nft_sorted->len && i < TRACE_REPORT_TOP_N; i++) {
        NftDropAgg *a = g_ptr_array_index(nft_sorted, i);
        JsonObject *o = json_object_new();
        json_object_set_string_member(o, "table", a->table);
        json_object_set_string_member(o, "chain", a->chain);
        json_object_set_string_member(o, "handle", a->handle);
        json_object_set_int_member(o, "count", (gint64)a->count);
        json_array_add_object_element(nft_arr, o);
    }
    g_ptr_array_free(nft_sorted, TRUE);

                                   
    GPtrArray *reason_sorted = g_ptr_array_new();
    {
        GHashTableIter it;
        gpointer k, v;
        g_hash_table_iter_init(&it, reasons);
        while (g_hash_table_iter_next(&it, &k, &v))
            g_ptr_array_add(reason_sorted, v);
    }
    g_ptr_array_sort(reason_sorted, _cmp_reason_agg);
    if (reason_sorted->len > TRACE_REPORT_TOP_N) truncated = TRUE;
    JsonArray *reason_arr = json_array_new();
    for (guint i = 0; i < reason_sorted->len && i < TRACE_REPORT_TOP_N; i++) {
        ReasonAgg *a = g_ptr_array_index(reason_sorted, i);
        JsonObject *o = json_object_new();
        json_object_set_string_member(o, "reason", a->reason);
        json_object_set_int_member(o, "count", (gint64)a->count);
        json_array_add_object_element(reason_arr, o);
    }
    g_ptr_array_free(reason_sorted, TRUE);

    json_object_set_int_member(root, "total_events", (gint64)total_events);
    json_object_set_array_member(root, "nft_drops", nft_arr);
    json_object_set_array_member(root, "skb_drop_reasons", reason_arr);
    json_object_set_boolean_member(root, "truncated", truncated);

    g_hash_table_destroy(nft);
    g_hash_table_destroy(reasons);
    return root;
}

                                                                      

                                                             
                                             
                                                 
              
                                                  
                                                    
static gboolean
_trace_id_valid(const char *id)
{
    if (!id || !*id) return FALSE;

    gsize len = strlen(id);
    if (len > 64) return FALSE;                                       

    const char *dash = strchr(id, '-');
    if (!dash || dash == id) return FALSE;                     

                                   
    for (const char *p = id; p < dash; p++)
        if (*p < '0' || *p > '9') return FALSE;

                                      
    const char *suf = dash + 1;
    if (strlen(suf) != 4) return FALSE;
    for (int i = 0; i < 4; i++) {
        char c = suf[i];
        gboolean hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!hex) return FALSE;
    }
    return TRUE;
}

                                                    
                                                   
                                                          
                             
                                                               
JsonObject *
pcv_trace_report_root(const gchar *root, const char *trace_id, GError **error)
{
                                                                
    if (!_trace_id_valid(trace_id)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "invalid trace_id (expected ^[0-9]+-[0-9a-f]{4}$)");
        return NULL;
    }
    if (!root) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "root is NULL");
        return NULL;
    }

                                                    
    gchar      *out_dir   = g_build_filename(root, trace_id, NULL);
    gchar      *data_path = g_build_filename(out_dir, "run.data", NULL);
    gchar      *marker    = g_build_filename(out_dir, PCV_TRACE_RUNNING_MARKER, NULL);
    const gchar *argv[]   = { "retis", "print", data_path,
                              "--format", "single-line", NULL };
    gchar      *out       = NULL;
    GError     *serr      = NULL;
    JsonObject *result    = NULL;

                                                              
                                                           
                                                           
                                                           
                                                     
                                                  
                             
    if (g_file_test(marker, G_FILE_TEST_EXISTS)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "trace '%s' still capturing — retry after it stops", trace_id);
        goto out;
    }

                                                       
    if (!g_file_test(data_path, G_FILE_TEST_EXISTS)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "no capture data for trace_id '%s' (run.data absent)", trace_id);
        goto out;
    }

                                                              
                                                         
                                        
    if (!pcv_trace_selfcheck(error))
        goto out;

                                                                         
    if (!pcv_spawn_sync_timeout(argv, &out, NULL, 30u, &serr)) {
        g_propagate_prefixed_error(error, serr, "retis print failed: ");
        goto out;
    }

    result = pcv_trace_report_parse(out);

out:
    g_free(out);
    g_free(out_dir);
    g_free(data_path);
    g_free(marker);
    return result;
}

                                              
JsonObject *
pcv_trace_report(const char *trace_id, GError **error)
{
    return pcv_trace_report_root(PCV_TRACE_OUT_ROOT, trace_id, error);
}

                                                                             
                                                       
  
                                                        
                                                                                

                                                    
                                    
void
pcv_trace_purge_all_root(const gchar *root)
{
    if (!root) return;

    GDir *dir = g_dir_open(root, 0, NULL);
    if (!dir) return;                               

    const gchar *name;
    while ((name = g_dir_read_name(dir)) != NULL) {
        gchar *path = g_build_filename(root, name, NULL);
        if (!g_file_test(path, G_FILE_TEST_IS_DIR)) { g_free(path); continue; }

        gchar *marker = g_build_filename(path, PCV_TRACE_RUNNING_MARKER, NULL);
        gboolean orphaned = g_file_test(marker, G_FILE_TEST_EXISTS);
        g_free(marker);

        if (orphaned) {
            if (nftw(path, _pcv_trace_recursive_unlink_cb, 16, FTW_DEPTH | FTW_PHYS) != 0)
                PCV_LOG_WARN(TRACE_LOG_DOM, "purge_all: 미완 산출물 삭제 실패: %s", path);
            else
                PCV_LOG_INFO(TRACE_LOG_DOM, "purge_all: 미완 산출물(.running) 정리: %s", path);
        }
        g_free(path);
    }
    g_dir_close(dir);
}

                                                    
                                               
                                 
void
pcv_trace_purge_all(void)
{
                                                     
                                                                 
                                             
                                                     
                                                            
                                         
      
                                                        
                                                  
                       
    gchar *pgrep_out = NULL;
    GError *pgrep_err = NULL;
    const gchar *pgrep_argv[] = { "pgrep", "-f", PCV_TRACE_OUT_ROOT, NULL };
    gboolean pgrep_ok = pcv_spawn_sync(pgrep_argv, &pgrep_out, NULL, &pgrep_err);
    g_clear_error(&pgrep_err);

    gboolean found = pgrep_ok && pgrep_out && *g_strstrip(pgrep_out);
    if (found) {
        PCV_LOG_WARN(TRACE_LOG_DOM,
            "purge_all: 잔존 retis 프로세스 발견(PID: %s) — kill 시도",
            pgrep_out);
    }
    g_free(pgrep_out);

    gchar *pkill_path = g_find_program_in_path("pkill");
    if (!pkill_path) {
        PCV_LOG_WARN(TRACE_LOG_DOM,
            "purge_all: pkill 미설치 — 잔존 retis 프로세스 정리 생략(best-effort)");
    } else {
        g_free(pkill_path);
        const gchar *pkill_argv[] = { "pkill", "-f", PCV_TRACE_OUT_ROOT, NULL };
        GError *err = NULL;
        if (!pcv_spawn_sync(pkill_argv, NULL, NULL, &err)) {
                                                             
                                                              
                                         
            if (found)
                PCV_LOG_WARN(TRACE_LOG_DOM,
                    "purge_all: pkill -f %s 실패: %s",
                    PCV_TRACE_OUT_ROOT, err && err->message ? err->message : "unknown");
            g_clear_error(&err);
        } else if (found) {
            PCV_LOG_INFO(TRACE_LOG_DOM, "purge_all: 잔존 retis 프로세스 kill 완료: pkill -f %s",
                         PCV_TRACE_OUT_ROOT);
        }
    }

                                                                
                                                           
                                                   
                                                     
                                               
                                                 
    if (found) {
        const gint  wait_steps  = 30;                                    
        gboolean    still_alive = TRUE;

        for (gint i = 0; i < wait_steps && still_alive; i++) {
            g_usleep(100 * 1000);
            gchar *out = NULL;
            GError *e = NULL;
            const gchar *argv[] = { "pgrep", "-f", PCV_TRACE_OUT_ROOT, NULL };
            gboolean ok = pcv_spawn_sync(argv, &out, NULL, &e);
            g_clear_error(&e);
            still_alive = ok && out && *g_strstrip(out);
            g_free(out);
        }

        if (still_alive) {
                                                              
                                                          
            PCV_LOG_WARN(TRACE_LOG_DOM,
                "purge_all: SIGTERM 후 3초간 잔존 — SIGKILL 로 강제 종료");
            const gchar *kill_argv[] = { "pkill", "-KILL", "-f", PCV_TRACE_OUT_ROOT, NULL };
            GError *e = NULL;
            if (!pcv_spawn_sync(kill_argv, NULL, NULL, &e))
                g_clear_error(&e);
            g_usleep(200 * 1000);                                    
        }
    }

                                                             
    pcv_trace_purge_all_root(PCV_TRACE_OUT_ROOT);
}
