#ifndef PURECVISOR_PROMETHEUS_EXPORTER_H
#define PURECVISOR_PROMETHEUS_EXPORTER_H

   
                              
                                                                   
  
                           
                                                   
                                                    
                                        
  
                                                    
                                                    
                                                       
                          
  
                                                                              
         
                                                                              
  
                                                              
                         
  
                        
                                                               
                                                        
  
                                                                              
                                        
                                                                              
  
                                                             
  
                                         
                        
  
                                   
                                              
  
                                                                              
             
                                                                              
  
                                          
                                                                               
                                    
  
                                          
                                                                    
                                                                      
  
                                                                              
         
                                                                              
  
                                
                                                                         
                                                                                  
  
                                
                                                                 
  
                                        
                                                                 
  
                                                                              
           
                                                                              
  
                                              
                                                                      
                                                                       
   

#include <glib.h>
#include <string.h>                                                        

G_BEGIN_DECLS

  
                                                
                                                                                                     
                                                                                                             
                                                                                         
                                                                      
   
#define PCV_PROM_MAX_METRICS 8192

   
                                                                 
  
                                                         
                                                       
  
                                                             
                                                                           
   
#define PCV_PROM_VM_SERIES_MAX 64

   
                                          
  
                                                  
                                                 
                                      
  
                                                              
                                               
                                                                     
                                                                           
                                          
                                                        
                                                   
                                             
                                                               
   
#define PCV_PROM_LABELS_MAX 256

   
                                                         
  
                                               
                                
  
                                                             
                                                  
                                                          
                                            
  
                                                                   
                                                                               
                            
  
                                                        
                                                        
   
static inline gboolean
pcv_prom_labels_fit(const gchar *labels)
{
    if (!labels) return TRUE;                             
    return strlen(labels) < PCV_PROM_LABELS_MAX;
}

   
                                                                                             
                                                                                             
  
                                                                       
                                                                    
  
                                                                                                               
                                                                              
                                                                                                   
                                                                            
  
                                
                            
                                                                           
                             
   
G_GNUC_NULL_TERMINATED gboolean pcv_prom_labels_build(gchar *buf, gsize cap, ...);

   
                                                                                                            
                                                                                                               
                                                                            
                
                                                                                         
   
gint pcv_prom_labels_vm_value(const gchar *line, gchar *out, gsize cap);

                

                                                                          
                                   
void pcv_prom_init(void);

   
                                                               
  
                                                         
                                                           
  
                                                            
                                                                     
                                                      
                                                                                 
                                                                                             
                                                                                        
                                                                           
                                                             
   
void pcv_prom_flush(void);

                      

   
                            
                                            
                                                                
                                    
                                    
  
                                                                            
   
void pcv_prom_inc(const gchar *name, const gchar *label_key, const gchar *label_val);

                    

   
                                       
                                            
                           
                                         
                                         
                          
   
void pcv_prom_gauge_set(const gchar *name, const gchar *label_key,
                         const gchar *label_val, gdouble value);

   
                                           
                                                    
  
                                                   
                        
                                                                         
                       
  
                                                                  
                                                     
  
                                                                            
                                                                                                         
   
void pcv_prom_gauge_set_labels(const gchar *name, const gchar *labels,
                                gdouble value);

   
                                                                      
  
                                                                            
                                                
  
                                   
                                                                                                     
   
void pcv_prom_inc_labels(const gchar *name, const gchar *labels);

   
                                                                        
  
                                                                                  
                                              
  
                                                          
                    
                               
                                               
                                                                                           
  
                                    
   
typedef enum {
    PCV_PROM_KIND_GAUGE = 0,
    PCV_PROM_KIND_COUNTER,
    PCV_PROM_KIND_SOURCE_COUNTER,
} PcvPromKind;

   
                                               
  
                                                                               
                                                                    
  
                                                                                  
                                                                                 
                        
   
void pcv_prom_source_counter_set_labels(const gchar *name, const gchar *labels, gdouble value);

   
                                                
  
                                                    
                                                  
                                                    
  
                                                                          
                                                         
                                                       
                                                             
                                                   
                                                       
  
                                                                   
                                                           
                    
  
                                      
                                                                    
   
static inline gboolean
pcv_prom_labels_are_high_cardinality(const gchar *labels)
{
    if (!labels) return FALSE;
    return strstr(labels, "vm=") != NULL ||
           strstr(labels, "vm_id=") != NULL ||
           strstr(labels, "vm_name=") != NULL;
}

                                         

                                                    
                                                       
void pcv_prom_rpc_start(const gchar *method);

   
                                                                   
                                                    
                                             
                                                                
                                    
   
void pcv_prom_rpc_end(const gchar *method, gboolean success, gdouble duration_ms);

   
                                                        
                                                        
                                                           
                                                              
                                        
                                         
   
void pcv_prom_zfs_inflight_lock_observe(const gchar *pool_name,
                                        const gchar *op,
                                        const gchar *result,
                                        gdouble wait_ms);

                                     

   
                                                            
                                                        
                                                   
  
                                                 
                                 
   
gchar *pcv_prom_render(void);

G_END_DECLS

#endif                                       
