   
                      
                                                                                                           
  
                           
                                                                                                  
                                                                                                                     
                                                                                                                  
                                                                                                                      
                                                                                                  
                                                             
  
                                                                
                                            
   

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
#include "modules/virt/pcv_virt_open.h"
#include "purecvisor/version.h"
#include "api/rest_internal.h"
#include "api/rest_response.h"
#include "api/rest_request.h"
#include "api/rest_async.h"
#include "api/rest_transport.h"
#include "api/rest_request_auth.h"
#include "api/rest_public_render.h"

                                                                                                                                                     
                                                                   
PcvRestStep
pcv_rest_ui_static(PcvRestRequest *rq)
{
    SoupServerMessage *msg = rq->msg;
    const gchar *path = rq->path;
      
                                                            
      
                                                         
                                              
      
                                             
                                                
                                                            
       
                                                            
    if (g_strcmp0(path, "/") == 0) {
        soup_server_message_set_redirect(msg, 302, "/ui/");
        return PCV_REST_STEP_CLEANUP;
    }

                                                            
                                                     
                                                         
    if (g_str_has_prefix(path, "/ui")) {
        const gchar *file_path = path + 3;                            
        if (!file_path[0] || g_strcmp0(file_path, "/") == 0)
            file_path = "/index.html";

                                                            
        if (strstr(file_path, "..")) {
            pcv_rest_send_error(msg, 403, "FORBIDDEN", "Path traversal blocked");
            return PCV_REST_STEP_CLEANUP;
        }

        static const char *ui_base_dir = "/usr/local/share/purecvisor/ui";
                                          
        static char ui_base_resolved[PATH_MAX] = {0};
        static size_t ui_base_len = 0;
        if (ui_base_resolved[0] == '\0') {
            if (!realpath(ui_base_dir, ui_base_resolved)) {
                pcv_rest_send_error(msg, 500, "SERVER_ERROR", "UI base dir missing");
                return PCV_REST_STEP_CLEANUP;
            }
            ui_base_len = strlen(ui_base_resolved);
        }

        gchar *full = g_strdup_printf("%s%s", ui_base_dir, file_path);

                                              
        char resolved[PATH_MAX];
        if (!realpath(full, resolved)) {
            int e = errno;
            pcv_rest_send_error(msg, e == ENOENT ? 404 : 403,
                   e == ENOENT ? "NOT_FOUND" : "FORBIDDEN",
                   e == ENOENT ? "UI file not found" : "Path traversal blocked");
            g_free(full);
            return PCV_REST_STEP_CLEANUP;
        }
                                                       
        if (strncmp(resolved, ui_base_resolved, ui_base_len) != 0 ||
            (resolved[ui_base_len] != '/' && resolved[ui_base_len] != '\0')) {
            pcv_rest_send_error(msg, 403, "FORBIDDEN", "Path traversal blocked");
            g_free(full);
            return PCV_REST_STEP_CLEANUP;
        }

        gchar *content = nullptr;
        gsize len = 0;

        if (g_file_get_contents(resolved, &content, &len, NULL)) {
            const gchar *ct = "text/html; charset=utf-8";
            if (g_str_has_suffix(file_path, ".js") || g_str_has_suffix(file_path, ".mjs"))
                ct = "application/javascript";
            if (g_str_has_suffix(file_path, ".css"))  ct = "text/css";
            if (g_str_has_suffix(file_path, ".json")) ct = "application/json";
            if (g_str_has_suffix(file_path, ".md"))   ct = "text/markdown; charset=utf-8";
            if (g_str_has_suffix(file_path, ".png"))  ct = "image/png";
            if (g_str_has_suffix(file_path, ".ico"))  ct = "image/x-icon";
            if (g_str_has_suffix(file_path, ".svg"))  ct = "image/svg+xml";
            if (g_str_has_suffix(file_path, ".webp")) ct = "image/webp";
            if (g_str_has_suffix(file_path, ".woff")) ct = "font/woff";
            if (g_str_has_suffix(file_path, ".woff2")) ct = "font/woff2";

            soup_server_message_set_status(msg, 200, NULL);
            SoupMessageHeaders *rh = soup_server_message_get_response_headers(msg);
            soup_message_headers_replace(rh, "Cache-Control", "no-cache, must-revalidate");
                                                                              
                                                                     
                                                                              
                                                                 
                                                                                   
            soup_message_headers_replace(rh, "Content-Security-Policy", PCV_CSP_POLICY);
            soup_message_headers_replace(rh, "X-Frame-Options", "SAMEORIGIN");
            soup_message_headers_replace(rh, "Referrer-Policy",
                                          "strict-origin-when-cross-origin");
            soup_message_headers_replace(rh, "X-Content-Type-Options", "nosniff");
            soup_server_message_set_response(msg, ct, SOUP_MEMORY_TAKE, content, len);
        } else {
            pcv_rest_send_error(msg, 404, "NOT_FOUND", "UI file not found");
        }
        g_free(full);
        return PCV_REST_STEP_CLEANUP;
    }
    return PCV_REST_STEP_NEXT;
}

                                                                                   
typedef struct {
    GMainContext       *rest_ctx;
    PcvRestHealthInput  in;
    gchar              *tls_reason;                                    
    PcvRestRendered     out;
} _HealthProbe;

                                                                                                  
static gboolean
_health_probe_done(gpointer data)
{
    _HealthProbe *p = data;
    GPtrArray *waiters = pcv_rest_health_complete(p->out.body, p->out.status, g_get_monotonic_time());
    for (guint i = 0; i < waiters->len; i++) {
        SoupServerMessage *m = g_ptr_array_index(waiters, i);
        pcv_rest_send_rendered(m, &p->out);
        soup_server_message_unpause(m);
        g_object_unref(m);
    }
    g_ptr_array_unref(waiters);
    pcv_rest_rendered_clear(&p->out);
    g_free(p->tls_reason);
    g_free(p);
    return G_SOURCE_REMOVE;
}

                                                                                    
static void
_health_probe_worker(GTask *task, gpointer src, gpointer task_data, GCancellable *cancel)
{
    (void)task; (void)src; (void)cancel;
    _HealthProbe *p = task_data;
    pcv_rest_render_health(&p->in, &p->out);
    pcv_drain_idle(p->rest_ctx, _health_probe_done, p, NULL);
}

                                                                                                      
                                   
static void
_daemon_rpc_render(gchar *rpc, const gchar *fail_detail, PcvRestRendered *out)
{
    gchar *resp = pcv_rest_rpc_over_uds(rpc);
    g_free(rpc);
    pcv_rest_render_rpc_result(resp, fail_detail, out);
    g_free(resp);
}

static void
_version_render(gpointer data, PcvRestRendered *out)
{
    (void)data;
    _daemon_rpc_render(pcv_rest_build_rpc("daemon.version", NULL), "Version RPC failed", out);
}

static void
_update_check_render(gpointer data, PcvRestRendered *out)
{
    (void)data;
    _daemon_rpc_render(pcv_rest_build_rpc("daemon.update_check", NULL), "Update-check RPC failed", out);
}

                                                                                       
static void
_internal_vms_render(gpointer data, PcvRestRendered *out)
{
    (void)data;
    pcv_rest_render_internal_vms(out);
}

static void
_internal_telemetry_render(gpointer data, PcvRestRendered *out)
{
    (void)data;
    pcv_rest_render_internal_telemetry(out);
}

#if PCV_CLUSTER_ENABLED
                                                                     
static void
_cluster_vms_render(gpointer data, PcvRestRendered *out)
{
    (void)data;
    gchar *rpc = g_strdup("{\"jsonrpc\":\"2.0\",\"method\":\"cluster.vm.list\",\"params\":{},\"id\":\"rest\"}");
    gchar *uds_resp = pcv_rest_rpc_over_uds(rpc);
    g_free(rpc);
    out->status = 200;
    out->content_type = NULL;
    out->body = uds_resp ? uds_resp : g_strdup("{\"result\":[]}");
}
#endif

                                                                                            
static void
_metrics_render(gpointer data, PcvRestRendered *out)
{
    (void)data;
                                             
    gchar *rpc = pcv_rest_build_rpc("monitor.fleet", NULL);
    gchar *resp = pcv_rest_rpc_over_uds(rpc);
    g_free(rpc);
    pcv_rest_render_metrics(resp, out);
    g_free(resp);
}

                                                                                                                                                                                    
                                                                                                                           
PcvRestStep
pcv_rest_health(PcvRestRequest *rq)
{
    SoupServerMessage *msg = rq->msg;
    const gchar *path = rq->path;
    GHashTable *query = rq->query;
    PcvRestServer *self = rq->self;
                                                                     
                                              
      
                                                          
                                                                 
                                                       
      
                    
                                                     
                                               
      
                     
                                          
                                    
                              
                                     
                           
                                      
                                                      
                                              
      
                  
                      
                                               
                                                          
      
                   
                                        
                                                        
                                                                        
                                                                     
                                                                   
      
                                                                 
                                                 
                                      
      
               
                                             
                                    
      
                                                                                            
                                                                        
    if (g_strcmp0(path, REST_API_PREFIX "/health/recent-errors") == 0) {
        if (!pcv_rest_collector_gate(msg)) return PCV_REST_STEP_CLEANUP;                            
        const gchar *vm_q = NULL;
        gint lim = 5;
        if (query) {
            vm_q = g_hash_table_lookup(query, "vm");
            const gchar *lim_q = g_hash_table_lookup(query, "limit");
            if (lim_q) {
                gint v = atoi(lim_q);
                if (v > 0 && v <= 50) lim = v;
            }
        }
        JsonArray *failures = pcv_audit_recent_failures(vm_q, lim);
        JsonObject *root = json_object_new();
        JsonNode *fnode = json_node_new(JSON_NODE_ARRAY);
        json_node_take_array(fnode, failures);
        json_object_set_member(root, "data", fnode);
        JsonNode *rn = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(rn, root);
        JsonGenerator *gen = json_generator_new();
        json_generator_set_root(gen, rn);
        gchar *body = json_generator_to_data(gen, NULL);
        pcv_rest_send_json(msg, 200, body);
        g_free(body);
        g_object_unref(gen);
        json_node_free(rn);
        return PCV_REST_STEP_CLEANUP;
    }

    if (g_strcmp0(path, REST_API_PREFIX "/health") == 0) {
                                                                              
        gchar *cached = NULL;
        guint cached_code = 0;
        g_object_ref(msg);                                           
        PcvRestHealthEntry entry = pcv_rest_health_enter(g_get_monotonic_time(), msg, &cached, &cached_code);
        if (entry == PCV_REST_HEALTH_CACHED) {
            g_object_unref(msg);
            pcv_rest_send_json(msg, cached_code, cached);
            g_free(cached);
            return PCV_REST_STEP_CLEANUP;
        }
        soup_server_message_pause(msg);
        if (entry == PCV_REST_HEALTH_PROBE) {
            _HealthProbe *p = g_new0(_HealthProbe, 1);
            p->rest_ctx = self->rest_ctx;
            p->in.mode = self->transport.mode;
            p->in.tls_degraded = pcv_rest_tls_degraded;
            p->tls_reason = g_strdup(pcv_rest_tls_degraded_reason);
            p->in.tls_degraded_reason = p->tls_reason;
            GTask *task = pcv_drain_task_new(NULL, NULL, NULL, NULL);
            g_task_set_task_data(task, p, NULL);                                     
            g_task_run_in_thread(task, _health_probe_worker);
            g_object_unref(task);
        }
        return PCV_REST_STEP_CLEANUP;
    }
    return PCV_REST_STEP_NEXT;
}

                                                                                                                                                                             
                                                                   
PcvRestStep
pcv_rest_version(PcvRestRequest *rq)
{
    SoupServerMessage *msg = rq->msg;
    const gchar *path = rq->path;
    const gchar *method = rq->method;
                                                                
    if (g_strcmp0(path, REST_API_PREFIX "/version") == 0
        && g_strcmp0(method, "GET") == 0)
    {
        pcv_rest_defer_rendered(msg, rq->self->rest_ctx, _version_render, NULL, NULL);
        return PCV_REST_STEP_CLEANUP;
    }

                                                                        
    if (g_strcmp0(path, REST_API_PREFIX "/update-check") == 0
        && g_strcmp0(method, "GET") == 0)
    {
        pcv_rest_defer_rendered(msg, rq->self->rest_ctx, _update_check_render, NULL, NULL);
        return PCV_REST_STEP_CLEANUP;
    }
    return PCV_REST_STEP_NEXT;
}

                                                                                                                                                                             
                                                                       
PcvRestStep
pcv_rest_internal(PcvRestRequest *rq)
{
    SoupServerMessage *msg = rq->msg;
    const gchar *path = rq->path;
    const gchar *method = rq->method;
      
                                                                                            
      
                              
                                                  
                             
      
                                               
                                                     
      
                                                       
       
    if (g_strcmp0(path, REST_API_PREFIX "/internal/vms") == 0
        && g_strcmp0(method, "GET") == 0)
    {
        if (!pcv_rest_collector_gate(msg)) return PCV_REST_STEP_CLEANUP;                            
        pcv_rest_defer_rendered(msg, rq->self->rest_ctx, _internal_vms_render, NULL, NULL);
        return PCV_REST_STEP_CLEANUP;
    }

#if PCV_CLUSTER_ENABLED
                                                                                                       
    if (g_strcmp0(path, REST_API_PREFIX "/internal/cluster/vms") == 0
        && g_strcmp0(method, "GET") == 0)
    {
        if (!pcv_rest_collector_gate(msg)) return PCV_REST_STEP_CLEANUP;                            
        pcv_rest_defer_rendered(msg, rq->self->rest_ctx, _cluster_vms_render, NULL, NULL);
        return PCV_REST_STEP_CLEANUP;
    }
#endif

      
                                                                                                    
      
                                                   
                                                                        
      
                                                                                                      
      
                                                             
       
    if (g_strcmp0(path, REST_API_PREFIX "/internal/telemetry") == 0
        && g_strcmp0(method, "GET") == 0)
    {
        if (!pcv_rest_collector_gate(msg)) return PCV_REST_STEP_CLEANUP;                            
        pcv_rest_defer_rendered(msg, rq->self->rest_ctx, _internal_telemetry_render, NULL, NULL);
        return PCV_REST_STEP_CLEANUP;
    }
    return PCV_REST_STEP_NEXT;
}

                                                                                                                                                                
                                                                               
PcvRestStep
pcv_rest_metrics(PcvRestRequest *rq)
{
    SoupServerMessage *msg = rq->msg;
    const gchar *path = rq->path;
    const gchar *method = rq->method;
      
                                                                            
      
                                             
                                                                       
      
                 
                                                     
                                                
                                                    
      
              
                                                   
                                                      
                                                   
                                                              
                                                                           
      
                                                                
       
    if (g_strcmp0(path, REST_API_PREFIX "/metrics") == 0
        && g_strcmp0(method, "GET") == 0)
    {
        if (!pcv_rest_collector_gate(msg)) return PCV_REST_STEP_CLEANUP;                            

                                                                   
        pcv_rest_defer_rendered(msg, rq->self->rest_ctx, _metrics_render, NULL, NULL);
        return PCV_REST_STEP_CLEANUP;
    }
    return PCV_REST_STEP_NEXT;
}

                                                                                    
gboolean
pcv_rest_collector_gate(SoupServerMessage *msg)
{
    PcvCollectorAuthMode mode = pcv_rest_collector_auth_mode_from_config(NULL);
    gboolean direct = pcv_rest_client_identity_is_direct_local(pcv_rest_client_identity_get(msg));
    if (!pcv_rest_collector_needs_auth(mode, direct))
        return TRUE;
    gchar *subject = pcv_rest_authenticate(msg, NULL, NULL);                                           
    if (!subject)
        return FALSE;                                                       
    g_free(subject);
    return TRUE;
}

                          
void
pcv_rest_collector_auth_log_config(void)
{
    gboolean recognized = TRUE;
    PcvCollectorAuthMode mode = pcv_rest_collector_auth_mode_from_config(&recognized);
    g_autofree gchar *raw = pcv_config_dup_raw_value("metrics", "auth");
    if (!recognized && raw && *g_strstrip(raw) == '\0')
        PCV_LOG_WARN(REST_LOG_DOM, "[metrics] auth 값이 비어 있다 — required로 취급(수집기 경로에 토큰 필요)");
    else if (!recognized)
        PCV_LOG_WARN(REST_LOG_DOM, "[metrics] auth='%s' 알 수 없는 값 — required로 취급(수집기 경로에 토큰 필요)", raw ? raw : "");
    else if (mode == PCV_COLLECTOR_AUTH_NONE)
        PCV_LOG_WARN(REST_LOG_DOM, "[metrics] auth=none — /metrics·/internal/*·/health/recent-errors가 누구에게나 무인증 공개");
}
