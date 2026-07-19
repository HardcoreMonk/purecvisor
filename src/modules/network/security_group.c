
#include <glib.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <sqlite3.h>
#include "../../utils/pcv_spawn.h"
#include "../../utils/pcv_log.h"
#include "../../utils/pcv_validate.h"
#include "../../utils/pcv_config.h"
#include "../../utils/pcv_worker_pool.h"
#include "security_group.h"
#include "security_group_nft.h"
#include "vm_iface.h"
#include "vm_vnet_cache.h"
#include "../security/security_event.h"
#include "../security/security_store.h"

typedef struct {
    gchar  *direction;
    gchar  *protocol;
    gint    port_start;
    gint    port_end;
    gchar  *source;
    gint64  db_id;
} SgRule;

typedef struct {
    gchar     *name;
    gchar     *description;
    GPtrArray *rules;
    GPtrArray *vm_bindings;
} SecurityGroup;

static GHashTable *g_sg_map = nullptr;
static GMutex      g_sg_mu;
static sqlite3    *g_sg_db  = nullptr;

#define SG_DB_PATH "/var/lib/purecvisor/security_groups.db"
#define SG_NFT_SCRIPT_PATH "/run/purecvisor/sg-ruleset.nft"

#define SG_SPAWN_TIMEOUT_SEC 30
static GMutex g_sg_nft_mu;
static GMutex g_sg_dispatch_mu;
static guint g_sg_resync_timer_id = 0;
static gint  g_sg_resync_inflight = 0;

static void
_sg_rule_free(gpointer data)
{
    SgRule *r = data;
    if (!r) return;
    g_free(r->direction);
    g_free(r->protocol);
    g_free(r->source);
    g_free(r);
}

static void
_sg_free(gpointer data)
{
    SecurityGroup *sg = data;
    if (!sg) return;
    g_free(sg->name);
    g_free(sg->description);
    if (sg->rules) g_ptr_array_unref(sg->rules);
    if (sg->vm_bindings) g_ptr_array_unref(sg->vm_bindings);
    g_free(sg);
}

static void
_ensure_init(void)
{
    if (!g_sg_map) {
        g_sg_map = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, _sg_free);
    }
}

static void _sg_db_init(void) {
    if (g_sg_db) return;
    int rc = sqlite3_open(SG_DB_PATH, &g_sg_db);
    if (rc != SQLITE_OK) {
        PCV_LOG_WARN("SG", "Cannot open SG database %s: %s", SG_DB_PATH, sqlite3_errmsg(g_sg_db));
        g_sg_db = nullptr;
        return;
    }
    sqlite3_exec(g_sg_db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(g_sg_db, "PRAGMA busy_timeout=3000;", NULL, NULL, NULL);

    const char *ddl =
        "CREATE TABLE IF NOT EXISTS security_groups ("
        "  name TEXT PRIMARY KEY, description TEXT);"
        "CREATE TABLE IF NOT EXISTS sg_rules ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  group_name TEXT NOT NULL,"
        "  direction TEXT NOT NULL DEFAULT 'ingress',"
        "  protocol TEXT NOT NULL DEFAULT 'tcp',"
        "  port_start INTEGER NOT NULL DEFAULT 0,"
        "  port_end INTEGER NOT NULL DEFAULT 0,"
        "  source TEXT NOT NULL DEFAULT '0.0.0.0/0',"
        "  FOREIGN KEY(group_name) REFERENCES security_groups(name) ON DELETE CASCADE);"
        "CREATE TABLE IF NOT EXISTS sg_vm_bindings ("
        "  group_name TEXT NOT NULL,"
        "  vm_name TEXT NOT NULL,"
        "  PRIMARY KEY(group_name, vm_name),"
        "  FOREIGN KEY(group_name) REFERENCES security_groups(name) ON DELETE CASCADE);";
    sqlite3_exec(g_sg_db, ddl, NULL, NULL, NULL);
    sqlite3_exec(g_sg_db, "PRAGMA foreign_keys=ON;", NULL, NULL, NULL);
}

static void
_sg_db_exec_finalize(sqlite3_stmt *stmt, const char *what)
{
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE)
        PCV_LOG_WARN("SG", "%s 실패 (rc=%d): %s", what, rc, sqlite3_errmsg(g_sg_db));
    sqlite3_finalize(stmt);
}

static void _sg_db_save_group(const gchar *name, const gchar *desc) {
    if (!g_sg_db) return;
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(g_sg_db, "INSERT OR REPLACE INTO security_groups(name,description) VALUES(?,?)", -1, &stmt, NULL) != SQLITE_OK || !stmt) return;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, desc ? desc : "", -1, SQLITE_STATIC);
    _sg_db_exec_finalize(stmt, "security_groups INSERT");
}

static void _sg_db_delete_group(const gchar *name) {
    if (!g_sg_db) return;
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(g_sg_db, "DELETE FROM security_groups WHERE name=?", -1, &stmt, NULL) != SQLITE_OK || !stmt) return;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
    _sg_db_exec_finalize(stmt, "security_groups DELETE");
}

static gint64 _sg_db_save_rule(const gchar *group, const gchar *dir, const gchar *proto,
                               gint port_start, gint port_end, const gchar *source) {
    if (!g_sg_db) return -1;
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(g_sg_db,
        "INSERT INTO sg_rules(group_name,direction,protocol,port_start,port_end,source) VALUES(?,?,?,?,?,?)",
        -1, &stmt, NULL) != SQLITE_OK || !stmt) return -1;
    sqlite3_bind_text(stmt, 1, group, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, dir, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, proto, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 4, port_start);
    sqlite3_bind_int(stmt, 5, port_end);
    sqlite3_bind_text(stmt, 6, source, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        PCV_LOG_WARN("SG", "sg_rules INSERT 실패 (rc=%d): %s", rc, sqlite3_errmsg(g_sg_db));
        sqlite3_finalize(stmt);
        return -1;
    }
    gint64 row_id = sqlite3_last_insert_rowid(g_sg_db);
    sqlite3_finalize(stmt);
    return row_id;
}

static void _sg_db_delete_rule(gint64 rule_id) {
    if (!g_sg_db) return;
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(g_sg_db, "DELETE FROM sg_rules WHERE id=?", -1, &stmt, NULL) != SQLITE_OK || !stmt) return;
    sqlite3_bind_int64(stmt, 1, rule_id);
    _sg_db_exec_finalize(stmt, "sg_rules DELETE");
}

static void _sg_db_save_binding(const gchar *group, const gchar *vm) {
    if (!g_sg_db) return;
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(g_sg_db, "INSERT OR IGNORE INTO sg_vm_bindings(group_name,vm_name) VALUES(?,?)", -1, &stmt, NULL) != SQLITE_OK || !stmt) return;
    sqlite3_bind_text(stmt, 1, group, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, vm, -1, SQLITE_STATIC);
    _sg_db_exec_finalize(stmt, "sg_vm_bindings INSERT");
}

static void _sg_db_delete_binding(const gchar *group, const gchar *vm) {
    if (!g_sg_db) return;
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(g_sg_db,
            "DELETE FROM sg_vm_bindings WHERE group_name=? AND vm_name=?",
            -1, &stmt, NULL) != SQLITE_OK || !stmt) return;
    sqlite3_bind_text(stmt, 1, group, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, vm, -1, SQLITE_STATIC);
    _sg_db_exec_finalize(stmt, "sg_vm_bindings DELETE");
}

static gboolean
_nft_run_script(const gchar *script, GError **error)
{
    g_mutex_lock(&g_sg_nft_mu);
    g_mkdir_with_parents("/run/purecvisor", 0755);
    gboolean ok = g_file_set_contents(SG_NFT_SCRIPT_PATH, script, -1, error);
    if (ok) {
        const gchar *argv[] = {"nft", "-f", SG_NFT_SCRIPT_PATH, NULL};
        gchar *std_err = nullptr;
        ok = pcv_spawn_sync_timeout(argv, NULL, &std_err, SG_SPAWN_TIMEOUT_SEC, error);
        if (!ok)
            PCV_LOG_ERROR("SG", "nft -f 트랜잭션 실패: %s",
                          std_err ? std_err : "unknown");
        g_free(std_err);
    }
    g_mutex_unlock(&g_sg_nft_mu);
    return ok;
}

static gboolean
_nft_ensure(GError **error)
{

    const gchar *mp[] = {"modprobe", "nf_conntrack_bridge", NULL};
    if (!pcv_spawn_sync_timeout(mp, NULL, NULL, SG_SPAWN_TIMEOUT_SEC, NULL))
        PCV_LOG_WARN("SG", "modprobe nf_conntrack_bridge 실패 — ct 규칙 적용이 거부될 수 있음");
    gchar *script = pcv_sg_nft_build_ensure_script();
    gboolean ok = _nft_run_script(script, error);
    g_free(script);
    return ok;
}

static void
_nft_teardown_legacy(void)
{
    const gchar *ls[] = {"nft", "list", "table", "inet", "purecvisor", NULL};
    gchar *out = nullptr;
    if (!pcv_spawn_sync_timeout(ls, &out, NULL, SG_SPAWN_TIMEOUT_SEC, NULL) || !out) {
        g_free(out);
        return;
    }
    gchar **lines = g_strsplit(out, "\n", -1);
    for (gchar **l = lines; *l; l++) {
        gchar *t = g_strstrip(*l);
        if (!g_str_has_prefix(t, "chain sg-"))
            continue;
        gchar *name = g_strdup(t + strlen("chain "));
        gchar *sp = strchr(name, ' ');
        if (sp) *sp = '\0';
        const gchar *fl[]  = {"nft", "flush",  "chain", "inet", "purecvisor", name, NULL};
        const gchar *del[] = {"nft", "delete", "chain", "inet", "purecvisor", name, NULL};
        (void)pcv_spawn_sync_timeout(fl,  NULL, NULL, SG_SPAWN_TIMEOUT_SEC, NULL);
        (void)pcv_spawn_sync_timeout(del, NULL, NULL, SG_SPAWN_TIMEOUT_SEC, NULL);
        PCV_LOG_INFO("SG", "legacy chain 제거: inet purecvisor %s", name);
        g_free(name);
    }
    g_strfreev(lines);
    g_free(out);
}

static GPtrArray *
_snapshot_rule_view(SecurityGroup *sg)
{
    GPtrArray *view = g_ptr_array_new();
    for (guint i = 0; i < sg->rules->len; i++) {
        SgRule *r = g_ptr_array_index(sg->rules, i);
        SgNftRule *n = g_new0(SgNftRule, 1);
        n->direction  = g_strdup(r->direction);
        n->protocol   = g_strdup(r->protocol);
        n->port_start = r->port_start;
        n->port_end   = r->port_end;
        n->source     = g_strdup(r->source);
        g_ptr_array_add(view, n);
    }
    return view;
}

static void
_rule_view_free(GPtrArray *view)
{
    for (guint i = 0; i < view->len; i++) {
        SgNftRule *n = g_ptr_array_index(view, i);
        g_free((gchar *)n->direction);
        g_free((gchar *)n->protocol);
        g_free((gchar *)n->source);
        g_free(n);
    }
    g_ptr_array_unref(view);
}

static gboolean
_rebuild_dispatch_ex(const gchar *prefix, GError **error)
{

    g_mutex_lock(&g_sg_dispatch_mu);
    g_mutex_lock(&g_sg_mu);
    _ensure_init();
    GHashTable *vm_map = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, (GDestroyNotify)g_ptr_array_unref);
    GHashTable *egress_groups = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, g_sg_map);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        SecurityGroup *sg = v;
        for (guint i = 0; i < sg->rules->len; i++) {
            SgRule *r = g_ptr_array_index(sg->rules, i);
            if (g_strcmp0(r->direction, "egress") == 0) {
                g_hash_table_add(egress_groups, g_strdup(sg->name));
                break;
            }
        }
        for (guint i = 0; i < sg->vm_bindings->len; i++) {
            const gchar *vm = g_ptr_array_index(sg->vm_bindings, i);
            GPtrArray *groups = g_hash_table_lookup(vm_map, vm);
            if (!groups) {
                groups = g_ptr_array_new_with_free_func(g_free);
                g_hash_table_insert(vm_map, g_strdup(vm), groups);
            }
            g_ptr_array_add(groups, g_strdup(sg->name));
        }
    }
    g_mutex_unlock(&g_sg_mu);

    GPtrArray *bindings = g_ptr_array_new();
    g_hash_table_iter_init(&it, vm_map);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        const gchar *vm = k;
        GPtrArray *groups = v;
        gboolean egress = FALSE;
        for (guint j = 0; j < groups->len; j++) {
            if (g_hash_table_contains(egress_groups, g_ptr_array_index(groups, j))) {
                egress = TRUE;
                break;
            }
        }

        GPtrArray *ifaces = pcv_vm_vnet_cache_get(vm);
        if (!ifaces) {
            ifaces = pcv_vm_iface_list(vm);
            if (ifaces->len > 0)
                pcv_vm_vnet_cache_put(vm, ifaces);
        }
        for (guint j = 0; j < ifaces->len; j++) {
            SgNftBinding *b = g_new0(SgNftBinding, 1);
            b->vnet            = g_strdup(g_ptr_array_index(ifaces, j));
            b->groups          = groups;
            b->egress_enforced = egress;
            g_ptr_array_add(bindings, b);
        }
        g_ptr_array_unref(ifaces);
    }

    gchar *dscript = pcv_sg_nft_build_dispatch_script(bindings);
    gchar *full = prefix ? g_strconcat(prefix, "\n", dscript, NULL)
                         : g_strdup(dscript);
    gboolean ok = _nft_run_script(full, error);
    g_free(full);
    g_free(dscript);

    for (guint i = 0; i < bindings->len; i++) {
        SgNftBinding *b = g_ptr_array_index(bindings, i);
        g_free((gchar *)b->vnet);
        g_free(b);
    }
    g_ptr_array_unref(bindings);
    g_hash_table_destroy(vm_map);
    g_hash_table_destroy(egress_groups);
    g_mutex_unlock(&g_sg_dispatch_mu);
    return ok;
}

static gboolean
_rebuild_dispatch(GError **error)
{
    return _rebuild_dispatch_ex(NULL, error);
}

[[nodiscard]] gboolean
pcv_security_group_create(const gchar *name, const gchar *description)
{
    if (!name || !*name) return FALSE;

    if (!pcv_validate_bridge_name(name)) {
        PCV_LOG_WARN("SG", "Rejected security group create: invalid name '%s'", name);
        return FALSE;
    }
    _sg_db_init();

    g_mutex_lock(&g_sg_mu);
    _ensure_init();
    if (g_hash_table_contains(g_sg_map, name)) {
        g_mutex_unlock(&g_sg_mu);
        return FALSE;
    }
    SecurityGroup *sg = g_new0(SecurityGroup, 1);
    sg->name        = g_strdup(name);
    sg->description = g_strdup(description ? description : "");
    sg->rules       = g_ptr_array_new_with_free_func(_sg_rule_free);
    sg->vm_bindings = g_ptr_array_new_with_free_func(g_free);
    g_hash_table_insert(g_sg_map, sg->name, sg);
    g_mutex_unlock(&g_sg_mu);

    GError *error = nullptr;
    gchar *gscript = pcv_sg_nft_build_group_script(name, NULL);
    gboolean ok = _nft_ensure(&error) && _nft_run_script(gscript, &error);
    g_free(gscript);
    if (!ok) {
        PCV_LOG_ERROR("SG", "'%s' nft 반영 실패 — 생성 롤백: %s",
                      name, error ? error->message : "unknown");
        g_clear_error(&error);
        g_mutex_lock(&g_sg_mu);
        g_hash_table_remove(g_sg_map, name);
        g_mutex_unlock(&g_sg_mu);
        return FALSE;
    }
    _sg_db_save_group(name, description);
    PCV_LOG_INFO("SG", "Created security group '%s' (bridge pcv_sg 스코프 체인)", name);
    return TRUE;
}

gboolean
pcv_security_group_delete(const gchar *name)
{
    if (!name) return FALSE;

    g_mutex_lock(&g_sg_mu);
    _ensure_init();
    SecurityGroup *sg = g_hash_table_lookup(g_sg_map, name);
    if (!sg) {
        g_mutex_unlock(&g_sg_mu);
        return FALSE;
    }
    g_hash_table_steal(g_sg_map, name);
    g_mutex_unlock(&g_sg_mu);

    GPtrArray *unbind_vms = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < sg->vm_bindings->len; i++)
        g_ptr_array_add(unbind_vms, g_strdup(g_ptr_array_index(sg->vm_bindings, i)));

    GError *error = nullptr;
    gboolean ok = _rebuild_dispatch(&error);
    if (ok) {
        gchar *dscript = pcv_sg_nft_build_group_delete_script(name);
        ok = _nft_run_script(dscript, &error);
        g_free(dscript);
    }
    if (!ok) {
        PCV_LOG_ERROR("SG", "'%s' nft 삭제 실패 — 롤백: %s",
                      name, error ? error->message : "unknown");
        g_clear_error(&error);
        g_mutex_lock(&g_sg_mu);
        g_hash_table_insert(g_sg_map, sg->name, sg);
        g_mutex_unlock(&g_sg_mu);
        g_ptr_array_unref(unbind_vms);
        return FALSE;
    }
    _sg_db_delete_group(name);
    _sg_free(sg);

    for (guint i = 0; i < unbind_vms->len; i++) {
        const gchar *rvm = g_ptr_array_index(unbind_vms, i);
        if (!pcv_security_group_vm_is_bound(rvm))
            pcv_vm_vnet_cache_evict(rvm);
    }
    g_ptr_array_unref(unbind_vms);
    PCV_LOG_INFO("SG", "Deleted security group '%s'", name);
    return TRUE;
}

JsonArray *
pcv_security_group_list(void)
{
    JsonArray *arr = json_array_new();

    g_mutex_lock(&g_sg_mu);
    _ensure_init();

    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, g_sg_map);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        SecurityGroup *sg = value;
        JsonObject *obj = json_object_new();
        json_object_set_string_member(obj, "name", sg->name);
        json_object_set_string_member(obj, "description", sg->description);
        json_object_set_int_member(obj, "rule_count", (gint64)sg->rules->len);

        JsonArray *vm_arr = json_array_new();
        for (guint i = 0; i < sg->vm_bindings->len; i++)
            json_array_add_string_element(vm_arr, g_ptr_array_index(sg->vm_bindings, i));
        json_object_set_array_member(obj, "vms", vm_arr);

        JsonArray *rule_arr = json_array_new();
        for (guint i = 0; i < sg->rules->len; i++) {
            SgRule *r = g_ptr_array_index(sg->rules, i);
            JsonObject *robj = json_object_new();
            json_object_set_string_member(robj, "direction", r->direction);
            json_object_set_string_member(robj, "protocol", r->protocol);
            json_object_set_int_member(robj, "port_start", r->port_start);
            json_object_set_int_member(robj, "port_end", r->port_end);
            json_object_set_string_member(robj, "source", r->source);
            json_object_set_int_member(robj, "id", r->db_id);
            json_array_add_object_element(rule_arr, robj);
        }
        json_object_set_array_member(obj, "rules", rule_arr);

        json_array_add_object_element(arr, obj);
    }
    g_mutex_unlock(&g_sg_mu);
    return arr;
}

gboolean
pcv_security_group_rule_add(const gchar *name, JsonObject *rule)
{
    if (!name || !rule) return FALSE;

    const gchar *direction = json_object_has_member(rule, "direction")
        ? json_object_get_string_member(rule, "direction") : "ingress";
    const gchar *protocol = json_object_has_member(rule, "protocol")
        ? json_object_get_string_member(rule, "protocol") : "tcp";
    const gchar *source = json_object_has_member(rule, "source")
        ? json_object_get_string_member(rule, "source") : "0.0.0.0/0";
    gint port_start = json_object_has_member(rule, "port_start")
        ? (gint)json_object_get_int_member(rule, "port_start")
        : (json_object_has_member(rule, "port")
            ? (gint)json_object_get_int_member(rule, "port") : 0);
    gint port_end = json_object_has_member(rule, "port_end")
        ? (gint)json_object_get_int_member(rule, "port_end") : 0;

    if (g_strcmp0(direction, "ingress") != 0 &&
        g_strcmp0(direction, "egress")  != 0) {
        PCV_LOG_WARN("SG", "Rejected rule for '%s': invalid direction '%s'",
                     name, direction ? direction : "(null)");
        return FALSE;
    }

    if (!pcv_validate_l4_proto(protocol)) {
        PCV_LOG_WARN("SG", "Rejected rule for '%s': invalid protocol '%s'",
                     name, protocol ? protocol : "(null)");
        return FALSE;
    }

    if (!pcv_validate_cidr(source)) {
        PCV_LOG_WARN("SG", "Rejected rule for '%s': invalid source CIDR '%s'",
                     name, source ? source : "(null)");
        return FALSE;
    }

    if (port_start > 0 && !pcv_validate_port(port_start)) {
        PCV_LOG_WARN("SG", "Rejected rule for '%s': invalid port_start %d",
                     name, port_start);
        return FALSE;
    }
    if (port_end > 0 && !pcv_validate_port(port_end)) {
        PCV_LOG_WARN("SG", "Rejected rule for '%s': invalid port_end %d",
                     name, port_end);
        return FALSE;
    }
    if (port_start > 0 && port_end > 0 && port_end < port_start) {
        PCV_LOG_WARN("SG", "Rejected rule for '%s': port_end %d < port_start %d",
                     name, port_end, port_start);
        return FALSE;
    }

    _sg_db_init();
    if (!g_sg_db) {
        PCV_LOG_WARN("SG", "Rejected rule_add for '%s': SG DB unavailable (degraded)", name);
        return FALSE;
    }

    g_mutex_lock(&g_sg_mu);
    _ensure_init();
    SecurityGroup *sg = g_hash_table_lookup(g_sg_map, name);
    if (!sg) {
        g_mutex_unlock(&g_sg_mu);
        return FALSE;
    }

    SgRule *r = g_new0(SgRule, 1);
    r->direction  = g_strdup(direction);
    r->protocol   = g_strdup(protocol);
    r->port_start = port_start;
    r->port_end   = port_end;
    r->source     = g_strdup(source);
    g_ptr_array_add(sg->rules, r);

    GPtrArray *view = _snapshot_rule_view(sg);
    g_mutex_unlock(&g_sg_mu);

    GError *error = nullptr;
    gchar *gscript = pcv_sg_nft_build_group_script(name, view);
    gboolean ok = _rebuild_dispatch_ex(gscript, &error);
    g_free(gscript);
    _rule_view_free(view);

    if (!ok) {
        PCV_LOG_ERROR("SG", "rule_add '%s' nft 반영 실패 — 롤백: %s",
                      name, error ? error->message : "unknown");
        g_clear_error(&error);
        g_mutex_lock(&g_sg_mu);
        SecurityGroup *sg2 = g_hash_table_lookup(g_sg_map, name);
        if (sg2) g_ptr_array_remove(sg2->rules, r);
        g_mutex_unlock(&g_sg_mu);
        return FALSE;
    }

    gint64 db_id = _sg_db_save_rule(name, direction, protocol,
                                    port_start, port_end, source);

    if (db_id < 1) {

        PCV_LOG_ERROR("SG", "rule_add '%s' DB 저장 실패 — nft 롤백", name);
        g_mutex_lock(&g_sg_mu);
        SecurityGroup *sgc = g_hash_table_lookup(g_sg_map, name);
        GPtrArray *rb_view = NULL;
        if (sgc && g_ptr_array_find(sgc->rules, r, NULL)) {
            g_ptr_array_remove(sgc->rules, r);
            rb_view = _snapshot_rule_view(sgc);
        }
        g_mutex_unlock(&g_sg_mu);
        if (rb_view) {
            GError *rberr = nullptr;
            gchar *rbscript = pcv_sg_nft_build_group_script(name, rb_view);
            if (!_rebuild_dispatch_ex(rbscript, &rberr)) {
                PCV_LOG_ERROR("SG", "rule_add '%s' 보상 revert 실패"
                              "(이중 결함 — 다음 변이/restore 가 자가치유): %s",
                              name, rberr ? rberr->message : "unknown");
                g_clear_error(&rberr);
            }
            g_free(rbscript);
            _rule_view_free(rb_view);
        }
        return FALSE;
    }

    g_mutex_lock(&g_sg_mu);
    SecurityGroup *sg3 = g_hash_table_lookup(g_sg_map, name);
    if (sg3 && g_ptr_array_find(sg3->rules, r, NULL)) {
        r->db_id = db_id;
    } else {

        _sg_db_delete_rule(db_id);
    }
    g_mutex_unlock(&g_sg_mu);
    PCV_LOG_INFO("SG", "Applied rule to '%s': %s %s port %d-%d from %s",
        name, direction, protocol, port_start, port_end, source);
    return TRUE;
}

gboolean
pcv_security_group_apply_to_vm(const gchar *vm, const gchar *sg_name)
{
    if (!vm || !sg_name) return FALSE;

    if (!pcv_validate_vm_name(vm)) {
        PCV_LOG_WARN("SG", "Rejected bind: invalid vm '%s'", vm);
        return FALSE;
    }

    g_mutex_lock(&g_sg_mu);
    _ensure_init();
    SecurityGroup *sg = g_hash_table_lookup(g_sg_map, sg_name);
    if (!sg) {
        g_mutex_unlock(&g_sg_mu);
        return FALSE;
    }
    for (guint i = 0; i < sg->vm_bindings->len; i++) {
        if (g_strcmp0(g_ptr_array_index(sg->vm_bindings, i), vm) == 0) {
            g_mutex_unlock(&g_sg_mu);
            return TRUE;
        }
    }
    g_ptr_array_add(sg->vm_bindings, g_strdup(vm));
    g_mutex_unlock(&g_sg_mu);

    pcv_vm_vnet_cache_evict(vm);

    GError *error = nullptr;
    if (!_rebuild_dispatch(&error)) {
        PCV_LOG_ERROR("SG", "bind '%s'→VM '%s' nft 반영 실패 — 롤백: %s",
                      sg_name, vm, error ? error->message : "unknown");
        g_clear_error(&error);
        g_mutex_lock(&g_sg_mu);
        SecurityGroup *sg2 = g_hash_table_lookup(g_sg_map, sg_name);
        if (sg2) {
            for (guint i = 0; i < sg2->vm_bindings->len; i++) {
                if (g_strcmp0(g_ptr_array_index(sg2->vm_bindings, i), vm) == 0) {
                    g_ptr_array_remove_index(sg2->vm_bindings, i);
                    break;
                }
            }
        }
        g_mutex_unlock(&g_sg_mu);

        if (!pcv_security_group_vm_is_bound(vm))
            pcv_vm_vnet_cache_evict(vm);
        return FALSE;
    }
    _sg_db_save_binding(sg_name, vm);
    PCV_LOG_INFO("SG", "Applied security group '%s' to VM '%s'", sg_name, vm);
    return TRUE;
}

gboolean
pcv_security_group_rule_remove(const gchar *name, gint64 rule_id)
{
    if (!name) return FALSE;
    if (rule_id <= 0) {
        PCV_LOG_WARN("SG", "Rejected rule_remove for '%s': invalid rule_id %ld",
                     name, (long)rule_id);
        return FALSE;
    }

    g_mutex_lock(&g_sg_mu);
    _ensure_init();
    SecurityGroup *sg = g_hash_table_lookup(g_sg_map, name);
    if (!sg) {
        g_mutex_unlock(&g_sg_mu);
        return FALSE;
    }
    SgRule *stolen = nullptr;
    for (guint i = 0; i < sg->rules->len; i++) {
        SgRule *r = g_ptr_array_index(sg->rules, i);
        if (r->db_id == rule_id) {
            stolen = g_ptr_array_steal_index(sg->rules, i);
            break;
        }
    }
    if (!stolen) {
        g_mutex_unlock(&g_sg_mu);
        return FALSE;
    }
    GPtrArray *view = _snapshot_rule_view(sg);
    g_mutex_unlock(&g_sg_mu);

    GError *error = nullptr;
    gchar *gscript = pcv_sg_nft_build_group_script(name, view);
    gboolean ok = _rebuild_dispatch_ex(gscript, &error);
    g_free(gscript);
    _rule_view_free(view);

    if (!ok) {
        PCV_LOG_ERROR("SG", "rule_remove '%s'/%ld nft 반영 실패 — 롤백: %s",
                      name, (long)rule_id, error ? error->message : "unknown");
        g_clear_error(&error);
        g_mutex_lock(&g_sg_mu);
        SecurityGroup *sg2 = g_hash_table_lookup(g_sg_map, name);
        if (sg2) g_ptr_array_add(sg2->rules, stolen);
        else     _sg_rule_free(stolen);
        g_mutex_unlock(&g_sg_mu);
        return FALSE;
    }
    _sg_db_delete_rule(rule_id);
    _sg_rule_free(stolen);
    PCV_LOG_INFO("SG", "Removed rule %ld from '%s'", (long)rule_id, name);
    return TRUE;
}

static void
_sg_set_evidence(gchar *dst, gsize dstsz, const gchar *ejstr)
{
    pcv_security_event_set_evidence(dst, dstsz, ejstr);
}

static void
_emit_sg_restore_failure_event(gint group_count, const GError *err)
{
    (void)pcv_security_store_ensure_open();

    PcvSecurityEvent ev = {0};
    ev.timestamp   = g_get_real_time() / G_USEC_PER_SEC;
    ev.source      = PCV_SECURITY_SOURCE_PCV_AUDIT;
    ev.type        = PCV_SECURITY_EVENT_AUDIT_PATTERN;
    ev.severity    = PCV_SECURITY_SEVERITY_CRIT;
    ev.confidence  = 100;
    ev.target_kind = PCV_SECURITY_TARGET_HOST;
    ev.status      = PCV_SECURITY_STATUS_OPEN;
    g_strlcpy(ev.target, "host", sizeof(ev.target));
    g_snprintf(ev.summary, sizeof(ev.summary),
               "security group nft 테이블 복원 실패 — 바인딩된 %d개 그룹의 VM 이 부팅 시 미필터 상태",
               group_count);
    g_strlcpy(ev.recommended_action, "sg-resync", sizeof(ev.recommended_action));
    JsonObject *ej = json_object_new();
    json_object_set_string_member(ej, "error",
        (err && err->message) ? err->message : "unknown");
    JsonNode *ejroot = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(ejroot, ej);
    gchar *ejstr = json_to_string(ejroot, FALSE);
    _sg_set_evidence(ev.evidence_json, sizeof ev.evidence_json, ejstr);
    g_free(ejstr);
    json_node_free(ejroot);
    pcv_security_event_make_id(&ev, "sg");
    GError *serr = nullptr;
    if (!pcv_security_store_insert_event(&ev, &serr)) {
        PCV_LOG_WARN("SG", "restore security_event 기록 실패: %s",
                     serr ? serr->message : "unknown");
        g_clear_error(&serr);
    }
}

void
pcv_security_group_restore(void)
{
    _sg_db_init();
    if (!g_sg_db) return;

    g_mutex_lock(&g_sg_mu);
    _ensure_init();

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(g_sg_db, "SELECT name, description FROM security_groups",
                       -1, &stmt, NULL) == SQLITE_OK && stmt) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const gchar *name = (const gchar *)sqlite3_column_text(stmt, 0);
            const gchar *desc = (const gchar *)sqlite3_column_text(stmt, 1);
            SecurityGroup *sg = g_new0(SecurityGroup, 1);
            sg->name = g_strdup(name);
            sg->description = g_strdup(desc ? desc : "");
            sg->rules = g_ptr_array_new_with_free_func(_sg_rule_free);
            sg->vm_bindings = g_ptr_array_new_with_free_func(g_free);
            g_hash_table_insert(g_sg_map, sg->name, sg);
        }
        sqlite3_finalize(stmt);
    }

    stmt = nullptr;
    if (sqlite3_prepare_v2(g_sg_db,
        "SELECT id, group_name, direction, protocol, port_start, port_end, source FROM sg_rules",
        -1, &stmt, NULL) == SQLITE_OK && stmt) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            gint64 id = sqlite3_column_int64(stmt, 0);
            const gchar *grp = (const gchar *)sqlite3_column_text(stmt, 1);
            SecurityGroup *sg = g_hash_table_lookup(g_sg_map, grp);
            if (!sg) continue;
            SgRule *r = g_new0(SgRule, 1);
            r->db_id = id;
            r->direction  = g_strdup((const gchar *)sqlite3_column_text(stmt, 2));
            r->protocol   = g_strdup((const gchar *)sqlite3_column_text(stmt, 3));
            r->port_start = sqlite3_column_int(stmt, 4);
            r->port_end   = sqlite3_column_int(stmt, 5);
            r->source     = g_strdup((const gchar *)sqlite3_column_text(stmt, 6));
            g_ptr_array_add(sg->rules, r);
        }
        sqlite3_finalize(stmt);
    }

    stmt = nullptr;
    if (sqlite3_prepare_v2(g_sg_db, "SELECT group_name, vm_name FROM sg_vm_bindings",
                       -1, &stmt, NULL) == SQLITE_OK && stmt) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const gchar *grp = (const gchar *)sqlite3_column_text(stmt, 0);
            const gchar *vm  = (const gchar *)sqlite3_column_text(stmt, 1);
            SecurityGroup *sg = g_hash_table_lookup(g_sg_map, grp);
            if (sg) g_ptr_array_add(sg->vm_bindings, g_strdup(vm));
        }
        sqlite3_finalize(stmt);
    }

    gint count = g_hash_table_size(g_sg_map);
    g_mutex_unlock(&g_sg_mu);

    _nft_teardown_legacy();

    GError *error = nullptr;
    if (!_nft_ensure(&error)) {
        gint gc = g_hash_table_size(g_sg_map);
        PCV_LOG_ERROR("SG", "restore: pcv_sg 테이블 준비 실패 — nft 반영 건너뜀: %s",
                      error ? error->message : "unknown");

        PCV_LOG_INFO("SG", "Loaded %d security groups from DB (nft 미반영 — 위 ERROR 참조)", gc);

        _emit_sg_restore_failure_event(gc, error);
        g_clear_error(&error);
        return;
    }

    g_mutex_lock(&g_sg_mu);
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *views = g_ptr_array_new();
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, g_sg_map);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        g_ptr_array_add(names, g_strdup((const gchar *)k));
        g_ptr_array_add(views, _snapshot_rule_view((SecurityGroup *)v));
    }
    g_mutex_unlock(&g_sg_mu);

    for (guint i = 0; i < names->len; i++) {
        gchar *script = pcv_sg_nft_build_group_script(
            g_ptr_array_index(names, i), g_ptr_array_index(views, i));
        if (!_nft_run_script(script, &error)) {
            PCV_LOG_ERROR("SG", "restore: 그룹 '%s' 체인 복원 실패: %s",
                          (gchar *)g_ptr_array_index(names, i),
                          error ? error->message : "unknown");
            g_clear_error(&error);
        }
        g_free(script);
        _rule_view_free(g_ptr_array_index(views, i));
    }
    g_ptr_array_unref(names);
    g_ptr_array_unref(views);

    if (!_rebuild_dispatch(&error)) {
        PCV_LOG_ERROR("SG", "restore: 디스패치 복원 실패: %s",
                      error ? error->message : "unknown");
        g_clear_error(&error);
    }

    if (count > 0)
        PCV_LOG_INFO("SG", "Restored %d security groups from database", count);
}

void
pcv_security_group_resync_all(void)
{

    g_mutex_lock(&g_sg_mu);
    _ensure_init();
    GHashTable *set = g_hash_table_new(g_str_hash, g_str_equal);
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, g_sg_map);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        SecurityGroup *sg = v;
        for (guint i = 0; i < sg->vm_bindings->len; i++)
            g_hash_table_add(set, g_ptr_array_index(sg->vm_bindings, i));
    }
    GPtrArray *vms = g_ptr_array_new_with_free_func(g_free);
    g_hash_table_iter_init(&it, set);
    while (g_hash_table_iter_next(&it, &k, &v))
        g_ptr_array_add(vms, g_strdup((const gchar *)k));
    g_hash_table_destroy(set);
    g_mutex_unlock(&g_sg_mu);

    if (vms->len == 0) {
        g_ptr_array_unref(vms);
        return;
    }

    for (guint i = 0; i < vms->len; i++) {
        const gchar *vm = g_ptr_array_index(vms, i);
        GPtrArray *ifaces = pcv_vm_iface_list(vm);
        if (ifaces->len > 0)
            pcv_vm_vnet_cache_put(vm, ifaces);
        g_ptr_array_unref(ifaces);
    }
    g_ptr_array_unref(vms);

    GError *error = nullptr;
    if (!_rebuild_dispatch(&error)) {
        PCV_LOG_WARN("SG", "resync_all: dispatch 재생성 실패 (다음 주기 재시도): %s",
                     error ? error->message : "unknown");
        g_clear_error(&error);
    }
}

static void
_sg_resync_worker(GTask *task, gpointer src, gpointer td, GCancellable *c)
{
    (void)src; (void)td; (void)c;
    pcv_security_group_resync_all();
    g_atomic_int_set(&g_sg_resync_inflight, 0);
    g_task_return_boolean(task, TRUE);
}

static gboolean
_sg_resync_tick(gpointer data)
{
    (void)data;
    if (!g_atomic_int_compare_and_exchange(&g_sg_resync_inflight, 0, 1))
        return G_SOURCE_CONTINUE;
    GTask *t = g_task_new(NULL, NULL, NULL, NULL);
    pcv_worker_pool_push(t, _sg_resync_worker);
    g_object_unref(t);
    return G_SOURCE_CONTINUE;
}

void
pcv_security_group_resync_timer_init(void)
{
    gint interval = pcv_config_get_int("security_group", "resync_interval_sec", 300);
    if (interval <= 0) {
        PCV_LOG_INFO("SG", "vnet resync 타이머 비활성 (resync_interval_sec=%d)", interval);
        return;
    }
    g_sg_resync_timer_id = g_timeout_add_seconds((guint)interval, _sg_resync_tick, NULL);
    PCV_LOG_INFO("SG", "vnet resync 타이머 등록 (%d초 주기)", interval);
}

void
pcv_security_group_resync_timer_shutdown(void)
{
    if (g_sg_resync_timer_id) {
        g_source_remove(g_sg_resync_timer_id);
        g_sg_resync_timer_id = 0;
    }
}

gboolean
pcv_security_group_detach_vm(const gchar *vm, const gchar *sg_name)
{
    if (!vm || !sg_name) return FALSE;

    if (!pcv_validate_vm_name(vm)) {
        PCV_LOG_WARN("SG", "Rejected detach: invalid vm '%s'", vm);
        return FALSE;
    }

    g_mutex_lock(&g_sg_mu);
    _ensure_init();
    SecurityGroup *sg = g_hash_table_lookup(g_sg_map, sg_name);
    gboolean found = FALSE;
    if (sg) {
        for (guint i = 0; i < sg->vm_bindings->len; i++) {
            if (g_strcmp0(g_ptr_array_index(sg->vm_bindings, i), vm) == 0) {
                g_ptr_array_remove_index(sg->vm_bindings, i);
                found = TRUE;
                break;
            }
        }
    }
    g_mutex_unlock(&g_sg_mu);
    if (!found) return FALSE;

    GError *error = nullptr;
    if (!_rebuild_dispatch(&error)) {
        PCV_LOG_ERROR("SG", "detach '%s'←VM '%s' nft 반영 실패 — 롤백: %s",
                      sg_name, vm, error ? error->message : "unknown");
        g_clear_error(&error);
        g_mutex_lock(&g_sg_mu);
        SecurityGroup *sg2 = g_hash_table_lookup(g_sg_map, sg_name);
        if (sg2) {

            gboolean dup = FALSE;
            for (guint i = 0; i < sg2->vm_bindings->len; i++) {
                if (g_strcmp0(g_ptr_array_index(sg2->vm_bindings, i), vm) == 0) {
                    dup = TRUE;
                    break;
                }
            }
            if (!dup) g_ptr_array_add(sg2->vm_bindings, g_strdup(vm));
        }
        g_mutex_unlock(&g_sg_mu);
        return FALSE;
    }
    _sg_db_delete_binding(sg_name, vm);

    if (!pcv_security_group_vm_is_bound(vm))
        pcv_vm_vnet_cache_evict(vm);
    PCV_LOG_INFO("SG", "Detached security group '%s' from VM '%s'", sg_name, vm);
    return TRUE;
}

static void
_emit_sg_sync_failure_event(const gchar *vm, const GError *err)
{
    PcvSecurityEvent ev = {0};
    ev.timestamp   = g_get_real_time() / G_USEC_PER_SEC;
    ev.source      = PCV_SECURITY_SOURCE_PCV_AUDIT;
    ev.type        = PCV_SECURITY_EVENT_AUDIT_PATTERN;
    ev.severity    = PCV_SECURITY_SEVERITY_CRIT;
    ev.confidence  = 100;
    ev.target_kind = PCV_SECURITY_TARGET_VM;
    ev.status      = PCV_SECURITY_STATUS_OPEN;
    g_strlcpy(ev.target, vm, sizeof(ev.target));
    g_snprintf(ev.summary, sizeof(ev.summary),
               "security group dispatch sync failed for VM '%s' (SG 미적용 상태)", vm);
    g_strlcpy(ev.recommended_action, "sg-resync", sizeof(ev.recommended_action));
    JsonObject *ej = json_object_new();
    json_object_set_string_member(ej, "error",
        (err && err->message) ? err->message : "unknown");
    JsonNode *ejroot = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(ejroot, ej);
    gchar *ejstr = json_to_string(ejroot, FALSE);
    _sg_set_evidence(ev.evidence_json, sizeof ev.evidence_json, ejstr);
    g_free(ejstr);
    json_node_free(ejroot);
    pcv_security_event_make_id(&ev, "sg");
    GError *serr = nullptr;
    if (!pcv_security_store_insert_event(&ev, &serr)) {
        PCV_LOG_WARN("SG", "security_event 기록 실패: %s",
                     serr ? serr->message : "unknown");
        g_clear_error(&serr);
    }
}

static gboolean
_vm_is_bound_locked(const gchar *vm)
{
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, g_sg_map);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        SecurityGroup *sg = v;
        for (guint i = 0; i < sg->vm_bindings->len; i++)
            if (g_strcmp0(g_ptr_array_index(sg->vm_bindings, i), vm) == 0)
                return TRUE;
    }
    return FALSE;
}

gboolean
pcv_security_group_vm_is_bound(const gchar *vm)
{
    if (!vm) return FALSE;
    g_mutex_lock(&g_sg_mu);
    _ensure_init();
    gboolean bound = _vm_is_bound_locked(vm);
    g_mutex_unlock(&g_sg_mu);
    return bound;
}

void
pcv_security_group_sync_vm(const gchar *vm_name)
{
    if (!vm_name) return;

    g_mutex_lock(&g_sg_mu);
    _ensure_init();
    gboolean bound = _vm_is_bound_locked(vm_name);
    g_mutex_unlock(&g_sg_mu);
    if (!bound) return;

    pcv_vm_vnet_cache_evict(vm_name);

    GError *error = nullptr;
    if (_rebuild_dispatch(&error)) return;
    g_clear_error(&error);
    if (_rebuild_dispatch(&error)) return;

    PCV_LOG_ERROR("SG", "sync_vm '%s' 실패 (재시도 포함) — SG 미적용 상태: %s",
                  vm_name, error ? error->message : "unknown");
    _emit_sg_sync_failure_event(vm_name, error);
    g_clear_error(&error);
}
