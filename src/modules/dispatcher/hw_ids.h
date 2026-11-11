   
                 
                                                                                 
  
                           
                                                                                               
                                                                                                       
                                                                                                       
                                                                                                  
                                                      
                                                       
                                                                         
  
                                                                                              
                                
  
            
                                                                                                                    
                                                                                           
   
#ifndef PURECVISOR_DISPATCHER_HW_IDS_H
#define PURECVISOR_DISPATCHER_HW_IDS_H

#include <glib.h>

G_BEGIN_DECLS

                                                                                
                                                   
#define PCV_HW_IDS_MAX_BYTES (16U * 1024U * 1024U)

                                              
typedef enum {
    PCV_HW_IDS_PCI,
    PCV_HW_IDS_USB,
} PcvHwIdsKind;

                                                 
typedef struct PcvHwIds PcvHwIds;

   
                                                                                           
                                                                               
                            
                                       
   
PcvHwIds *pcv_hw_ids_load(const gchar *root, PcvHwIdsKind kind);

                                                                                  
                               
const gchar *pcv_hw_ids_vendor(const PcvHwIds *ids, const gchar *vendor_hex4);

                                                                                               
                                                                 
const gchar *pcv_hw_ids_device(const PcvHwIds *ids, const gchar *vendor_hex4, const gchar *device_hex4);

                                                                                       
                                                                                          
gboolean pcv_hw_ids_available(const PcvHwIds *ids);

                                                         
void pcv_hw_ids_free(PcvHwIds *ids);

G_END_DECLS

#endif                                     
