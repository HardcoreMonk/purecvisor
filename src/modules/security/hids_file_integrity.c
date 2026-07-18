/**
 * @file hids_file_integrity.c
 * @brief HIDS 파일 무결성 구현 — TOCTOU-내성 해싱·admin refresh·drift 스캔
 *
 * hids_file_integrity.h 계약의 구현. baseline 은 security DB 계열의 SQLite 파일
 * (file_baseline 테이블)에 저장된다.
 *
 * [아키텍처 위치]
 *   Native HIDS v1 수집단. refresh(admin) 로 신뢰 스냅샷을 심고, scan(무부작용) 으로
 *   현재 파일과 비교해 drift 를 JSON 으로 반환한다. 상위 계층이 이를 PcvSecurityEvent
 *   로 승격하거나 UI/진단에 노출한다.
 *
 * [판단 근거 — 위변조 내성]
 *   compute_file_state 는 경로를 단 한 번만 해소한다: O_NOFOLLOW 로 열어 심볼릭 링크
 *   스왑을 open 단계에서 실패시키고, 같은 fd 에서 fstat(메타데이터)와 스트리밍 해시
 *   (내용)를 모두 얻는다. stat→별도 읽기의 이중 해소는 TOCTOU 경쟁·링크 스왑에 뚫리는
 *   패턴이라 폐기했다.
 *
 * [불변식 — ADR-0024]
 *   scan 은 baseline 을 절대 쓰지 않는다. refresh 만 baseline 을 교체하며 트랜잭션
 *   (BEGIN IMMEDIATE + DELETE + 재삽입 + COMMIT) 으로 원자적이다 — 중간 실패 시
 *   ROLLBACK 으로 이전 신뢰 스냅샷을 보존한다. DB 를 열 수 없으면 status 는 UNKNOWN
 *   으로 남아 없는 baseline 을 trusted 로 신뢰하지 않는다.
 *
 * Operator note:
 *   이 파일은 "중요 파일이 몰래 바뀌었는지"를 감시한다. baseline 은 admin 이 직접
 *   refresh 해야만 신뢰 상태가 되며, 그 전까지는 모든 파일이 UNKNOWN 으로 취급되어
 *   변경 여부를 단정하지 않는다(오탐으로 정상 변경을 위협으로 신고하지 않기 위함).
 */
#include "modules/security/hids_file_integrity.h"
#include "modules/audit/pcv_audit.h"

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <sqlite3.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/*
 * Native HIDS v1 stores a tiny file baseline in the same security DB family.
 * Refresh is explicit admin action; scan only reports drift and never mutates
 * trusted state.
 */
static GQuark
hids_file_integrity_error_quark(void)
{
    return g_quark_from_static_string("pcv-hids-file-integrity");
}

static gint64
now_sec(void)
{
    return (gint64)time(NULL);
}

static void
set_sqlite_error(sqlite3 *db, GError **error, gint code, const gchar *context)
{
    if (!error) {
        return;
    }
    g_set_error(error, hids_file_integrity_error_quark(), code,
                "%s: %s", context, db ? sqlite3_errmsg(db) : "database is closed");
}

static gboolean
exec_sql(sqlite3 *db, const gchar *sql, GError **error)
{
    gchar *errmsg = NULL;
    gint rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        if (error) {
            g_set_error(error, hids_file_integrity_error_quark(), rc,
                        "%s", errmsg ? errmsg : sqlite3_errmsg(db));
        }
        sqlite3_free(errmsg);
        return FALSE;
    }
    return TRUE;
}

static gboolean
init_schema(sqlite3 *db, GError **error)
{
    static const gchar *schema[] = {
        "PRAGMA journal_mode=WAL",
        "CREATE TABLE IF NOT EXISTS file_baseline ("
        "  path TEXT PRIMARY KEY,"
        "  sha256 TEXT NOT NULL,"
        "  size INTEGER NOT NULL,"
        "  mode INTEGER NOT NULL,"
        "  mtime INTEGER NOT NULL,"
        "  refreshed_at INTEGER NOT NULL,"
        "  refreshed_by TEXT NOT NULL"
        ")",
    };

    for (gsize i = 0; i < G_N_ELEMENTS(schema); i++) {
        if (!exec_sql(db, schema[i], error)) {
            return FALSE;
        }
    }
    return TRUE;
}

static gboolean
open_db(const gchar *db_path, sqlite3 **out_db, GError **error)
{
    if (!db_path || !*db_path) {
        g_set_error(error, hids_file_integrity_error_quark(), SQLITE_MISUSE,
                    "database path is required");
        return FALSE;
    }

    sqlite3 *db = NULL;
    gint rc = sqlite3_open(db_path, &db);
    if (rc != SQLITE_OK) {
        set_sqlite_error(db, error, rc, "open baseline database");
        if (db) {
            sqlite3_close(db);
        }
        return FALSE;
    }

    if (!init_schema(db, error)) {
        sqlite3_close(db);
        return FALSE;
    }

    *out_db = db;
    return TRUE;
}

static gboolean
compute_file_state(const gchar *path,
                   gchar **out_sha256,
                   gint64 *out_size,
                   gint64 *out_mode,
                   gint64 *out_mtime,
                   GError **error)
{
    /*
     * Tamper-evidence requires a SINGLE path resolution. We open the path once
     * with O_NOFOLLOW (so a symlink swapped in at the final path component fails
     * the open outright) and derive BOTH metadata (fstat) and the content hash
     * (streamed read) from that same fd. The old code did g_stat() then a
     * separate g_file_get_contents(): two independent resolutions that both
     * followed symlinks, which a TOCTOU race or symlink swap defeats.
     *
     * Hashing streams the fd incrementally via GChecksum. g_checksum_get_string
     * returns the same lowercase SHA-256 hex as g_compute_checksum_for_data, so
     * existing baselines remain byte-for-byte comparable.
     */
    if (!path || !*path) {
        g_set_error(error, hids_file_integrity_error_quark(), SQLITE_MISUSE,
                    "file path is required");
        return FALSE;
    }

    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        gint saved_errno = errno;
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                    "open %s: %s", path, g_strerror(saved_errno));
        return FALSE;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        gint saved_errno = errno;
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                    "stat %s: %s", path, g_strerror(saved_errno));
        close(fd);
        return FALSE;
    }
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        g_set_error(error, hids_file_integrity_error_quark(), SQLITE_MISUSE,
                    "%s is not a regular file", path);
        return FALSE;
    }

    GChecksum *ck = g_checksum_new(G_CHECKSUM_SHA256);
    guchar buf[65536];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        g_checksum_update(ck, buf, n);
    }
    if (n < 0) {
        gint saved_errno = errno;
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                    "read %s: %s", path, g_strerror(saved_errno));
        g_checksum_free(ck);
        close(fd);
        return FALSE;
    }

    *out_sha256 = g_strdup(g_checksum_get_string(ck));
    g_checksum_free(ck);
    *out_size = (gint64)st.st_size;
    *out_mode = (gint64)st.st_mode;
    *out_mtime = (gint64)st.st_mtime;
    close(fd);
    return TRUE;
}

static gboolean
insert_baseline_row(sqlite3 *db,
                    const gchar *path,
                    const gchar *sha256,
                    gint64 size,
                    gint64 mode,
                    gint64 mtime,
                    gint64 refreshed_at,
                    const gchar *refreshed_by,
                    GError **error)
{
    const gchar *sql =
        "INSERT OR REPLACE INTO file_baseline"
        "(path,sha256,size,mode,mtime,refreshed_at,refreshed_by) "
        "VALUES(?,?,?,?,?,?,?)";
    sqlite3_stmt *stmt = NULL;
    gint rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        set_sqlite_error(db, error, rc, "prepare baseline insert");
        return FALSE;
    }

    sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, sha256, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, size);
    sqlite3_bind_int64(stmt, 4, mode);
    sqlite3_bind_int64(stmt, 5, mtime);
    sqlite3_bind_int64(stmt, 6, refreshed_at);
    sqlite3_bind_text(stmt, 7, refreshed_by ? refreshed_by : "-", -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        set_sqlite_error(db, error, rc, "execute baseline insert");
        return FALSE;
    }
    return TRUE;
}

PcvHidsBaselineStatus
pcv_hids_baseline_status(const gchar *db_path)
{
    /*
     * "trusted" means an admin has refreshed at least one baseline row. Missing
     * or unreadable DBs stay unknown rather than silently trusting the host.
     */
    sqlite3 *db = NULL;
    GError *error = NULL;
    if (!open_db(db_path, &db, &error)) {
        g_clear_error(&error);
        return PCV_HIDS_BASELINE_UNKNOWN;
    }

    const gchar *sql = "SELECT COUNT(*) FROM file_baseline";
    sqlite3_stmt *stmt = NULL;
    gint rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_close(db);
        return PCV_HIDS_BASELINE_UNKNOWN;
    }

    gint64 rows = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        rows = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    return rows > 0 ? PCV_HIDS_BASELINE_TRUSTED : PCV_HIDS_BASELINE_UNKNOWN;
}

gboolean
pcv_hids_baseline_refresh(const gchar *db_path,
                          const gchar * const *paths,
                          gsize n_paths,
                          const gchar *admin_user,
                          GError **error)
{
    if (!paths || n_paths == 0) {
        g_set_error(error, hids_file_integrity_error_quark(), SQLITE_MISUSE,
                    "at least one baseline path is required");
        return FALSE;
    }

    sqlite3 *db = NULL;
    if (!open_db(db_path, &db, error)) {
        return FALSE;
    }

    gboolean ok = FALSE;
    gboolean in_txn = FALSE;
    if (!exec_sql(db, "BEGIN IMMEDIATE", error)) {
        goto out;
    }
    in_txn = TRUE;
    if (!exec_sql(db, "DELETE FROM file_baseline", error)) {
        goto out;
    }

    gint64 refreshed_at = now_sec();
    for (gsize i = 0; i < n_paths; i++) {
        g_autofree gchar *sha256 = NULL;
        gint64 size = 0;
        gint64 mode = 0;
        gint64 mtime = 0;
        if (!compute_file_state(paths[i], &sha256, &size, &mode, &mtime, error)) {
            goto out;
        }
        if (!insert_baseline_row(db, paths[i], sha256, size, mode, mtime,
                                 refreshed_at, admin_user, error)) {
            goto out;
        }
    }

    if (!exec_sql(db, "COMMIT", error)) {
        goto out;
    }
    in_txn = FALSE;
    ok = TRUE;

out:
    if (!ok && in_txn) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    }
    sqlite3_close(db);

    if (ok) {
        pcv_audit_log(admin_user, "security.baseline.refresh", "file_baseline",
                      "ok", 0, 0, "local");
    }
    return ok;
}

static JsonObject *
build_scan_event(const gchar *path,
                 const gchar *status,
                 const gchar *expected_sha256,
                 const gchar *actual_sha256,
                 GError *error)
{
    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "path", path ? path : "");
    json_object_set_string_member(obj, "status", status);
    if (expected_sha256) {
        json_object_set_string_member(obj, "expected_sha256", expected_sha256);
    }
    if (actual_sha256) {
        json_object_set_string_member(obj, "actual_sha256", actual_sha256);
    }
    if (error) {
        json_object_set_string_member(obj, "error", error->message);
    }
    return obj;
}

static void
scan_one_path(sqlite3 *db, GPtrArray *changes, const gchar *path)
{
    /*
     * Every scan finding is returned as JSON so callers can decide whether to
     * convert it into a SecurityEvent, show it in UI, or keep it as diagnostics.
     */
    g_autoptr(GError) error = NULL;
    g_autofree gchar *sha256 = NULL;
    gint64 size = 0;
    gint64 mode = 0;
    gint64 mtime = 0;
    if (!compute_file_state(path, &sha256, &size, &mode, &mtime, &error)) {
        g_ptr_array_add(changes, build_scan_event(path, "unreadable", NULL, NULL, error));
        return;
    }

    const gchar *sql =
        "SELECT sha256,size,mode,mtime FROM file_baseline WHERE path=?";
    sqlite3_stmt *stmt = NULL;
    gint rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        g_ptr_array_add(changes, build_scan_event(path, "scan_error", NULL, sha256, NULL));
        return;
    }

    sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
        g_ptr_array_add(changes, build_scan_event(path, "missing_baseline", NULL, sha256, NULL));
    } else if (rc == SQLITE_ROW) {
        const gchar *expected_sha256 = (const gchar *)sqlite3_column_text(stmt, 0);
        gint64 expected_size = sqlite3_column_int64(stmt, 1);
        gint64 expected_mode = sqlite3_column_int64(stmt, 2);
        gint64 expected_mtime = sqlite3_column_int64(stmt, 3);

        if (g_strcmp0(expected_sha256, sha256) != 0 ||
            expected_size != size ||
            expected_mode != mode ||
            expected_mtime != mtime) {
            g_ptr_array_add(changes, build_scan_event(path, "changed",
                                                      expected_sha256, sha256, NULL));
        }
    } else {
        g_ptr_array_add(changes, build_scan_event(path, "scan_error", NULL, sha256, NULL));
    }
    sqlite3_finalize(stmt);
}

GPtrArray *
pcv_hids_file_integrity_scan(const gchar *db_path,
                             const gchar * const *paths,
                             gsize n_paths)
{
    GPtrArray *changes = g_ptr_array_new_with_free_func((GDestroyNotify)json_object_unref);
    if (!paths || n_paths == 0) {
        return changes;
    }

    sqlite3 *db = NULL;
    GError *error = NULL;
    if (!open_db(db_path, &db, &error)) {
        g_clear_error(&error);
        return changes;
    }

    for (gsize i = 0; i < n_paths; i++) {
        scan_one_path(db, changes, paths[i]);
    }

    sqlite3_close(db);
    return changes;
}
