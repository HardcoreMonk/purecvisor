   
                            
                                      
  
                           
                                                                                                                                           
                                                                                                         
                                                                                                           
                                               
  
                                              
   

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
#include "api/rest_internal.h"
#include "api/rest_response.h"
#include "api/rest_request.h"
#include "api/rest_async.h"
#include "api/rest_transport.h"
#include "api/rest_request_auth.h"

                                                                  
              
                                                                     

                                                                            
                                                                                    
                                                           
                                                                                              
                                                                                            
                                                                                  
                                                                            
static gchar *
_authenticate_subject_identity(SoupServerMessage *msg, const gchar *token, gchar *subject,
                               gint *out_subject_role)
{
    PcvRole subject_role = PCV_ROLE_VIEWER;
    PcvSubjectCheck who = pcv_rbac_subject_check_token(subject, token, &subject_role);
    if (who == PCV_SUBJECT_OK) {
        if (out_subject_role) *out_subject_role = (gint)subject_role;
        return subject;
    }
    const gchar *rip = pcv_rest_auth_audit_address(msg);
    gboolean unavailable = (who == PCV_SUBJECT_UNAVAILABLE);
    PCV_LOG_WARN(REST_LOG_DOM, "AUDIT: auth failed (%s) from %s",
                 unavailable ? "subject lookup unavailable" : "invalid token subject", rip);
    pcv_audit_log("-", "auth.failed", unavailable ? "subject lookup unavailable" : "invalid token",
                  "fail", unavailable ? 503 : 401, 0, rip);
    if (unavailable)
        pcv_rest_send_error(msg, 503, "SERVICE_UNAVAILABLE", "Authentication store unavailable");
    else
        pcv_rest_send_error(msg, 401, "UNAUTHORIZED", "Invalid token");
    g_free(subject);
    return NULL;
}
   
                                                      
  
                                                    
                                                   
                                                      
  
                   
                                        
                                                    
                                      
                                                    
  
         
                        
                                                            
                                         
                         
                     
  
                                                                     
                                                                
                                                                                            
                                                                                       
                                                                  
                                                                                         
  
                         
                                                                       
                                                               
                                                               
   
gchar *
pcv_rest_authenticate(SoupServerMessage *msg, gint *out_key_role, gint *out_subject_role)
{
    if (out_key_role) *out_key_role = -1;                            
    if (out_subject_role) *out_subject_role = (gint)PCV_ROLE_VIEWER;

    SoupMessageHeaders *req =
        soup_server_message_get_request_headers(msg);

                                                           
      
                                      
                                                            
                                              
                                        
                                                                
                                                                                                                   
      
                                                                                         
                                
                                                                      
    const gchar *api_key = soup_message_headers_get_one(req, "X-API-Key");
                                                                                                
                                                                                 
    if (!api_key || !*api_key)
        api_key = pcv_rest_bearer_api_key(soup_message_headers_get_one(req, "Authorization"));
    if (api_key && *api_key) {
        GError *aerr = nullptr;
                                                                              
                                                                            
                                                                                           
                                                         
                                                          
        PcvRole stored_role = PCV_ROLE_VIEWER;
        gchar *user = pcv_rbac_verify_api_key(api_key, &stored_role, &aerr);
        if (user) {
            if (out_key_role) *out_key_role = (gint)stored_role;
            if (out_subject_role) *out_subject_role = (gint)stored_role;                          
            return user;
        }
                                    
        const gchar *rip = pcv_rest_auth_audit_address(msg);
        PCV_LOG_WARN(REST_LOG_DOM, "AUDIT: auth failed (invalid API key) from %s", rip);
        pcv_audit_log("-", "auth.failed",
                      aerr ? aerr->message : "invalid API key", "fail", 401, 0, rip);
        pcv_rest_send_error(msg, 401, "UNAUTHORIZED",
               aerr ? aerr->message : "Invalid API key");
        if (aerr) g_error_free(aerr);
        return NULL;
    }

                                                            
    const gchar *auth = soup_message_headers_get_one(req, "Authorization");

    if (!auth || !*auth) {
        const gchar *rip = pcv_rest_auth_audit_address(msg);
        PCV_LOG_WARN(REST_LOG_DOM, "AUDIT: auth failed (no header) from %s", rip);
        pcv_audit_log("-", "auth.failed", "no Authorization header", "fail", 401, 0, rip);
        pcv_rest_send_error(msg, 401, "UNAUTHORIZED", "Authorization header required");
        return NULL;
    }

    GError *err     = nullptr;
    gchar  *subject = pcv_jwt_verify(auth, &err);
    if (!subject) {
        const gchar *rip = pcv_rest_auth_audit_address(msg);
        PCV_LOG_WARN(REST_LOG_DOM, "AUDIT: auth failed (invalid token) from %s", rip);
        pcv_audit_log("-", "auth.failed",
                      err ? err->message : "invalid token", "fail", 401, 0, rip);
        pcv_rest_send_error(msg, 401, "UNAUTHORIZED",
               err ? err->message : "Invalid token");
        if (err) g_error_free(err);
    }
    return subject ? _authenticate_subject_identity(msg, auth, subject, out_subject_role) : NULL;
}

                                                            
                                                
                                                   
                                 
                                                            
                                                       
                                                       
                                                                       
                                                                
                                                 
                                                     
                                         
                                                               
                                                       
gboolean
pcv_rest_cors_origin_allowed(const gchar *origin, const gchar *host_hdr)
{
    if (!origin || origin[0] == '\0')
        return FALSE;                                               
    const gchar *sep = strstr(origin, "://");
    if (!sep)
        return FALSE;
    const gchar *oh = sep + 3;                                    
    gsize oh_len = 0;
    while (oh[oh_len] && oh[oh_len] != '/' && oh[oh_len] != ':')
        oh_len++;
    if (oh_len == 0)
        return FALSE;

    gchar *origin_host = g_strndup(oh, oh_len);
    gboolean allow = (g_strcmp0(origin_host, "localhost") == 0 ||
                      g_strcmp0(origin_host, "127.0.0.1") == 0 ||
                      g_strcmp0(origin_host, "::1") == 0);
    if (!allow && host_hdr && host_hdr[0]) {
        gsize hh_len = 0;
        while (host_hdr[hh_len] && host_hdr[hh_len] != ':')
            hh_len++;
        gchar *host_host = g_strndup(host_hdr, hh_len);
        allow = (hh_len > 0 && g_strcmp0(origin_host, host_host) == 0);
        g_free(host_host);
    }
    g_free(origin_host);
    return allow;
}
