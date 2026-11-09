   
                       
                                                                                          
  
                           
                                                                                                  
                                                                                                                     
                                                                                                                  
                                                             
  
                                                       
   

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

                                                                                                                                                                                                   
                                                          
PcvRestStep
pcv_rest_prelude(PcvRestRequest *rq)
{
    SoupServerMessage *msg = rq->msg;
    const gchar *path = rq->path;
    const gchar *method = rq->method;
    const gchar *request_client_ip = rq->request_client_ip;
                                                       
                                                                     
                                                             
    soup_message_headers_replace(
        soup_server_message_get_response_headers(msg), "Connection", "close");

                                                      
                                                           
                                                                    
                                                                
                                                        
                                                      
                                                        
                                                   
    g_signal_connect(msg, "finished", G_CALLBACK(pcv_rest_on_msg_finished), NULL);

      
                                                             
      
                                                               
                                                           
                                   
      
                                                  
                                             
                                                         
      
                                                        
                                                        
                                                             
       
    {
        static GMutex  rate_mu;
        static GHashTable *rate_counts = nullptr;                                            
        static gint64  window_start = 0;
        g_mutex_lock(&rate_mu);
        if (!rate_counts)
            rate_counts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        gint64 now = g_get_monotonic_time() / G_USEC_PER_SEC;
        if (now - window_start > 60) {
            g_hash_table_remove_all(rate_counts);
            window_start = now;
        }
        const gchar *rate_ip = request_client_ip;
        gchar *rate_key =
            pcv_rest_rate_limit_key_for_message(msg, path, method);
        gint count = GPOINTER_TO_INT(g_hash_table_lookup(rate_counts, rate_key)) + 1;
        g_hash_table_insert(rate_counts, g_strdup(rate_key), GINT_TO_POINTER(count));
                                                              
        if (g_hash_table_size(rate_counts) > 1024) {
            GHashTableIter evict_iter;
            gpointer evict_key, evict_val;
            gint removed = 0;
            gint target = (gint)g_hash_table_size(rate_counts) / 2;
            g_hash_table_iter_init(&evict_iter, rate_counts);
            while (g_hash_table_iter_next(&evict_iter, &evict_key, &evict_val) && removed < target) {
                g_hash_table_iter_remove(&evict_iter);
                removed++;
            }
            PCV_LOG_WARN(REST_LOG_DOM,
                "Rate limit table overflow: evicted %d/%d bucket entries (LRU approximate)",
                removed, removed + (gint)g_hash_table_size(rate_counts));
                                              
            count = GPOINTER_TO_INT(g_hash_table_lookup(rate_counts, rate_key)) + 1;
            g_hash_table_insert(rate_counts, g_strdup(rate_key), GINT_TO_POINTER(count));
        }
        g_mutex_unlock(&rate_mu);
                                            
        gint endpoint_limit = pcv_get_endpoint_rate_limit(path, method);
        if (count > endpoint_limit) {
            PCV_LOG_WARN(REST_LOG_DOM,
                "Rate limit exceeded for %s on %s (%d req/min, limit=%d, key=%s)",
                rate_ip, path, count, endpoint_limit, rate_key);
            g_free(rate_key);
            pcv_rest_send_error(msg, 429, "RATE_LIMIT", "Too many requests");
            return PCV_REST_STEP_RETURN;
        }
        g_free(rate_key);
    }

                                                                
      
                                                     
                                             
                                               
      
                                                        
                                                          
                                                                      
    {
        static GMutex      _user_rl_mu;
        static GHashTable *_user_rl_map   = nullptr;                                         
        static gint64      _user_rl_start = 0;
#define USER_RATE_LIMIT  1200

                                                        
                                                                  
                                                          
        SoupMessageHeaders *rl_reqh = soup_server_message_get_request_headers(msg);
        const gchar *rl_auth = soup_message_headers_get_one(rl_reqh, "Authorization");
        gchar *rl_user = nullptr;
        if (rl_auth && g_str_has_prefix(rl_auth, "Bearer ")) {
            GError *rl_err = nullptr;
            rl_user = pcv_jwt_verify(rl_auth, &rl_err);
            if (rl_err) g_error_free(rl_err);
        }

        if (rl_user && *rl_user) {
            g_mutex_lock(&_user_rl_mu);
            if (!_user_rl_map)
                _user_rl_map = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                     g_free, NULL);
            gint64 now_u = g_get_monotonic_time() / G_USEC_PER_SEC;
            if (now_u - _user_rl_start > 60) {
                g_hash_table_remove_all(_user_rl_map);
                _user_rl_start = now_u;
            }
            gint ucount = GPOINTER_TO_INT(
                g_hash_table_lookup(_user_rl_map, rl_user)) + 1;
            g_hash_table_insert(_user_rl_map, g_strdup(rl_user),
                                GINT_TO_POINTER(ucount));
                                             
            if (g_hash_table_size(_user_rl_map) > 512) {
                g_hash_table_remove_all(_user_rl_map);
                ucount = 1;
            }
            g_mutex_unlock(&_user_rl_mu);
            if (ucount > USER_RATE_LIMIT) {
                PCV_LOG_WARN(REST_LOG_DOM,
                    "Per-user rate limit exceeded for '%s' (%d req/min)",
                    rl_user, ucount);
                g_free(rl_user);
                pcv_rest_send_error(msg, 429, "RATE_LIMIT",
                    "Per-user rate limit exceeded");
                return PCV_REST_STEP_RETURN;
            }
        }
        g_free(rl_user);
#undef USER_RATE_LIMIT
    }

                                                
                                                            
                                                  
                                                                         
    if (g_strcmp0(method, "POST") == 0 || g_strcmp0(method, "PUT") == 0 ||
        g_strcmp0(method, "DELETE") == 0) {
        const gchar *remote_str = request_client_ip;
                                                            
        SoupMessageBody *audit_body = soup_server_message_get_request_body(msg);
        const gchar *body_str = audit_body ? audit_body->data : NULL;
        gsize body_len = audit_body ? (gsize)audit_body->length : 0;
        gboolean is_auth = (g_strstr_len(path, -1, "/auth/token") != nullptr) ||
                            (g_strstr_len(path, -1, "/auth/refresh") != nullptr) ||
                            (g_strstr_len(path, -1, "/auth/logout") != nullptr) ||
                            (g_strstr_len(path, -1, "/auth/register") != nullptr) ||
                            (g_strstr_len(path, -1, "/auth/password") != nullptr) ||
                            (g_strstr_len(path, -1, "/auth/totp/") != nullptr) ||
                                                                                     
                            pcv_rest_body_has_secret(body_str, body_len);
        if (body_str && body_len > 0 && !is_auth) {
            gchar *truncated = g_strndup(body_str, MIN(body_len, 200));
            PCV_LOG_INFO(REST_LOG_DOM, "AUDIT: %s %s from %s body=%.200s",
                         method, path, remote_str, truncated);
            g_free(truncated);
        } else {
            PCV_LOG_INFO(REST_LOG_DOM, "AUDIT: %s %s from %s%s",
                         method, path, remote_str,
                         is_auth ? " body=(credentials masked)" : "");
        }
    }

                                                        
                                        
                                       
                                                      
                                   
                                                                      
    rq->req_id = pcv_generate_request_id();
    {
        SoupMessageHeaders *rh = soup_server_message_get_response_headers(msg);
        soup_message_headers_replace(rh, "X-Request-ID", rq->req_id);
    }
    pcv_log_req_id_set(rq->req_id);

                                                                
                                                   
                                                  
                                                                      
    {
        SoupMessageHeaders *reqh_tp = soup_server_message_get_request_headers(msg);
        const gchar *incoming_tp = soup_message_headers_get_one(reqh_tp, "traceparent");
        rq->trace_ctx = incoming_tp ? pcv_trace_context_parse(incoming_tp)
                                    : NULL;
        if (!rq->trace_ctx)
            rq->trace_ctx = pcv_trace_context_new();
        gchar *tp_out = pcv_trace_context_format(rq->trace_ctx);
        SoupMessageHeaders *rh_tp = soup_server_message_get_response_headers(msg);
        soup_message_headers_replace(rh_tp, "traceparent", tp_out);
        g_free(tp_out);
    }

    PCV_LOG_INFO(REST_LOG_DOM, "[%s] [trace=%s span=%s] %s %s",
                 rq->req_id, rq->trace_ctx->trace_id, rq->trace_ctx->span_id, method, path);

      
                                                                   
      
                                                                 
                                             
      
                                                    
                                         
                                                                
      
                            
                                                    
                                                       
                      
       
                                                             
                                                        
                                           
                                                             
                                                
                                                        
                                                           
                                          
                                                                      
    {
        SoupMessageHeaders *rh = soup_server_message_get_response_headers(msg);
        SoupMessageHeaders *reqh = soup_server_message_get_request_headers(msg);
        const gchar *origin = soup_message_headers_get_one(reqh, "Origin");
        const gchar *host_hdr = soup_message_headers_get_one(reqh, "Host");

                                                                               
                                                                      
                                                           
        if (origin && origin[0] != '\0' && pcv_rest_cors_origin_allowed(origin, host_hdr)) {
            soup_message_headers_replace(rh, "Access-Control-Allow-Origin", origin);
            soup_message_headers_replace(rh, "Access-Control-Allow-Methods",
                                          "GET, POST, PUT, DELETE, OPTIONS");
            soup_message_headers_replace(rh, "Access-Control-Allow-Headers",
                                          "Authorization, Content-Type, X-API-Key");
            soup_message_headers_replace(rh, "Access-Control-Allow-Credentials", "true");
                                                              
            soup_message_headers_replace(rh, "Vary", "Origin");
        }
    }
    if (g_strcmp0(method, "OPTIONS") == 0) {
        soup_server_message_set_status(msg, 204, NULL);                                  
        return PCV_REST_STEP_CLEANUP;
    }
    return PCV_REST_STEP_NEXT;
}
