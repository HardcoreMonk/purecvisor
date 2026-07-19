
#include <glib.h>
#include <libvirt/libvirt.h>
#include "../src/modules/dispatcher/hotplug_affect_policy.h"

static void test_active_default_live_config(void) {
    g_assert_cmpuint(pcv_hotplug_compute_affect_flags(TRUE, FALSE), ==,
                     (unsigned int)(VIR_DOMAIN_AFFECT_LIVE | VIR_DOMAIN_AFFECT_CONFIG));
}

static void test_inactive_default_config(void) {
    g_assert_cmpuint(pcv_hotplug_compute_affect_flags(FALSE, FALSE), ==,
                     (unsigned int)VIR_DOMAIN_AFFECT_CONFIG);
}

static void test_config_only_active_no_live(void) {
    unsigned int f = pcv_hotplug_compute_affect_flags(TRUE, TRUE);
    g_assert_cmpuint(f, ==, (unsigned int)VIR_DOMAIN_AFFECT_CONFIG);
    g_assert_true((f & VIR_DOMAIN_AFFECT_LIVE) == 0);
}

static void test_config_only_inactive(void) {
    g_assert_cmpuint(pcv_hotplug_compute_affect_flags(FALSE, TRUE), ==,
                     (unsigned int)VIR_DOMAIN_AFFECT_CONFIG);
}

void test_hotplug_flags_register(void) {
    g_test_add_func("/hotplug_flags/active_default_live_config",
                    test_active_default_live_config);
    g_test_add_func("/hotplug_flags/inactive_default_config",
                    test_inactive_default_config);
    g_test_add_func("/hotplug_flags/config_only_active_no_live",
                    test_config_only_active_no_live);
    g_test_add_func("/hotplug_flags/config_only_inactive",
                    test_config_only_inactive);
}
