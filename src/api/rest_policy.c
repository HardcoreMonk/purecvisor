   
                      
                                            
  
                           
                                                                                                                                           
                                                                                                         
                                                                                                           
                                               
  
                                                               
   

#include "api/drain.h"
#include "api/rest_shutdown.h"
#include "api/rest_server.h"
#include "api/rest_middleware.h"
#include "api/rest_auth.h"
#include "api/rest_client_identity.h"
#include "utils/pcv_crypto.h"
#include "utils/pcv_jwt.h"
#include "utils/pcv_totp.h"
#include "utils/pcv_log.h"
#include "modules/daemons/prometheus_exporter.h"
#include "api/ws_server.h"
#include "utils/pcv_config.h"
#include "utils/pcv_tls.h"
#include "utils/pcv_bpf.h"
#include "utils/pcv_secure.h"
#include "modules/dispatcher/rpc_utils.h"
#if PCV_CLUSTER_ENABLED
#include "../modules/cluster/cluster_manager.h"
#endif
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <libvirt/libvirt.h>
#include <stdlib.h>
#include <limits.h>
#include <errno.h>
#include <string.h>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <glib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <unistd.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <zlib.h>
#include "modules/network/ovn_manager.h"
#include "modules/network/dpdk_manager.h"
#include "utils/pcv_spawn.h"
#include "modules/auth/pcv_rbac.h"
#include "modules/audit/pcv_audit.h"
#include "modules/virt/circuit_breaker.h"
#include "purecvisor/version.h"

                                                               
                                                       
                                                       
                                                                   
PcvRestTlsMode
pcv_rest_tls_mode_from_config(gboolean https_enabled)
{
    return https_enabled ? PCV_REST_TLS_INTERNAL
                         : PCV_REST_TLS_EXTERNAL_TERMINATION;
}

   
                                                      
  
                                                  
                                                         
                                                            
  
                                                  
                                                 
                                                
                                                   
  
                                                   
                                                           
                                                                
                  
                                                                  
                                               
                                                     
                                                        
                                            
   
PcvRestTlsHealth
pcv_rest_tls_health(PcvRestTlsMode mode,
                    gboolean tls_enabled,
                    gboolean tls_degraded,
                    const gchar *degraded_reason)
{
    if (mode == PCV_REST_TLS_EXTERNAL_TERMINATION) {
        return (PcvRestTlsHealth) {
            .mode = "external_termination",
            .enabled = FALSE,
            .degraded = FALSE,
            .status = "disabled_by_config",
            .reason = "external TLS termination required",
        };
    }

    return (PcvRestTlsHealth) {
        .mode = "internal",
        .enabled = tls_enabled,
        .degraded = tls_degraded,
        .status = tls_degraded ? "degraded" : "ok",
        .reason = tls_degraded ? degraded_reason : NULL,
    };
}

   
                                                                
  
                                                                 
                                                                                     
                                                
                                                        
                              
  
                                                 
                                           
  
                                                    
                                                            
                                   
   
PcvRestAuditHealthImpact
pcv_rest_audit_chain_health_impact(gboolean current_ok,
                                   gboolean historical_break)
{
    if (!current_ok)
        return PCV_REST_AUDIT_HEALTH_CRITICAL;
    if (historical_break)
        return PCV_REST_AUDIT_HEALTH_INFORMATIONAL;
    return PCV_REST_AUDIT_HEALTH_NONE;
}

                                                                                            
                                                            
gboolean
pcv_rest_method_is_user_admin(const gchar *method)
{
    return g_strcmp0(method, "auth.user.create") == 0 ||
           g_strcmp0(method, "auth.user.delete") == 0 ||
           g_strcmp0(method, "auth.role.set") == 0;
}

   
                                                                       
  
                                              
                                                  
                    
  
                                            
                                   
   
guint
pcv_rest_http_status_for_rpc_error(gint64 rpc_error_code)
{
    if (rpc_error_code == PURE_RPC_ERR_INVALID_PARAMS)
        return 400;
    if (rpc_error_code == PURE_RPC_ERR_METHOD_NOT_FOUND ||
        rpc_error_code == PURE_RPC_ERR_VM_NOT_FOUND ||
        rpc_error_code == PURE_RPC_ERR_NOT_FOUND)
        return 404;
    if (rpc_error_code == PURE_RPC_ERR_CONFLICT ||
        rpc_error_code == PURE_RPC_ERR_BUSY ||
        rpc_error_code == PURE_RPC_ERR_HOSTKEY_UNTRUSTED ||                       
        rpc_error_code == PURE_RPC_ERR_HOSTKEY_MISMATCH)
        return 409;
    if (rpc_error_code == PURE_RPC_ERR_TIMEOUT)
        return 504;
    if (rpc_error_code == PURE_RPC_ERR_FORBIDDEN)
        return 403;
    if (rpc_error_code == PURE_RPC_ERR_TOTP_INVALID_CODE)
        return 401;
    if (rpc_error_code == PURE_RPC_ERR_TOTP_LOCKED)
        return 429;
    return 500;
}

   
                                                               
  
                                                        
                                                                  
                                                               
                                                       
                                                    
  
                                                   
                                              
  
                                                          
                                     
                                                         
                                
                                                                   
                     
   
PcvRestRpcMethodStatus
pcv_rest_rpc_envelope_method(const gchar *rpc_json, gchar **method_out)
{
    g_return_val_if_fail(method_out != NULL, PCV_REST_RPC_METHOD_MALFORMED);
    *method_out = NULL;
    if (!rpc_json)
        return PCV_REST_RPC_METHOD_MALFORMED;

    PcvRestRpcMethodStatus status = PCV_REST_RPC_METHOD_MALFORMED;
    JsonParser *parser = json_parser_new();
                                                                                                                                            
    if (json_parser_load_from_data(parser, rpc_json, -1, NULL)) {
        JsonNode *root = json_parser_get_root(parser);
        JsonObject *obj = (root && JSON_NODE_HOLDS_OBJECT(root))
                          ? json_node_get_object(root) : NULL;
        JsonNode *member = obj ? json_object_get_member(obj, "method") : NULL;
        if (obj && !member) {
            status = PCV_REST_RPC_METHOD_MISSING;
        } else if (member && JSON_NODE_HOLDS_VALUE(member) &&
                   json_node_get_value_type(member) == G_TYPE_STRING) {
            const gchar *method = json_node_get_string(member);
            if (method && *method) {
                *method_out = g_strdup(method);
                status = PCV_REST_RPC_METHOD_OK;
            }
        }
    }
    g_object_unref(parser);
    return status;
}

   
                                                            
  
                                                
                                                    
                                                             
  
                                                   
                                                    
                    
  
                                                          
                                                                 
                                                          
                                              
                                           
                                                 
                                          
                                                                           
                                   
   
PcvRestTransportPlan
pcv_rest_transport_plan(PcvRestTlsMode mode,
                        const gchar *configured_plaintext_bind)
{
    gboolean internal = mode == PCV_REST_TLS_INTERNAL;
    const gchar *bind_mode =
        internal && g_strcmp0(configured_plaintext_bind, "all") == 0
            ? "all" : "loopback";

    return (PcvRestTransportPlan) {
        .mode = mode,
        .initialize_tls = internal,
        .load_certificate = internal,
        .create_tls_context = internal,
        .listen_https = internal,
        .plaintext_bind_mode = bind_mode,
        .plaintext_host = g_strcmp0(bind_mode, "all") == 0
                            ? "0.0.0.0" : "127.0.0.1",
    };
}

                                      
                                                    
                                            
                                       
                                                         
                                       
void
pcv_rest_transport_initialize(const PcvRestTransportPlan *transport,
                              PcvRestTlsInitFunc init_func)
{
    g_return_if_fail(transport != NULL);
    g_return_if_fail(init_func != NULL);

    if (transport->initialize_tls)
        init_func();
}

   
                                                        
  
                                                  
                                                
                                   
  
                                                    
                                                  
                                                          
                                                   
  
                     
                                                       
                                    
                                                          
                                                    
                                                                                
                                                                   
                               
                                                             
                                                    
                     
                           
                                                                
                                              
                                                
                                 
                                                        
                                               
   
gboolean
pcv_rest_transport_start(const PcvRestTransportPlan *transport,
                         const PcvRestTransportOps *ops,
                         gpointer context,
                         PcvRestTransportOutcome *outcome,
                         GError **error)
{
    g_return_val_if_fail(transport != NULL, FALSE);
    g_return_val_if_fail(ops != NULL, FALSE);
    g_return_val_if_fail(outcome != NULL, FALSE);

    *outcome = (PcvRestTransportOutcome) {0};
    GError *step_error = NULL;

    if (transport->load_certificate) {
        outcome->certificate_loaded =
            ops->load_certificate(context, NULL, &step_error);
        g_clear_error(&step_error);
    }
    if (transport->create_tls_context && outcome->certificate_loaded) {
        outcome->tls_context_created =
            ops->create_tls_context(context, NULL, &step_error);
        g_clear_error(&step_error);
    }

    outcome->plaintext_listening =
        ops->listen_plaintext(context, transport->plaintext_host, error);
    if (!outcome->plaintext_listening)
        return FALSE;

    if (transport->listen_https &&
        outcome->certificate_loaded &&
        outcome->tls_context_created) {
        outcome->https_listening =
            ops->listen_https(context, "0.0.0.0", &step_error);
        g_clear_error(&step_error);
    }
    return TRUE;
}

                                
                                                   
                                                    
                                                                      
                                                 
const gchar *
pcv_rest_auth_audit_address(SoupServerMessage *message)
{
    g_return_val_if_fail(SOUP_IS_SERVER_MESSAGE(message), "unknown");

    const PcvRestClientIdentity *identity =
        pcv_rest_client_identity_get(message);
    return pcv_rest_client_identity_get_client_ip(identity);
}

   
                                                              
  
                                                                 
                                              
                                           
  
                                                 
                                    
                                                    
                                                           
                                      
   
gchar *
pcv_rest_rate_limit_key_for_message(SoupServerMessage *message,
                                    const gchar *path,
                                    const gchar *http_method)
{
    return pcv_build_rate_limit_key(
        pcv_rest_auth_audit_address(message), path, http_method);
}

                        
PcvCollectorAuthMode
pcv_rest_collector_auth_mode_parse(const gchar *value, gboolean *recognized)
{
    g_autofree gchar *v = g_strstrip(g_strdup(value ? value : ""));
    if (recognized)
        *recognized = TRUE;
    if (g_ascii_strcasecmp(v, "loopback") == 0)
        return PCV_COLLECTOR_AUTH_LOOPBACK;
    if (g_ascii_strcasecmp(v, "required") == 0)
        return PCV_COLLECTOR_AUTH_REQUIRED;
    if (g_ascii_strcasecmp(v, "none") == 0)
        return PCV_COLLECTOR_AUTH_NONE;
    if (recognized)
        *recognized = FALSE;
    return PCV_COLLECTOR_AUTH_REQUIRED;
}

                        
PcvCollectorAuthMode
pcv_rest_collector_auth_mode_from_config(gboolean *recognized)
{
    g_autofree gchar *raw = pcv_config_dup_raw_value("metrics", "auth");
    return pcv_rest_collector_auth_mode_parse(raw ? raw : "loopback", recognized);
}

                        
gboolean
pcv_rest_collector_needs_auth(PcvCollectorAuthMode mode, gboolean direct_local)
{
    switch (mode) {
    case PCV_COLLECTOR_AUTH_NONE:
        return FALSE;
    case PCV_COLLECTOR_AUTH_LOOPBACK:
        return !direct_local;
    case PCV_COLLECTOR_AUTH_REQUIRED:
    default:
        return TRUE;
    }
}

   
                                                                            
  
                                                                      
                                                                   
                                                            
                                                           
                                                                   
  
                                                                            
                                                     
  
                                                    
                               
                                                       
                                    
                                                                 
                                                 
   
JsonObject *
pcv_rest_container_volume_detach_params(const gchar *name, gchar **segments, JsonObject *body)
{
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", name ? name : "");

    if (segments && segments[0] && *segments[0]) {
        for (guint i = 0; segments[i]; i++) {
            if (!*segments[i])
                return params;                                     
        }
        g_autofree gchar *joined = g_strjoinv("/", segments);
        g_autofree gchar *container_path = g_strconcat("/", joined, NULL);
        json_object_set_string_member(params, "container_path", container_path);
        return params;
    }

    JsonNode *path_node = body ? json_object_get_member(body, "container_path") : NULL;
    if (path_node && JSON_NODE_HOLDS_VALUE(path_node) &&
        json_node_get_value_type(path_node) == G_TYPE_STRING)
        json_object_set_string_member(params, "container_path",
                                      json_node_get_string(path_node));
    return params;
}
