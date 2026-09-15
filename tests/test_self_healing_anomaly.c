                                                                                     
                                                                                       
                                                                                    

                                                                        

                                    
  
                 
                                                            
                                               
                                                       
  
                                                                            
  
                                     
                                                                        
                                                                         
                                                                                
                                                            
                         
  
                   
                                                                    
                                                       
                                                                
                                                                  
                                                       
                                                          
                                                                    
                                             
                                                              
  
                                                        
                                                                    
                                                                
                                                   
                                                                         
                                              
  
                                                                     
  
                                                                         
  
                                                            
                                                            
                                                             
                                                             
                                                                    
  
                                                                
                                                         
                                                           
                                                          
  
                                    
                                              
   
#include <glib.h>
#include <string.h>
#include "modules/ai/self_healing.h"
#include "modules/ai/anomaly_detector.h"                                         

                                                    
extern gboolean pcv_healing_should_trigger_agent_now(void);

#define HAMMER_THREADS 3
#define HAMMER_ITERS   30000






static gint64 test_monotonic_us = -1;

gint64
pcv_test_healing_monotonic_time(void)
{
    return test_monotonic_us >= 0 ? test_monotonic_us : g_get_monotonic_time();
}

                                                
                                                              
                                                              
             
static void
_ensure_init(void)
{

    g_assert_cmpint(test_monotonic_us, ==, -1);
    static gboolean done = FALSE;
    if (!done) {
        pcv_healing_init();                                                   
        done = TRUE;
    }
}

                                                
                                                 
                                         
static gint
_count_occurrences(const gchar *haystack, const gchar *needle)
{
    gint n = 0;
    for (const gchar *p = haystack; (p = strstr(p, needle)) != NULL; p += strlen(needle))
        n++;
    return n;
}

                                                                   
                                                             
static const char *const MET_A = "aio1_hammer_alpha";
static const char *const MET_B = "aio1_hammer_beta";

static gpointer
_writer(gpointer d)
{
    const char *metric = d;
    for (int i = 0; i < HAMMER_ITERS; i++)
        pcv_healing_on_anomaly(metric, 1.0, 5.0, 2.0, NULL);
    return NULL;
}

static gpointer
_reader(gpointer d)
{
    (void)d;
    for (int i = 0; i < HAMMER_ITERS; i++)
        (void)pcv_healing_should_trigger_agent_now();
    return NULL;
}

                                                           
                                         
static void
test_anomaly_track_race(void)
{
    _ensure_init();                              

    GThread *th[HAMMER_THREADS];
    th[0] = g_thread_new("aio1-w0", _writer, (gpointer)MET_A);
    th[1] = g_thread_new("aio1-w1", _writer, (gpointer)MET_B);
    th[2] = g_thread_new("aio1-rd", _reader, NULL);
    for (int i = 0; i < HAMMER_THREADS; i++)
        g_thread_join(th[i]);

                                                             
                                                               
                                  
    g_assert_true(pcv_healing_get_mode());

                                      
    (void)pcv_healing_should_trigger_agent_now();
    pcv_healing_on_anomaly(MET_A, 1.0, 5.0, 2.0, NULL);
}

                                                                      

                                                                  
                                                        
                                        
static void
test_vm_cooldown_is_per_target(void)
{
    _ensure_init();

                          
    pcv_healing_on_anomaly(PCV_ANOMALY_AUTO_METRIC_MEM_FULL, 42.0, 3.0, 2.5, "fa11-vm-a");
                                                           
    pcv_healing_on_anomaly(PCV_ANOMALY_AUTO_METRIC_MEM_FULL, 43.0, 3.1, 2.5, "fa11-vm-b");

    gchar *hist = pcv_healing_get_history_json();
                                                                     
                                   
    g_assert_cmpint(_count_occurrences(hist, "vm=fa11-vm-a"), ==, 1);
    g_assert_cmpint(_count_occurrences(hist, "vm=fa11-vm-b"), ==, 1);
    g_free(hist);

                                                              
    pcv_healing_on_anomaly(PCV_ANOMALY_AUTO_METRIC_MEM_FULL, 44.0, 3.2, 2.5, "fa11-vm-a");
    hist = pcv_healing_get_history_json();
    g_assert_cmpint(_count_occurrences(hist, "vm=fa11-vm-a"), ==, 1);               
    g_assert_cmpint(_count_occurrences(hist, "vm=fa11-vm-b"), ==, 1);
    g_free(hist);

                                            
                                       
    pcv_healing_on_anomaly(PCV_ANOMALY_AUTO_METRIC_MEM_FULL, 45.0, 3.3, 2.5, "fa11-vm-c");
    hist = pcv_healing_get_history_json();
    g_assert_cmpint(_count_occurrences(hist, "vm=fa11-vm-c"), ==, 1);
    g_free(hist);
}

                                                     
                                                                          
                                                     
                                 
  
                                                                         
                                                                
static void
test_restart_policy_cooldown_is_per_target(void)
{
    _ensure_init();
    g_assert_true(pcv_healing_get_mode());                              

    pcv_healing_on_anomaly("vm-unresponsive", 1.0, 99.0, 0.0, "fa11-uuid-1");
    pcv_healing_on_anomaly("vm-unresponsive", 1.0, 99.0, 0.0, "fa11-uuid-2");
    pcv_healing_on_anomaly("vm-unresponsive", 1.0, 99.0, 0.0, "fa11-uuid-1");            

    gchar *hist = pcv_healing_get_history_json();
                                                        
    g_assert_cmpint(_count_occurrences(hist, "\"target\":\"fa11-uuid-1\""), ==, 1);
    g_assert_cmpint(_count_occurrences(hist, "\"target\":\"fa11-uuid-2\""), ==, 1);
    g_free(hist);
}

                                                  





static void
test_hostwide_cooldown_stays_policy_scoped(void)
{
    _ensure_init();
    g_assert_true(pcv_healing_get_mode());

    const gchar *const CPU_KEY = "\"target\":\"cpu-overload\"";
    const gchar *const MEM_KEY = "\"target\":\"mem-pressure\"";
    const gchar *const DRY_RUN_KEY = "\"result\":\"dry_run\"";
    gchar *before = pcv_healing_get_history_json();
    gint cpu_before = _count_occurrences(before, CPU_KEY);
    gint mem_before = _count_occurrences(before, MEM_KEY);
    gint dry_run_before = _count_occurrences(before, DRY_RUN_KEY);
    g_free(before);
    gchar *pending_before = pcv_healing_get_pending_json();

    const gint64 cooldown_us = 600 * G_USEC_PER_SEC;
    const struct {
        gint64 now_us;
        const gchar *metric;
        gint cpu_added;
        gint mem_added;
    } steps[] = {
        {0, "purecvisor_host_cpu_percent", 1, 0},
        {0, "purecvisor_host_cpu_percent", 1, 0},
        {1, "purecvisor_host_memory_percent", 1, 1},
        {1, "purecvisor_host_memory_percent", 1, 1},
        {cooldown_us - 1, "purecvisor_host_cpu_percent", 1, 1},
        {cooldown_us, "purecvisor_host_cpu_percent", 2, 1},
        {cooldown_us, "purecvisor_host_cpu_percent", 2, 1},
        {cooldown_us, "purecvisor_host_memory_percent", 2, 1},
        {cooldown_us + 1, "purecvisor_host_memory_percent", 2, 2},
        {cooldown_us + 1, "purecvisor_host_memory_percent", 2, 2},
    };

    for (guint i = 0; i < G_N_ELEMENTS(steps); i++) {
        test_monotonic_us = steps[i].now_us;
        g_test_message("step=%u monotonic_us=%" G_GINT64_FORMAT " metric=%s",
                       i, steps[i].now_us, steps[i].metric);
        pcv_healing_on_anomaly(steps[i].metric, 95.0, 4.0, 3.0, NULL);

        gchar *after = pcv_healing_get_history_json();
        g_assert_cmpint(_count_occurrences(after, CPU_KEY), ==,
                        cpu_before + steps[i].cpu_added);
        g_assert_cmpint(_count_occurrences(after, MEM_KEY), ==,
                        mem_before + steps[i].mem_added);
        g_assert_cmpint(_count_occurrences(after, DRY_RUN_KEY), ==,
                        dry_run_before + steps[i].cpu_added + steps[i].mem_added);
        g_free(after);
    }
    test_monotonic_us = -1;

    gchar *pending_after = pcv_healing_get_pending_json();
    g_assert_cmpstr(pending_after, ==, pending_before);
    g_free(pending_after);
    g_free(pending_before);
    g_assert_true(pcv_healing_get_mode());
}

void
test_self_healing_anomaly_register(void)
{
                                                        
                                                 
                                         
    g_test_add_func("/selfhealing/vm_cooldown_per_target", test_vm_cooldown_is_per_target);
    g_test_add_func("/selfhealing/restart_cooldown_per_target",
                    test_restart_policy_cooldown_is_per_target);
    g_test_add_func("/selfhealing/hostwide_cooldown_policy_scoped",
                    test_hostwide_cooldown_stays_policy_scoped);
    g_test_add_func("/selfhealing/anomaly_track_race", test_anomaly_track_race);
}
