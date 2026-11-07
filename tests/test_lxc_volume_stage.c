#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include "modules/lxc/lxc_volume_stage.h"

static void test_runtime_config_path(void)
{
    gchar *tmp = g_dir_make_tmp("pcvpathXXXXXX", NULL);
    g_assert_nonnull(tmp);
    gchar *rt = g_build_filename(tmp, "runtime", NULL);
    pcv_lxc_volume_set_roots_for_test(tmp, rt);
    gchar *p = pcv_lxc_runtime_config_path("ct");
    gchar *want = g_build_filename(rt, "ct", "config", NULL);
    g_assert_cmpstr(p, ==, want);
    pcv_lxc_volume_set_roots_for_test(NULL, NULL);
    g_free(p); g_free(want); g_free(rt); g_free(tmp);
}

static void test_runtime_config_remove_lifecycle(void)
{
    gchar *tmp = g_dir_make_tmp("pcvstageXXXXXX", NULL);
    g_assert_nonnull(tmp);
    gchar *vstage = g_build_filename(tmp, "volstage", NULL);
    gchar *rt = g_build_filename(tmp, "runtime", NULL);
    pcv_lxc_volume_set_roots_for_test(vstage, rt);

    gchar *ct_rt = g_build_filename(rt, "ct", NULL);
    g_mkdir_with_parents(ct_rt, 0700);
    gchar *cfg = g_build_filename(ct_rt, "config", NULL);
    g_assert_true(g_file_set_contents(cfg, "x\n", -1, NULL));
    g_assert_true(g_file_test(cfg, G_FILE_TEST_EXISTS));

    pcv_lxc_runtime_config_remove("ct");
    g_assert_false(g_file_test(ct_rt, G_FILE_TEST_EXISTS));

    pcv_lxc_runtime_config_remove("ct");
    pcv_lxc_volstage_clear("ct");

    pcv_lxc_volume_set_roots_for_test(NULL, NULL);
    g_free(ct_rt); g_free(cfg); g_free(vstage); g_free(rt); g_free(tmp);
}

static void test_name_guard(void)
{
    pcv_lxc_runtime_config_remove("../escape");
    pcv_lxc_runtime_config_remove("a/b");
    pcv_lxc_volstage_clear(".");
    pcv_lxc_volstage_clear(NULL);
}

void test_lxc_volume_stage_register(void);
void test_lxc_volume_stage_register(void)
{
    g_test_add_func("/lxc_volume_stage/runtime_config_path", test_runtime_config_path);
    g_test_add_func("/lxc_volume_stage/runtime_config_remove_lifecycle", test_runtime_config_remove_lifecycle);
    g_test_add_func("/lxc_volume_stage/name_guard", test_name_guard);
}
