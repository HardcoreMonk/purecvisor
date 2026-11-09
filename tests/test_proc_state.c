   
                          
                                                                 
  
                           
                                                          
                                                          
                           
                                      
  
                       
                                                         
            
   
#include <glib.h>
#include <unistd.h>

#include "utils/pcv_proc_state.h"

static void
test_parses_comm_with_parens(void)
{
    g_assert_cmpint(pcv_proc_stat_state("123 (a) b) D 1 2 3"), ==, 'D');
    g_assert_cmpint(pcv_proc_stat_state("1 (systemd) S 0 1 1"), ==, 'S');
}

static void
test_zombie_is_not_alive_text(void)
{
    g_assert_cmpint(pcv_proc_stat_state("77 (defunct) Z 1 77 77"), ==, 'Z');
}

static void
test_self_is_alive(void)
{
    g_assert_true(pcv_proc_pid_alive((GPid)getpid()));
    g_assert_false(pcv_proc_pid_alive((GPid)0));
    g_assert_false(pcv_proc_pid_alive((GPid)-5));
}

static void
test_garbage_returns_nul(void)
{
    g_assert_cmpint(pcv_proc_stat_state(NULL), ==, 0);
    g_assert_cmpint(pcv_proc_stat_state(""), ==, 0);
    g_assert_cmpint(pcv_proc_stat_state("no parens here"), ==, 0);
    g_assert_cmpint(pcv_proc_stat_state("1 (x)"), ==, 0);
    g_assert_cmpint(pcv_proc_stat_state("1 (x) 9"), ==, 0);
}

void
test_proc_state_register(void)
{
    g_test_add_func("/proc_state/parses_comm_with_parens", test_parses_comm_with_parens);
    g_test_add_func("/proc_state/zombie_is_not_alive_text", test_zombie_is_not_alive_text);
    g_test_add_func("/proc_state/self_is_alive", test_self_is_alive);
    g_test_add_func("/proc_state/garbage_returns_nul", test_garbage_returns_nul);
}
