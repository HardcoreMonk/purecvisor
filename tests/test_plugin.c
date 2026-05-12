

























#include <glib.h>
#include <string.h>
#include "modules/plugin/pcv_plugin_api.h"


















static gboolean
plugin_path_is_valid(const gchar *path)
{
    if (!path || path[0] == '\0')
        return FALSE;
    if (strlen(path) > 256)
        return FALSE;

    if (strstr(path, "/../") != NULL)
        return FALSE;
    if (g_str_has_suffix(path, "/.."))
        return FALSE;
    if (g_str_has_prefix(path, "../"))
        return FALSE;
    if (strcmp(path, "..") == 0)
        return FALSE;
    return TRUE;
}








static gboolean
symbol_name_is_valid(const gchar *sym)
{
    if (!sym || sym[0] == '\0')
        return FALSE;
    gsize len = strlen(sym);
    if (len > 128)
        return FALSE;

    if (!g_ascii_isalpha(sym[0]) && sym[0] != '_')
        return FALSE;

    for (gsize i = 1; i < len; i++) {
        if (!g_ascii_isalnum(sym[i]) && sym[i] != '_')
            return FALSE;
    }
    return TRUE;
}










static gboolean
abi_version_compatible(guint plugin_abi, guint host_abi)
{
    if (plugin_abi == 0)
        return FALSE;
    return plugin_abi == host_abi;
}




typedef struct {
    gchar   name[64];
    gchar   version[32];
    void   *module;
    void  (*shutdown_fn)(void);
} TestLoadedPlugin;


static void
test_plugin_clear(TestLoadedPlugin *p)
{
    memset(p, 0, sizeof(*p));
}



#define TEST_MAX_METHODS 8

typedef struct {
    gchar method[128];
} TestMethodEntry;

typedef struct {
    TestMethodEntry entries[TEST_MAX_METHODS];
    gint            count;
} TestRegistry;


static gboolean
test_registry_add(TestRegistry *reg, const gchar *method)
{
    if (!reg || !method || method[0] == '\0')
        return FALSE;
    if (reg->count >= TEST_MAX_METHODS)
        return FALSE;

    for (gint i = 0; i < reg->count; i++) {
        if (strcmp(reg->entries[i].method, method) == 0)
            return FALSE;
    }
    g_strlcpy(reg->entries[reg->count].method, method,
              sizeof(reg->entries[0].method));
    reg->count++;
    return TRUE;
}





static void test_plugin_path_valid(void)
{

    g_assert_true(plugin_path_is_valid("/usr/lib/purecvisor/plugins"));
    g_assert_true(plugin_path_is_valid("/etc/purecvisor/plugins.d"));
    g_assert_true(plugin_path_is_valid("/opt/pcv/ext"));

    g_assert_true(plugin_path_is_valid("/usr/lib/purecvisor/plugins/"));

    g_assert_true(plugin_path_is_valid("/p"));
}

static void test_plugin_path_invalid(void)
{

    g_assert_false(plugin_path_is_valid(NULL));

    g_assert_false(plugin_path_is_valid(""));

    g_assert_false(plugin_path_is_valid("/usr/lib/../etc/passwd"));

    g_assert_false(plugin_path_is_valid("/usr/lib/.."));

    g_assert_false(plugin_path_is_valid("../etc/shadow"));

    g_assert_false(plugin_path_is_valid(".."));

    gchar *over = g_strnfill(257, 'x');
    over[0] = '/';
    g_assert_false(plugin_path_is_valid(over));
    g_free(over);
}

static void test_plugin_path_boundary_256(void)
{

    gchar *at256 = g_strnfill(256, 'a');
    at256[0] = '/';
    g_assert_true(plugin_path_is_valid(at256));
    g_free(at256);


    gchar *at257 = g_strnfill(257, 'a');
    at257[0] = '/';
    g_assert_false(plugin_path_is_valid(at257));
    g_free(at257);
}





static void test_plugin_name_extract_normal(void)
{
    gchar *name = g_path_get_basename("/usr/lib/purecvisor/plugins/libfoo.so");
    g_assert_cmpstr(name, ==, "libfoo.so");
    g_free(name);
}

static void test_plugin_name_extract_edge_cases(void)
{

    gchar *n1 = g_path_get_basename("libbar.so");
    g_assert_cmpstr(n1, ==, "libbar.so");
    g_free(n1);


    gchar *n2 = g_path_get_basename("/usr/lib/plugins/");
    g_assert_cmpstr(n2, ==, "plugins");
    g_free(n2);


    gchar *n3 = g_path_get_basename(".");
    g_assert_cmpstr(n3, ==, ".");
    g_free(n3);


    gchar *n4 = g_path_get_basename("/a/b/c/libplugin_v2.so");
    g_assert_cmpstr(n4, ==, "libplugin_v2.so");
    g_free(n4);
}





static void test_symbol_name_valid(void)
{

    g_assert_true(symbol_name_is_valid("pcv_plugin_get_meta"));
    g_assert_true(symbol_name_is_valid("pcv_plugin_register"));
    g_assert_true(symbol_name_is_valid("pcv_plugin_shutdown"));

    g_assert_true(symbol_name_is_valid("_init"));

    g_assert_true(symbol_name_is_valid("f"));

    g_assert_true(symbol_name_is_valid("handler_v2"));

    gchar *max128 = g_strnfill(128, 'a');
    g_assert_true(symbol_name_is_valid(max128));
    g_free(max128);
}

static void test_symbol_name_invalid(void)
{

    g_assert_false(symbol_name_is_valid(NULL));

    g_assert_false(symbol_name_is_valid(""));

    g_assert_false(symbol_name_is_valid("1init"));
    g_assert_false(symbol_name_is_valid("0handler"));

    g_assert_false(symbol_name_is_valid("my-plugin"));

    g_assert_false(symbol_name_is_valid("my.func"));

    g_assert_false(symbol_name_is_valid("get meta"));

    gchar *over128 = g_strnfill(129, 'a');
    g_assert_false(symbol_name_is_valid(over128));
    g_free(over128);
}





static void test_abi_version_compatible(void)
{

    g_assert_true(abi_version_compatible(1, 1));

    g_assert_true(abi_version_compatible(2, 2));
    g_assert_true(abi_version_compatible(42, 42));
}

static void test_abi_version_incompatible(void)
{

    g_assert_false(abi_version_compatible(2, 1));

    g_assert_false(abi_version_compatible(1, 2));

    g_assert_false(abi_version_compatible(100, 1));
}

static void test_abi_version_zero_rejected(void)
{

    g_assert_false(abi_version_compatible(0, 1));
    g_assert_false(abi_version_compatible(0, 0));
    g_assert_false(abi_version_compatible(0, 2));
}

static void test_abi_version_constant(void)
{

    g_assert_cmpuint(PCV_PLUGIN_ABI_VERSION, ==, 1u);

    g_assert_true(abi_version_compatible(PCV_PLUGIN_ABI_VERSION,
                                          PCV_PLUGIN_ABI_VERSION));
    g_assert_false(abi_version_compatible(PCV_PLUGIN_ABI_VERSION + 1,
                                           PCV_PLUGIN_ABI_VERSION));
}





static void
dummy_shutdown(void) {  }

static void test_unload_clears_all_fields(void)
{
    TestLoadedPlugin p;
    memset(&p, 0xAB, sizeof(p));


    g_strlcpy(p.name, "libfoo", sizeof(p.name));
    g_strlcpy(p.version, "1.0", sizeof(p.version));
    p.module = (void*)0xDEADBEEF;
    p.shutdown_fn = dummy_shutdown;


    test_plugin_clear(&p);


    g_assert_null(p.module);
    g_assert_null(p.shutdown_fn);
    g_assert_cmpstr(p.name, ==, "");
    g_assert_cmpstr(p.version, ==, "");
}

static void test_unload_idempotent_clear(void)
{
    TestLoadedPlugin p;
    p.module = (void*)0x1234;
    p.shutdown_fn = dummy_shutdown;

    test_plugin_clear(&p);
    g_assert_null(p.module);
    g_assert_null(p.shutdown_fn);


    test_plugin_clear(&p);
    g_assert_null(p.module);
    g_assert_null(p.shutdown_fn);
}





static void test_registry_duplicate_rejected(void)
{
    TestRegistry reg = {0};


    gboolean ok1 = test_registry_add(&reg, "myplugin.hello");
    g_assert_true(ok1);
    g_assert_cmpint(reg.count, ==, 1);


    gboolean ok2 = test_registry_add(&reg, "myplugin.hello");
    g_assert_false(ok2);
    g_assert_cmpint(reg.count, ==, 1);


    gboolean ok3 = test_registry_add(&reg, "myplugin.bye");
    g_assert_true(ok3);
    g_assert_cmpint(reg.count, ==, 2);


    g_assert_cmpstr(reg.entries[0].method, ==, "myplugin.hello");
}





void test_plugin_register(void)
{

    g_test_add_func("/plugin/path/valid",          test_plugin_path_valid);
    g_test_add_func("/plugin/path/invalid",        test_plugin_path_invalid);
    g_test_add_func("/plugin/path/boundary_256",   test_plugin_path_boundary_256);


    g_test_add_func("/plugin/name/extract_normal", test_plugin_name_extract_normal);
    g_test_add_func("/plugin/name/extract_edges",  test_plugin_name_extract_edge_cases);


    g_test_add_func("/plugin/symbol/valid",        test_symbol_name_valid);
    g_test_add_func("/plugin/symbol/invalid",      test_symbol_name_invalid);


    g_test_add_func("/plugin/abi/compatible",      test_abi_version_compatible);
    g_test_add_func("/plugin/abi/incompatible",    test_abi_version_incompatible);
    g_test_add_func("/plugin/abi/zero_rejected",   test_abi_version_zero_rejected);
    g_test_add_func("/plugin/abi/constant",        test_abi_version_constant);


    g_test_add_func("/plugin/unload/clears_all",   test_unload_clears_all_fields);
    g_test_add_func("/plugin/unload/idempotent",   test_unload_idempotent_clear);


    g_test_add_func("/plugin/registry/duplicate",  test_registry_duplicate_rejected);
}
