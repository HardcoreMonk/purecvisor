   
                        
                                          
  
                           
                                                   
                                                    
                                        
  
                                                     
                                                       
                                                    
                                                     
  
                                                       
                                               
                                                           
                                                       
                                                                               
                                                                      
                                                                         
                                                            
                                                        
                                                                  
                                                                
                                                               
  
          
                                                                    
                                                                         
                                                                                   
                                                           
                                                    
                                               
                                                                       
                                                                         
  
           
                          
                                   
  
              
                                              
   
#include "pcv_job_queue.h"
#include "pcv_job_queue_internal.h"
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
    case PCV_JOB_INTERRUPTED: return "interrupted";
    default:                return "unknown";
    }
}

                                                                 

                                                     
                                                           
                                                       
static gboolean
_migrate_schema(void)
{
    sqlite3_stmt *stmt = NULL;
    gint version = -1;
    if (sqlite3_prepare_v2(G.db, "PRAGMA user_version", -1, &stmt, NULL) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_ROW)
        version = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    if (version < 0 || version > 2) return FALSE;
    if (version == 2) {
        gint rc = sqlite3_prepare_v2(G.db, "SELECT job_id,type,target,status,progress,detail,params,result,created_at,updated_at,generation,method,recovery_reason,outcome_known FROM jobs LIMIT 0", -1, &stmt, NULL);
        sqlite3_finalize(stmt);
        return rc == SQLITE_OK;
    }
    if (sqlite3_exec(G.db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) return FALSE;
    const gchar *sql[] = {
        "CREATE TABLE IF NOT EXISTS jobs (job_id TEXT PRIMARY KEY,type TEXT NOT NULL,target TEXT,status INTEGER DEFAULT 0,progress INTEGER DEFAULT 0,detail TEXT,params TEXT,result TEXT,created_at INTEGER,updated_at INTEGER)",
        "ALTER TABLE jobs ADD COLUMN generation TEXT NOT NULL DEFAULT ''",
        "ALTER TABLE jobs ADD COLUMN method TEXT NOT NULL DEFAULT ''",
        "ALTER TABLE jobs ADD COLUMN recovery_reason TEXT",
        "ALTER TABLE jobs ADD COLUMN outcome_known INTEGER NOT NULL DEFAULT 0",
        "UPDATE jobs SET method=CASE type WHEN 'ova_export' THEN 'vm.export.ova' WHEN 'ova_import' THEN 'vm.import.ova' ELSE type END, outcome_known=CASE WHEN status IN (2,3) THEN 1 ELSE 0 END",
        "CREATE INDEX IF NOT EXISTS idx_jobs_status ON jobs(status)",
        "CREATE INDEX IF NOT EXISTS idx_jobs_created ON jobs(created_at DESC)",
        "PRAGMA user_version=2"
    };
    gboolean ok = TRUE;
    for (guint i = 0; ok && i < G_N_ELEMENTS(sql); i++)
        ok = sqlite3_exec(G.db, sql[i], NULL, NULL, NULL) == SQLITE_OK;
    if (ok) {
        ok = sqlite3_prepare_v2(G.db, "SELECT job_id FROM jobs", -1, &stmt, NULL) == SQLITE_OK;
        gint rc = SQLITE_DONE;
        while (ok && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            gchar *generation = g_uuid_string_random();
            sqlite3_stmt *update = NULL;
            ok = sqlite3_prepare_v2(G.db, "UPDATE jobs SET generation=? WHERE job_id=?", -1, &update, NULL) == SQLITE_OK;
            if (ok) ok = sqlite3_bind_text(update, 1, generation, -1, SQLITE_TRANSIENT) == SQLITE_OK;
            if (ok) ok = sqlite3_bind_text(update, 2, (const gchar *)sqlite3_column_text(stmt, 0), -1, SQLITE_TRANSIENT) == SQLITE_OK;
            if (ok) ok = sqlite3_step(update) == SQLITE_DONE && sqlite3_changes(G.db) == 1;
            sqlite3_finalize(update);
            g_free(generation);
        }
        if (rc != SQLITE_DONE) ok = FALSE;
        sqlite3_finalize(stmt);
    }
    if (ok) ok = sqlite3_exec(G.db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK;
    if (!ok) {
        sqlite3_exec(G.db, "ROLLBACK", NULL, NULL, NULL);
        PCV_LOG_WARN(JOB_LOG_DOM, "Job schema migration failed — admission disabled");
    }
    return ok;
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

                                                  
    const gchar *setup_sql[] = { "PRAGMA journal_mode=WAL" };
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

    if (!_migrate_schema()) {
        sqlite3_close(G.db);
        G.db = NULL;
    } else if (sqlite3_exec(G.db, "CREATE INDEX IF NOT EXISTS idx_jobs_method_target ON jobs(method, target, created_at)",
                            NULL, NULL, NULL) != SQLITE_OK) {
                                                                                                  
        PCV_LOG_WARN(JOB_LOG_DOM, "Job lookup index creation failed — latest-job lookup scans the table");
    }
    G.initialized = TRUE;
    PCV_LOG_INFO(JOB_LOG_DOM, "Job queue initialized (db=%s)", db_path);
    g_mutex_unlock(&G.mu);
    pcv_job_recovery_init(db_path);
}

                                                       
                                  
void
pcv_job_queue_shutdown(void)
{
    pcv_job_recovery_shutdown();
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
                                                                              
                                                          
                                              
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(G.db,
        "SELECT job_id FROM jobs WHERE status IN (2,3,4) AND outcome_known=1 AND updated_at<?",
        -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, cutoff);
        GPtrArray *ids = g_ptr_array_new_with_free_func(g_free);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const gchar *id = (const gchar *)sqlite3_column_text(stmt, 0);
            if (!pcv_job_recovery_protected(id)) g_ptr_array_add(ids, g_strdup(id));
        }
        sqlite3_finalize(stmt);
        for (guint i = 0; i < ids->len; i++) {
            stmt = NULL;
            if (sqlite3_prepare_v2(G.db, "DELETE FROM jobs WHERE job_id=?", -1, &stmt, NULL) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, g_ptr_array_index(ids, i), -1, SQLITE_TRANSIENT);
                sqlite3_step(stmt);
            }
            sqlite3_finalize(stmt);
        }
        g_ptr_array_unref(ids);
    }
    g_mutex_unlock(&G.mu);
}

                                                             
                                                       
gboolean
pcv_job_queue_retention_tick(gint64 *last_run, gint64 now, gint retention_days)
{
    if (!last_run) return FALSE;
    if (now - *last_run < PCV_JOB_RETENTION_INTERVAL_SECONDS)
        return FALSE;                                                     
    *last_run = now;
    if (retention_days <= 0)
        return FALSE;                                                                      
    pcv_job_queue_cleanup_old(retention_days * 24);                 
    return TRUE;
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
    const gchar *sql = "UPDATE jobs SET status=?, progress=?, detail=?, updated_at=? WHERE job_id=? AND generation='' AND status<2";
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
    const gchar *sql = "UPDATE jobs SET status=?, progress=100, result=?, outcome_known=1, updated_at=? WHERE job_id=? AND generation=''";
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
    s = (const gchar *)sqlite3_column_text(stmt, 10);
    json_object_set_string_member(obj, "generation", s ? s : "");
    s = (const gchar *)sqlite3_column_text(stmt, 11);
    json_object_set_string_member(obj, "method", s ? s : "");
    s = (const gchar *)sqlite3_column_text(stmt, 12);
    if (s) json_object_set_string_member(obj, "recovery_reason", s);
    gchar *reason = pcv_job_recovery_reason(json_object_get_string_member(obj, "job_id"));
    if (reason) json_object_set_string_member(obj, "recovery_reason", reason);
    g_free(reason);
    json_object_set_boolean_member(obj, "outcome_known", sqlite3_column_int(stmt, 13) != 0);
    json_object_set_boolean_member(obj, "recovery_pending",
                                  pcv_job_recovery_pending(json_object_get_string_member(obj, "job_id")));
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
        "created_at,updated_at,generation,method,recovery_reason,outcome_known FROM jobs ORDER BY created_at DESC LIMIT ?";
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
        "created_at,updated_at,generation,method,recovery_reason,outcome_known FROM jobs WHERE job_id=?";
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
pcv_job_find_latest(const gchar *method, const gchar *target, JsonObject **out)
{
    g_return_val_if_fail(out != NULL, FALSE);
    *out = NULL;
    if (!method || !target) return FALSE;

    ensure_mutex();

    g_mutex_lock(&G.mu);
    if (!G.db) { g_mutex_unlock(&G.mu); return FALSE; }

    const gchar *sql =
        "SELECT job_id,type,target,status,progress,detail,params,result,"
        "created_at,updated_at,generation,method,recovery_reason,outcome_known FROM jobs"
        " WHERE method=? AND target=? ORDER BY created_at DESC, rowid DESC LIMIT 1";
    sqlite3_stmt *stmt = NULL;
    gint rc = sqlite3_prepare_v2(G.db, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 1, method, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 2, target, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) *out = _row_to_json(stmt);
    sqlite3_finalize(stmt);
    g_mutex_unlock(&G.mu);
    return rc == SQLITE_ROW || rc == SQLITE_DONE;
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
        "UPDATE jobs SET status=4, outcome_known=0, recovery_reason='cancel_requested', detail='Cancellation requested; worker outcome unconfirmed', updated_at=?"
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

                                                              
                                                        
gchar *
pcv_job_db_admit(const gchar *type, const gchar *target, const gchar *params,
                 const gchar *method, const gchar *generation)
{
    ensure_mutex();
    g_mutex_lock(&G.mu);
    gchar *id = NULL;
    sqlite3_stmt *stmt = NULL;
    gboolean ok = G.db && sqlite3_exec(G.db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK;
    if (ok) {
        ok = sqlite3_prepare_v2(G.db, "SELECT count(*) FROM jobs WHERE status<2", -1, &stmt, NULL) == SQLITE_OK;
        if (ok) ok = sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) < 128;
        sqlite3_finalize(stmt);
        stmt = NULL;
    }
    if (ok) ok = sqlite3_prepare_v2(G.db,
        "INSERT INTO jobs(job_id,type,target,params,method,generation,status,progress,created_at,updated_at) VALUES(?,?,?,?,?,?,0,0,?,?)",
        -1, &stmt, NULL) == SQLITE_OK;
    if (ok) {
        gint rc = SQLITE_OK;
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 2, type, -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 3, target, -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 4, params, -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 5, method, -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 6, generation, -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 7, time(NULL));
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 8, time(NULL));
        for (guint i = 0; rc == SQLITE_OK && i < JOB_ID_ATTEMPTS; i++) {
            g_free(id);
            id = g_strdup_printf("job-%08x", g_random_int());
            rc = sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
            if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
            if (sqlite3_extended_errcode(G.db) != SQLITE_CONSTRAINT_PRIMARYKEY || i + 1 == JOB_ID_ATTEMPTS) break;
            sqlite3_reset(stmt);
            rc = SQLITE_OK;
        }
        ok = _write_finished(stmt, rc, "admit", id);
        stmt = NULL;
    }
    sqlite3_finalize(stmt);
    if (ok) ok = sqlite3_exec(G.db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK;
    if (!ok) {
        if (G.db && !sqlite3_get_autocommit(G.db)) sqlite3_exec(G.db, "ROLLBACK", NULL, NULL, NULL);
        g_clear_pointer(&id, g_free);
    }
    g_mutex_unlock(&G.mu);
    return id;
}

gboolean
pcv_job_db_progress(const PcvJobRef *ref, gint progress, const gchar *detail)
{
    ensure_mutex();
    g_mutex_lock(&G.mu);
    sqlite3_stmt *stmt = NULL;
    gint rc = G.db ? sqlite3_prepare_v2(G.db,
        "UPDATE jobs SET status=1,progress=?,detail=?,updated_at=? WHERE job_id=? AND generation=? AND method=? AND target IS ? AND status<2",
        -1, &stmt, NULL) : SQLITE_ERROR;
    if (rc == SQLITE_OK) {
        if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 1, CLAMP(progress, 0, 100));
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 2, detail, -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 3, time(NULL));
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 4, pcv_job_id(ref), -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 5, pcv_job_generation(ref), -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 6, pcv_job_method(ref), -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 7, pcv_job_target(ref), -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    }
    gboolean ok = G.db && _write_finished(stmt, rc, "progress", pcv_job_id(ref));
    g_mutex_unlock(&G.mu);
    return ok;
}

PcvJobApply
pcv_job_db_apply(const PcvJobRef *ref, PcvJobStatus status, const gchar *result)
{
    ensure_mutex();
    g_mutex_lock(&G.mu);
    PcvJobApply applied = PCV_JOB_RETRY;
    sqlite3_stmt *stmt = NULL;
    gboolean transaction = G.db && sqlite3_exec(G.db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK;
    if (transaction && sqlite3_prepare_v2(G.db,
        "SELECT generation,method,target,status,result FROM jobs WHERE job_id=?", -1, &stmt, NULL) == SQLITE_OK) {
        gint rc = sqlite3_bind_text(stmt, 1, pcv_job_id(ref), -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) applied = PCV_JOB_CONFLICT;
        if (rc == SQLITE_ROW) {
            gboolean identity = g_strcmp0((const gchar *)sqlite3_column_text(stmt, 0), pcv_job_generation(ref)) == 0 &&
                g_strcmp0((const gchar *)sqlite3_column_text(stmt, 1), pcv_job_method(ref)) == 0 &&
                g_strcmp0((const gchar *)sqlite3_column_text(stmt, 2), pcv_job_target(ref)) == 0;
            gint previous = sqlite3_column_int(stmt, 3);
            if (!identity) applied = PCV_JOB_CONFLICT;
            else if (previous >= 2)
                applied = previous == (gint)status && g_strcmp0((const gchar *)sqlite3_column_text(stmt, 4), result) == 0
                    ? PCV_JOB_APPLIED : PCV_JOB_CONFLICT;
        }
        sqlite3_finalize(stmt);
        stmt = NULL;
        if (applied == PCV_JOB_RETRY && rc == SQLITE_ROW && sqlite3_prepare_v2(G.db,
            "UPDATE jobs SET status=?,progress=100,result=?,outcome_known=1,recovery_reason=NULL,updated_at=? WHERE job_id=? AND generation=? AND status<2",
            -1, &stmt, NULL) == SQLITE_OK) {
            gint write_rc = SQLITE_OK;
            if (write_rc == SQLITE_OK) write_rc = sqlite3_bind_int(stmt, 1, status);
            if (write_rc == SQLITE_OK) write_rc = sqlite3_bind_text(stmt, 2, result, -1, SQLITE_TRANSIENT);
            if (write_rc == SQLITE_OK) write_rc = sqlite3_bind_int64(stmt, 3, time(NULL));
            if (write_rc == SQLITE_OK) write_rc = sqlite3_bind_text(stmt, 4, pcv_job_id(ref), -1, SQLITE_TRANSIENT);
            if (write_rc == SQLITE_OK) write_rc = sqlite3_bind_text(stmt, 5, pcv_job_generation(ref), -1, SQLITE_TRANSIENT);
            if (write_rc == SQLITE_OK) write_rc = sqlite3_step(stmt);
            if (_write_finished(stmt, write_rc, "replay", pcv_job_id(ref))) applied = PCV_JOB_APPLIED;
            stmt = NULL;
        }
    }
    sqlite3_finalize(stmt);
    if (transaction) {
        if (applied == PCV_JOB_APPLIED) {
            if (sqlite3_exec(G.db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) applied = PCV_JOB_RETRY;
        }
        if (!sqlite3_get_autocommit(G.db)) sqlite3_exec(G.db, "ROLLBACK", NULL, NULL, NULL);
    }
    g_mutex_unlock(&G.mu);
    return applied;
}

                                            
                                              
gboolean
pcv_job_db_reconcile(GHashTable *durable_ids)
{
    ensure_mutex();
    g_mutex_lock(&G.mu);
    sqlite3_stmt *stmt = NULL;
    GPtrArray *ids = g_ptr_array_new_with_free_func(g_free);
    gint rc = G.db ? sqlite3_exec(G.db, "BEGIN IMMEDIATE", NULL, NULL, NULL) : SQLITE_ERROR;
    gboolean transaction = rc == SQLITE_OK;
    if (rc == SQLITE_OK) rc = sqlite3_prepare_v2(G.db, "SELECT job_id FROM jobs WHERE status<2", -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            const gchar *id = (const gchar *)sqlite3_column_text(stmt, 0);
            if (!g_hash_table_contains(durable_ids, id)) g_ptr_array_add(ids, g_strdup(id));
        }
        if (rc == SQLITE_DONE) rc = SQLITE_OK;
    }
    sqlite3_finalize(stmt);
    for (guint i = 0; rc == SQLITE_OK && i < ids->len; i++) {
        stmt = NULL;
        rc = sqlite3_prepare_v2(G.db,
            "UPDATE jobs SET status=5,outcome_known=0,recovery_reason='restart_without_durable_result',detail='Execution interrupted; actual outcome unknown',updated_at=? WHERE job_id=? AND status<2",
            -1, &stmt, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, time(NULL));
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 2, g_ptr_array_index(ids, i), -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) rc = sqlite3_changes(G.db) == 1 ? SQLITE_OK : SQLITE_ERROR;
        sqlite3_finalize(stmt);
    }
    if (rc == SQLITE_OK) rc = sqlite3_exec(G.db, "COMMIT", NULL, NULL, NULL);
    if (transaction && !sqlite3_get_autocommit(G.db)) sqlite3_exec(G.db, "ROLLBACK", NULL, NULL, NULL);
    if (rc != SQLITE_OK) PCV_LOG_WARN(JOB_LOG_DOM, "Restart reconcile deferred (sqlite=%d)", rc);
    g_ptr_array_unref(ids);
    g_mutex_unlock(&G.mu);
    return rc == SQLITE_OK;
}
