   
                            
                                             
  
                           
                                                                       
                                                                    
  
                                                                            
   
#ifndef PCV_DISPATCHER_SHARED_H
#define PCV_DISPATCHER_SHARED_H

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include "api/uds_server.h"

                                                                                  
                           
const gchar *
pcv_disp_json_string_member(JsonObject *params, const gchar *key);
gboolean
pcv_disp_valid_vm_storage_pool(const gchar *pool);
gboolean
pcv_disp_json_int_member(JsonObject *params, const gchar *key, gint *out);
const gchar *
pcv_disp_dispatcher_caller_subject(JsonObject *params, GSocketConnection *connection);
gint
pcv_disp_dispatcher_caller_role(JsonObject *params, GSocketConnection *connection);

                                                                                                     
                                                                              
                              
void
pcv_disp_webhook_member_for_caller(JsonObject *object, const gchar *member,
                                   JsonObject *params, GSocketConnection *connection);
JsonNode *
pcv_disp_paginate_array(JsonArray *full_array, gint offset, gint limit);
void
pcv_disp_get_pagination_params(JsonObject *params, gint *out_offset, gint *out_limit);
void
pcv_disp_wipe_json_string_member(JsonObject *object, const gchar *key);

#endif                              
