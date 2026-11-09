   
                
                                                          
  
                           
                                                                         
                                                              
                                                                
                                                      
  
                                                 
                                                   
                          
  
                                                               
                                                          
                                                                      
   

#ifndef PURECVISOR_DRAIN_H
#define PURECVISOR_DRAIN_H

#include <gio/gio.h>

G_BEGIN_DECLS

                                                               
                                                    
                                                 
GTask *pcv_drain_task_new(gpointer source, GCancellable *cancel,
                        GAsyncReadyCallback callback, gpointer data);
void pcv_drain_track_task(GTask *task);
                                                                                    
                                                                              
                                        
void pcv_drain_task_set_actor(GTask *task, const gchar *actor);
                                                        
                                                
const gchar *pcv_drain_task_actor_of(gpointer task_or_result);
                                                                                       
                                         
#define pcv_drain_task_actor(x) \
    _Generic((x), GTask *: pcv_drain_task_actor_of, GAsyncResult *: pcv_drain_task_actor_of)(x)
                                                                                                     
                                         
void pcv_drain_work_acquire(void);
void pcv_drain_work_release(void);
gint pcv_drain_get_work(void);
gboolean pcv_drain_is_terminating(void);
gboolean pcv_drain_admit_rest(void);

                                                    
                                                        
void pcv_drain_invoke(GMainContext *context, GSourceFunc callback, gpointer data);
void pcv_drain_idle(GMainContext *context, GSourceFunc callback,
                    gpointer data, GDestroyNotify destroy);
guint pcv_drain_timeout(guint milliseconds, GSourceFunc callback,
                        gpointer data, GDestroyNotify destroy);
                                                                      
                                                                              
                                                                   
                                                                                    
                                                                                    
                                                                   
gboolean pcv_drain_try_invoke(GMainContext *context, gint priority, GSourceFunc callback,
                              gpointer data, GDestroyNotify destroy);

   
                  
                                       
                                  
   
void pcv_drain_init(void);

   
                 
                                                              
                                 
                                               
   
gboolean pcv_drain_inc(void);

   
                 
                                                            
                                        
                                           
   
void pcv_drain_dec(void);

   
                         
                                      
                                       
   
gboolean pcv_drain_is_shutdown(void);

   
                          
                                    
                             
   
gint pcv_drain_get_inflight(void);

   
                             
                                                
                                                 
                                                   
                                 
   
void pcv_drain_notify_stopping(void);

   
                   
                                                        
                           
                                              
  
                                                                                
                                                           
                                                              
   
void pcv_drain_begin(GMainLoop *loop, guint timeout_sec);

   
                          
                                             
                                              
                                      
   
void pcv_drain_notify_ready(void);

   
                             
                                                
                                                 
                                               
                                 
   
void pcv_drain_notify_watchdog(void);

   
                               
                                                     
                                                                
                                      
   
guint64 pcv_drain_get_watchdog_usec(void);

   
                    
                                                 
                             
                                                     
                                                               
   
void pcv_drain_cancel(void);

   
                      
                                                
                                                           
   
void pcv_drain_shutdown(void);

G_END_DECLS

#endif                         
