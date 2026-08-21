                                                                                     
                                                                                                 
                                                                   
                                                          
                                    
                                                         
                                                       
#include <glib.h>
#include <libvirt/libvirt.h>
#include "../src/modules/daemons/pcv_vm_death_class.h"

static void test_anomaly_crash_class_true(void) {
                                                                   
    g_assert_true(pcv_vm_death_is_anomaly(VIR_DOMAIN_EVENT_STOPPED,
                                          VIR_DOMAIN_EVENT_STOPPED_CRASHED));
    g_assert_true(pcv_vm_death_is_anomaly(VIR_DOMAIN_EVENT_STOPPED,
                                          VIR_DOMAIN_EVENT_STOPPED_FAILED));
                                      
    g_assert_true(pcv_vm_death_is_anomaly(VIR_DOMAIN_EVENT_CRASHED,
                                          VIR_DOMAIN_EVENT_CRASHED_PANICKED));
    g_assert_true(pcv_vm_death_is_anomaly(VIR_DOMAIN_EVENT_CRASHED, 999));
}

static void test_anomaly_deliberate_false(void) {
                                  
    g_assert_false(pcv_vm_death_is_anomaly(VIR_DOMAIN_EVENT_STOPPED,
                                           VIR_DOMAIN_EVENT_STOPPED_SHUTDOWN));
    g_assert_false(pcv_vm_death_is_anomaly(VIR_DOMAIN_EVENT_STOPPED,
                                           VIR_DOMAIN_EVENT_STOPPED_DESTROYED));
    g_assert_false(pcv_vm_death_is_anomaly(VIR_DOMAIN_EVENT_STOPPED,
                                           VIR_DOMAIN_EVENT_STOPPED_MIGRATED));
    g_assert_false(pcv_vm_death_is_anomaly(VIR_DOMAIN_EVENT_STOPPED,
                                           VIR_DOMAIN_EVENT_STOPPED_SAVED));
    g_assert_false(pcv_vm_death_is_anomaly(VIR_DOMAIN_EVENT_STOPPED,
                                           VIR_DOMAIN_EVENT_STOPPED_FROM_SNAPSHOT));
}

static void test_anomaly_unrelated_events_false(void) {
                          
    g_assert_false(pcv_vm_death_is_anomaly(VIR_DOMAIN_EVENT_STARTED, 0));
    g_assert_false(pcv_vm_death_is_anomaly(VIR_DOMAIN_EVENT_DEFINED, 0));
    g_assert_false(pcv_vm_death_is_anomaly(VIR_DOMAIN_EVENT_UNDEFINED, 0));
}

void test_vm_death_class_register(void) {
    g_test_add_func("/vm_death_class/crash_class_true", test_anomaly_crash_class_true);
    g_test_add_func("/vm_death_class/deliberate_false", test_anomaly_deliberate_false);
    g_test_add_func("/vm_death_class/unrelated_false", test_anomaly_unrelated_events_false);
}
