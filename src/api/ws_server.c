   
                    
                                              
  
                           
                                                   
                                                    
                                        
  
                       
                                                    
                                                 
  
                                                                      
           
                                                                   
                                           
                                                          
                                            
                                                   
  
          
  
                                
                                                         
                                                          
                                         
  
                                       
                                                                                      
                                                                  
                                                                         
  
         
                                                           
                                                      
                                                                 
                                                                          
                                                            
  
        
                                                        
                                                     
                                                                    
                                                                           
                                                            
  
                                                      
                                                      
                                                     
                                             
  
                         
                                                                          
                                                     
         
                                                              
                                                                
                                                                              
                                                                              
                                                      
                                                                     
                                                     
                                                        
                                         
                                                                       
                                                                       
                                                                              
                                                                        
                                                                                  
                                                        
                                                                        
                                                                              
                                                                          
                                                                      
   
#include "api/drain.h"
#include "api/ws_send.h"
#include "ws_server.h"
#include "rest_client_identity.h"
#include "utils/pcv_log.h"
#include "utils/pcv_jwt.h"
#include "utils/pcv_config.h"
#include "modules/auth/pcv_rbac.h"                                                                     
#include "../modules/dispatcher/rpc_utils.h"                                           
#include <json-glib/json-glib.h>
#include <string.h>
#include <time.h>
#include <gio/gio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <sys/statvfs.h>

#define WS_LOG_DOM "ws_server"

                         
                                                                      
                                                    
                                                              
                                                      
static void _ws_warn_deprecated_url_token(SoupServerMessage *msg);
#define WS_PATH    "/api/v1/ws/events"                    
#define VNC_PATH           "/api/v1/ws/vnc"                   
                                                                           
                                                   
                                                
#define WS_DEF_MAX_CONNECTIONS  1000                       
#define WS_DEF_MAX_PER_IP       100                                          
#define WS_DEF_IDLE_TIMEOUT     300                        
#define WS_DEF_IDLE_CHECK       60                      
#define WS_DEF_PING_INTERVAL    30                             
#define WS_DEF_PONG_TIMEOUT     60                                 
#define WS_DEF_MAX_PENDING      500                                    
                                                                  
                                                              
#define WS_FANOUT_MAX_QUEUED    1024
#define WS_FANOUT_DROP_WARN_SEC 60                         

                                                                                   
                                                                      
static gint ws_max_connections   = WS_DEF_MAX_CONNECTIONS;
static gint ws_max_per_ip        = WS_DEF_MAX_PER_IP;
static gint ws_idle_timeout_sec  = WS_DEF_IDLE_TIMEOUT;
static gint ws_idle_check_interval = WS_DEF_IDLE_CHECK;
static gint ws_ping_interval_sec = WS_DEF_PING_INTERVAL;
static gint ws_pong_timeout_sec  = WS_DEF_PONG_TIMEOUT;
static gint ws_max_pending_msgs  = WS_DEF_MAX_PENDING;

  
                           
  
                                                      
                      
  
                                          
                                                                
                                          
                                                                           
                                            
                                                           
                                                                  
                                                                            
                                                             
   
static struct {
    GPtrArray  *clients;                                                            
    GHashTable *ip_counts;                                                         
    GMutex      mu;                                 
    gint        initialized;
    GSource    *timers[4];                                       
    GMainContext *ctx;                                                           
} G = {0};

                                                          
                                                    
                           
                                                             
                                                             
                                                                      
static gboolean
_ws_conn_is_authed(SoupWebsocketConnection *conn)
{
    return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(conn), "pcv-ws-authed")) == 1;
}

   
                           
  
                                                     
                     
  
                                                
                                     
  
                               
                            
                                                                 
   
static void
_on_ws_closed(SoupWebsocketConnection *conn, gpointer data __attribute__((unused)))
{
    g_mutex_lock(&G.mu);
    if (!G.clients) { g_mutex_unlock(&G.mu); return; }
                                                      
                                                  
                                                        
    gchar *disc_ip = g_strdup(g_object_get_data(G_OBJECT(conn), "client-ip"));
    g_ptr_array_remove(G.clients, conn);                                                  
                       
    if (disc_ip && G.ip_counts) {
        gint cnt = GPOINTER_TO_INT(g_hash_table_lookup(G.ip_counts, disc_ip));
        if (cnt <= 1)
            g_hash_table_remove(G.ip_counts, disc_ip);                          
        else
                                                               
            g_hash_table_insert(G.ip_counts, g_strdup(disc_ip),
                                GINT_TO_POINTER(cnt - 1));
    }
    guint remaining = G.clients->len;
    g_mutex_unlock(&G.mu);
    PCV_LOG_INFO(WS_LOG_DOM, "WebSocket client disconnected (total=%d)", remaining);
    g_free(disc_ip);                            
}

   
                             
  
                                                     
                                                            
  
                                      
                                                
                                                                
  
                                      
                                                 
                                 
                              
                                                                        
                                                    
   
static void
_on_ws_message(SoupWebsocketConnection *conn,
               SoupWebsocketDataType type __attribute__((unused)),
               GBytes *message,
               gpointer data __attribute__((unused)))
{
                                                            
                                                  
    gint now = (gint)time(NULL);
    g_object_set_data(G_OBJECT(conn), "last_active",
                      GINT_TO_POINTER(now));

                                                                         
                                                                            
                                                       
                                           
    gint *pending = g_object_get_data(G_OBJECT(conn), "pcv-pending");
    if (pending) g_atomic_int_set(pending, 0);

    gsize sz = 0;
    const gchar *text = g_bytes_get_data(message, &sz);                                  

                                                                     
                                                           
    gint authed = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(conn), "pcv-ws-authed"));
    if (!authed && sz > 0 && text) {                                       
        JsonParser *p = NULL;
        GError *perr = NULL;
                                                          
        if (pcv_rpc_parse_guarded(text, (gssize)sz, &p, &perr)) {
            JsonObject *obj = json_node_get_object(json_parser_get_root(p));
            if (obj) {
                                                                      
                const gchar *msg_type = json_object_get_string_member_with_default(obj, "type", "");
                if (g_strcmp0(msg_type, "auth") == 0) {                           
                    const gchar *token = json_object_get_string_member_with_default(obj, "token", "");
                    GError *jwt_err = NULL;
                    gchar *subject = pcv_jwt_verify(token, &jwt_err);                             
                                                                                             
                                                                                
                                                                                         
                    if (subject && pcv_rbac_subject_check_token(subject, token, NULL) != PCV_SUBJECT_OK)
                        g_clear_pointer(&subject, g_free);
                    if (subject) {
                                                                      
                        g_object_set_data_full(G_OBJECT(conn), "pcv-ws-subject", g_strdup(subject), g_free);
                        g_object_set_data(G_OBJECT(conn), "pcv-ws-authed", GINT_TO_POINTER(1));
                        PCV_LOG_INFO(WS_LOG_DOM, "WebSocket protocol-auth OK: user=%s", subject);
                        pcv_ws_send_text_if_open(conn, "{\"type\":\"auth_ok\"}");
                        g_free(subject);                                      
                    } else {
                                                                       
                        PCV_LOG_WARN(WS_LOG_DOM, "WebSocket protocol-auth failed: %s",
                                     jwt_err ? jwt_err->message : "invalid");
                        pcv_ws_send_text_if_open(conn, "{\"type\":\"auth_fail\"}");
                        soup_websocket_connection_close(conn, SOUP_WEBSOCKET_CLOSE_POLICY_VIOLATION,
                                                        "authentication failed");
                        if (jwt_err) g_error_free(jwt_err);                     
                    }
                }
            }
            g_object_unref(p);                        
        } else {
                                                             
                                                           
            if (perr) g_error_free(perr);
        }
                                                        
                                                              
                                            
        if (!authed) {                                
            return;
        }
    }

                                                                       
                                                                
                                                                     
    if (sz > 0 && text && sz < 256 && g_strstr_len(text, (gssize)sz, "pong")) {
        g_object_set_data(G_OBJECT(conn), "last_pong",
                          GINT_TO_POINTER(now));
    }
}

   
                                
  
                                                       
                                                       
                    
  
                                                             
                                                               
  
                                                           
                                                            
                                                       
  
                                                            
                                                              
  
                                
                               
                                          
                                    
                              
   
static void
_on_ws_connected(SoupServer *server __attribute__((unused)),
                 SoupServerMessage *msg,
                 const char *path __attribute__((unused)),
                 SoupWebsocketConnection *conn,
                 gpointer data __attribute__((unused)))
{
                                                         
                                                    
                                                          
                                                         
    _ws_warn_deprecated_url_token(msg);
                                                        
    g_object_set_data(G_OBJECT(conn), "pcv-ws-authed", GINT_TO_POINTER(0));
                                                            
    g_object_set_data(G_OBJECT(conn), "pcv-ws-auth-deadline",
                      GINT_TO_POINTER((gint)time(NULL) + 5));

                                                            
                                                                 
                   
                                                         
    const PcvRestClientIdentity *identity =
        pcv_rest_client_identity_get(msg);
    gchar *client_ip = g_strdup(
        pcv_rest_client_identity_get_client_ip(identity));

    g_mutex_lock(&G.mu);                           
    if (!g_atomic_int_get(&G.initialized) || pcv_drain_is_shutdown()) {
        g_mutex_unlock(&G.mu); g_free(client_ip);
        soup_websocket_connection_close(conn, 1001, "server shutdown"); return;
    }
                                                             
    if ((gint)G.clients->len >= ws_max_connections) {
        g_mutex_unlock(&G.mu);
        PCV_LOG_WARN(WS_LOG_DOM,
            "WebSocket connection rejected — global limit reached (%d)", ws_max_connections);
        g_free(client_ip);
        soup_websocket_connection_close(conn, SOUP_WEBSOCKET_CLOSE_GOING_AWAY,
                                        "too many connections");
        return;
    }
                                                  
                                                                
    if (!pcv_client_identity_admission_try(
            G.ip_counts, client_ip, ws_max_per_ip)) {
        g_mutex_unlock(&G.mu);
        PCV_LOG_WARN(WS_LOG_DOM,
            "WebSocket connection rejected — per-IP limit (%d) for %s",
            ws_max_per_ip, client_ip);
        g_free(client_ip);
        soup_websocket_connection_close(conn, SOUP_WEBSOCKET_CLOSE_GOING_AWAY,
                                        "too many connections from this IP");
        return;
    }
                                        
                                                                                    
    g_object_set_data_full(G_OBJECT(conn), "client-ip",
                           g_strdup(client_ip), g_free);
    g_free(client_ip);                                        
                                                                      
                                                                            
                                                                           
                                                                                   
    gint connect_time = (gint)time(NULL);
    g_object_set_data(G_OBJECT(conn), "last_active",
                      GINT_TO_POINTER(connect_time));
    g_object_set_data(G_OBJECT(conn), "last_pong",
                      GINT_TO_POINTER(connect_time));
    g_ptr_array_add(G.clients, g_object_ref(conn));                                     
    g_mutex_unlock(&G.mu);

                                                                        
                                                                       
    gint *pending = g_new0(gint, 1);
    g_object_set_data_full(G_OBJECT(conn), "pcv-pending", pending, g_free);

                                                        
    g_signal_connect(conn, "closed", G_CALLBACK(_on_ws_closed), NULL);
    g_signal_connect(conn, "message", G_CALLBACK(_on_ws_message), NULL);

    PCV_LOG_INFO(WS_LOG_DOM, "WebSocket client connected (total=%d)",
                 G.clients->len);
}

                                                                             
                                           
  
                                                             
  
                                                       
                                                     
                                                        
  
                                                  
                                                              
                                                                  
                                                                               

#include "vnc_proxy.h"

   
                                                                
  
                                                    
                                                     
                                                       
  
                                                  
                                                       
  
            
                                                        
                                                              
  
                                                                   
                                       
                                                         
                                            
   
                                                            
                                                      
                                                       
                                                      
                                                               
                                                  
                                                   
                                                           
                                                                        
                             
  
                                                        
                                                 
                                                                  
static void
_ws_warn_deprecated_url_token(SoupServerMessage *msg)
{
    GUri *uri = soup_server_message_get_uri(msg);
    const gchar *query = uri ? g_uri_get_query(uri) : NULL;
    if (!query) return;

    GHashTable *params = g_uri_parse_params(query, -1, "&", G_URI_PARAMS_NONE, NULL);
    if (!params) return;

    const gchar *t = g_hash_table_lookup(params, "token");
    if (t && t[0] != '\0') {
                                                           
        PCV_LOG_WARN(WS_LOG_DOM,
            "WebSocket URL ?token= is deprecated and ignored — authenticate via the "
            "post-connect {\"type\":\"auth\"} message instead (a token in the URL leaks "
            "through proxy/access logs and Referer headers)");
    }
    g_hash_table_unref(params);
}

   
                              
  
                                                         
                                                         
  
                                                 
                                         
                                             
                                                                        
                                                                 
                                       
   
static gboolean
_ws_heartbeat_cb(gpointer data __attribute__((unused)))
{
    if (!g_atomic_int_get(&G.initialized)) return G_SOURCE_REMOVE;                           

    time_t now = time(NULL);
    GPtrArray *stale = g_ptr_array_new();                           
    gchar *ping_msg = g_strdup_printf("{\"type\":\"ping\",\"ts\":%ld}", (long)now);

    g_mutex_lock(&G.mu);
    for (guint i = 0; i < G.clients->len; i++) {
        SoupWebsocketConnection *conn = g_ptr_array_index(G.clients, i);
        if (soup_websocket_connection_get_state(conn) != SOUP_WEBSOCKET_STATE_OPEN)
            continue;                        

                                                                  
        gint last_pong = GPOINTER_TO_INT(
            g_object_get_data(G_OBJECT(conn), "last_pong"));
                                                                  
                                                 
        if (last_pong > 0 && (now - (time_t)last_pong) > ws_pong_timeout_sec) {
            g_ptr_array_add(stale, g_object_ref(conn));
            continue;                            
        }

                                                                          
        if (!_ws_conn_is_authed(conn))
            continue;

                                                                           
        pcv_ws_send_text_if_open(conn, ping_msg);
    }
    g_mutex_unlock(&G.mu);

                                               
    for (guint i = 0; i < stale->len; i++) {
        SoupWebsocketConnection *conn = g_ptr_array_index(stale, i);
        PCV_LOG_INFO(WS_LOG_DOM, "Closing WebSocket — no pong within %ds",
                     ws_pong_timeout_sec);
        soup_websocket_connection_close(conn, SOUP_WEBSOCKET_CLOSE_GOING_AWAY,
                                        "pong timeout");
        g_object_unref(conn);                           
    }
    g_ptr_array_free(stale, TRUE);
    g_free(ping_msg);

    return G_SOURCE_CONTINUE;
}

                                                        
                                                         
                                                        
                                                              
                                                              
static gboolean
_ws_auth_deadline_check(gpointer data __attribute__((unused)))
{
    if (!g_atomic_int_get(&G.initialized)) return G_SOURCE_REMOVE;                       
    time_t now = time(NULL);
    GPtrArray *to_close = g_ptr_array_new();                            
    g_mutex_lock(&G.mu);
    for (guint i = 0; i < G.clients->len; i++) {
        SoupWebsocketConnection *conn = g_ptr_array_index(G.clients, i);
        gint authed = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(conn), "pcv-ws-authed"));
        if (authed) continue;                              
                                                     
        gint deadline = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(conn), "pcv-ws-auth-deadline"));
        if (deadline > 0 && (gint)now > deadline) {
            g_ptr_array_add(to_close, g_object_ref(conn));                       
        }
    }
    g_mutex_unlock(&G.mu);
                                                    
    for (guint i = 0; i < to_close->len; i++) {
        SoupWebsocketConnection *conn = g_ptr_array_index(to_close, i);
        if (soup_websocket_connection_get_state(conn) == SOUP_WEBSOCKET_STATE_OPEN) {
            PCV_LOG_WARN(WS_LOG_DOM,
                "Closing unauthenticated WebSocket connection (>5s, ADR-0010)");
            soup_websocket_connection_close(conn,
                SOUP_WEBSOCKET_CLOSE_POLICY_VIOLATION, "auth timeout");
        }
        g_object_unref(conn);                       
    }
    g_ptr_array_free(to_close, TRUE);
    return G_SOURCE_CONTINUE;
}

   
                                                                 
  
                                                 
                                                        
                                                            
                                                     
                        
  
                                                 
                                              
                              
  
                                                        
                                               
                                                                 
                                          
                                                       
                                                 
                                                   
                                                        
                                       
   
static gboolean
_ws_idle_cleanup(gpointer data __attribute__((unused)))
{
    if (!g_atomic_int_get(&G.initialized)) return G_SOURCE_REMOVE;

    time_t now = time(NULL);
    GPtrArray *to_close = g_ptr_array_new();

    g_mutex_lock(&G.mu);
    for (guint i = 0; i < G.clients->len; i++) {
        SoupWebsocketConnection *conn = g_ptr_array_index(G.clients, i);
                                      
                                                            
        gint authed = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(conn), "pcv-ws-authed"));
        if (!authed) {
            gint deadline = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(conn), "pcv-ws-auth-deadline"));
            if (deadline > 0 && (gint)now > deadline) {
                g_ptr_array_add(to_close, g_object_ref(conn));
                continue;                                   
            }
        }
                                                                         
                                                                    
        gint last = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(conn), "last_active"));
        if (last > 0 && (now - (time_t)last) > ws_idle_timeout_sec) {
            g_ptr_array_add(to_close, g_object_ref(conn));
        }
    }
    g_mutex_unlock(&G.mu);

                                                                    
    for (guint i = 0; i < to_close->len; i++) {
        SoupWebsocketConnection *conn = g_ptr_array_index(to_close, i);
        if (soup_websocket_connection_get_state(conn) == SOUP_WEBSOCKET_STATE_OPEN) {
            PCV_LOG_INFO(WS_LOG_DOM, "Closing idle WebSocket connection (>%ds)",
                         ws_idle_timeout_sec);
            soup_websocket_connection_close(conn, SOUP_WEBSOCKET_CLOSE_GOING_AWAY,
                                            "idle timeout");
        }
        g_object_unref(conn);                       
    }
    g_ptr_array_free(to_close, TRUE);

    return G_SOURCE_CONTINUE;
}

                                                                             
                               
  
                                                       
                                            
  
                                                     
                                   
                                                                               

                                                      
                                                      
extern JsonObject *pcv_ebpf_telemetry_get_host(void);

   
                                                        
  
                                               
                                                      
                
  
                                                 
                                             
  
                                                  
                                                     
                                                         
                                                            
                                                                     
                                                            
                   
                        
                                                        
                                              
                                            
   
static gboolean
_ws_push_metrics_cb(gpointer user_data __attribute__((unused)))
{
                                                                      
                                                               
                                                            
    if (!g_atomic_int_get(&G.initialized)) return G_SOURCE_CONTINUE;

                               
    g_mutex_lock(&G.mu);
    gboolean has_clients = (G.clients && G.clients->len > 0);
    g_mutex_unlock(&G.mu);
    if (!has_clients) return G_SOURCE_CONTINUE;                            

                                       
    JsonObject *payload = json_object_new();

    JsonObject *host = pcv_ebpf_telemetry_get_host();                           
    if (host) {
                                                    
        if (json_object_has_member(host, "cpu_percent"))
            json_object_set_double_member(payload, "cpu_percent",
                json_object_get_double_member(host, "cpu_percent"));
        if (json_object_has_member(host, "mem_percent"))
            json_object_set_double_member(payload, "mem_percent",
                json_object_get_double_member(host, "mem_percent"));
        json_object_unref(host);                          
    }

                          
    struct statvfs vfs;
    if (statvfs("/", &vfs) == 0) {
                                                             
        guint64 total  = (guint64)vfs.f_blocks * vfs.f_frsize;
        guint64 free_b = (guint64)vfs.f_bfree  * vfs.f_frsize;
        if (total > 0)                 
                                         
            json_object_set_double_member(payload, "disk_percent",
                100.0 * (1.0 - (gdouble)free_b / (gdouble)total));
    }

                           
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(node, payload);                                      
    gchar *json_str = json_to_string(node, FALSE);

    pcv_ws_broadcast("metrics-update", json_str);                     

    g_free(json_str);
    json_node_unref(node);
    json_object_unref(payload);

    return G_SOURCE_CONTINUE;
}

   
                                                               
  
                                                       
                                                                 
                                                          
                                                            
  
                                                
                                                     
                                             
                            
  
                                                           
                                                         
                                                       
                                                
                                                                
                                                       
                                                
                                                           
                                                           
                                              
                                               
                                                                        
                                                                    
                          
                                                
                                             
                                                          
   
void
pcv_ws_server_init(SoupServer *soup)
{
                                                                            
    ws_max_connections    = pcv_config_get_int("ws", "max_connections",    WS_DEF_MAX_CONNECTIONS);
    ws_max_per_ip         = pcv_config_get_int("ws", "max_per_ip",        WS_DEF_MAX_PER_IP);
    ws_idle_timeout_sec   = pcv_config_get_int("ws", "idle_timeout_sec",  WS_DEF_IDLE_TIMEOUT);
    ws_idle_check_interval = pcv_config_get_int("ws", "idle_check_sec",   WS_DEF_IDLE_CHECK);
    ws_ping_interval_sec  = pcv_config_get_int("ws", "ping_interval_sec", WS_DEF_PING_INTERVAL);
    ws_pong_timeout_sec   = pcv_config_get_int("ws", "pong_timeout_sec",  WS_DEF_PONG_TIMEOUT);
    ws_max_pending_msgs   = pcv_config_get_int("ws", "max_pending_msgs",  WS_DEF_MAX_PENDING);

                                            
    vnc_stopping = FALSE;
                                                                         
                                               
    GMainContext *owner = g_main_context_ref_thread_default();
    g_mutex_lock(&G.mu);
    GMainContext *previous = g_steal_pointer(&G.ctx);
    G.ctx = owner;
    g_mutex_unlock(&G.mu);
    if (previous) g_main_context_unref(previous);
                                                             
    G.clients = g_ptr_array_new_with_free_func(g_object_unref);
                                                                     
    G.ip_counts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
                                                                 
    g_atomic_int_set(&G.initialized, TRUE);

                                                                      
    soup_server_add_websocket_handler(soup, WS_PATH,
                                       NULL, NULL,
                                       _on_ws_connected, NULL, NULL);

                                                  
    char *vnc_protocols[] = { VNC_PROTOCOL, NULL };
    soup_server_add_websocket_handler(soup, VNC_PATH,
                                       NULL, vnc_protocols,
                                       _on_vnc_connected, NULL, NULL);

                                                                  
                                                                         
                                                                      
                                  

                                                                                  
    G.timers[0] = g_timeout_source_new_seconds(ws_idle_check_interval);
    g_source_set_callback(G.timers[0], _ws_idle_cleanup, NULL, NULL);
    g_source_attach(G.timers[0], g_main_context_get_thread_default());

                                                              
    G.timers[1] = g_timeout_source_new_seconds(1);
    g_source_set_callback(G.timers[1], _ws_auth_deadline_check, NULL, NULL);
    g_source_attach(G.timers[1], g_main_context_get_thread_default());

                                                                                    
    G.timers[2] = g_timeout_source_new_seconds(ws_ping_interval_sec);
    g_source_set_callback(G.timers[2], _ws_heartbeat_cb, NULL, NULL);
    g_source_attach(G.timers[2], g_main_context_get_thread_default());

                                          
    G.timers[3] = g_timeout_source_new_seconds(10);
    g_source_set_callback(G.timers[3], _ws_push_metrics_cb, NULL, NULL);
    g_source_attach(G.timers[3], g_main_context_get_thread_default());

    PCV_LOG_INFO(WS_LOG_DOM, "WebSocket handler registered at %s, %s (idle timeout=%ds, ping=%ds, metrics push=10s)",
                 WS_PATH, VNC_PATH, ws_idle_timeout_sec, ws_ping_interval_sec);
}

   
                                        
  
                                                       
                  
  
                      
                                                                
                                        
                                                                
                                                     
                                    
   
void
pcv_ws_server_shutdown(void)
{
    _vnc_shutdown();                                      
    for (guint i = 0; i < G_N_ELEMENTS(G.timers); i++) _vnc_source_clear(&G.timers[i]);
    g_mutex_lock(&G.mu);
    g_atomic_int_set(&G.initialized, FALSE);
    GPtrArray *clients = g_steal_pointer(&G.clients);
    g_clear_pointer(&G.ip_counts, g_hash_table_unref);
    GMainContext *owner = g_steal_pointer(&G.ctx);                            
    g_mutex_unlock(&G.mu);
    if (owner) g_main_context_unref(owner);                                           
    if (!clients) return;
                                                         
                                                  
    for (guint i = 0; i < clients->len; i++) {
        SoupWebsocketConnection *ws = g_ptr_array_index(clients, i);
        g_signal_handlers_disconnect_by_func(ws, G_CALLBACK(_on_ws_closed), NULL);
        g_signal_handlers_disconnect_by_func(ws, G_CALLBACK(_on_ws_message), NULL);
        _vnc_transport_close(ws);
    }
    g_ptr_array_unref(clients);
}

                                                                 
gboolean pcv_ws_server_is_quiescent(void)
{
    return g_atomic_int_get(&vnc_sessions) == 0 &&
           g_atomic_int_get(&vnc_auth_tasks) == 0 &&
           g_atomic_int_get(&vnc_close_waits) == 0 &&
           g_atomic_int_get(&vnc_transport_closes) == 0;
}

                                                                 
                                                
                                                          
                                                                     
                                                      
                                                    
                                                                         
static gboolean
_ws_broadcast_fanout(gpointer data)
{
    const gchar *msg = data;

                                                 
    g_mutex_lock(&G.mu);
    if (!G.clients) { g_mutex_unlock(&G.mu); return G_SOURCE_REMOVE; }                        
                                                   
                                                     
    for (guint i = 0; i < G.clients->len;                      ) {
        SoupWebsocketConnection *conn = g_ptr_array_index(G.clients, i);
        if (soup_websocket_connection_get_state(conn) != SOUP_WEBSOCKET_STATE_OPEN) {
            i++;                                                       
            continue;
        }
                                                                    
                                                            
                                                                      
        if (!_ws_conn_is_authed(conn)) {
            i++;
            continue;
        }
                                                               
                                                                         
                                                                
        gint *pending = g_object_get_data(G_OBJECT(conn), "pcv-pending");
        if (pending && g_atomic_int_get(pending) >= ws_max_pending_msgs) {
            const gchar *bp_ip = g_object_get_data(G_OBJECT(conn), "client-ip");
            PCV_LOG_WARN(WS_LOG_DOM,
                "WebSocket backpressure exceeded (%d msgs) for %s, closing",
                g_atomic_int_get(pending), bp_ip ? bp_ip : "unknown");
            soup_websocket_connection_close(conn,
                SOUP_WEBSOCKET_CLOSE_GOING_AWAY, "backpressure");
                                                                         
                                                                               
                                                                    
            g_ptr_array_remove_index(G.clients, i);
            continue;
        }
        gboolean sent = pcv_ws_send_text_if_open(conn, msg);                 
        if (sent && pending) g_atomic_int_add(pending, 1);                                    
                                                                               
        g_object_set_data(G_OBJECT(conn), "last_active",
                          GINT_TO_POINTER((gint)time(NULL)));
        i++;                               
    }
    g_mutex_unlock(&G.mu);
    return G_SOURCE_REMOVE;
}

                                                             
static gint ws_fanout_queued;
static gint ws_fanout_dropped;
static gint64 ws_fanout_last_warn_us;                                  
G_LOCK_DEFINE_STATIC(ws_fanout_warn);

                                                                  
                                                                   
                                                                              
static gboolean
_ws_fanout_slot_take(void)
{
    if (g_atomic_int_add(&ws_fanout_queued, 1) < WS_FANOUT_MAX_QUEUED)
        return TRUE;
    g_atomic_int_add(&ws_fanout_queued, -1);
    gint dropped = g_atomic_int_add(&ws_fanout_dropped, 1) + 1;
    gint64 now = g_get_monotonic_time();
    G_LOCK(ws_fanout_warn);
    gboolean warn = now - ws_fanout_last_warn_us >= WS_FANOUT_DROP_WARN_SEC * G_USEC_PER_SEC;
    if (warn) ws_fanout_last_warn_us = now;
    G_UNLOCK(ws_fanout_warn);
    if (warn)
        PCV_LOG_WARN(WS_LOG_DOM, "WebSocket broadcast queue full (%d queued, REST loop stalled?), dropped %d so far",
                     WS_FANOUT_MAX_QUEUED, dropped);
    return FALSE;
}

                                                              
static void
_ws_fanout_queued_free(gpointer data)
{
    g_free(data);
    g_atomic_int_add(&ws_fanout_queued, -1);
}

                                                          
                                                       
                                         
                                                                
static GMainContext *
_ws_owner_ref(void)
{
    g_mutex_lock(&G.mu);
    GMainContext *owner = (g_atomic_int_get(&G.initialized) && G.ctx)
        ? g_main_context_ref(G.ctx) : NULL;
    g_mutex_unlock(&G.mu);
    return owner;
}

   
                                                    
  
                                                     
                                                     
             
  
                                                      
                                                
  
                 
                                                              
                                                          
                       
                      
  
                                                     
                                                                                 
                                                               
                                                             
                                                                       
                                                                   
                                                                                                                                      
                                   
  
                                             
                                                                        
   
void
pcv_ws_broadcast(const gchar *type, const gchar *payload_json)
{
    if (!g_atomic_int_get(&G.initialized) || !type) return;                                  

                                          
                                                                  
    gint64 ts = (gint64)time(NULL);
    gchar *msg = g_strdup_printf("{\"type\":\"%s\",\"ts\":%ld,\"payload\":%s}",
                                  type, (long)ts,
                                  payload_json ? payload_json : "{}");

                                                                 
                                                                             
    GMainContext *owner = _ws_owner_ref();
    gboolean queued = owner && !g_main_context_is_owner(owner);                           
    if (queued && !_ws_fanout_slot_take()) {
        g_free(msg);
        g_main_context_unref(owner);
        return;
    }
    if (!pcv_ws_dispatch_on_owner(owner, _ws_broadcast_fanout, msg, queued ? _ws_fanout_queued_free : g_free)) {
        if (queued) g_atomic_int_add(&ws_fanout_queued, -1);
        g_free(msg);
    }
    if (owner) g_main_context_unref(owner);
}

                                                                           
                                                
                                                                                         
                                                                                          
                                                                                  
static gboolean
_ws_close_subject_on_owner(gpointer data)
{
    const gchar *user = data;
    GPtrArray *victims = g_ptr_array_new();
    g_mutex_lock(&G.mu);
    for (guint i = 0; G.clients && i < G.clients->len; i++) {
        SoupWebsocketConnection *conn = g_ptr_array_index(G.clients, i);
        if (_ws_conn_is_authed(conn) &&
            g_strcmp0(g_object_get_data(G_OBJECT(conn), "pcv-ws-subject"), user) == 0)
            g_ptr_array_add(victims, g_object_ref(conn));
    }
    g_mutex_unlock(&G.mu);
    for (guint i = 0; i < victims->len; i++) {
        SoupWebsocketConnection *conn = g_ptr_array_index(victims, i);
        if (soup_websocket_connection_get_state(conn) == SOUP_WEBSOCKET_STATE_OPEN)
            soup_websocket_connection_close(conn, SOUP_WEBSOCKET_CLOSE_POLICY_VIOLATION, "subject removed");
        g_object_unref(conn);
    }
    PCV_LOG_INFO(WS_LOG_DOM, "Closed %u WebSocket event connection(s) of removed subject", victims->len);
    g_ptr_array_free(victims, TRUE);
    _vnc_close_subject(user);
    return G_SOURCE_REMOVE;
}

                                                                
void
pcv_ws_close_subject(const gchar *username)
{
    if (!username || !*username) return;
    gchar *user = g_strdup(username);
    GMainContext *owner = _ws_owner_ref();
    if (!pcv_ws_dispatch_on_owner(owner, _ws_close_subject_on_owner, user, g_free))
        g_free(user);                               
    if (owner) g_main_context_unref(owner);
}

   
                              
  
                                            
  
                       
                       
  
                                  
   
gint
pcv_ws_client_count(void)
{
    if (!g_atomic_int_get(&G.initialized)) return 0;
    g_mutex_lock(&G.mu);
    gint n = G.clients ? (gint)G.clients->len : 0;
    g_mutex_unlock(&G.mu);
    return n;
}

                                                         
                                                       
                 
                                            
                                                         
                                                        
                                                                  
                                                            
                                                              
                                                    
                                                                    
                                                                       
static void
_broadcast_job_complete(const gchar *job_id, const gchar *method,
                        const gchar *status, const gchar *error_msg, gint persistence,
                        gint recovery, const gchar *reason)
{
    if (!job_id || !method || !status) return;
    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "job_id", job_id);
    json_object_set_string_member(obj, "method", method);
    json_object_set_string_member(obj, "status", status);
    if (error_msg && *error_msg)
        json_object_set_string_member(obj, "error", error_msg);
    if (persistence >= 0)
        json_object_set_boolean_member(obj, "result_persisted", persistence != 0);
    if (recovery >= 0) json_object_set_boolean_member(obj, "recovery_pending", recovery != 0);
    if (reason) json_object_set_string_member(obj, "recovery_reason", reason);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, obj);
    gchar *payload = json_to_string(node, FALSE);
    pcv_ws_broadcast("job.complete", payload);
    g_free(payload);
    json_node_free(node);
    PCV_LOG_INFO(WS_LOG_DOM, "Broadcast job.complete: job_id=%s method=%s status=%s",
                 job_id, method, status);
}

void
pcv_ws_broadcast_job_complete(const gchar *job_id, const gchar *method,
                               const gchar *status, const gchar *error_msg)
{
    _broadcast_job_complete(job_id, method, status, error_msg, -1, -1, NULL);
}

                                                                               
                                                                                
                                                 
void
pcv_ws_broadcast_job_complete_mt(const gchar *job_id, const gchar *method,
                                  const gchar *status, const gchar *error_msg)
{
    _broadcast_job_complete(job_id, method, status, error_msg, -1, -1, NULL);
}

                                                 
                                                   
                                                                              
                                             
static void
_job_result_ready(const PcvJobRef *ref, PcvJobStatus status, const gchar *error,
                  gboolean persisted, gboolean pending, const gchar *reason,
                  gpointer data G_GNUC_UNUSED)
{
    _broadcast_job_complete(pcv_job_id(ref), pcv_job_method(ref),
        status == PCV_JOB_COMPLETED ? "completed" : "failed", error,
        persisted ? 1 : 0, pending ? 1 : 0, reason);
}

void
pcv_ws_finish_job(PcvJobRef *ref, PcvJobStatus status, const gchar *result, const gchar *error)
{
    pcv_job_finish_async(ref, status, result, error, _job_result_ready, NULL, NULL);
}
