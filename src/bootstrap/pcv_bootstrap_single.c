   
                                                                    
  
                           
                                                   
                                                    
                                        
  
                                                    
                                                    
                                         
  
                                                                      
                                                  
                                                          
                                                     
                                                                  
                                                                                      
                                                                                 
                                                   
                                         
                                                                       
                                                               
                                                       
                                
                                                       
                                     
                                                             
                                                        
                                                         
                                                          
                                                        
  
            
                                               
                                                     
                                                     
               
  
           
                                                 
                                                  
                                            
   
#include "pcv_bootstrap.h"

#include "modules/network/ovn_manager.h"
#include "modules/network/ovs_overlay.h"
#include "modules/network/network_manager.h"                                    
#include "modules/network/network_firewall.h"                                        
#include "modules/network/network_dhcp.h"                                    
#include "utils/pcv_config.h"
#include "utils/pcv_validate.h"                                               

  
                                                                
                                                  
                                                
   
const gchar *
pcv_bootstrap_get_daemon_binary_path(void)
{
    return "/usr/local/bin/purecvisorsd";
}

  
                                                                      
                                                   
                                                               
   
void
pcv_bootstrap_init_cluster_manager(void)
{
    g_message("[init] Single Edge mode — cluster manager bootstrap skipped");
}

  
                                                                      
                                               
                             
   
void
pcv_bootstrap_init_scheduler_proxy(void)
{
    g_message("[init] Single Edge mode — scheduler/proxy bootstrap skipped");
}

  
                                                                   
                                           
                             
   
void
pcv_bootstrap_init_federation(void)
{
    g_message("[init] Single Edge mode — federation bootstrap skipped");
}

  
                                                                  
                                                         
                                                       
                                                 
                                                                 
   
void
pcv_bootstrap_init_runtime_network(void)
{
                                                                      
                                                          
                                                       
    GError *physical_err = NULL;
    if (!pcv_network_reconcile_physical_bridges(&physical_err)) {
        g_warning("[init] physical bridge reconcile fail-closed: %s",
                  physical_err ? physical_err->message : "unknown");
        g_clear_error(&physical_err);
    }

                     
                                                      
                                                     
                                                    
                                                             
                                                       
                                             
                                                        
      
                                                                    
                                         
                                                                
                                                                           
                                                       
                                                        
                                                          
                             
                                                         
                                                     
                                            
    if (pcv_config_get_int("network", "default_ensure", 1)) {
        const gchar *def_br   = pcv_config_get_string("network", "default_bridge", "pcvnat0");
        const gchar *def_cidr = pcv_config_get_string("network", "default_subnet", "10.78.0.1/24");
        GError *net_err = NULL;

        gchar *sys_path = g_strdup_printf("/sys/class/net/%s", def_br);
        gboolean br_exists = g_file_test(sys_path, G_FILE_TEST_IS_DIR);
        g_free(sys_path);

        if (!br_exists) {
            if (network_bridge_create(def_br, def_cidr, 1500, &net_err)) {
                g_message("[init] default NAT bridge '%s' created (%s)", def_br, def_cidr);
                br_exists = TRUE;
                                                                
                pcv_network_meta_save(def_br, "nat", def_cidr);
            } else {
                g_warning("[init] default bridge create failed: %s",
                          net_err ? net_err->message : "unknown");
                g_clear_error(&net_err);
            }
        }

        if (br_exists) {
            gchar *nat_mark = g_strdup_printf(PCV_NETWORK_RUNDIR "/nat-%s.ok", def_br);
            if (!g_file_test(nat_mark, G_FILE_TEST_EXISTS)) {
                if (network_firewall_setup_nat(def_br, def_cidr, &net_err)) {
                    if (!g_file_set_contents(nat_mark, "ok\n", -1, NULL))
                        g_warning("[init] NAT marker write failed: %s", nat_mark);
                    g_message("[init] default NAT rules applied on '%s'", def_br);
                } else {
                    g_warning("[init] default NAT setup failed (재시작 시 재시도): %s",
                              net_err ? net_err->message : "unknown");
                    g_clear_error(&net_err);
                }
            }
            g_free(nat_mark);

                                                     
                                                           
                                                              
                                                           
                                                
              
                                                              
                                                          
                                                                    
                                                                          
            if (network_dhcp_start_ex(def_br, def_cidr, TRUE, NULL, &net_err)) {
                g_message("[init] default network DHCP+DNS converged on '%s'", def_br);
            } else {
                g_warning("[init] default DHCP convergence failed: %s",
                          net_err ? net_err->message : "unknown");
                g_clear_error(&net_err);
            }
        }
    }

                                                                     
    if (pcv_ovn_is_available()) {
        GError *ovn_local_err = NULL;
        if (pcv_ovn_single_prepare_local(&ovn_local_err)) {
            g_message("[init] OVN local controller prepared in Single Edge");
        } else {
            g_warning("[init] OVN local controller prepare failed: %s",
                      ovn_local_err ? ovn_local_err->message : "unknown");
            g_clear_error(&ovn_local_err);
        }
    }

                                                                  
    const gchar *ovl_br = pcv_config_get_string("overlay", "default_bridge", "");
    if (!ovl_br || !*ovl_br)                                         
        return;

                                                                 
    GError *ovl_err = NULL;
    pcv_overlay_create(ovl_br,
                       pcv_config_get_int("overlay", "default_vni", 100),
                       pcv_config_get_string("overlay", "default_cidr", ""),
                       &ovl_err);
    if (ovl_err) {
        g_warning("Overlay auto-create: %s", ovl_err->message);
        g_error_free(ovl_err);
    }

    g_message("OVS overlay '%s' auto-provisioned (VNI=%d)",
              ovl_br,
              pcv_config_get_int("overlay", "default_vni", 100));
}

  
                                                                         
                                                
                      
   
void
pcv_bootstrap_shutdown_cluster_stack(void)
{
}
