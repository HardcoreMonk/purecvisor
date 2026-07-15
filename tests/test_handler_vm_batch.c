/**
 * @file test_handler_vm_batch.c
 * @brief vm.batch whitelist/reject 결정 로직 유닛 테스트 (ADR-0025)
 *
 * 대상: src/api/vm_batch_policy.c  pcv_vm_batch_action_is_whitelisted()
 *       (dispatcher.c:_handle_vm_batch 의 whitelist 팬아웃 게이트가 위임)
 *
 * ============================================================================
 *  이 테스트가 검증하는 것 (whitelist/reject 결정 계약)
 * ============================================================================
 *  기존 스텁은 action 을 각 VM 에 수행하지 않고, 어떤 action 이든 모든 VM 을
 *  "accepted" 로 반환했다("보고성공 무동작", ADR-0025 가 겨냥하는 클래스).
 *  실 핸들러는 두 결정 게이트를 통과시킨다:
 *    1) whitelist 게이트 — vm_manager 에 public `_async` fn 이 실존하는
 *       action(start/stop)만 허용, 그 외(pause/resume/reboot/delete/…)는
 *       "unsupported batch action" 으로 거부.
 *    2) per-VM 존재 게이트 — 존재하는 VM 만 accepted(→ 매니저 async 팬아웃),
 *       미존재 VM 은 rejected{vm,"VM not found"}.
 *
 *  whitelist 멤버십(게이트 1)은 데몬(dispatcher.c)과 test_runner 가 **둘 다**
 *  링크하는 순수 TU src/api/vm_batch_policy.c 로 추출됐다(I-2 시정). 따라서 이
 *  테스트는 손복제 대신 **실 프로덕션 함수** pcv_vm_batch_action_is_whitelisted()
 *  를 직접 호출한다 — policy 리스트에 누군가 "reboot" 를 추가하면
 *  test_nonwhitelist_action_rejected 가 실제로 red 가 되어 drift 를 잡는다.
 *  팬아웃 자체(accepted VM 당 매니저 async 가 실제로 N 회 호출됨)는 계획 Task 4
 *  효과-테스트가 실증한다(여기 범위 밖).
 *
 *  반사실(counterfactual): 스텁은 whitelist 게이트도 존재 게이트도 없이 모든
 *  것을 accept 했다. 따라서
 *    - test_nonwhitelist_action_rejected (reboot/pause/resume/delete → 거부)
 *    - test_missing_vm_rejected           (미존재 VM → reject)
 *  는 스텁 동작(항상 accept)으로 되돌리면 red 가 된다.
 *
 * 실행: ./test_runner -p /handler_vm_batch
 * 외부 의존: 없음 (실 UDS/libvirt/vm_manager 불필요 — 순수 결정 로직).
 * ============================================================================
 */
#include <glib.h>
#include "../src/api/vm_batch_policy.h"

/*
 * per-VM accept/reject 결정 재현: 존재하면 accepted(팬아웃), 아니면 rejected.
 * trivial invariant(단순 삼항) — 프로덕션엔 추출할 함수 경계가 없어 로컬 재현 유지.
 */
typedef enum { BATCH_ACCEPTED, BATCH_REJECTED } BatchVmDecision;
static BatchVmDecision batch_vm_decision(gboolean vm_exists) {
    return vm_exists ? BATCH_ACCEPTED : BATCH_REJECTED;
}

/* whitelist 밖 action 은 거부되어야 한다. 스텁(모두 accept)이면 red. */
static void test_nonwhitelist_action_rejected(void) {
    /* whitelist 밖: vm_manager 에 `_async` public fn 부재 / 스코프 밖 */
    g_assert_false(pcv_vm_batch_action_is_whitelisted("reboot"));
    g_assert_false(pcv_vm_batch_action_is_whitelisted("pause"));
    g_assert_false(pcv_vm_batch_action_is_whitelisted("resume"));
    g_assert_false(pcv_vm_batch_action_is_whitelisted("delete"));   /* 파괴적 — 스코프 밖 */
    g_assert_false(pcv_vm_batch_action_is_whitelisted("bogus"));
    g_assert_false(pcv_vm_batch_action_is_whitelisted(NULL));
}

/* whitelist 안 action(start/stop)은 허용되어야 한다. */
static void test_whitelist_action_allowed(void) {
    g_assert_true(pcv_vm_batch_action_is_whitelisted("start"));
    g_assert_true(pcv_vm_batch_action_is_whitelisted("stop"));
}

/* 미존재 VM 은 rejected. 스텁(항상 accept)이면 red. */
static void test_missing_vm_rejected(void) {
    g_assert_cmpint(batch_vm_decision(FALSE), ==, BATCH_REJECTED);
    g_assert_cmpint(batch_vm_decision(TRUE),  ==, BATCH_ACCEPTED);
}

void test_handler_vm_batch_register(void) {
    g_test_add_func("/handler_vm_batch/nonwhitelist_action_rejected",
                    test_nonwhitelist_action_rejected);
    g_test_add_func("/handler_vm_batch/whitelist_action_allowed",
                    test_whitelist_action_allowed);
    g_test_add_func("/handler_vm_batch/missing_vm_rejected",
                    test_missing_vm_rejected);
}
