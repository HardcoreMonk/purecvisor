
#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

extern gboolean pcv_dpdk_is_available(void);
extern JsonObject *pcv_dpdk_status(void);
extern JsonObject *pcv_dpdk_hugepage_info(void);
extern JsonArray *pcv_dpdk_list(void);
extern gboolean pcv_dpdk_unbind(const gchar *pci_addr, GError **error);
extern gboolean pcv_dpdk_bridge_delete(const gchar *name, GError **error);
extern gboolean pcv_dpdk_bridge_create(const gchar *name, const gchar *dpdk_port,
                                       GError **error);

extern gboolean pcv_validate_pci_addr(const gchar *addr);

extern gboolean pcv_dpdk_nic_is_protected(const gchar *pci_addr, gchar **reason);
extern gboolean pcv_dpdk_route_is_default_dev(const gchar *netdev, const gchar *proc_base);

static void test_dpdk_status_structure(void) {
    JsonObject *obj = pcv_dpdk_status();
    g_assert_nonnull(obj);
    g_assert_true(json_object_has_member(obj, "available"));
    g_assert_true(json_object_has_member(obj, "vdev_count"));
    json_object_unref(obj);
}

static void test_dpdk_hugepage_structure(void) {
    JsonObject *obj = pcv_dpdk_hugepage_info();
    g_assert_nonnull(obj);
    g_assert_true(json_object_has_member(obj, "total_mb"));
    g_assert_true(json_object_has_member(obj, "free_mb"));
    g_assert_true(json_object_has_member(obj, "hugepage_1g_total"));
    g_assert_true(json_object_has_member(obj, "hugepage_2m_total"));
    json_object_unref(obj);
}

static void test_dpdk_list_empty(void) {
    JsonArray *arr = pcv_dpdk_list();
    g_assert_nonnull(arr);

    json_array_unref(arr);
}

static void test_dpdk_unbind_idempotent(void) {

    g_assert_true(pcv_dpdk_unbind("0000:ff:1f.7", NULL));
}

static void test_dpdk_bridge_delete_idempotent(void) {
    g_assert_true(pcv_dpdk_bridge_delete("nonexist-dpdk-br", NULL));
}

static void test_pci_addr_valid(void) {
    g_assert_true(pcv_validate_pci_addr("0000:01:00.0"));
    g_assert_true(pcv_validate_pci_addr("0000:3b:10.1"));
    g_assert_true(pcv_validate_pci_addr("ffff:ff:1f.7"));
}

static void test_pci_addr_invalid(void) {
    g_assert_false(pcv_validate_pci_addr(NULL));
    g_assert_false(pcv_validate_pci_addr(""));
    g_assert_false(pcv_validate_pci_addr("../../etc"));
    g_assert_false(pcv_validate_pci_addr("01:00.0"));
    g_assert_false(pcv_validate_pci_addr("0000:01:00"));
    g_assert_false(pcv_validate_pci_addr("0000:01:20.0"));
    g_assert_false(pcv_validate_pci_addr("0000:01:00.8"));
    g_assert_false(pcv_validate_pci_addr("0000:01:00.0 ; rm -rf /"));
}

static void test_dpdk_bridge_create_reject_injection(void) {
    GError *err = NULL;

    gboolean ok = pcv_dpdk_bridge_create("br0", "x; touch /tmp/pcv_dpdk_pwn", &err);
    g_assert_false(ok);
    g_assert_nonnull(err);
    g_clear_error(&err);

    g_assert_false(g_file_test("/tmp/pcv_dpdk_pwn", G_FILE_TEST_EXISTS));
}

static void test_dpdk_nic_route_default(void) {
    gchar *base = g_dir_make_tmp("pcvdpdk_XXXXXX", NULL);
    g_assert_nonnull(base);
    gchar *pd = g_build_filename(base, "proc", "net", NULL);
    g_assert_cmpint(g_mkdir_with_parents(pd, 0700), ==, 0);
    gchar *route = g_build_filename(pd, "route", NULL);

    g_assert_true(g_file_set_contents(route,
        "Iface\tDestination\tGateway\tFlags\n"
        "eth0\t00000000\t0102A8C0\t0003\n"
        "eth1\t0000A8C0\t00000000\t0001\n", -1, NULL));
    g_assert_true (pcv_dpdk_route_is_default_dev("eth0", base));
    g_assert_false(pcv_dpdk_route_is_default_dev("eth1", base));
    g_assert_false(pcv_dpdk_route_is_default_dev("ethX", base));
    g_assert_false(pcv_dpdk_route_is_default_dev(NULL,   base));
    g_unlink(route); g_rmdir(pd);
    gchar *pdir = g_build_filename(base, "proc", NULL);
    g_rmdir(pdir); g_rmdir(base);
    g_free(route); g_free(pd); g_free(pdir); g_free(base);
}

static void test_dpdk_nic_null_failsecure(void) {
    gchar *reason = NULL;
    g_assert_true(pcv_dpdk_nic_is_protected(NULL, &reason));
    g_free(reason); reason = NULL;
    g_assert_true(pcv_dpdk_nic_is_protected("", &reason));
    g_free(reason);
}

static void test_dpdk_nic_absent_netdir_passes(void) {
    gchar *reason = NULL;

    g_assert_false(pcv_dpdk_nic_is_protected("ffff:ff:1f.7", &reason));
    g_assert_null(reason);
    g_free(reason);
}

static void test_dpdk_nic_malformed_failsecure(void) {
    gchar *reason = NULL;
    g_assert_true(pcv_dpdk_nic_is_protected("../../../etc", &reason));
    g_assert_nonnull(reason);
    g_free(reason); reason = NULL;
    g_assert_true(pcv_dpdk_nic_is_protected("not-a-bdf", &reason));
    g_free(reason);
}

void test_dpdk_register(void) {
    g_test_add_func("/dpdk/status/structure",          test_dpdk_status_structure);
    g_test_add_func("/dpdk/hugepage/structure",        test_dpdk_hugepage_structure);
    g_test_add_func("/dpdk/list/empty",                test_dpdk_list_empty);
    g_test_add_func("/dpdk/unbind/idempotent",         test_dpdk_unbind_idempotent);
    g_test_add_func("/dpdk/bridge_delete/idempotent",  test_dpdk_bridge_delete_idempotent);
    g_test_add_func("/dpdk/pci_addr/valid",            test_pci_addr_valid);
    g_test_add_func("/dpdk/pci_addr/invalid",          test_pci_addr_invalid);
    g_test_add_func("/dpdk/bridge_create/reject_injection",
                    test_dpdk_bridge_create_reject_injection);
    g_test_add_func("/dpdk/nic_protected/route_default",  test_dpdk_nic_route_default);
    g_test_add_func("/dpdk/nic_protected/null_failsecure", test_dpdk_nic_null_failsecure);
    g_test_add_func("/dpdk/nic_protected/absent_netdir",  test_dpdk_nic_absent_netdir_passes);
    g_test_add_func("/dpdk/nic_protected/malformed",      test_dpdk_nic_malformed_failsecure);
}
