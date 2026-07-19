
#include <glib.h>
#include "../src/utils/pcv_spawn.h"
#include "../src/api/snapshot_verify_probe.h"

static void ensure_spawn(void) {
    static gboolean initialized = FALSE;
    if (!initialized) { pcv_spawn_launcher_init(); initialized = TRUE; }
}

static void test_probe_nonexistent_is_false(void) {
    ensure_spawn();
    gboolean exists =
        pcv_snapshot_verify_probe("nonexistent-pcv-test-pool-XYZ/vm@snap-does-not-exist");
    g_assert_false(exists);
}

static void test_probe_null_safe(void) {
    ensure_spawn();
    g_assert_false(pcv_snapshot_verify_probe(NULL));
    g_assert_false(pcv_snapshot_verify_probe(""));
}

void test_handler_snapshot_verify_register(void) {
    g_test_add_func("/handler_snapshot_verify/probe_nonexistent_is_false",
                    test_probe_nonexistent_is_false);
    g_test_add_func("/handler_snapshot_verify/probe_null_safe",
                    test_probe_null_safe);
}
