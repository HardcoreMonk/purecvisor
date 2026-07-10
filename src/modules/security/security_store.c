#include "modules/security/security_store.h"

#include "utils/pcv_config.h"          /* pcv_config_get_string (ensure_open 경로) */
#include <glib/gstdio.h>               /* g_mkdir_with_parents */
#include "modules/audit/pcv_audit.h"
#include "modules/daemons/alert_engine.h"
#include "modules/security/security_policy.h"

#include <sqlite3.h>
#include <time.h>

/*
 * Security Guard persistence lives in one local SQLite database so Web UI,
 * CLI/TUI, UDS handlers, audit, and HIPS approval state all read the same
 * event stream. Reads intentionally return empty JSON containers on store
 * failures while G.degraded records that the operator should investigate.
 */
static struct {
    sqlite3 *db;
    GMutex mu;
    gboolean mutex_ready;
    gboolean degraded;
    gchar *path;
} G = {0};

static gsize g_mutex_once = 0;

/*
 * Retention cap for terminal (resolved/suppressed) security_events rows.
 * Attacker-influenced distinct coalesce keys and post-resolution re-occurrences
 * append rows without bound; capping the terminal history prevents a
 * disk-exhaustion DoS. open/action_pending rows are never pruned so the live
 * operator queue is preserved.
 */
#define PCV_MAX_RETAINED_EVENTS 20000

static gint g_retention_cap = PCV_MAX_RETAINED_EVENTS;

/*
 * Test-only hook (no header declaration; tests forward-declare it, matching the
 * pcv_test_* pattern). cap <= 0 restores the compiled default.
 */
void
pcv_security_store_set_retention_cap_for_test(gint cap)
{
    g_retention_cap = (cap > 0) ? cap : PCV_MAX_RETAINED_EVENTS;
}

static void
ensure_mutex(void)
{
    if (g_once_init_enter(&g_mutex_once)) {
        g_mutex_init(&G.mu);
        G.mutex_ready = TRUE;
        g_once_init_leave(&g_mutex_once, 1);
    }
}

static GQuark
security_store_error_quark(void)
{
    return g_quark_from_static_string("pcv-security-store");
}

static void
set_sqlite_error(GError **error, gint code, const gchar *context)
{
    if (!error) {
        return;
    }
    g_set_error(error, security_store_error_quark(), code,
                "%s: %s", context, G.db ? sqlite3_errmsg(G.db) : "database is closed");
}

static gint64
now_sec(void)
{
    return (gint64)time(NULL);
}

static const gchar *
col_text(sqlite3_stmt *stmt, gint idx)
{
    const unsigned char *s = sqlite3_column_text(stmt, idx);
    return s ? (const gchar *)s : "";
}

static gboolean
exec_sql(const gchar *sql, GError **error)
{
    gchar *errmsg = NULL;
    gint rc = sqlite3_exec(G.db, sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        if (error) {
            g_set_error(error, security_store_error_quark(), rc,
                        "%s", errmsg ? errmsg : sqlite3_errmsg(G.db));
        }
        sqlite3_free(errmsg);
        G.degraded = TRUE;
        return FALSE;
    }
    return TRUE;
}

static gboolean
init_schema(GError **error)
{
    /*
     * The partial unique index is the coalescing contract: repeated open risks
     * update occurrence_count instead of creating unbounded rows. Resolved or
     * suppressed history stays immutable for audit review.
     */
    static const gchar *schema[] = {
        "PRAGMA journal_mode=WAL",
        "CREATE TABLE IF NOT EXISTS security_events ("
        "  event_id TEXT PRIMARY KEY,"
        "  timestamp INTEGER NOT NULL,"
        "  source TEXT NOT NULL,"
        "  type TEXT NOT NULL,"
        "  severity TEXT NOT NULL,"
        "  confidence INTEGER NOT NULL,"
        "  target_kind TEXT NOT NULL,"
        "  target TEXT NOT NULL,"
        "  summary TEXT NOT NULL,"
        "  recommended_action TEXT NOT NULL,"
        "  status TEXT NOT NULL,"
        "  evidence_json TEXT NOT NULL,"
        "  coalesce_key TEXT NOT NULL,"
        "  occurrence_count INTEGER NOT NULL DEFAULT 1,"
        "  last_seen INTEGER NOT NULL,"
        "  created_at INTEGER NOT NULL"
        ")",
        "CREATE INDEX IF NOT EXISTS idx_security_events_ts "
        "ON security_events(timestamp DESC)",
        "CREATE INDEX IF NOT EXISTS idx_security_events_sev "
        "ON security_events(severity, status)",
        "CREATE UNIQUE INDEX IF NOT EXISTS idx_security_events_coalesce_open "
        "ON security_events(coalesce_key) "
        "WHERE status IN ('open', 'action_pending')",
        "CREATE TABLE IF NOT EXISTS security_config ("
        "  key TEXT PRIMARY KEY,"
        "  value TEXT NOT NULL,"
        "  updated_at INTEGER NOT NULL,"
        "  updated_by TEXT NOT NULL"
        ")",
        "INSERT OR IGNORE INTO security_config(key,value,updated_at,updated_by) "
        "VALUES('enabled','false',strftime('%s','now'),'system')",
        "CREATE TABLE IF NOT EXISTS security_actions ("
        "  event_id TEXT PRIMARY KEY,"
        "  action TEXT NOT NULL,"
        "  target_kind TEXT NOT NULL,"
        "  target TEXT NOT NULL,"
        "  status TEXT NOT NULL,"
        "  ttl_sec INTEGER NOT NULL DEFAULT 3600,"
        "  expires_at INTEGER NOT NULL DEFAULT 0,"
        "  requested_at INTEGER NOT NULL,"
        "  decided_at INTEGER NOT NULL DEFAULT 0,"
        "  decided_by TEXT NOT NULL DEFAULT '',"
        "  reason TEXT NOT NULL DEFAULT '',"
        "  job_id TEXT NOT NULL DEFAULT '',"
        "  error TEXT NOT NULL DEFAULT ''"
        ")",
        "CREATE INDEX IF NOT EXISTS idx_security_actions_status "
        "ON security_actions(status, requested_at DESC)",
    };

    for (gsize i = 0; i < G_N_ELEMENTS(schema); i++) {
        if (!exec_sql(schema[i], error)) {
            return FALSE;
        }
    }
    return TRUE;
}

gboolean
pcv_security_store_open(const gchar *path)
{
    if (!path || !*path) {
        return FALSE;
    }

    ensure_mutex();

    g_mutex_lock(&G.mu);
    if (G.db) {
        sqlite3_close(G.db);
        G.db = NULL;
    }
    g_clear_pointer(&G.path, g_free);
    G.degraded = FALSE;

    gint rc = sqlite3_open(path, &G.db);
    if (rc != SQLITE_OK) {
        if (G.db) {
            sqlite3_close(G.db);
            G.db = NULL;
        }
        G.degraded = TRUE;
        g_mutex_unlock(&G.mu);
        return FALSE;
    }
    G.path = g_strdup(path);

    GError *error = NULL;
    gboolean ok = init_schema(&error);
    if (!ok) {
        g_clear_error(&error);
        sqlite3_close(G.db);
        G.db = NULL;
        g_clear_pointer(&G.path, g_free);
    }
    g_mutex_unlock(&G.mu);
    return ok;
}

gboolean
pcv_security_store_ensure_open(void)
{
    /* 전용 init 뮤텍스로 check→open 을 원자화해 동시 RPC 하에서도 단일 open 보장.
     * pcv_security_store_open 은 G.mu 를 자체 획득하므로 여기선 G.mu 를 보유하지 않는다. */
    static GMutex ensure_mu;
    static gsize  ensure_mu_init = 0;
    if (g_once_init_enter(&ensure_mu_init)) {
        g_mutex_init(&ensure_mu);
        g_once_init_leave(&ensure_mu_init, 1);
    }
    g_mutex_lock(&ensure_mu);

    ensure_mutex();
    g_mutex_lock(&G.mu);
    gboolean already = (G.db != NULL);
    g_mutex_unlock(&G.mu);

    gboolean ok = already;
    if (!already) {
        const gchar *path = pcv_config_get_string("security", "db_path",
                                                  PCV_SECURITY_DB_DEFAULT);
        if (path && *path) {
            gchar *dir = g_path_get_dirname(path);
            if (dir && *dir) (void)g_mkdir_with_parents(dir, 0750);
            g_free(dir);
        }
        ok = pcv_security_store_open(path);
    }
    g_mutex_unlock(&ensure_mu);
    return ok;
}

void
pcv_security_store_close(void)
{
    if (!G.mutex_ready) {
        return;
    }

    ensure_mutex();
    g_mutex_lock(&G.mu);
    if (G.db) {
        sqlite3_close(G.db);
        G.db = NULL;
    }
    g_clear_pointer(&G.path, g_free);
    G.degraded = FALSE;
    g_mutex_unlock(&G.mu);
}

static gboolean
update_existing_event(const PcvSecurityEvent *ev, const gchar *key, GError **error)
{
    /*
     * Coalescing preserves the first event_id as the operator/audit anchor while
     * refreshing the evidence and last_seen fields with the newest observation.
     */
    const gchar *sql =
        "UPDATE security_events "
        "SET occurrence_count=occurrence_count+1, last_seen=?, timestamp=?, "
        "severity=?, confidence=?, summary=?, evidence_json=? "
        "WHERE coalesce_key=? AND status IN ('open','action_pending')";
    sqlite3_stmt *stmt = NULL;
    gint rc = sqlite3_prepare_v2(G.db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        set_sqlite_error(error, rc, "prepare coalesce update");
        G.degraded = TRUE;
        return FALSE;
    }

    gint64 ts = ev->timestamp > 0 ? ev->timestamp : now_sec();
    sqlite3_bind_int64(stmt, 1, ts);
    sqlite3_bind_int64(stmt, 2, ts);
    sqlite3_bind_text(stmt, 3, pcv_security_severity_to_string(ev->severity), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, CLAMP(ev->confidence, 0, 100));
    sqlite3_bind_text(stmt, 5, ev->summary, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, ev->evidence_json, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, key, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        set_sqlite_error(error, rc, "execute coalesce update");
        G.degraded = TRUE;
        return FALSE;
    }
    return sqlite3_changes(G.db) > 0;
}

static void
enforce_event_retention(void)
{
    /*
     * Prune the OLDEST terminal (resolved/suppressed) rows beyond the retention
     * cap, keeping the newest g_retention_cap of them. open/action_pending rows
     * are excluded from both the delete and the keep-set, so live queue state is
     * never dropped. Ordering uses timestamp then rowid: the table has a TEXT
     * PRIMARY KEY (event_id) and no explicit id column, so SQLite's implicit
     * rowid is the monotonic insertion-order tiebreak for "oldest".
     *
     * Called under G.mu (held by the insert). Best-effort: a retention failure
     * is logged but must not fail the triggering insert.
     */
    const gchar *sql =
        "DELETE FROM security_events "
        "WHERE status IN ('resolved','suppressed') AND rowid NOT IN ("
        "SELECT rowid FROM security_events "
        "WHERE status IN ('resolved','suppressed') "
        "ORDER BY timestamp DESC, rowid DESC LIMIT ?1)";
    sqlite3_stmt *stmt = NULL;
    gint rc = sqlite3_prepare_v2(G.db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        g_warning("security_store: prepare retention prune failed: %s",
                  sqlite3_errmsg(G.db));
        return;
    }
    sqlite3_bind_int(stmt, 1, g_retention_cap);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        g_warning("security_store: execute retention prune failed: %s",
                  sqlite3_errmsg(G.db));
    }
}

gboolean
pcv_security_store_insert_event(const PcvSecurityEvent *ev, GError **error)
{
    if (!ev) {
        g_set_error(error, security_store_error_quark(), SQLITE_MISUSE,
                    "event is required");
        return FALSE;
    }

    ensure_mutex();
    g_mutex_lock(&G.mu);
    if (!G.db) {
        g_mutex_unlock(&G.mu);
        g_set_error(error, security_store_error_quark(), SQLITE_MISUSE,
                    "security store is not open");
        return FALSE;
    }

    g_autofree gchar *key = pcv_security_event_coalesce_key(ev);
    gboolean coalesced = update_existing_event(ev, key, error);
    if (coalesced) {
        g_mutex_unlock(&G.mu);
        return TRUE;
    }
    if (error && *error) {
        g_mutex_unlock(&G.mu);
        return FALSE;
    }

    const gchar *sql =
        "INSERT INTO security_events("
        "event_id,timestamp,source,type,severity,confidence,target_kind,target,summary,"
        "recommended_action,status,evidence_json,coalesce_key,occurrence_count,last_seen,created_at"
        ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,1,?,?)";
    sqlite3_stmt *stmt = NULL;
    gint rc = sqlite3_prepare_v2(G.db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        set_sqlite_error(error, rc, "prepare insert event");
        G.degraded = TRUE;
        g_mutex_unlock(&G.mu);
        return FALSE;
    }

    gint64 ts = ev->timestamp > 0 ? ev->timestamp : now_sec();
    gint64 created_at = now_sec();
    sqlite3_bind_text(stmt, 1, ev->event_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, ts);
    sqlite3_bind_text(stmt, 3, pcv_security_source_to_string(ev->source), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, pcv_security_type_to_string(ev->type), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, pcv_security_severity_to_string(ev->severity), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 6, CLAMP(ev->confidence, 0, 100));
    sqlite3_bind_text(stmt, 7, pcv_security_target_kind_to_string(ev->target_kind), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8, ev->target, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 9, ev->summary, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 10, ev->recommended_action, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 11, pcv_security_status_to_string(ev->status), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 12, ev->evidence_json, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 13, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 14, ts);
    sqlite3_bind_int64(stmt, 15, created_at);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    gboolean ok = (rc == SQLITE_DONE);
    if (!ok) {
        set_sqlite_error(error, rc, "execute insert event");
        G.degraded = TRUE;
    } else {
        enforce_event_retention();
    }
    g_mutex_unlock(&G.mu);
    return ok;
}

gboolean
pcv_security_submit_event(PcvSecurityEvent *ev, GError **error)
{
    if (!ev) {
        g_set_error(error, security_store_error_quark(), SQLITE_MISUSE,
                    "event is required");
        return FALSE;
    }

    if (ev->event_id[0] == '\0') {
        pcv_security_event_make_id(ev, "sec");
    }
    /*
     * Policy normalization happens at submit time so every consumer sees the
     * same severity and action, regardless of whether the event came from HIDS,
     * runtime probes, logs, or future adapters.
     */
    ev->severity = pcv_security_policy_normalize_severity(ev);

    const gchar *action = pcv_security_policy_recommend_action(ev);
    g_strlcpy(ev->recommended_action, action ? action : "manual_runbook",
              sizeof ev->recommended_action);

    if (!pcv_security_store_insert_event(ev, error)) {
        return FALSE;
    }

    if (pcv_security_policy_should_audit(ev)) {
        /*
         * WARN/CRIT security events use event_id as the audit target. That gives
         * the UI, audit log, and pending action table one correlation key.
         */
        const gchar *severity = pcv_security_severity_to_string(ev->severity);
        pcv_audit_log(NULL, "security.event", ev->event_id,
                      "ok", 0, 0, "local");
        pcv_alert_record_security_event(ev->event_id, severity, ev->summary);
    }
    return TRUE;
}

static JsonObject *
row_to_event_json(sqlite3_stmt *stmt)
{
    /*
     * JSON field names are the public Security Events contract. Keep this row
     * mapper boring and one-to-one with the SELECT list so REST, UDS, CLI/TUI,
     * and Web UI all observe the same shape.
     */
    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "event_id", col_text(stmt, 0));
    json_object_set_int_member(obj, "timestamp", sqlite3_column_int64(stmt, 1));
    json_object_set_string_member(obj, "source", col_text(stmt, 2));
    json_object_set_string_member(obj, "type", col_text(stmt, 3));
    json_object_set_string_member(obj, "severity", col_text(stmt, 4));
    json_object_set_int_member(obj, "confidence", sqlite3_column_int(stmt, 5));
    json_object_set_string_member(obj, "target_kind", col_text(stmt, 6));
    json_object_set_string_member(obj, "target", col_text(stmt, 7));
    json_object_set_string_member(obj, "summary", col_text(stmt, 8));
    json_object_set_string_member(obj, "recommended_action", col_text(stmt, 9));
    json_object_set_string_member(obj, "status", col_text(stmt, 10));
    json_object_set_string_member(obj, "evidence_json", col_text(stmt, 11));
    json_object_set_string_member(obj, "coalesce_key", col_text(stmt, 12));
    json_object_set_int_member(obj, "occurrence_count", sqlite3_column_int(stmt, 13));
    json_object_set_int_member(obj, "last_seen", sqlite3_column_int64(stmt, 14));
    json_object_set_int_member(obj, "created_at", sqlite3_column_int64(stmt, 15));
    return obj;
}

JsonArray *
pcv_security_store_list_events(gint offset, gint limit,
                               const gchar *severity,
                               const gchar *source,
                               const gchar *status)
{
    /*
     * Bound list size at the store layer. UI filters can request subsets, but a
     * broken caller cannot force the daemon to materialize the whole event DB.
     */
    JsonArray *arr = json_array_new();
    if (offset < 0) {
        offset = 0;
    }
    if (limit <= 0 || limit > 500) {
        limit = 100;
    }

    ensure_mutex();
    g_mutex_lock(&G.mu);
    if (!G.db) {
        g_mutex_unlock(&G.mu);
        G.degraded = TRUE;
        return arr;
    }

    GString *sql = g_string_new(
        "SELECT event_id,timestamp,source,type,severity,confidence,target_kind,target,summary,"
        "recommended_action,status,evidence_json,coalesce_key,occurrence_count,last_seen,created_at "
        "FROM security_events WHERE 1=1");
    if (severity && *severity) {
        g_string_append(sql, " AND severity=?");
    }
    if (source && *source) {
        g_string_append(sql, " AND source=?");
    }
    if (status && *status) {
        g_string_append(sql, " AND status=?");
    }
    g_string_append(sql, " ORDER BY timestamp DESC, created_at DESC LIMIT ? OFFSET ?");

    sqlite3_stmt *stmt = NULL;
    gint rc = sqlite3_prepare_v2(G.db, sql->str, -1, &stmt, NULL);
    g_string_free(sql, TRUE);
    if (rc != SQLITE_OK) {
        G.degraded = TRUE;
        g_mutex_unlock(&G.mu);
        return arr;
    }

    gint idx = 1;
    if (severity && *severity) {
        sqlite3_bind_text(stmt, idx++, severity, -1, SQLITE_TRANSIENT);
    }
    if (source && *source) {
        sqlite3_bind_text(stmt, idx++, source, -1, SQLITE_TRANSIENT);
    }
    if (status && *status) {
        sqlite3_bind_text(stmt, idx++, status, -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_int(stmt, idx++, limit);
    sqlite3_bind_int(stmt, idx, offset);

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        JsonObject *obj = row_to_event_json(stmt);
        json_array_add_object_element(arr, obj);
    }
    if (rc != SQLITE_DONE) {
        G.degraded = TRUE;
    }
    sqlite3_finalize(stmt);
    g_mutex_unlock(&G.mu);
    return arr;
}

JsonObject *
pcv_security_store_get_event(const gchar *event_id)
{
    if (!event_id || !*event_id) {
        return NULL;
    }

    ensure_mutex();
    g_mutex_lock(&G.mu);
    if (!G.db) {
        g_mutex_unlock(&G.mu);
        G.degraded = TRUE;
        return NULL;
    }

    const gchar *sql =
        "SELECT event_id,timestamp,source,type,severity,confidence,target_kind,target,summary,"
        "recommended_action,status,evidence_json,coalesce_key,occurrence_count,last_seen,created_at "
        "FROM security_events WHERE event_id=?";
    sqlite3_stmt *stmt = NULL;
    gint rc = sqlite3_prepare_v2(G.db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        G.degraded = TRUE;
        g_mutex_unlock(&G.mu);
        return NULL;
    }

    sqlite3_bind_text(stmt, 1, event_id, -1, SQLITE_TRANSIENT);
    JsonObject *obj = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        obj = row_to_event_json(stmt);
    }
    sqlite3_finalize(stmt);
    g_mutex_unlock(&G.mu);
    return obj;
}

gboolean
pcv_security_store_update_event_status(const gchar *event_id,
                                       PcvSecurityStatus status,
                                       GError **error)
{
    /*
     * Event status is updated independently from action status. A user can
     * suppress or resolve an event without pretending an executable HIPS action
     * was approved.
     */
    if (!event_id || !*event_id) {
        g_set_error(error, security_store_error_quark(), SQLITE_MISUSE,
                    "event_id is required");
        return FALSE;
    }

    ensure_mutex();
    g_mutex_lock(&G.mu);
    if (!G.db) {
        g_mutex_unlock(&G.mu);
        g_set_error(error, security_store_error_quark(), SQLITE_MISUSE,
                    "security store is not open");
        return FALSE;
    }

    const gchar *sql = "UPDATE security_events SET status=? WHERE event_id=?";
    sqlite3_stmt *stmt = NULL;
    gint rc = sqlite3_prepare_v2(G.db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        set_sqlite_error(error, rc, "prepare update status");
        G.degraded = TRUE;
        g_mutex_unlock(&G.mu);
        return FALSE;
    }
    sqlite3_bind_text(stmt, 1, pcv_security_status_to_string(status), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, event_id, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    gboolean ok = (rc == SQLITE_DONE && sqlite3_changes(G.db) > 0);
    sqlite3_finalize(stmt);
    if (!ok && rc != SQLITE_DONE) {
        set_sqlite_error(error, rc, "execute update status");
        G.degraded = TRUE;
    }
    g_mutex_unlock(&G.mu);
    return ok;
}

gint
pcv_security_store_count_by_coalesce_key(const gchar *coalesce_key)
{
    /*
     * Tests use this to prove coalescing remains bounded. Production callers
     * should prefer list/get APIs instead of depending on the partial index.
     */
    if (!coalesce_key) {
        return 0;
    }

    ensure_mutex();
    g_mutex_lock(&G.mu);
    if (!G.db) {
        g_mutex_unlock(&G.mu);
        G.degraded = TRUE;
        return 0;
    }

    const gchar *sql = "SELECT COUNT(*) FROM security_events WHERE coalesce_key=?";
    sqlite3_stmt *stmt = NULL;
    gint count = 0;
    if (sqlite3_prepare_v2(G.db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, coalesce_key, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            count = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    } else {
        G.degraded = TRUE;
    }
    g_mutex_unlock(&G.mu);
    return count;
}

gboolean
pcv_security_store_get_bool_config(const gchar *key, gboolean def)
{
    /*
     * Config reads fail closed to the caller-provided default. Security Guard
     * startup should not crash the daemon if the local SQLite file is missing.
     */
    if (!key || !*key) {
        return def;
    }

    ensure_mutex();
    g_mutex_lock(&G.mu);
    if (!G.db) {
        g_mutex_unlock(&G.mu);
        return def;
    }

    const gchar *sql = "SELECT value FROM security_config WHERE key=?";
    sqlite3_stmt *stmt = NULL;
    gboolean value = def;
    if (sqlite3_prepare_v2(G.db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const gchar *s = col_text(stmt, 0);
            value = g_strcmp0(s, "true") == 0 || g_strcmp0(s, "1") == 0;
        }
        sqlite3_finalize(stmt);
    } else {
        G.degraded = TRUE;
    }
    g_mutex_unlock(&G.mu);
    return value;
}

gboolean
pcv_security_store_set_bool_config(const gchar *key,
                                   gboolean value,
                                   const gchar *admin_user,
                                   GError **error)
{
    if (!key || !*key) {
        g_set_error(error, security_store_error_quark(), SQLITE_MISUSE,
                    "config key is required");
        return FALSE;
    }

    ensure_mutex();
    g_mutex_lock(&G.mu);
    if (!G.db) {
        g_mutex_unlock(&G.mu);
        g_set_error(error, security_store_error_quark(), SQLITE_MISUSE,
                    "security store is not open");
        return FALSE;
    }

    const gchar *sql =
        "INSERT INTO security_config(key,value,updated_at,updated_by) VALUES(?,?,?,?) "
        "ON CONFLICT(key) DO UPDATE SET "
        "value=excluded.value, updated_at=excluded.updated_at, updated_by=excluded.updated_by";
    sqlite3_stmt *stmt = NULL;
    gint rc = sqlite3_prepare_v2(G.db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        set_sqlite_error(error, rc, "prepare set config");
        G.degraded = TRUE;
        g_mutex_unlock(&G.mu);
        return FALSE;
    }

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value ? "true" : "false", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, now_sec());
    sqlite3_bind_text(stmt, 4, admin_user && *admin_user ? admin_user : "system",
                      -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    gboolean ok = (rc == SQLITE_DONE);
    if (!ok) {
        set_sqlite_error(error, rc, "execute set config");
        G.degraded = TRUE;
    }
    g_mutex_unlock(&G.mu);
    return ok;
}

gboolean
pcv_security_store_upsert_pending_action(const PcvSecurityEvent *ev,
                                         const gchar *action,
                                         gint ttl_sec,
                                         GError **error)
{
    if (!ev || !ev->event_id[0] || !action || !*action) {
        g_set_error(error, security_store_error_quark(), SQLITE_MISUSE,
                    "event_id and action are required");
        return FALSE;
    }
    if (ttl_sec <= 0) {
        ttl_sec = 3600;
    }

    ensure_mutex();
    g_mutex_lock(&G.mu);
    if (!G.db) {
        g_mutex_unlock(&G.mu);
        g_set_error(error, security_store_error_quark(), SQLITE_MISUSE,
                    "security store is not open");
        return FALSE;
    }

    const gchar *sql =
        /*
         * Re-opening a still-pending event resets the decision fields. This is
         * deliberate: a new observation should require a fresh operator decision
         * instead of silently inheriting an older approval/dismissal.
         */
        "INSERT INTO security_actions("
        "event_id,action,target_kind,target,status,ttl_sec,expires_at,requested_at"
        ") VALUES(?,?,?,?,?,?,?,?) "
        "ON CONFLICT(event_id) DO UPDATE SET "
        "action=excluded.action, target_kind=excluded.target_kind, target=excluded.target, "
        "status='pending', ttl_sec=excluded.ttl_sec, expires_at=excluded.expires_at, "
        "requested_at=excluded.requested_at, decided_at=0, decided_by='', reason='', "
        "job_id='', error=''";
    sqlite3_stmt *stmt = NULL;
    gint rc = sqlite3_prepare_v2(G.db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        set_sqlite_error(error, rc, "prepare pending action");
        G.degraded = TRUE;
        g_mutex_unlock(&G.mu);
        return FALSE;
    }

    gint64 now = now_sec();
    sqlite3_bind_text(stmt, 1, ev->event_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, action, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, pcv_security_target_kind_to_string(ev->target_kind),
                      -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, ev->target, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, "pending", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 6, ttl_sec);
    sqlite3_bind_int64(stmt, 7, now + ttl_sec);
    sqlite3_bind_int64(stmt, 8, now);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    gboolean ok = (rc == SQLITE_DONE);
    if (!ok) {
        set_sqlite_error(error, rc, "execute pending action");
        G.degraded = TRUE;
    }
    g_mutex_unlock(&G.mu);
    return ok;
}

static JsonObject *
row_to_action_json(sqlite3_stmt *stmt)
{
    /*
     * Pending action JSON is consumed by both approve/dismiss handlers and the
     * UI review table. Preserve job_id/error even when empty so async completion
     * state can be rendered without schema probing.
     */
    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "event_id", col_text(stmt, 0));
    json_object_set_string_member(obj, "action", col_text(stmt, 1));
    json_object_set_string_member(obj, "target_kind", col_text(stmt, 2));
    json_object_set_string_member(obj, "target", col_text(stmt, 3));
    json_object_set_string_member(obj, "status", col_text(stmt, 4));
    json_object_set_int_member(obj, "ttl_sec", sqlite3_column_int(stmt, 5));
    json_object_set_int_member(obj, "expires_at", sqlite3_column_int64(stmt, 6));
    json_object_set_int_member(obj, "requested_at", sqlite3_column_int64(stmt, 7));
    json_object_set_int_member(obj, "decided_at", sqlite3_column_int64(stmt, 8));
    json_object_set_string_member(obj, "decided_by", col_text(stmt, 9));
    json_object_set_string_member(obj, "reason", col_text(stmt, 10));
    json_object_set_string_member(obj, "job_id", col_text(stmt, 11));
    json_object_set_string_member(obj, "error", col_text(stmt, 12));
    return obj;
}

JsonArray *
pcv_security_store_list_pending_actions(void)
{
    JsonArray *arr = json_array_new();

    ensure_mutex();
    g_mutex_lock(&G.mu);
    if (!G.db) {
        G.degraded = TRUE;
        g_mutex_unlock(&G.mu);
        return arr;
    }

    /* [감사 NEW-A2] 이전에는 expires_at를 기록만 하고 어디서도 읽지 않아, TTL이 지난
     * pending HIPS 액션이 무기한 승인 가능 상태로 잔존했다(예: TTL 1h인 block_ip를
     * 3일 뒤 승인). 만료된 pending을 결과에서 제외한다(ttl_sec<=0은 무만료로 취급). */
    const gchar *sql =
        "SELECT event_id,action,target_kind,target,status,ttl_sec,expires_at,requested_at,"
        "decided_at,decided_by,reason,job_id,error "
        "FROM security_actions WHERE status='pending' "
        "AND (ttl_sec <= 0 OR expires_at > ?1) ORDER BY requested_at DESC";
    sqlite3_stmt *stmt = NULL;
    gint rc = sqlite3_prepare_v2(G.db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        G.degraded = TRUE;
        g_mutex_unlock(&G.mu);
        return arr;
    }
    sqlite3_bind_int64(stmt, 1, now_sec());

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        JsonObject *obj = row_to_action_json(stmt);
        json_array_add_object_element(arr, obj);
    }
    if (rc != SQLITE_DONE) {
        G.degraded = TRUE;
    }
    sqlite3_finalize(stmt);
    g_mutex_unlock(&G.mu);
    return arr;
}

JsonObject *
pcv_security_store_get_action(const gchar *event_id)
{
    if (!event_id || !*event_id) {
        return NULL;
    }

    ensure_mutex();
    g_mutex_lock(&G.mu);
    if (!G.db) {
        G.degraded = TRUE;
        g_mutex_unlock(&G.mu);
        return NULL;
    }

    const gchar *sql =
        "SELECT event_id,action,target_kind,target,status,ttl_sec,expires_at,requested_at,"
        "decided_at,decided_by,reason,job_id,error "
        "FROM security_actions WHERE event_id=?";
    sqlite3_stmt *stmt = NULL;
    gint rc = sqlite3_prepare_v2(G.db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        G.degraded = TRUE;
        g_mutex_unlock(&G.mu);
        return NULL;
    }

    sqlite3_bind_text(stmt, 1, event_id, -1, SQLITE_TRANSIENT);
    JsonObject *obj = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        obj = row_to_action_json(stmt);
    }
    sqlite3_finalize(stmt);
    g_mutex_unlock(&G.mu);
    return obj;
}

gboolean
pcv_security_store_update_action_status(const gchar *event_id,
                                        const gchar *status,
                                        const gchar *admin_user,
                                        const gchar *reason,
                                        GError **error)
{
    if (!event_id || !*event_id || !status || !*status) {
        g_set_error(error, security_store_error_quark(), SQLITE_MISUSE,
                    "event_id and status are required");
        return FALSE;
    }

    ensure_mutex();
    g_mutex_lock(&G.mu);
    if (!G.db) {
        g_mutex_unlock(&G.mu);
        g_set_error(error, security_store_error_quark(), SQLITE_MISUSE,
                    "security store is not open");
        return FALSE;
    }

    const gchar *sql =
        /*
         * Only pending actions can transition. Once approved or dismissed, the
         * row becomes decision history and cannot be replayed by a stale button.
         * [감사 NEW-A2] 만료된 pending은 'approved' 전이를 거부한다(TTL 지난 액션을
         * 뒤늦게 승인·실행 차단). dismiss는 만료여도 허용(정리 목적).
         */
        "UPDATE security_actions "
        "SET status=?1, decided_at=?2, decided_by=?3, reason=?4 "
        "WHERE event_id=?5 AND status='pending' "
        "AND (?1 <> 'approved' OR ttl_sec <= 0 OR expires_at > ?6)";
    sqlite3_stmt *stmt = NULL;
    gint rc = sqlite3_prepare_v2(G.db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        set_sqlite_error(error, rc, "prepare action status update");
        G.degraded = TRUE;
        g_mutex_unlock(&G.mu);
        return FALSE;
    }

    sqlite3_bind_text(stmt, 1, status, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, now_sec());
    sqlite3_bind_text(stmt, 3, admin_user && *admin_user ? admin_user : "system",
                      -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, reason ? reason : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, event_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 6, now_sec());
    rc = sqlite3_step(stmt);
    gboolean ok = (rc == SQLITE_DONE && sqlite3_changes(G.db) > 0);
    sqlite3_finalize(stmt);
    if (!ok && rc != SQLITE_DONE) {
        set_sqlite_error(error, rc, "execute action status update");
        G.degraded = TRUE;
    }
    g_mutex_unlock(&G.mu);
    return ok;
}

JsonObject *
pcv_security_store_health(void)
{
    JsonObject *obj = json_object_new();

    ensure_mutex();
    g_mutex_lock(&G.mu);
    gboolean open = G.db != NULL;
    gboolean degraded = G.degraded;
    g_autofree gchar *path = g_strdup(G.path ? G.path : "");
    gint event_count = 0;

    if (G.db) {
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(G.db, "SELECT COUNT(*) FROM security_events",
                               -1, &stmt, NULL) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                event_count = sqlite3_column_int(stmt, 0);
            }
            sqlite3_finalize(stmt);
        } else {
            degraded = TRUE;
            G.degraded = TRUE;
        }
    }
    g_mutex_unlock(&G.mu);

    json_object_set_boolean_member(obj, "open", open);
    json_object_set_boolean_member(obj, "degraded", degraded);
    json_object_set_string_member(obj, "path", path);
    json_object_set_int_member(obj, "event_count", event_count);
    return obj;
}
