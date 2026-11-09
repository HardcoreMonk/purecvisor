   
                       
                                             
  
                           
                                                                                       
                                                                             
                                                            
                                 
  
                                                                                               
   
#ifndef PCV_REST_REQUEST_H
#define PCV_REST_REQUEST_H

#include <glib.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include "api/rest_server.h"

                                                             
                           
JsonObject *
pcv_rest_parse_body(SoupServerMessage *msg);
gchar **
pcv_rest_split_path(const gchar *path);
gchar *
pcv_rest_build_rpc(const gchar *method, JsonObject *params);
gchar *
pcv_rest_build_rpc_name(const gchar *method, const gchar *name);
gchar *
pcv_rest_rpc_attach_auth_context(const gchar *rpc_json,
                         const gchar *subject,
                         PcvRole      role);
JsonObject *
pcv_rest_vpc_params(JsonObject *body,
                 GHashTable *query,
                 const gchar *id_key,
                 const gchar *id_value);

                                                                                                            
JsonObject *
pcv_rest_audit_search_params(SoupServerMessage *message, GHashTable *query, JsonObject *body);
gboolean
pcv_rest_body_has_secret(const gchar *body, gsize len);

#endif                         
