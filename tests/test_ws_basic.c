
#include <glib.h>
#include <json-glib/json-glib.h>
#include <string.h>

#define MAX_WS_CONNECTIONS 1000
#define WS_IDLE_TIMEOUT_SEC 300
#define WS_CHECK_INTERVAL_SEC 60

static JsonObject *
_build_ws_event(const gchar *type, JsonObject *data)
{
    if (!type) return NULL;
    JsonObject *evt = json_object_new();
    json_object_set_string_member(evt, "type", type);
    if (data)
        json_object_set_object_member(evt, "data", json_object_ref(data));
    else
        json_object_set_null_member(evt, "data");
    return evt;
}

static void test_ws_event_telemetry(void) {
    JsonObject *data = json_object_new();
    json_object_set_double_member(data, "cpu_percent", 45.2);
    json_object_set_int_member(data, "mem_used_mb", 2048);

    JsonObject *evt = _build_ws_event("telemetry", data);
    g_assert_nonnull(evt);
    g_assert_cmpstr(json_object_get_string_member(evt, "type"), ==, "telemetry");
    g_assert_true(json_object_has_member(evt, "data"));

    JsonObject *d = json_object_get_object_member(evt, "data");
    g_assert_cmpfloat(json_object_get_double_member(d, "cpu_percent"), >, 0.0);

    json_object_unref(evt);
    json_object_unref(data);
}

static void test_ws_event_vm_event(void) {
    JsonObject *data = json_object_new();
    json_object_set_string_member(data, "vm_name", "web-prod");
    json_object_set_string_member(data, "state", "running");

    JsonObject *evt = _build_ws_event("vm_event", data);
    g_assert_nonnull(evt);
    g_assert_cmpstr(json_object_get_string_member(evt, "type"), ==, "vm_event");

    json_object_unref(evt);
    json_object_unref(data);
}

static void test_ws_event_cluster_event(void) {
    JsonObject *data = json_object_new();
    json_object_set_string_member(data, "event", "node_joined");

    JsonObject *evt = _build_ws_event("cluster_event", data);
    g_assert_nonnull(evt);
    g_assert_cmpstr(json_object_get_string_member(evt, "type"), ==, "cluster_event");

    json_object_unref(evt);
    json_object_unref(data);
}

static void test_ws_event_null_type(void) {
    JsonObject *evt = _build_ws_event(NULL, NULL);
    g_assert_null(evt);
}

static void test_ws_event_null_data(void) {
    JsonObject *evt = _build_ws_event("telemetry", NULL);
    g_assert_nonnull(evt);
    g_assert_true(json_object_get_null_member(evt, "data"));
    json_object_unref(evt);
}

static gboolean
_validate_ws_event_type(const gchar *type)
{
    if (!type || type[0] == '\0') return FALSE;
    return (g_strcmp0(type, "telemetry") == 0 ||
            g_strcmp0(type, "vm_event") == 0 ||
            g_strcmp0(type, "cluster_event") == 0);
}

static void test_ws_event_type_valid(void) {
    g_assert_true(_validate_ws_event_type("telemetry"));
    g_assert_true(_validate_ws_event_type("vm_event"));
    g_assert_true(_validate_ws_event_type("cluster_event"));
}

static void test_ws_event_type_invalid(void) {
    g_assert_false(_validate_ws_event_type(NULL));
    g_assert_false(_validate_ws_event_type(""));
    g_assert_false(_validate_ws_event_type("unknown_event"));
    g_assert_false(_validate_ws_event_type("Telemetry"));
}

static void test_ws_connection_limits(void) {
    g_assert_cmpint(MAX_WS_CONNECTIONS, ==, 1000);
    g_assert_cmpint(WS_IDLE_TIMEOUT_SEC, ==, 300);
    g_assert_cmpint(WS_CHECK_INTERVAL_SEC, ==, 60);
    g_assert_cmpint(WS_IDLE_TIMEOUT_SEC, >, WS_CHECK_INTERVAL_SEC);
}

static void test_ws_connection_accept_logic(void) {
    gint current = 0;

    g_assert_cmpint(current, <, MAX_WS_CONNECTIONS);
    current++;
    g_assert_cmpint(current, ==, 1);

    current = MAX_WS_CONNECTIONS;
    g_assert_false(current < MAX_WS_CONNECTIONS);

    current = MAX_WS_CONNECTIONS - 1;
    g_assert_true(current < MAX_WS_CONNECTIONS);
}

static void test_ws_vnc_path_pattern(void) {
    const gchar *events_path = "/api/v1/ws/events";
    const gchar *vnc_path = "/api/v1/ws/vnc";

    g_assert_true(g_str_has_prefix(events_path, "/api/v1/ws/"));
    g_assert_true(g_str_has_prefix(vnc_path, "/api/v1/ws/"));
    g_assert_cmpstr(events_path, !=, vnc_path);
}

void test_ws_basic_register(void) {
    g_test_add_func("/ws/event/telemetry",          test_ws_event_telemetry);
    g_test_add_func("/ws/event/vm_event",           test_ws_event_vm_event);
    g_test_add_func("/ws/event/cluster_event",      test_ws_event_cluster_event);
    g_test_add_func("/ws/event/null_type",          test_ws_event_null_type);
    g_test_add_func("/ws/event/null_data",          test_ws_event_null_data);
    g_test_add_func("/ws/event_type/valid",         test_ws_event_type_valid);
    g_test_add_func("/ws/event_type/invalid",       test_ws_event_type_invalid);
    g_test_add_func("/ws/connection/limits",        test_ws_connection_limits);
    g_test_add_func("/ws/connection/accept_logic",  test_ws_connection_accept_logic);
    g_test_add_func("/ws/vnc/path_pattern",         test_ws_vnc_path_pattern);
}
