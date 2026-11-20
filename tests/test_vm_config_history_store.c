#include <glib.h>
#include <glib/gstdio.h>
#include <sqlite3.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/modules/core/vm_config_history_store.h"

#define DAY_MS 86400000LL
#define T0 1760000000000LL

typedef struct {
    gchar *dir;
    gchar *db;
    gchar *key;
} Tmp;

static void
tmp_init(Tmp *t)
{
    t->dir = g_dir_make_tmp("pcv-vmch-XXXXXX", NULL);
    g_assert_nonnull(t->dir);
    t->db = g_build_filename(t->dir, "vm_config_history.db", NULL);
    t->key = g_build_filename(t->dir, PCV_VMCH_KEY_NAME, NULL);
}

static void
tmp_clear(Tmp *t)
{
    GDir *d = g_dir_open(t->dir, 0, NULL);
    const gchar *n;
    while (d && (n = g_dir_read_name(d))) {
        gchar *p = g_build_filename(t->dir, n, NULL);
        g_unlink(p);
        g_free(p);
    }
    if (d)
        g_dir_close(d);
    g_rmdir(t->dir);
    g_free(t->dir);
    g_free(t->db);
    g_free(t->key);
}

static guint
mode_of(const gchar *p)
{
    struct stat sb;
    g_assert_cmpint(lstat(p, &sb), ==, 0);
    return sb.st_mode & 07777;
}

static PcvVmCfgRow *
mk(const gchar *uuid, const gchar *owner, const gchar *tenant, gint64 at, gint64 epoch, gsize snapshot_pad)
{
    PcvVmCfgRow *r = g_new0(PcvVmCfgRow, 1);
    r->vm_uuid = g_strdup(uuid);
    r->vm_name = g_strdup("vm");
    r->owner = g_strdup(owner);
    r->owner_tenant = g_strdup(tenant);
    r->kind = g_strdup("changed");
    r->scope = g_strdup("config");
    r->cause = g_strdup("reconcile");
    r->observed_at = at;
    r->attribution = g_strdup("rpc");
    r->actor = g_strdup("alice");
    r->actor_tenant = g_strdup("t1");
    r->actor_method = g_strdup("vm.limit");
    r->config_digest = g_strdup("d");
    gchar *pad = g_strnfill(snapshot_pad, 'x');
    r->snapshot_json = g_strdup_printf("{\"config\":{\"pad\":\"%s\"}}", pad);
    g_free(pad);
    r->changes_json = g_strdup("[]");
    r->normalizer_version = 1;
    r->epoch_id = epoch;
    return r;
}

static gchar *
put(PcvVmCfgStore *st, PcvVmCfgRow *r, const gchar *name, gboolean present)
{
    PcvVmCfgLatestUpdate lu = {.present = present, .current_name = name, .confirmed_at = r->observed_at};
    GError *e = NULL;
    g_assert_true(pcv_vm_cfg_store_insert(st, r, &lu, &e));
    g_assert_no_error(e);
    gchar *id = g_strdup(r->entry_id);
    pcv_vm_cfg_row_free(r);
    return id;
}

static PcvVmCfgStore *
open_w(const Tmp *t)
{
    GError *e = NULL;
    PcvVmCfgStore *st = pcv_vm_cfg_store_open(t->db, TRUE, &e);
    g_assert_no_error(e);
    g_assert_nonnull(st);
    return st;
}

static void
test_store_modes_0600(void)
{
    Tmp t;
    tmp_init(&t);
    g_assert_true(g_file_set_contents(t.db, "", 0, NULL));
    g_chmod(t.db, 0644);
    PcvVmCfgStore *st = open_w(&t);
    g_assert_true(pcv_vm_cfg_store_created(st));
    g_assert_cmphex(mode_of(t.db), ==, 0600);
    g_assert_cmphex(mode_of(t.key), ==, 0600);
    gsize len = 0;
    g_assert_nonnull(pcv_vm_cfg_store_salt(st, &len));
    g_assert_cmpuint(len, ==, PCV_VMCH_SALT_LEN);
    g_assert_cmpuint(strlen(pcv_vm_cfg_store_salt_id(st)), ==, 16);
    pcv_vm_cfg_store_close(st);
    st = open_w(&t);
    g_assert_false(pcv_vm_cfg_store_created(st));
    pcv_vm_cfg_store_close(st);

    gchar *real = g_build_filename(t.dir, "real.db", NULL);
    gchar *link = g_build_filename(t.dir, "link.db", NULL);
    g_assert_true(g_file_set_contents(real, "", 0, NULL));
    g_assert_cmpint(symlink(real, link), ==, 0);
    GError *e = NULL;
    g_assert_null(pcv_vm_cfg_store_open(link, TRUE, &e));
    g_assert_nonnull(e);
    g_clear_error(&e);
    g_unlink(t.key);
    g_assert_cmpint(symlink(real, t.key), ==, 0);
    g_assert_null(pcv_vm_cfg_store_open(t.db, TRUE, &e));
    g_assert_nonnull(e);
    g_assert_null(strstr(e->message, t.dir));
    g_clear_error(&e);
    g_unlink(link);
    g_unlink(real);
    g_free(real);
    g_free(link);
    tmp_clear(&t);
}

static GBytes *
slurp(const gchar *p)
{
    gchar *c = NULL;
    gsize n = 0;
    if (!g_file_get_contents(p, &c, &n, NULL))
        return g_bytes_new(NULL, 0);
    return g_bytes_new_take(c, n);
}

static gboolean
has_bytes(GBytes *b, const void *needle, gsize n)
{
    gsize len = 0;
    const guint8 *d = g_bytes_get_data(b, &len);
    return len >= n && memmem(d, len, needle, n) != NULL;
}

static void
test_store_salt_not_in_db(void)
{
    Tmp t;
    tmp_init(&t);
    PcvVmCfgStore *st = open_w(&t);
    gint64 ep = pcv_vm_cfg_store_epoch_begin(st, T0, NULL);
    g_free(put(st, mk("u1", "alice", "t1", T0, ep, 10), "web", TRUE));
    gsize len = 0;
    const guint8 *salt = pcv_vm_cfg_store_salt(st, &len);
    gchar *hex = g_malloc0(len * 2 + 1);
    for (gsize i = 0; i < len; i++)
        g_snprintf(hex + i * 2, 3, "%02x", salt[i]);
    GBytes *keyf = slurp(t.key);
    g_assert_cmpuint(g_bytes_get_size(keyf), ==, PCV_VMCH_SALT_LEN);
    g_assert_true(has_bytes(keyf, salt, len));
    gchar *wal = g_strconcat(t.db, "-wal", NULL);
    GBytes *dbb = slurp(t.db), *walb = slurp(wal);
    g_assert_false(has_bytes(dbb, salt, len));
    g_assert_false(has_bytes(dbb, hex, strlen(hex)));
    g_assert_false(has_bytes(walb, salt, len));
    g_assert_false(has_bytes(walb, hex, strlen(hex)));
    const gchar *sid = pcv_vm_cfg_store_salt_id(st);
    g_assert_true(has_bytes(dbb, sid, strlen(sid)) || has_bytes(walb, sid, strlen(sid)));
    pcv_vm_cfg_store_close(st);

    g_unlink(t.key);
    GError *e = NULL;
    PcvVmCfgStore *ro = pcv_vm_cfg_store_open(t.db, FALSE, &e);
    g_assert_no_error(e);
    g_assert_null(pcv_vm_cfg_store_salt(ro, NULL));
    g_assert_cmpuint(strlen(pcv_vm_cfg_store_salt_id(ro)), ==, 16);
    g_assert_false(g_file_test(t.key, G_FILE_TEST_EXISTS));
    pcv_vm_cfg_store_close(ro);
    g_bytes_unref(keyf);
    g_bytes_unref(dbb);
    g_bytes_unref(walb);
    g_free(wal);
    g_free(hex);
    tmp_clear(&t);
}

static void
test_store_newer_version_unavailable(void)
{
    Tmp t;
    tmp_init(&t);
    pcv_vm_cfg_store_close(open_w(&t));
    sqlite3 *db = NULL;
    g_assert_cmpint(sqlite3_open(t.db, &db), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_exec(db, "PRAGMA user_version=2", NULL, NULL, NULL), ==, SQLITE_OK);
    sqlite3_close(db);
    GError *e = NULL;
    g_assert_null(pcv_vm_cfg_store_open(t.db, TRUE, &e));
    g_assert_nonnull(e);
    g_clear_error(&e);
    g_assert_null(pcv_vm_cfg_store_open(t.db, FALSE, &e));
    g_clear_error(&e);
    tmp_clear(&t);
}

static void
test_store_auto_vacuum_incremental(void)
{
    Tmp t;
    tmp_init(&t);
    pcv_vm_cfg_store_close(open_w(&t));
    sqlite3 *db = NULL;
    sqlite3_stmt *s = NULL;
    g_assert_cmpint(sqlite3_open_v2(t.db, &db, SQLITE_OPEN_READONLY, NULL), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_prepare_v2(db, "PRAGMA auto_vacuum", -1, &s, NULL), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_step(s), ==, SQLITE_ROW);
    g_assert_cmpint(sqlite3_column_int(s, 0), ==, 2);
    sqlite3_finalize(s);
    g_assert_cmpint(sqlite3_prepare_v2(db, "PRAGMA journal_mode", -1, &s, NULL), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_step(s), ==, SQLITE_ROW);
    g_assert_cmpstr((const gchar *)sqlite3_column_text(s, 0), ==, "wal");
    sqlite3_finalize(s);
    sqlite3_close(db);
    tmp_clear(&t);
}

static void
test_store_insert_chain_prev(void)
{
    Tmp t;
    tmp_init(&t);
    PcvVmCfgStore *st = open_w(&t);
    gint64 ep = pcv_vm_cfg_store_epoch_begin(st, T0, NULL);
    PcvVmCfgRow *a = mk("u1", "alice", "t1", T0, ep, 1), *b = mk("u1", "alice", "t1", T0 + 5, ep, 1);
    PcvVmCfgLatestUpdate lu = {.present = TRUE, .current_name = "web", .confirmed_at = T0, .run_action = PCV_VMCH_RUN_SET,
                               .run_id = 7, .run_started_at = T0, .run_live_digest = "L"};
    g_assert_true(pcv_vm_cfg_store_insert(st, a, &lu, NULL));
    lu.run_action = PCV_VMCH_RUN_KEEP;
    g_assert_true(pcv_vm_cfg_store_insert(st, b, &lu, NULL));
    g_assert_cmpint(b->prev_id, ==, a->id);
    g_assert_cmpint(a->prev_id, ==, 0);
    g_assert_true(pcv_vm_cfg_entry_id_valid(a->entry_id));
    g_assert_cmpstr(a->entry_id, !=, b->entry_id);
    PcvVmCfgLatestRow l;
    g_assert_true(pcv_vm_cfg_store_latest(st, "u1", &l, NULL));
    g_assert_true(l.have_row);
    g_assert_cmpint(l.last_id, ==, b->id);
    g_assert_cmpint(l.run_id, ==, 7);
    g_assert_cmpstr(l.run_live_digest, ==, "L");
    g_assert_cmpstr(l.last->entry_id, ==, b->entry_id);
    pcv_vm_cfg_latest_clear(&l);
    PcvVmCfgRow *prev = pcv_vm_cfg_store_get_prev(st, b);
    g_assert_cmpstr(prev->entry_id, ==, a->entry_id);
    pcv_vm_cfg_row_free(prev);

    PcvVmCfgLatestUpdate clr = {.present = TRUE, .current_name = "web", .confirmed_at = T0 + 9,
                                .run_action = PCV_VMCH_RUN_CLEAR};
    g_assert_true(pcv_vm_cfg_store_touch(st, "u1", &clr, NULL));
    g_assert_true(pcv_vm_cfg_store_latest(st, "u1", &l, NULL));
    g_assert_cmpint(l.run_id, ==, -1);
    g_assert_cmpint(l.last_confirmed_at, ==, T0 + 9);
    pcv_vm_cfg_latest_clear(&l);
    g_assert_true(pcv_vm_cfg_store_latest(st, "unknown", &l, NULL));
    g_assert_false(l.have_row);
    pcv_vm_cfg_latest_clear(&l);
    pcv_vm_cfg_row_free(a);
    pcv_vm_cfg_row_free(b);
    pcv_vm_cfg_store_close(st);
    tmp_clear(&t);
}

static gboolean
only_a(const gchar *owner, const gchar *owner_tenant, gpointer ud)
{
    (void)owner_tenant;
    (*(guint *)ud)++;
    return !g_strcmp0(owner, "a");
}

static void
test_store_list_keyset_and_filter_then_limit(void)
{
    Tmp t;
    tmp_init(&t);
    PcvVmCfgStore *st = open_w(&t);
    gint64 ep = pcv_vm_cfg_store_epoch_begin(st, T0, NULL);
    for (gint i = 0; i < 30; i++)
        g_free(put(st, mk(i < 15 ? "ub" : "ua", i < 15 ? "b" : "a", "tx", T0 + (i < 15 ? 1000 + i : i), ep, 1),
                   "vm", TRUE));
    PcvVmCfgListFilter f = {.since = 0, .until = G_MAXINT64};
    GPtrArray *rows = g_ptr_array_new_with_free_func((GDestroyNotify)pcv_vm_cfg_row_free);
    gboolean more = FALSE;
    guint calls = 0;
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    guint pages = 0;
    do {
        g_ptr_array_set_size(rows, 0);
        g_assert_true(pcv_vm_cfg_store_list(st, &f, 5, only_a, &calls, rows, &more, NULL));
        for (guint i = 0; i < rows->len; i++) {
            PcvVmCfgRow *r = g_ptr_array_index(rows, i);
            g_assert_cmpstr(r->owner, ==, "a");
            g_assert_false(g_hash_table_contains(seen, r->entry_id));
            g_hash_table_add(seen, g_strdup(r->entry_id));
        }
        if (rows->len) {
            PcvVmCfgRow *last = g_ptr_array_index(rows, rows->len - 1);
            f.cursor_at = last->observed_at;
            g_strlcpy(f.cursor_id, last->entry_id, sizeof f.cursor_id);
        }
        if (pages == 0)
            g_assert_cmpuint(rows->len, ==, 5);
        pages++;
    } while (more && pages < 10);
    g_assert_cmpuint(g_hash_table_size(seen), ==, 15);
    g_assert_cmpuint(pages, ==, 3);
    g_assert_cmpuint(calls, >=, 30);

    PcvVmCfgListFilter g = {.since = 0, .until = G_MAXINT64, .uuid = "ub"};
    g_ptr_array_set_size(rows, 0);
    g_assert_true(pcv_vm_cfg_store_list(st, &g, 100, NULL, NULL, rows, &more, NULL));
    g_assert_cmpuint(rows->len, ==, 15);
    PcvVmCfgListFilter h = {.since = 0, .until = G_MAXINT64, .narrow = TRUE, .narrow_subject = "a"};
    g_ptr_array_set_size(rows, 0);
    g_assert_true(pcv_vm_cfg_store_list(st, &h, 100, NULL, NULL, rows, &more, NULL));
    g_assert_cmpuint(rows->len, ==, 15);
    PcvVmCfgListFilter k = {.since = T0 + 1000, .until = T0 + 1004};
    g_ptr_array_set_size(rows, 0);
    g_assert_true(pcv_vm_cfg_store_list(st, &k, 100, NULL, NULL, rows, &more, NULL));
    g_assert_cmpuint(rows->len, ==, 5);
    g_hash_table_unref(seen);
    g_ptr_array_unref(rows);
    pcv_vm_cfg_store_close(st);
    tmp_clear(&t);
}

static void
test_store_get_by_entry_id(void)
{
    Tmp t;
    tmp_init(&t);
    PcvVmCfgStore *st = open_w(&t);
    gint64 ep = pcv_vm_cfg_store_epoch_begin(st, T0, NULL);
    gchar *id = put(st, mk("u1", "alice", "t1", T0, ep, 1), "web", TRUE);
    PcvVmCfgRow *r = pcv_vm_cfg_store_get(st, id);
    g_assert_nonnull(r);
    g_assert_cmpstr(r->vm_uuid, ==, "u1");
    g_assert_cmpstr(r->actor_method, ==, "vm.limit");
    pcv_vm_cfg_row_free(r);
    g_assert_null(pcv_vm_cfg_store_get(st, "00000000000000000000000000000000"));
    g_free(id);
    pcv_vm_cfg_store_close(st);
    tmp_clear(&t);
}

static void
test_store_resolve_name_present_only(void)
{
    Tmp t;
    tmp_init(&t);
    PcvVmCfgStore *st = open_w(&t);
    gint64 ep = pcv_vm_cfg_store_epoch_begin(st, T0, NULL);
    g_free(put(st, mk("old-uuid", "alice", "t1", T0, ep, 1), "web", TRUE));
    gchar *u = pcv_vm_cfg_store_resolve_name(st, "web");
    g_assert_cmpstr(u, ==, "old-uuid");
    g_free(u);
    g_free(put(st, mk("old-uuid", "alice", "t1", T0 + 1, ep, 1), "web", FALSE));
    g_assert_null(pcv_vm_cfg_store_resolve_name(st, "web"));
    g_free(put(st, mk("new-uuid", "dave", "t3", T0 + 2, ep, 1), "web", TRUE));
    u = pcv_vm_cfg_store_resolve_name(st, "web");
    g_assert_cmpstr(u, ==, "new-uuid");
    g_free(u);
    g_free(put(st, mk("third", "x", NULL, T0 + 3, ep, 1), "web", TRUE));
    u = pcv_vm_cfg_store_resolve_name(st, "web");
    g_assert_cmpstr(u, ==, "third");
    g_free(u);
    g_assert_null(pcv_vm_cfg_store_resolve_name(st, "nope"));
    pcv_vm_cfg_store_close(st);
    tmp_clear(&t);
}

static void
test_store_prune_age_keeps_latest_present(void)
{
    Tmp t;
    tmp_init(&t);
    PcvVmCfgStore *st = open_w(&t);
    gint64 ep = pcv_vm_cfg_store_epoch_begin(st, T0 - 400 * DAY_MS, NULL);
    g_free(put(st, mk("live", "a", NULL, T0 - 400 * DAY_MS, ep, 1), "v1", TRUE));
    gchar *keep = put(st, mk("live", "a", NULL, T0 - 399 * DAY_MS, ep, 1), "v1", TRUE);
    g_free(put(st, mk("gone", "a", NULL, T0 - 398 * DAY_MS, ep, 1), "v2", FALSE));
    g_free(put(st, mk("fresh", "a", NULL, T0 - 1 * DAY_MS, ep, 1), "v3", TRUE));
    PcvVmCfgPruneResult res;
    g_assert_true(pcv_vm_cfg_store_prune(st, T0, 365, 30, 0, 0, NULL, 0, &res, NULL));
    g_assert_cmpuint(res.deleted_age, ==, 2);
    PcvVmCfgRow *r = pcv_vm_cfg_store_get(st, keep);
    g_assert_nonnull(r);
    pcv_vm_cfg_row_free(r);
    PcvVmCfgStoreInfo info;
    g_assert_true(pcv_vm_cfg_store_info(st, &info));
    g_assert_cmpint(info.rows, ==, 2);
    g_assert_cmpint(info.pruned_before, ==, T0 - 365 * DAY_MS);
    PcvVmCfgLatestRow l;
    pcv_vm_cfg_store_latest(st, "gone", &l, NULL);
    g_assert_false(l.have_row);
    pcv_vm_cfg_latest_clear(&l);
    g_free(keep);
    pcv_vm_cfg_store_close(st);
    tmp_clear(&t);
}

static void
test_store_prune_cap_fair(void)
{
    Tmp t;
    tmp_init(&t);
    PcvVmCfgStore *st = open_w(&t);
    gint64 ep = pcv_vm_cfg_store_epoch_begin(st, T0, NULL);
    for (gint i = 0; i < 5; i++)
        g_free(put(st, mk("quiet", "q", NULL, T0 + i, ep, 4000), "q", TRUE));
    for (gint i = 0; i < 150; i++)
        g_free(put(st, mk("noisy", "n", NULL, T0 + 100 + i, ep, 4000), "n", TRUE));
    gint64 before = pcv_vm_cfg_store_bytes(st);
    g_assert_cmpint(before, >, 500 * 1024);
    PcvVmCfgPruneResult res;
    g_assert_true(pcv_vm_cfg_store_prune(st, T0 + 1000, 365, 30, 300 * 1024, 20, NULL, 0, &res, NULL));
    g_assert_cmpuint(res.deleted_cap, >, 0);
    g_assert_cmpuint(pcv_vm_cfg_store_rows_since(st, "quiet", 0), ==, 5);
    g_assert_cmpuint(pcv_vm_cfg_store_rows_since(st, "noisy", 0), <, 150);
    g_assert_cmpint(pcv_vm_cfg_store_bytes(st), <=, 300 * 1024);
    g_assert_false(res.over_cap);
    pcv_vm_cfg_store_close(st);
    tmp_clear(&t);
}

static void
test_store_prune_cap_many_ties_converges(void)
{
    Tmp t;
    tmp_init(&t);
    PcvVmCfgStore *st = open_w(&t);
    gint64 ep = pcv_vm_cfg_store_epoch_begin(st, T0, NULL);
    for (gint v = 0; v < 200; v++) {
        gchar *u = g_strdup_printf("tie-%03d", v);
        for (gint i = 0; i < 8; i++)
            g_free(put(st, mk(u, "o", NULL, T0 + v * 10 + i, ep, 2000), u, TRUE));
        g_free(u);
    }
    gint64 before = pcv_vm_cfg_store_bytes(st);
    g_assert_cmpint(before, >, 3 * 1024 * 1024);
    PcvVmCfgPruneResult res;
    g_assert_true(pcv_vm_cfg_store_prune(st, T0 + 10000, 365, 30, 1536 * 1024, 20, NULL, 0, &res, NULL));
    g_assert_false(res.over_cap);
    g_assert_false(res.interrupted);
    g_assert_cmpuint(res.deleted_cap, >, 400);
    g_assert_cmpint(pcv_vm_cfg_store_bytes(st), <=, 1536 * 1024);
    pcv_vm_cfg_store_close(st);
    tmp_clear(&t);
}

static void
test_store_prune_honors_stop(void)
{
    Tmp t;
    tmp_init(&t);
    PcvVmCfgStore *st = open_w(&t);
    gint64 ep = pcv_vm_cfg_store_epoch_begin(st, T0, NULL);
    for (gint i = 0; i < 150; i++)
        g_free(put(st, mk("noisy", "n", NULL, T0 + i, ep, 4000), "n", TRUE));
    gint stop = 1;
    PcvVmCfgPruneResult res;
    g_assert_true(pcv_vm_cfg_store_prune(st, T0 + 1000, 365, 30, 300 * 1024, 20, &stop, 0, &res, NULL));
    g_assert_true(res.interrupted);
    g_assert_cmpuint(res.deleted_cap, ==, 0);
    g_assert_cmpuint(pcv_vm_cfg_store_rows_since(st, "noisy", 0), ==, 150);
    stop = 0;
    g_assert_true(pcv_vm_cfg_store_prune(st, T0 + 1000, 365, 30, 300 * 1024, 20, &stop, 0, &res, NULL));
    g_assert_false(res.interrupted);
    g_assert_cmpuint(res.deleted_cap, >, 0);
    pcv_vm_cfg_store_close(st);
    tmp_clear(&t);
}

static void
test_store_key_atomic_create(void)
{
    Tmp t;
    tmp_init(&t);
    gchar *tmp = g_strconcat(t.key, ".tmp", NULL);
    g_assert_true(g_file_set_contents(tmp, "stale", 5, NULL));
    g_assert_true(g_file_set_contents(t.key, "", 0, NULL));
    g_chmod(t.key, 0600);
    PcvVmCfgStore *st = open_w(&t);
    GBytes *k = slurp(t.key);
    g_assert_cmpuint(g_bytes_get_size(k), ==, PCV_VMCH_SALT_LEN);
    g_bytes_unref(k);
    g_assert_cmphex(mode_of(t.key), ==, 0600);
    g_assert_false(g_file_test(tmp, G_FILE_TEST_EXISTS));
    pcv_vm_cfg_store_close(st);
    g_free(tmp);
    tmp_clear(&t);
}

static void
test_store_actor_retention(void)
{
    Tmp t;
    tmp_init(&t);
    PcvVmCfgStore *st = open_w(&t);
    gint64 ep = pcv_vm_cfg_store_epoch_begin(st, T0, NULL);
    gchar *old = put(st, mk("u", "a", NULL, T0 - 31 * DAY_MS, ep, 1), "v", TRUE);
    gchar *recent = put(st, mk("u", "a", NULL, T0 - 29 * DAY_MS, ep, 1), "v", TRUE);
    PcvVmCfgPruneResult res;
    g_assert_true(pcv_vm_cfg_store_prune(st, T0, 365, 30, 0, 0, NULL, 0, &res, NULL));
    g_assert_cmpuint(res.actor_expired, ==, 1);
    PcvVmCfgRow *r = pcv_vm_cfg_store_get(st, old);
    g_assert_null(r->actor);
    g_assert_null(r->actor_tenant);
    g_assert_true(r->actor_expired);
    g_assert_cmpstr(r->actor_method, ==, "vm.limit");
    g_assert_cmpstr(r->attribution, ==, "rpc");
    pcv_vm_cfg_row_free(r);
    r = pcv_vm_cfg_store_get(st, recent);
    g_assert_cmpstr(r->actor, ==, "alice");
    g_assert_false(r->actor_expired);
    pcv_vm_cfg_row_free(r);
    g_free(old);
    g_free(recent);
    pcv_vm_cfg_store_close(st);
    tmp_clear(&t);
}

static void
test_store_epoch_prune(void)
{
    Tmp t;
    tmp_init(&t);
    PcvVmCfgStore *st = open_w(&t);
    gint64 e1 = pcv_vm_cfg_store_epoch_begin(st, T0 - 500 * DAY_MS, NULL);
    pcv_vm_cfg_store_epoch_end(st, e1, T0 - 499 * DAY_MS, "shutdown", NULL);
    gint64 e2 = pcv_vm_cfg_store_epoch_begin(st, T0 - 450 * DAY_MS, NULL);
    pcv_vm_cfg_store_epoch_end(st, e2, T0 - 449 * DAY_MS, "shutdown", NULL);
    g_free(put(st, mk("u", "a", NULL, T0 - 450 * DAY_MS, e2, 1), "v", TRUE));
    gint64 e3 = pcv_vm_cfg_store_epoch_begin(st, T0 - 10 * DAY_MS, NULL);
    PcvVmCfgPruneResult res;
    g_assert_true(pcv_vm_cfg_store_prune(st, T0, 365, 30, 0, 0, NULL, 0, &res, NULL));
    g_assert_cmpuint(res.epochs_deleted, ==, 0);
    g_free(put(st, mk("v", "a", NULL, T0 - 400 * DAY_MS, e3, 1), "w", FALSE));
    g_assert_true(pcv_vm_cfg_store_prune(st, T0, 365, 30, 0, 0, NULL, 0, &res, NULL));
    g_assert_cmpuint(res.deleted_age, ==, 1);
    g_assert_cmpuint(res.epochs_deleted, ==, 1);
    GArray *eps = pcv_vm_cfg_store_epochs(st, 0, T0);
    g_assert_cmpuint(eps->len, ==, 2);
    g_array_unref(eps);
    pcv_vm_cfg_store_close(st);
    tmp_clear(&t);
}

static void
test_store_crash_inferred_close(void)
{
    Tmp t;
    tmp_init(&t);
    PcvVmCfgStore *st = open_w(&t);
    gint64 e = pcv_vm_cfg_store_epoch_begin(st, T0, NULL);
    g_assert_true(pcv_vm_cfg_store_epoch_heartbeat(st, e, T0 + 300000, NULL));
    pcv_vm_cfg_store_close(st);
    st = open_w(&t);
    g_assert_cmpuint(pcv_vm_cfg_store_close_open_epochs_crash(st, NULL), ==, 1);
    g_assert_cmpint(pcv_vm_cfg_store_last_epoch_end(st), ==, T0 + 300000);
    GArray *eps = pcv_vm_cfg_store_epochs(st, 0, T0 + 900000);
    g_assert_cmpuint(eps->len, ==, 1);
    g_assert_cmpstr(g_array_index(eps, PcvVmCfgEpoch, 0).end_reason, ==, "crash_inferred");
    g_array_unref(eps);
    g_assert_cmpuint(pcv_vm_cfg_store_close_open_epochs_crash(st, NULL), ==, 0);
    pcv_vm_cfg_store_close(st);
    tmp_clear(&t);
}

static void
test_store_readonly_concurrent(void)
{
    Tmp t;
    tmp_init(&t);
    PcvVmCfgStore *st = open_w(&t);
    gint64 ep = pcv_vm_cfg_store_epoch_begin(st, T0, NULL);
    g_free(put(st, mk("u", "a", NULL, T0, ep, 1), "v", TRUE));
    sqlite3 *w = NULL;
    g_assert_cmpint(sqlite3_open(t.db, &w), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_exec(w, "BEGIN IMMEDIATE; INSERT INTO meta(key, value) VALUES('probe', 'x');", NULL, NULL,
                                 NULL),
                    ==, SQLITE_OK);
    GError *e = NULL;
    PcvVmCfgStore *ro = pcv_vm_cfg_store_open(t.db, FALSE, &e);
    g_assert_no_error(e);
    PcvVmCfgListFilter f = {.since = 0, .until = G_MAXINT64};
    GPtrArray *rows = g_ptr_array_new_with_free_func((GDestroyNotify)pcv_vm_cfg_row_free);
    gboolean more = FALSE;
    g_assert_true(pcv_vm_cfg_store_list(ro, &f, 10, NULL, NULL, rows, &more, NULL));
    g_assert_cmpuint(rows->len, ==, 1);
    g_ptr_array_unref(rows);
    pcv_vm_cfg_store_close(ro);
    sqlite3_exec(w, "ROLLBACK", NULL, NULL, NULL);
    sqlite3_close(w);
    pcv_vm_cfg_store_close(st);
    tmp_clear(&t);
}

void test_vm_config_history_store_register(void);
void
test_vm_config_history_store_register(void)
{
    g_test_add_func("/vm_config_history/store/store_modes_0600", test_store_modes_0600);
    g_test_add_func("/vm_config_history/store/store_salt_not_in_db", test_store_salt_not_in_db);
    g_test_add_func("/vm_config_history/store/store_key_atomic_create", test_store_key_atomic_create);
    g_test_add_func("/vm_config_history/store/store_newer_version_unavailable", test_store_newer_version_unavailable);
    g_test_add_func("/vm_config_history/store/store_auto_vacuum_incremental", test_store_auto_vacuum_incremental);
    g_test_add_func("/vm_config_history/store/store_insert_chain_prev", test_store_insert_chain_prev);
    g_test_add_func("/vm_config_history/store/store_list_keyset_and_filter_then_limit",
                    test_store_list_keyset_and_filter_then_limit);
    g_test_add_func("/vm_config_history/store/store_get_by_entry_id", test_store_get_by_entry_id);
    g_test_add_func("/vm_config_history/store/store_resolve_name_present_only", test_store_resolve_name_present_only);
    g_test_add_func("/vm_config_history/store/store_prune_age_keeps_latest_present",
                    test_store_prune_age_keeps_latest_present);
    g_test_add_func("/vm_config_history/store/store_prune_cap_fair", test_store_prune_cap_fair);
    g_test_add_func("/vm_config_history/store/store_prune_cap_many_ties_converges", test_store_prune_cap_many_ties_converges);
    g_test_add_func("/vm_config_history/store/store_prune_honors_stop", test_store_prune_honors_stop);
    g_test_add_func("/vm_config_history/store/store_actor_retention", test_store_actor_retention);
    g_test_add_func("/vm_config_history/store/store_epoch_prune", test_store_epoch_prune);
    g_test_add_func("/vm_config_history/store/store_crash_inferred_close", test_store_crash_inferred_close);
    g_test_add_func("/vm_config_history/store/store_readonly_concurrent", test_store_readonly_concurrent);
}
