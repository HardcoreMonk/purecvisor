













#include <glib.h>
#include <json-glib/json-glib.h>


extern JsonObject *pcv_sriov_status(void);
extern JsonArray *pcv_sriov_list(const gchar *pf);
extern gboolean pcv_sriov_disable(const gchar *pf, GError **error);
extern gchar *pcv_sriov_vf_pci_addr(const gchar *pf, gint vf_index);



static void test_sriov_status_structure(void) {
    JsonObject *obj = pcv_sriov_status();
    g_assert_nonnull(obj);
    g_assert_true(json_object_has_member(obj, "available"));
    g_assert_true(json_object_has_member(obj, "physical_functions"));
    JsonArray *pfs = json_object_get_array_member(obj, "physical_functions");
    g_assert_nonnull(pfs);
    json_object_unref(obj);
}

static void test_sriov_list_empty(void) {
    JsonArray *arr = pcv_sriov_list(NULL);
    g_assert_nonnull(arr);

    json_array_unref(arr);
}

static void test_sriov_list_nonexist_pf(void) {
    JsonArray *arr = pcv_sriov_list("nonexist99");
    g_assert_nonnull(arr);
    g_assert_cmpuint(json_array_get_length(arr), ==, 0);
    json_array_unref(arr);
}



static void test_sriov_disable_idempotent(void) {

    gboolean ok = pcv_sriov_disable("nonexist99", NULL);

    g_assert_true(ok);
}



static void test_sriov_vf_pci_null(void) {
    gchar *pci = pcv_sriov_vf_pci_addr("nonexist99", 0);
    g_assert_null(pci);
}



void test_sriov_register(void) {
    g_test_add_func("/sriov/status/structure",       test_sriov_status_structure);
    g_test_add_func("/sriov/list/empty",             test_sriov_list_empty);
    g_test_add_func("/sriov/list/nonexist_pf",       test_sriov_list_nonexist_pf);
    g_test_add_func("/sriov/disable/idempotent",     test_sriov_disable_idempotent);
    g_test_add_func("/sriov/vf_pci/null",            test_sriov_vf_pci_null);
}
