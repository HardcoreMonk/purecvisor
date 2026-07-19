
#include <glib.h>
#include <sys/prctl.h>
#include "../src/utils/pcv_privdrop.h"

static void test_prctl_constants(void) {
    g_assert_cmpint(PR_SET_NO_NEW_PRIVS, ==, 38);
    g_assert_cmpint(PR_GET_NO_NEW_PRIVS, ==, 39);
}

static void test_prctl_get_nnp(void) {

    int nnp = prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0);

    g_assert_true(nnp == 0 || nnp == 1);
}

static void test_seccomp_mode_readable(void) {

    int mode = prctl(PR_GET_SECCOMP, 0, 0, 0, 0);
    g_assert_true(mode >= 0 && mode <= 2);
}

static void test_apply_all_subprocess(void) {
    if (g_test_subprocess()) {
        pcv_privdrop_apply_all();
        return;
    }
    g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
    g_test_trap_assert_passed();
}

static void test_no_new_privs_subprocess(void) {
    if (g_test_subprocess()) {
        gboolean ok = pcv_privdrop_no_new_privs();

        (void)ok;
        return;
    }
    g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
    g_test_trap_assert_passed();
}

static void test_capabilities_subprocess(void) {
    if (g_test_subprocess()) {
        gboolean ok = pcv_privdrop_capabilities();
        (void)ok;
        return;
    }
    g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
    g_test_trap_assert_passed();
}

static void test_seccomp_subprocess(void) {
    if (g_test_subprocess()) {
        gboolean ok = pcv_privdrop_seccomp();
        (void)ok;
        return;
    }
    g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
    g_test_trap_assert_passed();
}

void test_privdrop_register(void) {
    g_test_add_func("/privdrop/prctl_constants",     test_prctl_constants);
    g_test_add_func("/privdrop/prctl_get_nnp",       test_prctl_get_nnp);
    g_test_add_func("/privdrop/seccomp_mode_readable", test_seccomp_mode_readable);
    g_test_add_func("/privdrop/apply_all_subprocess",   test_apply_all_subprocess);
    g_test_add_func("/privdrop/no_new_privs_subprocess", test_no_new_privs_subprocess);
    g_test_add_func("/privdrop/capabilities_subprocess", test_capabilities_subprocess);
    g_test_add_func("/privdrop/seccomp_subprocess",      test_seccomp_subprocess);
}
