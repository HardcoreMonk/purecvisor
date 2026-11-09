                                                                                      
                                                                                         
                                                                                             
                                                                      
                                                               
                                                                     
                              
                            
  
                                                                                                
  
                 
                                                                   
                                                   
  
                                     
                                    
   
#include <glib.h>
#include <libvirt/libvirt.h>
#include "modules/virt/vm_start_capacity.h"

void test_vm_start_pin_register(void);

                                                                                    
                                                                     
static void test_info_active_states(void)
{
    static const struct { int state; gboolean active; } table[] = {
        { VIR_DOMAIN_NOSTATE,     TRUE  },
        { VIR_DOMAIN_RUNNING,     TRUE  },
        { VIR_DOMAIN_BLOCKED,     TRUE  },
        { VIR_DOMAIN_PAUSED,      TRUE  },
        { VIR_DOMAIN_SHUTDOWN,    TRUE  },
        { VIR_DOMAIN_SHUTOFF,     FALSE },
        { VIR_DOMAIN_CRASHED,     TRUE  },
        { VIR_DOMAIN_PMSUSPENDED, TRUE  },
    };
    g_assert_cmpint(G_N_ELEMENTS(table), ==, 8);
    for (guint i = 0; i < G_N_ELEMENTS(table); i++) {
        g_assert_cmpint(table[i].state, ==, (int)i);                       
        g_assert_cmpint(pcv_vm_start_info_active(table[i].state), ==, table[i].active);
    }
}

                                             
                                                                 
static void test_pin_count_table(void)
{
    static const struct { guint requested, domain_vcpus, expected; } table[] = {
        { 0, 4, 4 },                           
        { 0, 0, 1 },                          
        { 1, 4, 1 },                                       
        { 6, 4, 6 },                     
    };
    for (guint i = 0; i < G_N_ELEMENTS(table); i++)
        g_assert_cmpuint(pcv_vm_start_pin_count(table[i].requested, table[i].domain_vcpus), ==,
                         table[i].expected);
}

void test_vm_start_pin_register(void)
{
    g_test_add_func("/vm_start_pin/info_active_states", test_info_active_states);
    g_test_add_func("/vm_start_pin/pin_count_table", test_pin_count_table);
}
