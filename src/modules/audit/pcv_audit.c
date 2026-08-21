   
                    
                                                           
  
                           
                                                   
                                                    
                                        
  
          
                                                   
                                                   
                                                     
                                                    
                                                  
                                       
  
          
                                                           
                                                 
  
           
                                    
                                                          
  
         
                                              
                                                   
                                                            
  
        
                                                 
                                                  
                                                       
  
         
                                                 
                                                      
  
                                                                       
   
#include "pcv_audit.h"
#include "pcv_audit_chain.h"
#include "modules/dispatcher/rpc_completion.h"                                         
#include "utils/pcv_log.h"
#include <errno.h>
#include <fcntl.h>
#include <sqlite3.h>
#include <json-glib/json-glib.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

#define AUDIT_LOG_DOM "audit"
constexpr int AUDIT_QUEUE_MAX      = 10000;                                       
constexpr int AUDIT_RETENTION_DAYS = 30;                                     
constexpr int AUDIT_CLEANUP_INTERVAL = 3600;                           
constexpr int AUDIT_DB_MAX_PAGES   = 262144;                                           
constexpr int AUDIT_RATE_LIMIT     = 1000;                                 

                                                          
static_assert(AUDIT_RETENTION_DAYS >= 1, "Must retain at least 1 day");                         
static_assert(AUDIT_DB_MAX_PAGES >= 1024, "DB too small");                                                  

                                             
                                                       
                                                 
                                       
                                                         
static struct {
    sqlite3       *db;
    GAsyncQueue   *queue;
    GThread       *worker;
    gboolean       running;
    gint64         total_count;
    gint64         dropped_count;                         
    gchar         *node_name;
    gint64         last_cleanup;                                                 
    gint           lock_fd;                                             
    gchar         *lock_path;
    PcvAuditChainHealth chain_health;
} G = {.lock_fd = -1};

                                             
                                                      
                                                           
                                                    
                                                           
static GMutex dropped_mtx;
static GMutex chain_health_mtx;

static void
_audit_set_chain_health(const PcvAuditChainHealth *health)
{
    g_mutex_lock(&chain_health_mtx);
    if (health)
        G.chain_health = *health;
    else
        G.chain_health = (PcvAuditChainHealth){0};
    g_mutex_unlock(&chain_health_mtx);
}

   
                      
                             
  
                                      
                          
  
                                                             
   
static void
_audit_record_free(PcvAuditRecord *rec)
{
    if (!rec) return;                                             
    g_free(rec->username);
    g_free(rec->method);
    g_free(rec->target);
    g_free(rec->result);
    g_free(rec->src_ip);
    g_free(rec);
}

                                                                 
                                         
                                                                    

                                                          
               
                         
  
                                             
                                                            
                                             
  
                                                    
  
                                                    
                                               
   
                                                 

                                                          
                   
                                                              
                         
                                                            
  
                    
                                                                       
                                                            
                                                       
  
                                                         
  
                                                    
                                                    
   
                                                      
                     
                                                         
                            
  
                                                 
                                            
  
                                          
   
   
                      
  
                                               
                                               
                                   
                           
  
                                                      
  
                                                                      
  
                                                  
                                                  
   
static PcvAuditRecord *
_audit_record_copy(const gchar *username, const gchar *method,
                    const gchar *target, const gchar *result,
                    gint error_code, gint64 duration_ms,
                    const gchar *src_ip)
{
    PcvAuditRecord *rec = g_new0(PcvAuditRecord, 1);                                         
                                                    
                                                        
    rec->username    = g_strdup(username ? username : "-");                         
    rec->method      = g_strdup(method ? method : "?");                         
    rec->target      = g_strdup(target ? target : "");
    rec->result      = g_strdup(result ? result : "ok");                                
    rec->error_code  = error_code;
    rec->duration_ms = duration_ms;
    rec->src_ip      = g_strdup(src_ip ? src_ip : "");
                                                        
                                                               
    rec->event_us    = g_get_real_time();
    return rec;
}

   
                 
  
                                              
  
                 
                                                 
                                              
                             
              
  
                                           
                                                     
  
                                               
                                                  
                                                              
                                                            
   
static gpointer
_audit_worker(gpointer data __attribute__((unused)))
{
                                                        
                                                           
    while (G.running || g_async_queue_length(G.queue) > 0) {
                                                          
                                             
        PcvAuditRecord *rec = g_async_queue_timeout_pop(G.queue, 500000);            
        if (!rec) continue;                                      

                                                                        
                                                                     
                                                                  
        if (G.db) {
                                                               
            GDateTime *wdt = g_date_time_new_now_utc();
            gchar *ts = wdt ? g_date_time_format(wdt, "%Y-%m-%d %H:%M:%S")
                            : g_strdup("");
            if (wdt) g_date_time_unref(wdt);

                                                                     
                                                      
                                   
            GDateTime *edt =
                g_date_time_new_from_unix_utc(rec->event_us / G_USEC_PER_SEC);
            gchar *event_ts = NULL;
            if (edt) {
                gchar *base = g_date_time_format(edt, "%Y-%m-%d %H:%M:%S");
                event_ts = g_strdup_printf("%s.%06d", base,
                                           (gint)(rec->event_us % G_USEC_PER_SEC));
                g_free(base);
                g_date_time_unref(edt);
            }

            PcvAuditChainRecord chain_record = {
                .ts = ts,
                .node = G.node_name,
                .username = rec->username,
                .method = rec->method,
                .target = rec->target,
                .result = rec->result,
                .error_code = rec->error_code,
                .duration_ms = rec->duration_ms,
                .src_ip = rec->src_ip,
                .event_ts = event_ts ? event_ts : "",
            };
            GError *append_error = NULL;
            if (!pcv_audit_chain_append(G.db, &chain_record, NULL,
                                        &append_error)) {
                PcvAuditChainHealth failed = pcv_audit_get_chain_health();
                failed.current_ok = FALSE;
                g_strlcpy(failed.reason, "append_failed", sizeof(failed.reason));
                _audit_set_chain_health(&failed);
                PCV_LOG_WARN(AUDIT_LOG_DOM, "SQLite atomic append failed: %s",
                             append_error ? append_error->message : "unknown error");
                g_clear_error(&append_error);
            }
            g_free(ts);
            g_free(event_ts);
        }

                                         
        PCV_LOG_AUDIT(AUDIT_LOG_DOM, rec->method, rec->target,
                       "user=%s result=%s code=%d dur=%ldms",
                       rec->username, rec->result, rec->error_code,
                       (long)rec->duration_ms);

        G.total_count++;
        _audit_record_free(rec);

                                                        
                                                        
                                                          
                                            
        gint64 mono_now = g_get_monotonic_time() / G_USEC_PER_SEC;                  
        if (G.db && (mono_now - G.last_cleanup) >= AUDIT_CLEANUP_INTERVAL) {
            G.last_cleanup = mono_now;
            GDateTime *now = g_date_time_new_now_utc();
            GDateTime *cutoff_dt = now
                ? g_date_time_add_days(now, -AUDIT_RETENTION_DAYS) : NULL;
            gchar *cutoff = cutoff_dt
                ? g_date_time_format(cutoff_dt, "%Y-%m-%d %H:%M:%S") : NULL;
            gint deleted = 0;
            GError *retention_error = NULL;
            gboolean retained = cutoff && pcv_audit_chain_retention(
                G.db, cutoff, &deleted, &retention_error);
            if (retained && deleted > 0) {
                PCV_LOG_INFO(AUDIT_LOG_DOM,
                    "Retention cleanup: %d records older than %d days deleted",
                    deleted, AUDIT_RETENTION_DAYS);
            }
            if (!retained) {
                PcvAuditChainHealth failed = pcv_audit_get_chain_health();
                failed.current_ok = FALSE;
                g_strlcpy(failed.reason, "retention_failed", sizeof(failed.reason));
                _audit_set_chain_health(&failed);
                PCV_LOG_WARN(AUDIT_LOG_DOM, "Retention checkpoint/delete failed: %s",
                             retention_error ? retention_error->message
                                             : "cutoff calculation failed");
            } else {
                PcvAuditChainHealth verified = {0};
                PcvAuditChainHealth previous = pcv_audit_get_chain_health();
                GError *verify_error = NULL;
                if (pcv_audit_chain_verify(G.db, &verified, &verify_error)) {
                                                                 
                                                                 
                                                              
                                                          
                    if (g_strcmp0(previous.reason, "append_failed") != 0)
                        _audit_set_chain_health(&verified);
                } else {
                    if (verify_error) {
                        verified = pcv_audit_get_chain_health();
                        verified.current_ok = FALSE;
                        g_strlcpy(verified.reason, "verify_failed",
                                  sizeof(verified.reason));
                    }
                    _audit_set_chain_health(&verified);
                    PCV_LOG_WARN(AUDIT_LOG_DOM,
                        "Post-retention audit verification failed: %s",
                        verify_error ? verify_error->message : verified.reason);
                }
                g_clear_error(&verify_error);
            }
            g_clear_error(&retention_error);
            g_free(cutoff);
            if (cutoff_dt) g_date_time_unref(cutoff_dt);
            if (now) g_date_time_unref(now);

                                                     
            sqlite3_wal_checkpoint_v2(G.db, nullptr,
                                      SQLITE_CHECKPOINT_PASSIVE,
                                      nullptr, nullptr);
        }
    }

    return NULL;
}

   
                  
                                                                 
  
                    
                        
                                          
                                  
  
                                                          
                                        
  
                                                    
                                             
   
gboolean
pcv_audit_init(const gchar *db_path, GError **error)
{
    g_return_val_if_fail(db_path != NULL && *db_path != '\0', FALSE);

    gchar hostname[64] = {0};
    gethostname(hostname, sizeof(hostname) - 1);
    G.node_name = g_strdup(hostname);

                                                                   
                                                            
                                                       
    G.lock_path = g_strconcat(db_path, ".lock", NULL);
    G.lock_fd = open(G.lock_path,
                     O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (G.lock_fd < 0 || flock(G.lock_fd, LOCK_EX | LOCK_NB) != 0) {
        gint saved_errno = errno;
        if (G.lock_fd >= 0) close(G.lock_fd);
        G.lock_fd = -1;
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                    "audit DB lock acquisition failed (%s): %s",
                    G.lock_path, g_strerror(saved_errno));
        g_clear_pointer(&G.lock_path, g_free);
        g_clear_pointer(&G.node_name, g_free);
        return FALSE;
    }

    gint open_rc = sqlite3_open(db_path, &G.db);
    if (open_rc != SQLITE_OK) {
        PCV_LOG_WARN(AUDIT_LOG_DOM, "SQLite open failed: %s — file-only mode",
                     db_path);
        if (G.db) sqlite3_close(G.db);
        G.db = NULL;
        PcvAuditChainHealth unavailable = {0};
        g_strlcpy(unavailable.reason, "db_unavailable",
                  sizeof(unavailable.reason));
        _audit_set_chain_health(&unavailable);
    } else {
        sqlite3_busy_timeout(G.db, 5000);
        gint setup_rc = sqlite3_exec(G.db, "PRAGMA journal_mode=WAL", NULL,
                                    NULL, NULL);
        PcvAuditChainHealth prepared = {0};
        GError *chain_error = NULL;
                                                    
        if (setup_rc == SQLITE_OK &&
            !pcv_audit_chain_prepare(G.db, &prepared, &chain_error))
            setup_rc = SQLITE_CORRUPT;
        if (setup_rc == SQLITE_OK)
            setup_rc = sqlite3_exec(G.db,
                "CREATE INDEX IF NOT EXISTS idx_audit_ts ON audit_log(ts);"
                "CREATE INDEX IF NOT EXISTS idx_audit_method ON audit_log(method)",
                NULL, NULL, NULL);
        if (setup_rc == SQLITE_OK) {
            gchar *pragma = g_strdup_printf("PRAGMA max_page_count=%d",
                                            AUDIT_DB_MAX_PAGES);
            setup_rc = sqlite3_exec(G.db, pragma, NULL, NULL, NULL);
            g_free(pragma);
        }
        if (setup_rc != SQLITE_OK) {
            if (chain_error)
                g_propagate_error(error, chain_error);
            else
                g_set_error(error, PCV_AUDIT_CHAIN_ERROR,
                            PCV_AUDIT_CHAIN_ERROR_SQLITE,
                            "audit DB setup failed: sqlite rc=%d: %s",
                            setup_rc, sqlite3_errmsg(G.db));
            sqlite3_close(G.db);
            G.db = NULL;
            flock(G.lock_fd, LOCK_UN);
            close(G.lock_fd);
            G.lock_fd = -1;
            g_clear_pointer(&G.lock_path, g_free);
            g_clear_pointer(&G.node_name, g_free);
            return FALSE;
        }
        _audit_set_chain_health(&prepared);
        PCV_LOG_INFO(AUDIT_LOG_DOM,
                     "Audit hash-chain verified (epoch=%ld historical_break=%s)",
                     (long)prepared.active_epoch,
                     prepared.historical_break ? "true" : "false");
    }

    G.queue = g_async_queue_new();
    G.running = TRUE;
    G.total_count = 0;
                                                      
                                       
    G.last_cleanup = g_get_monotonic_time() / G_USEC_PER_SEC;
    G.worker = g_thread_new("audit-writer", _audit_worker, NULL);

    PCV_LOG_INFO(AUDIT_LOG_DOM, "Audit trail initialized (db=%s, node=%s)",
                 db_path, G.node_name);
    return TRUE;
}

   
                      
  
                   
                                                       
  
                                         
                               
  
                                                     
   
void
pcv_audit_shutdown(void)
{
    G.running = FALSE;                                                             
    if (G.worker) {
        g_thread_join(G.worker);                                      
        G.worker = NULL;
    }
    if (G.queue) {
        g_async_queue_unref(G.queue);
        G.queue = NULL;
    }
    if (G.db) {
        sqlite3_close(G.db);
        G.db = NULL;
    }
    if (G.lock_fd >= 0) {
        flock(G.lock_fd, LOCK_UN);
        close(G.lock_fd);
        G.lock_fd = -1;
    }
    g_free(G.node_name);
    G.node_name = NULL;
    g_clear_pointer(&G.lock_path, g_free);
    _audit_set_chain_health(NULL);
    PCV_LOG_INFO(AUDIT_LOG_DOM, "Audit trail shutdown (total=%ld records)",
                 (long)G.total_count);
}

   
                 
                                              
                                                  
                                           
                                              
                               
                            
                                                      
  
                               
                                     
  
                                                     
  
                                                
                                             
                                                      
   
void
pcv_audit_log(const gchar *username, const gchar *method,
               const gchar *target, const gchar *result,
               gint error_code, gint64 duration_ms,
               const gchar *src_ip)
{
                                                                       
                                                                       
                                                      
    pcv_rpc_completion_note_audit(method);
    if (!G.running || !G.queue) return;                                             

                                                      
                                                        
                                                       
                                         
    {
                                                      
                                                       
        static GMutex bucket_mtx;                                           
        static gint64 bucket_ts = 0;                                    
        static gint   bucket_tokens = 0;                        
        gint64 now_sec = g_get_monotonic_time() / G_USEC_PER_SEC;
        g_mutex_lock(&bucket_mtx);
        if (now_sec != bucket_ts) {                                 
            bucket_ts = now_sec;
            bucket_tokens = 0;
        }
        if (bucket_tokens >= AUDIT_RATE_LIMIT) {                               
                                                                   
            g_mutex_lock(&dropped_mtx);
            G.dropped_count++;
            gint64 dc = G.dropped_count;
            g_mutex_unlock(&dropped_mtx);
            g_mutex_unlock(&bucket_mtx);
                                                                               
                                                             
                                                      
                                              
            if (dc == 1 || dc == 10 || dc == 100 || dc == 1000 ||
                dc == 10000 || dc == 100000) {
                g_warning("[audit] rate-limit dropped %ld records total", (long)dc);
            }
            return;                                 
        }
        bucket_tokens++;                           
        g_mutex_unlock(&bucket_mtx);
    }

    gint qlen = g_async_queue_length(G.queue);                            

                             
                                                       
    if (qlen >= AUDIT_QUEUE_MAX) {
                                                                        
        g_mutex_lock(&dropped_mtx);
        G.dropped_count++;
        gint64 dc = G.dropped_count;
        g_mutex_unlock(&dropped_mtx);
                                                                           
                                                                    
        if (dc == 1 || dc == 10 || dc == 100 || dc == 1000 ||
            dc == 10000 || dc == 100000) {
            g_warning("[audit] queue overflow: %ld records dropped total", (long)dc);
        }
                                                         
                                                         
                                                      
        static gint64 last_warn = 0;
        gint64 now = g_get_monotonic_time() / G_USEC_PER_SEC;
        if (now - last_warn >= 60) {
            PCV_LOG_WARN(AUDIT_LOG_DOM,
                "Audit queue overflow (%d/%d) — %ld records dropped total",
                qlen, AUDIT_QUEUE_MAX, (long)dc);
            last_warn = now;
        }
        return;
    }

                                                      
                                                              
                                           
    if (qlen >= (AUDIT_QUEUE_MAX * 9 / 10)) {
        static gint64 last_90_warn = 0;
        gint64 now = g_get_monotonic_time() / G_USEC_PER_SEC;
        if (now - last_90_warn >= 60) {                                 
            PCV_LOG_WARN(AUDIT_LOG_DOM,
                "Audit queue nearing capacity: %d/%d (90%%)", qlen, AUDIT_QUEUE_MAX);
            last_90_warn = now;
        }
    }

                                                
                                                                      
    PcvAuditRecord *rec = _audit_record_copy(username, method, target,
                                              result, error_code,
                                              duration_ms, src_ip);
    g_async_queue_push(G.queue, rec);                                 
}

                                                         
                                                          
                                                    
gint64
pcv_audit_get_total_count(void)
{
    return G.total_count;
}

                                                                  
                                    
gint
pcv_audit_get_queue_depth(void)
{
    if (!G.queue) return 0;                            
    return g_async_queue_length(G.queue);
}

                                                     
                                                  
gint64
pcv_audit_get_dropped_count(void)
{
                                                                              
    g_mutex_lock(&dropped_mtx);
    gint64 dc = G.dropped_count;
    g_mutex_unlock(&dropped_mtx);
    return dc;
}

PcvAuditChainHealth
pcv_audit_get_chain_health(void)
{
    g_mutex_lock(&chain_health_mtx);
    PcvAuditChainHealth health = G.chain_health;
    g_mutex_unlock(&chain_health_mtx);
    return health;
}

   
                    
                                                   
                                                   
                                             
                                               
                                            
  
                                        
                                         
  
                                                                    
   
                                                            
                                                               
                                                                 
                                                          
                                                
                                                  
JsonArray *
pcv_audit_recent_failures(const gchar *target_filter, gint limit)
{
    JsonArray *arr = json_array_new();                                               
    if (!G.db) return arr;                                           

                                             
    const gchar *sql_with_target =
        "SELECT ts, method, target, result, error_code, duration_ms, event_ts "
        "FROM audit_log WHERE result='fail' AND target=? "
        "ORDER BY id DESC LIMIT ?";
    const gchar *sql_all =
        "SELECT ts, method, target, result, error_code, duration_ms, event_ts "
        "FROM audit_log WHERE result='fail' "
        "ORDER BY id DESC LIMIT ?";

    sqlite3_stmt *stmt = NULL;
    gint use_limit = limit > 0 ? limit : 5;                                           

                                                           
                                                        
    if (target_filter && *target_filter) {                                    
        if (sqlite3_prepare_v2(G.db, sql_with_target, -1, &stmt, NULL) != SQLITE_OK) return arr;                          
        sqlite3_bind_text(stmt, 1, target_filter, -1, SQLITE_TRANSIENT);                                    
        sqlite3_bind_int(stmt, 2, use_limit);
    } else {
        if (sqlite3_prepare_v2(G.db, sql_all, -1, &stmt, NULL) != SQLITE_OK) return arr;
        sqlite3_bind_int(stmt, 1, use_limit);
    }

                                        
                                                                        
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        JsonObject *obj = json_object_new();
        const gchar *col_ts       = (const gchar *)sqlite3_column_text(stmt, 0);                             
        const gchar *col_method   = (const gchar *)sqlite3_column_text(stmt, 1);
        const gchar *col_target   = (const gchar *)sqlite3_column_text(stmt, 2);
        const gchar *col_result   = (const gchar *)sqlite3_column_text(stmt, 3);
        const gchar *col_event_ts = (const gchar *)sqlite3_column_text(stmt, 6);
        json_object_set_string_member(obj, "ts",       col_ts     ? col_ts     : "");
        json_object_set_string_member(obj, "event_ts", col_event_ts ? col_event_ts : "");
        json_object_set_string_member(obj, "method",   col_method ? col_method : "");
        json_object_set_string_member(obj, "target",   col_target ? col_target : "");
        json_object_set_string_member(obj, "result",   col_result ? col_result : "");
        json_object_set_int_member(obj, "error_code",  sqlite3_column_int(stmt, 4));
        json_object_set_int_member(obj, "duration_ms", sqlite3_column_int(stmt, 5));
                                                                            
        gchar *msg = g_strdup_printf("%s failed (code=%d, dur=%dms)",
                                      col_method ? col_method : "unknown",
                                      sqlite3_column_int(stmt, 4),
                                      sqlite3_column_int(stmt, 5));
        json_object_set_string_member(obj, "message", msg);
        g_free(msg);
        json_array_add_object_element(arr, obj);
    }
    sqlite3_finalize(stmt);
    return arr;
}

   
                                                                    
                                                                     
  
                                                     
                                                            
   
JsonArray *
pcv_audit_search(const gchar *from_ts, const gchar *to_ts,
                  const gchar *username, const gchar *method_pattern,
                  gint limit)
{
    JsonArray *arr = json_array_new();
    if (!G.db) return arr;                        

                                                 
                                                            
                                                                      
    const gchar *sql =
        "SELECT ts, username, method, target, result, src_ip, duration_ms, event_ts "
        "FROM audit_log WHERE 1=1 "
        "AND (? IS NULL OR ts >= ?) "
        "AND (? IS NULL OR ts <= ?) "
        "AND (? IS NULL OR username = ?) "
        "AND (? IS NULL OR method LIKE ?) "
        "ORDER BY ts DESC LIMIT ?";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(G.db, sql, -1, &stmt, NULL) != SQLITE_OK) return arr;

                                                          
                                                            
    sqlite3_bind_text(stmt, 1, from_ts, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, from_ts, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, to_ts, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, to_ts, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, username, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, username, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, method_pattern, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8, method_pattern, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 9, limit > 0 ? limit : 100);                               

                                                           
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        JsonObject *obj = json_object_new();
        const gchar *col_ts       = (const gchar *)sqlite3_column_text(stmt, 0);
        const gchar *col_user     = (const gchar *)sqlite3_column_text(stmt, 1);
        const gchar *col_method   = (const gchar *)sqlite3_column_text(stmt, 2);
        const gchar *col_target   = (const gchar *)sqlite3_column_text(stmt, 3);
        const gchar *col_result   = (const gchar *)sqlite3_column_text(stmt, 4);
        const gchar *col_srcip    = (const gchar *)sqlite3_column_text(stmt, 5);
        const gchar *col_event_ts = (const gchar *)sqlite3_column_text(stmt, 7);
        json_object_set_string_member(obj, "ts",       col_ts     ? col_ts     : "");
        json_object_set_string_member(obj, "event_ts", col_event_ts ? col_event_ts : "");
        json_object_set_string_member(obj, "username", col_user   ? col_user   : "");
        json_object_set_string_member(obj, "method",   col_method ? col_method : "");
        json_object_set_string_member(obj, "target",   col_target ? col_target : "");
        json_object_set_string_member(obj, "result",   col_result ? col_result : "");
        json_object_set_string_member(obj, "src_ip",   col_srcip  ? col_srcip  : "");
        json_object_set_int_member(obj, "duration_ms", sqlite3_column_int(stmt, 6));
        json_array_add_object_element(arr, obj);
    }
    sqlite3_finalize(stmt);
    return arr;
}
