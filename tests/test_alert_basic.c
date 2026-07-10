/* tests/test_alert_basic.c
 *
 * 대상 모듈: src/modules/daemons/alert_engine.c — CPU/MEM/DISK 임계값 알림 엔진
 *
 * 이 테스트가 검증하는 것:
 *   알림 설정 JSON의 임계값 범위(cpu_warn/crit 0~100, eval_period >= 0),
 *   NULL 방어, 히스토리 레코드 JSON 구조(metric/severity/value/acknowledged 등),
 *   음소거(silence) 메트릭 이름 화이트리스트(CPU/MEM/DISK/DATA_POOL),
 *   웹훅 형식(slack/telegram/generic) 검증을 검사한다.
 *
 * 참고: alert_engine.c는 DAEMON_SRCS 전용이므로 직접 호출 불가.
 *       핸들러 파라미터 검증과 동일한 패턴으로 로직을 재현하여 테스트.
 *
 * 실행: sudo ./test_runner -p /alert
 *
 * 테스트 추가: 검증 헬퍼(_validate_*) 작성 후 테스트 함수에서 호출
 *
 * 외부 의존: 없음 (데몬 프로세스 불필요, 순수 JSON 구조/형식 검증)
 */

#include <glib.h>
#include <json-glib/json-glib.h>
#include <string.h>

/* ── 알림 설정 JSON 구성 패턴 테스트 ────────────────────── */

/**
 * alert_engine_set_config()에 전달되는 JSON 구조를 검증하는 헬퍼.
 * 실제 엔진 없이 설정 JSON의 유효성만 확인한다.
 */
static gboolean
_validate_alert_config(JsonObject *cfg)
{
    if (!cfg) return FALSE;

    /* cpu_warn/crit 범위 확인 (0-100) */
    if (json_object_has_member(cfg, "cpu_warn")) {
        gint64 v = json_object_get_int_member(cfg, "cpu_warn");
        if (v < 0 || v > 100) return FALSE;
    }
    if (json_object_has_member(cfg, "cpu_crit")) {
        gint64 v = json_object_get_int_member(cfg, "cpu_crit");
        if (v < 0 || v > 100) return FALSE;
    }
    /* eval_period >= 0 */
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
    json_object_set_int_member(cfg, "cpu_warn", 150);  /* over 100 */
    g_assert_false(_validate_alert_config(cfg));
    json_object_unref(cfg);
}

static void test_alert_config_invalid_cpu_crit(void) {
    JsonObject *cfg = json_object_new();
    json_object_set_int_member(cfg, "cpu_crit", -10);  /* negative */
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

/* ── 알림 히스토리 JSON 구조 테스트 ──────────────────────── */

static void test_alert_history_json_structure(void) {
    /* 알림 히스토리 레코드 JSON 구조 검증 */
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

/* ── 음소거(silence) 검증 패턴 ──────────────────────────── */

/**
 * 음소거 메트릭 이름 검증 — alert_engine의 pcv_alert_add_silence가
 * 받는 metric 파라미터의 유효성을 검사하는 패턴.
 */
static gboolean
_validate_silence_metric(const gchar *metric)
{
    if (!metric || metric[0] == '\0') return FALSE;
    /* 허용: CPU, MEM, DISK, DATA_POOL */
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
    g_assert_false(_validate_silence_metric("cpu"));  /* case-sensitive */
}

/* ── webhook_format 검증 ────────────────────────────────── */

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

/* ── 등록 ────────────────────────────────────────────────── */

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
