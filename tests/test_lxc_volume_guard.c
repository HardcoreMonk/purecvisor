#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include "modules/lxc/lxc_volume_guard.h"

static void test_dest_canon(void)
{
    gchar *a = pcv_lxc_volume_dest_canon("/mnt//./data/");
    g_assert_cmpstr(a, ==, "mnt/data");
    g_free(a);
    gchar *b = pcv_lxc_volume_dest_canon("/");
    g_assert_cmpstr(b, ==, "");
    g_free(b);
    gchar *c = pcv_lxc_volume_dest_canon(NULL);
    g_assert_cmpstr(c, ==, "");
    g_free(c);
}

static void test_volstage_slot_deterministic(void)
{
    gchar *s0 = pcv_lxc_volstage_slot("/run/purecvisor-volstage", "ct", "/mnt/data", 0);
    gchar *s0b = pcv_lxc_volstage_slot("/run/purecvisor-volstage", "ct", "mnt/data", 0);
    gchar *s1 = pcv_lxc_volstage_slot("/run/purecvisor-volstage", "ct", "/mnt/data", 1);
    g_assert_cmpstr(s0, ==, s0b);
    g_assert_cmpstr(s0, !=, s1);
    g_assert_true(g_str_has_prefix(s0, "/run/purecvisor-volstage/ct/"));
    g_assert_true(g_str_has_suffix(s0, ".0"));
    g_free(s0);
    g_free(s0b);
    g_free(s1);
}

static void test_runtime_config_replaces_source(void)
{
    const gchar *cfg =
        "lxc.rootfs.path = dir:/srv/ct/rootfs\n"
        "lxc.mount.entry = /host/secret mnt/data none bind,ro 0 0\n"
        "lxc.net.0.type = empty\n";
    GError *e = NULL;
    gchar *rt = pcv_lxc_runtime_config_text(cfg, "/tmp/volstage-seam", "ct", &e);
    g_assert_no_error(e);
    g_assert_nonnull(rt);
    g_assert_null(strstr(rt, "/host/secret"));
    g_assert_nonnull(strstr(rt, "/tmp/volstage-seam/ct/"));
    g_assert_nonnull(strstr(rt, "lxc.rootfs.path = dir:/srv/ct/rootfs"));
    g_assert_nonnull(strstr(rt, "lxc.net.0.type = empty"));
    g_free(rt);
}

static void test_runtime_config_skips_non_bind(void)
{
    const gchar *cfg =
        "lxc.mount.entry = proc proc proc nodev,noexec 0 0\n"
        "lxc.mount.entry = /host/x mnt/x none bind 0 0\n";
    GError *e = NULL;
    gchar *rt = pcv_lxc_runtime_config_text(cfg, "/run/purecvisor-volstage", "ct", &e);
    g_assert_no_error(e);
    g_assert_nonnull(strstr(rt, "lxc.mount.entry = proc proc proc nodev,noexec 0 0"));
    g_assert_null(strstr(rt, "/host/x"));
    g_free(rt);
}

static void test_config_root_gid(void)
{
    gint64 gid = -1;
    g_assert_true(pcv_lxc_config_root_gid("lxc.idmap = g 0 1000000 65536\n", &gid));
    g_assert_cmpint(gid, ==, 1000000);
    g_assert_false(pcv_lxc_config_root_gid("lxc.net.0.type = empty\n", NULL));
    g_assert_false(pcv_lxc_config_root_gid("lxc.idmap = u 0 1000000 65536\n", NULL));
}

static void test_source_open_rejects_bad(void)
{
    GError *e = NULL;
    int fd = pcv_lxc_volume_source_open("relative/path", NULL, &e);
    g_assert_cmpint(fd, ==, -1);
    g_assert_true(g_error_matches(e, PCV_LXC_MOUNT_ERROR, PCV_LXC_MOUNT_ERROR_BAD_PATH));
    g_clear_error(&e);
    fd = pcv_lxc_volume_source_open("/etc/../etc", NULL, &e);
    g_assert_cmpint(fd, ==, -1);
    g_assert_true(g_error_matches(e, PCV_LXC_MOUNT_ERROR, PCV_LXC_MOUNT_ERROR_BAD_PATH));
    g_clear_error(&e);
}

static void test_config_with_guard_hook_idempotent(void)
{
    const gchar *cfg = "lxc.mount.entry = /host/x mnt/x none bind 0 0\n";
    gchar *h1 = pcv_lxc_config_with_guard_hook(cfg);
    g_assert_nonnull(h1);
    g_assert_nonnull(strstr(h1, PCV_LXC_VOLUME_GUARD_HOOK_VALUE));
    gchar *h2 = pcv_lxc_config_with_guard_hook(h1);
    g_assert_null(h2);
    g_free(h1);
    gchar *none = pcv_lxc_config_with_guard_hook("lxc.net.0.type = empty\n");
    g_assert_null(none);
}

static gchar *write_cfg(const gchar *dir, const gchar *name, const gchar *body)
{
    gchar *cdir = g_build_filename(dir, name, NULL);
    g_mkdir_with_parents(cdir, 0700);
    gchar *path = g_build_filename(cdir, "config", NULL);
    g_file_set_contents(path, body, -1, NULL);
    g_free(cdir);
    return path;
}

static void test_hook_judgment(void)
{
    gchar *tmp = g_dir_make_tmp("pcvguardXXXXXX", NULL);
    g_assert_nonnull(tmp);
    gchar *stage = g_build_filename(tmp, "stage", NULL);

    gchar *inside = g_strdup_printf("lxc.mount.entry = %s/stage/ct/abc.0 mnt/x none bind,ro 0 0\n", tmp);
    gchar *p_inside = write_cfg(tmp, "ct", inside);
    g_assert_cmpint(pcv_lxc_volume_guard_hook_run_with_root(stage, p_inside, "ct"), ==, 0);

    gchar *outside = g_strdup("lxc.mount.entry = /host/secret mnt/x none bind,ro 0 0\n");
    gchar *p_outside = write_cfg(tmp, "ct2", outside);
    g_assert_cmpint(pcv_lxc_volume_guard_hook_run_with_root(stage, p_outside, "ct2"), ==, 1);

    gchar *rel = g_strdup("lxc.mount.entry = relsrc mnt/x none bind,ro 0 0\n");
    gchar *p_rel = write_cfg(tmp, "ct3", rel);
    g_assert_cmpint(pcv_lxc_volume_guard_hook_run_with_root(stage, p_rel, "ct3"), ==, 1);

    g_assert_cmpint(pcv_lxc_volume_guard_hook_run_with_root(stage, "/no/such/config", "ct"), ==, 1);

    g_free(inside); g_free(outside); g_free(rel);
    g_free(p_inside); g_free(p_outside); g_free(p_rel);
    g_free(stage); g_free(tmp);
}

void test_lxc_volume_guard_register(void);
void test_lxc_volume_guard_register(void)
{
    g_test_add_func("/lxc_volume_guard/dest_canon", test_dest_canon);
    g_test_add_func("/lxc_volume_guard/volstage_slot_deterministic", test_volstage_slot_deterministic);
    g_test_add_func("/lxc_volume_guard/runtime_config_replaces_source", test_runtime_config_replaces_source);
    g_test_add_func("/lxc_volume_guard/runtime_config_skips_non_bind", test_runtime_config_skips_non_bind);
    g_test_add_func("/lxc_volume_guard/config_root_gid", test_config_root_gid);
    g_test_add_func("/lxc_volume_guard/source_open_rejects_bad", test_source_open_rejects_bad);
    g_test_add_func("/lxc_volume_guard/config_with_guard_hook_idempotent", test_config_with_guard_hook_idempotent);
    g_test_add_func("/lxc_volume_guard/hook_judgment", test_hook_judgment);
}
