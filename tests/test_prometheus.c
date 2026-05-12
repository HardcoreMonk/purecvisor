










































#include <glib.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include "modules/daemons/ebpf_telemetry.h"
#include "modules/daemons/prometheus_exporter.h"

#ifndef PCV_PROM_MAX_METRICS
#define PCV_PROM_MAX_METRICS 2048
#endif

#define PCV_PROM_LARGE_HOST_MIN_SLOTS ((256 * 8) + 512)
















typedef struct {
    const gchar *name;
    const gchar *labels;
    gdouble      value;
} CounterEntry;






static gchar *
_serialize_counters(const CounterEntry *entries, guint n_entries)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "saved_at");
    json_builder_add_int_value(b, (gint64)1713916800);
    json_builder_set_member_name(b, "counters");
    json_builder_begin_array(b);
    for (guint i = 0; i < n_entries; i++) {
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "name");
        json_builder_add_string_value(b, entries[i].name);
        json_builder_set_member_name(b, "labels");
        json_builder_add_string_value(b, entries[i].labels);
        json_builder_set_member_name(b, "value");
        json_builder_add_double_value(b, entries[i].value);
        json_builder_end_object(b);
    }
    json_builder_end_array(b);
    json_builder_end_object(b);

    JsonNode *root = json_builder_get_root(b);
    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, root);
    gchar *data = json_generator_to_data(gen, NULL);
    json_node_free(root);
    g_object_unref(gen);
    g_object_unref(b);
    return data;
}







typedef struct {
    gchar   name[128];
    gchar   labels[128];
    gdouble value;
} RestoredCounter;

static GArray *
_deserialize_counters(const gchar *json_str)
{
    if (!json_str) return NULL;

    JsonParser *jp = json_parser_new();
    GError *err = NULL;
    if (!json_parser_load_from_data(jp, json_str, -1, &err)) {
        g_clear_error(&err);
        g_object_unref(jp);
        return NULL;
    }

    JsonNode *root_n = json_parser_get_root(jp);

    if (!root_n || !JSON_NODE_HOLDS_OBJECT(root_n)) {
        g_object_unref(jp);
        return NULL;
    }

    JsonObject *root = json_node_get_object(root_n);
    if (!json_object_has_member(root, "counters")) {
        g_object_unref(jp);
        return NULL;
    }

    JsonArray *arr = json_object_get_array_member(root, "counters");
    if (!arr) {
        g_object_unref(jp);
        return NULL;
    }

    GArray *result = g_array_new(FALSE, TRUE, sizeof(RestoredCounter));
    guint n = json_array_get_length(arr);
    for (guint i = 0; i < n; i++) {
        JsonObject *o = json_array_get_object_element(arr, i);
        if (!o) continue;
        const gchar *name   = json_object_get_string_member(o, "name");
        const gchar *labels = json_object_get_string_member(o, "labels");
        gdouble      value  = json_object_get_double_member(o, "value");
        if (!name || !labels) continue;
        RestoredCounter rc = {0};
        g_strlcpy(rc.name,   name,   sizeof(rc.name));
        g_strlcpy(rc.labels, labels, sizeof(rc.labels));
        rc.value = value;
        g_array_append_val(result, rc);
    }

    g_object_unref(jp);
    return result;
}




static void test_checkpoint_roundtrip_multiple(void) {
    const CounterEntry entries[] = {
        { "purecvisor_rpc_requests_total", "method=\"vm.list\",status=\"ok\"",  42.0 },
        { "purecvisor_rpc_requests_total", "method=\"vm.start\",status=\"ok\"", 7.0  },
        { "purecvisor_rpc_errors_total",   "method=\"vm.delete\"",              3.0  },
    };
    const guint n = G_N_ELEMENTS(entries);

    gchar *json_str = _serialize_counters(entries, n);
    g_assert_nonnull(json_str);

    GArray *restored = _deserialize_counters(json_str);
    g_assert_nonnull(restored);
    g_assert_cmpuint(restored->len, ==, n);

    for (guint i = 0; i < n; i++) {
        RestoredCounter *rc = &g_array_index(restored, RestoredCounter, i);
        g_assert_cmpstr(rc->name,   ==, entries[i].name);
        g_assert_cmpstr(rc->labels, ==, entries[i].labels);
        g_assert_cmpfloat_with_epsilon(rc->value, entries[i].value, 1e-9);
    }

    g_array_free(restored, TRUE);
    g_free(json_str);
}


static void test_checkpoint_roundtrip_empty(void) {
    gchar *json_str = _serialize_counters(NULL, 0);
    g_assert_nonnull(json_str);

    GArray *restored = _deserialize_counters(json_str);
    g_assert_nonnull(restored);
    g_assert_cmpuint(restored->len, ==, 0);

    g_array_free(restored, TRUE);
    g_free(json_str);
}


static void test_checkpoint_corrupted_non_object_root(void) {

    const gchar *bad_json = "[{\"name\":\"x\",\"labels\":\"\",\"value\":1}]";
    GArray *result = _deserialize_counters(bad_json);

    g_assert_null(result);
    if (result) g_array_free(result, TRUE);
}




static void test_checkpoint_corrupted_array_root(void) {
    const gchar *json_str = "[]";
    GArray *result = _deserialize_counters(json_str);
    g_assert_null(result);
    if (result) g_array_free(result, TRUE);
}


static void test_checkpoint_corrupted_null_root(void) {
    const gchar *json_str = "null";
    GArray *result = _deserialize_counters(json_str);
    g_assert_null(result);
    if (result) g_array_free(result, TRUE);
}


static void test_checkpoint_corrupted_truncated(void) {
    const gchar *json_str = "{\"counters\":{";
    GArray *result = _deserialize_counters(json_str);

    g_assert_null(result);
    if (result) g_array_free(result, TRUE);
}







typedef struct {
    gchar    name[128];
    gboolean is_counter;
} MetricReg;

#define MAX_TEST_METRICS 32

typedef struct {
    MetricReg entries[MAX_TEST_METRICS];
    gint      count;
} TestRegistry;







static gboolean
_registry_register(TestRegistry *reg, const gchar *name, gboolean is_counter)
{
    if (!name || name[0] == '\0') return FALSE;
    for (gint i = 0; i < reg->count; i++) {
        if (g_strcmp0(reg->entries[i].name, name) == 0) {

            return reg->entries[i].is_counter == is_counter;
        }
    }
    if (reg->count >= MAX_TEST_METRICS) return FALSE;
    gint idx = reg->count++;
    g_strlcpy(reg->entries[idx].name, name, sizeof(reg->entries[idx].name));
    reg->entries[idx].is_counter = is_counter;
    return TRUE;
}


static void test_type_mismatch_counter_then_gauge(void) {
    TestRegistry reg = {0};

    g_assert_true(_registry_register(&reg, "rpc_total", TRUE));

    g_assert_false(_registry_register(&reg, "rpc_total", FALSE));
}


static void test_type_same_reregister(void) {
    TestRegistry reg = {0};
    g_assert_true(_registry_register(&reg, "rpc_total", TRUE));

    g_assert_true(_registry_register(&reg, "rpc_total", TRUE));

    g_assert_cmpint(reg.count, ==, 1);
}


static void test_registry_capacity_large_host_budget(void) {
    g_assert_cmpint(PCV_PROM_MAX_METRICS, >=, PCV_PROM_LARGE_HOST_MIN_SLOTS);
}


static void test_node_cpu_parser_skips_aggregate_line(void) {
    g_assert_false(pcv_ebpf_proc_stat_is_cpu_core_line(
        "cpu  3726 0 556809 739 0 7 0 0 0 0"));
    g_assert_true(pcv_ebpf_proc_stat_is_cpu_core_line(
        "cpu0 34 0 101 355351 8 0 0 0 0 0"));
}











static gboolean
_validate_metric_name(const gchar *name)
{
    if (!name || name[0] == '\0') return FALSE;


    if (!g_ascii_isalpha(name[0]) && name[0] != '_' && name[0] != ':')
        return FALSE;


    for (gsize i = 1; name[i] != '\0'; i++) {
        if (!g_ascii_isalnum(name[i]) && name[i] != '_' && name[i] != ':')
            return FALSE;
    }
    return TRUE;
}


static void test_metric_name_valid(void) {
    g_assert_true(_validate_metric_name("rpc_total"));
    g_assert_true(_validate_metric_name("purecvisor_rpc_requests_total"));
    g_assert_true(_validate_metric_name("node_cpu_seconds_total"));
    g_assert_true(_validate_metric_name("_private_metric"));
    g_assert_true(_validate_metric_name(":colon_prefix"));
    g_assert_true(_validate_metric_name("CamelCase123"));
}


static void test_metric_name_invalid(void) {

    g_assert_false(_validate_metric_name("9rpc_total"));
    g_assert_false(_validate_metric_name("0counter"));

    g_assert_false(_validate_metric_name(""));
    g_assert_false(_validate_metric_name(NULL));

    g_assert_false(_validate_metric_name("rpc total"));
    g_assert_false(_validate_metric_name("rpc-total"));
    g_assert_false(_validate_metric_name("rpc.total"));
}


















static gboolean
_validate_label_format(const gchar *labels)
{
    if (!labels) return FALSE;
    if (labels[0] == '\0') return TRUE;

    const gchar *p = labels;
    while (*p != '\0') {
        if (!g_ascii_isalpha(*p) && *p != '_') return FALSE;
        p++;

        while (g_ascii_isalnum(*p) || *p == '_') p++;
        if (p[0] != '=' || p[1] != '"') return FALSE;
        p += 2;

        while (*p != '\0') {
            if (*p == '\\' && p[1] != '\0') {
                p += 2;
                continue;
            }
            if (*p == '"') break;
            p++;
        }
        if (*p != '"') return FALSE;
        p++;

        if (*p == '\0') return TRUE;
        if (*p != ',') return FALSE;
        p++;
        if (*p == '\0') return FALSE;
    }

    return TRUE;
}


static void test_label_format_valid(void) {

    g_assert_true(_validate_label_format(""));

    g_assert_true(_validate_label_format("method=\"vm.list\""));

    g_assert_true(_validate_label_format(
        "method=\"vm.list\",status=\"ok\""));

    g_assert_true(_validate_label_format(
        "version=\"1.0 beta\""));

    g_assert_true(_validate_label_format(
        "policy=\"vm-unresponsive\""));
}


static void test_label_format_invalid(void) {

    g_assert_false(_validate_label_format("method=\"vm.list"));
    g_assert_false(_validate_label_format(
        "method=\"vm.list\",status=\"ok"));

    g_assert_false(_validate_label_format("vm-unresponsive"));
    g_assert_false(_validate_label_format("policy"));

    g_assert_false(_validate_label_format("policy-name=\"vm-unresponsive\""));

    g_assert_false(_validate_label_format(NULL));
}




void test_prometheus_register(void) {

    g_test_add_func("/prometheus/checkpoint/roundtrip_multiple",
                    test_checkpoint_roundtrip_multiple);
    g_test_add_func("/prometheus/checkpoint/roundtrip_empty",
                    test_checkpoint_roundtrip_empty);
    g_test_add_func("/prometheus/checkpoint/corrupted_non_object_root",
                    test_checkpoint_corrupted_non_object_root);


    g_test_add_func("/prometheus/checkpoint/corrupted_array_root",
                    test_checkpoint_corrupted_array_root);
    g_test_add_func("/prometheus/checkpoint/corrupted_null_root",
                    test_checkpoint_corrupted_null_root);
    g_test_add_func("/prometheus/checkpoint/corrupted_truncated",
                    test_checkpoint_corrupted_truncated);


    g_test_add_func("/prometheus/registry/type_mismatch_counter_then_gauge",
                    test_type_mismatch_counter_then_gauge);
    g_test_add_func("/prometheus/registry/type_same_reregister",
                    test_type_same_reregister);
    g_test_add_func("/prometheus/registry/capacity_large_host_budget",
                    test_registry_capacity_large_host_budget);
    g_test_add_func("/prometheus/node_cpu_parser/skips_aggregate_line",
                    test_node_cpu_parser_skips_aggregate_line);


    g_test_add_func("/prometheus/metric_name/valid",
                    test_metric_name_valid);
    g_test_add_func("/prometheus/metric_name/invalid",
                    test_metric_name_invalid);


    g_test_add_func("/prometheus/label_format/valid",
                    test_label_format_valid);
    g_test_add_func("/prometheus/label_format/invalid",
                    test_label_format_invalid);
}
