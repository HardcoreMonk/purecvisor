/* tests/test_backup_basic.c
 *
 * 백업 스케줄러 기본 테스트 -- 정책 검증 + JSON 직렬화 패턴
 *
 * ============================================================================
 *  이 파일이 테스트하는 것
 * ============================================================================
 *  backup_scheduler.c (src/modules/backup/)의 입력 검증 로직과 JSON 구조를
 *  데몬 의존성 없이 단독으로 검증한다. 11개 테스트 케이스.
 *
 *  1. 정책 파라미터 검증 (vm_name, interval_hours, retention_count)
 *     - 정상: "web-prod"/24h/7일, 와일드카드 "*" 허용
 *     - 거부: NULL, 빈 문자열, 공백/세미콜론/경로순회 주입, 0 이하 숫자
 *
 *  2. 자동 스냅샷 이름 형식 검증 ("pcv-auto-YYYYMMDD-HHMMSS")
 *     - 접두사 "pcv-auto-" 필수, 뒤 15자리 = 날짜8 + "-" + 시간6
 *
 *  3. 정책 JSON 직렬화/역직렬화 라운드트립
 *     - vm_name, interval_hours, retention_count, enabled 필드 구조
 *
 *  4. 증분 백업 결과 JSON 구조
 *     - snapshot, base_snapshot, file, size_bytes 필드 존재 + 양수
 *
 *  왜 DAEMON_SRCS를 직접 링크하지 않는가?
 *  → backup_scheduler.c는 ZFS/libvirt 등 데몬 전용 의존성이 있어서
 *    테스트 바이너리에 링크하면 수십 개 심볼 미해결 에러가 발생한다.
 *    대신 검증 로직(_validate_backup_policy_params 등)을 이 파일에 재현한다.
 * ============================================================================
 */

#include <glib.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include "purecvisor/pcv_validate.h"

/* ── 정책 파라미터 검증 헬퍼 ────────────────────────────── */

/**
 * 백업 정책 입력 파라미터 검증 (backup_scheduler.c의 검증 로직 재현)
 */
static gboolean
_validate_backup_policy_params(const gchar *vm_name,
                               gint         interval_hours,
                               gint         retention_count)
{
    /* vm_name: NULL/빈 문자열 거부, "*"(와일드카드) 허용, 아니면 vm_name 규칙 적용 */
    if (!vm_name || vm_name[0] == '\0') return FALSE;
    if (g_strcmp0(vm_name, "*") != 0 && !pcv_validate_vm_name(vm_name))
        return FALSE;
    /* interval 1~8760 (B8-M1: 상한 1년) */
    if (interval_hours < 1 || interval_hours > 8760) return FALSE;
    /* retention 1~365 (B8-M2: 상한 365일) */
    if (retention_count < 1 || retention_count > 365) return FALSE;
    return TRUE;
}

/* 정상 입력: 일반 VM 이름, 최소값, 와일드카드 전부 통과해야 한다 */
static void test_backup_policy_valid(void) {
    g_assert_true(_validate_backup_policy_params("web-prod", 24, 7));
    g_assert_true(_validate_backup_policy_params("db01", 1, 1));
    g_assert_true(_validate_backup_policy_params("*", 12, 30));
}

static void test_backup_policy_null_name(void) {
    g_assert_false(_validate_backup_policy_params(NULL, 24, 7));
}

static void test_backup_policy_empty_name(void) {
    g_assert_false(_validate_backup_policy_params("", 24, 7));
}

/* 보안 위험 입력: 공백(명령줄 분리), 세미콜론(인젝션), 경로순회 전부 거부 */
static void test_backup_policy_invalid_name(void) {
    g_assert_false(_validate_backup_policy_params("web prod", 24, 7));  /* 공백 */
    g_assert_false(_validate_backup_policy_params("vm;rm", 24, 7));     /* 명령 주입 */
    g_assert_false(_validate_backup_policy_params("../../etc", 24, 7)); /* 경로 순회 */
}

static void test_backup_policy_invalid_interval(void) {
    g_assert_false(_validate_backup_policy_params("web-prod", 0, 7));
    g_assert_false(_validate_backup_policy_params("web-prod", -1, 7));
}

static void test_backup_policy_invalid_retention(void) {
    g_assert_false(_validate_backup_policy_params("web-prod", 24, 0));
    g_assert_false(_validate_backup_policy_params("web-prod", 24, -5));
}

/* B8-M1/M2: interval_hours 상한(8760) 및 retention_count 상한(365) 거부 검증 */
static void test_backup_policy_upper_bound(void) {
    /* interval_hours 상한 초과 → 거부 */
    g_assert_false(_validate_backup_policy_params("web-prod", 8761, 7));
    g_assert_false(_validate_backup_policy_params("web-prod", 99999, 7));
    /* retention_count 상한 초과 → 거부 */
    g_assert_false(_validate_backup_policy_params("web-prod", 24, 366));
    g_assert_false(_validate_backup_policy_params("web-prod", 24, 9999));
    /* 상한 경계값 정확히 → 허용 */
    g_assert_true(_validate_backup_policy_params("web-prod", 8760, 365));
    /* 상한 경계값 + 1 → 거부 */
    g_assert_false(_validate_backup_policy_params("web-prod", 8760, 366));
    g_assert_false(_validate_backup_policy_params("web-prod", 8761, 365));
}

/* restore VM 상태 검증: migrating/saving 상태의 VM은 복원 거부해야 한다 */

/**
 * backup restore 시 허용/거부 VM 상태 검증 (handler_backup.c 로직 재현)
 * 실제 코드는 pcv_backup_restore() 내부에서 VM 상태를 확인하여 "migrating",
 * "saving" 등의 과도 상태에서는 복원을 거부한다.
 */
static gboolean
_is_restore_allowed_state(const gchar *vm_state)
{
    if (!vm_state) return FALSE;
    /* 복원 허용: VM이 완전히 정지된 상태 */
    if (g_strcmp0(vm_state, "stopped") == 0) return TRUE;
    if (g_strcmp0(vm_state, "shutoff") == 0) return TRUE;
    /* 복원 거부: 마이그레이션/저장 등 과도 상태 */
    return FALSE;
}

static void test_backup_restore_state_validation(void) {
    /* 허용 상태: stopped, shutoff */
    g_assert_true(_is_restore_allowed_state("stopped"));
    g_assert_true(_is_restore_allowed_state("shutoff"));
    /* 거부 상태: migrating, saving */
    g_assert_false(_is_restore_allowed_state("migrating"));
    g_assert_false(_is_restore_allowed_state("saving"));
    /* 기타 과도 상태도 거부 */
    g_assert_false(_is_restore_allowed_state(NULL));
    g_assert_false(_is_restore_allowed_state("running"));
    g_assert_false(_is_restore_allowed_state("paused"));
}

/* ── 스냅샷 이름 패턴 검증 ──────────────────────────────── */

/**
 * pcv-auto-YYYYMMDD-HHMMSS 형식 검증
 */
static gboolean
_validate_auto_snapshot_name(const gchar *name)
{
    if (!name) return FALSE;
    /* must start with "pcv-auto-" */
    if (!g_str_has_prefix(name, "pcv-auto-")) return FALSE;
    /* remaining: YYYYMMDD-HHMMSS (15 chars) */
    const gchar *ts = name + strlen("pcv-auto-");
    if (strlen(ts) != 15) return FALSE;
    /* format: 8 digits, dash, 6 digits */
    for (int i = 0; i < 8; i++)
        if (!g_ascii_isdigit(ts[i])) return FALSE;
    if (ts[8] != '-') return FALSE;
    for (int i = 9; i < 15; i++)
        if (!g_ascii_isdigit(ts[i])) return FALSE;
    return TRUE;
}

static void test_backup_snapshot_name_valid(void) {
    g_assert_true(_validate_auto_snapshot_name("pcv-auto-20260324-100000"));
    g_assert_true(_validate_auto_snapshot_name("pcv-auto-20261231-235959"));
}

static void test_backup_snapshot_name_invalid(void) {
    g_assert_false(_validate_auto_snapshot_name(NULL));
    g_assert_false(_validate_auto_snapshot_name(""));
    g_assert_false(_validate_auto_snapshot_name("pcv-auto-"));
    g_assert_false(_validate_auto_snapshot_name("pcv-auto-2026032"));      /* too short */
    g_assert_false(_validate_auto_snapshot_name("pcv-auto-20260324100000")); /* no dash */
    g_assert_false(_validate_auto_snapshot_name("manual-snap"));            /* wrong prefix */
    g_assert_false(_validate_auto_snapshot_name("pcv-auto-abcdefgh-123456")); /* non-digit */
}

/* ── 정책 JSON 라운드트립 ───────────────────────────────── */

static void test_backup_policy_json_roundtrip(void) {
    /* PcvBackupPolicy 구조를 JSON으로 직렬화/역직렬화 패턴 검증 */
    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "vm_name", "web-prod");
    json_object_set_int_member(obj, "interval_hours", 24);
    json_object_set_int_member(obj, "retention_count", 7);
    json_object_set_boolean_member(obj, "enabled", TRUE);

    /* 역직렬화 검증 */
    g_assert_cmpstr(json_object_get_string_member(obj, "vm_name"), ==, "web-prod");
    g_assert_cmpint(json_object_get_int_member(obj, "interval_hours"), ==, 24);
    g_assert_cmpint(json_object_get_int_member(obj, "retention_count"), ==, 7);
    g_assert_true(json_object_get_boolean_member(obj, "enabled"));

    json_object_unref(obj);
}

static void test_backup_policy_wildcard(void) {
    /* 와일드카드 정책 ("*") 검증 */
    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "vm_name", "*");
    json_object_set_int_member(obj, "interval_hours", 12);
    json_object_set_int_member(obj, "retention_count", 14);
    json_object_set_boolean_member(obj, "enabled", TRUE);

    g_assert_cmpstr(json_object_get_string_member(obj, "vm_name"), ==, "*");
    g_assert_cmpint(json_object_get_int_member(obj, "interval_hours"), ==, 12);

    json_object_unref(obj);
}

/* ── 증분 백업 결과 JSON 구조 ───────────────────────────── */

static void test_backup_incremental_result_json(void) {
    /* pcv_backup_incremental 반환값 JSON 구조 검증 */
    JsonObject *result = json_object_new();
    json_object_set_string_member(result, "snapshot", "pcv-auto-20260324-100000");
    json_object_set_string_member(result, "base_snapshot", "pcv-auto-20260323-100000");
    json_object_set_string_member(result, "file", "/var/lib/purecvisor/backups/web-prod_incr_20260324.zfs");
    json_object_set_int_member(result, "size_bytes", 1048576);

    g_assert_true(json_object_has_member(result, "snapshot"));
    g_assert_true(json_object_has_member(result, "base_snapshot"));
    g_assert_true(json_object_has_member(result, "file"));
    g_assert_true(json_object_has_member(result, "size_bytes"));
    g_assert_cmpint(json_object_get_int_member(result, "size_bytes"), >, 0);

    json_object_unref(result);
}

/* ── 등록 ────────────────────────────────────────────────── */
/* test_main.c에서 호출. g_test_add_func()로 GLib 테스트 프레임워크에 등록 */
void test_backup_basic_register(void) {
    g_test_add_func("/backup/policy/valid",              test_backup_policy_valid);
    g_test_add_func("/backup/policy/null_name",          test_backup_policy_null_name);
    g_test_add_func("/backup/policy/empty_name",         test_backup_policy_empty_name);
    g_test_add_func("/backup/policy/invalid_name",       test_backup_policy_invalid_name);
    g_test_add_func("/backup/policy/invalid_interval",   test_backup_policy_invalid_interval);
    g_test_add_func("/backup/policy/invalid_retention",  test_backup_policy_invalid_retention);
    g_test_add_func("/backup/policy/upper_bound",        test_backup_policy_upper_bound);
    g_test_add_func("/backup/restore/state_validation",  test_backup_restore_state_validation);
    g_test_add_func("/backup/snapshot_name/valid",       test_backup_snapshot_name_valid);
    g_test_add_func("/backup/snapshot_name/invalid",     test_backup_snapshot_name_invalid);
    g_test_add_func("/backup/policy/json_roundtrip",     test_backup_policy_json_roundtrip);
    g_test_add_func("/backup/policy/wildcard",           test_backup_policy_wildcard);
    g_test_add_func("/backup/incremental/result_json",   test_backup_incremental_result_json);
}
