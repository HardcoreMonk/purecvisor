/* src/modules/ai/self_healing_restart.c
 *
 * self-healing-restart 안전통제 결정 로직 구현 (추출 seam). 헤더 주석 참조.
 */
#include "self_healing_restart.h"

/* PCV_SAFETY_CONTROL: self-healing-restart — 워커 스레드에서 실제 virDomainCreate로
 * VM 재시작 실배선 (AF-1, 결정 로직 추출 seam) */
const gchar *
pcv_healing_restart_decide(int is_active,
                           int (*create_fn)(gpointer dom), gpointer dom,
                           gint *rb_feedback)
{
    if (is_active > 0) {
        /* running-guard: 이미 실행 중 → create_fn 호출 안 함. VM 이 건강하다는
         * 신호이므로 브레이커를 성공으로 취급해 리셋한다. */
        *rb_feedback = +1;
        return "skipped";
    }
    if (create_fn(dom) == 0) {
        *rb_feedback = +1;
        return "success";
    }
    *rb_feedback = -1;   /* 진짜 재시작 실패(create_fn) → 브레이커 카운트 */
    return "failed";
}
