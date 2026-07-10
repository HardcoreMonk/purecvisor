/**
 * @file pcv_audit.c
 * @brief 감사 로그 — SQLite WAL + 파일 이중 기록, GAsyncQueue 비동기 워커
 *
 * [동작 흐름]
 *   pcv_audit_log() → PcvAuditRecord 복사 → GAsyncQueue push
 *   워커 GThread → pop → SQLite INSERT + 파일 append
 *
 * [스레드 안전]
 *   GAsyncQueue: lock-free push/pop
 *   SQLite: WAL 모드 (동시 읽기 허용)
 */
#include "pcv_audit.h"
#include "utils/pcv_log.h"
#include <sqlite3.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <time.h>

#define AUDIT_LOG_DOM "audit"
constexpr int AUDIT_QUEUE_MAX      = 10000;
constexpr int AUDIT_RETENTION_DAYS = 30;
constexpr int AUDIT_CLEANUP_INTERVAL = 3600;   /* 1시간마다 retention 정리 */
constexpr int AUDIT_DB_MAX_PAGES   = 262144;   /* ~1GB SQLite 상한 (4KB page × 262144) */
constexpr int AUDIT_RATE_LIMIT     = 1000;     /* 초당 최대 감사 레코드 수 (토큰 버킷) */

/* ── C23 컴파일 타임 검증 ─────────────────────────────────── */
static_assert(AUDIT_RETENTION_DAYS >= 1, "Must retain at least 1 day");
static_assert(AUDIT_DB_MAX_PAGES >= 1024, "DB too small");

static struct {
    sqlite3       *db;
    GAsyncQueue   *queue;
    GThread       *worker;
    gboolean       running;
    gint64         total_count;
    gint64         dropped_count;  /* 큐 오버플로로 드롭된 레코드 수 */
    gchar         *node_name;
    gint64         last_cleanup;   /* monotonic time of last retention cleanup */
} G = {0};

/**
 * _audit_record_free:
 * @rec: 해제할 감사 레코드 (NULL 안전)
 *
 * PcvAuditRecord의 모든 동적 할당 멤버를 해제합니다.
 * 워커 스레드에서 처리 완료 후 호출됩니다.
 */
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

/**
 * _audit_record_copy:
 *
 * 감사 로그 파라미터를 깊은 복사하여 새 PcvAuditRecord를 생성합니다.
 * GAsyncQueue에 push하기 전에 호출하며, 호출자 스레드의 스택 변수가
 * 워커 스레드에서 접근될 때 이미 해제되어 있을 수 있으므로
 * 반드시 g_strdup으로 복사해야 합니다.
 *
 * NULL 파라미터에 대한 기본값: username="-", method="?", 나머지=""
 *
 * Returns: (transfer full): 새 PcvAuditRecord* (_audit_record_free 필요)
 */
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
    /* 이벤트 발생시각(epoch µs, UTC) — pcv_audit_log 호출 즉시 캡처.
     * 워커의 INSERT 시각(ts=datetime('now'))과 달리 큐 지연에 영향받지 않는다. */
    rec->event_us    = g_get_real_time();
    return rec;
}

/**
 * _audit_worker:
 *
 * 전용 워커 스레드 — GAsyncQueue에서 감사 레코드를 꺼내 처리합니다.
 *
 * 처리 흐름 (무한 루프):
 *   1. g_async_queue_timeout_pop(500ms) — 레코드 대기
 *   2. SQLite INSERT (prepared statement 재사용으로 성능 확보)
 *   3. 파일 로그 (PCV_LOG_AUDIT)
 *   4. 레코드 해제
 *
 * 종료 조건: G.running=FALSE 이고 큐가 비어있으면 루프 탈출
 * 종료 시에도 큐에 남은 레코드를 모두 처리한 후 종료합니다 (graceful drain).
 */
static gpointer
_audit_worker(gpointer data __attribute__((unused)))
{
    sqlite3_stmt *stmt = NULL;
    const gchar *sql =
        "INSERT INTO audit_log(ts,node,username,method,target,result,error_code,duration_ms,src_ip,event_ts) "
        "VALUES(datetime('now'),?,?,?,?,?,?,?,?,?)";

    if (G.db)
        sqlite3_prepare_v2(G.db, sql, -1, &stmt, NULL);

    while (G.running || g_async_queue_length(G.queue) > 0) {
        PcvAuditRecord *rec = g_async_queue_timeout_pop(G.queue, 500000); /* 500ms */
        if (!rec) continue;

        /* SQLite INSERT */
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
            /* event_ts: 발생시각(rec->event_us)을 UTC ISO8601(µs 포함)로 포맷.
             * ts(datetime('now'))가 초 단위 기록시각인 반면, event_ts는 발생
             * 절대시각을 µs 정밀도로 보존해 큐 지연 분석을 가능하게 한다. */
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
            sqlite3_bind_text(stmt, 9, event_ts ? event_ts : "", -1,
                              SQLITE_TRANSIENT);
            int rc = sqlite3_step(stmt);
            if (rc != SQLITE_DONE) {
                g_warning("[audit] SQLite INSERT failed: %s", sqlite3_errmsg(G.db));
            }
            g_free(event_ts);
        }

        /* 파일 기록 (기존 PCV_LOG_AUDIT 경로) */
        PCV_LOG_AUDIT(AUDIT_LOG_DOM, rec->method, rec->target,
                       "user=%s result=%s code=%d dur=%ldms",
                       rec->username, rec->result, rec->error_code,
                       (long)rec->duration_ms);

        G.total_count++;
        _audit_record_free(rec);

        /* 주기적 retention 정리 (AUDIT_CLEANUP_INTERVAL 초마다) */
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

            /* WAL 체크포인트 — retention 정리와 동일 1시간 주기 */
            sqlite3_wal_checkpoint_v2(G.db, nullptr,
                                      SQLITE_CHECKPOINT_PASSIVE,
                                      nullptr, nullptr);
        }
    }

    if (stmt) sqlite3_finalize(stmt);
    return NULL;
}

/**
 * pcv_audit_init:
 * @db_path: SQLite DB 경로 (예: "/var/lib/purecvisor/pcv_audit.db")
 *
 * 감사 로그 모듈을 초기화합니다:
 *   1) 호스트명 획득 (노드 식별용)
 *   2) SQLite DB 열기 + WAL 모드 + 테이블/인덱스 생성
 *   3) GAsyncQueue 생성 + 워커 스레드 시작
 *
 * SQLite 열기 실패 시에도 파일 로그만으로 동작합니다 (graceful degradation).
 * WAL 모드는 읽기/쓰기 동시 접근을 허용하여 성능을 향상시킵니다.
 */
void
pcv_audit_init(const gchar *db_path)
{
    gchar hostname[64] = {0};
    gethostname(hostname, sizeof(hostname) - 1);
    G.node_name = g_strdup(hostname);

    /* SQLite 열기 (WAL 모드) */
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
            "  event_ts TEXT,"
            "  node TEXT NOT NULL,"
            "  username TEXT,"
            "  method TEXT NOT NULL,"
            "  target TEXT,"
            "  result TEXT NOT NULL,"
            "  error_code INTEGER,"
            "  duration_ms INTEGER,"
            "  src_ip TEXT"
            ")", NULL, NULL, NULL);
        /* 기존 DB 마이그레이션(멱등): event_ts 컬럼이 없으면 추가한다.
         * CREATE TABLE IF NOT EXISTS는 이미 존재하는 테이블에 새 컬럼을
         * 반영하지 않으므로, PRAGMA table_info로 부재 확인 후 ALTER 한다. */
        {
            gboolean has_event_ts = FALSE;
            sqlite3_stmt *pragma_stmt = NULL;
            if (sqlite3_prepare_v2(G.db, "PRAGMA table_info(audit_log)",
                                   -1, &pragma_stmt, NULL) == SQLITE_OK) {
                while (sqlite3_step(pragma_stmt) == SQLITE_ROW) {
                    const gchar *cname =
                        (const gchar *)sqlite3_column_text(pragma_stmt, 1);
                    if (cname && g_strcmp0(cname, "event_ts") == 0) {
                        has_event_ts = TRUE;
                        break;
                    }
                }
                sqlite3_finalize(pragma_stmt);
            }
            if (!has_event_ts) {
                sqlite3_exec(G.db,
                    "ALTER TABLE audit_log ADD COLUMN event_ts TEXT",
                    NULL, NULL, NULL);
            }
        }
        sqlite3_exec(G.db, "CREATE INDEX IF NOT EXISTS idx_audit_ts ON audit_log(ts)",
                     NULL, NULL, NULL);
        sqlite3_exec(G.db, "CREATE INDEX IF NOT EXISTS idx_audit_method ON audit_log(method)",
                     NULL, NULL, NULL);
        /* DB 크기 상한 (~1GB) */
        gchar *pragma = g_strdup_printf("PRAGMA max_page_count=%d", AUDIT_DB_MAX_PAGES);
        sqlite3_exec(G.db, pragma, NULL, NULL, NULL);
        g_free(pragma);
    }

    /* 비동기 큐 + 워커 스레드 */
    G.queue = g_async_queue_new();
    G.running = TRUE;
    G.total_count = 0;
    G.worker = g_thread_new("audit-writer", _audit_worker, NULL);

    PCV_LOG_INFO(AUDIT_LOG_DOM, "Audit trail initialized (db=%s, node=%s)",
                 db_path, G.node_name);
}

/**
 * pcv_audit_shutdown:
 *
 * 감사 로그 모듈을 종료합니다.
 * 순서: 워커 중지 플래그 → 스레드 join(큐 drain) → 큐 해제 → SQLite 닫기
 *
 * g_thread_join()이 워커의 남은 레코드 처리를 기다리므로,
 * 종료 시 큐에 쌓인 감사 로그가 유실되지 않습니다.
 */
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

/**
 * pcv_audit_log:
 * @username:    사용자 이름 (JWT에서 추출, NULL이면 "-")
 * @method:      RPC 메서드 또는 액션 이름 (예: "vm.create")
 * @target:      대상 리소스 (예: VM 이름, 네트워크 이름)
 * @result:      결과 설명 (예: "ok", "error: ...")
 * @error_code:  에러 코드 (성공 시 0)
 * @duration_ms: 처리 시간 (밀리초)
 * @src_ip:      요청 소스 IP (REST=클라이언트 IP, UDS="local")
 *
 * 감사 레코드를 비동기 큐에 추가합니다 (논블로킹).
 * 워커 스레드가 큐에서 꺼내 SQLite + 파일에 기록합니다.
 *
 * 큐 크기가 AUDIT_QUEUE_MAX(10000)을 초과하면 드롭합니다 (과부하 보호).
 */
void
pcv_audit_log(const gchar *username, const gchar *method,
               const gchar *target, const gchar *result,
               gint error_code, gint64 duration_ms,
               const gchar *src_ip)
{
    if (!G.running || !G.queue) return;

    /* Rate limiting — 초당 최대 AUDIT_RATE_LIMIT건 (토큰 버킷)
     * GMutex로 bucket_ts/bucket_tokens 쌍의 원자적 갱신을 보장합니다.
     * pcv_audit_log()는 여러 GTask 워커 스레드에서 동시 호출될 수 있으므로
     * 두 변수를 함께 읽고-갱신하는 구간을 직렬화해야 합니다. */
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
            /* Emit warning at exponential thresholds so admins notice drops */
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

    /* 큐 크기 제한 — 초과 시 드롭 + 경고 */
    if (qlen >= AUDIT_QUEUE_MAX) {
        G.dropped_count++;
        gint64 dc = G.dropped_count;
        /* Threshold alert at exponential milestones for early detection */
        if (dc == 1 || dc == 10 || dc == 100 || dc == 1000 ||
            dc == 10000 || dc == 100000) {
            g_warning("[audit] queue overflow: %ld records dropped total", (long)dc);
        }
        /* Periodic 60s warning for sustained overflow */
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

    /* 90% 경고 — 큐 포화 임박 감지 */
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

/**
 * pcv_audit_log_rpc:
 *
 * RPC 핸들러에서 간편하게 호출할 수 있는 감사 로그 래퍼.
 * username, target, src_ip를 NULL로 전달합니다.
 */
void
pcv_audit_log_rpc(const gchar *method, const gchar *result,
                   gint error_code, gint64 duration_ms)
{
    pcv_audit_log(NULL, method, NULL, result, error_code, duration_ms, NULL);
}

/** pcv_audit_get_total_count: 데몬 시작 이후 기록된 총 감사 레코드 수 반환 */
gint64
pcv_audit_get_total_count(void)
{
    return G.total_count;
}

/** pcv_audit_get_queue_depth: 비동기 큐 대기 레코드 수 반환 (Prometheus 메트릭용) */
gint
pcv_audit_get_queue_depth(void)
{
    if (!G.queue) return 0;
    return g_async_queue_length(G.queue);
}

/** pcv_audit_get_dropped_count: 큐 오버플로로 드롭된 레코드 수 반환 */
gint64
pcv_audit_get_dropped_count(void)
{
    return G.dropped_count;
}

/**
 * pcv_audit_search:
 * @from_ts:        검색 시작 시각 (ISO 형식, NULL이면 제한 없음)
 * @to_ts:          검색 종료 시각 (ISO 형식, NULL이면 제한 없음)
 * @username:       사용자 필터 (정확 매칭, NULL이면 전체)
 * @method_pattern: 메서드 패턴 (LIKE 패턴, NULL이면 전체)
 * @limit:          최대 결과 수 (0 이하이면 기본값 100)
 *
 * audit_log 테이블에서 조건에 맞는 감사 레코드를 검색합니다.
 * SQLite 바인드 파라미터로 SQL injection을 방지합니다.
 *
 * Returns: (transfer full): JsonArray* — 호출자가 json_array_unref() 필요
 */
JsonArray *
pcv_audit_recent_failures(const gchar *target_filter, gint limit)
{
    JsonArray *arr = json_array_new();
    if (!G.db) return arr;

    /* result='fail' 인 레코드만, target 옵션 필터링 */
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
        /* UI는 'message' 필드를 기대 — error_code 기반 메시지 또는 method/target에서 합성 */
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
