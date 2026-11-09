                                                                             
                             
                                                                             
  
                           
                                                                      
                                                                      
                                                                    
  
                                                              
                                                                                
#ifndef PURECVISOR_PCV_PROC_STATE_H
#define PURECVISOR_PCV_PROC_STATE_H

#include <glib.h>

G_BEGIN_DECLS

                                                
gchar pcv_proc_stat_state(const gchar *stat_text);
                                                        
gboolean pcv_proc_pid_alive(GPid pid);

G_END_DECLS

#endif                                  
