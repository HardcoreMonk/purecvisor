   
                            
                                           
  
                           
                                                                                       
                                                                             
                                                            
                                 
  
                                                                                             
   
#ifndef PCV_REST_REQUEST_AUTH_H
#define PCV_REST_REQUEST_AUTH_H

#include <glib.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include "api/rest_server.h"

                                                                  
                           
gchar *
pcv_rest_authenticate(SoupServerMessage *msg, gint *out_key_role, gint *out_subject_role);
gboolean
pcv_rest_cors_origin_allowed(const gchar *origin, const gchar *host_hdr);

#endif                              
