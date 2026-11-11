#include <glib.h>

static const char *
_suricata_gate_skip_reason(void)
{
    if (geteuid() != 0)
        return "root 필요 — systemctl kill/restart + /var/log/suricata 쓰기(.50 실측 §3)";
    gchar *p = g_find_program_in_path("suricata");
    if (!p)
        return "suricata 미설치 — 통합 실경로 스킵(이 개발 머신 기본 상태)";
    g_free(p);
    return NULL;
}

static void
test_suricata_engine_down_restart_root(void)
{
    const char *skip = _suricata_gate_skip_reason();
    if (skip) { g_test_skip(skip); return; }

                                                   
    if (pcv_suricata_probe() != PCV_SURICATA_ACTIVE) {
        g_test_skip("suricata.service 비활성 — 재기동 관측 불가(먼저 systemctl start suricata)");
        return;
    }

                                                               
                                                          
                                                        
                          
    if (system("systemctl kill --signal=SIGKILL suricata >/dev/null 2>&1") != 0) {
        g_test_skip("systemctl kill 실패 — 크래시 주입 불가");
        return;
    }

                                               
    gboolean went_down = FALSE;
    gint64 d1 = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
    while (g_get_monotonic_time() < d1) {
        if (pcv_suricata_probe() == PCV_SURICATA_FAILED) { went_down = TRUE; break; }
        g_usleep(200 * 1000);
    }
    g_assert_true(went_down);                                             

                                                      
                                                     
    pcv_suricata_health_start();

                                        
    gboolean recovered = FALSE;
    gint64 d2 = g_get_monotonic_time() + 60 * G_USEC_PER_SEC;
    while (g_get_monotonic_time() < d2) {
        if (pcv_suricata_probe() == PCV_SURICATA_ACTIVE) { recovered = TRUE; break; }
        g_usleep(500 * 1000);
    }

    pcv_suricata_health_stop();

    g_assert_true(recovered);                         
}

void test_suricata_integration_pre_fix_register(void) {
    g_test_add_func("/suricata/integration/engine_down_restart", test_suricata_engine_down_restart_root);
}
