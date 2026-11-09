   
                     
                                        
  
                           
                                                                                       
                                                                             
                                                            
                                 
  
                                                                                                  
   
#ifndef PCV_REST_ASYNC_H
#define PCV_REST_ASYNC_H

#include <glib.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include "api/rest_server.h"
#include "api/rest_public_render.h"

                                                           
                           
void
pcv_rest_on_msg_finished(SoupServerMessage *msg, gpointer user_data __attribute__((unused)));
void
pcv_rest_async_ctx_free(gpointer p);
void
pcv_rest_async_worker(GTask *task, gpointer src __attribute__((unused)),
                   gpointer task_data, GCancellable *cancel __attribute__((unused)));

                                                                    
typedef void (*PcvRestRenderFn)(gpointer data, PcvRestRendered *out);
                                                                        
void
pcv_rest_send_rendered(SoupServerMessage *msg, const PcvRestRendered *r);
                                                                                                        
                                                                                        
void
pcv_rest_defer_rendered(SoupServerMessage *msg, GMainContext *ctx, PcvRestRenderFn fn, gpointer data, GDestroyNotify destroy);

#endif                       
