#include <glib.h>

#include "bootstrap/pcv_bootstrap.h"

static void
test_bootstrap_async_registration_matches_edition(void)
{
    GHashTable *async_methods = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    pcv_bootstrap_register_async_methods(async_methods);

    g_assert_cmpuint(g_hash_table_size(async_methods), ==, 0);

    g_hash_table_destroy(async_methods);
}

static void
test_bootstrap_route_registration_matches_edition(void)
{
    GHashTable *rpc_routes = g_hash_table_new(g_str_hash, g_str_equal);

    pcv_bootstrap_register_rpc_routes(rpc_routes);

    g_assert_cmpuint(g_hash_table_size(rpc_routes), ==, 0);

    g_hash_table_destroy(rpc_routes);
}

void
test_bootstrap_rpc_registration_register(void)
{
    g_test_add_func("/bootstrap_rpc_registration/async_methods_match_edition",
                    test_bootstrap_async_registration_matches_edition);
    g_test_add_func("/bootstrap_rpc_registration/routes_match_edition",
                    test_bootstrap_route_registration_matches_edition);
}
