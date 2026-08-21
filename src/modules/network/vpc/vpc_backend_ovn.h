   
                          
                                                         
  
                           
                                                                      
                                                                       
                                     
  
                                                           
                                              
   
#pragma once

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

JsonObject *pcv_vpc_ovn_backend_capability(const gchar *transit_pool);
gboolean pcv_vpc_ovn_require_ready(const gchar *transit_pool, GError **error);
gboolean pcv_vpc_ovn_ensure_vpc(JsonObject *vpc, JsonObject *binding, GError **error);
gboolean pcv_vpc_ovn_remove_vpc(JsonObject *vpc, JsonObject *binding, GError **error);
gboolean pcv_vpc_ovn_set_egress(JsonObject *vpc, JsonObject *binding, GError **error);
gboolean pcv_vpc_ovn_ensure_subnet(JsonObject *vpc, JsonObject *binding,
                                   JsonObject *subnet, GError **error);
gboolean pcv_vpc_ovn_remove_subnet(JsonObject *vpc, JsonObject *binding,
                                   JsonObject *subnet, GError **error);
gboolean pcv_vpc_ovn_ensure_attachment(JsonObject *vpc, JsonObject *subnet,
                                       JsonObject *attachment, GError **error);
gboolean pcv_vpc_ovn_remove_attachment(JsonObject *vpc, JsonObject *subnet,
                                       JsonObject *attachment, GError **error);
gboolean pcv_vpc_ovn_set_quarantine(JsonObject *vpc, gboolean enabled, GError **error);
gboolean pcv_vpc_ovn_sync_policy(JsonObject *vpc,
                                 JsonArray *attachments,
                                 gboolean quarantine,
                                 GError **error);

G_END_DECLS
