/**
 * @file pcv_job_queue.c
 * @brief 통합 작업 큐 — SQLite 기반 비동기 작업 상태 추적
 *
 * [동작 흐름]
 *   pcv_job_create()       → INSERT (PENDING)
 *   pcv_job_update_status() → UPDATE (RUNNING, progress%, detail)
 *   pcv_job_set_result()    → UPDATE (COMPLETED/FAILED, result)
 *   pcv_job_list()          → SELECT (newest first)
 *   pcv_job_get()           → SELECT by job_id
 *   pcv_job_cancel()        → UPDATE status=CANCELLED (PENDING/RUNNING만)
 *
 * [스레드 안전]
 *   GMutex로 모든 DB 접근 직렬화.
 *   SQLite WAL 모드로 읽기/쓰기 동시 접근 지원.
 *
 * [Job ID 형식]
 *   "job-XXXXXXXX" (8자리 hex, g_random_int 기반)
 */
#include "pcv_job_queue.h"
#include "utils/pcv_log.h"
#include "utils/pcv_config.h"
#include <sqlite3.h>
#include <string.h>
#include <time.h>

#define JOB_LOG_DOM "job_queue"
#define JOB_DEFAULT_LIMIT 50

static struct {
    sqlite3  *db;
    GMutex    mu;
    gboolean  initialized;
} G = {0};

static gsize g_mutex_once = 0;

/* 뮤텍스 지연 초기화를 g_once 로 감싼다: 여러 스레드가 첫 job API 를 동시에 밟아도
 * g_mutex_init 이 정확히 한 번만 실행된다. 모든 public 진입점이 락을 잡기 전에
 * ensure_mutex() 를 먼저 호출하므로, 명시적 init 순서에 의존하지 않는다. */
static void
ensure_mutex(void)
{
    if (g_once_init_enter(&g_mutex_once)) {
        g_mutex_init(&G.mu);
        g_once_init_leave(&g_mutex_once, 1);
    }
}

/* ── 상태 → 문자열 변환 ─────────────────────────────────────── */
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

/* ── 생명주기 ─────────────────────────────────────────────────── */

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

    /* 테스트 격리: 환경변수 우선 (PCV_JOBS_DB_PATH > daemon.conf > 기본값) */
    const gchar *env_path = g_getenv("PCV_JOBS_DB_PATH");
    const gchar *db_path = env_path && *env_path
        ? env_path
        : pcv_config_get_string("jobs", "db_path",
                                "/var/lib/purecvisor/pcv_jobs.db");

    /* DB 를 못 열면 큐를 "비활성"으로 두고(G.db=NULL) 여전히 initialized=TRUE 로
     * 표시한다. 이후 모든 변경 API 는 G.db==NULL 을 보고 조용히 no-op 이 되어,
     * job 상태 추적이 실패해도 실제 VM/스토리지 작업 자체는 계속 진행되게 한다
     * (job 큐는 관측 계층이지 작업의 전제조건이 아니다). */
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

    /* WAL: 읽기(list/get)와 쓰기(update)가 서로를 블록하지 않게 한다 — UI 폴링이
     * 진행 중인 작업의 진행률 UPDATE 를 막지 않도록. */
    sqlite3_exec(G.db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
    sqlite3_exec(G.db,
        "CREATE TABLE IF NOT EXISTS jobs ("
        "  job_id TEXT PRIMARY KEY,"
        "  type TEXT NOT NULL,"
        "  target TEXT,"
        "  status INTEGER DEFAULT 0,"
        "  progress INTEGER DEFAULT 0,"
        "  detail TEXT,"
        "  params TEXT,"
        "  result TEXT,"
        "  created_at INTEGER,"
        "  updated_at INTEGER"
        ")", NULL, NULL, NULL);
    sqlite3_exec(G.db,
        "CREATE INDEX IF NOT EXISTS idx_jobs_status ON jobs(status)",
        NULL, NULL, NULL);
    sqlite3_exec(G.db,
        "CREATE INDEX IF NOT EXISTS idx_jobs_created ON jobs(created_at DESC)",
        NULL, NULL, NULL);

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

    /* status >= 2 는 종료 상태(completed/failed/cancelled)만 지운다는 뜻이다.
     * pending(0)/running(1)은 아직 워커가 갱신 중일 수 있어 age 와 무관하게 보존 —
     * 진행 중 작업 레코드를 지우면 UI 가 "사라진 작업"으로 오인한다. */
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

/* ── 작업 생성/갱신 ──────────────────────────────────────────── */

gchar *
pcv_job_create(const gchar *type, const gchar *target,
                const gchar *params_json)
{
    gchar *job_id = g_strdup_printf("job-%08x", g_random_int());
    gint64 now = (gint64)time(NULL);

    ensure_mutex();

    /* 큐가 비활성이어도 job_id 는 먼저 만들어 반환한다: 호출자(핸들러)는 이 ID 를
     * accepted 응답에 실어 클라이언트에 이미 돌려주므로, DB 부재로 인해 ID 계약이
     * 깨지면 안 된다. 이 경우 이후 상태 조회만 비게 된다(작업 자체는 진행). */
    g_mutex_lock(&G.mu);
    if (!G.db) {
        g_mutex_unlock(&G.mu);
        PCV_LOG_WARN(JOB_LOG_DOM, "Job queue not initialized, returning ID only");
        return job_id;
    }

    const gchar *sql =
        "INSERT INTO jobs(job_id,type,target,status,progress,params,created_at,updated_at)"
        " VALUES(?,?,?,0,0,?,?,?)";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(G.db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, job_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, type ? type : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, target, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, params_json, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 5, now);
        sqlite3_bind_int64(stmt, 6, now);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    g_mutex_unlock(&G.mu);

    PCV_LOG_INFO(JOB_LOG_DOM, "Job created: %s type=%s target=%s",
                 job_id, type ? type : "?", target ? target : "?");
    return job_id;
}

void
pcv_job_update_status(const gchar *job_id, PcvJobStatus status,
                       gint progress_pct, const gchar *detail)
{
    if (!job_id) return;
    gint64 now = (gint64)time(NULL);

    ensure_mutex();

    g_mutex_lock(&G.mu);
    if (!G.db) { g_mutex_unlock(&G.mu); return; }

    const gchar *sql =
        "UPDATE jobs SET status=?, progress=?, detail=?, updated_at=? WHERE job_id=?";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(G.db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, (int)status);
        sqlite3_bind_int(stmt, 2, progress_pct);
        sqlite3_bind_text(stmt, 3, detail, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 4, now);
        sqlite3_bind_text(stmt, 5, job_id, -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    g_mutex_unlock(&G.mu);
}

void
pcv_job_set_result(const gchar *job_id, PcvJobStatus status,
                    const gchar *result_json)
{
    if (!job_id) return;
    gint64 now = (gint64)time(NULL);

    ensure_mutex();

    g_mutex_lock(&G.mu);
    if (!G.db) { g_mutex_unlock(&G.mu); return; }

    const gchar *sql =
        "UPDATE jobs SET status=?, progress=100, result=?, updated_at=? WHERE job_id=?";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(G.db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, (int)status);
        sqlite3_bind_text(stmt, 2, result_json, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 3, now);
        sqlite3_bind_text(stmt, 4, job_id, -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    g_mutex_unlock(&G.mu);

    PCV_LOG_INFO(JOB_LOG_DOM, "Job %s finished: %s", job_id, _status_str(status));
}

/* ── 조회 ────────────────────────────────────────────────────── */

/**
 * _row_to_json:
 * sqlite3_stmt의 현재 행을 JsonObject로 변환합니다.
 */
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

    /* PENDING(0) 또는 RUNNING(1) 상태만 취소 가능 */
    /* status < 2 가드가 취소를 non-terminal(pending/running)로 한정한다: 이미
     * 끝난 작업을 cancelled 로 되돌리지 않는다. WHERE 가 0행을 바꾸면(이미 종료
     * 또는 없는 job) sqlite3_changes==0 → FALSE 를 돌려주어 호출자가 "취소 못 함"을
     * 구분할 수 있다. 실제 워커 중단은 이 레코드 마킹과 별개다(협조적 취소). */
    const gchar *sql =
        "UPDATE jobs SET status=4, detail='Cancelled by user', updated_at=?"
        " WHERE job_id=? AND status < 2";
    sqlite3_stmt *stmt;
    gboolean ok = FALSE;
    if (sqlite3_prepare_v2(G.db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, now);
        sqlite3_bind_text(stmt, 2, job_id, -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        ok = (sqlite3_changes(G.db) > 0);
        sqlite3_finalize(stmt);
    }
    g_mutex_unlock(&G.mu);

    if (ok)
        PCV_LOG_INFO(JOB_LOG_DOM, "Job cancelled: %s", job_id);
    return ok;
}
