   
                        
                                                 
  
                           
                                                   
                                                    
                                        
  
                                                    
                                                             
                                                         
  
                                                            
                                                                             
                                                                             
                                                   
                                                                
  
          
                                                             
                                           
                                                           
  
            
                                                        
                                                                         
                                                             
  
           
                                                 
              
                                                              
                                                               
                                                                            
                                                                   
  
           
                                      
                        
   

#ifndef PCV_JOB_QUEUE_H
#define PCV_JOB_QUEUE_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

   
                
             
   
typedef enum {
    PCV_JOB_PENDING   = 0,                      
    PCV_JOB_RUNNING   = 1,              
    PCV_JOB_COMPLETED = 2,               
    PCV_JOB_FAILED    = 3,            
    PCV_JOB_CANCELLED = 4,                                  
    PCV_JOB_INTERRUPTED = 5                         
} PcvJobStatus;

                                                                 

   
                      
                                               
                                   
                                    
                                                
                                                      
   
void pcv_job_queue_init(void);

   
                          
                                                       
                                                                                   
                                
   
void pcv_job_queue_shutdown(void);

   
                             
                                                     
                                                 
                           
                                           
  
                                                                                
   
void pcv_job_queue_cleanup_old(gint max_age_hours);

                                                                             
#define PCV_JOB_RETENTION_INTERVAL_SECONDS (24 * 3600)

   
                                
                                                            
                                                                          
                                                                                  
                                                                   
                                                 
  
                                                                                  
                         
                                           
                                                         
  
                                                                       
   
gboolean pcv_job_queue_retention_tick(gint64 *last_run, gint64 now, gint retention_days);

                                                           

   
                
                         
                                                     
  
                                       
                                                  
   
JsonArray *pcv_job_list(gint limit);

   
               
                       
                                             
  
                      
                                                                
   
JsonObject *pcv_job_get(const gchar *job_id);

   
                       
                                                                  
                                                          
  
                                                                                    
                                                                          
   
gboolean pcv_job_find_latest(const gchar *method, const gchar *target, JsonObject **out);

   
                  
                             
                                                   
  
                      
                                                       
                                                  
                                                       
   
gboolean pcv_job_cancel(const gchar *job_id);

G_END_DECLS

#endif                      
