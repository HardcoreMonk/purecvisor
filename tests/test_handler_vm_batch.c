
#include <glib.h>
#include "../src/api/vm_batch_policy.h"

typedef enum { BATCH_ACCEPTED, BATCH_REJECTED } BatchVmDecision;
static BatchVmDecision batch_vm_decision(gboolean vm_exists) {
    return vm_exists ? BATCH_ACCEPTED : BATCH_REJECTED;
}

static void test_nonwhitelist_action_rejected(void) {

    g_assert_false(pcv_vm_batch_action_is_whitelisted("reboot"));
    g_assert_false(pcv_vm_batch_action_is_whitelisted("pause"));
    g_assert_false(pcv_vm_batch_action_is_whitelisted("resume"));
    g_assert_false(pcv_vm_batch_action_is_whitelisted("delete"));
    g_assert_false(pcv_vm_batch_action_is_whitelisted("bogus"));
    g_assert_false(pcv_vm_batch_action_is_whitelisted(NULL));
}

static void test_whitelist_action_allowed(void) {
    g_assert_true(pcv_vm_batch_action_is_whitelisted("start"));
    g_assert_true(pcv_vm_batch_action_is_whitelisted("stop"));
}

static void test_missing_vm_rejected(void) {
    g_assert_cmpint(batch_vm_decision(FALSE), ==, BATCH_REJECTED);
    g_assert_cmpint(batch_vm_decision(TRUE),  ==, BATCH_ACCEPTED);
}

void test_handler_vm_batch_register(void) {
    g_test_add_func("/handler_vm_batch/nonwhitelist_action_rejected",
                    test_nonwhitelist_action_rejected);
    g_test_add_func("/handler_vm_batch/whitelist_action_allowed",
                    test_whitelist_action_allowed);
    g_test_add_func("/handler_vm_batch/missing_vm_rejected",
                    test_missing_vm_rejected);
}
