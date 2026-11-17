#ifndef PURECVISOR_VM_DIAG_VIEW_H
#define PURECVISOR_VM_DIAG_VIEW_H

   
                       
                                                                     
  
                           
                                                                          
                                                                               
                                                     
                                                                       
                                                                               
                                                                                         
  
                       
                                                        
                                                     
          
  
        
                                                  
                                                                 
                                                             
        
                                                   
                                       
   

#include <glib.h>
#include <json-glib/json-glib.h>

#include "vm_diag.h"

G_BEGIN_DECLS

   
                                                         
                       
                                           
                                                          
   
void pcv_vm_diag_store_publish(PcvVmDiagSnapshot *take);

   
                               
                                               
                                                       
   
PcvVmDiagSnapshot *pcv_vm_diag_store_copy(void);

   
                                                                             
  
                                                                       
                                       
  
                                      
                                                         
                                                               
                                                                  
   
gboolean pcv_vm_diag_store_guest_mem(const gchar *uuid, gdouble *guest_used_mb_out, gdouble *alloc_mb_out);

   
                                    
                                                                                 
                                                    
                                                                
   
gint64 pcv_vm_diag_store_age_ms(gint64 now_mono_us);

   
                            
                                                                                 
                                           
   
typedef struct {
    gchar *name;                     
    gchar *uuid;                    
    gchar *owner;                                                
    gint   domain_id;                                 
} PcvVmDiagOwnerRow;

   
                                                                                  
                                                                    
                                                                                               
                                                                
                                                    
                                                                            
   
GPtrArray *pcv_vm_diag_store_owner_rows(gint64 now_mono_us);

                                                                                
#define PCV_VM_DIAG_PROM_GAUGE          0
#define PCV_VM_DIAG_PROM_SOURCE_COUNTER 2

   
                                  
                                                  
                               
                                   
                   
                                                                          
                         
   
typedef void (*PcvVmDiagPromFn)(const gchar *name, const gchar *labels, gdouble value,
                                gint kind, gpointer ud);

   
                                                 
  
                                                 
  
                                                                                      
                                                                                                  
                                           
                                                                                                  
                                                                                  
                                                                    
                                                                                                       
                                                                                                           
                                                                               
                                                                              
                                                             
                                                                                                                                 
                                                                                                                  
                                                                                            
  
                                                  
                                                                
                                  
                                       
                                                              
   
void pcv_vm_diag_prom_series(const PcvVmDiagSnapshot *s, guint vm_cap, PcvVmDiagPromFn fn,
                             gpointer ud, guint *prom_truncated_out);

   
                          
                                                                            
  
                                                                
                          
  
                                                     
                                                                                                  
                                                                                          
                                                                 
                             
   
typedef struct {
    gboolean all;
    gboolean (*visible)(const PcvVmDiagConfig *cfg, gpointer ud);
    gpointer ud;
} PcvVmDiagViewer;

   
                                                                     
  
                                                                   
                                
  
      
                                                                                
                                                                  
                                                                                 
                                                                                              
                                                                          
                                           
                                                             
                                       
  
                                                  
                                                        
                                      
                                                    
                                                                                
   
JsonNode *pcv_vm_diag_view_json(const PcvVmDiagSnapshot *s, const PcvVmDiagViewer *v,
                                const gchar *vm_filter, gint64 now_mono_us);

G_END_DECLS

#endif                                
