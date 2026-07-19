
#include <glib.h>
#include <json-glib/json-glib.h>
#include <unistd.h>
#include <string.h>

#include "../src/utils/pcv_spawn.h"
#include "../src/modules/network/vm_vnet_cache.h"
#include <sqlite3.h>

extern gboolean pcv_security_group_create(const gchar *name, const gchar *description);
extern gboolean pcv_security_group_rule_add(const gchar *name, JsonObject *rule);
extern gboolean pcv_security_group_delete(const gchar *name);
extern gboolean pcv_security_group_apply_to_vm(const gchar *vm, const gchar *sg_name);
extern gboolean pcv_security_group_detach_vm(const gchar *vm, const gchar *sg_name);
extern gboolean pcv_security_group_vm_is_bound(const gchar *vm);
extern gboolean pcv_security_group_rule_remove(const gchar *name, gint64 rule_id);
extern void pcv_security_group_resync_all(void);

#define SGV5_GROUP "sgv5"

static void _ensure_group(void) {
    gboolean created = pcv_security_group_create(SGV5_GROUP, "V5 injection regression");
    (void)created;
}

static JsonObject *_rule(const gchar *direction, const gchar *protocol,
                         const gchar *source, gint port) {
    JsonObject *o = json_object_new();
    if (direction) json_object_set_string_member(o, "direction", direction);
    if (protocol)  json_object_set_string_member(o, "protocol", protocol);
    if (source)    json_object_set_string_member(o, "source", source);
    if (port > 0)  json_object_set_int_member(o, "port", port);
    return o;
}

static void test_sg_reject_source_injection(void) {
    _ensure_group();
    JsonObject *rule = _rule("ingress", "tcp",
                             "1.2.3.4/32 accept; flush ruleset #", 80);
    g_assert_false(pcv_security_group_rule_add(SGV5_GROUP, rule));
    json_object_unref(rule);
}

static void test_sg_reject_protocol_injection(void) {
    _ensure_group();
    JsonObject *rule = _rule("ingress", "tcp; drop", "10.0.0.0/24", 80);
    g_assert_false(pcv_security_group_rule_add(SGV5_GROUP, rule));
    json_object_unref(rule);
}

static void test_sg_reject_protocol_unknown(void) {
    _ensure_group();
    JsonObject *rule = _rule("ingress", "sctp", "10.0.0.0/24", 80);
    g_assert_false(pcv_security_group_rule_add(SGV5_GROUP, rule));
    json_object_unref(rule);
}

static void test_sg_reject_direction(void) {
    _ensure_group();
    JsonObject *rule = _rule("bogus", "tcp", "10.0.0.0/24", 80);
    g_assert_false(pcv_security_group_rule_add(SGV5_GROUP, rule));
    json_object_unref(rule);
}

static void test_sg_reject_bad_group_name(void) {
    gboolean created = pcv_security_group_create("sg; flush", "bad name");
    g_assert_false(created);
}

static void test_sg_accept_clean(void) {
    if (geteuid() != 0) {
        g_test_skip("root(+netns) 필요 — nft 성공이 TRUE 의 전제");
        return;
    }
    _ensure_group();
    JsonObject *rule = _rule("ingress", "tcp", "10.0.0.0/24", 80);
    g_assert_true(pcv_security_group_rule_add(SGV5_GROUP, rule));
    json_object_unref(rule);
}

static void test_sg_nft_state_integration(void) {
    if (geteuid() != 0) {
        g_test_skip("root(+netns) 필요");
        return;
    }

    (void)pcv_security_group_create("sgint", "integration");
    JsonObject *rule = _rule("ingress", "tcp", "10.9.8.0/24", 8080);
    g_assert_true(pcv_security_group_rule_add("sgint", rule));
    json_object_unref(rule);

    gchar *out = NULL;
    const gchar *ls[] = {"nft", "list", "table", "bridge", "pcv_sg", NULL};
    g_assert_true(pcv_spawn_sync(ls, &out, NULL, NULL));
    g_assert_nonnull(strstr(out, "ingress-dispatch"));
    g_assert_nonnull(strstr(out, "egress-dispatch"));
    g_assert_nonnull(strstr(out, "baseline-in"));
    g_assert_nonnull(strstr(out, "sg-sgint-in"));
    g_assert_nonnull(strstr(out, "10.9.8.0/24"));

    g_assert_null(strstr(out, "hook input"));
    g_assert_null(strstr(out, "hook output"));

    g_assert_null(strstr(out, "drop"));
    g_free(out);

    g_assert_true(pcv_security_group_delete("sgint"));
    out = NULL;
    g_assert_true(pcv_spawn_sync(ls, &out, NULL, NULL));
    g_assert_null(strstr(out, "sg-sgint-in"));
    g_free(out);
}

static void test_sg_dispatch_reads_cache(void) {
    if (geteuid() != 0) { g_test_skip("root(+netns) 필요"); return; }

    (void)pcv_security_group_create("sgcache", "I-2 cache proof");
    JsonObject *r1 = _rule("ingress", "tcp", "10.7.0.0/24", 443);
    g_assert_true(pcv_security_group_rule_add("sgcache", r1));
    json_object_unref(r1);

    g_assert_true(pcv_security_group_apply_to_vm("ghost-vm", "sgcache"));

    GPtrArray *fake = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(fake, g_strdup("vnetCACHED"));
    pcv_vm_vnet_cache_put("ghost-vm", fake);
    g_ptr_array_unref(fake);

    JsonObject *r2 = _rule("ingress", "udp", "10.7.0.0/24", 53);
    g_assert_true(pcv_security_group_rule_add("sgcache", r2));
    json_object_unref(r2);

    gchar *out = NULL;
    const gchar *ls[] = {"nft", "list", "table", "bridge", "pcv_sg", NULL};
    g_assert_true(pcv_spawn_sync(ls, &out, NULL, NULL));
    g_assert_nonnull(strstr(out, "vnetCACHED"));
    g_free(out);

    (void)pcv_security_group_detach_vm("ghost-vm", "sgcache");
    pcv_vm_vnet_cache_evict("ghost-vm");
    g_assert_true(pcv_security_group_delete("sgcache"));
}

static void test_sg_rule_add_rolls_back_on_db_write_fail(void) {
    if (geteuid() != 0) { g_test_skip("root(+netns) 필요"); return; }

    const char *db_path = "/var/lib/purecvisor/security_groups.db";

    g_assert_true(pcv_security_group_create("sgr8", "R8 write-fail"));

    sqlite3 *aux = NULL;
    g_assert_cmpint(sqlite3_open(db_path, &aux), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_exec(aux, "DELETE FROM security_groups WHERE name='sgr8';",
                                 NULL, NULL, NULL), ==, SQLITE_OK);
    sqlite3_close(aux);

    JsonObject *rule = _rule("ingress", "tcp", "10.55.44.0/24", 8080);
    gboolean added = pcv_security_group_rule_add("sgr8", rule);
    json_object_unref(rule);
    g_assert_false(added);

    gchar *out = NULL;
    const gchar *ls[] = {"nft", "list", "table", "bridge", "pcv_sg", NULL};
    if (pcv_spawn_sync(ls, &out, NULL, NULL)) {
        g_assert_null(strstr(out ? out : "", "10.55.44.0/24"));
        g_free(out);
    }

    g_assert_true(pcv_security_group_delete("sgr8"));
}

static void test_sg_apply_evicts_stale(void) {
    if (geteuid() != 0) { g_test_skip("root(+netns) 필요"); return; }

    (void)pcv_security_group_create("sgstale", "C1 regression");
    JsonObject *rule = _rule("ingress", "tcp", "10.8.0.0/24", 22);
    g_assert_true(pcv_security_group_rule_add("sgstale", rule));
    json_object_unref(rule);

    GPtrArray *stale = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(stale, g_strdup("vnetSTALE"));
    pcv_vm_vnet_cache_put("ghost2-vm", stale);
    g_ptr_array_unref(stale);

    g_assert_true(pcv_security_group_apply_to_vm("ghost2-vm", "sgstale"));

    gchar *out = NULL;
    const gchar *ls[] = {"nft", "list", "table", "bridge", "pcv_sg", NULL};
    g_assert_true(pcv_spawn_sync(ls, &out, NULL, NULL));

    g_assert_null(strstr(out, "vnetSTALE"));
    g_free(out);

    (void)pcv_security_group_detach_vm("ghost2-vm", "sgstale");
    pcv_vm_vnet_cache_evict("ghost2-vm");
    g_assert_true(pcv_security_group_delete("sgstale"));
}

static void test_sg_vm_is_bound(void) {
    (void)pcv_security_group_create("sgbind", "bind gate");
    g_assert_false(pcv_security_group_vm_is_bound("vm-unbound"));
    g_assert_false(pcv_security_group_vm_is_bound(NULL));

    gboolean applied = pcv_security_group_apply_to_vm("vm-gate", "sgbind");
    g_assert_cmpint(pcv_security_group_vm_is_bound("vm-gate"), ==, applied);

    if (applied) {
        (void)pcv_security_group_detach_vm("vm-gate", "sgbind");
        g_assert_false(pcv_security_group_vm_is_bound("vm-gate"));
    }
    (void)pcv_security_group_delete("sgbind");
}

static void test_sg_rule_remove_rejects_nonpositive_id(void) {
    g_assert_false(pcv_security_group_rule_remove("sgv5", 0));
    g_assert_false(pcv_security_group_rule_remove("sgv5", -1));
    g_assert_false(pcv_security_group_rule_remove(NULL, 5));
}

static void test_sg_resync_keeps_cache_on_empty(void) {
    if (geteuid() != 0) { g_test_skip("root(+netns) 필요"); return; }

    (void)pcv_security_group_create("sgrsync", "I2-R1 resync");
    JsonObject *rule = _rule("ingress", "tcp", "10.6.0.0/24", 22);
    g_assert_true(pcv_security_group_rule_add("sgrsync", rule));
    json_object_unref(rule);

    g_assert_true(pcv_security_group_apply_to_vm("ghost-rsync", "sgrsync"));

    GPtrArray *fake = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(fake, g_strdup("vnetRSYNC"));
    pcv_vm_vnet_cache_put("ghost-rsync", fake);
    g_ptr_array_unref(fake);

    pcv_security_group_resync_all();

    gchar *out = NULL;
    const gchar *ls[] = {"nft", "list", "table", "bridge", "pcv_sg", NULL};
    g_assert_true(pcv_spawn_sync(ls, &out, NULL, NULL));
    g_assert_nonnull(strstr(out, "vnetRSYNC"));
    g_free(out);

    (void)pcv_security_group_detach_vm("ghost-rsync", "sgrsync");
    pcv_vm_vnet_cache_evict("ghost-rsync");
    g_assert_true(pcv_security_group_delete("sgrsync"));
}

static void test_sg_detach_reaps_cache(void) {
    if (geteuid() != 0) { g_test_skip("root(+netns) 필요"); return; }

    (void)pcv_security_group_create("sgreap", "I2-R3 detach reap");
    JsonObject *rule = _rule("ingress", "tcp", "10.5.0.0/24", 22);
    g_assert_true(pcv_security_group_rule_add("sgreap", rule));
    json_object_unref(rule);

    g_assert_true(pcv_security_group_apply_to_vm("ghost-reap", "sgreap"));
    GPtrArray *fake = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(fake, g_strdup("vnetREAP"));
    pcv_vm_vnet_cache_put("ghost-reap", fake);
    g_ptr_array_unref(fake);

    g_assert_true(pcv_security_group_detach_vm("ghost-reap", "sgreap"));

    GPtrArray *after = pcv_vm_vnet_cache_get("ghost-reap");
    g_assert_null(after);

    (void)pcv_security_group_delete("sgreap");
}

static void test_sg_detach_retains_multigroup(void) {
    if (geteuid() != 0) { g_test_skip("root(+netns) 필요"); return; }

    (void)pcv_security_group_create("sgmulti-a", "R3 multi a");
    (void)pcv_security_group_create("sgmulti-b", "R3 multi b");
    JsonObject *ra = _rule("ingress", "tcp", "10.5.1.0/24", 80);
    g_assert_true(pcv_security_group_rule_add("sgmulti-a", ra));
    json_object_unref(ra);
    JsonObject *rb = _rule("ingress", "tcp", "10.5.2.0/24", 443);
    g_assert_true(pcv_security_group_rule_add("sgmulti-b", rb));
    json_object_unref(rb);

    g_assert_true(pcv_security_group_apply_to_vm("ghost-multi", "sgmulti-a"));
    g_assert_true(pcv_security_group_apply_to_vm("ghost-multi", "sgmulti-b"));
    GPtrArray *fake = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(fake, g_strdup("vnetMULTI"));
    pcv_vm_vnet_cache_put("ghost-multi", fake);
    g_ptr_array_unref(fake);

    g_assert_true(pcv_security_group_detach_vm("ghost-multi", "sgmulti-a"));

    GPtrArray *after = pcv_vm_vnet_cache_get("ghost-multi");
    g_assert_nonnull(after);
    g_ptr_array_unref(after);

    (void)pcv_security_group_detach_vm("ghost-multi", "sgmulti-b");
    pcv_vm_vnet_cache_evict("ghost-multi");
    (void)pcv_security_group_delete("sgmulti-a");
    (void)pcv_security_group_delete("sgmulti-b");
}

static void test_sg_delete_reaps_cache(void) {
    if (geteuid() != 0) { g_test_skip("root(+netns) 필요"); return; }

    (void)pcv_security_group_create("sgdelreap", "I2-R3 delete reap");
    JsonObject *rule = _rule("ingress", "tcp", "10.5.3.0/24", 22);
    g_assert_true(pcv_security_group_rule_add("sgdelreap", rule));
    json_object_unref(rule);

    g_assert_true(pcv_security_group_apply_to_vm("ghost-del", "sgdelreap"));
    GPtrArray *fake = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(fake, g_strdup("vnetDEL"));
    pcv_vm_vnet_cache_put("ghost-del", fake);
    g_ptr_array_unref(fake);

    g_assert_true(pcv_security_group_delete("sgdelreap"));

    GPtrArray *after = pcv_vm_vnet_cache_get("ghost-del");
    g_assert_null(after);
}

static void test_sg_delete_retains_multigroup(void) {
    if (geteuid() != 0) { g_test_skip("root(+netns) 필요"); return; }

    (void)pcv_security_group_create("sgdelm-a", "R3 del multi a");
    (void)pcv_security_group_create("sgdelm-b", "R3 del multi b");
    JsonObject *ra = _rule("ingress", "tcp", "10.5.4.0/24", 80);
    g_assert_true(pcv_security_group_rule_add("sgdelm-a", ra));
    json_object_unref(ra);
    JsonObject *rb = _rule("ingress", "tcp", "10.5.5.0/24", 443);
    g_assert_true(pcv_security_group_rule_add("sgdelm-b", rb));
    json_object_unref(rb);

    g_assert_true(pcv_security_group_apply_to_vm("ghost-delm", "sgdelm-a"));
    g_assert_true(pcv_security_group_apply_to_vm("ghost-delm", "sgdelm-b"));
    GPtrArray *fake = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(fake, g_strdup("vnetDELM"));
    pcv_vm_vnet_cache_put("ghost-delm", fake);
    g_ptr_array_unref(fake);

    g_assert_true(pcv_security_group_delete("sgdelm-a"));

    GPtrArray *after = pcv_vm_vnet_cache_get("ghost-delm");
    g_assert_nonnull(after);
    g_ptr_array_unref(after);

    (void)pcv_security_group_detach_vm("ghost-delm", "sgdelm-b");
    pcv_vm_vnet_cache_evict("ghost-delm");
    (void)pcv_security_group_delete("sgdelm-b");
}

void test_security_group_register(void) {
    g_test_add_func("/security_group/reject_source_injection",   test_sg_reject_source_injection);
    g_test_add_func("/security_group/reject_protocol_injection", test_sg_reject_protocol_injection);
    g_test_add_func("/security_group/reject_protocol_unknown",   test_sg_reject_protocol_unknown);
    g_test_add_func("/security_group/reject_direction",          test_sg_reject_direction);
    g_test_add_func("/security_group/reject_bad_group_name",     test_sg_reject_bad_group_name);
    g_test_add_func("/security_group/accept_clean",              test_sg_accept_clean);
    g_test_add_func("/security_group/nft_state_integration",     test_sg_nft_state_integration);
    g_test_add_func("/security_group/dispatch_reads_cache",      test_sg_dispatch_reads_cache);
    g_test_add_func("/security_group/apply_evicts_stale",        test_sg_apply_evicts_stale);
    g_test_add_func("/security_group/vm_is_bound",               test_sg_vm_is_bound);
    g_test_add_func("/security_group/rule_remove_rejects_nonpositive_id",
                    test_sg_rule_remove_rejects_nonpositive_id);
    g_test_add_func("/security_group/resync_keeps_cache_on_empty",
                    test_sg_resync_keeps_cache_on_empty);
    g_test_add_func("/security_group/detach_reaps_cache",       test_sg_detach_reaps_cache);
    g_test_add_func("/security_group/detach_retains_multigroup", test_sg_detach_retains_multigroup);
    g_test_add_func("/security_group/delete_reaps_cache",        test_sg_delete_reaps_cache);
    g_test_add_func("/security_group/delete_retains_multigroup", test_sg_delete_retains_multigroup);
    g_test_add_func("/security_group/rule_add_rolls_back_on_db_write_fail",
                    test_sg_rule_add_rolls_back_on_db_write_fail);
}
