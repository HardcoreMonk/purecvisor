
#include <glib.h>
#include <gio/gio.h>
#include <libvirt-gobject/libvirt-gobject.h>
#include "../src/modules/virt/vm_manager.h"

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
}
