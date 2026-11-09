   
                        
                                                     
  
                           
                                                                                       
                                                                             
                                                            
                                 
  
                                                                                              
   
#ifndef PCV_REST_RESPONSE_H
#define PCV_REST_RESPONSE_H

#include <glib.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include "api/rest_server.h"

                                                              
                           
gchar *
pcv_rest_rpc_over_uds_timeout(const gchar *rpc_json, gint timeout_sec);
gchar *
pcv_rest_rpc_over_uds(const gchar *rpc_json);
void
pcv_rest_send_json(SoupServerMessage *msg, guint status, const gchar *body);
void
pcv_rest_send_error(SoupServerMessage *msg, guint status,
       const gchar *code, const gchar *detail);
void
pcv_rest_send_rpc_result(SoupServerMessage *msg, const gchar *rpc_resp);

#endif                          
