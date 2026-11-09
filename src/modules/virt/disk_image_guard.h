                                                                             
                                      
                                                                          
  
                           
                                                   
                                                    
                                        
                                                                                   
                                                                     
                                                                                                
                                                                              
  
                                                                
                                        
  
                                                                        
                                                                                       
                                                                                        
                                                                                  
                                                  
                                       
                                        
                                                                                

#ifndef PURECVISOR_DISK_IMAGE_GUARD_H
#define PURECVISOR_DISK_IMAGE_GUARD_H

#include <glib.h>

G_BEGIN_DECLS

                                                        
#define PCV_DISK_IMAGE_INFO_TIMEOUT_SEC 30

                                 
typedef enum {
    PCV_DISK_IMAGE_BASE,                                               
    PCV_DISK_IMAGE_OVA_VMDK                                                                          
} PcvDiskImageKind;

   
                             
                                                
                                                    
                         
                                                                          
                                                                      
  
                          
                                                  
                     
   
gboolean pcv_disk_image_judge_info(const gchar *info_json, const gchar *path, PcvDiskImageKind kind,
                                   gchar **format_out, GError **error);

   
                          
                             
                         
                                                                          
                                                      
  
                                                                   
                                                     
                     
   
gboolean pcv_disk_image_inspect(const gchar *path, PcvDiskImageKind kind, gchar **format_out, GError **error);

   
                          
                                  
                                  
                                                                
  
                                             
                                                
                                     
   
gboolean pcv_disk_image_recheck(const gchar *path, const gchar *expected_format, GError **error);

   
                               
                                                              
                                           
                            
                                                            
                                               
  
                                                  
                        
   
gboolean pcv_disk_image_convert_into(const gchar *source, const gchar *source_format,
                                     const gchar *target_format, const gchar *target, GError **error);

   
                                
                                         
                                              
  
                                                                
                                           
                                                               
   
gchar *pcv_disk_image_driver_format(const gchar *domain_xml, const gchar *target_dev);

   
                              
                         
                                                                                           
                      
                                         
  
                                                                                            
                                                                                   
                                                      
                     
   
gboolean pcv_disk_image_resize_file(const gchar *path, const gchar *format, gint size_gb, GError **error);

G_END_DECLS

#endif                                    
