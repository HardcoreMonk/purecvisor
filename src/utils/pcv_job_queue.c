   
                        
                                          
  
                           
                                                   
                                                    
                                        
  
                                                     
                                                       
                                                    
                                                     
  
                                                       
                                               
                                                           
                                                       
                                                                      
                                                               


                                                        
                                                                  
                                              
  
          
                                              
                                                                  
                                                                
                                                    
                                               
                                                                         
  
           
                          
                                   
  
              
                                              
   
#include "pcv_job_queue.h"
#include "utils/pcv_log.h"
#include "utils/pcv_config.h"
#include <sqlite3.h>
#include <string.h>
#include <time.h>

#define JOB_LOG_DOM "job_queue"
#define JOB_ID_ATTEMPTS 32
#define JOB_DEFAULT_LIMIT 50                                             

                                          
                                                               
static struct {
    sqlite3  *db;
    GMutex    mu;                                    
    gboolean  initialized;                              
} G = {0};

static gsize g_mutex_once = 0;                                   

                                                 
                                         
                                                                    
static void
ensure_mutex(void)
{
    if (g_once_init_enter(&g_mutex_once)) {                        
        g_mutex_init(&G.mu);
        g_once_init_leave(&g_mutex_once, 1);                          
    }
}

                                                            
                                                     
                                                                
                                                             
static const gchar *
_status_str(PcvJobStatus s)
{
    switch (s) {
    case PCV_JOB_PENDING:   return "pending";
    case PCV_JOB_RUNNING:   return "running";
    case PCV_JOB_COMPLETED: return "completed";
    case PCV_JOB_FAILED:    return "failed";
    case PCV_JOB_CANCELLED: return "cancelled";
    default:                return "unknown";
    }
}

                                                                 

                                                               
                                                    
                                                         


                                                   
void
pcv_job_queue_init(void)
{
    ensure_mutex();

    g_mutex_lock(&G.mu);
    if (G.initialized) {                                 
        g_mutex_unlock(&G.mu);
        PCV_LOG_INFO(JOB_LOG_DOM, "Job queue already initialized");
        return;
    }

                                                 
                                                       
    const gchar *env_path = g_getenv("PCV_JOBS_DB_PATH");
    const gchar *db_path = env_path && *env_path
        ? env_path
        : pcv_config_get_string("jobs", "db_path",
                                "/var/lib/purecvisor/pcv_jobs.db");

    if (sqlite3_open(db_path, &G.db) != SQLITE_OK) {

                                                                       
        PCV_LOG_WARN(JOB_LOG_DOM, "SQLite open failed: %s — job queue disabled",
                     db_path);
        if (G.db) {
            sqlite3_close(G.db);
        }
        G.db = NULL;
        G.initialized = TRUE;                             
        g_mutex_unlock(&G.mu);
        return;
    }


    const gchar *setup_sql[] = {
        "PRAGMA journal_mode=WAL",
        "CREATE TABLE IF NOT EXISTS jobs ("
        "  job_id TEXT PRIMARY KEY, type TEXT NOT NULL, target TEXT,"
        "  status INTEGER DEFAULT 0, progress INTEGER DEFAULT 0, detail TEXT,"
        "  params TEXT, result TEXT, created_at INTEGER, updated_at INTEGER)",
        "CREATE INDEX IF NOT EXISTS idx_jobs_status ON jobs(status)",
        "CREATE INDEX IF NOT EXISTS idx_jobs_created ON jobs(created_at DESC)"
    };
    for (guint i = 0; i < G_N_ELEMENTS(setup_sql); i++) {
        gint rc = sqlite3_exec(G.db, setup_sql[i], NULL, NULL, NULL);
        if (rc != SQLITE_OK) {
            PCV_LOG_WARN(JOB_LOG_DOM, "Job queue setup failed (sqlite=%d) — queue disabled", rc);
            sqlite3_close(G.db);
            G.db = NULL;
            G.initialized = TRUE;
            g_mutex_unlock(&G.mu);
            return;
        }
    }

    G.initialized = TRUE;
    PCV_LOG_INFO(JOB_LOG_DOM, "Job queue initialized (db=%s)", db_path);
    g_mutex_unlock(&G.mu);
}

                                                       
                                  
void
pcv_job_queue_shutdown(void)
{
    ensure_mutex();

    g_mutex_lock(&G.mu);
    if (G.db) {
        sqlite3_close(G.db);
        G.db = NULL;
    }
    G.initialized = FALSE;
    g_mutex_unlock(&G.mu);

    PCV_LOG_INFO(JOB_LOG_DOM, "Job queue shutdown");
}

                                                                     
                                                  
                                                   
                                 
void
pcv_job_queue_cleanup_old(gint max_age_hours)
{
    ensure_mutex();

    g_mutex_lock(&G.mu);
    if (!G.db) { g_mutex_unlock(&G.mu); return; }                       

                                                                    
    gint64 cutoff = (gint64)time(NULL) - (gint64)max_age_hours * 3600;
                                                                              
    const gchar *sql = "DELETE FROM jobs WHERE status >= 2 AND updated_at < ?";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(G.db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, cutoff);
        sqlite3_step(stmt);
        gint deleted = sqlite3_changes(G.db);
        if (deleted > 0)
            PCV_LOG_INFO(JOB_LOG_DOM, "Cleaned up %d old jobs (age > %dh)",
                         deleted, max_age_hours);
        sqlite3_finalize(stmt);
    }
    g_mutex_unlock(&G.mu);
}

                                                              




static gboolean
_write_finished(sqlite3_stmt *stmt, gint rc, const gchar *operation, const gchar *job_id)
{
    gint rows = rc == SQLITE_DONE ? sqlite3_changes(G.db) : 0;
    gint finalized = sqlite3_finalize(stmt);
    gboolean persisted = rc == SQLITE_DONE && rows == 1 && finalized == SQLITE_OK;
    if (!persisted)
        PCV_LOG_WARN(JOB_LOG_DOM, "Job %s %s not persisted (sqlite=%d rows=%d finalize=%d)",
                     job_id, operation, rc, rows, finalized);
    return persisted;
}






gchar *
pcv_job_create(const gchar *type, const gchar *target, const gchar *params_json)
{
    ensure_mutex();
    g_mutex_lock(&G.mu);
    if (!G.db) {
        g_mutex_unlock(&G.mu);
        PCV_LOG_WARN(JOB_LOG_DOM, "Job queue disabled — admission rejected");
        return NULL;
    }

    const gchar *sql =
        "INSERT INTO jobs (job_id, type, target, status, progress, params, created_at, updated_at)"
        " VALUES (?, ?, ?, 0, 0, ?, ?, ?)";
    sqlite3_stmt *stmt = NULL;
    gint rc = sqlite3_prepare_v2(G.db, sql, -1, &stmt, NULL);
    gint64 now = (gint64)time(NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 2, type ? type : "", -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 3, target, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 4, params_json, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 5, now);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 6, now);

    gchar *job_id = NULL;
    for (guint attempt = 0; rc == SQLITE_OK && attempt < JOB_ID_ATTEMPTS; attempt++) {
        g_free(job_id);
        job_id = g_strdup_printf("job-%08x", g_random_int());
        rc = sqlite3_bind_text(stmt, 1, job_id, -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
        if ((rc & 0xff) != SQLITE_CONSTRAINT ||
            sqlite3_extended_errcode(G.db) != SQLITE_CONSTRAINT_PRIMARYKEY ||
            attempt + 1 == JOB_ID_ATTEMPTS)
            break;

        sqlite3_reset(stmt);
        rc = SQLITE_OK;
    }
    gboolean persisted = _write_finished(stmt, rc, "create", job_id ? job_id : "(no ID)");
    g_mutex_unlock(&G.mu);
    if (!persisted) {
        g_free(job_id);
        return NULL;
    }
    PCV_LOG_INFO(JOB_LOG_DOM, "Job created: %s (type=%s target=%s)",
                 job_id, type ? type : "", target ? target : "");
    return job_id;
}




gboolean
pcv_job_update_status(const gchar *job_id, PcvJobStatus status,
                       gint progress_pct, const gchar *detail)
{
    if (!job_id) return FALSE;
    ensure_mutex();
    g_mutex_lock(&G.mu);
    if (!G.db) { g_mutex_unlock(&G.mu); return FALSE; }
    const gchar *sql = "UPDATE jobs SET status=?, progress=?, detail=?, updated_at=? WHERE job_id=?";
    sqlite3_stmt *stmt = NULL;
    gint rc = sqlite3_prepare_v2(G.db, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 1, (int)status);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 2, progress_pct);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 3, detail, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 4, (gint64)time(NULL));
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 5, job_id, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    gboolean persisted = _write_finished(stmt, rc, "status update", job_id);
    g_mutex_unlock(&G.mu);
    return persisted;
}





gboolean
pcv_job_set_result(const gchar *job_id, PcvJobStatus status, const gchar *result_json)
{
    if (!job_id) return FALSE;
    ensure_mutex();
    g_mutex_lock(&G.mu);
    if (!G.db) { g_mutex_unlock(&G.mu); return FALSE; }
    const gchar *sql = "UPDATE jobs SET status=?, progress=100, result=?, updated_at=? WHERE job_id=?";
    sqlite3_stmt *stmt = NULL;
    gint rc = sqlite3_prepare_v2(G.db, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 1, (int)status);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 2, result_json, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 3, (gint64)time(NULL));
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 4, job_id, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    gboolean persisted = _write_finished(stmt, rc, "result update", job_id);
    g_mutex_unlock(&G.mu);
    if (persisted)
        PCV_LOG_INFO(JOB_LOG_DOM, "Job %s finished: %s", job_id, _status_str(status));
    return persisted;
}

                                                                  

   
                
                                         
                                                    
                                                                    
                                                          
                                                                    
   
static JsonObject *
_row_to_json(sqlite3_stmt *stmt)
{
    JsonObject *obj = json_object_new();
    const gchar *s;

    s = (const gchar *)sqlite3_column_text(stmt, 0);
    json_object_set_string_member(obj, "job_id", s ? s : "");
    s = (const gchar *)sqlite3_column_text(stmt, 1);
    json_object_set_string_member(obj, "type", s ? s : "");
    s = (const gchar *)sqlite3_column_text(stmt, 2);
    json_object_set_string_member(obj, "target", s ? s : "");

    gint status_val = sqlite3_column_int(stmt, 3);
                                                                    
    json_object_set_string_member(obj, "status", _status_str((PcvJobStatus)status_val));
    json_object_set_int_member(obj, "status_code", status_val);
    json_object_set_int_member(obj, "progress", sqlite3_column_int(stmt, 4));

                                                                         
    s = (const gchar *)sqlite3_column_text(stmt, 5);
    if (s) json_object_set_string_member(obj, "detail", s);
    s = (const gchar *)sqlite3_column_text(stmt, 6);
    if (s) json_object_set_string_member(obj, "params", s);
    s = (const gchar *)sqlite3_column_text(stmt, 7);
    if (s) json_object_set_string_member(obj, "result", s);

    json_object_set_int_member(obj, "created_at", sqlite3_column_int64(stmt, 8));
    json_object_set_int_member(obj, "updated_at", sqlite3_column_int64(stmt, 9));
    return obj;
}

                                                            
                                             
                                                         
                                                                     
JsonArray *
pcv_job_list(gint limit)
{
    JsonArray *arr = json_array_new();
    if (limit <= 0) limit = JOB_DEFAULT_LIMIT;                           

    ensure_mutex();

    g_mutex_lock(&G.mu);
    if (!G.db) { g_mutex_unlock(&G.mu); return arr; }                        

    const gchar *sql =
        "SELECT job_id,type,target,status,progress,detail,params,result,"
        "created_at,updated_at FROM jobs ORDER BY created_at DESC LIMIT ?";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(G.db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, limit);
        while (sqlite3_step(stmt) == SQLITE_ROW)                                    
            json_array_add_object_element(arr, _row_to_json(stmt));
        sqlite3_finalize(stmt);
    }
    g_mutex_unlock(&G.mu);
    return arr;
}

                                               
                                           
                                                                     
JsonObject *
pcv_job_get(const gchar *job_id)
{
    if (!job_id) return NULL;

    ensure_mutex();

    g_mutex_lock(&G.mu);
    if (!G.db) { g_mutex_unlock(&G.mu); return NULL; }

    const gchar *sql =
        "SELECT job_id,type,target,status,progress,detail,params,result,"
        "created_at,updated_at FROM jobs WHERE job_id=?";
    sqlite3_stmt *stmt;
    JsonObject *obj = NULL;
    if (sqlite3_prepare_v2(G.db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, job_id, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW)
            obj = _row_to_json(stmt);
        sqlite3_finalize(stmt);
    }
    g_mutex_unlock(&G.mu);
    return obj;
}

                                                     
                                                    
                                                    
                                                   
                                                                   
gboolean
pcv_job_cancel(const gchar *job_id)
{
    if (!job_id) return FALSE;
    gint64 now = (gint64)time(NULL);

    ensure_mutex();

    g_mutex_lock(&G.mu);
    if (!G.db) { g_mutex_unlock(&G.mu); return FALSE; }

                                                                                     
                                                                      
    const gchar *sql =
        "UPDATE jobs SET status=4, detail='Cancelled by user', updated_at=?"
        " WHERE job_id=? AND status < 2";
    sqlite3_stmt *stmt = NULL;
    gint rc = sqlite3_prepare_v2(G.db, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, now);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 2, job_id, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    gboolean ok = _write_finished(stmt, rc, "cancel", job_id);
    g_mutex_unlock(&G.mu);

    if (ok)
        PCV_LOG_INFO(JOB_LOG_DOM, "Job cancelled: %s", job_id);
    return ok;
}
