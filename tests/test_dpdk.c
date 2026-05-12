













#include <glib.h>
#include <json-glib/json-glib.h>


extern gboolean pcv_dpdk_is_available(void);
extern JsonObject *pcv_dpdk_status(void);
extern JsonObject *pcv_dpdk_hugepage_info(void);
extern JsonArray *pcv_dpdk_list(void);
extern gboolean pcv_dpdk_unbind(const gchar *pci_addr, GError **error);
extern gboolean pcv_dpdk_bridge_delete(const gchar *name, GError **error);


extern gboolean pcv_validate_pci_addr(const gchar *addr);



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



void test_dpdk_register(void) {
    g_test_add_func("/dpdk/status/structure",          test_dpdk_status_structure);
    g_test_add_func("/dpdk/hugepage/structure",        test_dpdk_hugepage_structure);
    g_test_add_func("/dpdk/list/empty",                test_dpdk_list_empty);
    g_test_add_func("/dpdk/unbind/idempotent",         test_dpdk_unbind_idempotent);
    g_test_add_func("/dpdk/bridge_delete/idempotent",  test_dpdk_bridge_delete_idempotent);
    g_test_add_func("/dpdk/pci_addr/valid",            test_pci_addr_valid);
    g_test_add_func("/dpdk/pci_addr/invalid",          test_pci_addr_invalid);
}
