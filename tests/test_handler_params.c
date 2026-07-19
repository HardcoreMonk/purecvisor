
#include <glib.h>
#include <json-glib/json-glib.h>
#include <string.h>

typedef struct {
    gint   error_code;
    gchar *error_message;
    const gchar *result_value;
} ParamValidationResult;

static ParamValidationResult
validate_require_param(JsonObject *params, const gchar *key)
{
    ParamValidationResult r = { 0, NULL, NULL };

    if (!params || !json_object_has_member(params, key)) {
        r.error_code = -32602;
        r.error_message = g_strdup_printf("Missing required parameter: %s", key);
        return r;
    }

    const gchar *val = json_object_get_string_member(params, key);
    if (!val || val[0] == '\0') {
        r.error_code = -32602;
        r.error_message = g_strdup_printf("Empty or invalid parameter: %s", key);
        return r;
    }

    r.result_value = val;
    return r;
}

static ParamValidationResult
validate_require_param_or(JsonObject *params, const gchar *key1, const gchar *key2)
{
    ParamValidationResult r = { 0, NULL, NULL };
    const gchar *val = NULL;

    if (params && json_object_has_member(params, key1))
        val = json_object_get_string_member(params, key1);
    if ((!val || val[0] == '\0') && params && json_object_has_member(params, key2))
        val = json_object_get_string_member(params, key2);

    if (!val || val[0] == '\0') {
        r.error_code = -32602;
        r.error_message = g_strdup_printf("Missing required parameter: %s or %s", key1, key2);
        return r;
    }

    r.result_value = val;
    return r;
}

static void
apply_bidirectional_alias(JsonObject *params)
{
    if (!params) return;

    if (!json_object_has_member(params, "vm_id") &&
         json_object_has_member(params, "name")) {
        const gchar *alias = json_object_get_string_member(params, "name");
        if (alias) json_object_set_string_member(params, "vm_id", alias);
    } else if (!json_object_has_member(params, "name") &&
                json_object_has_member(params, "vm_id")) {
        const gchar *alias = json_object_get_string_member(params, "vm_id");
        if (alias) json_object_set_string_member(params, "name", alias);
    }
}

static void test_require_param_valid(void) {
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "vm_id", "test-vm");

    ParamValidationResult r = validate_require_param(params, "vm_id");
    g_assert_cmpint(r.error_code, ==, 0);
    g_assert_cmpstr(r.result_value, ==, "test-vm");
    g_assert_null(r.error_message);

    json_object_unref(params);
}

static void test_require_param_null_params(void) {
    ParamValidationResult r = validate_require_param(NULL, "vm_id");
    g_assert_cmpint(r.error_code, ==, -32602);
    g_assert_nonnull(r.error_message);
    g_assert_nonnull(strstr(r.error_message, "vm_id"));
    g_free(r.error_message);
}

static void test_require_param_missing_key(void) {
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "other_key", "value");

    ParamValidationResult r = validate_require_param(params, "vm_id");
    g_assert_cmpint(r.error_code, ==, -32602);
    g_assert_nonnull(strstr(r.error_message, "vm_id"));
    g_free(r.error_message);
    json_object_unref(params);
}

static void test_require_param_empty_string(void) {
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "vm_id", "");

    ParamValidationResult r = validate_require_param(params, "vm_id");
    g_assert_cmpint(r.error_code, ==, -32602);
    g_assert_nonnull(strstr(r.error_message, "vm_id"));
    g_free(r.error_message);
    json_object_unref(params);
}

static void test_require_param_null_value(void) {
    JsonObject *params = json_object_new();
    json_object_set_null_member(params, "vm_id");

    ParamValidationResult r = validate_require_param(params, "vm_id");
    g_assert_cmpint(r.error_code, ==, -32602);
    g_free(r.error_message);
    json_object_unref(params);
}

static void test_require_param_or_primary(void) {
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", "web-prod");

    ParamValidationResult r = validate_require_param_or(params, "name", "vm_id");
    g_assert_cmpint(r.error_code, ==, 0);
    g_assert_cmpstr(r.result_value, ==, "web-prod");
    g_assert_null(r.error_message);

    json_object_unref(params);
}

static void test_require_param_or_fallback(void) {
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "vm_id", "web-prod");

    ParamValidationResult r = validate_require_param_or(params, "name", "vm_id");
    g_assert_cmpint(r.error_code, ==, 0);
    g_assert_cmpstr(r.result_value, ==, "web-prod");

    json_object_unref(params);
}

static void test_require_param_or_both_present(void) {
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", "primary-val");
    json_object_set_string_member(params, "vm_id", "fallback-val");

    ParamValidationResult r = validate_require_param_or(params, "name", "vm_id");
    g_assert_cmpint(r.error_code, ==, 0);

    g_assert_cmpstr(r.result_value, ==, "primary-val");

    json_object_unref(params);
}

static void test_require_param_or_neither(void) {
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "other", "irrelevant");

    ParamValidationResult r = validate_require_param_or(params, "name", "vm_id");
    g_assert_cmpint(r.error_code, ==, -32602);
    g_assert_nonnull(strstr(r.error_message, "name"));
    g_assert_nonnull(strstr(r.error_message, "vm_id"));
    g_free(r.error_message);
    json_object_unref(params);
}

static void test_require_param_or_primary_empty(void) {
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", "");
    json_object_set_string_member(params, "vm_id", "fallback-works");

    ParamValidationResult r = validate_require_param_or(params, "name", "vm_id");
    g_assert_cmpint(r.error_code, ==, 0);

    g_assert_cmpstr(r.result_value, ==, "fallback-works");

    json_object_unref(params);
}

static void test_alias_name_to_vm_id(void) {
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", "web-prod");

    apply_bidirectional_alias(params);

    g_assert_true(json_object_has_member(params, "vm_id"));
    g_assert_cmpstr(json_object_get_string_member(params, "vm_id"), ==, "web-prod");

    g_assert_cmpstr(json_object_get_string_member(params, "name"), ==, "web-prod");

    json_object_unref(params);
}

static void test_alias_vm_id_to_name(void) {
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "vm_id", "db-master");

    apply_bidirectional_alias(params);

    g_assert_true(json_object_has_member(params, "name"));
    g_assert_cmpstr(json_object_get_string_member(params, "name"), ==, "db-master");
    g_assert_cmpstr(json_object_get_string_member(params, "vm_id"), ==, "db-master");

    json_object_unref(params);
}

static void test_alias_both_present_no_overwrite(void) {
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", "name-val");
    json_object_set_string_member(params, "vm_id", "vmid-val");

    apply_bidirectional_alias(params);

    g_assert_cmpstr(json_object_get_string_member(params, "name"), ==, "name-val");
    g_assert_cmpstr(json_object_get_string_member(params, "vm_id"), ==, "vmid-val");

    json_object_unref(params);
}

static void test_alias_null_params(void) {

    apply_bidirectional_alias(NULL);
}

static void test_alias_no_vm_keys(void) {
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "other", "value");

    apply_bidirectional_alias(params);

    g_assert_false(json_object_has_member(params, "vm_id"));
    g_assert_false(json_object_has_member(params, "name"));

    json_object_unref(params);
}

static void test_json_get_string_member_missing_returns_null(void) {
    JsonObject *params = json_object_new();

    const gchar *val = json_object_get_string_member(params, "nonexistent");
    g_assert_null(val);

    json_object_unref(params);
}

static void test_json_get_int_member_missing_returns_zero(void) {
    JsonObject *params = json_object_new();

    gint64 val = json_object_get_int_member(params, "memory_mb");
    g_assert_cmpint(val, ==, 0);

    json_object_unref(params);
}

static void test_json_has_member_before_get_pattern(void) {
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "vm_id", "test-vm");
    json_object_set_int_member(params, "vcpu_count", 4);

    g_assert_true(json_object_has_member(params, "vm_id"));
    const gchar *vm_id = json_object_get_string_member(params, "vm_id");
    g_assert_cmpstr(vm_id, ==, "test-vm");

    g_assert_true(json_object_has_member(params, "vcpu_count"));
    gint64 vcpu = json_object_get_int_member(params, "vcpu_count");
    g_assert_cmpint(vcpu, ==, 4);

    g_assert_false(json_object_has_member(params, "memory_mb"));

    json_object_unref(params);
}

static void test_vm_stop_params_valid(void) {

    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "vm_id", "web-prod");

    ParamValidationResult r = validate_require_param(params, "vm_id");
    g_assert_cmpint(r.error_code, ==, 0);
    g_assert_cmpstr(r.result_value, ==, "web-prod");

    json_object_unref(params);
}

static void test_vm_stop_params_missing_vm_id(void) {

    JsonObject *params = json_object_new();

    ParamValidationResult r = validate_require_param(params, "vm_id");
    g_assert_cmpint(r.error_code, ==, -32602);
    g_free(r.error_message);

    json_object_unref(params);
}

static void test_disk_attach_params_valid(void) {

    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "vm_id", "db-01");
    json_object_set_string_member(params, "source", "/dev/zvol/pcvpool/vms/db-01-data");
    json_object_set_string_member(params, "target", "vdb");

    ParamValidationResult r1 = validate_require_param(params, "vm_id");
    ParamValidationResult r2 = validate_require_param(params, "source");
    ParamValidationResult r3 = validate_require_param(params, "target");

    g_assert_cmpint(r1.error_code, ==, 0);
    g_assert_cmpint(r2.error_code, ==, 0);
    g_assert_cmpint(r3.error_code, ==, 0);
    g_assert_cmpstr(r1.result_value, ==, "db-01");
    g_assert_cmpstr(r3.result_value, ==, "vdb");

    json_object_unref(params);
}

static void test_disk_attach_params_partial(void) {

    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "vm_id", "db-01");
    json_object_set_string_member(params, "source", "/dev/zvol/pool/data");

    ParamValidationResult r = validate_require_param(params, "target");
    g_assert_cmpint(r.error_code, ==, -32602);
    g_assert_nonnull(strstr(r.error_message, "target"));
    g_free(r.error_message);

    json_object_unref(params);
}

static void test_snapshot_params_dual_key(void) {

    JsonObject *rest_params = json_object_new();
    json_object_set_string_member(rest_params, "name", "web-prod");
    json_object_set_string_member(rest_params, "snapshot_name", "snap-20260330");

    ParamValidationResult r1 = validate_require_param_or(rest_params, "name", "vm_id");
    ParamValidationResult r2 = validate_require_param_or(rest_params, "snapshot_name", "snap_name");
    g_assert_cmpint(r1.error_code, ==, 0);
    g_assert_cmpint(r2.error_code, ==, 0);
    g_assert_cmpstr(r1.result_value, ==, "web-prod");
    g_assert_cmpstr(r2.result_value, ==, "snap-20260330");
    json_object_unref(rest_params);

    JsonObject *uds_params = json_object_new();
    json_object_set_string_member(uds_params, "vm_id", "web-prod");
    json_object_set_string_member(uds_params, "snap_name", "snap-20260330");

    ParamValidationResult r3 = validate_require_param_or(uds_params, "name", "vm_id");
    ParamValidationResult r4 = validate_require_param_or(uds_params, "snapshot_name", "snap_name");
    g_assert_cmpint(r3.error_code, ==, 0);
    g_assert_cmpint(r4.error_code, ==, 0);
    g_assert_cmpstr(r3.result_value, ==, "web-prod");
    g_assert_cmpstr(r4.result_value, ==, "snap-20260330");
    json_object_unref(uds_params);
}

static void test_set_memory_params_valid(void) {

    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "vm_id", "test-vm");
    json_object_set_int_member(params, "memory_mb", 4096);

    ParamValidationResult r = validate_require_param(params, "vm_id");
    g_assert_cmpint(r.error_code, ==, 0);

    g_assert_true(json_object_has_member(params, "memory_mb"));
    gint64 mem = json_object_get_int_member(params, "memory_mb");
    g_assert_cmpint(mem, ==, 4096);

    json_object_unref(params);
}

static void test_set_memory_params_missing_both(void) {

    JsonObject *params = json_object_new();

    ParamValidationResult r = validate_require_param(params, "vm_id");
    g_assert_cmpint(r.error_code, ==, -32602);
    g_free(r.error_message);

    g_assert_false(json_object_has_member(params, "memory_mb"));

    json_object_unref(params);
}

static guint
node_drain_read_timeout_sec(JsonObject *params)
{
    return json_object_has_member(params, "timeout_sec")
        ? (guint)json_object_get_int_member(params, "timeout_sec") : 30;
}

static void test_node_drain_timeout_default_30(void) {

    JsonObject *params = json_object_new();
    g_assert_cmpuint(node_drain_read_timeout_sec(params), ==, 30);
    json_object_unref(params);
}

static void test_node_drain_timeout_custom_value_applied(void) {

    JsonObject *params = json_object_new();
    json_object_set_int_member(params, "timeout_sec", 90);
    g_assert_cmpuint(node_drain_read_timeout_sec(params), ==, 90);
    json_object_unref(params);
}

static gboolean
disk_attach_bus_is_valid(const gchar *bus)
{
    return g_strcmp0(bus, "virtio") == 0 || g_strcmp0(bus, "scsi") == 0 ||
           g_strcmp0(bus, "sata")   == 0 || g_strcmp0(bus, "ide")  == 0;
}

static void test_disk_attach_bus_default_virtio(void) {
    JsonObject *params = json_object_new();
    const gchar *bus = json_object_has_member(params, "bus")
        ? json_object_get_string_member(params, "bus") : "virtio";
    g_assert_cmpstr(bus, ==, "virtio");
    g_assert_true(disk_attach_bus_is_valid(bus));
    json_object_unref(params);
}

static void test_disk_attach_bus_allowlist_accepts_all_four(void) {
    const gchar *allowed[] = { "virtio", "scsi", "sata", "ide" };
    for (guint i = 0; i < G_N_ELEMENTS(allowed); i++)
        g_assert_true(disk_attach_bus_is_valid(allowed[i]));
}

static void test_disk_attach_bus_allowlist_rejects_injection(void) {

    g_assert_false(disk_attach_bus_is_valid("xen"));
    g_assert_false(disk_attach_bus_is_valid("virtio'/><disk type='file"));
    g_assert_false(disk_attach_bus_is_valid(""));
    g_assert_false(disk_attach_bus_is_valid(NULL));
}

static void test_disk_attach_bus_xml_interpolation_uses_requested_bus(void) {

    const gchar *bus = "scsi";
    g_assert_true(disk_attach_bus_is_valid(bus));
    gchar *xml_payload = g_strdup_printf(
        "<disk type='block' device='disk'>\n"
        "  <driver name='qemu' type='raw' cache='none' io='native'/>\n"
        "  <source dev='%s'/>\n"
        "  <target dev='%s' bus='%s'/>\n"
        "</disk>", "/dev/zvol/pcvpool/data", "vdb", bus);
    g_assert_nonnull(strstr(xml_payload, "bus='scsi'"));
    g_assert_null(strstr(xml_payload, "bus='virtio'"));
    g_free(xml_payload);
}

void test_handler_params_register(void) {

    g_test_add_func("/handler_params/require_param/valid",
                    test_require_param_valid);
    g_test_add_func("/handler_params/require_param/null_params",
                    test_require_param_null_params);
    g_test_add_func("/handler_params/require_param/missing_key",
                    test_require_param_missing_key);
    g_test_add_func("/handler_params/require_param/empty_string",
                    test_require_param_empty_string);
    g_test_add_func("/handler_params/require_param/null_value",
                    test_require_param_null_value);

    g_test_add_func("/handler_params/require_param_or/primary",
                    test_require_param_or_primary);
    g_test_add_func("/handler_params/require_param_or/fallback",
                    test_require_param_or_fallback);
    g_test_add_func("/handler_params/require_param_or/both_present",
                    test_require_param_or_both_present);
    g_test_add_func("/handler_params/require_param_or/neither",
                    test_require_param_or_neither);
    g_test_add_func("/handler_params/require_param_or/primary_empty",
                    test_require_param_or_primary_empty);

    g_test_add_func("/handler_params/alias/name_to_vm_id",
                    test_alias_name_to_vm_id);
    g_test_add_func("/handler_params/alias/vm_id_to_name",
                    test_alias_vm_id_to_name);
    g_test_add_func("/handler_params/alias/both_no_overwrite",
                    test_alias_both_present_no_overwrite);
    g_test_add_func("/handler_params/alias/null_params",
                    test_alias_null_params);
    g_test_add_func("/handler_params/alias/no_vm_keys",
                    test_alias_no_vm_keys);

    g_test_add_func("/handler_params/json/get_string_missing",
                    test_json_get_string_member_missing_returns_null);
    g_test_add_func("/handler_params/json/get_int_missing",
                    test_json_get_int_member_missing_returns_zero);
    g_test_add_func("/handler_params/json/has_before_get",
                    test_json_has_member_before_get_pattern);

    g_test_add_func("/handler_params/scenario/vm_stop_valid",
                    test_vm_stop_params_valid);
    g_test_add_func("/handler_params/scenario/vm_stop_missing",
                    test_vm_stop_params_missing_vm_id);
    g_test_add_func("/handler_params/scenario/disk_attach_valid",
                    test_disk_attach_params_valid);
    g_test_add_func("/handler_params/scenario/disk_attach_partial",
                    test_disk_attach_params_partial);
    g_test_add_func("/handler_params/scenario/snapshot_dual_key",
                    test_snapshot_params_dual_key);
    g_test_add_func("/handler_params/scenario/set_memory_valid",
                    test_set_memory_params_valid);
    g_test_add_func("/handler_params/scenario/set_memory_missing",
                    test_set_memory_params_missing_both);

    g_test_add_func("/handler_params/node_drain/timeout_default_30",
                    test_node_drain_timeout_default_30);
    g_test_add_func("/handler_params/node_drain/timeout_custom_applied",
                    test_node_drain_timeout_custom_value_applied);
    g_test_add_func("/handler_params/disk_attach/bus_default_virtio",
                    test_disk_attach_bus_default_virtio);
    g_test_add_func("/handler_params/disk_attach/bus_allowlist_accepts_all",
                    test_disk_attach_bus_allowlist_accepts_all_four);
    g_test_add_func("/handler_params/disk_attach/bus_allowlist_rejects_injection",
                    test_disk_attach_bus_allowlist_rejects_injection);
    g_test_add_func("/handler_params/disk_attach/bus_xml_interpolation",
                    test_disk_attach_bus_xml_interpolation_uses_requested_bus);
}
