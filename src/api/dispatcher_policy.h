   
                            
                                                                  
  
                           
                                                                       
                                                                    
  
                                                                    
   
#ifndef PCV_DISPATCHER_POLICY_H
#define PCV_DISPATCHER_POLICY_H

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include "api/uds_server.h"

                                                                                  
                           
void pcv_dispatcher_init_policy_map(void);
gboolean pcv_dispatcher_check_rbac(const gchar *method, gint caller_role);
gboolean
pcv_disp_vm_method_requires_owner_scope(const gchar *method);
gboolean pcv_dispatcher_vm_target_aliases_valid(const gchar *method, JsonObject *params,
                                                const gchar **reason_out);

#endif                              
