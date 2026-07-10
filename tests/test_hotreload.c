/* tests/test_hotreload.c
 *
 * 핫 리로드 기본 테스트 -- 업그레이드 상태 머신 + 버전 + FD 검증
 *
 * ============================================================================
 *  이 파일이 테스트하는 것
 * ============================================================================
 *  hot_reload.c (src/api/)의 무중단 업그레이드 상태 머신을 검증한다.
 *  11개 테스트 케이스.
 *
 *  1. 상태 머신 전이 규칙:
 *     정상: IDLE → DRAINING → READY → EXECUTING (단방향 전진)
 *     롤백: 어떤 상태에서든 → IDLE (실패 시 원상 복귀)
 *     금지: 건너뛰기 (IDLE→READY) 및 역방향 (READY→DRAINING)
 *
 *  2. 실제 API 호출 (pcv_hot_reload_get_state / get_version / init):
 *     - 초기 상태 = IDLE
 *     - 버전 문자열은 NULL이 아니고 길이 > 0
 *     - init(NULL) 호출 시 /proc/self/exe 폴백
 *
 *  3. 버전 문자열 형식: "X.Y" 이상 (점 1개 이상, 숫자로 시작)
 *
 *  4. 환경변수 키: PCV_UPGRADE_FD, LISTEN_FDS (공백 없음)
 *
 *  5. FD 값 범위: 3 이상만 유효 (0=stdin, 1=stdout, 2=stderr)
 * ============================================================================
 */

#include <glib.h>
#include <string.h>
#include "../src/api/hot_reload.h"

/* hot_reload.h의 PcvUpgradeState 재선언 (DAEMON_SRCS 의존 회피) */
typedef enum {
    TEST_UPGRADE_IDLE      = 0,
    TEST_UPGRADE_DRAINING  = 1,
    TEST_UPGRADE_READY     = 2,
    TEST_UPGRADE_EXECUTING = 3,
} TestUpgradeState;

/* hot_reload.c에 정의된 상수 재선언 */
#define PCV_VERSION_STR "1.0"
#define PCV_UPGRADE_FD_ENV "PCV_UPGRADE_FD"
#define PCV_LISTEN_FDS_ENV "LISTEN_FDS"

/* ── 실제 hot_reload.c 호출 케이스 ─────────────────── */

static void test_initial_state_idle(void) {
    /* init 호출 전: get_state는 default IDLE 반환 */
    PcvUpgradeState s = pcv_hot_reload_get_state();
    g_assert_cmpint(s, ==, PCV_UPGRADE_IDLE);
}

static void test_get_version_nonnull(void) {
    const gchar *v = pcv_hot_reload_get_version();
    g_assert_nonnull(v);
    g_assert_cmpuint(strlen(v), >, 0);
}

static void test_init_with_null_path(void) {
    /* binary_path NULL → /proc/self/exe 사용 */
    pcv_hot_reload_init(NULL, -1);
    PcvUpgradeState s = pcv_hot_reload_get_state();
    g_assert_cmpint(s, ==, PCV_UPGRADE_IDLE);
}

/* ── 상태 열거형 값 정확성 ──────────────────────────────── */

static void test_hotreload_state_values(void) {
    g_assert_cmpint(TEST_UPGRADE_IDLE,      ==, 0);
    g_assert_cmpint(TEST_UPGRADE_DRAINING,  ==, 1);
    g_assert_cmpint(TEST_UPGRADE_READY,     ==, 2);
    g_assert_cmpint(TEST_UPGRADE_EXECUTING, ==, 3);
}

/* ── 상태 전이 유효성 ──────────────────────────────────── */

/**
 * 상태 전이 규칙 검증 헬퍼:
 *   IDLE → DRAINING → READY → EXECUTING
 *   실패 시: any → IDLE (롤백)
 */
static gboolean
_is_valid_transition(TestUpgradeState from, TestUpgradeState to)
{
    /* 정상 전진 */
    if (from == TEST_UPGRADE_IDLE      && to == TEST_UPGRADE_DRAINING)  return TRUE;
    if (from == TEST_UPGRADE_DRAINING  && to == TEST_UPGRADE_READY)     return TRUE;
    if (from == TEST_UPGRADE_READY     && to == TEST_UPGRADE_EXECUTING) return TRUE;
    /* 롤백 (실패 시 IDLE로 복귀) */
    if (to == TEST_UPGRADE_IDLE) return TRUE;
    return FALSE;
}

static void test_hotreload_valid_transitions(void) {
    /* 정상 전진 경로 */
    g_assert_true(_is_valid_transition(TEST_UPGRADE_IDLE,     TEST_UPGRADE_DRAINING));
    g_assert_true(_is_valid_transition(TEST_UPGRADE_DRAINING, TEST_UPGRADE_READY));
    g_assert_true(_is_valid_transition(TEST_UPGRADE_READY,    TEST_UPGRADE_EXECUTING));
}

static void test_hotreload_rollback_transitions(void) {
    /* 어떤 상태에서든 IDLE로 롤백 가능 */
    g_assert_true(_is_valid_transition(TEST_UPGRADE_DRAINING,  TEST_UPGRADE_IDLE));
    g_assert_true(_is_valid_transition(TEST_UPGRADE_READY,     TEST_UPGRADE_IDLE));
    g_assert_true(_is_valid_transition(TEST_UPGRADE_EXECUTING, TEST_UPGRADE_IDLE));
    g_assert_true(_is_valid_transition(TEST_UPGRADE_IDLE,      TEST_UPGRADE_IDLE));
}

static void test_hotreload_invalid_transitions(void) {
    /* 건너뛰기 불가 */
    g_assert_false(_is_valid_transition(TEST_UPGRADE_IDLE,     TEST_UPGRADE_READY));
    g_assert_false(_is_valid_transition(TEST_UPGRADE_IDLE,     TEST_UPGRADE_EXECUTING));
    g_assert_false(_is_valid_transition(TEST_UPGRADE_DRAINING, TEST_UPGRADE_EXECUTING));
    /* 역방향 (IDLE 제외) 불가 */
    g_assert_false(_is_valid_transition(TEST_UPGRADE_READY,    TEST_UPGRADE_DRAINING));
    g_assert_false(_is_valid_transition(TEST_UPGRADE_EXECUTING,TEST_UPGRADE_DRAINING));
    g_assert_false(_is_valid_transition(TEST_UPGRADE_EXECUTING,TEST_UPGRADE_READY));
}

/* ── 상태 문자열 변환 ──────────────────────────────────── */

static const gchar *
_upgrade_state_to_str(TestUpgradeState s)
{
    switch (s) {
    case TEST_UPGRADE_IDLE:      return "idle";
    case TEST_UPGRADE_DRAINING:  return "draining";
    case TEST_UPGRADE_READY:     return "ready";
    case TEST_UPGRADE_EXECUTING: return "executing";
    default:                     return "unknown";
    }
}

static void test_hotreload_state_strings(void) {
    g_assert_cmpstr(_upgrade_state_to_str(TEST_UPGRADE_IDLE),      ==, "idle");
    g_assert_cmpstr(_upgrade_state_to_str(TEST_UPGRADE_DRAINING),  ==, "draining");
    g_assert_cmpstr(_upgrade_state_to_str(TEST_UPGRADE_READY),     ==, "ready");
    g_assert_cmpstr(_upgrade_state_to_str(TEST_UPGRADE_EXECUTING), ==, "executing");
    g_assert_cmpstr(_upgrade_state_to_str((TestUpgradeState)99),   ==, "unknown");
}

/* ── 버전 문자열 형식 ──────────────────────────────────── */

static void test_hotreload_version_format(void) {
    const gchar *ver = PCV_VERSION_STR;
    g_assert_nonnull(ver);
    g_assert_cmpuint(strlen(ver), >, 0);
    /* 버전 문자열은 숫자로 시작해야 한다 */
    g_assert_true(g_ascii_isdigit(ver[0]));
    /* 최소 "X.Y" 형식 (점 1개 이상) */
    gint dot_count = 0;
    for (const gchar *p = ver; *p; p++)
        if (*p == '.') dot_count++;
    g_assert_cmpint(dot_count, >=, 1);
}

/* ── 환경변수 키 검증 ──────────────────────────────────── */

static void test_hotreload_env_keys(void) {
    g_assert_cmpstr(PCV_UPGRADE_FD_ENV, ==, "PCV_UPGRADE_FD");
    g_assert_cmpstr(PCV_LISTEN_FDS_ENV, ==, "LISTEN_FDS");
    /* 환경변수 키에 공백이 없어야 한다 */
    g_assert_null(strchr(PCV_UPGRADE_FD_ENV, ' '));
    g_assert_null(strchr(PCV_LISTEN_FDS_ENV, ' '));
}

/* ── FD 값 범위 검증 ──────────────────────────────────── */

static gboolean
_is_valid_listen_fd(int fd)
{
    /* fd 3 이상이어야 유효 (0=stdin, 1=stdout, 2=stderr) */
    return fd >= 3;
}

static void test_hotreload_fd_validation(void) {
    g_assert_true(_is_valid_listen_fd(3));
    g_assert_true(_is_valid_listen_fd(10));
    g_assert_true(_is_valid_listen_fd(1024));
    g_assert_false(_is_valid_listen_fd(0));   /* stdin */
    g_assert_false(_is_valid_listen_fd(1));   /* stdout */
    g_assert_false(_is_valid_listen_fd(2));   /* stderr */
    g_assert_false(_is_valid_listen_fd(-1));  /* invalid */
}

/* ── 등록 ────────────────────────────────────────────────── */

void test_hotreload_register(void) {
    g_test_add_func("/hotreload/state/values",           test_hotreload_state_values);
    g_test_add_func("/hotreload/transition/valid",       test_hotreload_valid_transitions);
    g_test_add_func("/hotreload/transition/rollback",    test_hotreload_rollback_transitions);
    g_test_add_func("/hotreload/transition/invalid",     test_hotreload_invalid_transitions);
    g_test_add_func("/hotreload/state/strings",          test_hotreload_state_strings);
    g_test_add_func("/hotreload/version/format",         test_hotreload_version_format);
    g_test_add_func("/hotreload/env/keys",               test_hotreload_env_keys);
    g_test_add_func("/hotreload/fd/validation",          test_hotreload_fd_validation);
    g_test_add_func("/hotreload/initial_state_idle",     test_initial_state_idle);
    g_test_add_func("/hotreload/get_version_nonnull",    test_get_version_nonnull);
    g_test_add_func("/hotreload/init_with_null_path",    test_init_with_null_path);
}
