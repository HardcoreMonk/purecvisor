























#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <string.h>

#include "modules/lxc/lxc_driver.h"



static void
test_bw_net_index_prefers_exact_name(void)
{

    const gchar *names[] = {"eth0", "eth1", "mgmt0"};
    g_assert_cmpint(pcv_lxc_bw_net_index(names, 3, "eth0"), ==, 0);
    g_assert_cmpint(pcv_lxc_bw_net_index(names, 3, "mgmt0"), ==, 2);
}

static void
test_bw_net_index_unnamed_slot_uses_kernel_default(void)
{

    const gchar *names[] = {"", NULL, "data"};
    g_assert_cmpint(pcv_lxc_bw_net_index(names, 3, "eth0"), ==, 0);
    g_assert_cmpint(pcv_lxc_bw_net_index(names, 3, "eth1"), ==, 1);

    g_assert_cmpint(pcv_lxc_bw_net_index(names, 3, "eth2"), ==, -1);
}

static void
test_bw_net_index_rejects_unknown(void)
{
    const gchar *names[] = {"eth0"};
    g_assert_cmpint(pcv_lxc_bw_net_index(names, 1, "eth1"), ==, -1);
    g_assert_cmpint(pcv_lxc_bw_net_index(names, 1, "eth"), ==, -1);
    g_assert_cmpint(pcv_lxc_bw_net_index(names, 1, "eth01x"), ==, -1);
    g_assert_cmpint(pcv_lxc_bw_net_index(names, 1, "wlan0"), ==, -1);
    g_assert_cmpint(pcv_lxc_bw_net_index(names, 1, NULL), ==, -1);
    g_assert_cmpint(pcv_lxc_bw_net_index(NULL, 0, "eth0"), ==, -1);
}



static void
test_bw_normalize_veth_accepts_lxc_names(void)
{
    gchar *v = pcv_lxc_bw_normalize_veth("vethAB12cd");
    g_assert_cmpstr(v, ==, "vethAB12cd");
    g_free(v);
    v = pcv_lxc_bw_normalize_veth("  vethQ9x2Lm\n");
    g_assert_cmpstr(v, ==, "vethQ9x2Lm");
    g_free(v);
}

static void
test_bw_normalize_veth_rejects_unsafe_values(void)
{

    const gchar *bad[] = {
        "", "   ", "7:vethAB12cd@if6:<BROADCAST,MULTICAST,UP>mtu1500",
        "veth;reboot", "-vethX", "veth0123456789abc", NULL,
    };
    for (gsize i = 0; bad[i]; i++)
        g_assert_null(pcv_lxc_bw_normalize_veth(bad[i]));
    g_assert_null(pcv_lxc_bw_normalize_veth(NULL));
}



static void
test_set_bandwidth_missing_container_is_error(void)
{
    GError *error = NULL;

    g_assert_false(pcv_lxc_set_bandwidth("zbc-z16-no-such-ctr", NULL, 0, 3000, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error(&error);
    g_assert_false(pcv_lxc_set_bandwidth("zbc-z16-no-such-ctr", "eth0", 5000, 0, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error(&error);
}

static void
test_set_bandwidth_rejects_empty_or_invalid_request(void)
{
    GError *error = NULL;

    g_assert_false(pcv_lxc_set_bandwidth("zbc-z16-no-such-ctr", NULL, 0, 0, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);
    g_assert_false(pcv_lxc_set_bandwidth(NULL, NULL, 1, 1, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);
    g_assert_false(pcv_lxc_set_bandwidth("bad name;x", NULL, 1, 1, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);
    g_assert_false(pcv_lxc_set_bandwidth("zbc-z16-no-such-ctr", "eth0;x", 1, 1, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);
}



static void
test_rootless_explicit_request_fails_closed(void)
{

    g_assert_cmpint(pcv_lxc_rootless_decide(1, FALSE, FALSE), ==, PCV_LXC_ROOTLESS_REJECT);
    g_assert_cmpint(pcv_lxc_rootless_decide(1, TRUE, FALSE), ==, PCV_LXC_ROOTLESS_REJECT);
    g_assert_cmpint(pcv_lxc_rootless_decide(1, FALSE, TRUE), ==, PCV_LXC_ROOTLESS_ON);
}

static void
test_rootless_default_and_explicit_off_unchanged(void)
{

    g_assert_cmpint(pcv_lxc_rootless_decide(0, TRUE, TRUE), ==, PCV_LXC_ROOTLESS_OFF);
    g_assert_cmpint(pcv_lxc_rootless_decide(-1, FALSE, TRUE), ==, PCV_LXC_ROOTLESS_OFF);
    g_assert_cmpint(pcv_lxc_rootless_decide(-1, TRUE, TRUE), ==, PCV_LXC_ROOTLESS_ON);

    g_assert_cmpint(pcv_lxc_rootless_decide(-1, TRUE, FALSE), ==, PCV_LXC_ROOTLESS_OFF);
}

static gchar *
write_subid(const gchar *dir, const gchar *file, const gchar *contents)
{
    gchar *path = g_build_filename(dir, file, NULL);
    g_assert_true(g_file_set_contents(path, contents, -1, NULL));
    return path;
}

static void
test_rootless_subid_requires_both_ranges(void)
{
    GError *error = NULL;
    gchar *dir = g_dir_make_tmp("pcv-z17-subid-XXXXXX", &error);
    g_assert_no_error(error);
    gchar *uid_ok = write_subid(dir, "subuid", "root:100000:65536\n");
    gchar *gid_ok = write_subid(dir, "subgid", "root:100000:65536\n");
    gchar *gid_short = write_subid(dir, "subgid.short", "root:100000:1000\n");
    gchar *missing = g_build_filename(dir, "absent", NULL);

    g_assert_true(pcv_lxc_rootless_subid_available(uid_ok, gid_ok, 100000, 65536));
    g_assert_false(pcv_lxc_rootless_subid_available(uid_ok, gid_short, 100000, 65536));
    g_assert_false(pcv_lxc_rootless_subid_available(missing, gid_ok, 100000, 65536));
    g_assert_false(pcv_lxc_rootless_subid_available(uid_ok, missing, 100000, 65536));

    g_unlink(uid_ok); g_unlink(gid_ok); g_unlink(gid_short);
    g_rmdir(dir);
    g_free(uid_ok); g_free(gid_ok); g_free(gid_short); g_free(missing); g_free(dir);
}

void
test_lxc_driver_register(void)
{
    g_test_add_func("/lxc_driver/bandwidth/net_index_prefers_exact_name",
                    test_bw_net_index_prefers_exact_name);
    g_test_add_func("/lxc_driver/bandwidth/net_index_unnamed_slot_uses_kernel_default",
                    test_bw_net_index_unnamed_slot_uses_kernel_default);
    g_test_add_func("/lxc_driver/bandwidth/net_index_rejects_unknown",
                    test_bw_net_index_rejects_unknown);
    g_test_add_func("/lxc_driver/bandwidth/normalize_veth_accepts_lxc_names",
                    test_bw_normalize_veth_accepts_lxc_names);
    g_test_add_func("/lxc_driver/bandwidth/normalize_veth_rejects_unsafe_values",
                    test_bw_normalize_veth_rejects_unsafe_values);
    g_test_add_func("/lxc_driver/bandwidth/missing_container_is_error",
                    test_set_bandwidth_missing_container_is_error);
    g_test_add_func("/lxc_driver/bandwidth/rejects_empty_or_invalid_request",
                    test_set_bandwidth_rejects_empty_or_invalid_request);
    g_test_add_func("/lxc_driver/rootless/explicit_request_fails_closed",
                    test_rootless_explicit_request_fails_closed);
    g_test_add_func("/lxc_driver/rootless/default_and_explicit_off_unchanged",
                    test_rootless_default_and_explicit_off_unchanged);
    g_test_add_func("/lxc_driver/rootless/subid_requires_both_ranges",
                    test_rootless_subid_requires_both_ranges);
}
