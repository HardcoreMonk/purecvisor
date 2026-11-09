   
                           
                                             
                                                                   
                                                         
                                                       
   
#ifndef PCV_TEST_JOB_QUEUE_LEGACY_H
#define PCV_TEST_JOB_QUEUE_LEGACY_H
#include "utils/pcv_job_queue.h"
                                                       

   
                  
                                                                   
                                                
                                                    
                                                        
                                 
  
                                                          
                                       
                                             
                                                                        
   
gchar *pcv_job_create(const gchar *type, const gchar *target,
                       const gchar *params_json);

   
                         
                        
                                               
                                                                      
  
                            
                           
                                  
                                          
   
gboolean pcv_job_update_status(const gchar *job_id, PcvJobStatus status,
                            gint progress_pct, const gchar *detail);

   
                      
                                                                      
                                                       
                                                        
                                                              
  
                           
                                              
                                           
   
gboolean pcv_job_set_result(const gchar *job_id, PcvJobStatus status,
                         const gchar *result_json);


#endif
