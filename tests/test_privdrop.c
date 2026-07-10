/* tests/test_privdrop.c
 *
 * 대상 모듈: src/utils/pcv_privdrop.c — 데몬 보안 강화 (capability 드롭 + seccomp)
 *
 * 이 테스트가 검증하는 것:
 *   prctl 상수 존재, NNP/seccomp 모드 조회 가능성,
 *   pcv_privdrop_apply_all / no_new_privs / capabilities / seccomp
 *   각 함수의 크래시 없는 실행을 검사한다.
 *
 * 중요: pcv_privdrop_* 호출은 프로세스 전역에 영향 (capability 드롭, seccomp 설치).
 *       g_test_trap_subprocess로 격리하여 test_runner 본체를 보호한다.
 *
 * 실행: sudo ./test_runner -p /privdrop   (sudo 필요: capability 조작)
 *
 * 외부 의존: /proc (prctl 시스템 콜, 리눅스 전용)
 */

#include <glib.h>
#include <sys/prctl.h>
#include "../src/utils/pcv_privdrop.h"

/* ── prctl 상수 확인 ─────────────────────────────── */

static void test_prctl_constants(void) {
    g_assert_cmpint(PR_SET_NO_NEW_PRIVS, ==, 38);
    g_assert_cmpint(PR_GET_NO_NEW_PRIVS, ==, 39);
}

/* ── prctl 호출 가능 확인 ────────────────────────── */

static void test_prctl_get_nnp(void) {
    /* 현재 프로세스의 NNP 상태 조회 (설정 변경 없이 읽기만) */
    int nnp = prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0);
    /* 0(해제) 또는 1(설정) — crash 없이 반환되면 성공 */
    g_assert_true(nnp == 0 || nnp == 1);
}

/* ── seccomp 모드 조회 ───────────────────────────── */

static void test_seccomp_mode_readable(void) {
    /* PR_GET_SECCOMP: 현재 seccomp 모드 조회 (0=disabled, 1=strict, 2=filter) */
    int mode = prctl(PR_GET_SECCOMP, 0, 0, 0, 0);
    g_assert_true(mode >= 0 && mode <= 2);
}

/* ── pcv_privdrop_* 격리 호출 (g_test_trap_subprocess) ──
 *
 * 부수효과 위험: capability 드롭/seccomp 설치는 프로세스 전역에 영향.
 * subprocess로 격리하여 test_runner 본체는 영향 받지 않음. */

/* pcv_privdrop_apply_all 호출 (서브프로세스 격리 — 본체에 영향 없음) */
static void test_apply_all_subprocess(void) {
    if (g_test_subprocess()) {
        pcv_privdrop_apply_all();
        return;
    }
    g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
    g_test_trap_assert_passed();
}

static void test_no_new_privs_subprocess(void) {
    if (g_test_subprocess()) {
        gboolean ok = pcv_privdrop_no_new_privs();
        /* root/non-root 양쪽 모두 false-safe (graceful degradation) */
        (void)ok;
        return;
    }
    g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
    g_test_trap_assert_passed();
}

static void test_capabilities_subprocess(void) {
    if (g_test_subprocess()) {
        gboolean ok = pcv_privdrop_capabilities();
        (void)ok;
        return;
    }
    g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
    g_test_trap_assert_passed();
}

static void test_seccomp_subprocess(void) {
    if (g_test_subprocess()) {
        gboolean ok = pcv_privdrop_seccomp();
        (void)ok;
        return;
    }
    g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
    g_test_trap_assert_passed();
}

/* ── 등록 ──────────────────────────────────────────── */

void test_privdrop_register(void) {
    g_test_add_func("/privdrop/prctl_constants",     test_prctl_constants);
    g_test_add_func("/privdrop/prctl_get_nnp",       test_prctl_get_nnp);
    g_test_add_func("/privdrop/seccomp_mode_readable", test_seccomp_mode_readable);
    g_test_add_func("/privdrop/apply_all_subprocess",   test_apply_all_subprocess);
    g_test_add_func("/privdrop/no_new_privs_subprocess", test_no_new_privs_subprocess);
    g_test_add_func("/privdrop/capabilities_subprocess", test_capabilities_subprocess);
    g_test_add_func("/privdrop/seccomp_subprocess",      test_seccomp_subprocess);
}
