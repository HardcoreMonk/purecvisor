                                                                             
                            
                                                                              
  
                           
                                                                                                   
                                                                                                 
                                                                                      
                                                                                   
                                                                                  
                                                                                                                         
                                                                                
                                                                              
  
                                                                              
                                            
                                                                                
#ifndef PURECVISOR_PCV_PCI_SYSFS_H
#define PURECVISOR_PCV_PCI_SYSFS_H

#include <glib.h>

G_BEGIN_DECLS

   
                           
                                                                               
                                                                        
                                                                                                             
                                                                                            
                                     
  
                                                                                      
                                                                                  
                                                                                     
  
                                                                            
                  
  
                                                        
   
gboolean pcv_pci_iommu_group_dir(const gchar *root, const gchar *bdf, gchar **group_dir_out, gchar **group_id_out,
                                 GError **error);

G_END_DECLS

#endif                                 
