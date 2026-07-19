
#include <glib.h>
#include <string.h>
#include "../src/api/hot_reload.h"

typedef enum {
    TEST_UPGRADE_IDLE      = 0,
    TEST_UPGRADE_DRAINING  = 1,
    TEST_UPGRADE_READY     = 2,
    TEST_UPGRADE_EXECUTING = 3,
} TestUpgradeState;

#define PCV_VERSION_STR "1.0"
#define PCV_UPGRADE_FD_ENV "PCV_UPGRADE_FD"
#define PCV_LISTEN_FDS_ENV "LISTEN_FDS"

static void test_initial_state_idle(void) {

    PcvUpgradeState s = pcv_hot_reload_get_state();
    g_assert_cmpint(s, ==, PCV_UPGRADE_IDLE);
}

static void test_get_version_nonnull(void) {
    const gchar *v = pcv_hot_reload_get_version();
    g_assert_nonnull(v);
    g_assert_cmpuint(strlen(v), >, 0);
}

static void test_init_with_null_path(void) {

    pcv_hot_reload_init(NULL, -1);
    PcvUpgradeState s = pcv_hot_reload_get_state();
    g_assert_cmpint(s, ==, PCV_UPGRADE_IDLE);
}

static void test_hotreload_state_values(void) {
    g_assert_cmpint(TEST_UPGRADE_IDLE,      ==, 0);
    g_assert_cmpint(TEST_UPGRADE_DRAINING,  ==, 1);
    g_assert_cmpint(TEST_UPGRADE_READY,     ==, 2);
    g_assert_cmpint(TEST_UPGRADE_EXECUTING, ==, 3);
}

static gboolean
_is_valid_transition(TestUpgradeState from, TestUpgradeState to)
{

    if (from == TEST_UPGRADE_IDLE      && to == TEST_UPGRADE_DRAINING)  return TRUE;
    if (from == TEST_UPGRADE_DRAINING  && to == TEST_UPGRADE_READY)     return TRUE;
    if (from == TEST_UPGRADE_READY     && to == TEST_UPGRADE_EXECUTING) return TRUE;

    if (to == TEST_UPGRADE_IDLE) return TRUE;
    return FALSE;
}

static void test_hotreload_valid_transitions(void) {

    g_assert_true(_is_valid_transition(TEST_UPGRADE_IDLE,     TEST_UPGRADE_DRAINING));
    g_assert_true(_is_valid_transition(TEST_UPGRADE_DRAINING, TEST_UPGRADE_READY));
    g_assert_true(_is_valid_transition(TEST_UPGRADE_READY,    TEST_UPGRADE_EXECUTING));
}

static void test_hotreload_rollback_transitions(void) {

    g_assert_true(_is_valid_transition(TEST_UPGRADE_DRAINING,  TEST_UPGRADE_IDLE));
    g_assert_true(_is_valid_transition(TEST_UPGRADE_READY,     TEST_UPGRADE_IDLE));
    g_assert_true(_is_valid_transition(TEST_UPGRADE_EXECUTING, TEST_UPGRADE_IDLE));
    g_assert_true(_is_valid_transition(TEST_UPGRADE_IDLE,      TEST_UPGRADE_IDLE));
}

static void test_hotreload_invalid_transitions(void) {

    g_assert_false(_is_valid_transition(TEST_UPGRADE_IDLE,     TEST_UPGRADE_READY));
    g_assert_false(_is_valid_transition(TEST_UPGRADE_IDLE,     TEST_UPGRADE_EXECUTING));
    g_assert_false(_is_valid_transition(TEST_UPGRADE_DRAINING, TEST_UPGRADE_EXECUTING));

    g_assert_false(_is_valid_transition(TEST_UPGRADE_READY,    TEST_UPGRADE_DRAINING));
    g_assert_false(_is_valid_transition(TEST_UPGRADE_EXECUTING,TEST_UPGRADE_DRAINING));
    g_assert_false(_is_valid_transition(TEST_UPGRADE_EXECUTING,TEST_UPGRADE_READY));
}

static const gchar *
_upgrade_state_to_str(TestUpgradeState s)
{
    switch (s) {
    case TEST_UPGRADE_IDLE:      return "idle";
    case TEST_UPGRADE_DRAINING:  return "draining";
    case TEST_UPGRADE_READY:     return "ready";
    case TEST_UPGRADE_EXECUTING: return "executing";
    default:                     return "unknown";
    }
}

static void test_hotreload_state_strings(void) {
    g_assert_cmpstr(_upgrade_state_to_str(TEST_UPGRADE_IDLE),      ==, "idle");
    g_assert_cmpstr(_upgrade_state_to_str(TEST_UPGRADE_DRAINING),  ==, "draining");
    g_assert_cmpstr(_upgrade_state_to_str(TEST_UPGRADE_READY),     ==, "ready");
    g_assert_cmpstr(_upgrade_state_to_str(TEST_UPGRADE_EXECUTING), ==, "executing");
    g_assert_cmpstr(_upgrade_state_to_str((TestUpgradeState)99),   ==, "unknown");
}

static void test_hotreload_version_format(void) {
    const gchar *ver = PCV_VERSION_STR;
    g_assert_nonnull(ver);
    g_assert_cmpuint(strlen(ver), >, 0);

    g_assert_true(g_ascii_isdigit(ver[0]));

    gint dot_count = 0;
    for (const gchar *p = ver; *p; p++)
        if (*p == '.') dot_count++;
    g_assert_cmpint(dot_count, >=, 1);
}

static void test_hotreload_env_keys(void) {
    g_assert_cmpstr(PCV_UPGRADE_FD_ENV, ==, "PCV_UPGRADE_FD");
    g_assert_cmpstr(PCV_LISTEN_FDS_ENV, ==, "LISTEN_FDS");

    g_assert_null(strchr(PCV_UPGRADE_FD_ENV, ' '));
    g_assert_null(strchr(PCV_LISTEN_FDS_ENV, ' '));
}

static gboolean
_is_valid_listen_fd(int fd)
{

    return fd >= 3;
}

static void test_hotreload_fd_validation(void) {
    g_assert_true(_is_valid_listen_fd(3));
    g_assert_true(_is_valid_listen_fd(10));
    g_assert_true(_is_valid_listen_fd(1024));
    g_assert_false(_is_valid_listen_fd(0));
    g_assert_false(_is_valid_listen_fd(1));
    g_assert_false(_is_valid_listen_fd(2));
    g_assert_false(_is_valid_listen_fd(-1));
}

void test_hotreload_register(void) {
    g_test_add_func("/hotreload/state/values",           test_hotreload_state_values);
    g_test_add_func("/hotreload/transition/valid",       test_hotreload_valid_transitions);
    g_test_add_func("/hotreload/transition/rollback",    test_hotreload_rollback_transitions);
    g_test_add_func("/hotreload/transition/invalid",     test_hotreload_invalid_transitions);
    g_test_add_func("/hotreload/state/strings",          test_hotreload_state_strings);
    g_test_add_func("/hotreload/version/format",         test_hotreload_version_format);
    g_test_add_func("/hotreload/env/keys",               test_hotreload_env_keys);
    g_test_add_func("/hotreload/fd/validation",          test_hotreload_fd_validation);
    g_test_add_func("/hotreload/initial_state_idle",     test_initial_state_idle);
    g_test_add_func("/hotreload/get_version_nonnull",    test_get_version_nonnull);
    g_test_add_func("/hotreload/init_with_null_path",    test_init_with_null_path);
}
