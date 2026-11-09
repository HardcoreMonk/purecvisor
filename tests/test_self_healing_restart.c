                                                                                  
                                                                                     
                               
                                                                
                         
                                    
  
                                                                      
                                                                  
  
                                    
                                                    
                                                  
                                                       
  
                                     
                                                                                  
                                                  
                                                       
                                                                
  
                                             
                                              
   

#include <glib.h>
#include <gio/gio.h>
#include <string.h>
#include "modules/ai/self_healing.h"
#include "modules/ai/self_healing_restart.h"


void pcv_test_vm_start_set_result(PcvVmStartSubmit r);
guint pcv_test_vm_start_call_count(void);
gboolean pcv_test_vm_start_last_skip_if_active(void);
const gchar *pcv_test_vm_start_last_origin(void);
const gchar *pcv_test_vm_start_last_actor(void);
gboolean pcv_test_vm_start_last_on_thread(GThread *thread);

                                                          
                                                                      
static void
test_running_guard_skip(void)
{
    gint fb = 99;
    g_assert_cmpstr(pcv_healing_restart_outcome(PCV_VM_START_ALREADY_ACTIVE, FALSE, NULL, &fb), ==, "skipped");
    g_assert_cmpint(fb, ==, 1);
}

                                                            
static void
test_stopped_create_success(void)
{
    gint fb = 99;
    g_assert_cmpstr(pcv_healing_restart_outcome(PCV_VM_START_SUBMITTED, TRUE, NULL, &fb), ==, "success");
    g_assert_cmpint(fb, ==, 1);
}

                                                            
static void
test_stopped_create_failure(void)
{
    gint fb = 99;
    GError *e = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, "create failed");
    g_assert_cmpstr(pcv_healing_restart_outcome(PCV_VM_START_SUBMITTED, FALSE, e, &fb), ==, "failed");
    g_assert_cmpint(fb, ==, -1);
    g_error_free(e);
}

static void
test_rejected_is_skipped_without_feedback(void)
{
    gint fb = 99;
    g_assert_cmpstr(pcv_healing_restart_outcome(PCV_VM_START_REJECTED, FALSE, NULL, &fb), ==, "skipped");
    g_assert_cmpint(fb, ==, 0);
}

static void
test_non_vm_faults_do_not_trip_breaker(void)
{
    const gint codes[] = { G_IO_ERROR_BUSY, G_IO_ERROR_NOT_FOUND, G_IO_ERROR_NOT_CONNECTED };
    for (guint i = 0; i < G_N_ELEMENTS(codes); i++) {
        gint fb = 99;
        GError *e = g_error_new_literal(G_IO_ERROR, codes[i], "x");
        g_assert_cmpstr(pcv_healing_restart_outcome(PCV_VM_START_SUBMITTED, FALSE, e, &fb), ==, "failed");
        g_assert_cmpint(fb, ==, 0);
        g_error_free(e);
    }
}

static void
test_failure_without_error_counts(void)
{
    gint fb = 99;
    g_assert_cmpstr(pcv_healing_restart_outcome(PCV_VM_START_SUBMITTED, FALSE, NULL, &fb), ==, "failed");
    g_assert_cmpint(fb, ==, -1);
}

#define DISPATCH_TEST_VM "11111111-2222-3333-4444-555555555555"

static gpointer
_trigger_from_thread(gpointer data)
{
    (void)data;
    pcv_healing_on_anomaly("vm-unresponsive", 1.0, 99.0, 0.0, DISPATCH_TEST_VM);
    return NULL;
}

static gint
_count_occurrences(const gchar *haystack, const gchar *needle)
{
    gint n = 0;
    for (const gchar *p = haystack; (p = strstr(p, needle)) != NULL; p += strlen(needle))
        n++;
    return n;
}

static void
test_restart_dispatch_runs_on_main_loop(void)
{
    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 10 * G_USEC_PER_SEC, 0);
        g_test_trap_assert_passed();
        return;
    }

    pcv_healing_init();
    pcv_healing_set_mode(FALSE);
    g_assert_false(pcv_healing_get_mode());

    pcv_test_vm_start_set_result(PCV_VM_START_ALREADY_ACTIVE);
    guint before = pcv_test_vm_start_call_count();
    GThread *t = g_thread_new("anomaly", _trigger_from_thread, NULL);
    g_thread_join(t);
    g_assert_cmpuint(pcv_test_vm_start_call_count(), ==, before);
    while (g_main_context_iteration(NULL, FALSE)) { }
    g_assert_cmpuint(pcv_test_vm_start_call_count(), ==, before + 1);
    g_assert_true(pcv_test_vm_start_last_on_thread(g_thread_self()));
    g_assert_true(pcv_test_vm_start_last_skip_if_active());
    g_assert_cmpstr(pcv_test_vm_start_last_origin(), ==, "self-healing");
    g_assert_cmpstr(pcv_test_vm_start_last_actor(), ==, "system");

    gchar *hist = pcv_healing_get_history_json();
    g_assert_cmpint(_count_occurrences(hist, "\"target\":\"" DISPATCH_TEST_VM "\""), ==, 1);
    g_assert_cmpint(_count_occurrences(hist, "\"result\":\"skipped\""), ==, 1);
    g_free(hist);
}

static void
test_alert_only_spares_rate_budget(void)
{
    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 10 * G_USEC_PER_SEC, 0);
        g_test_trap_assert_passed();
        return;
    }
    pcv_healing_init();
    pcv_healing_set_mode(FALSE);
    pcv_test_vm_start_set_result(PCV_VM_START_ALREADY_ACTIVE);

    pcv_healing_on_anomaly("purecvisor_host_cpu_percent", 95.0, 4.0, 3.0, NULL);
    pcv_healing_on_anomaly("purecvisor_host_memory_percent", 95.0, 4.0, 3.0, NULL);
    pcv_healing_on_anomaly("vm-reboot-loop", 1.0, 99.0, 0.0, NULL);

    const gchar *vms[] = {"heal-rate-vm-a", "heal-rate-vm-b", "heal-rate-vm-c", "heal-rate-vm-d"};
    guint base = pcv_test_vm_start_call_count();
    for (guint i = 0; i < G_N_ELEMENTS(vms); i++) {
        pcv_healing_on_anomaly("vm-unresponsive", 1.0, 99.0, 0.0, vms[i]);
        while (g_main_context_iteration(NULL, FALSE)) { }
        guint want = base + (i < 3 ? i + 1 : 3);
        g_assert_cmpuint(pcv_test_vm_start_call_count(), ==, want);
    }
}

void
test_self_healing_restart_register(void)
{
    g_test_add_func("/self_healing_restart/running_guard_skip", test_running_guard_skip);
    g_test_add_func("/self_healing_restart/stopped_create_success", test_stopped_create_success);
    g_test_add_func("/self_healing_restart/stopped_create_failure", test_stopped_create_failure);
    g_test_add_func("/self_healing_restart/rejected_is_skipped_without_feedback",
                    test_rejected_is_skipped_without_feedback);
    g_test_add_func("/self_healing_restart/non_vm_faults_do_not_trip_breaker",
                    test_non_vm_faults_do_not_trip_breaker);
    g_test_add_func("/self_healing_restart/failure_without_error_counts",
                    test_failure_without_error_counts);
    g_test_add_func("/self_healing_restart/restart_dispatch_runs_on_main_loop",
                    test_restart_dispatch_runs_on_main_loop);
    g_test_add_func("/self_healing_restart/alert_only_spares_rate_budget",
                    test_alert_only_spares_rate_budget);
}
