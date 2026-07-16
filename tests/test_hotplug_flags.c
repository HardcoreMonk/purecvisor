/**
 * @file test_hotplug_flags.c
 * @brief vm.set_vcpu / vm.set_memory 의 apply="config" 하위호환 계약 유닛테스트 (ADR-0025, Fix B)
 *
 * 대상: src/modules/dispatcher/hotplug_affect_policy.c
 *       pcv_hotplug_compute_affect_flags(is_active, config_only)
 *       (handler_vm_hotplug.c 의 hotplug_get_affect_flags → 이 순수 함수로 위임;
 *        데몬·test_runner 가 둘 다 이 TU 를 링크)
 *
 * ============================================================================
 *  이 테스트가 검증하는 것 (affect-flag 결정 계약)
 * ============================================================================
 *  Fix B 는 apply="config" 파라미터로 실행 중 VM 이어도 CONFIG-only(다음 부팅
 *  반영, LIVE 미적용)를 제공한다. 실행 중 vCPU 를 부팅 수 아래로 줄이는 live
 *  decrease 는 QEMU/KVM 에서 -32000 "failed to find appropriate hotpluggable
 *  vcpus" 로 실패하므로, 프론트가 apply="config" 로 재전송해 계약을 사용한다.
 *
 *  플래그 계산은 데몬(handler_vm_hotplug.c)과 test_runner 가 **둘 다** 링크하는
 *  순수 함수 pcv_hotplug_compute_affect_flags() 로 추출됐다. 따라서 이 테스트는
 *  손복제 대신 **실 프로덕션 함수**를 직접 호출한다 — 계약이 드리프트하면 red 가
 *  된다.
 *
 *  반사실(counterfactual): compute 함수에서 `&& !config_only` 분기를 제거하면
 *  (is_active=TRUE, config_only=TRUE) 케이스가 LIVE|CONFIG 를 반환하게 되어
 *  test_config_only_active_no_live 가 red 가 된다 ("실행 중이어도 CONFIG-only"
 *  계약 붕괴 감지 — 이 게이트가 없으면 Fix B 의 핵심 동작이 무효화된다).
 *
 * 실행: ./test_runner -p /hotplug_flags
 * 외부 의존: 없음 (실 UDS/libvirt 도메인 불필요 — 순수 결정 로직).
 * ============================================================================
 */
#include <glib.h>
#include <libvirt/libvirt.h>
#include "../src/modules/dispatcher/hotplug_affect_policy.h"

/* (active=T, config_only=F) → LIVE|CONFIG : 현행 동작(apply 미지정/"live", 실행 중). */
static void test_active_default_live_config(void) {
    g_assert_cmpuint(pcv_hotplug_compute_affect_flags(TRUE, FALSE), ==,
                     (unsigned int)(VIR_DOMAIN_AFFECT_LIVE | VIR_DOMAIN_AFFECT_CONFIG));
}

/* (active=F, config_only=F) → CONFIG : 정지 VM 은 실행 아님이라 LIVE 없음(현행). */
static void test_inactive_default_config(void) {
    g_assert_cmpuint(pcv_hotplug_compute_affect_flags(FALSE, FALSE), ==,
                     (unsigned int)VIR_DOMAIN_AFFECT_CONFIG);
}

/* 핵심: (active=T, config_only=T) → CONFIG (실행 중이어도 LIVE 없음).
 * 반사실: compute 의 `&& !config_only` 제거 시 LIVE 가 붙어 이 케이스가 red. */
static void test_config_only_active_no_live(void) {
    unsigned int f = pcv_hotplug_compute_affect_flags(TRUE, TRUE);
    g_assert_cmpuint(f, ==, (unsigned int)VIR_DOMAIN_AFFECT_CONFIG);
    g_assert_true((f & VIR_DOMAIN_AFFECT_LIVE) == 0);
}

/* (active=F, config_only=T) → CONFIG : 정지 VM 은 통상과 동일. */
static void test_config_only_inactive(void) {
    g_assert_cmpuint(pcv_hotplug_compute_affect_flags(FALSE, TRUE), ==,
                     (unsigned int)VIR_DOMAIN_AFFECT_CONFIG);
}

void test_hotplug_flags_register(void) {
    g_test_add_func("/hotplug_flags/active_default_live_config",
                    test_active_default_live_config);
    g_test_add_func("/hotplug_flags/inactive_default_config",
                    test_inactive_default_config);
    g_test_add_func("/hotplug_flags/config_only_active_no_live",
                    test_config_only_active_no_live);
    g_test_add_func("/hotplug_flags/config_only_inactive",
                    test_config_only_inactive);
}
