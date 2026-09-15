   
                      
                                 
  
                           
                                                   
                                                    
                                        
  
                                                      



  
                                                                       
       
                                                   
                                   
  
                             
                                                
                            
  
              


  
                     
                                 
                                                 
                                            
                                       
  
                                     
                                          
                                             
  
             


  
          
                                                     
                                                       
                                                
  
          

  
           
                                                       
                                                         
  

                                                

                                                                       
   
#ifndef PURECVISOR_OVS_OVERLAY_H
#define PURECVISOR_OVS_OVERLAY_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS



#define PCV_OVERLAY_VNI_MIN 1
#define PCV_OVERLAY_VNI_MAX 16777215











gboolean pcv_overlay_validate_name(const gchar *name);
gboolean pcv_overlay_validate_vni(gint64 vni);
gboolean pcv_overlay_validate_peer_ip(const gchar *peer_ip);
gboolean pcv_overlay_validate_cidr(const gchar *cidr);











gchar *pcv_overlay_peer_port_name(gint64 vni, const gchar *peer_ip);













typedef gboolean (*PcvOverlayExecFn)(const gchar * const *argv,
                                     gchar **stdout_out,
                                     GError **error);

void pcv_overlay_set_test_context(PcvOverlayExecFn exec_fn, const gchar *meta_dir);
typedef void (*PcvOverlayRestoreSnapshotHook)(const gchar *meta_path,
                                              gpointer user_data);

void pcv_overlay_set_restore_snapshot_test_hook(PcvOverlayRestoreSnapshotHook hook,
                                                gpointer user_data);
typedef void (*PcvOverlayLifecycleTestHook)(const gchar *operation,
                                            gpointer user_data);

void pcv_overlay_set_lifecycle_test_hook(PcvOverlayLifecycleTestHook hook,
                                         gpointer user_data);
typedef void (*PcvOverlayMetadataTestHook)(const gchar *overlay_name,
                                           const gchar *canonical_path,
                                           const gchar *claim_path,
                                           gpointer user_data);

void pcv_overlay_set_metadata_test_hook(PcvOverlayMetadataTestHook hook,
                                        gpointer user_data);
typedef gboolean (*PcvOverlayDirSyncTestHook)(GError **error,
                                              gpointer user_data);

void pcv_overlay_set_dir_sync_test_hook(PcvOverlayDirSyncTestHook hook,
                                        gpointer user_data);
typedef gboolean (*PcvOverlayMetadataStatTestHook)(const gchar *path,
                                                   GError **error,
                                                   gpointer user_data);

void pcv_overlay_set_metadata_stat_test_hook(
    PcvOverlayMetadataStatTestHook hook, gpointer user_data);

void pcv_overlay_set_metadata_limit_for_test(gsize max_bytes);

void pcv_overlay_set_restore_deadline_for_test(gint64 milliseconds);

                                          

   
                                   

  
                                   

   
void pcv_overlay_init(const gchar *local_tunnel_ip);

   
                                      
  
                                  
                                           
   
void pcv_overlay_shutdown(void);

   

  
                                                                         




   
void pcv_overlay_restore(void);

   


  


                                                                                   
   
void pcv_overlay_reconcile(void);


                                                     
                                                            

                                                       
void pcv_overlay_reconcile_timer_init(void);

                                                         

void pcv_overlay_reconcile_timer_shutdown(void);

                                    

   
                                             
                                                     
                                        
                                           
                                                           
                    

   
gboolean    pcv_overlay_create(const gchar *name, gint vni, const gchar *cidr, GError **error);

   
                                         
                     
                    


   
gboolean    pcv_overlay_delete(const gchar *name, GError **error);

   
                                          


   
JsonArray  *pcv_overlay_list(GError **error);

   
                                       
                 


   
JsonObject *pcv_overlay_info(const gchar *name, GError **error);



   
                                              


                 

                    
  



   
gboolean pcv_overlay_add_peer(const gchar *name, const gchar *peer_tunnel_ip, GError **error);

   
                                                 
                 
                             
                    


   
gboolean pcv_overlay_remove_peer(const gchar *name, const gchar *peer_tunnel_ip, GError **error);

G_END_DECLS

#endif                               
