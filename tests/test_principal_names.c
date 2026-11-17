                                                                                                            
                                                                                     
                                                                                             
                                                                      
                                                                                   
                                                 
#include <glib.h>
#include <gio/gio.h>
#include <sqlite3.h>
#include <string.h>

#include "modules/auth/pcv_principal_names.h"

                                                   
static const gchar *const k_norm_inputs[] = {
    "alice", " alice", "alice ", "\talice\n", "\f\ralice\r\f", " \t\n\f\r alice \r\f\n\t ",
    "\valice\v", " \valice\v ", "a b", " a\tb ", "", "   ", "\t\n", "\v", " \v ", "-", " grpc", "grpc\n",
};

                                                                    
static void
test_reserved_table(void)
{
    const gchar *const reserved[] = { "grpc", "local-admin", "system", "ai-agent", "ai-ops", "-" };
    for (gsize i = 0; i < G_N_ELEMENTS(reserved); i++)
        g_assert_true(pcv_principal_name_is_reserved(reserved[i]));
    const gchar *const plain[] = { "GRPC", "Grpc", "System", "LOCAL-ADMIN", "AI-Agent", "admin", "grpc2", "systemd",
                                   "local", "ai", "--", "", " grpc", "system " };
    for (gsize i = 0; i < G_N_ELEMENTS(plain); i++)
        g_assert_false(pcv_principal_name_is_reserved(plain[i]));
    g_assert_false(pcv_principal_name_is_reserved(NULL));
}

                                                                               
static void
test_normalize_set(void)
{
    g_assert_null(pcv_principal_name_normalize(NULL));
    g_autofree gchar *a = pcv_principal_name_normalize(" \t\n\f\r alice \r\f\n\t ");
    g_assert_cmpstr(a, ==, "alice");
    g_autofree gchar *v = pcv_principal_name_normalize(" \valice\v ");
    g_assert_cmpstr(v, ==, "\valice\v");
    g_autofree gchar *in = pcv_principal_name_normalize(" a\tb ");
    g_assert_cmpstr(in, ==, "a\tb");
    g_autofree gchar *blank = pcv_principal_name_normalize("   ");
    g_assert_cmpstr(blank, ==, "");
    for (gsize i = 0; i < G_N_ELEMENTS(k_norm_inputs); i++) {
        g_autofree gchar *mine = pcv_principal_name_normalize(k_norm_inputs[i]);
        g_autofree gchar *glib = g_strstrip(g_strdup(k_norm_inputs[i]));
        g_assert_cmpstr(mine, ==, glib);
    }
}

                                                                                      
static void
test_normalize_sql_trim_equal(void)
{
    sqlite3 *db = NULL;
    g_assert_cmpint(sqlite3_open(":memory:", &db), ==, SQLITE_OK);
    sqlite3_stmt *st = NULL;
    g_assert_cmpint(sqlite3_prepare_v2(db, "SELECT trim(?1, " PCV_PRINCIPAL_TRIM_SQL ");", -1, &st, NULL), ==, SQLITE_OK);
    for (gsize i = 0; i < G_N_ELEMENTS(k_norm_inputs); i++) {
        sqlite3_reset(st);
        sqlite3_bind_text(st, 1, k_norm_inputs[i], -1, SQLITE_STATIC);
        g_assert_cmpint(sqlite3_step(st), ==, SQLITE_ROW);
        const gchar *sql = (const gchar *)sqlite3_column_text(st, 0);
        g_autofree gchar *mine = pcv_principal_name_normalize(k_norm_inputs[i]);
        g_assert_cmpstr(sql ? sql : "", ==, mine);
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
}

                                                                                                  
                                                                     
static void
test_signup_name_conflict(void)
{
    const gint codes[] = { PCV_PRINCIPAL_ERROR_RESERVED, PCV_PRINCIPAL_ERROR_KEY_NAME, PCV_PRINCIPAL_ERROR_USER_NAME,
                           PCV_PRINCIPAL_ERROR_DELETED, PCV_PRINCIPAL_ERROR_EXISTING_USER };
    for (gsize i = 0; i < G_N_ELEMENTS(codes); i++) {
        g_autoptr(GError) e = g_error_new_literal(PCV_PRINCIPAL_ERROR, codes[i], "x");
        g_assert_true(pcv_principal_signup_name_conflict(e));
    }
    g_autoptr(GError) exists = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_EXISTS, "x");
    g_assert_true(pcv_principal_signup_name_conflict(exists));
    g_autoptr(GError) failed = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, "x");
    g_assert_false(pcv_principal_signup_name_conflict(failed));
    g_autoptr(GError) inval = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "x");
    g_assert_false(pcv_principal_signup_name_conflict(inval));
    g_autoptr(GError) other = g_error_new_literal(G_FILE_ERROR, G_FILE_ERROR_EXIST, "x");
    g_assert_false(pcv_principal_signup_name_conflict(other));
    g_assert_false(pcv_principal_signup_name_conflict(NULL));
}

                                                                                                     
static void
test_tenant_lookup_applies(void)
{
    g_assert_true(pcv_principal_tenant_lookup_applies(-1));
    g_assert_false(pcv_principal_tenant_lookup_applies(0));
    g_assert_false(pcv_principal_tenant_lookup_applies(1));
    g_assert_false(pcv_principal_tenant_lookup_applies(2));
}

                                                                                                           
                                                                                    
static void
test_rpc_method_grammar(void)
{
    static const gchar *const ok[] = { "vm.list", "vm.create", "vm.start", "vm.stop", "vm.delete", "vm.resize_disk", "vm.clone",
                                       "network.list", "network.create", "network.delete", "storage.pool.list", "monitor.fleet",
                                       "audit.search", "daemon.version", "a", "A9._-z" };
    for (gsize i = 0; i < G_N_ELEMENTS(ok); i++)
        g_assert_true(pcv_rpc_method_name_is_valid(ok[i]));
    g_autofree gchar *n127 = g_strnfill(127, 'm');
    g_autofree gchar *n128 = g_strnfill(128, 'm');
    g_assert_true(pcv_rpc_method_name_is_valid(n127));
    g_assert_false(pcv_rpc_method_name_is_valid(n128));
    static const gchar *const bad[] = { "", ".x", "-x", "_x", "a b", "a\"b", "/purecvisor.v1.X/Y", "vm/list", "a\nb" };
    for (gsize i = 0; i < G_N_ELEMENTS(bad); i++)
        g_assert_false(pcv_rpc_method_name_is_valid(bad[i]));
    g_assert_false(pcv_rpc_method_name_is_valid(NULL));
}

                               
void
test_principal_names_register(void)
{
    g_test_add_func("/principal_names/reserved_table", test_reserved_table);
    g_test_add_func("/principal_names/normalize_set", test_normalize_set);
    g_test_add_func("/principal_names/normalize_sql_trim_equal", test_normalize_sql_trim_equal);
    g_test_add_func("/principal_names/signup_name_conflict", test_signup_name_conflict);
    g_test_add_func("/principal_names/tenant_lookup_applies", test_tenant_lookup_applies);
    g_test_add_func("/principal_names/rpc_method_grammar", test_rpc_method_grammar);
}
