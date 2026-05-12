














#include <glib.h>
#include <json-glib/json-glib.h>
#include "modules/network/ovs_overlay.h"


extern gboolean pcv_ovn_is_available(void);
extern JsonArray *pcv_ovn_switch_list(void);
extern JsonArray *pcv_ovn_router_list(void);
extern JsonArray *pcv_ovn_nat_list(const gchar *router);
extern JsonArray *pcv_ovn_dhcp_list(void);
extern JsonArray *pcv_ovn_acl_list(const gchar *sw);
extern JsonObject *pcv_ovn_status(void);
extern gboolean pcv_ovn_switch_delete(const gchar *name, GError **error);
extern gboolean pcv_ovn_router_delete(const gchar *name, GError **error);



static void test_ovn_switch_list_empty(void) {
    JsonArray *arr = pcv_ovn_switch_list();
    g_assert_nonnull(arr);
    g_assert_cmpuint(json_array_get_length(arr), ==, 0);
    json_array_unref(arr);
}

static void test_ovn_router_list_empty(void) {
    JsonArray *arr = pcv_ovn_router_list();
    g_assert_nonnull(arr);
    g_assert_cmpuint(json_array_get_length(arr), ==, 0);
    json_array_unref(arr);
}

static void test_ovn_nat_list_empty(void) {
    JsonArray *arr = pcv_ovn_nat_list("nonexist");
    g_assert_nonnull(arr);
    g_assert_cmpuint(json_array_get_length(arr), ==, 0);
    json_array_unref(arr);
}

static void test_ovn_dhcp_list_empty(void) {
    JsonArray *arr = pcv_ovn_dhcp_list();
    g_assert_nonnull(arr);
    g_assert_cmpuint(json_array_get_length(arr), ==, 0);
    json_array_unref(arr);
}

static void test_ovn_acl_list_empty(void) {
    JsonArray *arr = pcv_ovn_acl_list("nonexist");
    g_assert_nonnull(arr);
    g_assert_cmpuint(json_array_get_length(arr), ==, 0);
    json_array_unref(arr);
}



static void test_ovn_switch_delete_idempotent(void) {
    g_assert_true(pcv_ovn_switch_delete("nonexist-sw", NULL));
}

static void test_ovn_router_delete_idempotent(void) {
    g_assert_true(pcv_ovn_router_delete("nonexist-lr", NULL));
}



static void test_ovn_status_structure(void) {
    JsonObject *obj = pcv_ovn_status();
    g_assert_nonnull(obj);
    g_assert_true(json_object_has_member(obj, "available"));
    json_object_unref(obj);
}

#if !PCV_CLUSTER_ENABLED
static void test_overlay_list_empty_single(void) {
    JsonArray *arr = pcv_overlay_list();
    g_assert_nonnull(arr);
    g_assert_cmpuint(json_array_get_length(arr), ==, 0);
    json_array_unref(arr);
}

static void test_overlay_info_reports_disabled_before_init(void) {
    JsonObject *obj = pcv_overlay_info("pcvoverlay0");
    g_assert_nonnull(obj);
    g_assert_true(json_object_has_member(obj, "error"));
    g_assert_cmpstr(json_object_get_string_member(obj, "error"), ==, "overlay disabled");
    json_object_unref(obj);
}
#endif



void test_ovn_register(void) {
    g_test_add_func("/ovn/switch_list/empty",      test_ovn_switch_list_empty);
    g_test_add_func("/ovn/router_list/empty",      test_ovn_router_list_empty);
    g_test_add_func("/ovn/nat_list/empty",         test_ovn_nat_list_empty);
    g_test_add_func("/ovn/dhcp_list/empty",        test_ovn_dhcp_list_empty);
    g_test_add_func("/ovn/acl_list/empty",         test_ovn_acl_list_empty);
    g_test_add_func("/ovn/switch_delete/idempotent", test_ovn_switch_delete_idempotent);
    g_test_add_func("/ovn/router_delete/idempotent", test_ovn_router_delete_idempotent);
    g_test_add_func("/ovn/status/structure",       test_ovn_status_structure);
#if !PCV_CLUSTER_ENABLED
    g_test_add_func("/overlay/list/empty_single",  test_overlay_list_empty_single);
    g_test_add_func("/overlay/info/disabled_before_init",
                    test_overlay_info_reports_disabled_before_init);
#endif
}
