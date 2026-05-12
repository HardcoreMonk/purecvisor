











#include "pcv_audit.h"
#include "utils/pcv_log.h"
#include <sqlite3.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <time.h>

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
} G = {0};








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
    return rec;
}















static gpointer
_audit_worker(gpointer data __attribute__((unused)))
{
    sqlite3_stmt *stmt = NULL;
    const gchar *sql =
        "INSERT INTO audit_log(ts,node,username,method,target,result,error_code,duration_ms,src_ip) "
        "VALUES(datetime('now'),?,?,?,?,?,?,?,?)";

    if (G.db)
        sqlite3_prepare_v2(G.db, sql, -1, &stmt, NULL);

    while (G.running || g_async_queue_length(G.queue) > 0) {
        PcvAuditRecord *rec = g_async_queue_timeout_pop(G.queue, 500000);
        if (!rec) continue;


        if (stmt) {
            sqlite3_reset(stmt);
            sqlite3_bind_text(stmt, 1, G.node_name, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, rec->username, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, rec->method, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, rec->target, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 5, rec->result, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 6, rec->error_code);
            sqlite3_bind_int64(stmt, 7, rec->duration_ms);
            sqlite3_bind_text(stmt, 8, rec->src_ip, -1, SQLITE_TRANSIENT);
            int rc = sqlite3_step(stmt);
            if (rc != SQLITE_DONE) {
                g_warning("[audit] SQLite INSERT failed: %s", sqlite3_errmsg(G.db));
            }
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
            gchar *del_sql = g_strdup_printf(
                "DELETE FROM audit_log WHERE ts < datetime('now', '-%d days')",
                AUDIT_RETENTION_DAYS);
            gchar *err_msg = NULL;
            int rc = sqlite3_exec(G.db, del_sql, NULL, NULL, &err_msg);
            gint deleted = sqlite3_changes(G.db);
            if (rc == SQLITE_OK && deleted > 0) {
                PCV_LOG_INFO(AUDIT_LOG_DOM,
                    "Retention cleanup: %d records older than %d days deleted",
                    deleted, AUDIT_RETENTION_DAYS);
            }
            if (err_msg) sqlite3_free(err_msg);
            g_free(del_sql);


            sqlite3_wal_checkpoint_v2(G.db, nullptr,
                                      SQLITE_CHECKPOINT_PASSIVE,
                                      nullptr, nullptr);
        }
    }

    if (stmt) sqlite3_finalize(stmt);
    return NULL;
}













void
pcv_audit_init(const gchar *db_path)
{
    gchar hostname[64] = {0};
    gethostname(hostname, sizeof(hostname) - 1);
    G.node_name = g_strdup(hostname);


    if (sqlite3_open(db_path, &G.db) != SQLITE_OK) {
        PCV_LOG_WARN(AUDIT_LOG_DOM, "SQLite open failed: %s — file-only mode",
                     db_path);
        G.db = NULL;
    } else {
        sqlite3_exec(G.db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
        sqlite3_exec(G.db,
            "CREATE TABLE IF NOT EXISTS audit_log ("
            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  ts TEXT NOT NULL,"
            "  node TEXT NOT NULL,"
            "  username TEXT,"
            "  method TEXT NOT NULL,"
            "  target TEXT,"
            "  result TEXT NOT NULL,"
            "  error_code INTEGER,"
            "  duration_ms INTEGER,"
            "  src_ip TEXT"
            ")", NULL, NULL, NULL);
        sqlite3_exec(G.db, "CREATE INDEX IF NOT EXISTS idx_audit_ts ON audit_log(ts)",
                     NULL, NULL, NULL);
        sqlite3_exec(G.db, "CREATE INDEX IF NOT EXISTS idx_audit_method ON audit_log(method)",
                     NULL, NULL, NULL);

        gchar *pragma = g_strdup_printf("PRAGMA max_page_count=%d", AUDIT_DB_MAX_PAGES);
        sqlite3_exec(G.db, pragma, NULL, NULL, NULL);
        g_free(pragma);
    }


    G.queue = g_async_queue_new();
    G.running = TRUE;
    G.total_count = 0;
    G.worker = g_thread_new("audit-writer", _audit_worker, NULL);

    PCV_LOG_INFO(AUDIT_LOG_DOM, "Audit trail initialized (db=%s, node=%s)",
                 db_path, G.node_name);
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
    g_free(G.node_name);
    PCV_LOG_INFO(AUDIT_LOG_DOM, "Audit trail shutdown (total=%ld records)",
                 (long)G.total_count);
}
















void
pcv_audit_log(const gchar *username, const gchar *method,
               const gchar *target, const gchar *result,
               gint error_code, gint64 duration_ms,
               const gchar *src_ip)
{
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
            G.dropped_count++;
            gint64 dc = G.dropped_count;
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
        G.dropped_count++;
        gint64 dc = G.dropped_count;

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







void
pcv_audit_log_rpc(const gchar *method, const gchar *result,
                   gint error_code, gint64 duration_ms)
{
    pcv_audit_log(NULL, method, NULL, result, error_code, duration_ms, NULL);
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
    return G.dropped_count;
}














JsonArray *
pcv_audit_recent_failures(const gchar *target_filter, gint limit)
{
    JsonArray *arr = json_array_new();
    if (!G.db) return arr;


    const gchar *sql_with_target =
        "SELECT ts, method, target, result, error_code, duration_ms "
        "FROM audit_log WHERE result='fail' AND target=? "
        "ORDER BY id DESC LIMIT ?";
    const gchar *sql_all =
        "SELECT ts, method, target, result, error_code, duration_ms "
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
        const gchar *col_ts     = (const gchar *)sqlite3_column_text(stmt, 0);
        const gchar *col_method = (const gchar *)sqlite3_column_text(stmt, 1);
        const gchar *col_target = (const gchar *)sqlite3_column_text(stmt, 2);
        const gchar *col_result = (const gchar *)sqlite3_column_text(stmt, 3);
        json_object_set_string_member(obj, "ts",       col_ts     ? col_ts     : "");
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
        "SELECT ts, username, method, target, result, src_ip, duration_ms "
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
        const gchar *col_ts     = (const gchar *)sqlite3_column_text(stmt, 0);
        const gchar *col_user   = (const gchar *)sqlite3_column_text(stmt, 1);
        const gchar *col_method = (const gchar *)sqlite3_column_text(stmt, 2);
        const gchar *col_target = (const gchar *)sqlite3_column_text(stmt, 3);
        const gchar *col_result = (const gchar *)sqlite3_column_text(stmt, 4);
        const gchar *col_srcip  = (const gchar *)sqlite3_column_text(stmt, 5);
        json_object_set_string_member(obj, "ts",       col_ts     ? col_ts     : "");
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
