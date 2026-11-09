   
                              
                                                                                             
  
                           
                                                                                         
                                                                                               
                                                                         
                                                                                                  
                                                                           
                                                                              
  
                                                                              
                                                            
   
#include "cli_container_guard.h"
#include "cli_rpc.h"
#include "modules/lxc/lxc_volume_guard.h"

#include <glib.h>

                                                                                   
void
cmd_container_volume_guard_hook(int argc G_GNUC_UNUSED, char *argv[] G_GNUC_UNUSED)
{
    if (pcv_lxc_volume_guard_hook_main(g_getenv("LXC_CONFIG_FILE"), g_getenv("LXC_NAME")) == 0)
        pcv_cli_command_mark_success();
    else
        pcv_cli_command_mark_runtime_failure();
}
