                                                                                      
                                                                                      
                                                                 
                                                                     
                                                      
                   
  
                                                             
  
                 
                                                  
                                                  
                                   
                                       
  
                                 
  
                                            
   

#include <glib.h>
#include <json-glib/json-glib.h>
#include "modules/network/ovs_overlay.h"

                                
extern gboolean pcv_ovn_is_available(void);
extern JsonArray *pcv_ovn_switch_list(void);
extern JsonArray *pcv_ovn_router_list(void);
extern JsonArray *pcv_ovn_nat_list(const gchar *router);
extern JsonArray *pcv_ovn_nat_list_parse(const gchar *output);
extern JsonArray *pcv_ovn_dhcp_list(void);
extern JsonArray *pcv_ovn_acl_list(const gchar *sw);
extern JsonObject *pcv_ovn_status(void);
extern gboolean pcv_ovn_switch_delete(const gchar *name, GError **error);
extern gboolean pcv_ovn_router_delete(const gchar *name, GError **error);
                                                        
extern gboolean pcv_ovn_valid_id(const gchar *s);

                                                   

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

static void test_ovn_nat_list_parser_omits_cli_header(void) {
    const gchar *output =
        "TYPE             GATEWAY_PORT          MATCH                 EXTERNAL_IP        EXTERNAL_PORT    LOGICAL_IP          EXTERNAL_MAC         LOGICAL_PORT\n"
        "dnat_and_snat                                                192.0.2.11                          10.252.10.11\n"
        "snat                                                         192.0.2.10                          10.252.10.0/24\n";
    JsonArray *arr = pcv_ovn_nat_list_parse(output);
    g_assert_nonnull(arr);
    g_assert_cmpuint(json_array_get_length(arr), ==, 2);
    g_assert_true(g_str_has_prefix(json_array_get_string_element(arr, 0),
                                   "dnat_and_snat"));
    g_assert_true(g_str_has_prefix(json_array_get_string_element(arr, 1),
                                   "snat"));
    json_array_unref(arr);

    arr = pcv_ovn_nat_list_parse(
        "TYPE EXTERNAL_IP LOGICAL_IP\n");
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

                                      
  
                                                           
                                                      
                                               
                                                
static void test_ovn_valid_id_rejects_injection(void) {
                    
    g_assert_true(pcv_ovn_valid_id("pcv-ls0"));
    g_assert_true(pcv_ovn_valid_id("tenant-alpha-ls"));
    g_assert_true(pcv_ovn_valid_id("10.0.0.1"));                      

                       
    g_assert_false(pcv_ovn_valid_id("ls add"));                             
    g_assert_false(pcv_ovn_valid_id("sw --may-exist"));                
    g_assert_false(pcv_ovn_valid_id("--priv"));                              
    g_assert_false(pcv_ovn_valid_id("--"));                           
    g_assert_false(pcv_ovn_valid_id("sw;ls-del x"));               
    g_assert_false(pcv_ovn_valid_id("sw\"quote"));                 
    g_assert_false(pcv_ovn_valid_id(""));                            
    g_assert_false(pcv_ovn_valid_id(NULL));                         
}

                           

static void test_ovn_status_structure(void) {
    JsonObject *obj = pcv_ovn_status();
    g_assert_nonnull(obj);
    const gchar *members[] = {
        "available", "installed", "northbound_connected", "southbound_connected",
        "northd_synced", "controller_configured", "chassis_registered", NULL
    };
    for (guint i = 0; members[i]; i++)
        g_assert_true(json_object_has_member(obj, members[i]));
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
    g_test_add_func("/ovn/nat_list/header_omitted",
                    test_ovn_nat_list_parser_omits_cli_header);
    g_test_add_func("/ovn/dhcp_list/empty",        test_ovn_dhcp_list_empty);
    g_test_add_func("/ovn/acl_list/empty",         test_ovn_acl_list_empty);
    g_test_add_func("/ovn/switch_delete/idempotent", test_ovn_switch_delete_idempotent);
    g_test_add_func("/ovn/router_delete/idempotent", test_ovn_router_delete_idempotent);
    g_test_add_func("/ovn/valid_id/rejects_injection", test_ovn_valid_id_rejects_injection);
    g_test_add_func("/ovn/status/structure",       test_ovn_status_structure);
#if !PCV_CLUSTER_ENABLED
    g_test_add_func("/overlay/list/empty_single",  test_overlay_list_empty_single);
    g_test_add_func("/overlay/info/disabled_before_init",
                    test_overlay_info_reports_disabled_before_init);
#endif
}
