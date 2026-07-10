#include <glib.h>
#include "bootstrap/pcv_bootstrap.h"

/*
 * Bootstrap tests pin the public Single Edge identity. These assertions catch
 * accidental reintroduction of multi-edition service names or cluster capability.
 */
static void
test_bootstrap_reports_current_edition(void)
{
    const PcvBootstrapEditionInfo *info = pcv_bootstrap_get_edition_info();

    g_assert_nonnull(info);
    g_assert_nonnull(info->edition_name);
    g_assert_cmpstr(info->edition_name, ==, "single");
}

static void
test_bootstrap_cluster_flag_matches_build(void)
{
    const PcvBootstrapEditionInfo *info = pcv_bootstrap_get_edition_info();

    g_assert_false(info->cluster_enabled);
}

static void
test_bootstrap_info_contract_is_self_consistent(void)
{
    const PcvBootstrapEditionInfo *info = pcv_bootstrap_get_edition_info();

    g_assert_nonnull(info);
    g_assert_nonnull(info->edition_name);
    g_assert_cmpstr(info->edition_name, ==, "single");
    g_assert_false(info->cluster_enabled);
}

static void
test_bootstrap_daemon_binary_path_matches_build(void)
{
    const gchar *daemon_binary = pcv_bootstrap_get_daemon_binary_path();

    g_assert_nonnull(daemon_binary);
    g_assert_cmpstr(daemon_binary, ==, "/usr/local/bin/purecvisorsd");
}

static void
test_bootstrap_runtime_network_hook_is_exposed(void)
{
    g_assert_nonnull(pcv_bootstrap_init_runtime_network);
}

void
test_bootstrap_register(void)
{
    g_test_add_func("/bootstrap/reports_current_edition",
                    test_bootstrap_reports_current_edition);
    g_test_add_func("/bootstrap/cluster_flag_matches_build",
                    test_bootstrap_cluster_flag_matches_build);
    g_test_add_func("/bootstrap/info_contract_is_self_consistent",
                    test_bootstrap_info_contract_is_self_consistent);
    g_test_add_func("/bootstrap/daemon_binary_path_matches_build",
                    test_bootstrap_daemon_binary_path_matches_build);
    g_test_add_func("/bootstrap/runtime_network_hook_is_exposed",
                    test_bootstrap_runtime_network_hook_is_exposed);
}
