   
                  
                                                       
  
                           
                                                              
                                                                                  
                                                         
                                                                            
                                                                                    
                                                                                  
                                                                            
                                                              
                                                                     
                                                                   
                                                                     
                                                     
  
                       
                                                          
                                                                  
  
                                                            
                                                                      
                     
                                                           
                                            
   
#ifndef PURECVISOR_WS_SEND_H
#define PURECVISOR_WS_SEND_H

#include <glib.h>
#include <libsoup/soup.h>
#include "api/drain.h"

G_BEGIN_DECLS

   
                                                           
  
                                                               
                                                                     
                                               
                                    
   
static inline gboolean
pcv_ws_send_text_if_open(SoupWebsocketConnection *conn, const gchar *text)
{
    if (!conn || !text)
        return FALSE;
    if (soup_websocket_connection_get_state(conn) != SOUP_WEBSOCKET_STATE_OPEN)
        return FALSE;                                                                    
    soup_websocket_connection_send_text(conn, text);
    return TRUE;
}

   
                                                                      
  
                                                         
                                    
      
                                                                                    
                                                                                     
                                                                      
                                                   
                                                                  
                                
                                                                
   
static inline gboolean
pcv_ws_dispatch_on_owner(GMainContext *owner, GSourceFunc fn, gpointer data, GDestroyNotify notify)
{
    if (!owner || !fn)
        return FALSE;
    if (g_main_context_is_owner(owner)) {
        fn(data);
        if (notify)
            notify(data);
        return TRUE;
    }
    return pcv_drain_try_invoke(owner, G_PRIORITY_DEFAULT, fn, data, notify);
}

G_END_DECLS

#endif                           
