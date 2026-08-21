                                                                                               
                                                                                               
                                                                                
                                                                        
                             
   
                          
                                                             
  
                                                                               
                 
                                                                               
                                                                   
                      
  
                          
                                                  
                                           
                                            
  
          
                                                           
                                                       
                              
                                                         
                                                        
                                                    
  
                                                            
                                                
                                                                               
   
#include <glib.h>
#include <gio/gio.h>
#include <string.h>
#include <libvirt-gobject/libvirt-gobject.h>
#include "../src/modules/virt/vm_manager.h"

                                                              
                                                                   
extern gchar *_overlay_ethernet_iface_xml(void);
extern gchar *_overlay_metadata_xml(const gchar *network_mode, const gchar *tenant);
extern gboolean _overlay_metadata_parse(const gchar *metadata_xml,
                                         gchar **mode_out, gchar **tenant_out);

static GVirConnection *g_conn = NULL;
static gboolean g_have_conn = FALSE;

static void ensure_conn(void) {
    if (g_have_conn) return;
    g_conn = gvir_connection_new("test:///default");
    GError *err = NULL;
    if (!gvir_connection_open(g_conn, NULL, &err)) {
        if (err) g_error_free(err);
        g_object_unref(g_conn);
        g_conn = NULL;
    }
    g_have_conn = TRUE;
}

                                                      

static void test_new_with_null_conn(void) {
    PureCVisorVmManager *m = purecvisor_vm_manager_new(NULL);
    if (m) g_object_unref(m);
}

static void test_new_with_test_conn(void) {
    ensure_conn();
    if (!g_conn) { g_test_skip("libvirt test:/// 사용 불가"); return; }
    PureCVisorVmManager *m = purecvisor_vm_manager_new(g_conn);
    g_assert_nonnull(m);
    g_assert_true(PURECVISOR_IS_VM_MANAGER(m));
    g_object_unref(m);
}

                                                      

static void test_delete_status_unknown(void) {
    const gchar *st = pcv_vm_delete_status_get("nonexistent-vm-XYZ");
    g_assert_nonnull(st);
    g_assert_cmpstr(st, ==, "not_found");
}

static void test_delete_status_null_safe(void) {
                                       
    const gchar *st = pcv_vm_delete_status_get(NULL);
                                                
    if (st) {
        g_assert_true(g_strcmp0(st, "not_found") == 0 ||
                      g_strcmp0(st, "unknown") == 0);
    }
}

                                                          

static void test_cleanup_idempotent(void) {
    pcv_vm_manager_cleanup();
    pcv_vm_manager_cleanup();              
}

                                                     

static GMainLoop *g_loop = NULL;
static gboolean g_async_done = FALSE;
static JsonNode *g_async_node = NULL;
static GError *g_async_err = NULL;

static void on_list_done(GObject *src, GAsyncResult *res, gpointer u) {
    (void)u;
    g_async_node = purecvisor_vm_manager_list_vms_finish(
        PURECVISOR_VM_MANAGER(src), res, &g_async_err);
    g_async_done = TRUE;
    g_main_loop_quit(g_loop);
}

static void test_list_vms_test_driver(void) {
    ensure_conn();
    if (!g_conn) { g_test_skip("libvirt test:/// 사용 불가"); return; }

    PureCVisorVmManager *m = purecvisor_vm_manager_new(g_conn);
    g_loop = g_main_loop_new(NULL, FALSE);
    g_async_done = FALSE;
    g_async_node = NULL;
    g_async_err = NULL;

    purecvisor_vm_manager_list_vms_async(m, on_list_done, NULL);
    g_main_loop_run(g_loop);

    g_assert_true(g_async_done);
                                           
    if (g_async_node) {
        g_assert_true(JSON_NODE_HOLDS_ARRAY(g_async_node) || JSON_NODE_HOLDS_OBJECT(g_async_node));
        if (JSON_NODE_HOLDS_ARRAY(g_async_node)) {
            JsonArray *arr = json_node_get_array(g_async_node);
            g_test_message("test driver vms: %u", json_array_get_length(arr));
        }
        json_node_free(g_async_node);
    }
    if (g_async_err) g_error_free(g_async_err);

    g_object_unref(m);
    g_main_loop_unref(g_loop);
    g_loop = NULL;
}

                                                      
static void test_list_vms_metadata(void) {
    ensure_conn();
    if (!g_conn) { g_test_skip("libvirt test:/// 사용 불가"); return; }

    PureCVisorVmManager *m = purecvisor_vm_manager_new(g_conn);
    g_loop = g_main_loop_new(NULL, FALSE);
    g_async_done = FALSE;
    g_async_node = NULL;
    g_async_err = NULL;

    purecvisor_vm_manager_list_vms_async(m, on_list_done, NULL);
    g_main_loop_run(g_loop);

    g_assert_true(g_async_done);
    if (g_async_node && JSON_NODE_HOLDS_ARRAY(g_async_node)) {
        JsonArray *arr = json_node_get_array(g_async_node);
        for (guint i = 0; i < json_array_get_length(arr); i++) {
            JsonNode *el = json_array_get_element(arr, i);
            if (JSON_NODE_HOLDS_OBJECT(el)) {
                JsonObject *obj = json_node_get_object(el);
                                                                      
                if (json_object_has_member(obj, "name")) {
                    const gchar *n = json_object_get_string_member(obj, "name");
                    g_assert_nonnull(n);
                }
            }
        }
        json_node_free(g_async_node);
    }
    if (g_async_err) g_error_free(g_async_err);

    g_object_unref(m);
    g_main_loop_unref(g_loop);
    g_loop = NULL;
}

                                                       

static gboolean g_async_ok = FALSE;

static void on_start_done(GObject *src, GAsyncResult *res, gpointer u) {
    (void)u;
    g_async_ok = purecvisor_vm_manager_start_vm_finish(
        PURECVISOR_VM_MANAGER(src), res, &g_async_err);
    g_async_done = TRUE;
    g_main_loop_quit(g_loop);
}

static void test_start_vm_nonexistent(void) {
    ensure_conn();
    if (!g_conn) { g_test_skip("libvirt test:/// 사용 불가"); return; }

    PureCVisorVmManager *m = purecvisor_vm_manager_new(g_conn);
    g_loop = g_main_loop_new(NULL, FALSE);
    g_async_done = FALSE;
    g_async_err = NULL;
    g_async_ok = TRUE;

    purecvisor_vm_manager_start_vm_async(m, "nonexistent-pcv-vm", on_start_done, NULL);
    g_main_loop_run(g_loop);

    g_assert_true(g_async_done);
    g_assert_false(g_async_ok);                       
    if (g_async_err) g_error_free(g_async_err);

    g_object_unref(m);
    g_main_loop_unref(g_loop);
    g_loop = NULL;
}

static void on_stop_done(GObject *src, GAsyncResult *res, gpointer u) {
    (void)u;
    g_async_ok = purecvisor_vm_manager_stop_vm_finish(
        PURECVISOR_VM_MANAGER(src), res, &g_async_err);
    g_async_done = TRUE;
    g_main_loop_quit(g_loop);
}

static void test_stop_vm_nonexistent(void) {
    ensure_conn();
    if (!g_conn) { g_test_skip("libvirt test:/// 사용 불가"); return; }

    PureCVisorVmManager *m = purecvisor_vm_manager_new(g_conn);
    g_loop = g_main_loop_new(NULL, FALSE);
    g_async_done = FALSE;
    g_async_err = NULL;
    g_async_ok = TRUE;

    purecvisor_vm_manager_stop_vm_async(m, "nonexistent-pcv-vm", on_stop_done, NULL);
    g_main_loop_run(g_loop);

    g_assert_true(g_async_done);
    g_assert_false(g_async_ok);
    if (g_async_err) g_error_free(g_async_err);

    g_object_unref(m);
    g_main_loop_unref(g_loop);
    g_loop = NULL;
}

                                                         

static void on_delete_done(GObject *src, GAsyncResult *res, gpointer u) {
    (void)u;
    g_async_ok = purecvisor_vm_manager_delete_vm_finish(
        PURECVISOR_VM_MANAGER(src), res, &g_async_err);
    g_async_done = TRUE;
    g_main_loop_quit(g_loop);
}

static void test_delete_vm_nonexistent(void) {
    ensure_conn();
    if (!g_conn) { g_test_skip("libvirt test:/// 사용 불가"); return; }

    PureCVisorVmManager *m = purecvisor_vm_manager_new(g_conn);
    g_loop = g_main_loop_new(NULL, FALSE);
    g_async_done = FALSE;
    g_async_err = NULL;
    g_async_ok = TRUE;

    purecvisor_vm_manager_delete_vm_async(m, "nonexistent-vm-XYZ", on_delete_done, NULL);
    g_main_loop_run(g_loop);

    g_assert_true(g_async_done);
    g_assert_false(g_async_ok);
    if (g_async_err) g_error_free(g_async_err);

    g_object_unref(m);
    g_main_loop_unref(g_loop);
    g_loop = NULL;
}

                                                         

static void on_set_vcpu_done(GObject *src, GAsyncResult *res, gpointer u) {
    (void)u;
    g_async_ok = purecvisor_vm_manager_set_vcpu_finish(
        PURECVISOR_VM_MANAGER(src), res, &g_async_err);
    g_async_done = TRUE;
    g_main_loop_quit(g_loop);
}

static void test_set_vcpu_nonexistent(void) {
    ensure_conn();
    if (!g_conn) { g_test_skip("libvirt test:/// 사용 불가"); return; }

    PureCVisorVmManager *m = purecvisor_vm_manager_new(g_conn);
    g_loop = g_main_loop_new(NULL, FALSE);
    g_async_done = FALSE;
    g_async_err = NULL;

    purecvisor_vm_manager_set_vcpu_async(m, "nonexistent-vm", 4, NULL, on_set_vcpu_done, NULL);
    g_main_loop_run(g_loop);

    g_assert_true(g_async_done);
    if (g_async_err) g_error_free(g_async_err);

    g_object_unref(m);
    g_main_loop_unref(g_loop);
    g_loop = NULL;
}

static void on_set_mem_done(GObject *src, GAsyncResult *res, gpointer u) {
    (void)u;
    g_async_ok = purecvisor_vm_manager_set_memory_finish(
        PURECVISOR_VM_MANAGER(src), res, &g_async_err);
    g_async_done = TRUE;
    g_main_loop_quit(g_loop);
}

static void test_set_memory_nonexistent(void) {
    ensure_conn();
    if (!g_conn) { g_test_skip("libvirt test:/// 사용 불가"); return; }

    PureCVisorVmManager *m = purecvisor_vm_manager_new(g_conn);
    g_loop = g_main_loop_new(NULL, FALSE);
    g_async_done = FALSE;
    g_async_err = NULL;

    purecvisor_vm_manager_set_memory_async(m, "nonexistent-vm", 2048, NULL, on_set_mem_done, NULL);
    g_main_loop_run(g_loop);

    g_assert_true(g_async_done);
    if (g_async_err) g_error_free(g_async_err);

    g_object_unref(m);
    g_main_loop_unref(g_loop);
    g_loop = NULL;
}

                                                             
static void test_resolve_bridge_null_defaults(void) {
    gchar *r = purecvisor_vm_resolve_network_bridge(NULL);
    g_assert_cmpstr(r, ==, "pcvnat0");                            
    g_free(r);
}
static void test_resolve_bridge_empty_defaults(void) {
    gchar *r = purecvisor_vm_resolve_network_bridge("");
    g_assert_cmpstr(r, ==, "pcvnat0");
    g_free(r);
}
static void test_resolve_bridge_none_is_null(void) {
    g_assert_null(purecvisor_vm_resolve_network_bridge("none"));
}
static void test_resolve_bridge_explicit_passthrough(void) {
    gchar *r = purecvisor_vm_resolve_network_bridge("br-custom");
    g_assert_cmpstr(r, ==, "br-custom");
    g_free(r);
}

                                                                       
                                                                             
                                                                    

static void test_overlay_ethernet_iface_shape(void) {
    gchar *xml = _overlay_ethernet_iface_xml();
    g_assert_nonnull(xml);
    g_assert_nonnull(strstr(xml, "type='ethernet'"));
    g_assert_nonnull(strstr(xml, "<model type='virtio'/>"));
    g_assert_null(strstr(xml, "type='bridge'"));
    g_assert_null(strstr(xml, "<source"));
    g_assert_null(strstr(xml, "virtualport"));
    g_free(xml);
}

                                                              
                                                                     
                                                           

static void test_overlay_metadata_build_parse_roundtrip(void) {
    gchar *xml = _overlay_metadata_xml("tenant-overlay", "acme");
    g_assert_nonnull(xml);
    g_assert_nonnull(strstr(xml, "pcv:overlay"));

    gchar *mode = NULL, *tenant = NULL;
    gboolean ok = _overlay_metadata_parse(xml, &mode, &tenant);
    g_assert_true(ok);
    g_assert_cmpstr(mode, ==, "tenant-overlay");
    g_assert_cmpstr(tenant, ==, "acme");
    g_free(mode);
    g_free(tenant);
    g_free(xml);
}

static void test_overlay_metadata_build_non_overlay_is_empty(void) {
    gchar *xml1 = _overlay_metadata_xml("bridge", "acme");
    g_assert_cmpstr(xml1, ==, "");
    g_free(xml1);

    gchar *xml2 = _overlay_metadata_xml(NULL, NULL);
    g_assert_cmpstr(xml2, ==, "");
    g_free(xml2);

    gchar *xml3 = _overlay_metadata_xml("", "acme");
    g_assert_cmpstr(xml3, ==, "");
    g_free(xml3);
}

static void test_overlay_metadata_build_escapes_values(void) {
                                                                
                                                   
    gchar *xml = _overlay_metadata_xml("tenant-overlay", "a&b<c");
    g_assert_nonnull(xml);
    g_assert_null(strstr(xml, "tenant='a&b<c'"));
    g_assert_nonnull(strstr(xml, "&amp;"));
    g_assert_nonnull(strstr(xml, "&lt;"));
    g_free(xml);
}

static void test_overlay_metadata_parse_absent_returns_false(void) {
    gchar *mode = NULL, *tenant = NULL;
    gboolean ok = _overlay_metadata_parse(
        "<metadata><pcv:owner xmlns:pcv='urn:purecvisor:metadata'>x</pcv:owner></metadata>",
        &mode, &tenant);
    g_assert_false(ok);
    g_assert_null(mode);
    g_assert_null(tenant);
}

static void test_overlay_metadata_parse_malformed_returns_false(void) {
    gchar *mode = NULL, *tenant = NULL;
                                  
    gboolean ok = _overlay_metadata_parse(
        "<pcv:overlay xmlns:pcv='urn:purecvisor:overlay:1' network_mode='tenant-overlay'/>",
        &mode, &tenant);
    g_assert_false(ok);
    g_free(mode);
    g_free(tenant);
}

static void test_overlay_metadata_parse_null_safe(void) {
    gchar *mode = NULL, *tenant = NULL;
    g_assert_false(_overlay_metadata_parse(NULL, &mode, &tenant));
    g_assert_false(_overlay_metadata_parse("<pcv:overlay/>", NULL, &tenant));
}

                                                                            
                                                                             
                                                                                
                                                    
                                                                           
                                                      
                                                           
static void test_overlay_metadata_parse_libvirt_stripped_form(void) {
    gchar *mode = NULL, *tenant = NULL;
    gboolean ok = _overlay_metadata_parse(
        "<overlay network_mode=\"tenant-overlay\" tenant=\"acme\"/>",
        &mode, &tenant);
    g_assert_true(ok);
    g_assert_cmpstr(mode, ==, "tenant-overlay");
    g_assert_cmpstr(tenant, ==, "acme");
    g_free(mode);
    g_free(tenant);
}

                                                                
static void test_overlay_metadata_parse_libvirt_attr_reordered(void) {
    gchar *mode = NULL, *tenant = NULL;
    gboolean ok = _overlay_metadata_parse(
        "<overlay tenant=\"beta\" network_mode=\"tenant-overlay\"/>",
        &mode, &tenant);
    g_assert_true(ok);
    g_assert_cmpstr(mode, ==, "tenant-overlay");
    g_assert_cmpstr(tenant, ==, "beta");
    g_free(mode);
    g_free(tenant);
}

                                                                
                                                                        
                                                               
                                           

                                                             
                                                                          
#define OVL_LIVE_XML_TWO_IFACES \
    "<domain type='kvm'><devices>\n" \
    "  <interface type='bridge'>\n" \
    "    <mac address='52:54:00:aa:bb:cc'/>\n" \
    "    <source bridge='pcvnat0'/>\n" \
    "    <target dev='vnet3'/>\n" \
    "    <model type='virtio'/>\n" \
    "  </interface>\n" \
    "  <interface type='ethernet'>\n" \
    "    <mac address='52:54:00:12:34:56'/>\n" \
    "    <target dev='vnet7'/>\n" \
    "    <model type='virtio'/>\n" \
    "  </interface>\n" \
    "</devices></domain>"

static void test_overlay_live_iface_parse_picks_ethernet(void) {
    gchar *tap = NULL, *mac = NULL;
    gboolean ok = _overlay_live_iface_parse(OVL_LIVE_XML_TWO_IFACES, &tap, &mac);
    g_assert_true(ok);
    g_assert_cmpstr(tap, ==, "vnet7");
    g_assert_cmpstr(mac, ==, "52:54:00:12:34:56");
    g_free(tap);
    g_free(mac);
}

static void test_overlay_live_iface_parse_attr_order_independent(void) {
                                                       
    const gchar *xml =
        "<devices><interface managed='no' type='ethernet'>\n"
        "  <target dev='vnet11'/>\n"
        "  <mac address='52:54:00:de:ad:be'/>\n"
        "</interface></devices>";
    gchar *tap = NULL, *mac = NULL;
    gboolean ok = _overlay_live_iface_parse(xml, &tap, &mac);
    g_assert_true(ok);
    g_assert_cmpstr(tap, ==, "vnet11");
    g_assert_cmpstr(mac, ==, "52:54:00:de:ad:be");
    g_free(tap);
    g_free(mac);
}

static void test_overlay_live_iface_parse_absent_returns_false(void) {
                                                            
    const gchar *xml =
        "<devices><interface type='bridge'>\n"
        "  <mac address='52:54:00:aa:bb:cc'/>\n"
        "  <target dev='vnet3'/>\n"
        "</interface></devices>";
    gchar *tap = NULL, *mac = NULL;
    gboolean ok = _overlay_live_iface_parse(xml, &tap, &mac);
    g_assert_false(ok);
    g_assert_null(tap);
    g_assert_null(mac);
}

static void test_overlay_live_iface_parse_missing_target_returns_false(void) {
                                                               
    const gchar *xml =
        "<devices><interface type='ethernet'>\n"
        "  <mac address='52:54:00:12:34:56'/>\n"
        "  <model type='virtio'/>\n"
        "</interface></devices>";
    gchar *tap = NULL, *mac = NULL;
    gboolean ok = _overlay_live_iface_parse(xml, &tap, &mac);
    g_assert_false(ok);
    g_assert_null(tap);
    g_assert_null(mac);
}

static void test_overlay_live_iface_parse_null_safe(void) {
    gchar *tap = NULL, *mac = NULL;
    g_assert_false(_overlay_live_iface_parse(NULL, &tap, &mac));
    g_assert_false(_overlay_live_iface_parse("<devices/>", NULL, &mac));
    g_assert_false(_overlay_live_iface_parse("<devices/>", &tap, NULL));
}

                                                                 
                                                             

static void test_overlay_gw_cidr_basic(void) {
    gchar *gw = _overlay_gw_cidr_from_subnet("10.100.5.0/24");
    g_assert_cmpstr(gw, ==, "10.100.5.1/24");
    g_free(gw);
}

static void test_overlay_gw_cidr_high_index(void) {
    gchar *gw = _overlay_gw_cidr_from_subnet("10.100.200.0/24");
    g_assert_cmpstr(gw, ==, "10.100.200.1/24");
    g_free(gw);
}

static void test_overlay_gw_cidr_malformed_returns_null(void) {
    g_assert_null(_overlay_gw_cidr_from_subnet(NULL));
    g_assert_null(_overlay_gw_cidr_from_subnet("10.100.5.0"));                     
    g_assert_null(_overlay_gw_cidr_from_subnet("10.100.0/24"));                
    g_assert_null(_overlay_gw_cidr_from_subnet("garbage"));
}

void test_vm_manager_register(void) {
    g_test_add_func("/vm_manager/new_with_null_conn", test_new_with_null_conn);
    g_test_add_func("/vm_manager/new_with_test_conn", test_new_with_test_conn);
    g_test_add_func("/vm_manager/delete_status_unknown", test_delete_status_unknown);
    g_test_add_func("/vm_manager/delete_status_null_safe", test_delete_status_null_safe);
    g_test_add_func("/vm_manager/cleanup_idempotent", test_cleanup_idempotent);
    g_test_add_func("/vm_manager/list_vms_test_driver", test_list_vms_test_driver);
    g_test_add_func("/vm_manager/start_vm_nonexistent", test_start_vm_nonexistent);
    g_test_add_func("/vm_manager/stop_vm_nonexistent", test_stop_vm_nonexistent);
    g_test_add_func("/vm_manager/delete_vm_nonexistent", test_delete_vm_nonexistent);
    g_test_add_func("/vm_manager/set_vcpu_nonexistent", test_set_vcpu_nonexistent);
    g_test_add_func("/vm_manager/set_memory_nonexistent", test_set_memory_nonexistent);
    g_test_add_func("/vm_manager/list_vms_metadata", test_list_vms_metadata);
    g_test_add_func("/vm_manager/resolve_bridge_null_defaults", test_resolve_bridge_null_defaults);
    g_test_add_func("/vm_manager/resolve_bridge_empty_defaults", test_resolve_bridge_empty_defaults);
    g_test_add_func("/vm_manager/resolve_bridge_none_is_null", test_resolve_bridge_none_is_null);
    g_test_add_func("/vm_manager/resolve_bridge_explicit_passthrough", test_resolve_bridge_explicit_passthrough);
    g_test_add_func("/vm_manager/overlay_ethernet_iface_shape", test_overlay_ethernet_iface_shape);
    g_test_add_func("/vm_manager/overlay_metadata_build_parse_roundtrip", test_overlay_metadata_build_parse_roundtrip);
    g_test_add_func("/vm_manager/overlay_metadata_build_non_overlay_is_empty", test_overlay_metadata_build_non_overlay_is_empty);
    g_test_add_func("/vm_manager/overlay_metadata_build_escapes_values", test_overlay_metadata_build_escapes_values);
    g_test_add_func("/vm_manager/overlay_metadata_parse_absent_returns_false", test_overlay_metadata_parse_absent_returns_false);
    g_test_add_func("/vm_manager/overlay_metadata_parse_malformed_returns_false", test_overlay_metadata_parse_malformed_returns_false);
    g_test_add_func("/vm_manager/overlay_metadata_parse_null_safe", test_overlay_metadata_parse_null_safe);
    g_test_add_func("/vm_manager/overlay_metadata_parse_libvirt_stripped_form", test_overlay_metadata_parse_libvirt_stripped_form);
    g_test_add_func("/vm_manager/overlay_metadata_parse_libvirt_attr_reordered", test_overlay_metadata_parse_libvirt_attr_reordered);
    g_test_add_func("/vm_manager/overlay_live_iface_parse_picks_ethernet", test_overlay_live_iface_parse_picks_ethernet);
    g_test_add_func("/vm_manager/overlay_live_iface_parse_attr_order_independent", test_overlay_live_iface_parse_attr_order_independent);
    g_test_add_func("/vm_manager/overlay_live_iface_parse_absent_returns_false", test_overlay_live_iface_parse_absent_returns_false);
    g_test_add_func("/vm_manager/overlay_live_iface_parse_missing_target_returns_false", test_overlay_live_iface_parse_missing_target_returns_false);
    g_test_add_func("/vm_manager/overlay_live_iface_parse_null_safe", test_overlay_live_iface_parse_null_safe);
    g_test_add_func("/vm_manager/overlay_gw_cidr_basic", test_overlay_gw_cidr_basic);
    g_test_add_func("/vm_manager/overlay_gw_cidr_high_index", test_overlay_gw_cidr_high_index);
    g_test_add_func("/vm_manager/overlay_gw_cidr_malformed_returns_null", test_overlay_gw_cidr_malformed_returns_null);
}
