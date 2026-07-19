
#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <unistd.h>

extern JsonObject *pcv_sriov_status(void);
extern JsonArray *pcv_sriov_list(const gchar *pf);
extern gboolean pcv_sriov_disable(const gchar *pf, GError **error);
extern gchar *pcv_sriov_vf_pci_addr(const gchar *pf, gint vf_index);
extern gboolean pcv_sriov_set(const gchar *pf, gint vf_index,
                              const gchar *mac, gint vlan,
                              gint spoofchk, GError **error);
extern gboolean pcv_sriov_detach_vm(const gchar *vm_name,
                                    const gchar *pci_addr, GError **error);

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

static void test_sriov_disable_write_failure_propagates(void) {
    if (!g_file_test("/dev/full", G_FILE_TEST_EXISTS)) { g_test_skip("no /dev/full"); return; }
    gchar *root = g_dir_make_tmp("pcv-sriov-XXXXXX", NULL); g_assert_nonnull(root);
    gchar *devdir = g_build_filename(root, "testpf0", "device", NULL);
    g_assert_cmpint(g_mkdir_with_parents(devdir, 0700), ==, 0);
    gchar *numvfs = g_build_filename(devdir, "sriov_numvfs", NULL);
    g_assert_cmpint(symlink("/dev/full", numvfs), ==, 0);
    g_setenv("PCV_SRIOV_SYSFS_ROOT", root, TRUE);
    GError *err = NULL;
    gboolean ok = pcv_sriov_disable("testpf0", &err);
    g_unsetenv("PCV_SRIOV_SYSFS_ROOT");
    g_assert_false(ok);
    g_assert_nonnull(err); g_clear_error(&err);
    g_unlink(numvfs); g_rmdir(devdir);
    gchar *pfdir = g_build_filename(root, "testpf0", NULL); g_rmdir(pfdir); g_rmdir(root);
    g_free(numvfs); g_free(devdir); g_free(pfdir); g_free(root);
}

static void test_sriov_vf_pci_null(void) {
    gchar *pci = pcv_sriov_vf_pci_addr("nonexist99", 0);
    g_assert_null(pci);
}

static void test_sriov_disable_reject_injection(void) {
    GError *err = NULL;

    gboolean ok = pcv_sriov_disable("x; touch /tmp/pwn", &err);
    g_assert_false(ok);
    g_assert_nonnull(err);
    g_clear_error(&err);
}

static void test_sriov_set_reject_bad_mac(void) {
    GError *err = NULL;

    gboolean ok = pcv_sriov_set("eth0", 0, "52:54:00 vlan 4095", -1, -1, &err);
    g_assert_false(ok);
    g_assert_nonnull(err);
    g_clear_error(&err);
}

static void test_sriov_detach_reject_bad_vm(void) {
    GError *err = NULL;

    gboolean ok = pcv_sriov_detach_vm("vm; reboot", "0000:01:00.0", &err);
    g_assert_false(ok);
    g_assert_nonnull(err);
    g_clear_error(&err);
}

void test_sriov_register(void) {
    g_test_add_func("/sriov/status/structure",       test_sriov_status_structure);
    g_test_add_func("/sriov/list/empty",             test_sriov_list_empty);
    g_test_add_func("/sriov/list/nonexist_pf",       test_sriov_list_nonexist_pf);
    g_test_add_func("/sriov/disable/idempotent",     test_sriov_disable_idempotent);
    g_test_add_func("/sriov/disable/write_failure_propagates", test_sriov_disable_write_failure_propagates);
    g_test_add_func("/sriov/vf_pci/null",            test_sriov_vf_pci_null);
    g_test_add_func("/sriov/disable/reject_injection", test_sriov_disable_reject_injection);
    g_test_add_func("/sriov/set/reject_bad_mac",       test_sriov_set_reject_bad_mac);
    g_test_add_func("/sriov/detach/reject_bad_vm",     test_sriov_detach_reject_bad_vm);
}
