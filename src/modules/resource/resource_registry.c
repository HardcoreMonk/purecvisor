                                                                             
                                           
                                                                                     
  
                           
                                                                                                    
                                                                                     
                                                                                          
                                                            
                                                                     
                                                                                   
                                                                                               
                                                                                             
                                                                
                                                                          
                                                                           
                                                                           
  
                                                                          
                                                            
                                                                                
#include "resource_registry.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <limits.h>
#include <fcntl.h>
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "modules/dispatcher/dispatcher_shared.h"
#include "modules/network/vpc/vpc_model.h"                                      
#include "utils/pcv_config.h"
#include "utils/pcv_validate.h"

G_DEFINE_QUARK(pcv-res-error-quark, pcv_res_error)

                                                   
static GMutex   g_res_mu;
static sqlite3 *g_res_db = NULL;

static const gchar *const k_kind_names[] = { "image", "iso", "pci", "usb", "storage", "bridge" };

                                                                                                  
const gchar *
pcv_res_kind_to_string(PcvResKind kind)
{
    if ((guint)kind >= G_N_ELEMENTS(k_kind_names))
        return "unknown";
    return k_kind_names[kind];
}

                                                                                                  
gboolean
pcv_res_kind_from_string(const gchar *s, PcvResKind *out)
{
    if (!s)
        return FALSE;
    for (guint i = 0; i < G_N_ELEMENTS(k_kind_names); i++) {
        if (g_strcmp0(s, k_kind_names[i]) == 0) {
            if (out)
                *out = (PcvResKind)i;
            return TRUE;
        }
    }
    return FALSE;
}

                                                                         
                                                                                                                
                                                  
gboolean
pcv_res_name_is_valid(const gchar *name)
{
    if (!name || !g_ascii_isalnum(name[0]))
        return FALSE;
    gsize n = strlen(name);
    if (n > 63)
        return FALSE;
    for (gsize i = 1; i < n; i++) {
        gchar c = name[i];
        if (!g_ascii_isalnum(c) && c != '-' && c != '_' && c != '.')
            return FALSE;
    }
    return TRUE;
}

                                                                            

                                     
void
pcv_res_norm_clear(PcvResNorm *n)
{
    if (!n)
        return;
    g_clear_pointer(&n->ref, g_free);
    n->dev = 0;
    n->ino = 0;
    n->gen = 0;
    n->has_identity = FALSE;
}

static void
res_fail(GError **error, PcvResError code, const gchar *what)
{
    g_set_error_literal(error, PCV_RES_ERROR, code, what);
}

                            
static gboolean
is_hex4(const gchar *s)
{
    for (int i = 0; i < 4; i++)
        if (!g_ascii_isxdigit(s[i]))
            return FALSE;
    return s[4] == '\0';
}

                                                                       
static gboolean
norm_pci(const gchar *value, gchar **ref)
{
    gchar *lo = g_ascii_strdown(value, -1);
    gchar *full = (strlen(lo) == 7) ? g_strconcat("0000:", lo, NULL) : g_strdup(lo);
    g_free(lo);
    if (!pcv_validate_pci_addr(full)) {
        g_free(full);
        return FALSE;
    }
    *ref = full;
    return TRUE;
}

                                                                            
static gboolean
norm_usb(const gchar *value, gchar **ref)
{
    gchar **parts = g_strsplit(value, ":", -1);
    gboolean ok = FALSE;
    if (g_strv_length(parts) == 2) {
        const gchar *v = parts[0];
        const gchar *p = parts[1];
        if (g_str_has_prefix(v, "0x") || g_str_has_prefix(v, "0X"))
            v += 2;
        if (g_str_has_prefix(p, "0x") || g_str_has_prefix(p, "0X"))
            p += 2;
        if (is_hex4(v) && is_hex4(p)) {
            gchar *lv = g_ascii_strdown(v, -1);
            gchar *lp = g_ascii_strdown(p, -1);
            *ref = g_strdup_printf("%s:%s", lv, lp);
            g_free(lv);
            g_free(lp);
            ok = TRUE;
        }
    }
    g_strfreev(parts);
    return ok;
}

                                                    
static gboolean
has_ova_suffix(const gchar *path)
{
    gchar *lo = g_ascii_strdown(path, -1);
    gboolean r = g_str_has_suffix(lo, ".ova");
    g_free(lo);
    return r;
}

                                                                                                  
                                                                                                   
                                                            
static gboolean
ident_of_fd(int fd, guint64 *dev, guint64 *ino, guint64 *gen, guint32 *mode_out)
{
    struct statx sx;
    struct statfs sf;
    if (statx(fd, "", AT_EMPTY_PATH, STATX_TYPE | STATX_INO | STATX_BTIME, &sx) != 0 || fstatfs(fd, &sf) != 0)
        return FALSE;
    guint64 fsid = ((guint64)(guint32)sf.f_fsid.__val[0] << 32) | (guint32)sf.f_fsid.__val[1];
    *dev = fsid ? fsid : (guint64)makedev(sx.stx_dev_major, sx.stx_dev_minor);
    *ino = (guint64)sx.stx_ino;
    *gen = (sx.stx_mask & STATX_BTIME) ? (guint64)sx.stx_btime.tv_sec * 1000000000ULL + sx.stx_btime.tv_nsec : 0;
    *mode_out = sx.stx_mode;
    return TRUE;
}

                                                                                 
static gboolean
ident_of_path(const gchar *path, guint64 *dev, guint64 *ino, guint64 *gen, guint32 *mode_out)
{
    int fd = open(path, O_PATH | O_CLOEXEC);
    if (fd < 0)
        return FALSE;
    gboolean ok = ident_of_fd(fd, dev, ino, gen, mode_out);
    close(fd);
    return ok;
}

                                                                                          
                                                                                                    
                                                                   
                                                                  
gboolean
pcv_res_pin_matches(const gchar *path, const PcvResPin *pin)
{
    if (!pin || pin->ino == 0)
        return TRUE;
    if (!path || !*path)
        return FALSE;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        return FALSE;
    guint64 dev = 0, ino = 0, gen = 0;
    guint32 mode = 0;
    gboolean ok = ident_of_fd(fd, &dev, &ino, &gen, &mode) && S_ISREG(mode) && dev == pin->dev && ino == pin->ino &&
                  gen == pin->gen;
    close(fd);
    return ok;
}

                                                                
static gboolean
norm_file(PcvResKind kind, const gchar *value, PcvResNorm *out, GError **error)
{
    if (value[0] != '/') {
        res_fail(error, PCV_RES_ERROR_INVALID, "path must be absolute");
        return FALSE;
    }
    gchar *canon = realpath(value, NULL);
    if (!canon) {
        res_fail(error, PCV_RES_ERROR_NOT_FOUND, "path does not resolve to an existing file");
        return FALSE;
    }
    guint64 dev = 0, ino = 0, gen = 0;
    guint32 mode = 0;
    if (!ident_of_path(canon, &dev, &ino, &gen, &mode) || !S_ISREG(mode)) {
        free(canon);
        res_fail(error, PCV_RES_ERROR_INVALID, "path is not a regular file");
        return FALSE;
    }
    gboolean ok = (kind == PCV_RES_ISO)
                      ? pcv_validate_iso_path(canon)
                      : (pcv_validate_base_image_path(canon) || has_ova_suffix(canon));
    if (!ok) {
        free(canon);
        res_fail(error, PCV_RES_ERROR_INVALID, "file name is not allowed for this kind");
        return FALSE;
    }
    out->ref = g_strdup(canon);
    out->dev = dev;
    out->ino = ino;
    out->gen = gen;
    out->has_identity = TRUE;
    free(canon);
    return TRUE;
}

                                                                                    
                                                                              
static gboolean
norm_storage_dir(const gchar *value, PcvResNorm *out, GError **error)
{
    if (strlen(value) >= PATH_MAX || strstr(value, "//") || strcmp(value, "/..") == 0 ||
        strstr(value, "/../") || g_str_has_suffix(value, "/..")) {
        res_fail(error, PCV_RES_ERROR_INVALID, "path must not contain '..' or '//'");
        return FALSE;
    }
    gchar *work = g_strdup(value);
    gsize wl = strlen(work);
    if (wl > 1 && work[wl - 1] == '/')                         
        work[wl - 1] = '\0';
    gchar **comps = g_strsplit(work + 1, "/", -1);
    g_free(work);

    GString *prefix = g_string_new("");
    guint idx = 0;
    guint n = g_strv_length(comps);
    if (!(n == 1 && comps[0][0] == '\0')) {                          
        for (; idx < n; idx++) {
            struct stat lst;
            gsize keep = prefix->len;
            g_string_append_printf(prefix, "/%s", comps[idx]);
            if (lstat(prefix->str, &lst) != 0) {
                g_string_truncate(prefix, keep);
                break;
            }
        }
    }
    gboolean ok = FALSE;
    gchar *rp = realpath(prefix->len ? prefix->str : "/", NULL);
    guint64 dev = 0, ino = 0, gen = 0;
    guint32 mode = 0;
    if (!rp) {
        res_fail(error, PCV_RES_ERROR_NOT_FOUND, "path does not resolve");
    } else if (!ident_of_path(rp, &dev, &ino, &gen, &mode) || !S_ISDIR(mode)) {
        res_fail(error, PCV_RES_ERROR_INVALID, "path is not a directory");
    } else {
        gboolean rest_ok = TRUE;
        GString *ref = g_string_new(rp);
        for (guint i = idx; i < n; i++) {
            if (comps[i][0] == '\0' || strcmp(comps[i], ".") == 0) {
                rest_ok = FALSE;
                break;
            }
            if (strcmp(rp, "/") != 0 || ref->len > 1)
                g_string_append_c(ref, '/');
            g_string_append(ref, comps[i]);
        }
        if (!rest_ok || ref->len >= PATH_MAX) {
            res_fail(error, PCV_RES_ERROR_INVALID, "path has an empty or '.' component");
        } else {
            out->ref = g_string_free(ref, FALSE);
            ref = NULL;
            if (idx == n) {                                            
                out->dev = dev;
                out->ino = ino;
                out->gen = gen;
                out->has_identity = TRUE;
            }
            ok = TRUE;
        }
        if (ref)
            g_string_free(ref, TRUE);
    }
    free(rp);
    g_string_free(prefix, TRUE);
    g_strfreev(comps);
    return ok;
}

                                  
                                                               
gboolean
pcv_res_normalize(PcvResKind kind, const gchar *value, PcvResNorm *out, GError **error)
{
    g_return_val_if_fail(out != NULL, FALSE);
    out->ref = NULL;
    out->dev = out->ino = out->gen = 0;
    out->has_identity = FALSE;
    if (!value || !*value || (guint)kind >= G_N_ELEMENTS(k_kind_names)) {
        res_fail(error, PCV_RES_ERROR_INVALID, "empty or unknown value");
        return FALSE;
    }
    gboolean ok = FALSE;
    switch (kind) {
    case PCV_RES_IMAGE:
    case PCV_RES_ISO:
        return norm_file(kind, value, out, error);
    case PCV_RES_STORAGE:
        if (value[0] == '/')
            return norm_storage_dir(value, out, error);
        ok = pcv_disp_valid_vm_storage_pool(value);
        if (ok)
            out->ref = g_strdup(value);
        break;
    case PCV_RES_PCI:
        ok = norm_pci(value, &out->ref);
        break;
    case PCV_RES_USB:
        ok = norm_usb(value, &out->ref);
        break;
    case PCV_RES_BRIDGE:
        ok = pcv_validate_bridge_name(value);
        if (ok)
            out->ref = g_strdup(value);
        break;
    }
    if (!ok)
        res_fail(error, PCV_RES_ERROR_INVALID, "value is not valid for this kind");
    return ok;
}

                                                                       
                                                         
static gboolean
require_existing(PcvResKind kind, const PcvResNorm *norm, GError **error)
{
    if (kind == PCV_RES_STORAGE && norm->ref[0] == '/' && !norm->has_identity) {
        res_fail(error, PCV_RES_ERROR_NOT_FOUND, "storage directory does not exist");
        return FALSE;
    }
    return TRUE;
}

                                                                       

static const char k_schema_sql[] =
    "CREATE TABLE IF NOT EXISTS resources ("
    " name TEXT PRIMARY KEY, kind TEXT NOT NULL, ref TEXT NOT NULL, dev INTEGER, ino INTEGER, gen INTEGER,"
    " created_by TEXT, created_at INTEGER NOT NULL, UNIQUE(kind, ref));"
    "CREATE TABLE IF NOT EXISTS resource_grants ("
    " name TEXT NOT NULL REFERENCES resources(name) ON DELETE CASCADE,"
    " tenant TEXT NOT NULL, PRIMARY KEY(name, tenant));";

                                                                      
static const char k_pool_schema_sql[] =
    "CREATE TABLE IF NOT EXISTS vpc_pools (tenant TEXT NOT NULL, cidr TEXT NOT NULL, PRIMARY KEY(tenant, cidr));";

static void
db_fail(GError **error, sqlite3 *db, const gchar *what)
{
    g_set_error(error, PCV_RES_ERROR, PCV_RES_ERROR_IO, "%s: %s", what, db ? sqlite3_errmsg(db) : "no db");
}

                                                                                 
                                                                                     
                
static gboolean
prepare_db_file(const gchar *path, GError **error)
{
    int fd = open(path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        ((st.st_mode & 07777) != 0600 && fchmod(fd, 0600) != 0)) {
        g_set_error(error, PCV_RES_ERROR, PCV_RES_ERROR_IO, "resource registry db file is not a usable 0600 regular file");
        if (fd >= 0)
            close(fd);
        return FALSE;
    }
    close(fd);
    return TRUE;
}

                                                                                 
                                                                                              
                                                                                       
                                                          
static gboolean
open_locked(const gchar *db_path, GError **error)
{
    if (g_res_db)
        return TRUE;
    const gchar *path = (db_path && *db_path) ? db_path : PCV_RES_DEFAULT_DB_PATH;
    if (!prepare_db_file(path, error))
        return FALSE;
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL) !=
        SQLITE_OK) {
        db_fail(error, db, "resource registry open");
        sqlite3_close(db);
        return FALSE;
    }
    sqlite3_busy_timeout(db, 2000);
    sqlite3_stmt *st = NULL;
    int ver = 0;
    gboolean ok = sqlite3_exec(db, "PRAGMA foreign_keys=ON", NULL, NULL, NULL) == SQLITE_OK;
    if (ok && sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW)
            ver = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    if (ok && ver > 2) {
        g_set_error(error, PCV_RES_ERROR, PCV_RES_ERROR_IO, "resource registry schema version %d is newer than supported", ver);
        sqlite3_close(db);
        return FALSE;
    }
    if (ok && ver < 2)
        ok = sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK &&
             (ver >= 1 || sqlite3_exec(db, k_schema_sql, NULL, NULL, NULL) == SQLITE_OK) &&
             sqlite3_exec(db, k_pool_schema_sql, NULL, NULL, NULL) == SQLITE_OK &&
             sqlite3_exec(db, "PRAGMA user_version=2", NULL, NULL, NULL) == SQLITE_OK &&
             sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK;
    if (!ok) {
        db_fail(error, db, "resource registry schema");
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        sqlite3_close(db);
        return FALSE;
    }
    g_res_db = db;
    return TRUE;
}

                                                                                             
                                                                                         
                                                  
gboolean
pcv_res_registry_open(const gchar *db_path, GError **error)
{
    g_mutex_lock(&g_res_mu);
    gboolean ok = open_locked(db_path, error);
    g_mutex_unlock(&g_res_mu);
    return ok;
}

                                                                    
void
pcv_res_registry_close(void)
{
    g_mutex_lock(&g_res_mu);
    if (g_res_db) {
        sqlite3_close(g_res_db);
        g_res_db = NULL;
    }
    g_mutex_unlock(&g_res_mu);
}

                                                                       

static gboolean
exec_sql(const gchar *sql, GError **error)
{
    if (sqlite3_exec(g_res_db, sql, NULL, NULL, NULL) != SQLITE_OK) {
        db_fail(error, g_res_db, "resource registry sql");
        return FALSE;
    }
    return TRUE;
}

                                                                                  
static gboolean
row_by_name(const gchar *name, gchar **kind_s, gchar **ref_s, GError **error)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_res_db, "SELECT kind, ref FROM resources WHERE name=?", -1, &st, NULL) != SQLITE_OK) {
        db_fail(error, g_res_db, "resource registry query");
        return FALSE;
    }
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    gboolean found = FALSE;
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        *kind_s = g_strdup((const gchar *)sqlite3_column_text(st, 0));
        *ref_s = g_strdup((const gchar *)sqlite3_column_text(st, 1));
        found = TRUE;
    } else if (rc != SQLITE_DONE) {
        db_fail(error, g_res_db, "resource registry query");
    }
    sqlite3_finalize(st);
    return found;
}

                                                       
static gchar *
name_by_kind_ref(const gchar *kind_s, const gchar *ref)
{
    sqlite3_stmt *st = NULL;
    gchar *r = NULL;
    if (sqlite3_prepare_v2(g_res_db, "SELECT name FROM resources WHERE kind=? AND ref=?", -1, &st, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_text(st, 1, kind_s, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, ref, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW)
        r = g_strdup((const gchar *)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    return r;
}

                                                                            
static gboolean
upsert_locked(PcvResKind kind, const gchar *name, const PcvResNorm *norm, const gchar *actor, gboolean *refreshed,
              GError **error)
{
    const gchar *kind_s = pcv_res_kind_to_string(kind);
    gchar *ek = NULL, *er = NULL;
    GError *qe = NULL;
    gboolean exists = row_by_name(name, &ek, &er, &qe);
    if (qe) {
        g_propagate_error(error, qe);
        return FALSE;
    }
    sqlite3_stmt *st = NULL;
    gboolean ok = FALSE;
    *refreshed = FALSE;
    if (exists) {
        if (g_strcmp0(ek, kind_s) != 0 || g_strcmp0(er, norm->ref) != 0) {
            g_set_error(error, PCV_RES_ERROR, PCV_RES_ERROR_CONFLICT,
                        "resource name '%s' is already registered for a different kind or reference", name);
        } else if (sqlite3_prepare_v2(g_res_db, "UPDATE resources SET dev=?, ino=?, gen=? WHERE name=?", -1, &st, NULL) ==
                   SQLITE_OK) {
            if (norm->has_identity) {
                sqlite3_bind_int64(st, 1, (sqlite3_int64)norm->dev);
                sqlite3_bind_int64(st, 2, (sqlite3_int64)norm->ino);
                sqlite3_bind_int64(st, 3, (sqlite3_int64)norm->gen);
            } else {
                sqlite3_bind_null(st, 1);
                sqlite3_bind_null(st, 2);
                sqlite3_bind_null(st, 3);
            }
            sqlite3_bind_text(st, 4, name, -1, SQLITE_STATIC);
            ok = sqlite3_step(st) == SQLITE_DONE;
            *refreshed = ok;
        }
    } else {
        gchar *other = name_by_kind_ref(kind_s, norm->ref);
        if (other) {
            g_set_error(error, PCV_RES_ERROR, PCV_RES_ERROR_CONFLICT,
                        "%s '%s' is already registered as '%s'", kind_s, norm->ref, other);
            g_free(other);
        } else if (sqlite3_prepare_v2(g_res_db,
                                      "INSERT INTO resources(name, kind, ref, dev, ino, gen, created_by, created_at)"
                                      " VALUES(?,?,?,?,?,?,?,?)", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 2, kind_s, -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 3, norm->ref, -1, SQLITE_STATIC);
            if (norm->has_identity) {
                sqlite3_bind_int64(st, 4, (sqlite3_int64)norm->dev);
                sqlite3_bind_int64(st, 5, (sqlite3_int64)norm->ino);
                sqlite3_bind_int64(st, 6, (sqlite3_int64)norm->gen);
            } else {
                sqlite3_bind_null(st, 4);
                sqlite3_bind_null(st, 5);
                sqlite3_bind_null(st, 6);
            }
            sqlite3_bind_text(st, 7, actor ? actor : "", -1, SQLITE_STATIC);
            sqlite3_bind_int64(st, 8, (sqlite3_int64)(g_get_real_time() / G_USEC_PER_SEC));
            ok = sqlite3_step(st) == SQLITE_DONE;
        }
    }
    if (!ok && error && !*error)
        db_fail(error, g_res_db, "resource registry write");
    if (st)
        sqlite3_finalize(st);
    g_free(ek);
    g_free(er);
    return ok;
}

                                                          
static gboolean
grant_locked(const gchar *name, const gchar *tenant, gboolean *added, GError **error)
{
    gchar *ek = NULL, *er = NULL;
    GError *qe = NULL;
    gboolean exists = row_by_name(name, &ek, &er, &qe);
    g_free(ek);
    g_free(er);
    if (qe) {
        g_propagate_error(error, qe);
        return FALSE;
    }
    if (!exists) {
        g_set_error(error, PCV_RES_ERROR, PCV_RES_ERROR_NOT_FOUND, "resource '%s' is not registered", name);
        return FALSE;
    }
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_res_db, "INSERT OR IGNORE INTO resource_grants(name, tenant) VALUES(?,?)", -1, &st, NULL) !=
        SQLITE_OK) {
        db_fail(error, g_res_db, "resource registry grant");
        return FALSE;
    }
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, tenant, -1, SQLITE_STATIC);
    gboolean ok = sqlite3_step(st) == SQLITE_DONE;
    if (ok && added)
        *added = sqlite3_changes(g_res_db) > 0;
    if (!ok)
        db_fail(error, g_res_db, "resource registry grant");
    sqlite3_finalize(st);
    return ok;
}

                                                     
static int
delete_rows(const gchar *sql, const gchar *a, const gchar *b, GError **error)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_res_db, sql, -1, &st, NULL) != SQLITE_OK) {
        db_fail(error, g_res_db, "resource registry delete");
        return -1;
    }
    sqlite3_bind_text(st, 1, a, -1, SQLITE_STATIC);
    if (b)
        sqlite3_bind_text(st, 2, b, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    int n = (rc == SQLITE_DONE) ? sqlite3_changes(g_res_db) : -1;
    if (rc != SQLITE_DONE)
        db_fail(error, g_res_db, "resource registry delete");
    sqlite3_finalize(st);
    return n;
}

                                                           
gboolean
pcv_res_register(PcvResKind kind, const gchar *name, const gchar *value, const gchar *actor, gboolean *refreshed_out,
                 GError **error)
{
    if (refreshed_out)
        *refreshed_out = FALSE;
    if (!pcv_res_name_is_valid(name)) {
        res_fail(error, PCV_RES_ERROR_INVALID, "invalid resource name");
        return FALSE;
    }
    PcvResNorm norm = {0};
    if (!pcv_res_normalize(kind, value, &norm, error))
        return FALSE;
    if (!require_existing(kind, &norm, error)) {
        pcv_res_norm_clear(&norm);
        return FALSE;
    }
    g_mutex_lock(&g_res_mu);
    gboolean ok = open_locked(NULL, error) && exec_sql("BEGIN IMMEDIATE", error);
    if (ok) {
        gboolean refreshed = FALSE;
        ok = upsert_locked(kind, name, &norm, actor, &refreshed, error);
        if (ok)
            ok = exec_sql("COMMIT", error);
        if (!ok)
            sqlite3_exec(g_res_db, "ROLLBACK", NULL, NULL, NULL);
        else if (refreshed_out)
            *refreshed_out = refreshed;
    }
    g_mutex_unlock(&g_res_mu);
    pcv_res_norm_clear(&norm);
    return ok;
}

                                                                                
gboolean
pcv_res_unregister(const gchar *name, GError **error)
{
    if (!name) {
        res_fail(error, PCV_RES_ERROR_INVALID, "invalid resource name");
        return FALSE;
    }
    g_mutex_lock(&g_res_mu);
    int n = open_locked(NULL, error) ? delete_rows("DELETE FROM resources WHERE name=?", name, NULL, error) : -1;
    g_mutex_unlock(&g_res_mu);
    if (n == 0)
        g_set_error(error, PCV_RES_ERROR, PCV_RES_ERROR_NOT_FOUND, "resource '%s' is not registered", name);
    return n > 0;
}

                            
gboolean
pcv_res_grant(const gchar *name, const gchar *tenant, GError **error)
{
    if (!name || !pcv_res_name_is_valid(tenant)) {
        res_fail(error, PCV_RES_ERROR_INVALID, "invalid resource or tenant name");
        return FALSE;
    }
    g_mutex_lock(&g_res_mu);
    gboolean ok = open_locked(NULL, error) && grant_locked(name, tenant, NULL, error);
    g_mutex_unlock(&g_res_mu);
    return ok;
}

                                
gboolean
pcv_res_revoke(const gchar *name, const gchar *tenant, GError **error)
{
    if (!name || !tenant) {
        res_fail(error, PCV_RES_ERROR_INVALID, "invalid resource or tenant name");
        return FALSE;
    }
    g_mutex_lock(&g_res_mu);
    int n = open_locked(NULL, error)
                ? delete_rows("DELETE FROM resource_grants WHERE name=? AND tenant=?", name, tenant, error)
                : -1;
    g_mutex_unlock(&g_res_mu);
    if (n == 0)
        g_set_error(error, PCV_RES_ERROR, PCV_RES_ERROR_NOT_FOUND, "resource '%s' is not granted to tenant '%s'", name,
                    tenant);
    return n > 0;
}

                                          
gboolean
pcv_res_lookup(const gchar *name, PcvResKind *kind_out, gchar **ref_out, GError **error)
{
    if (ref_out)
        *ref_out = NULL;
    if (!name) {
        res_fail(error, PCV_RES_ERROR_INVALID, "invalid resource name");
        return FALSE;
    }
    g_mutex_lock(&g_res_mu);
    gchar *ks = NULL, *rs = NULL;
    GError *qe = NULL;
    gboolean found = open_locked(NULL, &qe) && row_by_name(name, &ks, &rs, &qe);
    g_mutex_unlock(&g_res_mu);
    if (qe) {
        g_propagate_error(error, qe);
    } else if (!found) {
        g_set_error(error, PCV_RES_ERROR, PCV_RES_ERROR_NOT_FOUND, "resource '%s' is not registered", name);
    } else {
        PcvResKind k = PCV_RES_IMAGE;
        pcv_res_kind_from_string(ks, &k);
        if (kind_out)
            *kind_out = k;
        if (ref_out) {
            *ref_out = rs;
            rs = NULL;
        }
    }
    g_free(ks);
    g_free(rs);
    return found && !qe;
}

                                             
static JsonArray *
tenants_of(const gchar *name)
{
    JsonArray *arr = json_array_new();
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_res_db, "SELECT tenant FROM resource_grants WHERE name=? ORDER BY tenant", -1, &st, NULL) ==
        SQLITE_OK) {
        sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
        while (sqlite3_step(st) == SQLITE_ROW)
            json_array_add_string_element(arr, (const gchar *)sqlite3_column_text(st, 0));
        sqlite3_finalize(st);
    }
    return arr;
}

                                             
JsonArray *
pcv_res_list(gboolean filter_kind, PcvResKind kind, GError **error)
{
    g_mutex_lock(&g_res_mu);
    if (!open_locked(NULL, error)) {
        g_mutex_unlock(&g_res_mu);
        return NULL;
    }
    sqlite3_stmt *st = NULL;
    const gchar *sql = filter_kind
        ? "SELECT name, kind, ref, created_by, created_at FROM resources WHERE kind=? ORDER BY name"
        : "SELECT name, kind, ref, created_by, created_at FROM resources ORDER BY name";
    if (sqlite3_prepare_v2(g_res_db, sql, -1, &st, NULL) != SQLITE_OK) {
        db_fail(error, g_res_db, "resource registry list");
        g_mutex_unlock(&g_res_mu);
        return NULL;
    }
    if (filter_kind)
        sqlite3_bind_text(st, 1, pcv_res_kind_to_string(kind), -1, SQLITE_STATIC);
    JsonArray *out = json_array_new();
    while (sqlite3_step(st) == SQLITE_ROW) {
        const gchar *name = (const gchar *)sqlite3_column_text(st, 0);
        JsonObject *o = json_object_new();
        json_object_set_string_member(o, "name", name);
        json_object_set_string_member(o, "kind", (const gchar *)sqlite3_column_text(st, 1));
        json_object_set_string_member(o, "ref", (const gchar *)sqlite3_column_text(st, 2));
        json_object_set_array_member(o, "tenants", tenants_of(name));
        const gchar *by = (const gchar *)sqlite3_column_text(st, 3);
        json_object_set_string_member(o, "created_by", by ? by : "");
        json_object_set_int_member(o, "created_at", sqlite3_column_int64(st, 4));
        json_array_add_object_element(out, o);
    }
    sqlite3_finalize(st);
    g_mutex_unlock(&g_res_mu);
    return out;
}

                                                                           

                                                                                 
static gboolean
path_contained(const gchar *base, const gchar *path)
{
    if ((base[0] == '/') != (path[0] == '/'))
        return FALSE;
    if (strcmp(base, path) == 0)
        return TRUE;
    if (strcmp(base, "/") == 0)
        return TRUE;
    gsize bl = strlen(base);
    return strncmp(path, base, bl) == 0 && path[bl] == '/';
}

                                                                          
static gboolean
row_matches(PcvResKind kind, const gchar *row_ref, gboolean row_has_id, guint64 row_dev, guint64 row_ino,
            guint64 row_gen, const PcvResNorm *norm)
{
    switch (kind) {
    case PCV_RES_IMAGE:
    case PCV_RES_ISO:
        return strcmp(row_ref, norm->ref) == 0 && norm->has_identity && row_has_id && norm->dev == row_dev &&
               norm->ino == row_ino && norm->gen == row_gen;
    case PCV_RES_STORAGE: {
        if (!path_contained(row_ref, norm->ref))
            return FALSE;
        if (row_ref[0] != '/')
            return TRUE;                                         
        guint64 d, i, g;                                                  
        guint32 mode;
        return row_has_id && ident_of_path(row_ref, &d, &i, &g, &mode) && S_ISDIR(mode) && d == row_dev &&
               i == row_ino && g == row_gen;
    }
    default:
        return strcmp(row_ref, norm->ref) == 0;
    }
}

                                                                                                      
static gboolean
default_covers(PcvResKind kind, const PcvResNorm *norm)
{
    if (kind == PCV_RES_BRIDGE) {
        const gchar *def = pcv_config_get_string("network", "default_bridge", "pcvnat0");
        return strcmp(norm->ref, "virbr0") == 0 || (def && *def && strcmp(norm->ref, def) == 0);
    }
    if (kind != PCV_RES_STORAGE)
        return FALSE;
    const gchar *defs[2] = { pcv_config_get_image_dir(), pcv_config_get_zvol_pool() };
    for (int i = 0; i < 2; i++) {
        PcvResNorm d = {0};
        if (defs[i] && *defs[i] && pcv_res_normalize(PCV_RES_STORAGE, defs[i], &d, NULL) &&
            path_contained(d.ref, norm->ref)) {
            pcv_res_norm_clear(&d);
            return TRUE;
        }
        pcv_res_norm_clear(&d);
    }
    return FALSE;
}

                                                                 
static gboolean
rows_cover(PcvResKind kind, const PcvResNorm *norm, const gchar *tenant)
{
    sqlite3_stmt *st = NULL;
    const gchar *sql = tenant
        ? "SELECT r.ref, r.dev, r.ino, r.gen FROM resources r JOIN resource_grants g ON g.name=r.name"
          " WHERE r.kind=? AND g.tenant=?"
        : "SELECT ref, dev, ino, gen FROM resources WHERE kind=?";
    if (sqlite3_prepare_v2(g_res_db, sql, -1, &st, NULL) != SQLITE_OK)
        return FALSE;
    sqlite3_bind_text(st, 1, pcv_res_kind_to_string(kind), -1, SQLITE_STATIC);
    if (tenant)
        sqlite3_bind_text(st, 2, tenant, -1, SQLITE_STATIC);
    gboolean hit = FALSE;
    while (!hit && sqlite3_step(st) == SQLITE_ROW) {
        gboolean has_id = sqlite3_column_type(st, 1) != SQLITE_NULL && sqlite3_column_type(st, 2) != SQLITE_NULL;
        hit = row_matches(kind, (const gchar *)sqlite3_column_text(st, 0), has_id,
                          (guint64)sqlite3_column_int64(st, 1), (guint64)sqlite3_column_int64(st, 2),
                          (guint64)sqlite3_column_int64(st, 3), norm);
    }
    sqlite3_finalize(st);
    return hit;
}

                                                                                     
                                                                  
gboolean
pcv_res_tenant_allows(PcvResKind kind, const PcvResNorm *norm, const gchar *tenant)
{
    if (!norm || !norm->ref || !pcv_res_name_is_valid(tenant))
        return FALSE;
    if (default_covers(kind, norm))
        return TRUE;
    g_mutex_lock(&g_res_mu);
    gboolean hit = open_locked(NULL, NULL) && rows_cover(kind, norm, tenant);
    g_mutex_unlock(&g_res_mu);
    return hit;
}

                                                                
gboolean
pcv_res_covered(PcvResKind kind, const gchar *ref)
{
    if (!ref || !*ref)
        return FALSE;
    PcvResNorm norm = { .ref = g_strdup(ref) };
    if (kind == PCV_RES_IMAGE || kind == PCV_RES_ISO) {
        guint32 mode = 0;
        if (ident_of_path(ref, &norm.dev, &norm.ino, &norm.gen, &mode) && S_ISREG(mode))
            norm.has_identity = TRUE;
    }
    gboolean hit = default_covers(kind, &norm);
    if (!hit) {
        g_mutex_lock(&g_res_mu);
        hit = open_locked(NULL, NULL) && rows_cover(kind, &norm, NULL);
        g_mutex_unlock(&g_res_mu);
    }
    pcv_res_norm_clear(&norm);
    return hit;
}

                                                                           

                                                               
static gboolean
import_one(const PcvResImportItem *it, const gchar *actor, guint *reg, guint *grants, GError **error)
{
    if (!it || !pcv_res_name_is_valid(it->name) || !it->ref) {
        res_fail(error, PCV_RES_ERROR_INVALID, "invalid resource name or reference");
        return FALSE;
    }
    for (guint t = 0; it->tenants && t < it->tenants->len; t++) {
        if (!pcv_res_name_is_valid(g_ptr_array_index(it->tenants, t))) {
            res_fail(error, PCV_RES_ERROR_INVALID, "invalid tenant name");
            return FALSE;
        }
    }
    PcvResNorm norm = {0};
    if (!pcv_res_normalize(it->kind, it->ref, &norm, error))
        return FALSE;
    gboolean refreshed = FALSE;
    gboolean ok = require_existing(it->kind, &norm, error) &&
                  upsert_locked(it->kind, it->name, &norm, actor, &refreshed, error);
    pcv_res_norm_clear(&norm);
    if (!ok)
        return FALSE;
    if (!refreshed)
        (*reg)++;
    for (guint t = 0; it->tenants && t < it->tenants->len; t++) {
        gboolean added = FALSE;
        if (!grant_locked(it->name, g_ptr_array_index(it->tenants, t), &added, error))
            return FALSE;
        if (added)
            (*grants)++;
    }
    return TRUE;
}

                                                                                 
gboolean
pcv_res_import(GPtrArray *items, const gchar *actor, guint *registered_out, guint *granted_out, GError **error)
{
    guint reg = 0, grants = 0;
    if (registered_out)
        *registered_out = 0;
    if (granted_out)
        *granted_out = 0;
    if (!items) {
        res_fail(error, PCV_RES_ERROR_INVALID, "items is required");
        return FALSE;
    }
    g_mutex_lock(&g_res_mu);
    gboolean ok = open_locked(NULL, error) && exec_sql("BEGIN IMMEDIATE", error);
    if (ok) {
        for (guint i = 0; ok && i < items->len; i++) {
            GError *ie = NULL;
            const PcvResImportItem *it = g_ptr_array_index(items, i);
            ok = import_one(it, actor, &reg, &grants, &ie);
            if (!ok)                                                                 
                g_propagate_prefixed_error(error, ie, "item %u: ", (it && it->index) ? it->index : i + 1);
        }
        if (ok)
            ok = exec_sql("COMMIT", error);
        if (!ok)
            sqlite3_exec(g_res_db, "ROLLBACK", NULL, NULL, NULL);
    }
    g_mutex_unlock(&g_res_mu);
    if (ok) {
        if (registered_out)
            *registered_out = reg;
        if (granted_out)
            *granted_out = grants;
    }
    return ok;
}

                                                                       

                                                                                         
                                                        
gboolean
pcv_res_vm_counts_for_revoke(const gchar *owner_tenant, const gchar *revoked_tenant)
{
    return owner_tenant && *owner_tenant && revoked_tenant && strcmp(owner_tenant, revoked_tenant) == 0;
}

                                                   
static void
trim_tail_sep(GString *s)
{
    while (s->len && strchr("-._", s->str[s->len - 1]))
        g_string_truncate(s, s->len - 1);
}

                                                                                      
                                                                      
static gchar *
suggest_body(PcvResKind kind, const gchar *ref)
{
    gchar *src;
    if (kind == PCV_RES_IMAGE || kind == PCV_RES_ISO) {
        src = g_path_get_basename(ref);
        gchar *dot = strrchr(src, '.');
        if (dot && dot != src)
            *dot = '\0';
    } else {
        src = g_strdup(ref[0] == '/' ? ref + 1 : ref);
    }
    GString *b = g_string_new("");
    for (const gchar *p = src; *p; p++) {
        gchar c = g_ascii_tolower(*p);
        if (g_ascii_isalnum(c) || (b->len && (c == '.' || c == '_')))
            g_string_append_c(b, c);
        else if (b->len && b->str[b->len - 1] != '-')
            g_string_append_c(b, '-');
    }
    g_free(src);
    trim_tail_sep(b);
    return g_string_free(b, FALSE);
}

                                                                                                
                                                                    
gchar *
pcv_res_unregistered_suggest_name(PcvResKind kind, const gchar *ref, GHashTable *taken)
{
    gchar *body = suggest_body(kind, ref ? ref : "");
    GString *base = g_string_new(pcv_res_kind_to_string(kind));
    if (*body)
        g_string_append_printf(base, "-%s", body);
    g_free(body);
    if (base->len > 63)
        g_string_truncate(base, 63);
    trim_tail_sep(base);
    gchar *name = g_strdup(base->str);
    for (guint n = 2; taken && g_hash_table_contains(taken, name); n++) {
        gchar suffix[16];
        g_snprintf(suffix, sizeof suffix, "-%u", n);
        GString *cand = g_string_new_len(base->str, (gssize)MIN(base->len, 63 - strlen(suffix)));
        trim_tail_sep(cand);
        g_string_append(cand, suffix);
        g_free(name);
        name = g_string_free(cand, FALSE);
    }
    g_string_free(base, TRUE);
    if (taken)
        g_hash_table_add(taken, g_strdup(name));
    return name;
}

                                                                        

                                                                                                           
                                                                                                
                                                                                       
gboolean
pcv_res_pool_cidr_is_valid(const gchar *cidr)
{
    static const gchar *const k_private_v4[] = { "10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16", "100.64.0.0/10" };
    PcvVpcIpv4Cidr c;
    g_autofree gchar *canonical = NULL;
    if (!cidr || !pcv_vpc_cidr_parse(cidr, &c, &canonical, NULL) || strcmp(cidr, canonical) != 0)
        return FALSE;
    for (guint i = 0; i < G_N_ELEMENTS(k_private_v4); i++) {
        PcvVpcIpv4Cidr b;
        if (pcv_vpc_cidr_parse(k_private_v4[i], &b, NULL, NULL) && c.network >= b.network && c.broadcast <= b.broadcast)
            return TRUE;
    }
    return FALSE;
}

                                                                                    
                                                            
const gchar *
pcv_res_pool_conflict(GPtrArray *pool, const gchar *cidr)
{
    PcvVpcIpv4Cidr c;
    if (!pool || !cidr || !pcv_vpc_cidr_parse(cidr, &c, NULL, NULL) ||
        g_ptr_array_find_with_equal_func(pool, cidr, g_str_equal, NULL))
        return NULL;
    for (guint i = 0; i < pool->len; i++) {
        PcvVpcIpv4Cidr b;
        const gchar *e = g_ptr_array_index(pool, i);
        if (e && pcv_vpc_cidr_parse(e, &b, NULL, NULL) && pcv_vpc_cidr_overlaps(&c, &b))
            return e;
    }
    return NULL;
}

                                                                                                   
static gboolean
pool_input_ok(const gchar *tenant, GPtrArray *cidrs, const gchar *one, GError **error)
{
    if (!pcv_res_name_is_valid(tenant)) {
        res_fail(error, PCV_RES_ERROR_INVALID, "invalid tenant name");
        return FALSE;
    }
    GPtrArray *seen = g_ptr_array_new();                                                   
    gboolean ok = TRUE;
    for (guint i = 0; ok && cidrs && i < cidrs->len; i++) {
        const gchar *c = g_ptr_array_index(cidrs, i);
        const gchar *hit = NULL;
        if (!pcv_res_pool_cidr_is_valid(c)) {
            g_set_error(error, PCV_RES_ERROR, PCV_RES_ERROR_INVALID, "invalid pool cidr: %s", c ? c : "(null)");
            ok = FALSE;
        } else if ((hit = pcv_res_pool_conflict(seen, c)) != NULL) {
            g_set_error(error, PCV_RES_ERROR, PCV_RES_ERROR_INVALID, "cidrs overlap each other: %s, %s", hit, c);
            ok = FALSE;
        } else {
            g_ptr_array_add(seen, (gpointer)c);
        }
    }
    g_ptr_array_unref(seen);
    if (ok && one && !pcv_res_pool_cidr_is_valid(one)) {
        g_set_error(error, PCV_RES_ERROR, PCV_RES_ERROR_INVALID, "invalid pool cidr: %s", one);
        ok = FALSE;
    }
    return ok;
}

                                                         
static gboolean
pool_insert_locked(const gchar *tenant, const gchar *cidr, GError **error)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_res_db, "INSERT OR IGNORE INTO vpc_pools(tenant, cidr) VALUES(?,?)", -1, &st, NULL) !=
        SQLITE_OK) {
        db_fail(error, g_res_db, "vpc pool write");
        return FALSE;
    }
    sqlite3_bind_text(st, 1, tenant, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, cidr, -1, SQLITE_STATIC);
    gboolean ok = sqlite3_step(st) == SQLITE_DONE;
    if (!ok)
        db_fail(error, g_res_db, "vpc pool write");
    sqlite3_finalize(st);
    return ok;
}

                                                                             
                                               
gboolean
pcv_res_pool_set(const gchar *tenant, GPtrArray *cidrs, GError **error)
{
    if (!pool_input_ok(tenant, cidrs, NULL, error))
        return FALSE;
    g_mutex_lock(&g_res_mu);
    gboolean ok = open_locked(NULL, error) && exec_sql("BEGIN IMMEDIATE", error);
    if (ok) {
        ok = delete_rows("DELETE FROM vpc_pools WHERE tenant=?", tenant, NULL, error) >= 0;
        for (guint i = 0; ok && cidrs && i < cidrs->len; i++)
            ok = pool_insert_locked(tenant, g_ptr_array_index(cidrs, i), error);
        if (ok)
            ok = exec_sql("COMMIT", error);
        if (!ok)
            sqlite3_exec(g_res_db, "ROLLBACK", NULL, NULL, NULL);
    }
    g_mutex_unlock(&g_res_mu);
    return ok;
}

                                                                        
static GPtrArray *
pool_rows_locked(const gchar *tenant, GError **error)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_res_db, "SELECT cidr FROM vpc_pools WHERE tenant=? ORDER BY cidr", -1, &st, NULL) !=
        SQLITE_OK) {
        db_fail(error, g_res_db, "vpc pool read");
        return NULL;
    }
    sqlite3_bind_text(st, 1, tenant, -1, SQLITE_STATIC);
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW)
        g_ptr_array_add(out, g_strdup((const gchar *)sqlite3_column_text(st, 0)));
    if (rc != SQLITE_DONE) {
        db_fail(error, g_res_db, "vpc pool read");
        g_clear_pointer(&out, g_ptr_array_unref);
    }
    sqlite3_finalize(st);
    return out;
}

                                                                                    
                                                                                                         
                                                        
gboolean
pcv_res_pool_add(const gchar *tenant, const gchar *cidr, GError **error)
{
    if (!cidr) {
        res_fail(error, PCV_RES_ERROR_INVALID, "invalid pool cidr");
        return FALSE;
    }
    if (!pool_input_ok(tenant, NULL, cidr, error))
        return FALSE;
    g_mutex_lock(&g_res_mu);
    GPtrArray *cur = open_locked(NULL, error) ? pool_rows_locked(tenant, error) : NULL;
    gboolean ok = cur != NULL;
    const gchar *hit = ok ? pcv_res_pool_conflict(cur, cidr) : NULL;
    if (hit) {
        g_set_error(error, PCV_RES_ERROR, PCV_RES_ERROR_INVALID, "overlaps existing pool cidr %s", hit);
        ok = FALSE;
    } else if (ok && !g_ptr_array_find_with_equal_func(cur, cidr, g_str_equal, NULL)) {
        ok = pool_insert_locked(tenant, cidr, error);
    }
    g_mutex_unlock(&g_res_mu);
    if (cur)
        g_ptr_array_unref(cur);
    return ok;
}

                                                                             
                                      
GPtrArray *
pcv_res_pool_list(const gchar *tenant)
{
    if (!pcv_res_name_is_valid(tenant))
        return g_ptr_array_new_with_free_func(g_free);
    g_mutex_lock(&g_res_mu);
    GPtrArray *out = open_locked(NULL, NULL) ? pool_rows_locked(tenant, NULL) : NULL;
    g_mutex_unlock(&g_res_mu);
    return out;
}

                                                                     
                                         
JsonArray *
pcv_res_pool_list_all(void)
{
    g_mutex_lock(&g_res_mu);
    JsonArray *out = NULL;
    sqlite3_stmt *st = NULL;
    if (open_locked(NULL, NULL) &&
        sqlite3_prepare_v2(g_res_db, "SELECT tenant, cidr FROM vpc_pools ORDER BY tenant, cidr", -1, &st, NULL) ==
            SQLITE_OK) {
        out = json_array_new();
        JsonArray *cur = NULL;
        const gchar *cur_tenant = NULL;
        int rc;
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            const gchar *t = (const gchar *)sqlite3_column_text(st, 0);
            if (!cur || g_strcmp0(cur_tenant, t) != 0) {
                JsonObject *o = json_object_new();
                json_object_set_string_member(o, "tenant", t);
                cur = json_array_new();
                json_object_set_array_member(o, "cidrs", cur);
                json_array_add_object_element(out, o);
                cur_tenant = json_object_get_string_member(o, "tenant");
            }
            json_array_add_string_element(cur, (const gchar *)sqlite3_column_text(st, 1));
        }
        if (rc != SQLITE_DONE)
            g_clear_pointer(&out, json_array_unref);
        sqlite3_finalize(st);
    }
    g_mutex_unlock(&g_res_mu);
    return out;
}
