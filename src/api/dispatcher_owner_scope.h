   
                                 
                                                                   
  
                           
                                                                       
                                                                    
  
                                                        
   
#ifndef PCV_DISPATCHER_OWNER_SCOPE_H
#define PCV_DISPATCHER_OWNER_SCOPE_H

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include "api/uds_server.h"

                                                                                       
                           
const gchar *
pcv_disp_vm_owner_scope_target_from_params(const gchar *method, JsonObject *params);
gboolean
pcv_disp_vm_owner_scoped_method_allowed(const gchar *method,
                                JsonObject *params,
                                GSocketConnection *connection,
                                gint caller_role,
                                gchar **deny_message);
const gchar *
pcv_disp_container_owner_scope_target_from_params(const gchar *method, JsonObject *params);
const gchar *
pcv_disp_dispatcher_audit_target_from_params(const gchar *method, JsonObject *params);
gboolean
pcv_disp_container_method_requires_owner_scope(const gchar *method);
gboolean
pcv_disp_container_owner_scoped_method_allowed(const gchar *method,
                                       JsonObject *params,
                                       GSocketConnection *connection,
                                       gint caller_role,
                                       gchar **deny_message);

#endif                                   
