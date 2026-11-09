   
                     
                                   
  
                           
                                                                                                                                           
                                                                                                         
                                                                                                           
                                               
  
                                                   
   

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


                                                                  
            
                                                                     
   
                                                  
  
                                                      
                                                                 
                                                                       
                                                                     
                                         
                                                          
                                                               
                                                         
  
                                                                       
                                                   
                                                            
                                                              
                                                                             
                                                     
                                                        
  
                                                
                                                          
  
                 
                                               
                                
                                        
                                     
                          
                                                                                    
                 
                                                            
  
            
                                         
                                           
                                                     
                                                        
  
                                         
                                  
                                            
                                     
                                            
   
                                       
  
                                                           
                                                             
  
               
                                                            
                                                        
                                                          
  
                                                      
                                                    
                                                
                                                             
                         
   
                                              
  
                                                 
                                         
                                                                 
                                       
  
                             
                                
                                            
                        
                                            
                        
  
                                         
                                                     
                                                 
  
       
                                                      
                                                   
                                  
  
                                       
                       
   
void
pcv_rest_on_msg_finished(SoupServerMessage *msg, gpointer user_data __attribute__((unused)))
{
    GSocket *gsock = soup_server_message_get_socket(msg);
    if (gsock) {
        int fd = g_socket_get_fd(gsock);
        if (fd >= 0) shutdown(fd, SHUT_RDWR);                            
    }
}

                                                                  
                                     
  
       
                                                                            
                                                                    
  
       
                                    
                                                   
                                                                     



   
                                                               
  
                                                 
                                                          
                                                        
                                 
  
                                       
                                             
   
void
pcv_rest_async_ctx_free(gpointer p)
{
    _RestAsyncCtx *ctx = p;
    if (!ctx) return;
    g_free(ctx->rpc);
    g_free(ctx->rpc_method);
    if (ctx->msg) g_object_unref(ctx->msg);
    g_free(ctx);
}


   
                                      
  
                                                     
                                                       
                                                    
                                                                        
                            
  
          
                                               
                                                   
                        
  
                                            
   
static gboolean
_rest_unpause_cb(gpointer data)
{
    _RestUnpauseData *ud = data;
    pcv_rest_send_rpc_result(ud->msg, ud->resp);                        
    soup_server_message_unpause(ud->msg);                                 
    g_object_unref(ud->msg);                                          
    g_free(ud->resp);
    g_free(ud);
    return G_SOURCE_REMOVE;                                       
}

   
                                        
  
                                                 
                                                     
                                       
                                                                     
                                                                       
                                                                   
  
                           
  
                                
                                                       
                               
                                                
  
                                                    
                                                                             
                                                           
  
                                                       
                                                      
                                                      
  
                
                                                                    
                                                               
                                             
  
                                                   
                               
                                     
                                      
   
void
pcv_rest_async_worker(GTask *task, gpointer src __attribute__((unused)),
                   gpointer task_data, GCancellable *cancel __attribute__((unused)))
{
    _RestAsyncCtx *actx = task_data;

                                                       
                                                     
    gint timeout = pcv_get_rpc_timeout(actx->rpc_method);
    gchar *resp = pcv_rest_rpc_over_uds_timeout(actx->rpc, timeout);

                                                                             
                                                                                       

                                                   
      
                                     
                                 
                                              
                                               
                                                             
                                                               
                                                     
      
                   
                                                      
                                                            
       
    _RestUnpauseData *ud = g_new0(_RestUnpauseData, 1);
    ud->msg = actx->msg;                                       
    ud->resp = resp;                                

    pcv_drain_idle(actx->rest_ctx, _rest_unpause_cb, ud, NULL);

                                             
                                                                         
                       
    g_free(actx->rpc);           actx->rpc = NULL;
    g_free(actx->rpc_method);    actx->rpc_method = NULL;
    actx->msg = NULL;                                               
}

                                                                                                                   
                                
void
pcv_rest_send_rendered(SoupServerMessage *msg, const PcvRestRendered *r)
{
    if (!r->content_type) {
        pcv_rest_send_json(msg, r->status, r->body);
        return;
    }
    soup_server_message_set_status(msg, r->status, NULL);
    SoupMessageHeaders *hdrs = soup_server_message_get_response_headers(msg);
    soup_message_headers_replace(hdrs, "Content-Type", r->content_type);
    soup_server_message_set_response(msg, r->content_type, SOUP_MEMORY_COPY, r->body, strlen(r->body));
}

                                                                              
typedef struct {
    SoupServerMessage *msg;
    GMainContext      *rest_ctx;
    PcvRestRenderFn    fn;
    gpointer           data;
    GDestroyNotify     destroy;
    PcvRestRendered    out;
} _RestDeferred;

                                                                                       
static gboolean
_rest_deferred_done(gpointer data)
{
    _RestDeferred *d = data;
    pcv_rest_send_rendered(d->msg, &d->out);
    soup_server_message_unpause(d->msg);
    g_object_unref(d->msg);
    pcv_rest_rendered_clear(&d->out);
    g_free(d);
    return G_SOURCE_REMOVE;
}

                                                                                          
                                        
static void
_rest_deferred_worker(GTask *task, gpointer src, gpointer task_data, GCancellable *cancel)
{
    (void)task; (void)src; (void)cancel;
    _RestDeferred *d = task_data;
    d->fn(d->data, &d->out);
    if (d->destroy)
        d->destroy(d->data);
    pcv_drain_idle(d->rest_ctx, _rest_deferred_done, d, NULL);
}

                                                                                             
                                                   
void
pcv_rest_defer_rendered(SoupServerMessage *msg, GMainContext *ctx, PcvRestRenderFn fn, gpointer data, GDestroyNotify destroy)
{
    _RestDeferred *d = g_new0(_RestDeferred, 1);
    d->msg = g_object_ref(msg);
    d->rest_ctx = ctx;
    d->fn = fn;
    d->data = data;
    d->destroy = destroy;
    soup_server_message_pause(msg);
    GTask *task = pcv_drain_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(task, d, NULL);                                      
    g_task_run_in_thread(task, _rest_deferred_worker);
    g_object_unref(task);
}
