   
                               
                                                                              
  
                           
                                                                                               
                                                                                  
                                                                     
                                      
  
                                                                
              
   
#pragma once

#include <glib.h>

G_BEGIN_DECLS

                                                                                                 
typedef struct {
    gchar *bridge;
    gchar *mode;
    gchar *cidr;
} PcvGuestFwNet;

                                                                    
typedef struct {
    GPtrArray *nets;
} PcvGuestFwSnapshot;

                                                                                       
PcvGuestFwNet *pcv_guest_fw_net_new(const gchar *bridge, const gchar *mode, const gchar *cidr);
void pcv_guest_fw_net_free(gpointer net);

                                                                            
PcvGuestFwSnapshot *pcv_guest_fw_snapshot_new(void);
void pcv_guest_fw_snapshot_free(PcvGuestFwSnapshot *snap);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(PcvGuestFwSnapshot, pcv_guest_fw_snapshot_free)

                         
void pcv_guest_fw_snapshot_add(PcvGuestFwSnapshot *snap, const gchar *bridge, const gchar *mode, const gchar *cidr);

   
                               
                                                                                            
                                 
                                                        
   
gchar *pcv_guest_fw_cidr_to_subnet(const gchar *cidr, GError **error);

   
                             
                                                                 
                                                                          
                                                             
   
gchar *pcv_guest_fw_build_script(const PcvGuestFwSnapshot *snap);

G_END_DECLS
