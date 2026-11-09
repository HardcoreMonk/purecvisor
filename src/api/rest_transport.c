   
                         
                                      
  
                           
                                                                                                                                           
                                                                                                         
                                                                                                           
                                               
  
                                            
   

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



                                                                         
                                                           
                                             
                                                               
                                                                              

   
                                                                 
  
                                                  
                                                                        
                                                 
  
                                                 
                                                 
                                                  
  
                                                             
                                                                      
                                        
                                 
                                                                  
                                                          
   
gboolean
pcv_rest_transport_load_certificate(gpointer data, const gchar *endpoint,
                            GError **error)
{
    (void)endpoint;
    (void)error;
    RestTransportContext *ctx = data;
    if (!pcv_tls_is_enabled()) {
        g_free(pcv_rest_tls_degraded_reason);
        pcv_rest_tls_degraded_reason =
            g_strdup("TLS setup failed (see pcv_tls boot log)");
        return FALSE;
    }
    GError *tls_error = NULL;
    ctx->certificate = g_tls_certificate_new_from_files(
        pcv_tls_get_cert_path(), pcv_tls_get_key_path(), &tls_error);
    if (!ctx->certificate) {
        g_free(pcv_rest_tls_degraded_reason);
        pcv_rest_tls_degraded_reason = g_strdup_printf(
            "cert load failed: %s",
            tls_error ? tls_error->message : "unknown");
        g_clear_error(&tls_error);
        return FALSE;
    }
    PCV_LOG_INFO(REST_LOG_DOM, "TLS certificate loaded: %s",
                 pcv_tls_get_cert_path());
    return TRUE;
}

   
                                                                           
  
                                            
                                               
                                                            
                                                            
                                                        
                                                               
                                              
  
                                                
                                                 
                                                   
  
                                                               
                                                              
                                                         
                                        
                                 
                                                 
                                                         
                 
   
gboolean
pcv_rest_transport_create_tls_context(gpointer data, const gchar *endpoint,
                              GError **error)
{
    (void)endpoint;
    (void)error;
    RestTransportContext *ctx = data;
    soup_server_set_tls_certificate(ctx->server->soup, ctx->certificate);

    const gchar *client_auth =
        pcv_config_get_string("tls", "client_auth", "none");
    gboolean want_request = g_strcmp0(client_auth, "request") == 0;
    gboolean want_require = g_strcmp0(client_auth, "require") == 0;
    if (!want_request && !want_require)
        return TRUE;

    const gchar *ca_path = pcv_tls_get_ca_path();
    if (!ca_path || !*ca_path) {
        PCV_LOG_ERROR(REST_LOG_DOM,
            "SECURITY: [tls] client_auth=%s 이지만 ca_path 미설정 — %s",
            client_auth,
            want_require
              ? "mTLS 강제 불가로 HTTPS 미개시(fail-secure)"
              : "클라이언트 인증서 검증 없이 진행(request)");
        return !want_require;
    }
    GError *db_error = NULL;
    GTlsDatabase *db = g_tls_file_database_new(ca_path, &db_error);
    if (!db) {
        PCV_LOG_ERROR(REST_LOG_DOM,
            "SECURITY: CA 검증 DB 생성 실패 (%s): %s — %s",
            ca_path, db_error ? db_error->message : "unknown",
            want_require
              ? "mTLS 강제 불가로 HTTPS 미개시(fail-secure)"
              : "클라이언트 인증서 검증 없이 진행(request)");
        g_clear_error(&db_error);
        return !want_require;
    }
    soup_server_set_tls_database(ctx->server->soup, db);
    soup_server_set_tls_auth_mode(
        ctx->server->soup,
        want_require ? G_TLS_AUTHENTICATION_REQUIRED
                     : G_TLS_AUTHENTICATION_REQUESTED);
    g_object_unref(db);
    PCV_LOG_INFO(REST_LOG_DOM,
                 "mTLS 클라이언트 인증서 검증 활성 "
                 "(client_auth=%s, ca=%s)", client_auth, ca_path);
    return TRUE;
}

   
                                                         
  
                                                       
                                             
                                                        
                              
  
                                                   
                                                      
                                                
                          
  
                                                          
                                                   
                                                     
                                     
                                        
                                                            
                                      
                         
   
gboolean
pcv_rest_transport_listen_plaintext(gpointer data, const gchar *endpoint,
                            GError **error)
{
    RestTransportContext *ctx = data;
    if (g_strcmp0(endpoint, "0.0.0.0") == 0) {
        return soup_server_listen_all(ctx->server->soup, ctx->server->port,
                                      SOUP_SERVER_LISTEN_IPV4_ONLY, error);
    }
    GInetAddress *lo = g_inet_address_new_loopback(AF_INET);
    GSocketAddress *addr =
        g_inet_socket_address_new(lo, ctx->server->port);
    gboolean ok = soup_server_listen(ctx->server->soup, addr, 0, error);
    g_object_unref(addr);
    g_object_unref(lo);
    return ok;
}

   
                                                                    
  
                                                      
                                                                  
                                               
                                             
  
                                                  
                                                  
                                                  
                              
  
                                                                   
                                                                                                              
                                        
                                                        
                                                                 
                                                             
   
gboolean
pcv_rest_transport_listen_https(gpointer data, const gchar *endpoint,
                        GError **error)
{
    (void)endpoint;
    (void)error;
    RestTransportContext *ctx = data;
    GError *listen_error = NULL;
    gboolean ok = soup_server_listen_all(
        ctx->server->soup, ctx->server->https_port,
        SOUP_SERVER_LISTEN_IPV4_ONLY | SOUP_SERVER_LISTEN_HTTPS,
        &listen_error);
    if (!ok) {
        g_free(pcv_rest_tls_degraded_reason);
        pcv_rest_tls_degraded_reason = g_strdup_printf(
            "HTTPS listen failed on port %u: %s",
            ctx->server->https_port,
            listen_error ? listen_error->message : "unknown");
        g_clear_error(&listen_error);
        return FALSE;
    }
    ctx->server->tls_active = TRUE;
    const gchar *hsts_cfg =
        pcv_config_get_string("tls", "hsts", "false");
    if (g_strcmp0(hsts_cfg, "true") == 0 ||
        g_strcmp0(hsts_cfg, "1") == 0)
        pcv_rest_hsts_enabled = TRUE;
    PCV_LOG_INFO(REST_LOG_DOM,
                 "HTTPS listening on https://0.0.0.0:%u (HSTS enabled)",
                 ctx->server->https_port);
    PCV_LOG_INFO(REST_LOG_DOM,
                 "HTTP/2 support enabled (TLS via ALPN negotiation)");
    return TRUE;
}
