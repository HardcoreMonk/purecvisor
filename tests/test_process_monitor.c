                                                                                        
                                                                             
                                                  
                                                              
                   
                               
  
                                           
  
                                                      
                                                    
                                                    
   
#include <glib.h>

#include "modules/daemons/process_monitor.h"

                                                                    
static void
test_shutdown_is_prompt(void)
{
    pcv_process_monitor_init();
    g_usleep(200 * 1000);

    gint64 t0 = g_get_monotonic_time();
    pcv_process_monitor_shutdown();
    gint64 elapsed_us = g_get_monotonic_time() - t0;

                                                                           
                                                
    g_assert_cmpint(elapsed_us, <, 10 * G_USEC_PER_SEC);
}

                                                 
static void
test_shutdown_without_init_and_twice_is_safe(void)
{
    pcv_process_monitor_shutdown();

    pcv_process_monitor_init();
    pcv_process_monitor_shutdown();
    pcv_process_monitor_shutdown();

                       
    pcv_process_monitor_init();
    pcv_process_monitor_shutdown();
}

void
test_process_monitor_register(void)
{
    g_test_add_func("/process_monitor/shutdown_is_prompt", test_shutdown_is_prompt);
    g_test_add_func("/process_monitor/shutdown_without_init_and_twice_is_safe",
                    test_shutdown_without_init_and_twice_is_safe);
}
