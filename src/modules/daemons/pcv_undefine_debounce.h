  
                                                    
                           
                                                         
                                                        
                       
                                                    
                                          
   
#ifndef PURECVISOR_PCV_UNDEFINE_DEBOUNCE_H
#define PURECVISOR_PCV_UNDEFINE_DEBOUNCE_H
#include <glib.h>
G_BEGIN_DECLS

  
                                                                        
  
                           
                                                   
                                                    
                                        
  
                                                  
                                                 
                                                  
                                            
  
                                              
                                                
                                            
                                                              
                                                                
                                                                         
                                                    
                                                    
                         
                         
   
typedef struct PcvUndefineDebounce PcvUndefineDebounce;

                                                          
PcvUndefineDebounce *pcv_undefine_debounce_new(void);
                                                           
                                                         
                                                 
                                                             
void                 pcv_undefine_debounce_free(PcvUndefineDebounce *d);

                                                                         
                       
                                                   
                                               
guint  pcv_undefine_debounce_note_undefined(PcvUndefineDebounce *d,
                                            const char *uuid, const char *name,
                                            guint timer_token);

                                                                          
                                                       
                                            
guint  pcv_undefine_debounce_note_defined(PcvUndefineDebounce *d, const char *uuid);

                                                                        
                                                
                                                         
gchar *pcv_undefine_debounce_take_expired(PcvUndefineDebounce *d, const char *uuid);

                                                            
void test_undefine_debounce_register(void);

G_END_DECLS
#endif
