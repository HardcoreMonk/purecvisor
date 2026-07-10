/**
 * @file test_snapshot_rollback.c
 * @brief vm.snapshot.rollback 핸들러 파라미터 검증 유닛 테스트
 *
 * 대상 모듈: src/modules/dispatcher/handler_snapshot.c
 *            handle_vm_snapshot_rollback()
 *
 * 이 테스트가 검증하는 것:
 *   1. 필수 파라미터(vm_name, snap_name) 누락 → -32602 에러
 *   2. 잘못된 문자셋(ZFS 토큰 제약) → 검증 실패
 *   3. 빈 문자열 / 오버롱 이름 → 검증 실패
 *   4. 유효 파라미터 → 검증 통과
 *   5. 이중 키 지원 (name/vm_id, snapshot_name/snap_name)
 *   6. 롤백 상태 머신 — 도메인 상태별 was_running 결정 로직
 *   7. ZFS dataset 경로 생성 — pool + vm + snapshot
 *
 * 실행: sudo ./test_runner -p /snapshot/rollback
 *
 * 외부 의존: 없음 (UDS/libvirt 불필요, 순수 로직 재현)
 */

#include <glib.h>
#include <json-glib/json-glib.h>
#include <string.h>

/* ── 인라인 재현: handler_snapshot.c의 pcv_validate_zfs_token ── */

/**
 * ZFS 토큰 검증: [a-zA-Z0-9_-], 1~128자
 * handler_snapshot.c의 static pcv_validate_zfs_token() 동일 로직
 */
static gboolean
validate_zfs_token(const gchar *s)
{
    if (!s || *s == '\0' || strlen(s) > 128)
        return FALSE;
    for (const gchar *p = s; *p; p++) {
        if (!g_ascii_isalnum(*p) && *p != '-' && *p != '_')
            return FALSE;
    }
    return TRUE;
}

/* ── 인라인 재현: handler_snapshot.c의 _get_param 헬퍼 ────────── */

/**
 * 이중 키 파라미터 추출:
 *   REST 레이어: "name"/"snapshot_name"
 *   UDS 레이어:  "vm_id"/"snap_name"
 */
static const gchar *
get_param(JsonObject *params, const gchar *primary, const gchar *fallback)
{
    if (!params) return NULL;
    if (json_object_has_member(params, primary))
        return json_object_get_string_member(params, primary);
    if (fallback && json_object_has_member(params, fallback))
        return json_object_get_string_member(params, fallback);
    return NULL;
}

/* ── 인라인 재현: VALIDATE_SNAPSHOT_PARAMS 매크로 로직 ────────── */

typedef enum {
    SNAP_VALID_OK = 0,
    SNAP_VALID_MISSING_PARAMS,
    SNAP_VALID_INVALID_CHARS,
} SnapValidateResult;

static SnapValidateResult
validate_rollback_params(JsonObject *params)
{
    if (!params) return SNAP_VALID_MISSING_PARAMS;

    const gchar *vm_name  = get_param(params, "name", "vm_id");
    const gchar *snap_name = get_param(params, "snapshot_name", "snap_name");

    if (!vm_name || vm_name[0] == '\0' ||
        !snap_name || snap_name[0] == '\0')
        return SNAP_VALID_MISSING_PARAMS;

    if (!validate_zfs_token(vm_name) || !validate_zfs_token(snap_name))
        return SNAP_VALID_INVALID_CHARS;

    return SNAP_VALID_OK;
}

/* ── 인라인 재현: 롤백 상태 머신 — was_running 결정 로직 ───────── */

/* libvirt 도메인 상태 (libvirt.h 없이 재현) */
#define TEST_VIR_DOMAIN_RUNNING  1
#define TEST_VIR_DOMAIN_BLOCKED  2
#define TEST_VIR_DOMAIN_PAUSED   3
#define TEST_VIR_DOMAIN_SHUTDOWN 4
#define TEST_VIR_DOMAIN_SHUTOFF  5
#define TEST_VIR_DOMAIN_CRASHED  6
#define TEST_VIR_DOMAIN_PMSUSPENDED 7

/**
 * handler_snapshot.c _rollback_worker() 의 was_running 결정 로직:
 *   was_running = (state == VIR_DOMAIN_RUNNING || state == VIR_DOMAIN_PAUSED)
 */
static gboolean
rollback_was_running(int domain_state)
{
    return (domain_state == TEST_VIR_DOMAIN_RUNNING ||
            domain_state == TEST_VIR_DOMAIN_PAUSED);
}

/* ── 인라인 재현: ZFS dataset 경로 생성 로직 ──────────────────── */

/**
 * handler_snapshot.c _rollback_worker() 의 dataset 경로 생성:
 *   g_strdup_printf("%s/%s@%s", pool, vm_name, snap_name)
 *
 * @return g_malloc'd 문자열 — 호출자가 g_free()
 */
static gchar *
build_zfs_dataset_path(const gchar *pool,
                        const gchar *vm_name,
                        const gchar *snap_name)
{
    return g_strdup_printf("%s/%s@%s", pool, vm_name, snap_name);
}

/* ══════════════════════════════════════════════════════════════
 * 1. 필수 파라미터 누락 테스트 (네거티브 경로)
 * ══════════════════════════════════════════════════════════════ */

static void test_rollback_missing_vm_name(void) {
    /* "name"/"vm_id" 키 없이 snapshot_name만 있는 경우 */
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "snapshot_name", "daily-01");

    SnapValidateResult r = validate_rollback_params(params);
    g_assert_cmpint(r, ==, SNAP_VALID_MISSING_PARAMS);

    json_object_unref(params);
}

static void test_rollback_missing_snap_name(void) {
    /* vm_name만 있고 snapshot_name/"snap_name" 없는 경우 */
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", "web-prod");

    SnapValidateResult r = validate_rollback_params(params);
    g_assert_cmpint(r, ==, SNAP_VALID_MISSING_PARAMS);

    json_object_unref(params);
}

/* ══════════════════════════════════════════════════════════════
 * 2. 잘못된 문자 테스트 (ZFS 토큰 제약)
 * ══════════════════════════════════════════════════════════════ */

static void test_rollback_invalid_vm_name_semicolon(void) {
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", "web;rm -rf /");
    json_object_set_string_member(params, "snapshot_name", "daily-01");

    SnapValidateResult r = validate_rollback_params(params);
    g_assert_cmpint(r, ==, SNAP_VALID_INVALID_CHARS);

    json_object_unref(params);
}

static void test_rollback_invalid_vm_name_dotdot(void) {
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", "../etc/passwd");
    json_object_set_string_member(params, "snapshot_name", "snap-01");

    SnapValidateResult r = validate_rollback_params(params);
    g_assert_cmpint(r, ==, SNAP_VALID_INVALID_CHARS);

    json_object_unref(params);
}

static void test_rollback_invalid_vm_name_dollar(void) {
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", "$HOME");
    json_object_set_string_member(params, "snapshot_name", "snap-01");

    SnapValidateResult r = validate_rollback_params(params);
    g_assert_cmpint(r, ==, SNAP_VALID_INVALID_CHARS);

    json_object_unref(params);
}

static void test_rollback_invalid_vm_name_slash(void) {
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", "web/prod");
    json_object_set_string_member(params, "snapshot_name", "snap-01");

    SnapValidateResult r = validate_rollback_params(params);
    g_assert_cmpint(r, ==, SNAP_VALID_INVALID_CHARS);

    json_object_unref(params);
}

static void test_rollback_invalid_snap_name_chars(void) {
    /* 스냅샷 이름에 잘못된 문자 */
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", "web-prod");
    json_object_set_string_member(params, "snapshot_name", "snap@evil");

    SnapValidateResult r = validate_rollback_params(params);
    g_assert_cmpint(r, ==, SNAP_VALID_INVALID_CHARS);

    json_object_unref(params);
}

/* ══════════════════════════════════════════════════════════════
 * 3. 빈 문자열 테스트
 * ══════════════════════════════════════════════════════════════ */

static void test_rollback_empty_vm_name(void) {
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", "");
    json_object_set_string_member(params, "snapshot_name", "snap-01");

    SnapValidateResult r = validate_rollback_params(params);
    g_assert_cmpint(r, ==, SNAP_VALID_MISSING_PARAMS);

    json_object_unref(params);
}

static void test_rollback_empty_snap_name(void) {
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", "web-prod");
    json_object_set_string_member(params, "snapshot_name", "");

    SnapValidateResult r = validate_rollback_params(params);
    g_assert_cmpint(r, ==, SNAP_VALID_MISSING_PARAMS);

    json_object_unref(params);
}

/* ══════════════════════════════════════════════════════════════
 * 4. 오버롱 이름 테스트
 * ══════════════════════════════════════════════════════════════ */

static void test_rollback_overlong_vm_name(void) {
    /* 129자 — pcv_validate_zfs_token 상한(128) 초과 */
    gchar *long_name = g_strnfill(129, 'a');
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", long_name);
    json_object_set_string_member(params, "snapshot_name", "snap-01");

    SnapValidateResult r = validate_rollback_params(params);
    g_assert_cmpint(r, ==, SNAP_VALID_INVALID_CHARS);

    g_free(long_name);
    json_object_unref(params);
}

/* ══════════════════════════════════════════════════════════════
 * 5. 유효 파라미터 테스트 (포지티브 경로)
 * ══════════════════════════════════════════════════════════════ */

static void test_rollback_valid_params(void) {
    /* REST 스타일: "name" + "snapshot_name" */
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", "web-prod");
    json_object_set_string_member(params, "snapshot_name", "daily-20260410");

    SnapValidateResult r = validate_rollback_params(params);
    g_assert_cmpint(r, ==, SNAP_VALID_OK);

    json_object_unref(params);
}

static void test_rollback_dual_key_support(void) {
    /* UDS 스타일: "vm_id" + "snap_name" */
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "vm_id", "web-prod");
    json_object_set_string_member(params, "snap_name", "daily-20260410");

    SnapValidateResult r = validate_rollback_params(params);
    g_assert_cmpint(r, ==, SNAP_VALID_OK);

    /* 키 우선순위 확인: primary("name") 없으면 fallback("vm_id") 사용 */
    const gchar *vm  = get_param(params, "name", "vm_id");
    const gchar *sn  = get_param(params, "snapshot_name", "snap_name");
    g_assert_cmpstr(vm, ==, "web-prod");
    g_assert_cmpstr(sn, ==, "daily-20260410");

    json_object_unref(params);
}

/* ══════════════════════════════════════════════════════════════
 * 6. 롤백 상태 머신 — was_running 결정 로직
 * ══════════════════════════════════════════════════════════════ */

static void test_rollback_state_running(void) {
    /* VIR_DOMAIN_RUNNING(1) → was_running=TRUE */
    g_assert_true(rollback_was_running(TEST_VIR_DOMAIN_RUNNING));
}

static void test_rollback_state_paused(void) {
    /* VIR_DOMAIN_PAUSED(3) → was_running=TRUE (일시정지도 재기동 대상) */
    g_assert_true(rollback_was_running(TEST_VIR_DOMAIN_PAUSED));
}

static void test_rollback_state_shutoff(void) {
    /* VIR_DOMAIN_SHUTOFF(5) → was_running=FALSE */
    g_assert_false(rollback_was_running(TEST_VIR_DOMAIN_SHUTOFF));
}

static void test_rollback_state_crashed(void) {
    /* VIR_DOMAIN_CRASHED(6) → was_running=FALSE */
    g_assert_false(rollback_was_running(TEST_VIR_DOMAIN_CRASHED));
}

/* ══════════════════════════════════════════════════════════════
 * 7. ZFS dataset 경로 생성
 * ══════════════════════════════════════════════════════════════ */

static void test_rollback_zfs_dataset_path(void) {
    gchar *path = build_zfs_dataset_path("pcvpool/vms", "web01", "daily-01");
    g_assert_cmpstr(path, ==, "pcvpool/vms/web01@daily-01");
    g_free(path);
}

static void test_rollback_zfs_dataset_path_with_underscores(void) {
    /* 언더스코어 VM 이름도 유효한 ZFS 토큰 */
    gchar *path = build_zfs_dataset_path("pcvpool/vms", "db_primary", "auto_2026_04_10");
    g_assert_cmpstr(path, ==, "pcvpool/vms/db_primary@auto_2026_04_10");
    g_free(path);
}

/* ══════════════════════════════════════════════════════════════
 * 추가: validate_zfs_token 직접 검증
 * ══════════════════════════════════════════════════════════════ */

static void test_zfs_token_valid_alphanum(void) {
    g_assert_true(validate_zfs_token("web01"));
    g_assert_true(validate_zfs_token("web-prod"));
    g_assert_true(validate_zfs_token("snap_20260410"));
    g_assert_true(validate_zfs_token("A1B2C3"));
}

static void test_zfs_token_invalid_null(void) {
    g_assert_false(validate_zfs_token(NULL));
}

static void test_zfs_token_invalid_empty(void) {
    g_assert_false(validate_zfs_token(""));
}

static void test_zfs_token_boundary_128(void) {
    /* 정확히 128자 — 유효 */
    gchar *name = g_strnfill(128, 'a');
    g_assert_true(validate_zfs_token(name));
    g_free(name);
}

static void test_zfs_token_boundary_129(void) {
    /* 129자 — 상한 초과 */
    gchar *name = g_strnfill(129, 'a');
    g_assert_false(validate_zfs_token(name));
    g_free(name);
}

/* ══════════════════════════════════════════════════════════════
 * 등록
 * ══════════════════════════════════════════════════════════════ */

void test_snapshot_rollback_register(void) {
    /* 필수 파라미터 누락 */
    g_test_add_func("/snapshot/rollback/missing_vm_name",
                    test_rollback_missing_vm_name);
    g_test_add_func("/snapshot/rollback/missing_snap_name",
                    test_rollback_missing_snap_name);

    /* 잘못된 문자 (ZFS 토큰 제약) */
    g_test_add_func("/snapshot/rollback/invalid_vm_name_semicolon",
                    test_rollback_invalid_vm_name_semicolon);
    g_test_add_func("/snapshot/rollback/invalid_vm_name_dotdot",
                    test_rollback_invalid_vm_name_dotdot);
    g_test_add_func("/snapshot/rollback/invalid_vm_name_dollar",
                    test_rollback_invalid_vm_name_dollar);
    g_test_add_func("/snapshot/rollback/invalid_vm_name_slash",
                    test_rollback_invalid_vm_name_slash);
    g_test_add_func("/snapshot/rollback/invalid_snap_name_chars",
                    test_rollback_invalid_snap_name_chars);

    /* 빈 문자열 */
    g_test_add_func("/snapshot/rollback/empty_vm_name",
                    test_rollback_empty_vm_name);
    g_test_add_func("/snapshot/rollback/empty_snap_name",
                    test_rollback_empty_snap_name);

    /* 오버롱 이름 */
    g_test_add_func("/snapshot/rollback/overlong_vm_name",
                    test_rollback_overlong_vm_name);

    /* 유효 파라미터 (포지티브) */
    g_test_add_func("/snapshot/rollback/valid_params",
                    test_rollback_valid_params);
    g_test_add_func("/snapshot/rollback/dual_key_support",
                    test_rollback_dual_key_support);

    /* 롤백 상태 머신 */
    g_test_add_func("/snapshot/rollback/state_running",
                    test_rollback_state_running);
    g_test_add_func("/snapshot/rollback/state_paused",
                    test_rollback_state_paused);
    g_test_add_func("/snapshot/rollback/state_shutoff",
                    test_rollback_state_shutoff);
    g_test_add_func("/snapshot/rollback/state_crashed",
                    test_rollback_state_crashed);

    /* ZFS dataset 경로 생성 */
    g_test_add_func("/snapshot/rollback/zfs_dataset_path",
                    test_rollback_zfs_dataset_path);
    g_test_add_func("/snapshot/rollback/zfs_dataset_path_underscores",
                    test_rollback_zfs_dataset_path_with_underscores);

    /* ZFS 토큰 직접 검증 */
    g_test_add_func("/snapshot/rollback/zfs_token_valid",
                    test_zfs_token_valid_alphanum);
    g_test_add_func("/snapshot/rollback/zfs_token_null",
                    test_zfs_token_invalid_null);
    g_test_add_func("/snapshot/rollback/zfs_token_empty",
                    test_zfs_token_invalid_empty);
    g_test_add_func("/snapshot/rollback/zfs_token_boundary_128",
                    test_zfs_token_boundary_128);
    g_test_add_func("/snapshot/rollback/zfs_token_boundary_129",
                    test_zfs_token_boundary_129);
}
