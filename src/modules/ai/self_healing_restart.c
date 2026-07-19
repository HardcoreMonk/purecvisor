
#include "self_healing_restart.h"

/* PCV_SAFETY_CONTROL: self-healing-restart — 워커 스레드에서 실제 virDomainCreate로
 * VM 재시작 실배선 (AF-1, 결정 로직 추출 seam) */
const gchar *
pcv_healing_restart_decide(int is_active,
                           int (*create_fn)(gpointer dom), gpointer dom,
                           gint *rb_feedback)
{
    if (is_active > 0) {

        *rb_feedback = +1;
        return "skipped";
    }
    if (create_fn(dom) == 0) {
        *rb_feedback = +1;
        return "success";
    }
    *rb_feedback = -1;
    return "failed";
}
