                                                                             
                                 
                                                                                 
                                                                                        
  
                           
                                                                           
                                                                       
                                                          
                                                                                   
                                                                                      
                                                                          
                                                                      
                                                                                     
                                                                             
                                                         
                                                                                             
                                                                      
                                                              
                                                                    
                                                                            
                                                                                           
                                                                       
                                                              
                                                                     
                                                                                         
  
                                                                   
                                                              
                                                               
                               
                                                                                
#ifndef PURECVISOR_MAINLOOP_GUARD_H
#define PURECVISOR_MAINLOOP_GUARD_H

#include <glib.h>

G_BEGIN_DECLS

                                                               
#define PCV_MAINLOOP_STALL_WARN_US  ((gint64)10 * G_USEC_PER_SEC)
                                                               
#define PCV_MAINLOOP_STALL_CRIT_US  ((gint64)60 * G_USEC_PER_SEC)

                                                              
                                                                                   
                                                                    
                                                                      
typedef struct {
    gboolean warned, critical;
    gint64   stall_start_us;
} PcvStallState;

                                                                           
                                              
typedef enum {
    PCV_STALL_NONE,
    PCV_STALL_WARN,
    PCV_STALL_CRIT,
    PCV_STALL_RECOVERED
} PcvStallVerdict;

                                                                                              
                                                                                      
                                                                                        
                                                                                
                                                             
                                                                     
PcvStallVerdict pcv_mainloop_stall_classify(PcvStallState *st, gint64 now_us, gint64 last_beat_us);

                                                                                         
                                       
                                                        
gboolean pcv_mainloop_deadline_expired(gint64 deadline_us, gint64 now_us);

                                                                                                
                                                                                
                                                                                
                                                                                
                                                                       
                                        
                                                          
void pcv_mainloop_guard_start(GMainLoop *loop, GSourceFunc on_signal);

                                                                                    
                                                                      
                                                                      
void pcv_mainloop_guard_signal_picked_up(void);

                                                                                  
                                                                      
                                                                      
                                                                            
                                                                       
void pcv_mainloop_guard_rpc_enter(const gchar *method);
void pcv_mainloop_guard_rpc_leave(void);

                                                                                         
                                        
                                             
typedef struct {
    gint64 (*now_us)(void);
    void   (*exit_now)(gint code);
} PcvMainloopGuardOps;

                                                                                       
                                                                         
                                                         
                                               
void pcv_mainloop_guard_set_ops_for_test(const PcvMainloopGuardOps *ops);

                                                                                     
                                       
                                                    
void pcv_mainloop_guard_tick_for_test(void);

                                                                                     
                                                                              
                                                    
                                                        
void pcv_mainloop_guard_start_for_test(GMainLoop *loop, GSourceFunc on_signal);

                                                                                     
                                                                                
                                               
void pcv_mainloop_guard_raise_for_test(gint signo);

G_END_DECLS

#endif                                  
