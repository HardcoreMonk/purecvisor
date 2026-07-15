/* tests/test_self_healing_restart.c
 *
 * 대상 모듈: src/modules/ai/self_healing_restart.c — self-healing-restart
 *            결정 로직 추출 seam (running-guard + virDomainCreate 3분기).
 *
 * 이 테스트가 검증하는 것 (효과 테스트 = 무동작→실동작):
 *   정지 VM 에 restart 액션이 실행되면 실제로 create_fn(도메인 기동)이
 *   호출되는지, 그리고 running-guard 가 이미 실행 중인 VM 에 대해서는
 *   create_fn 을 아예 호출하지 않는지 — 스파이(spy_calls)로 직접 관찰한다.
 *
 * 반사실 (load-bearing, running-guard):
 *   pcv_healing_restart_decide() 의 `if (is_active > 0) { ... return "skipped"; }`
 *   분기를 제거하면, is_active=1 케이스에서도 create_fn 이 호출되어
 *   spy_calls==1 이 되고 test_running_guard_skip 이 RED 된다
 *   (실증: 2026-07 게이트 작업 리포트 참조 — 임시 제거 → make test → RED → 복원).
 *
 * 실행: ./test_runner -p /self_healing_restart
 * 외부 의존: 없음 (libvirt 비의존 — 결정 로직만 추출된 소형 TU).
 */

#include <glib.h>
#include "modules/ai/self_healing_restart.h"

static int spy_calls;
static int spy_ret;

static int
spy_create(gpointer dom)
{
    (void)dom;
    spy_calls++;
    return spy_ret;
}

static void
spy_reset(int ret)
{
    spy_calls = 0;
    spy_ret = ret;
}

/* ── running-guard (핵심 반사실) ─────────────────────────────
 * is_active=1(이미 실행 중) → "skipped", create_fn 미호출, rb_feedback=+1. */
static void
test_running_guard_skip(void)
{
    spy_reset(0);
    gint rb_feedback = -99;
    const gchar *result = pcv_healing_restart_decide(1, spy_create, NULL, &rb_feedback);

    g_assert_cmpstr(result, ==, "skipped");
    g_assert_cmpint(spy_calls, ==, 0);   /* create_fn 이 호출되지 않아야 함 */
    g_assert_cmpint(rb_feedback, ==, +1);
}

/* ── 정지 + 성공 ─────────────────────────────────────────── */
static void
test_stopped_create_success(void)
{
    spy_reset(0);   /* create_fn → 0 = 성공 */
    gint rb_feedback = -99;
    const gchar *result = pcv_healing_restart_decide(0, spy_create, NULL, &rb_feedback);

    g_assert_cmpstr(result, ==, "success");
    g_assert_cmpint(spy_calls, ==, 1);
    g_assert_cmpint(rb_feedback, ==, +1);
}

/* ── 정지 + 실패 ─────────────────────────────────────────── */
static void
test_stopped_create_failure(void)
{
    spy_reset(-1);  /* create_fn → 비0 = 실패 */
    gint rb_feedback = -99;
    const gchar *result = pcv_healing_restart_decide(0, spy_create, NULL, &rb_feedback);

    g_assert_cmpstr(result, ==, "failed");
    g_assert_cmpint(spy_calls, ==, 1);
    g_assert_cmpint(rb_feedback, ==, -1);
}

void
test_self_healing_restart_register(void)
{
    g_test_add_func("/self_healing_restart/running_guard_skip", test_running_guard_skip);
    g_test_add_func("/self_healing_restart/stopped_create_success", test_stopped_create_success);
    g_test_add_func("/self_healing_restart/stopped_create_failure", test_stopped_create_failure);
}
