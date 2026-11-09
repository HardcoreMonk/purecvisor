   
                         
                                           
  
                           
                                                                                       
                                                                             
                                                            
                                 
  
                                                                                           
   
#ifndef PCV_REST_TRANSPORT_H
#define PCV_REST_TRANSPORT_H

#include <glib.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include "api/rest_server.h"

                                                               
                           
gboolean
pcv_rest_transport_load_certificate(gpointer data, const gchar *endpoint,
                            GError **error);
gboolean
pcv_rest_transport_create_tls_context(gpointer data, const gchar *endpoint,
                              GError **error);
gboolean
pcv_rest_transport_listen_plaintext(gpointer data, const gchar *endpoint,
                            GError **error);
gboolean
pcv_rest_transport_listen_https(gpointer data, const gchar *endpoint,
                        GError **error);

#endif                           
