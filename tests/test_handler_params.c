/**
 * @file test_handler_params.c
 * @brief RPC 핸들러 파라미터 검증 유닛 테스트
 *
 * 대상 모듈: src/modules/dispatcher/ 핸들러들의 공통 파라미터 검증 패턴
 *
 * 이 테스트가 검증하는 것:
 *   PCV_REQUIRE_PARAM / PCV_REQUIRE_PARAM_OR 매크로 로직을 재현하여
 *   필수 파라미터 누락/빈값/NULL 시 -32602 에러 반환을 검사한다.
 *   dispatcher.c의 name↔vm_id 양방향 앨리어싱, REST/UDS 이중 키 시나리오,
 *   json_object_get_*_member 안전 패턴도 포함.
 *
 * 실행: sudo ./test_runner -p /handler_params
 *
 * 테스트 추가:
 *   - 섹션 1~5에 맞는 위치에 함수 작성 후 등록 함수에 추가
 *   - 새로운 핸들러 패턴은 섹션 5(복합 시나리오)에 추가 권장
 *
 * 외부 의존: 없음 (UDS/libvirt 불필요, 순수 JSON 파싱 로직)
 */

#include <glib.h>
#include <json-glib/json-glib.h>
#include <string.h>

/* ── 테스트용 간이 검증 함수 (매크로 로직 재현) ───────────────── */

typedef struct {
    gint   error_code;
    gchar *error_message;
    const gchar *result_value;
} ParamValidationResult;

/**
 * 단일 키 필수 파라미터 검증 (PCV_REQUIRE_PARAM 매크로 로직)
 */
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

/**
 * 이중 키 필수 파라미터 검증 (PCV_REQUIRE_PARAM_OR 매크로 로직)
 */
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

/* ── 파라미터 앨리어싱 검증 (dispatcher.c 양방향 로직) ──────── */

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

/* ══════════════════════════════════════════════════════════════
 * 1. PCV_REQUIRE_PARAM 테스트
 * ══════════════════════════════════════════════════════════════ */

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

/* ══════════════════════════════════════════════════════════════
 * 2. PCV_REQUIRE_PARAM_OR 테스트
 * ══════════════════════════════════════════════════════════════ */

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
    /* primary 키 우선 */
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
    /* primary 빈 문자열 → fallback 사용 */
    g_assert_cmpstr(r.result_value, ==, "fallback-works");

    json_object_unref(params);
}

/* ══════════════════════════════════════════════════════════════
 * 3. 양방향 앨리어싱 테스트 (dispatcher.c 로직)
 * ══════════════════════════════════════════════════════════════ */

static void test_alias_name_to_vm_id(void) {
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", "web-prod");

    apply_bidirectional_alias(params);

    g_assert_true(json_object_has_member(params, "vm_id"));
    g_assert_cmpstr(json_object_get_string_member(params, "vm_id"), ==, "web-prod");
    /* 원래 name도 유지 */
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

    /* 둘 다 있으면 덮어쓰지 않음 */
    g_assert_cmpstr(json_object_get_string_member(params, "name"), ==, "name-val");
    g_assert_cmpstr(json_object_get_string_member(params, "vm_id"), ==, "vmid-val");

    json_object_unref(params);
}

static void test_alias_null_params(void) {
    /* NULL 전달 시 크래시 없이 정상 통과 */
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

/* ══════════════════════════════════════════════════════════════
 * 4. 핸들러 파라미터 파싱 패턴 테스트
 *    실제 핸들러가 사용하는 json_object_get_*_member 패턴 검증
 * ══════════════════════════════════════════════════════════════ */

static void test_json_get_string_member_missing_returns_null(void) {
    JsonObject *params = json_object_new();

    /* 존재하지 않는 키를 직접 get하면 NULL 반환 */
    const gchar *val = json_object_get_string_member(params, "nonexistent");
    g_assert_null(val);

    json_object_unref(params);
}

static void test_json_get_int_member_missing_returns_zero(void) {
    JsonObject *params = json_object_new();

    /* 존재하지 않는 int 키 → 0 반환 (has_member 체크 필수) */
    gint64 val = json_object_get_int_member(params, "memory_mb");
    g_assert_cmpint(val, ==, 0);

    json_object_unref(params);
}

static void test_json_has_member_before_get_pattern(void) {
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "vm_id", "test-vm");
    json_object_set_int_member(params, "vcpu_count", 4);

    /* 안전한 패턴: has_member → get */
    g_assert_true(json_object_has_member(params, "vm_id"));
    const gchar *vm_id = json_object_get_string_member(params, "vm_id");
    g_assert_cmpstr(vm_id, ==, "test-vm");

    g_assert_true(json_object_has_member(params, "vcpu_count"));
    gint64 vcpu = json_object_get_int_member(params, "vcpu_count");
    g_assert_cmpint(vcpu, ==, 4);

    /* 없는 키 → has_member가 FALSE */
    g_assert_false(json_object_has_member(params, "memory_mb"));

    json_object_unref(params);
}

/* ══════════════════════════════════════════════════════════════
 * 5. 복합 핸들러 시나리오 테스트
 * ══════════════════════════════════════════════════════════════ */

static void test_vm_stop_params_valid(void) {
    /* vm.stop 핸들러 파라미터 패턴 재현 */
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "vm_id", "web-prod");

    ParamValidationResult r = validate_require_param(params, "vm_id");
    g_assert_cmpint(r.error_code, ==, 0);
    g_assert_cmpstr(r.result_value, ==, "web-prod");

    json_object_unref(params);
}

static void test_vm_stop_params_missing_vm_id(void) {
    /* vm.stop에 vm_id 없이 호출 */
    JsonObject *params = json_object_new();

    ParamValidationResult r = validate_require_param(params, "vm_id");
    g_assert_cmpint(r.error_code, ==, -32602);
    g_free(r.error_message);

    json_object_unref(params);
}

static void test_disk_attach_params_valid(void) {
    /* device.disk.attach 핸들러 다중 파라미터 패턴 */
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
    /* device.disk.attach에 target 누락 */
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
    /* vm.snapshot.create — REST 레이어는 "name"/"snapshot_name",
     * UDS 레이어는 "vm_id"/"snap_name" 사용 */

    /* REST 스타일 */
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

    /* UDS 스타일 */
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
    /* vm.set_memory — vm_id + memory_mb (int) */
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
    /* vm.set_memory — vm_id + memory_mb 둘 다 누락 */
    JsonObject *params = json_object_new();

    ParamValidationResult r = validate_require_param(params, "vm_id");
    g_assert_cmpint(r.error_code, ==, -32602);
    g_free(r.error_message);

    g_assert_false(json_object_has_member(params, "memory_mb"));

    json_object_unref(params);
}

/* ══════════════════════════════════════════════════════════════
 * 등록
 * ══════════════════════════════════════════════════════════════ */

void test_handler_params_register(void) {
    /* PCV_REQUIRE_PARAM */
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

    /* PCV_REQUIRE_PARAM_OR */
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

    /* 양방향 앨리어싱 */
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

    /* JSON 파싱 패턴 */
    g_test_add_func("/handler_params/json/get_string_missing",
                    test_json_get_string_member_missing_returns_null);
    g_test_add_func("/handler_params/json/get_int_missing",
                    test_json_get_int_member_missing_returns_zero);
    g_test_add_func("/handler_params/json/has_before_get",
                    test_json_has_member_before_get_pattern);

    /* 복합 핸들러 시나리오 */
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
}
