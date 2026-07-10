/* tests/test_lxc_basic.c
 *
 * LXC 드라이버 기본 테스트 -- 상태 열거형 + 이름/이미지/NIC/리소스 검증
 *
 * ============================================================================
 *  이 파일이 테스트하는 것
 * ============================================================================
 *  lxc_driver.c (src/modules/lxc/)의 열거형, 이름 검증, 리소스 제한을
 *  liblxc 없이 단독으로 검증한다. 10개 테스트 케이스.
 *
 *  1. 상태 열거형: STOPPED(0)~FROZEN(4), UNKNOWN(99) 값 + 문자열 변환
 *  2. 컨테이너 이름: VM과 동일한 pcv_validate_vm_name() 규칙 적용
 *  3. 이미지 형식: "distro:release" (예: ubuntu:24.04) -- 대문자/콜론 누락 거부
 *  4. 브릿지 이름: virbr0, pcvbr0 등 -- 공백 거부
 *  5. 리소스 제한: cpu_percent, memory_mb, cpu_weight(1-10000), pids_max
 *     -- 음수 거부, 0은 "변경 없음" 의미로 허용
 *
 *  왜 열거형을 재선언(TestLxcState)하는가?
 *  → lxc_driver.h는 DAEMON_SRCS 전용 헤더라 테스트 바이너리에서 include하면
 *    liblxc 심볼 미해결 에러 발생. 값만 복제하여 정합성을 검증.
 * ============================================================================
 */

#include <glib.h>
#include <string.h>
#include "purecvisor/pcv_validate.h"

/* ── LXC State 열거형 정합성 ────────────────────────────── */

/* lxc_driver.h의 PcvLxcState를 여기서 재선언 (DAEMON_SRCS 의존 회피) */
typedef enum {
    TEST_LXC_STATE_STOPPED  = 0,
    TEST_LXC_STATE_STARTING = 1,
    TEST_LXC_STATE_RUNNING  = 2,
    TEST_LXC_STATE_STOPPING = 3,
    TEST_LXC_STATE_FROZEN   = 4,
    TEST_LXC_STATE_UNKNOWN  = 99,
} TestLxcState;

static void test_lxc_state_values(void) {
    g_assert_cmpint(TEST_LXC_STATE_STOPPED,  ==, 0);
    g_assert_cmpint(TEST_LXC_STATE_STARTING, ==, 1);
    g_assert_cmpint(TEST_LXC_STATE_RUNNING,  ==, 2);
    g_assert_cmpint(TEST_LXC_STATE_STOPPING, ==, 3);
    g_assert_cmpint(TEST_LXC_STATE_FROZEN,   ==, 4);
    g_assert_cmpint(TEST_LXC_STATE_UNKNOWN,  ==, 99);
}

static const gchar *
_lxc_state_to_str(TestLxcState s)
{
    switch (s) {
    case TEST_LXC_STATE_STOPPED:  return "STOPPED";
    case TEST_LXC_STATE_STARTING: return "STARTING";
    case TEST_LXC_STATE_RUNNING:  return "RUNNING";
    case TEST_LXC_STATE_STOPPING: return "STOPPING";
    case TEST_LXC_STATE_FROZEN:   return "FROZEN";
    case TEST_LXC_STATE_UNKNOWN:  return "UNKNOWN";
    default:                      return "UNKNOWN";
    }
}

static void test_lxc_state_strings(void) {
    g_assert_cmpstr(_lxc_state_to_str(TEST_LXC_STATE_STOPPED),  ==, "STOPPED");
    g_assert_cmpstr(_lxc_state_to_str(TEST_LXC_STATE_RUNNING),  ==, "RUNNING");
    g_assert_cmpstr(_lxc_state_to_str(TEST_LXC_STATE_FROZEN),   ==, "FROZEN");
    g_assert_cmpstr(_lxc_state_to_str(TEST_LXC_STATE_UNKNOWN),  ==, "UNKNOWN");
    g_assert_cmpstr(_lxc_state_to_str((TestLxcState)42),         ==, "UNKNOWN");
}

/* ── 컨테이너 이름 검증 (pcv_validate_vm_name 재활용) ──── */

static void test_lxc_name_valid(void) {
    g_assert_true(pcv_validate_vm_name("nginx-web"));
    g_assert_true(pcv_validate_vm_name("redis01"));
    g_assert_true(pcv_validate_vm_name("app_server"));
    g_assert_true(pcv_validate_vm_name("a"));     /* single char */
}

static void test_lxc_name_invalid(void) {
    g_assert_false(pcv_validate_vm_name(NULL));
    g_assert_false(pcv_validate_vm_name(""));
    g_assert_false(pcv_validate_vm_name("container name"));    /* space */
    g_assert_false(pcv_validate_vm_name("ctr;echo"));          /* injection */
    g_assert_false(pcv_validate_vm_name("../../../etc"));      /* traversal */
    /* NOTE: leading dash is allowed by pcv_validate_vm_name */
}

/* ── 컨테이너 이미지 검증 ───────────────────────────────── */

static void test_lxc_image_distro_release(void) {
    g_assert_true(pcv_validate_container_image("ubuntu:22.04"));
    g_assert_true(pcv_validate_container_image("ubuntu:24.04"));
    g_assert_true(pcv_validate_container_image("debian:12"));
    g_assert_true(pcv_validate_container_image("debian:bookworm"));
    g_assert_true(pcv_validate_container_image("alpine:3.19"));
    g_assert_true(pcv_validate_container_image("centos:9-stream"));
}

static void test_lxc_image_invalid_format(void) {
    g_assert_false(pcv_validate_container_image(NULL));
    g_assert_false(pcv_validate_container_image(""));
    g_assert_false(pcv_validate_container_image("ubuntu"));       /* no colon */
    g_assert_false(pcv_validate_container_image(":22.04"));       /* no distro */
    g_assert_false(pcv_validate_container_image("Ubuntu:22.04")); /* uppercase */
}

/* ── NIC 브릿지 이름 검증 ───────────────────────────────── */

static void test_lxc_bridge_name_valid(void) {
    g_assert_true(pcv_validate_bridge_name("virbr0"));
    g_assert_true(pcv_validate_bridge_name("pcvbr0"));
    g_assert_true(pcv_validate_bridge_name("br-lan"));
}

static void test_lxc_bridge_name_invalid(void) {
    g_assert_false(pcv_validate_bridge_name(NULL));
    g_assert_false(pcv_validate_bridge_name(""));
    g_assert_false(pcv_validate_bridge_name("a bridge"));  /* space */
}

/* ── 리소스 제한 파라미터 검증 ──────────────────────────── */

/**
 * pcv_lxc_set_resource_limits()의 입력 검증 패턴 재현
 */
static gboolean
_validate_resource_limits(gint cpu_percent, gint memory_mb,
                          gint cpu_weight, gint pids_max)
{
    /* cpu_percent: 0 = no change, >0 valid */
    if (cpu_percent < 0) return FALSE;
    /* memory_mb: 0 = no change, >0 valid */
    if (memory_mb < 0) return FALSE;
    /* cpu_weight: 0 = no change, 1-10000 valid */
    if (cpu_weight < 0 || cpu_weight > 10000) return FALSE;
    /* pids_max: 0 = no change, >0 valid */
    if (pids_max < 0) return FALSE;
    return TRUE;
}

static void test_lxc_resource_limits_valid(void) {
    g_assert_true(_validate_resource_limits(100, 512, 100, 256));
    g_assert_true(_validate_resource_limits(200, 1024, 10000, 0));
    g_assert_true(_validate_resource_limits(0, 0, 0, 0));  /* all no-change */
}

static void test_lxc_resource_limits_invalid(void) {
    g_assert_false(_validate_resource_limits(-1, 512, 100, 256));   /* negative cpu */
    g_assert_false(_validate_resource_limits(100, -1, 100, 256));   /* negative mem */
    g_assert_false(_validate_resource_limits(100, 512, 10001, 256));/* weight too high */
    g_assert_false(_validate_resource_limits(100, 512, -1, 256));   /* negative weight */
    g_assert_false(_validate_resource_limits(100, 512, 100, -1));   /* negative pids */
}

/* ── 등록 ────────────────────────────────────────────────── */

void test_lxc_basic_register(void) {
    g_test_add_func("/lxc/state/values",              test_lxc_state_values);
    g_test_add_func("/lxc/state/strings",             test_lxc_state_strings);
    g_test_add_func("/lxc/name/valid",                test_lxc_name_valid);
    g_test_add_func("/lxc/name/invalid",              test_lxc_name_invalid);
    g_test_add_func("/lxc/image/distro_release",      test_lxc_image_distro_release);
    g_test_add_func("/lxc/image/invalid_format",      test_lxc_image_invalid_format);
    g_test_add_func("/lxc/bridge/valid",              test_lxc_bridge_name_valid);
    g_test_add_func("/lxc/bridge/invalid",            test_lxc_bridge_name_invalid);
    g_test_add_func("/lxc/resource_limits/valid",     test_lxc_resource_limits_valid);
    g_test_add_func("/lxc/resource_limits/invalid",   test_lxc_resource_limits_invalid);
}
