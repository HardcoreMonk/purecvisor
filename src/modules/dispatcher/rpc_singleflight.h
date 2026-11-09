                                                                             
                                            
                                                                             
                                                            
  
                           
                                                               
                                                                 
                                                                       
                                                              
                                                              
                                                                      
                                                                  
                         
  
                                                             
                                                                                
#ifndef PURECVISOR_RPC_SINGLEFLIGHT_H
#define PURECVISOR_RPC_SINGLEFLIGHT_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct PcvSingleflight PcvSingleflight;

                                                  
PcvSingleflight *pcv_singleflight_new(GDestroyNotify waiter_free);
                                      
void pcv_singleflight_free(PcvSingleflight *sf);
                                                                       
gboolean pcv_singleflight_join(PcvSingleflight *sf, const gchar *key, gpointer waiter);
                                                                             
GPtrArray *pcv_singleflight_complete(PcvSingleflight *sf, const gchar *key);
                                                        
                                                                   
guint pcv_singleflight_waiter_count(PcvSingleflight *sf, const gchar *key);

G_END_DECLS

#endif                                    
