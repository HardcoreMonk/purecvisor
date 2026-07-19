
#ifndef PURECVISOR_SELF_HEALING_RESTART_H
#define PURECVISOR_SELF_HEALING_RESTART_H

#include <glib.h>

G_BEGIN_DECLS

/* PCV_SAFETY_CONTROL: self-healing-restart — 워커 스레드에서 실제 virDomainCreate로
 * VM 재시작 실배선 (AF-1, 결정 로직 추출 seam) */

const gchar *pcv_healing_restart_decide(int is_active,
                                        int (*create_fn)(gpointer dom), gpointer dom,
                                        gint *rb_feedback);

G_END_DECLS

#endif
