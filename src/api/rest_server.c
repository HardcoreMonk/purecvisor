   
                      
                                        
  
                                                     
                                                                         
                                  
  
                                                
                                                    
                                                     
                                                 
                                               
                               
  
                                   
                                                             
                                                     
                                 
       
                                                                     
                                                      
                                                                 
                                                  
                                                     
                                                  
                                                     
                                                                     
                                                       
                                                             
                                                                              
                                               
                                                                                               
                                                                  
                                                                         
                                                                          
                                                                          
  
           
                                                                       
                                            
                                                      
  
                                          
                                                    
                                            
                                   
  
                                                                
  
                      
                                                                           
                                                                      
                                                              
                                                                 
  
         
                    
                                                    
                                                                              
                                                                                                  
                                                                                
                                                                           
                        
                                                                       
                                                                 
                  
                                                     
                                                        
                                          
                                                 
  
         
                                                                 
                                                                
                                                                         
                                                            
                                    
                                                                                          
  
            
                                                                                   
                                                                                                  
                                                                              
                                                                                            
  
        
                                                          
                                                        
                                                       
                                                      
  
                                      
                                                                           
   

#include "api/drain.h"
#include "rest_shutdown.h"
#include "api/rest_login.h"
#include "rest_server.h"
#include "rest_middleware.h"
#include "rest_auth.h"
#include "rest_client_identity.h"
#include "utils/pcv_crypto.h"
#include "../utils/pcv_jwt.h"
#include "../utils/pcv_totp.h"                                                            
#include "../utils/pcv_log.h"
#include "../modules/daemons/prometheus_exporter.h"
#include "ws_server.h"
#include "../utils/pcv_config.h"
#include "../utils/pcv_tls.h"
#include "../utils/pcv_bpf.h"
#include "../utils/pcv_secure.h"                               
#include "../modules/dispatcher/rpc_utils.h"                                             
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
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <zlib.h>
#include "../modules/network/ovn_manager.h"
#include "../modules/network/dpdk_manager.h"
#include "../utils/pcv_spawn.h"
#include "../modules/auth/pcv_rbac.h"
#include "../modules/audit/pcv_audit.h"
#include "../modules/dispatcher/rpc_utils.h"
#include "../modules/virt/circuit_breaker.h"
#include "purecvisor/version.h"
#include <unistd.h>
#include "api/rest_internal.h"
#include "api/rest_response.h"
#include "api/rest_request.h"
#include "api/rest_async.h"
#include "api/rest_transport.h"
#include "api/rest_request_auth.h"

                                            


                                                            
                                                               
                                                  
gboolean pcv_rest_hsts_enabled = FALSE;

                                                        
                                              
gboolean pcv_rest_tls_degraded = FALSE;
                                                       
static GMainContext *self_rest_ctx = nullptr;
gchar   *pcv_rest_tls_degraded_reason = NULL;

                                                         
                                                                       

                                                                                      

                                                        


                                        
  
                           
                                      
                                           
                                           
                   
  
                               
                                    
                                
                                 
                                       
   
G_DEFINE_TYPE(PcvRestServer, pcv_rest_server, G_TYPE_OBJECT)


                                                                  
                 
                                                                     
   
                         
  
                                                  
                                                            
                                                      
                                                         
  
                                         
                                   
  
                
                                                            
  
                                 
                                           
                                        
  
                                                     
                                              
                                           
   
                                                                         



   
                                                                               
  
                                              
                                                 
                                       
  
                                                   
                                                        
                                                  
                                              
  
                                                                 
                                                                                    
                                                       
                                                                
                                                             
                                                             
                                        
                                                   
                                                                       
                              
                                               
                                                                      
                                                     
                                                                                  
                                                            
                                                       
                                                             
                                        
                                                              
  
                                                                       
                                                                              
                                                                                   
                                                          
  
                                                            
                                                                                           
                                                                     
                                                                                                    
                                                                                                          
                                                                                        
                                                                                  
                                                                               
                                                                                        
                                                                                         
                                                                                             
                                                                
                                                            
  
       
                                               
                                                                       
                               
                                                
                                                            
                                                                  
                                                                           
                                                                               
                                                                 
                                                                         
                                                                     
                                                                     
                                                                                 
                                                        
                                                
                                                              
                                                  
                                                         
                                                    
                                                 
                                                                            
  
                                                                        
                                                      
                             
                                                      
                                                               
   
static void
_on_request(SoupServer        *server   __attribute__((unused)),
            SoupServerMessage *msg,
            const gchar       *path,
            GHashTable        *query,
            gpointer           user_data)
{
    PcvRestServer *self = PCV_REST_SERVER(user_data);
    const gchar *method = soup_server_message_get_method(msg);
    PcvRestRequest rq_storage = {0};                                                  
    PcvRestRequest *rq = &rq_storage;
    rq->msg = msg;
    rq->path = path;
    rq->query = query;
    rq->self = self;
    rq->method = method;
    rq->req_id = nullptr;                                                    
    rq->trace_ctx = nullptr;                                              
    JsonObject *body = nullptr;                                                                 
    gchar **segs = nullptr, *subject = nullptr;
    rq->rpc = nullptr;
    const gchar *request_client_ip =
        pcv_rest_auth_audit_address(msg);
    rq->request_client_ip = request_client_ip;

    PCV_REST_RUN(pcv_rest_prelude(rq));
    PCV_REST_RUN(pcv_rest_ui_static(rq));
    PCV_REST_RUN(pcv_rest_health(rq));
    PCV_REST_RUN(pcv_rest_version(rq));
    PCV_REST_RUN(pcv_rest_internal(rq));
    PCV_REST_RUN(pcv_rest_metrics(rq));
    PCV_REST_RUN(pcv_rest_auth_token(rq));
    PCV_REST_RUN(pcv_rest_auth_refresh(rq));
    PCV_REST_RUN(pcv_rest_auth_logout(rq));
    PCV_REST_RUN(pcv_rest_auth_password(rq));
    PCV_REST_RUN(pcv_rest_auth_register(rq));
    PCV_REST_RUN(pcv_rest_auth_totp_verify(rq));
    PCV_REST_RUN(pcv_rest_auth_totp_enroll(rq));
      
                                                                       
                             
      
                                                                 
                                                     
                                                                       
       
                                             
      
                                                  
                                                                            
                                                                
                                                                       
      
                                                 
                                                                        
      
                                                                      
                                                                                     
                                                 
       
                                                                      
                                                                
                                                   
                                                         
    gint key_role = -1;
    gint subject_role = 0;                                                                 
    subject = pcv_rest_authenticate(msg, &key_role, &subject_role);
    rq->subject = subject;
    rq->key_role = key_role;
    rq->subject_role = subject_role;
    if (!subject) goto cleanup;                                                         

                                                            

                                                      
                                             
                                                               
      
                                       
                                                                     
                                                    
                                                             
                                                             
      
          
                                                                                
                                                                                        
                                                                           
                                                                               
                                                                      
    body = pcv_rest_parse_body(msg);
    rq->body = body;
    segs = pcv_rest_split_path(path);
    rq->segs = segs;
      
                                                                  
                                                               
                                                      
                                                            
      
                                              
                                                                           
       
    const guint nseg = g_strv_length(segs);
    const gchar *resource = nseg > 0 ? segs[0] : "";
    rq->resource = resource;
    const gchar *name     = nseg > 1 ? segs[1] : "";
    rq->name = name;
    const gchar *action   = nseg > 2 ? segs[2] : "";
    rq->action = action;
    const gchar *sub      = nseg > 3 ? segs[3] : "";
    rq->sub = sub;

                                               
      
                  
                            
                                                     
      
                                                                 
                                                                 
                                                                     
      
                    
                                                   
                                                            
                                       
      
                                                        
                                                          
                                        
       
    gboolean needs_libvirt =
        (g_strcmp0(resource, "vms") == 0 ||
         g_strcmp0(resource, "containers") == 0 ||
#if PCV_CLUSTER_ENABLED
         g_str_has_prefix(path, REST_API_PREFIX "/cluster/vms") ||
#endif
         FALSE);
                                                               
                                                               
                                            
    if (needs_libvirt && cb_should_reject_request()) {
        SoupMessageHeaders *rh = soup_server_message_get_response_headers(msg);
        soup_message_headers_replace(rh, "Retry-After", "30");
        pcv_rest_send_error(msg, 503, "SERVICE_UNAVAILABLE",
               "libvirt connection is down (circuit breaker open)");
        goto cleanup;
    }

    PcvRestStep st = pcv_rest_map_compute(rq);
    if (st == PCV_REST_STEP_UNMATCHED) st = pcv_rest_map_observe(rq);
    if (st == PCV_REST_STEP_UNMATCHED) st = pcv_rest_map_platform(rq);
    if (st == PCV_REST_STEP_CLEANUP) goto cleanup;
                                                            
    if (!rq->rpc) {
        pcv_rest_send_error(msg, 404, "NOT_FOUND",
               "Endpoint not found or method not allowed");
        goto cleanup;
    }

    {
                                                                                 
                                                             
                                                                                
                                                                     
                                                                                 
                                                                     
        PcvRole caller_role = (rq->key_role >= 0)
            ? (PcvRole)rq->key_role
            : (PcvRole)rq->subject_role;                                                     
        gchar *rpc_with_context = pcv_rest_rpc_attach_auth_context(rq->rpc, rq->subject, caller_role);
        g_free(rq->rpc);
        rq->rpc = rpc_with_context;
    }

                                                          
      
                                                        
                                                                 
                                                           
                                                               
                              
      
               
                                               
                                
                                               
                                                   
                                                                  
      
             
                                                                      
                                                              
                                                            
                                                               
      
                                            
                                                            
                                                      
                                                             
                                          
      
                                     
                                             
                                                         
                                            
                                                       
                                                    
       
    gchar *rpc_method = NULL;                                             
    {
                                                                                 
                                                               
        PcvRestRpcMethodStatus method_status =
            pcv_rest_rpc_envelope_method(rq->rpc, &rpc_method);
        if (method_status == PCV_REST_RPC_METHOD_MISSING) {
            static const char resp_missing_method[] =
                "{\"error\":{\"code\":\"BAD_REQUEST\",\"message\":\"Missing method field\"}}";
            g_message("[RBAC] reject: missing method field in RPC envelope");
            g_free(rq->rpc); rq->rpc = NULL;
            soup_server_message_set_status(msg, 400, "Bad Request");
            soup_server_message_set_response(msg, "application/json",
                SOUP_MEMORY_STATIC,
                resp_missing_method,
                sizeof(resp_missing_method) - 1);
            goto cleanup;
        }
        if (method_status != PCV_REST_RPC_METHOD_OK) {
            static const char resp_malformed_method[] =
                "{\"error\":{\"code\":\"BAD_REQUEST\",\"message\":\"Malformed method field\"}}";
            g_message("[RBAC] reject: malformed method field in RPC envelope");
            g_free(rq->rpc); rq->rpc = NULL;
            soup_server_message_set_status(msg, 400, "Bad Request");
            soup_server_message_set_response(msg, "application/json",
                SOUP_MEMORY_STATIC,
                resp_malformed_method,
                sizeof(resp_malformed_method) - 1);
            goto cleanup;
        }
                                                                              
                                                        
        PCV_LOG_INFO(REST_LOG_DOM, "→ RPC %.100s", rpc_method);
                                                                                                       
        gboolean permitted = (rq->key_role >= 0)
            ? pcv_rbac_role_permits((PcvRole)rq->key_role, rpc_method)
            : pcv_rbac_check_permission(rq->subject, rpc_method);
        if (!permitted) {
            static const char resp_forbidden[] =
                "{\"error\":{\"code\":\"FORBIDDEN\",\"message\":\"Insufficient permissions\"}}";
            g_message("[RBAC] denied: user=%s method=%s", rq->subject, rpc_method);
                                                                 
                                                            
            if (g_strcmp0(rpc_method, "vm.inspect") == 0)
                pcv_audit_log(rq->subject, "vm.inspect", "", "fail", PURE_RPC_ERR_FORBIDDEN, 0, NULL);
                                                                                      
            else if (pcv_rest_method_is_user_admin(rpc_method))
                pcv_audit_log(rq->subject, rpc_method, "", "denied", PURE_RPC_ERR_FORBIDDEN, 0, "rbac");
            g_free(rpc_method);
            g_free(rq->rpc);
            rq->rpc = NULL;
            soup_server_message_set_status(msg, 403, "Forbidden");
            soup_server_message_set_response(msg, "application/json",
                SOUP_MEMORY_STATIC,
                resp_forbidden,
                sizeof(resp_forbidden) - 1);
            goto cleanup;
        }
    }

                                                                     
                                
      
                                                           
                                                       
                                                        
      
                                                               
                                                   
                                                        
      
                                                       
                                                            
                                         
                                                    
                                                                        
    {
                           
                                                       
                                                            
                                                 
                                                         
        _RestAsyncCtx *actx = g_new0(_RestAsyncCtx, 1);
        actx->msg = g_object_ref(msg);                         
        actx->rpc = rq->rpc;                                                
                                                              
                                                        
        actx->rpc_method = g_steal_pointer(&rpc_method);
        actx->rest_ctx = self_rest_ctx;                                        

                                                               
                                                                     
                                                            
                                                            
        soup_server_message_pause(msg);

                                       
                                                                             
                                                                
                                                                        
        GTask *task = pcv_drain_task_new(NULL, NULL, NULL, NULL);
        g_task_set_task_data(task, actx, pcv_rest_async_ctx_free);
        g_task_run_in_thread(task, pcv_rest_async_worker);
        g_object_unref(task);
    }

                                            
    goto cleanup_no_free;

                                                            
      
                                                         
                                                   
                                                                
      
                                  
                                                             
                                                  
                                     
      
                                   
                                                 
                                                       
                                                     
                                                                      
cleanup:
    g_strfreev(rq->segs);                                   
    if (rq->body) json_object_unref(rq->body);                     
    g_free(rq->subject);                                          
    g_free(rq->rpc);                                            
    g_free(rq->req_id);                               
    pcv_trace_context_free(rq->trace_ctx);                            
    pcv_log_req_id_set(NULL);                                                
    return;

cleanup_no_free:
    g_strfreev(rq->segs);
    if (rq->body) json_object_unref(rq->body);
    g_free(rq->subject);
                                      
    g_free(rq->req_id);
    pcv_trace_context_free(rq->trace_ctx);
    pcv_log_req_id_set(NULL);
}

                                                             
                                                 
                                                         
static void
_on_request_drained(SoupServer *server, SoupServerMessage *msg, const gchar *path,
            GHashTable *query, gpointer user_data)
{
    if (!pcv_drain_admit_rest()) {
        soup_server_message_set_status(msg, SOUP_STATUS_SERVICE_UNAVAILABLE, NULL);
        return;
    }
    _on_request(server, msg, path, query, user_data);
    pcv_drain_work_release();
}

  
                                                                  
                               
  
                                             
                                                   
                                                                  
   

   
                                   
  
                                                    
                                              
                                                           
                                           
  
         
                                          
                                            
                                 
                                  
                           
   
static void
pcv_rest_server_finalize(GObject *object)
{
    PcvRestServer *self = PCV_REST_SERVER(object);
    pcv_rest_server_stop(self);
    if (self->rest_loop) {
        if (self->thread) {
            g_thread_join(self->thread);
            self->thread = nullptr;
        }
        g_main_loop_unref(self->rest_loop);
        self->rest_loop = nullptr;
    }
    if (self->rest_ctx) {
        g_main_context_unref(self->rest_ctx);
        self->rest_ctx = nullptr;
    }
    if (self->soup) {
        soup_server_disconnect(self->soup);
        g_object_unref(self->soup);
    }
    G_OBJECT_CLASS(pcv_rest_server_parent_class)->finalize(object);
}

                                                                  
                                                         
static void pcv_rest_server_class_init(PcvRestServerClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = pcv_rest_server_finalize;
}

                                                      
                                                     
                                                                          
static void pcv_rest_server_init(PcvRestServer *self)
{
    self->soup       = nullptr;
    self->port       = 8080;
    self->https_port = (guint16)pcv_config_get_int("tls", "https_port", 443);
    self->tls_active = FALSE;
    self->thread     = nullptr;
    self->rest_loop  = nullptr;
    self->rest_ctx   = nullptr;
}

   
                                                
  
                                                      
                                                       
                            
                                                          
                                                               
  
              
                                                                
                                                   
                            
                                                       
  
                                        
                                             
                                              
   
static gpointer
_rest_thread_func(gpointer data)
{
    PcvRestServer *self = (PcvRestServer *)data;
                                              
    g_main_context_push_thread_default(self->rest_ctx);
    g_main_loop_run(self->rest_loop);
    g_main_context_pop_thread_default(self->rest_ctx);
    return NULL;
}

                                                                  
                                             
                                                                     

   
                  
  
                                                    
                                      
                                                                    
                                                         
  
                                                    
                                             
                                                 
  
                                            
                                                                  
                                                           
   
PcvRestServer *
pcv_rest_server_new(PureCVisorDispatcher *dispatcher __attribute__((unused)),
                    guint16 port,
                    PcvRestTransportPlan transport)
{
    PcvRestServer *self = g_object_new(PCV_TYPE_REST_SERVER, NULL);
    self->transport = transport;
    self->port = (port > 0) ? port : (guint16)pcv_config_get_rest_port();
    return self;
}

   
                                           
  
                                                    
                                                                
                                                               
                                           
                                                           
                                                                    
  
         
                                      
                                                  
                                                       
                                                                
                                                                                    
                                   
                                     
                                                            
  
                              
                                      
  
                            
                                   
                            
   
gboolean
pcv_rest_server_start(PcvRestServer *self, GError **error)
{
                                                              
                                                            
                                                 
                                                          
                                     
       
                                                                     
                                                   
    self->rest_ctx  = g_main_context_new();
    self->rest_loop = g_main_loop_new(self->rest_ctx, FALSE);
    self_rest_ctx   = self->rest_ctx;                              

                                                       
                                                          
    g_main_context_push_thread_default(self->rest_ctx);

                                       
      
                     
                                                 
                                             
                                                                           
                                                 
    self->soup = soup_server_new("server-header", "PureCVisord/" PCV_PRODUCT_VERSION, NULL);

      
                                                   
                              
                                      
                                            
       
    soup_server_add_handler(self->soup, REST_API_PREFIX,
                             _on_request_drained, self, NULL);
    soup_server_add_handler(self->soup, "/ui",
                             _on_request_drained, self, NULL);
    soup_server_add_handler(self->soup, "/",
                             _on_request_drained, self, NULL);

                                                            
    pcv_ws_server_init(self->soup);
                                                               
    pcv_rest_proxy_trust_load_config();
    pcv_rest_collector_auth_log_config();

                                                                  
                                                              
                                                         
                                                  
                                                                     
                                                                     
                                                           
                                                           
                                                  
    RestTransportContext transport_context = {
        .server = self,
        .certificate = NULL,
    };
    const PcvRestTransportOps transport_ops = {
        .load_certificate = pcv_rest_transport_load_certificate,
        .create_tls_context = pcv_rest_transport_create_tls_context,
        .listen_plaintext = pcv_rest_transport_listen_plaintext,
        .listen_https = pcv_rest_transport_listen_https,
    };
    PcvRestTransportOutcome transport_outcome = {0};
    gboolean transport_ok = pcv_rest_transport_start(
        &self->transport, &transport_ops, &transport_context,
        &transport_outcome, error);
    g_clear_object(&transport_context.certificate);
    if (!transport_ok) {
        pcv_ws_server_shutdown();
        g_clear_object(&self->soup);
        g_main_context_pop_thread_default(self->rest_ctx);
        g_main_loop_unref(self->rest_loop);
        g_main_context_unref(self->rest_ctx);
        self->rest_loop = NULL;
        self->rest_ctx = NULL;
        return FALSE;
    }
    self->tls_active = transport_outcome.https_listening;

                                                   
    GSList *listeners = soup_server_get_listeners(self->soup);
    for (GSList *listener = listeners; listener; listener = listener->next) {
        int fd = g_socket_get_fd(G_SOCKET(listener->data));
        if (fd >= 0) {
            listen(fd, 1024);
            int optval = 1;
            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                       &optval, sizeof(optval));
        }
    }
    g_slist_free(listeners);

                                                                    
    if (self->transport.listen_https &&
        !self->tls_active) {
        pcv_rest_tls_degraded = TRUE;
        PCV_LOG_ERROR(REST_LOG_DOM,
                      "TLS DEGRADED: %s — 외부 HTTPS 표면 차단, 평문 HTTP(:%u)+UDS만 가용",
                      pcv_rest_tls_degraded_reason ? pcv_rest_tls_degraded_reason : "unknown",
                      self->port);
    }

    g_main_context_pop_thread_default(self->rest_ctx);

                        
    self->thread = g_thread_new("purecvisor-rest", _rest_thread_func, self);

                                                                          
    const gchar *plain_host = self->transport.plaintext_host;
    if (self->tls_active) {
        PCV_LOG_INFO(REST_LOG_DOM,
                     "REST API listening on http://%s:%u + https://0.0.0.0:%u%s",
                     plain_host, self->port, self->https_port, REST_API_PREFIX);
    } else if (self->transport.mode == PCV_REST_TLS_EXTERNAL_TERMINATION) {
        PCV_LOG_INFO(REST_LOG_DOM,
                     "REST API listening on http://%s:%u%s; "
                     "HTTPS disabled by configuration; "
                     "external TLS termination required (thread: %p)",
                     plain_host, self->port, REST_API_PREFIX,
                     (void *)self->thread);
    } else {
        PCV_LOG_INFO(REST_LOG_DOM,
                     "REST API DEGRADED: http://%s:%u%s only — 외부 HTTPS 표면 차단 (thread: %p)",
                     plain_host, self->port, REST_API_PREFIX, (void *)self->thread);
    }
    return TRUE;
}

   
                                           
  
                                                            
                                                  
                         
                                                             
                   
  
      
                                                                   
                                                             
                                                       
  
                                                       
   
void
pcv_rest_server_stop(PcvRestServer *self)
{
    if (self->thread) {
                                                      
                                                                  
        pcv_rest_shutdown_schedule(self->soup, self->rest_loop);
        g_thread_join(self->thread);
        pcv_rest_login_pool_shutdown();                                            
        self->thread = nullptr;
    } else if (self->soup) {
        soup_server_disconnect(self->soup);
    }
    g_clear_pointer(&pcv_rest_tls_degraded_reason, g_free);
    pcv_rest_tls_degraded = FALSE;
    PCV_LOG_INFO(REST_LOG_DOM, "REST API server stopped");
}

