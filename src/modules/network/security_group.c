

















































#include <glib.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <sqlite3.h>
#include "../../utils/pcv_spawn.h"
#include "../../utils/pcv_log.h"












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

static void _sg_db_save_group(const gchar *name, const gchar *desc) {
    if (!g_sg_db) return;
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(g_sg_db, "INSERT OR REPLACE INTO security_groups(name,description) VALUES(?,?)", -1, &stmt, NULL) != SQLITE_OK || !stmt) return;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, desc ? desc : "", -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

static void _sg_db_delete_group(const gchar *name) {
    if (!g_sg_db) return;
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(g_sg_db, "DELETE FROM security_groups WHERE name=?", -1, &stmt, NULL) != SQLITE_OK || !stmt) return;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
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
    sqlite3_step(stmt);
    gint64 row_id = sqlite3_last_insert_rowid(g_sg_db);
    sqlite3_finalize(stmt);
    return row_id;
}

static void _sg_db_delete_rule(gint64 rule_id) {
    if (!g_sg_db) return;
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(g_sg_db, "DELETE FROM sg_rules WHERE id=?", -1, &stmt, NULL) != SQLITE_OK || !stmt) return;
    sqlite3_bind_int64(stmt, 1, rule_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

static void _sg_db_save_binding(const gchar *group, const gchar *vm) {
    if (!g_sg_db) return;
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(g_sg_db, "INSERT OR IGNORE INTO sg_vm_bindings(group_name,vm_name) VALUES(?,?)", -1, &stmt, NULL) != SQLITE_OK || !stmt) return;
    sqlite3_bind_text(stmt, 1, group, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, vm, -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}






static void
_nft_ensure_table(void)
{
    const gchar *argv[] = {"nft", "add", "table", "inet", "purecvisor", NULL};
    gchar *std_err = nullptr;
    GError *error = nullptr;
    if (!pcv_spawn_sync(argv, NULL, &std_err, &error)) {
        PCV_LOG_WARN("SG", "nft add table failed: %s",
            error ? error->message : (std_err ? std_err : "unknown"));
        if (error) g_error_free(error);
    }
    g_free(std_err);
}


static void _nft_add_default_deny(const gchar *group_id);





static void
_nft_create_chain(const gchar *group_id)
{
    _nft_ensure_table();


    gchar *in_chain = g_strdup_printf("sg-%s-in", group_id);
    const gchar *in_argv[] = {
        "nft", "add", "chain", "inet", "purecvisor", in_chain,
        "{ type filter hook input priority 0 ; }", NULL
    };
    gchar *std_err = nullptr;
    GError *error = nullptr;
    if (!pcv_spawn_sync(in_argv, NULL, &std_err, &error)) {
        PCV_LOG_WARN("SG", "nft add chain %s failed: %s", in_chain,
            error ? error->message : (std_err ? std_err : "unknown"));
        if (error) g_error_free(error);
    }
    g_free(std_err);
    g_free(in_chain);


    gchar *out_chain = g_strdup_printf("sg-%s-out", group_id);
    const gchar *out_argv[] = {
        "nft", "add", "chain", "inet", "purecvisor", out_chain,
        "{ type filter hook output priority 0 ; }", NULL
    };
    std_err = nullptr;
    error = nullptr;
    if (!pcv_spawn_sync(out_argv, NULL, &std_err, &error)) {
        PCV_LOG_WARN("SG", "nft add chain %s failed: %s", out_chain,
            error ? error->message : (std_err ? std_err : "unknown"));
        if (error) g_error_free(error);
    }
    g_free(std_err);
    g_free(out_chain);


    _nft_add_default_deny(group_id);
}





static void
_nft_delete_chain(const gchar *group_id)
{
    const gchar *suffixes[] = {"in", "out", NULL};
    for (gint s = 0; suffixes[s]; s++) {
        gchar *chain = g_strdup_printf("sg-%s-%s", group_id, suffixes[s]);


        const gchar *flush_argv[] = {
            "nft", "flush", "chain", "inet", "purecvisor", chain, NULL
        };
        pcv_spawn_fire(flush_argv);


        const gchar *del_argv[] = {
            "nft", "delete", "chain", "inet", "purecvisor", chain, NULL
        };
        gchar *std_err = nullptr;
        GError *error = nullptr;
        if (!pcv_spawn_sync(del_argv, NULL, &std_err, &error)) {
            PCV_LOG_WARN("SG", "nft delete chain %s failed: %s", chain,
                error ? error->message : (std_err ? std_err : "unknown"));
            if (error) g_error_free(error);
        }
        g_free(std_err);
        g_free(chain);
    }
}





static void
_nft_add_default_deny(const gchar *group_id)
{
    const gchar *suffixes[] = {"in", "out", NULL};
    for (gint i = 0; suffixes[i]; i++) {
        gchar *chain = g_strdup_printf("sg-%s-%s", group_id, suffixes[i]);
        const gchar *argv[] = {
            "nft", "add", "rule", "inet", "purecvisor", chain, "drop", NULL
        };
        (void)pcv_spawn_sync(argv, NULL, NULL, NULL);
        g_free(chain);
    }
}











static gboolean
_nft_apply_rule(const gchar *group_id, const gchar *direction,
                const gchar *protocol, gint port_start, gint port_end,
                const gchar *cidr)
{

    gchar *chain = g_strdup_printf("sg-%s-%s", group_id,
        (g_strcmp0(direction, "egress") == 0) ? "out" : "in");

    gchar *port_str = nullptr;
    if (port_start > 0) {
        if (port_end > port_start) {
            port_str = g_strdup_printf("%d-%d", port_start, port_end);
        } else {
            port_str = g_strdup_printf("%d", port_start);
        }
    }



    const gchar *argv[14];
    gint i = 0;
    argv[i++] = "nft";
    argv[i++] = "add";
    argv[i++] = "rule";
    argv[i++] = "inet";
    argv[i++] = "purecvisor";
    argv[i++] = chain;


    if (cidr && g_strcmp0(cidr, "0.0.0.0/0") != 0) {
        argv[i++] = "ip";
        argv[i++] = (g_strcmp0(direction, "egress") == 0) ? "daddr" : "saddr";
        argv[i++] = cidr;
    }


    if (protocol && g_strcmp0(protocol, "icmp") != 0) {
        argv[i++] = protocol;
        if (port_str) {
            argv[i++] = "dport";
            argv[i++] = port_str;
        }
    } else if (protocol) {
        argv[i++] = protocol;
    }

    argv[i++] = "accept";
    argv[i] = nullptr;

    gchar *std_err = nullptr;
    GError *error = nullptr;
    gboolean ok = pcv_spawn_sync(argv, NULL, &std_err, &error);
    if (!ok) {
        PCV_LOG_WARN("SG", "nft add rule to %s failed: %s", chain,
            error ? error->message : (std_err ? std_err : "unknown"));
        if (error) g_error_free(error);
    }
    g_free(std_err);
    g_free(chain);
    g_free(port_str);
    return ok;
}





static void
_nft_rebuild_group(const gchar *name)
{

    const gchar *suffixes[] = {"in", "out", NULL};
    for (gint i = 0; suffixes[i]; i++) {
        gchar *chain = g_strdup_printf("sg-%s-%s", name, suffixes[i]);
        const gchar *flush_argv[] = {
            "nft", "flush", "chain", "inet", "purecvisor", chain, NULL
        };
        (void)pcv_spawn_sync(flush_argv, NULL, NULL, NULL);
        g_free(chain);
    }


    g_mutex_lock(&g_sg_mu);
    SecurityGroup *sg = g_hash_table_lookup(g_sg_map, name);
    if (sg) {
        for (guint i = 0; i < sg->rules->len; i++) {
            SgRule *r = g_ptr_array_index(sg->rules, i);
            _nft_apply_rule(name, r->direction, r->protocol,
                            r->port_start, r->port_end, r->source);
        }
    }
    g_mutex_unlock(&g_sg_mu);


    _nft_add_default_deny(name);
}










[[nodiscard]] gboolean
pcv_security_group_create(const gchar *name, const gchar *description)
{
    if (!name || !*name) return FALSE;

    _sg_db_init();

    g_mutex_lock(&g_sg_mu);
    _ensure_init();

    if (g_hash_table_contains(g_sg_map, name)) {
        g_mutex_unlock(&g_sg_mu);
        return FALSE;
    }

    SecurityGroup *sg = g_new0(SecurityGroup, 1);
    sg->name = g_strdup(name);
    sg->description = g_strdup(description ? description : "");
    sg->rules = g_ptr_array_new_with_free_func(_sg_rule_free);
    sg->vm_bindings = g_ptr_array_new_with_free_func(g_free);

    g_hash_table_insert(g_sg_map, sg->name, sg);
    g_mutex_unlock(&g_sg_mu);


    _sg_db_save_group(name, description);


    _nft_create_chain(name);
    PCV_LOG_INFO("SG", "Created security group '%s' with nftables chains (in/out)", name);
    return TRUE;
}




gboolean
pcv_security_group_delete(const gchar *name)
{
    if (!name) return FALSE;

    g_mutex_lock(&g_sg_mu);
    _ensure_init();
    gboolean removed = g_hash_table_remove(g_sg_map, name);
    g_mutex_unlock(&g_sg_mu);

    if (removed) {
        _sg_db_delete_group(name);
        _nft_delete_chain(name);
        PCV_LOG_INFO("SG", "Deleted security group '%s' and nftables chains", name);
    }
    return removed;
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

    g_mutex_lock(&g_sg_mu);
    _ensure_init();

    SecurityGroup *sg = g_hash_table_lookup(g_sg_map, name);
    if (!sg) {
        g_mutex_unlock(&g_sg_mu);
        return FALSE;
    }

    SgRule *r = g_new0(SgRule, 1);
    r->direction = g_strdup(json_object_has_member(rule, "direction")
        ? json_object_get_string_member(rule, "direction") : "ingress");
    r->protocol = g_strdup(json_object_has_member(rule, "protocol")
        ? json_object_get_string_member(rule, "protocol") : "tcp");
    r->port_start = json_object_has_member(rule, "port_start")
        ? (gint)json_object_get_int_member(rule, "port_start")
        : (json_object_has_member(rule, "port")
            ? (gint)json_object_get_int_member(rule, "port") : 0);
    r->port_end = json_object_has_member(rule, "port_end")
        ? (gint)json_object_get_int_member(rule, "port_end") : 0;
    r->source = g_strdup(json_object_has_member(rule, "source")
        ? json_object_get_string_member(rule, "source") : "0.0.0.0/0");


    r->db_id = _sg_db_save_rule(name, r->direction, r->protocol,
                                r->port_start, r->port_end, r->source);

    g_ptr_array_add(sg->rules, r);
    g_mutex_unlock(&g_sg_mu);


    _nft_rebuild_group(name);
    PCV_LOG_INFO("SG", "Applied rule to '%s': %s %s port %d-%d from %s",
        name, r->direction, r->protocol, r->port_start, r->port_end, r->source);
    return TRUE;
}








gboolean
pcv_security_group_apply_to_vm(const gchar *vm, const gchar *sg_name)
{
    if (!vm || !sg_name) return FALSE;

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


    guint rule_count = sg->rules->len;
    SgRule **rule_copy = nullptr;
    if (rule_count > 0) {
        rule_copy = g_new(SgRule *, rule_count);
        for (guint i = 0; i < rule_count; i++)
            rule_copy[i] = g_ptr_array_index(sg->rules, i);
    }
    const gchar *sg_id = sg->name;

    g_ptr_array_add(sg->vm_bindings, g_strdup(vm));
    g_mutex_unlock(&g_sg_mu);


    _sg_db_save_binding(sg_name, vm);


    for (guint i = 0; i < rule_count; i++) {
        _nft_apply_rule(sg_id, rule_copy[i]->direction,
            rule_copy[i]->protocol, rule_copy[i]->port_start,
            rule_copy[i]->port_end, rule_copy[i]->source);
    }
    g_free(rule_copy);
    PCV_LOG_INFO("SG", "Applied security group '%s' to VM '%s' (%u rules)",
        sg_name, vm, rule_count);
    return TRUE;
}








gboolean
pcv_security_group_rule_remove(const gchar *name, gint64 rule_id)
{
    if (!name) return FALSE;

    g_mutex_lock(&g_sg_mu);
    _ensure_init();
    SecurityGroup *sg = g_hash_table_lookup(g_sg_map, name);
    if (!sg) {
        g_mutex_unlock(&g_sg_mu);
        return FALSE;
    }

    gboolean found = FALSE;
    for (guint i = 0; i < sg->rules->len; i++) {
        SgRule *r = g_ptr_array_index(sg->rules, i);
        if (r->db_id == rule_id) {
            g_ptr_array_remove_index(sg->rules, i);
            found = TRUE;
            break;
        }
    }
    g_mutex_unlock(&g_sg_mu);

    if (!found) return FALSE;

    _sg_db_delete_rule(rule_id);

    _nft_rebuild_group(name);
    PCV_LOG_INFO("SG", "Removed rule %ld from '%s'", (long)rule_id, name);
    return TRUE;
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
            _nft_create_chain(name);
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
            _nft_apply_rule(grp, r->direction, r->protocol,
                            r->port_start, r->port_end, r->source);
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


    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, g_sg_map);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        _nft_add_default_deny((const gchar *)key);
    }

    gint count = g_hash_table_size(g_sg_map);
    g_mutex_unlock(&g_sg_mu);

    if (count > 0)
        PCV_LOG_INFO("SG", "Restored %d security groups from database", count);
}
