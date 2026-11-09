                                                                                   
                                                                    
                                                                    
#include <glib.h>
#include "../src/modules/dispatcher/snapshot_restart_policy.h"

static void test_rollback_restart_truth_table(void)
{
                                               
    g_assert_true(pcv_snapshot_rollback_should_restart(PCV_ROLLBACK_PATH_LIBVIRT, TRUE, TRUE));
    g_assert_false(pcv_snapshot_rollback_should_restart(PCV_ROLLBACK_PATH_LIBVIRT, TRUE, FALSE));
    g_assert_false(pcv_snapshot_rollback_should_restart(PCV_ROLLBACK_PATH_LIBVIRT, FALSE, TRUE));
    g_assert_false(pcv_snapshot_rollback_should_restart(PCV_ROLLBACK_PATH_LIBVIRT, FALSE, FALSE));
                                                          
    g_assert_true(pcv_snapshot_rollback_should_restart(PCV_ROLLBACK_PATH_ZFS, TRUE, TRUE));
    g_assert_true(pcv_snapshot_rollback_should_restart(PCV_ROLLBACK_PATH_ZFS, TRUE, FALSE));
    g_assert_false(pcv_snapshot_rollback_should_restart(PCV_ROLLBACK_PATH_ZFS, FALSE, TRUE));
    g_assert_false(pcv_snapshot_rollback_should_restart(PCV_ROLLBACK_PATH_ZFS, FALSE, FALSE));
}

void test_snapshot_restart_policy_register(void)
{
    g_test_add_func("/snapshot_restart_policy/rollback_restart_truth_table",
                    test_rollback_restart_truth_table);
}
