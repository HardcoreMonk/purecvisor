#include "modules/core/vm_config_history_store.h"

#include <errno.h>
#include <fcntl.h>
#include <openssl/rand.h>
#include <sqlite3.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "modules/virt/vm_config_history_norm.h"

#define VMCH_ERROR (g_quark_from_static_string("pcv-vm-cfg-store"))

struct _PcvVmCfgStore {
    sqlite3 *db;
    gchar   *path;
    gboolean writer;
    gboolean created;
    guint8   salt[PCV_VMCH_SALT_LEN];
    gboolean have_salt;
    gchar    salt_id[17];
};

static const char k_schema[] =
    "CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);"
    "CREATE TABLE IF NOT EXISTS coverage_epoch (id INTEGER PRIMARY KEY AUTOINCREMENT, started_at INTEGER NOT NULL,"
    " last_alive_at INTEGER NOT NULL, ended_at INTEGER, end_reason TEXT);"
    "CREATE TABLE IF NOT EXISTS config_change (id INTEGER PRIMARY KEY AUTOINCREMENT, entry_id TEXT NOT NULL UNIQUE,"
    " vm_uuid TEXT NOT NULL, vm_name TEXT NOT NULL, owner TEXT, owner_tenant TEXT, kind TEXT NOT NULL, scope TEXT NOT NULL,"
    " cause TEXT NOT NULL, observed_at INTEGER NOT NULL, window_start INTEGER, attribution TEXT NOT NULL, actor TEXT,"
    " actor_tenant TEXT, actor_expired INTEGER NOT NULL DEFAULT 0, actor_method TEXT, actor_result TEXT,"
    " config_digest TEXT NOT NULL, live_digest TEXT, snapshot_json TEXT NOT NULL, changes_json TEXT NOT NULL,"
    " prev_id INTEGER, normalizer_version INTEGER NOT NULL, salt_id TEXT NOT NULL,"
    " epoch_id INTEGER NOT NULL REFERENCES coverage_epoch(id));"
    "CREATE INDEX IF NOT EXISTS ix_cc_uuid ON config_change(vm_uuid, id);"
    "CREATE INDEX IF NOT EXISTS ix_cc_observed ON config_change(observed_at, entry_id);"
    "CREATE INDEX IF NOT EXISTS ix_cc_scope ON config_change(owner_tenant, owner, observed_at);"
    "CREATE TABLE IF NOT EXISTS vm_latest (vm_uuid TEXT PRIMARY KEY, last_id INTEGER NOT NULL,"
    " last_confirmed_at INTEGER NOT NULL, present INTEGER NOT NULL, current_name TEXT, run_id INTEGER,"
    " run_started_at INTEGER, run_live_digest TEXT);"
    "CREATE INDEX IF NOT EXISTS ix_latest_name ON vm_latest(current_name) WHERE present = 1;";

#define ROW_COLS                                                                                                  \
    "id, entry_id, vm_uuid, vm_name, owner, owner_tenant, kind, scope, cause, observed_at, window_start, "      \
    "attribution, actor, actor_tenant, actor_expired, actor_method, actor_result, config_digest, live_digest, " \
    "snapshot_json, changes_json, prev_id, normalizer_version, salt_id, epoch_id"

static void
_fail(GError **err, sqlite3 *db, const gchar *what)
{
    g_set_error(err, VMCH_ERROR, 1, "%s: %s", what, db ? sqlite3_errmsg(db) : "no db");
}

static int
_prepare_file(const gchar *path, gboolean create)
{
    int fd = open(path, O_RDWR | (create ? O_CREAT : 0) | O_NOFOLLOW | O_CLOEXEC, 0600);
    struct stat st;
    if (fd < 0)
        return -1;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || ((st.st_mode & 07777) != 0600 && fchmod(fd, 0600) != 0)) {
        close(fd);
        return -1;
    }
    return fd;
}

gchar *
pcv_vm_cfg_store_key_path(const gchar *db_path)
{
    gchar *dir = g_path_get_dirname((db_path && *db_path) ? db_path : PCV_VMCH_DEFAULT_DB_PATH);
    gchar *out = g_build_filename(dir, PCV_VMCH_KEY_NAME, NULL);
    g_free(dir);
    return out;
}

static gboolean
_create_key_atomic(const gchar *kp, guint8 *salt)
{
    gchar *tmp = g_strconcat(kp, ".tmp", NULL);
    unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    gboolean ok = fd >= 0 && RAND_bytes(salt, PCV_VMCH_SALT_LEN) == 1 &&
                  write(fd, salt, PCV_VMCH_SALT_LEN) == PCV_VMCH_SALT_LEN && fsync(fd) == 0;
    if (fd >= 0)
        close(fd);
    ok = ok && rename(tmp, kp) == 0;
    if (!ok)
        unlink(tmp);
    if (ok) {
        gchar *dir = g_path_get_dirname(kp);
        int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dfd >= 0) {
            (void)fsync(dfd);
            close(dfd);
        }
        g_free(dir);
    }
    g_free(tmp);
    return ok;
}

static gboolean
_load_key(PcvVmCfgStore *st, GError **err)
{
    gchar *kp = pcv_vm_cfg_store_key_path(st->path);
    int fd = _prepare_file(kp, FALSE);
    gboolean ok;
    if (fd < 0 && errno == ENOENT) {
        ok = _create_key_atomic(kp, st->salt);
    } else if (fd < 0) {
        g_free(kp);
        g_set_error(err, VMCH_ERROR, 2, "key file is not a usable 0600 regular file");
        return FALSE;
    } else {
        struct stat sb;
        ok = fstat(fd, &sb) == 0;
        if (ok && sb.st_size == 0)
            ok = _create_key_atomic(kp, st->salt);
        else if (ok)
            ok = sb.st_size == PCV_VMCH_SALT_LEN && pread(fd, st->salt, PCV_VMCH_SALT_LEN, 0) == PCV_VMCH_SALT_LEN;
        close(fd);
    }
    g_free(kp);
    if (!ok) {
        g_set_error(err, VMCH_ERROR, 2, "key file unreadable or malformed");
        return FALSE;
    }
    gchar *h = g_compute_checksum_for_data(G_CHECKSUM_SHA256, st->salt, PCV_VMCH_SALT_LEN);
    g_strlcpy(st->salt_id, h, sizeof st->salt_id);
    g_free(h);
    st->have_salt = TRUE;
    return TRUE;
}

static gboolean
_exec(sqlite3 *db, const gchar *sql)
{
    return sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK;
}

static gint64
_int_query(sqlite3 *db, const gchar *sql, gint64 def)
{
    sqlite3_stmt *s = NULL;
    gint64 v = def;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW &&
        sqlite3_column_type(s, 0) != SQLITE_NULL)
        v = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return v;
}

static gchar *
_meta_get(sqlite3 *db, const gchar *key)
{
    sqlite3_stmt *s = NULL;
    gchar *v = NULL;
    if (sqlite3_prepare_v2(db, "SELECT value FROM meta WHERE key = ?1", -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, key, -1, SQLITE_STATIC);
        if (sqlite3_step(s) == SQLITE_ROW)
            v = g_strdup((const gchar *)sqlite3_column_text(s, 0));
    }
    sqlite3_finalize(s);
    return v;
}

static gboolean
_meta_set(sqlite3 *db, const gchar *key, const gchar *value)
{
    sqlite3_stmt *s = NULL;
    gboolean ok = sqlite3_prepare_v2(db, "INSERT INTO meta(key, value) VALUES(?1, ?2) "
                                         "ON CONFLICT(key) DO UPDATE SET value = excluded.value",
                                     -1, &s, NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_text(s, 1, key, -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 2, value, -1, SQLITE_STATIC);
        ok = sqlite3_step(s) == SQLITE_DONE;
    }
    sqlite3_finalize(s);
    return ok;
}

static gboolean
_open_schema(PcvVmCfgStore *st, GError **err)
{
    sqlite3_busy_timeout(st->db, 2000);
    gint64 ver = _int_query(st->db, "PRAGMA user_version", -1);
    if (ver < 0 || ver > PCV_VMCH_USER_VERSION) {
        g_set_error(err, VMCH_ERROR, 3, "schema version %" G_GINT64_FORMAT " is not supported", ver);
        return FALSE;
    }
    if (!st->writer) {
        if (ver != PCV_VMCH_USER_VERSION)
            g_set_error(err, VMCH_ERROR, 3, "history db not initialized");
        return ver == PCV_VMCH_USER_VERSION;
    }
    if (ver == 0) {
        st->created = _int_query(st->db, "SELECT count(*) FROM sqlite_master WHERE name = 'config_change'", 0) == 0;
        if (st->created && !_exec(st->db, "PRAGMA auto_vacuum=INCREMENTAL")) {
            _fail(err, st->db, "auto_vacuum");
            return FALSE;
        }
    }
    if (!_exec(st->db, "PRAGMA journal_mode=WAL") || !_exec(st->db, "PRAGMA journal_size_limit=8388608") ||
        !_exec(st->db, "PRAGMA foreign_keys=ON")) {
        _fail(err, st->db, "pragma");
        return FALSE;
    }
    if (ver == 0) {
        gboolean ok = _exec(st->db, "BEGIN IMMEDIATE") && _exec(st->db, k_schema) &&
                      _exec(st->db, "PRAGMA user_version=1") && _exec(st->db, "COMMIT");
        if (!ok) {
            _fail(err, st->db, "schema");
            _exec(st->db, "ROLLBACK");
            return FALSE;
        }
        gchar *now = g_strdup_printf("%" G_GINT64_FORMAT, g_get_real_time() / 1000);
        _meta_set(st->db, "created_at", now);
        g_free(now);
    }
    gchar *nv = g_strdup_printf("%d", PCV_VMCH_NORMALIZER_VERSION);
    gboolean ok = _meta_set(st->db, "salt_id", st->salt_id) && _meta_set(st->db, "normalizer_version", nv);
    g_free(nv);
    if (!ok)
        _fail(err, st->db, "meta");
    return ok;
}

PcvVmCfgStore *
pcv_vm_cfg_store_open(const gchar *db_path, gboolean writer, GError **err)
{
    PcvVmCfgStore *st = g_new0(PcvVmCfgStore, 1);
    st->path = g_strdup((db_path && *db_path) ? db_path : PCV_VMCH_DEFAULT_DB_PATH);
    st->writer = writer;
    gboolean ok = TRUE;
    if (writer) {
        int fd = _prepare_file(st->path, TRUE);
        if (fd < 0) {
            g_set_error(err, VMCH_ERROR, 2, "history db file is not a usable 0600 regular file");
            ok = FALSE;
        } else {
            close(fd);
            ok = _load_key(st, err);
        }
    }
    int flags = writer ? (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE) : SQLITE_OPEN_READONLY;
    if (ok && sqlite3_open_v2(st->path, &st->db, flags | SQLITE_OPEN_NOMUTEX, NULL) != SQLITE_OK) {
        _fail(err, st->db, "history db open");
        ok = FALSE;
    }
    if (ok)
        ok = _open_schema(st, err);
    if (ok && !writer) {
        gchar *sid = _meta_get(st->db, "salt_id");
        g_strlcpy(st->salt_id, sid ? sid : "", sizeof st->salt_id);
        g_free(sid);
    }
    if (!ok) {
        pcv_vm_cfg_store_close(st);
        return NULL;
    }
    return st;
}

void
pcv_vm_cfg_store_close(PcvVmCfgStore *st)
{
    if (!st)
        return;
    if (st->db)
        sqlite3_close(st->db);
    memset(st->salt, 0, sizeof st->salt);
    g_free(st->path);
    g_free(st);
}

const guint8 *
pcv_vm_cfg_store_salt(const PcvVmCfgStore *st, gsize *len)
{
    if (len)
        *len = (st && st->have_salt) ? PCV_VMCH_SALT_LEN : 0;
    return (st && st->have_salt) ? st->salt : NULL;
}

const gchar *
pcv_vm_cfg_store_salt_id(const PcvVmCfgStore *st)
{
    return st ? st->salt_id : "";
}

gboolean
pcv_vm_cfg_store_created(const PcvVmCfgStore *st)
{
    return st && st->created;
}

static gchar *
_col_str(sqlite3_stmt *s, int i)
{
    return sqlite3_column_type(s, i) == SQLITE_NULL ? NULL : g_strdup((const gchar *)sqlite3_column_text(s, i));
}

static gint64
_col_int(sqlite3_stmt *s, int i)
{
    return sqlite3_column_type(s, i) == SQLITE_NULL ? 0 : sqlite3_column_int64(s, i);
}

static PcvVmCfgRow *
_row_from_stmt(sqlite3_stmt *s, int o)
{
    PcvVmCfgRow *r = g_new0(PcvVmCfgRow, 1);
    r->id = _col_int(s, o + 0);
    r->entry_id = _col_str(s, o + 1);
    r->vm_uuid = _col_str(s, o + 2);
    r->vm_name = _col_str(s, o + 3);
    r->owner = _col_str(s, o + 4);
    r->owner_tenant = _col_str(s, o + 5);
    r->kind = _col_str(s, o + 6);
    r->scope = _col_str(s, o + 7);
    r->cause = _col_str(s, o + 8);
    r->observed_at = _col_int(s, o + 9);
    r->window_start = _col_int(s, o + 10);
    r->attribution = _col_str(s, o + 11);
    r->actor = _col_str(s, o + 12);
    r->actor_tenant = _col_str(s, o + 13);
    r->actor_expired = _col_int(s, o + 14) != 0;
    r->actor_method = _col_str(s, o + 15);
    r->actor_result = _col_str(s, o + 16);
    r->config_digest = _col_str(s, o + 17);
    r->live_digest = _col_str(s, o + 18);
    r->snapshot_json = _col_str(s, o + 19);
    r->changes_json = _col_str(s, o + 20);
    r->prev_id = _col_int(s, o + 21);
    r->normalizer_version = (gint)_col_int(s, o + 22);
    r->salt_id = _col_str(s, o + 23);
    r->epoch_id = _col_int(s, o + 24);
    return r;
}

static PcvVmCfgRow *
_row_query(PcvVmCfgStore *st, const gchar *sql, const gchar *sarg, gint64 iarg)
{
    sqlite3_stmt *s = NULL;
    PcvVmCfgRow *r = NULL;
    if (sqlite3_prepare_v2(st->db, sql, -1, &s, NULL) == SQLITE_OK) {
        if (sarg)
            sqlite3_bind_text(s, 1, sarg, -1, SQLITE_STATIC);
        else
            sqlite3_bind_int64(s, 1, iarg);
        if (sqlite3_step(s) == SQLITE_ROW)
            r = _row_from_stmt(s, 0);
    }
    sqlite3_finalize(s);
    return r;
}

void
pcv_vm_cfg_latest_clear(PcvVmCfgLatestRow *l)
{
    if (!l)
        return;
    g_free(l->current_name);
    g_free(l->run_live_digest);
    pcv_vm_cfg_row_free(l->last);
    memset(l, 0, sizeof *l);
    l->run_id = -1;
}

gboolean
pcv_vm_cfg_store_latest(PcvVmCfgStore *st, const gchar *uuid, PcvVmCfgLatestRow *out, GError **err)
{
    memset(out, 0, sizeof *out);
    out->run_id = -1;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(st->db,
                           "SELECT last_id, last_confirmed_at, present, current_name, run_id, run_started_at, "
                           "run_live_digest FROM vm_latest WHERE vm_uuid = ?1",
                           -1, &s, NULL) != SQLITE_OK) {
        _fail(err, st->db, "latest");
        return FALSE;
    }
    sqlite3_bind_text(s, 1, uuid, -1, SQLITE_STATIC);
    if (sqlite3_step(s) == SQLITE_ROW) {
        out->last_id = sqlite3_column_int64(s, 0);
        out->last_confirmed_at = sqlite3_column_int64(s, 1);
        out->present = sqlite3_column_int(s, 2) != 0;
        out->current_name = _col_str(s, 3);
        out->run_id = sqlite3_column_type(s, 4) == SQLITE_NULL ? -1 : sqlite3_column_int64(s, 4);
        out->run_started_at = _col_int(s, 5);
        out->run_live_digest = _col_str(s, 6);
    }
    sqlite3_finalize(s);
    if (out->last_id > 0) {
        out->last = _row_query(st, "SELECT " ROW_COLS " FROM config_change WHERE id = ?1", NULL, out->last_id);
        out->have_row = out->last != NULL;
    }
    return TRUE;
}

static gboolean
_new_entry_id(gchar out[33])
{
    guint8 b[16];
    if (RAND_bytes(b, sizeof b) != 1)
        return FALSE;
    for (guint i = 0; i < sizeof b; i++)
        g_snprintf(out + i * 2, 3, "%02x", b[i]);
    return TRUE;
}

static void
_bind_text(sqlite3_stmt *s, int i, const gchar *v)
{
    if (v)
        sqlite3_bind_text(s, i, v, -1, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(s, i);
}

static gboolean
_latest_upsert(PcvVmCfgStore *st, const gchar *uuid, gint64 last_id, const PcvVmCfgLatestUpdate *lu)
{
    const gchar *sql =
        last_id > 0
            ? "INSERT INTO vm_latest(vm_uuid, last_id, last_confirmed_at, present, current_name, run_id, run_started_at,"
              " run_live_digest) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8) ON CONFLICT(vm_uuid) DO UPDATE SET"
              " last_id = excluded.last_id, last_confirmed_at = excluded.last_confirmed_at, present = excluded.present,"
              " current_name = excluded.current_name, run_id = CASE WHEN ?9 THEN excluded.run_id ELSE run_id END,"
              " run_started_at = CASE WHEN ?9 THEN excluded.run_started_at ELSE run_started_at END,"
              " run_live_digest = CASE WHEN ?9 OR ?10 THEN excluded.run_live_digest ELSE run_live_digest END"
            : "UPDATE vm_latest SET last_confirmed_at = ?3, present = ?4, current_name = ?5,"
              " run_id = CASE WHEN ?9 THEN ?6 ELSE run_id END,"
              " run_started_at = CASE WHEN ?9 THEN ?7 ELSE run_started_at END,"
              " run_live_digest = CASE WHEN ?9 OR ?10 THEN ?8 ELSE run_live_digest END WHERE vm_uuid = ?1";
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(st->db, sql, -1, &s, NULL) != SQLITE_OK)
        return FALSE;
    gboolean set = lu->run_action == PCV_VMCH_RUN_SET, clear = lu->run_action == PCV_VMCH_RUN_CLEAR;
    gboolean upd = lu->run_action == PCV_VMCH_RUN_UPDATE_LIVE;
    sqlite3_bind_text(s, 1, uuid, -1, SQLITE_STATIC);
    sqlite3_bind_int64(s, 2, last_id);
    sqlite3_bind_int64(s, 3, lu->confirmed_at);
    sqlite3_bind_int(s, 4, lu->present ? 1 : 0);
    _bind_text(s, 5, lu->present ? lu->current_name : NULL);
    if (set)
        sqlite3_bind_int64(s, 6, lu->run_id);
    else
        sqlite3_bind_null(s, 6);
    if (set)
        sqlite3_bind_int64(s, 7, lu->run_started_at);
    else
        sqlite3_bind_null(s, 7);
    _bind_text(s, 8, clear ? NULL : lu->run_live_digest);
    sqlite3_bind_int(s, 9, (set || clear) ? 1 : 0);
    sqlite3_bind_int(s, 10, upd ? 1 : 0);
    gboolean ok = sqlite3_step(s) == SQLITE_DONE;
    sqlite3_finalize(s);
    return ok;
}

gboolean
pcv_vm_cfg_store_insert(PcvVmCfgStore *st, PcvVmCfgRow *row, const PcvVmCfgLatestUpdate *lu, GError **err)
{
    gchar eid[33];
    if (!st->writer || !_new_entry_id(eid)) {
        g_set_error(err, VMCH_ERROR, 4, "history store not writable");
        return FALSE;
    }
    if (!_exec(st->db, "BEGIN IMMEDIATE")) {
        _fail(err, st->db, "begin");
        return FALSE;
    }
    gint64 prev = 0;
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(st->db, "SELECT last_id FROM vm_latest WHERE vm_uuid = ?1", -1, &q, NULL) == SQLITE_OK) {
        sqlite3_bind_text(q, 1, row->vm_uuid, -1, SQLITE_STATIC);
        if (sqlite3_step(q) == SQLITE_ROW)
            prev = sqlite3_column_int64(q, 0);
    }
    sqlite3_finalize(q);
    sqlite3_stmt *s = NULL;
    gboolean ok = sqlite3_prepare_v2(st->db,
                                     "INSERT INTO config_change(entry_id, vm_uuid, vm_name, owner, owner_tenant, kind,"
                                     " scope, cause, observed_at, window_start, attribution, actor, actor_tenant,"
                                     " actor_expired, actor_method, actor_result, config_digest, live_digest,"
                                     " snapshot_json, changes_json, prev_id, normalizer_version, salt_id, epoch_id)"
                                     " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,0,?14,?15,?16,?17,?18,?19,"
                                     "?20,?21,?22,?23)",
                                     -1, &s, NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_text(s, 1, eid, -1, SQLITE_STATIC);
        _bind_text(s, 2, row->vm_uuid);
        _bind_text(s, 3, row->vm_name ? row->vm_name : "");
        _bind_text(s, 4, row->owner);
        _bind_text(s, 5, row->owner_tenant);
        _bind_text(s, 6, row->kind);
        _bind_text(s, 7, row->scope);
        _bind_text(s, 8, row->cause);
        sqlite3_bind_int64(s, 9, row->observed_at);
        if (row->window_start > 0)
            sqlite3_bind_int64(s, 10, row->window_start);
        else
            sqlite3_bind_null(s, 10);
        _bind_text(s, 11, row->attribution ? row->attribution : "none");
        _bind_text(s, 12, row->actor);
        _bind_text(s, 13, row->actor_tenant);
        _bind_text(s, 14, row->actor_method);
        _bind_text(s, 15, row->actor_result);
        _bind_text(s, 16, row->config_digest);
        _bind_text(s, 17, row->live_digest);
        _bind_text(s, 18, row->snapshot_json);
        _bind_text(s, 19, row->changes_json ? row->changes_json : "[]");
        if (prev > 0)
            sqlite3_bind_int64(s, 20, prev);
        else
            sqlite3_bind_null(s, 20);
        sqlite3_bind_int(s, 21, row->normalizer_version);
        _bind_text(s, 22, row->salt_id ? row->salt_id : st->salt_id);
        sqlite3_bind_int64(s, 23, row->epoch_id);
        ok = sqlite3_step(s) == SQLITE_DONE;
    }
    sqlite3_finalize(s);
    gint64 id = ok ? sqlite3_last_insert_rowid(st->db) : 0;
    ok = ok && _latest_upsert(st, row->vm_uuid, id, lu) && _exec(st->db, "COMMIT");
    if (!ok) {
        _fail(err, st->db, "insert");
        _exec(st->db, "ROLLBACK");
        return FALSE;
    }
    row->id = id;
    row->prev_id = prev;
    g_free(row->entry_id);
    row->entry_id = g_strdup(eid);
    return TRUE;
}

gboolean
pcv_vm_cfg_store_touch(PcvVmCfgStore *st, const gchar *uuid, const PcvVmCfgLatestUpdate *lu, GError **err)
{
    if (!_latest_upsert(st, uuid, 0, lu)) {
        _fail(err, st->db, "touch");
        return FALSE;
    }
    return TRUE;
}

gboolean
pcv_vm_cfg_store_confirm(PcvVmCfgStore *st, const gchar *const *uuids, guint n, gint64 now, GError **err)
{
    sqlite3_stmt *s = NULL;
    gboolean ok = _exec(st->db, "BEGIN IMMEDIATE") &&
                  sqlite3_prepare_v2(st->db, "UPDATE vm_latest SET last_confirmed_at = ?2 WHERE vm_uuid = ?1", -1, &s,
                                     NULL) == SQLITE_OK;
    for (guint i = 0; ok && i < n; i++) {
        sqlite3_reset(s);
        sqlite3_bind_text(s, 1, uuids[i], -1, SQLITE_STATIC);
        sqlite3_bind_int64(s, 2, now);
        ok = sqlite3_step(s) == SQLITE_DONE;
    }
    sqlite3_finalize(s);
    ok = ok && _exec(st->db, "COMMIT");
    if (!ok) {
        _fail(err, st->db, "confirm");
        _exec(st->db, "ROLLBACK");
    }
    return ok;
}

gboolean
pcv_vm_cfg_store_fill_actor(PcvVmCfgStore *st, const gchar *entry_id, const gchar *actor, const gchar *actor_tenant,
                            const gchar *method, const gchar *result, GError **err)
{
    sqlite3_stmt *s = NULL;
    gboolean ok = sqlite3_prepare_v2(st->db,
                                     "UPDATE config_change SET attribution = 'rpc', actor = ?2, actor_tenant = ?3,"
                                     " actor_method = ?4, actor_result = ?5 WHERE entry_id = ?1 AND attribution = 'none'",
                                     -1, &s, NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_text(s, 1, entry_id, -1, SQLITE_STATIC);
        _bind_text(s, 2, actor);
        _bind_text(s, 3, actor_tenant);
        _bind_text(s, 4, method);
        _bind_text(s, 5, result);
        ok = sqlite3_step(s) == SQLITE_DONE;
    }
    sqlite3_finalize(s);
    if (!ok)
        _fail(err, st->db, "fill actor");
    return ok;
}

guint
pcv_vm_cfg_store_rows_since(PcvVmCfgStore *st, const gchar *uuid, gint64 since)
{
    sqlite3_stmt *s = NULL;
    guint n = 0;
    if (sqlite3_prepare_v2(st->db, "SELECT count(*) FROM config_change WHERE vm_uuid = ?1 AND observed_at >= ?2", -1, &s,
                           NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, uuid, -1, SQLITE_STATIC);
        sqlite3_bind_int64(s, 2, since);
        if (sqlite3_step(s) == SQLITE_ROW)
            n = (guint)sqlite3_column_int64(s, 0);
    }
    sqlite3_finalize(s);
    return n;
}

GPtrArray *
pcv_vm_cfg_store_present_uuids(PcvVmCfgStore *st)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(st->db, "SELECT vm_uuid FROM vm_latest WHERE present = 1", -1, &s, NULL) == SQLITE_OK) {
        while (sqlite3_step(s) == SQLITE_ROW)
            g_ptr_array_add(out, _col_str(s, 0));
    }
    sqlite3_finalize(s);
    return out;
}

static gboolean
_exec_bind(PcvVmCfgStore *st, const gchar *sql, gint64 a, gint64 b, const gchar *t, GError **err)
{
    sqlite3_stmt *s = NULL;
    gboolean ok = sqlite3_prepare_v2(st->db, sql, -1, &s, NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_int64(s, 1, a);
        sqlite3_bind_int64(s, 2, b);
        if (sqlite3_bind_parameter_count(s) >= 3)
            _bind_text(s, 3, t);
        ok = sqlite3_step(s) == SQLITE_DONE;
    }
    sqlite3_finalize(s);
    if (!ok)
        _fail(err, st->db, "epoch");
    return ok;
}

gint64
pcv_vm_cfg_store_epoch_begin(PcvVmCfgStore *st, gint64 now, GError **err)
{
    if (!_exec_bind(st, "INSERT INTO coverage_epoch(started_at, last_alive_at) VALUES(?1, ?2)", now, now, NULL, err))
        return 0;
    return sqlite3_last_insert_rowid(st->db);
}

gboolean
pcv_vm_cfg_store_epoch_heartbeat(PcvVmCfgStore *st, gint64 id, gint64 now, GError **err)
{
    return _exec_bind(st, "UPDATE coverage_epoch SET last_alive_at = ?2 WHERE id = ?1 AND ended_at IS NULL", id, now,
                      NULL, err);
}

gboolean
pcv_vm_cfg_store_epoch_end(PcvVmCfgStore *st, gint64 id, gint64 now, const gchar *reason, GError **err)
{
    return _exec_bind(st,
                      "UPDATE coverage_epoch SET ended_at = ?2, last_alive_at = ?2, end_reason = ?3 "
                      "WHERE id = ?1 AND ended_at IS NULL",
                      id, now, reason, err);
}

guint
pcv_vm_cfg_store_close_open_epochs_crash(PcvVmCfgStore *st, GError **err)
{
    if (!_exec(st->db, "UPDATE coverage_epoch SET ended_at = last_alive_at, end_reason = 'crash_inferred' "
                       "WHERE ended_at IS NULL")) {
        _fail(err, st->db, "crash close");
        return 0;
    }
    return (guint)sqlite3_changes(st->db);
}

gint64
pcv_vm_cfg_store_last_epoch_end(PcvVmCfgStore *st)
{
    return _int_query(st->db, "SELECT max(ended_at) FROM coverage_epoch WHERE ended_at IS NOT NULL", 0);
}

GArray *
pcv_vm_cfg_store_epochs(PcvVmCfgStore *st, gint64 since, gint64 until)
{
    GArray *out = g_array_new(FALSE, TRUE, sizeof(PcvVmCfgEpoch));
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(st->db,
                           "SELECT id, started_at, last_alive_at, ended_at, end_reason FROM coverage_epoch "
                           "WHERE started_at <= ?2 AND (ended_at IS NULL OR ended_at >= ?1 OR id = "
                           "(SELECT max(id) FROM coverage_epoch WHERE started_at < ?1)) ORDER BY started_at, id",
                           -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(s, 1, since);
        sqlite3_bind_int64(s, 2, until);
        while (sqlite3_step(s) == SQLITE_ROW) {
            PcvVmCfgEpoch e = {.id = sqlite3_column_int64(s, 0), .started_at = sqlite3_column_int64(s, 1),
                               .last_alive_at = sqlite3_column_int64(s, 2), .ended_at = _col_int(s, 3)};
            const gchar *r = (const gchar *)sqlite3_column_text(s, 4);
            g_strlcpy(e.end_reason, r ? r : "", sizeof e.end_reason);
            g_array_append_val(out, e);
        }
    }
    sqlite3_finalize(s);
    return out;
}

gboolean
pcv_vm_cfg_store_list(PcvVmCfgStore *st, const PcvVmCfgListFilter *f, guint limit, PcvVmCfgVisibleFn visible,
                      gpointer ud, GPtrArray *rows_out, gboolean *more, GError **err)
{
    *more = FALSE;
    gint64 cur_at = f->cursor_id[0] ? f->cursor_at : G_MAXINT64;
    gchar cur_id[33];
    g_strlcpy(cur_id, f->cursor_id[0] ? f->cursor_id : "g", sizeof cur_id);
    const gchar *sql =
        "SELECT " ROW_COLS " FROM config_change WHERE observed_at >= ?1 AND observed_at <= ?2"
        " AND (observed_at < ?3 OR (observed_at = ?3 AND entry_id < ?4))"
        " AND (?5 IS NULL OR vm_uuid = ?5)"
        " AND (?6 = 0 OR (owner IS NOT NULL AND (owner = ?7 OR (owner_tenant IS NOT NULL AND owner_tenant = ?8))))"
        " ORDER BY observed_at DESC, entry_id DESC LIMIT 200";
    for (guint rounds = 0; rounds < 1000; rounds++) {
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(st->db, sql, -1, &s, NULL) != SQLITE_OK) {
            _fail(err, st->db, "list");
            return FALSE;
        }
        sqlite3_bind_int64(s, 1, f->since);
        sqlite3_bind_int64(s, 2, f->until);
        sqlite3_bind_int64(s, 3, cur_at);
        sqlite3_bind_text(s, 4, cur_id, -1, SQLITE_TRANSIENT);
        _bind_text(s, 5, f->uuid);
        sqlite3_bind_int(s, 6, f->narrow ? 1 : 0);
        _bind_text(s, 7, f->narrow_subject ? f->narrow_subject : "");
        _bind_text(s, 8, f->narrow_tenant ? f->narrow_tenant : "");
        guint seen = 0;
        while (sqlite3_step(s) == SQLITE_ROW) {
            seen++;
            PcvVmCfgRow *r = _row_from_stmt(s, 0);
            cur_at = r->observed_at;
            g_strlcpy(cur_id, r->entry_id, sizeof cur_id);
            if (!visible || visible(r->owner, r->owner_tenant, ud)) {
                if (rows_out->len >= limit) {
                    pcv_vm_cfg_row_free(r);
                    *more = TRUE;
                    sqlite3_finalize(s);
                    return TRUE;
                }
                g_ptr_array_add(rows_out, r);
            } else {
                pcv_vm_cfg_row_free(r);
            }
        }
        sqlite3_finalize(s);
        if (seen < 200)
            break;
    }
    return TRUE;
}

PcvVmCfgRow *
pcv_vm_cfg_store_get(PcvVmCfgStore *st, const gchar *entry_id)
{
    return entry_id ? _row_query(st, "SELECT " ROW_COLS " FROM config_change WHERE entry_id = ?1", entry_id, 0) : NULL;
}

PcvVmCfgRow *
pcv_vm_cfg_store_get_prev(PcvVmCfgStore *st, const PcvVmCfgRow *row)
{
    if (!row || row->prev_id <= 0)
        return NULL;
    return _row_query(st, "SELECT " ROW_COLS " FROM config_change WHERE id = ?1", NULL, row->prev_id);
}

gint64
pcv_vm_cfg_store_last_resized(PcvVmCfgStore *st, const gchar *uuid, const gchar *dev)
{
    sqlite3_stmt *s = NULL;
    gint64 v = -1;
    gchar *path = g_strdup_printf("disk[%s].requested_size_bytes", dev ? dev : "");
    if (sqlite3_prepare_v2(st->db,
                           "SELECT json_extract(changes_json, '$[0].after') FROM config_change WHERE vm_uuid = ?1"
                           " AND kind = 'resized' AND json_extract(changes_json, '$[0].path') = ?2"
                           " ORDER BY id DESC LIMIT 1",
                           -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, uuid, -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 2, path, -1, SQLITE_STATIC);
        if (sqlite3_step(s) == SQLITE_ROW && sqlite3_column_type(s, 0) == SQLITE_INTEGER)
            v = sqlite3_column_int64(s, 0);
    }
    sqlite3_finalize(s);
    g_free(path);
    return v;
}

gchar *
pcv_vm_cfg_store_resolve_name(PcvVmCfgStore *st, const gchar *name)
{
    sqlite3_stmt *s = NULL;
    gchar *uuid = NULL;
    if (name && sqlite3_prepare_v2(st->db,
                                   "SELECT vm_uuid FROM vm_latest WHERE present = 1 AND current_name = ?1"
                                   " ORDER BY last_confirmed_at DESC LIMIT 1",
                                   -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
        if (sqlite3_step(s) == SQLITE_ROW)
            uuid = _col_str(s, 0);
    }
    sqlite3_finalize(s);
    return uuid;
}

static gint64
_file_size(const gchar *path)
{
    struct stat sb;
    return stat(path, &sb) == 0 ? (gint64)sb.st_size : 0;
}

gint64
pcv_vm_cfg_store_bytes(PcvVmCfgStore *st)
{
    gchar *wal = g_strconcat(st->path, "-wal", NULL);
    gint64 n = _file_size(st->path) + _file_size(wal);
    g_free(wal);
    return n;
}

gboolean
pcv_vm_cfg_store_info(PcvVmCfgStore *st, PcvVmCfgStoreInfo *out)
{
    memset(out, 0, sizeof *out);
    if (!st)
        return FALSE;
    out->rows = _int_query(st->db, "SELECT count(*) FROM config_change", 0);
    out->oldest_at = _int_query(st->db, "SELECT min(observed_at) FROM config_change", 0);
    gchar *pb = _meta_get(st->db, "pruned_before");
    out->pruned_before = pb ? g_ascii_strtoll(pb, NULL, 10) : 0;
    g_free(pb);
    out->bytes = pcv_vm_cfg_store_bytes(st);
    return TRUE;
}

#define PROTECTED "id NOT IN (SELECT last_id FROM vm_latest WHERE present = 1)"

static void
_raise_pruned_before(PcvVmCfgStore *st, gint64 v)
{
    gchar *old = _meta_get(st->db, "pruned_before");
    gint64 cur = old ? g_ascii_strtoll(old, NULL, 10) : 0;
    g_free(old);
    if (v > cur) {
        gchar *t = g_strdup_printf("%" G_GINT64_FORMAT, v);
        _meta_set(st->db, "pruned_before", t);
        g_free(t);
    }
}

static guint
_prune_age_batch(PcvVmCfgStore *st, gint64 cutoff, guint batch)
{
    sqlite3_stmt *s = NULL;
    guint n = 0;
    if (sqlite3_prepare_v2(st->db,
                           "DELETE FROM config_change WHERE id IN (SELECT id FROM config_change WHERE observed_at < ?1"
                           " AND " PROTECTED " ORDER BY id LIMIT ?2)",
                           -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(s, 1, cutoff);
        sqlite3_bind_int(s, 2, (int)batch);
        if (sqlite3_step(s) == SQLITE_DONE)
            n = (guint)sqlite3_changes(st->db);
    }
    sqlite3_finalize(s);
    return n;
}

static guint
_prune_cap_round(PcvVmCfgStore *st, guint batch)
{
    sqlite3_stmt *s = NULL;
    GPtrArray *uuids = g_ptr_array_new_with_free_func(g_free);
    GArray *counts = g_array_new(FALSE, TRUE, sizeof(PcvVmCfgUuidCount));
    if (sqlite3_prepare_v2(st->db, "SELECT vm_uuid, count(*) FROM config_change WHERE " PROTECTED " GROUP BY vm_uuid",
                           -1, &s, NULL) == SQLITE_OK) {
        while (sqlite3_step(s) == SQLITE_ROW) {
            g_ptr_array_add(uuids, _col_str(s, 0));
            PcvVmCfgUuidCount c = {.uuid = g_ptr_array_index(uuids, uuids->len - 1),
                                   .rows = (guint)sqlite3_column_int64(s, 1)};
            g_array_append_val(counts, c);
        }
    }
    sqlite3_finalize(s);
    guint *plan = g_new0(guint, MAX(counts->len, 1u));
    guint n = 0;
    gint64 newest = 0;
    if (pcv_vm_cfg_prune_plan((const PcvVmCfgUuidCount *)counts->data, counts->len, batch, plan) > 0) {
        sqlite3_stmt *q = NULL, *d = NULL;
        _exec(st->db, "BEGIN IMMEDIATE");
        if (sqlite3_prepare_v2(st->db,
                               "SELECT max(observed_at) FROM (SELECT observed_at FROM config_change WHERE vm_uuid = ?1"
                               " AND " PROTECTED " ORDER BY id LIMIT ?2)",
                               -1, &q, NULL) == SQLITE_OK &&
            sqlite3_prepare_v2(st->db,
                               "DELETE FROM config_change WHERE id IN (SELECT id FROM config_change WHERE vm_uuid = ?1"
                               " AND " PROTECTED " ORDER BY id LIMIT ?2)",
                               -1, &d, NULL) == SQLITE_OK) {
            for (guint i = 0; i < counts->len; i++) {
                if (!plan[i])
                    continue;
                const gchar *uuid = g_array_index(counts, PcvVmCfgUuidCount, i).uuid;
                sqlite3_bind_text(q, 1, uuid, -1, SQLITE_STATIC);
                sqlite3_bind_int(q, 2, (int)plan[i]);
                if (sqlite3_step(q) == SQLITE_ROW)
                    newest = MAX(newest, sqlite3_column_int64(q, 0));
                sqlite3_reset(q);
                sqlite3_bind_text(d, 1, uuid, -1, SQLITE_STATIC);
                sqlite3_bind_int(d, 2, (int)plan[i]);
                if (sqlite3_step(d) == SQLITE_DONE)
                    n += (guint)sqlite3_changes(st->db);
                sqlite3_reset(d);
            }
        }
        sqlite3_finalize(q);
        sqlite3_finalize(d);
        _exec(st->db, "COMMIT");
        if (n > 0)
            _raise_pruned_before(st, newest);
    }
    g_free(plan);
    g_array_unref(counts);
    g_ptr_array_unref(uuids);
    return n;
}

static gint64
_logical_bytes(PcvVmCfgStore *st)
{
    gint64 pages = _int_query(st->db, "PRAGMA page_count", 0) - _int_query(st->db, "PRAGMA freelist_count", 0);
    return MAX(pages, 0) * _int_query(st->db, "PRAGMA page_size", 4096);
}

static gboolean
_prune_should_yield(const gint *stop, gint64 deadline)
{
    return (stop && g_atomic_int_get(stop)) || (deadline && g_get_monotonic_time() >= deadline);
}

static void
_shrink(PcvVmCfgStore *st)
{
    _exec(st->db, "PRAGMA incremental_vacuum");
    _exec(st->db, "PRAGMA wal_checkpoint(TRUNCATE)");
}

gboolean
pcv_vm_cfg_store_prune(PcvVmCfgStore *st, gint64 now, gint retention_days, gint actor_retention_days, gint64 max_bytes,
                       guint batch, const gint *stop, gint64 budget_ms, PcvVmCfgPruneResult *out, GError **err)
{
    memset(out, 0, sizeof *out);
    if (!st || !st->writer) {
        g_set_error(err, VMCH_ERROR, 4, "history store not writable");
        return FALSE;
    }
    guint b = batch ? batch : PCV_VMCH_PRUNE_BATCH;
    gint64 deadline = budget_ms > 0 ? g_get_monotonic_time() + budget_ms * 1000 : 0;
    gint64 cutoff = now - (gint64)retention_days * 86400000LL;
    for (guint i = 0; i < 200; i++) {
        if (_prune_should_yield(stop, deadline)) {
            out->interrupted = TRUE;
            break;
        }
        guint n = _prune_age_batch(st, cutoff, b);
        out->deleted_age += n;
        if (n < b)
            break;
    }
    if (out->deleted_age > 0)
        _raise_pruned_before(st, cutoff);
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(st->db,
                           "UPDATE config_change SET actor = NULL, actor_tenant = NULL, actor_expired = 1"
                           " WHERE observed_at < ?1 AND actor_expired = 0 AND (actor IS NOT NULL OR actor_tenant IS NOT NULL)",
                           -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(s, 1, now - (gint64)actor_retention_days * 86400000LL);
        if (sqlite3_step(s) == SQLITE_DONE)
            out->actor_expired = (guint)sqlite3_changes(st->db);
    }
    sqlite3_finalize(s);
    s = NULL;
    _shrink(st);
    if (max_bytes > 0 && pcv_vm_cfg_store_bytes(st) > max_bytes) {
        gint64 target = max_bytes * 9 / 10;
        for (guint i = 0; i < 400 && _logical_bytes(st) > target; i++) {
            if (_prune_should_yield(stop, deadline)) {
                out->interrupted = TRUE;
                break;
            }
            guint n = _prune_cap_round(st, b);
            if (n == 0) {
                out->over_cap = TRUE;
                break;
            }
            out->deleted_cap += n;
        }
        _shrink(st);
        out->over_cap = out->over_cap || pcv_vm_cfg_store_bytes(st) > max_bytes;
    }

    gchar *pb = _meta_get(st->db, "pruned_before");
    gint64 pruned_before = pb ? g_ascii_strtoll(pb, NULL, 10) : 0;
    g_free(pb);
    if (pruned_before > 0 &&
        sqlite3_prepare_v2(st->db,
                           "DELETE FROM coverage_epoch WHERE ended_at IS NOT NULL AND ended_at < ?1"
                           " AND id NOT IN (SELECT epoch_id FROM config_change)",
                           -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(s, 1, pruned_before);
        if (sqlite3_step(s) == SQLITE_DONE)
            out->epochs_deleted = (guint)sqlite3_changes(st->db);
    }
    sqlite3_finalize(s);
    s = NULL;
    _exec(st->db, "DELETE FROM vm_latest WHERE present = 0 AND last_id NOT IN (SELECT id FROM config_change)");
    _shrink(st);
    return TRUE;
}
