
#include <glib.h>
#include <json-glib/json-glib.h>
#include <string.h>

static gboolean
_validate_alert_config(JsonObject *cfg)
{
    if (!cfg) return FALSE;

    if (json_object_has_member(cfg, "cpu_warn")) {
        gint64 v = json_object_get_int_member(cfg, "cpu_warn");
        if (v < 0 || v > 100) return FALSE;
    }
    if (json_object_has_member(cfg, "cpu_crit")) {
        gint64 v = json_object_get_int_member(cfg, "cpu_crit");
        if (v < 0 || v > 100) return FALSE;
    }

    if (json_object_has_member(cfg, "eval_period")) {
        gint64 v = json_object_get_int_member(cfg, "eval_period");
        if (v < 0) return FALSE;
    }
    return TRUE;
}

static void test_alert_config_valid(void) {
    JsonObject *cfg = json_object_new();
    json_object_set_int_member(cfg, "cpu_warn", 80);
    json_object_set_int_member(cfg, "cpu_crit", 95);
    json_object_set_int_member(cfg, "eval_period", 30);
    g_assert_true(_validate_alert_config(cfg));
    json_object_unref(cfg);
}

static void test_alert_config_null(void) {
    g_assert_false(_validate_alert_config(NULL));
}

static void test_alert_config_invalid_cpu_warn(void) {
    JsonObject *cfg = json_object_new();
    json_object_set_int_member(cfg, "cpu_warn", 150);
    g_assert_false(_validate_alert_config(cfg));
    json_object_unref(cfg);
}

static void test_alert_config_invalid_cpu_crit(void) {
    JsonObject *cfg = json_object_new();
    json_object_set_int_member(cfg, "cpu_crit", -10);
    g_assert_false(_validate_alert_config(cfg));
    json_object_unref(cfg);
}

static void test_alert_config_invalid_eval_period(void) {
    JsonObject *cfg = json_object_new();
    json_object_set_int_member(cfg, "eval_period", -5);
    g_assert_false(_validate_alert_config(cfg));
    json_object_unref(cfg);
}

static void test_alert_config_boundary_zero(void) {
    JsonObject *cfg = json_object_new();
    json_object_set_int_member(cfg, "cpu_warn", 0);
    json_object_set_int_member(cfg, "cpu_crit", 0);
    json_object_set_int_member(cfg, "eval_period", 0);
    g_assert_true(_validate_alert_config(cfg));
    json_object_unref(cfg);
}

static void test_alert_config_boundary_max(void) {
    JsonObject *cfg = json_object_new();
    json_object_set_int_member(cfg, "cpu_warn", 100);
    json_object_set_int_member(cfg, "cpu_crit", 100);
    g_assert_true(_validate_alert_config(cfg));
    json_object_unref(cfg);
}

static void test_alert_history_json_structure(void) {

    JsonObject *record = json_object_new();
    json_object_set_string_member(record, "metric", "CPU");
    json_object_set_string_member(record, "severity", "warn");
    json_object_set_double_member(record, "value", 85.2);
    json_object_set_int_member(record, "timestamp", 1711234567);
    json_object_set_string_member(record, "message", "CPU 85.2% exceeds threshold 80%");
    json_object_set_int_member(record, "alert_id", 1);
    json_object_set_boolean_member(record, "acknowledged", FALSE);
    json_object_set_boolean_member(record, "escalated", FALSE);

    g_assert_true(json_object_has_member(record, "metric"));
    g_assert_true(json_object_has_member(record, "severity"));
    g_assert_true(json_object_has_member(record, "value"));
    g_assert_true(json_object_has_member(record, "timestamp"));
    g_assert_true(json_object_has_member(record, "alert_id"));
    g_assert_true(json_object_has_member(record, "acknowledged"));
    g_assert_cmpstr(json_object_get_string_member(record, "metric"), ==, "CPU");
    g_assert_cmpstr(json_object_get_string_member(record, "severity"), ==, "warn");
    g_assert_cmpfloat(json_object_get_double_member(record, "value"), >, 0.0);

    json_object_unref(record);
}

static gboolean
_validate_silence_metric(const gchar *metric)
{
    if (!metric || metric[0] == '\0') return FALSE;

    return (g_strcmp0(metric, "CPU") == 0 ||
            g_strcmp0(metric, "MEM") == 0 ||
            g_strcmp0(metric, "DISK") == 0 ||
            g_strcmp0(metric, "DATA_POOL") == 0);
}

static void test_alert_silence_metric_valid(void) {
    g_assert_true(_validate_silence_metric("CPU"));
    g_assert_true(_validate_silence_metric("MEM"));
    g_assert_true(_validate_silence_metric("DISK"));
    g_assert_true(_validate_silence_metric("DATA_POOL"));
}

static void test_alert_silence_metric_invalid(void) {
    g_assert_false(_validate_silence_metric(NULL));
    g_assert_false(_validate_silence_metric(""));
    g_assert_false(_validate_silence_metric("NETWORK"));
    g_assert_false(_validate_silence_metric("cpu"));
}

static gboolean
_validate_webhook_format(const gchar *fmt)
{
    if (!fmt || fmt[0] == '\0') return FALSE;
    return (g_strcmp0(fmt, "slack") == 0 ||
            g_strcmp0(fmt, "telegram") == 0 ||
            g_strcmp0(fmt, "generic") == 0);
}

static void test_alert_webhook_format_valid(void) {
    g_assert_true(_validate_webhook_format("slack"));
    g_assert_true(_validate_webhook_format("telegram"));
    g_assert_true(_validate_webhook_format("generic"));
}

static void test_alert_webhook_format_invalid(void) {
    g_assert_false(_validate_webhook_format(NULL));
    g_assert_false(_validate_webhook_format(""));
    g_assert_false(_validate_webhook_format("email"));
    g_assert_false(_validate_webhook_format("pagerduty"));
}

void test_alert_basic_register(void) {
    g_test_add_func("/alert/config/valid",                test_alert_config_valid);
    g_test_add_func("/alert/config/null",                 test_alert_config_null);
    g_test_add_func("/alert/config/invalid_cpu_warn",     test_alert_config_invalid_cpu_warn);
    g_test_add_func("/alert/config/invalid_cpu_crit",     test_alert_config_invalid_cpu_crit);
    g_test_add_func("/alert/config/invalid_eval_period",  test_alert_config_invalid_eval_period);
    g_test_add_func("/alert/config/boundary_zero",        test_alert_config_boundary_zero);
    g_test_add_func("/alert/config/boundary_max",         test_alert_config_boundary_max);
    g_test_add_func("/alert/history/json_structure",      test_alert_history_json_structure);
    g_test_add_func("/alert/silence/metric_valid",        test_alert_silence_metric_valid);
    g_test_add_func("/alert/silence/metric_invalid",      test_alert_silence_metric_invalid);
    g_test_add_func("/alert/webhook/format_valid",        test_alert_webhook_format_valid);
    g_test_add_func("/alert/webhook/format_invalid",      test_alert_webhook_format_invalid);
}
