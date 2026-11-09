   
                    
                                                                
                           
                                                               
                                                            
                                                                  
                                                                
                                                 
                                               
                                                       
   
#ifndef PCV_VNC_PROXY_H
#define PCV_VNC_PROXY_H
#include "modules/auth/pcv_rbac.h"
#include "modules/virt/virt_conn_pool.h"
#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <fcntl.h>
#include <sys/random.h>
#include "utils/pcv_worker_pool.h"
#include "api/ws_send.h"                                                              

#define VNC_PROTOCOL "pcv-vnc-flow-v1"
#define VNC_BATCH (256u * 1024u)
#define VNC_QUEUE_CAP (4u * 1024u * 1024u)
#define VNC_FRAME_CAP (32u * 1024u)
#define VNC_MAX_SESSIONS 8
#ifndef PCV_VNC_AUTH_TIMEOUT_MS
#define PCV_VNC_AUTH_TIMEOUT_MS 5000
#endif
#ifndef PCV_VNC_ACK_TIMEOUT_MS
#define PCV_VNC_ACK_TIMEOUT_MS 10000
#endif
#ifndef PCV_VNC_WRITE_TIMEOUT_MS
#define PCV_VNC_WRITE_TIMEOUT_MS 10000
#endif
#ifndef PCV_VNC_CLOSE_TIMEOUT_MS
#define PCV_VNC_CLOSE_TIMEOUT_MS 2000
#endif

typedef struct {
    SoupWebsocketConnection *ws;
    GMainContext *context;
    int tcp_fd, port;
    GIOChannel *tcp_chan;
    GSource *tcp_watch, *write_watch, *deadline, *progress_watch;
    GByteArray *pending;
    gchar *nonce, *client_ip;
    gchar *auth_subject;                                                   
    guint64 sequence;
    gsize input_highwater, screen_batch_highwater;                            
    gboolean closing, authenticating, connected, awaiting_ack, eof;
} VncProxy;

typedef struct { gchar *token, *uuid, *client_ip; int requested_port; } VncAuth;
typedef struct { int port; gchar *subject; } VncAuthResult;                                     
static gint vnc_sessions;
static gint vnc_auth_tasks, vnc_close_waits, vnc_transport_closes;
static gboolean vnc_stopping;                         
static GPtrArray *vnc_proxies;                                    
static void _vnc_proxy_close(VncProxy *vp, const char *reason);
static gboolean _vnc_tcp_readable(GIOChannel *, GIOCondition, gpointer);
static gboolean _vnc_tcp_writable(GIOChannel *, GIOCondition, gpointer);

                                                              
                                                      
static void _vnc_source_clear(GSource **source)
{
    if (*source) { g_source_destroy(*source); g_clear_pointer(source, g_source_unref); }
}

                                                           
                                                     
static void _vnc_budget_release(gpointer data, GObject *object)
{ (void)data; (void)object; g_atomic_int_add(&vnc_sessions, -1); }

static void _vnc_proxy_free(VncProxy *vp)
{
    if (!vp) return;
    vp->closing = TRUE;
    PCV_LOG_INFO(WS_LOG_DOM,"VNC relay released input_highwater=%zu screen_batch_highwater=%zu",
                 vp->input_highwater,vp->screen_batch_highwater);
    if (vp->ws) {
        g_signal_handlers_disconnect_by_data(vp->ws, vp);
        g_object_set_data(G_OBJECT(vp->ws), "pcv-vnc-proxy", NULL);
    }
    _vnc_source_clear(&vp->tcp_watch);
    _vnc_source_clear(&vp->write_watch);
    _vnc_source_clear(&vp->deadline);
    _vnc_source_clear(&vp->progress_watch);
    if (vnc_proxies) g_ptr_array_remove_fast(vnc_proxies, vp);
    if (vp->tcp_chan) { g_io_channel_unref(vp->tcp_chan); vp->tcp_chan = NULL; }
    if (vp->tcp_fd >= 0) { close(vp->tcp_fd); vp->tcp_fd = -1; }
    g_clear_pointer(&vp->pending, g_byte_array_unref);
    g_clear_pointer(&vp->nonce, g_free);
    g_clear_pointer(&vp->auth_subject, g_free);
    g_clear_pointer(&vp->client_ip, g_free);
    g_clear_pointer(&vp->context, g_main_context_unref);
    g_clear_object(&vp->ws);
    g_free(vp);
}

                                                                     
                                                                  
typedef struct { SoupWebsocketConnection *ws; GSource *source; } VncCloseWait;
static void _vnc_close_wait_free(gpointer data)
{
    VncCloseWait *wait = data;
    g_signal_handlers_disconnect_by_data(wait->ws, wait);
    g_object_unref(wait->ws);
    g_free(wait);
    g_atomic_int_add(&vnc_close_waits, -1);
}
static void _vnc_close_finished(SoupWebsocketConnection *ws, gpointer data)
{
    (void)ws; VncCloseWait *wait = data;
    g_source_destroy(wait->source);
}
                                             
                                                
static void _vnc_transport_closed(GObject *stream, GAsyncResult *result, gpointer data)
{
    (void)data;
    GError *error = NULL;
    g_io_stream_close_finish(G_IO_STREAM(stream), result, &error);
    g_clear_error(&error);
    g_atomic_int_add(&vnc_transport_closes, -1);
}
static void _vnc_transport_close(SoupWebsocketConnection *ws)
{
    g_atomic_int_inc(&vnc_transport_closes);
    g_io_stream_close_async(soup_websocket_connection_get_io_stream(ws),
                           G_PRIORITY_DEFAULT, NULL, _vnc_transport_closed, NULL);
}
static gboolean _vnc_close_transport(gpointer data)
{
    VncCloseWait *wait = data;
    SoupWebsocketConnection *ws = wait->ws;
    if (soup_websocket_connection_get_state(ws) != SOUP_WEBSOCKET_STATE_CLOSED)
        _vnc_transport_close(ws);
    return G_SOURCE_REMOVE;
}

static void _vnc_proxy_close(VncProxy *vp, const char *reason)
{
    if (!vp || vp->closing) return;
    SoupWebsocketConnection *ws = g_object_ref(vp->ws);
    GMainContext *context = g_main_context_ref(vp->context);
    _vnc_proxy_free(vp);
    if (soup_websocket_connection_get_state(ws) == SOUP_WEBSOCKET_STATE_OPEN)
        soup_websocket_connection_close(ws, SOUP_WEBSOCKET_CLOSE_GOING_AWAY, reason);
    if (soup_websocket_connection_get_state(ws) != SOUP_WEBSOCKET_STATE_CLOSED) {
        GSource *source = g_timeout_source_new(PCV_VNC_CLOSE_TIMEOUT_MS);
        VncCloseWait *wait = g_new0(VncCloseWait, 1);
        g_atomic_int_inc(&vnc_close_waits);
        wait->ws = g_object_ref(ws); wait->source = source;
        g_signal_connect(ws, "closed", G_CALLBACK(_vnc_close_finished), wait);
        g_source_set_callback(source, _vnc_close_transport, wait, _vnc_close_wait_free);
        g_source_attach(source, context);
        g_source_unref(source);
    }
    g_main_context_unref(context);
    g_object_unref(ws);
}

static gboolean _vnc_timeout(gpointer data)
{
    VncProxy *vp = data;
    _vnc_proxy_close(vp, "VNC progress timeout");
    return G_SOURCE_REMOVE;
}

                                                    
                                                       
                                                                  
                                                      
static void _vnc_deadline(VncProxy *vp, guint milliseconds)
{
    _vnc_source_clear(&vp->deadline);
    vp->deadline = g_timeout_source_new(milliseconds);
    g_source_set_callback(vp->deadline, _vnc_timeout, vp, NULL);
    g_source_attach(vp->deadline, vp->context);
}

static void _vnc_read_watch(VncProxy *vp)
{
    if (vp->tcp_watch || vp->awaiting_ack) return;
    vp->tcp_watch = g_io_create_watch(vp->tcp_chan, G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL);
    g_source_set_callback(vp->tcp_watch, G_SOURCE_FUNC(_vnc_tcp_readable), vp, NULL);
    GMainContext *context = vp->context;
    g_source_attach(vp->tcp_watch, context);
}

                                                                     
                                                                
static gboolean _vnc_tcp_readable(GIOChannel *chan, GIOCondition cond, gpointer data)
{
    VncProxy *vp = data; (void)chan;
    if (vp->closing) return G_SOURCE_REMOVE;
    if (cond & (G_IO_ERR | G_IO_NVAL)) { _vnc_proxy_close(vp, "VNC read failed"); return G_SOURCE_REMOVE; }
    guint8 buffer[65536]; gsize total = 0;
    for (guint i = 0; i < 4; i++) {
        ssize_t n = read(vp->tcp_fd, buffer, sizeof(buffer));
        if (n > 0) { soup_websocket_connection_send_binary(vp->ws, buffer, (gsize)n); total += n; }
        else if (!n) { vp->eof = TRUE; break; }
        else if (errno == EINTR) continue;
        else if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        else { _vnc_proxy_close(vp, "VNC read failed"); return G_SOURCE_REMOVE; }
    }
    vp->screen_batch_highwater = MAX(vp->screen_batch_highwater, total);
    if (!total && vp->eof) { _vnc_proxy_close(vp, "VNC closed"); return G_SOURCE_REMOVE; }
    if (!total) return G_SOURCE_CONTINUE;
    vp->awaiting_ack = TRUE; vp->sequence++;
    gchar *checkpoint = g_strdup_printf("{\"type\":\"checkpoint\",\"nonce\":\"%s\",\"sequence\":\"%" G_GUINT64_FORMAT "\"}",vp->nonce,vp->sequence);
    pcv_ws_send_text_if_open(vp->ws, checkpoint);                                               
    g_free(checkpoint);
    _vnc_source_clear(&vp->tcp_watch);
    _vnc_deadline(vp, PCV_VNC_ACK_TIMEOUT_MS);
    return G_SOURCE_REMOVE;
}

                                                            
                                                             
static gboolean _vnc_write_timeout(gpointer data)
{
    VncProxy *vp = data;
    gint64 *last = g_object_get_data(G_OBJECT(vp->ws), "pcv-vnc-write-at");
    if (vp->pending->len && last && g_get_monotonic_time() - *last >= PCV_VNC_WRITE_TIMEOUT_MS * 1000LL) {
        _vnc_proxy_close(vp, "VNC write timeout"); return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void _vnc_progress(VncProxy *vp)
{
    gint64 *last = g_object_get_data(G_OBJECT(vp->ws), "pcv-vnc-write-at");
    if (!last) { last = g_new(gint64, 1); g_object_set_data_full(G_OBJECT(vp->ws), "pcv-vnc-write-at", last, g_free); }
    *last = g_get_monotonic_time();
}

                                           
                                               
                                                          
static gboolean _vnc_tcp_writable(GIOChannel *chan, GIOCondition cond, gpointer data)
{
    VncProxy *vp = data; (void)chan;
    if (cond & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) { _vnc_proxy_close(vp, "VNC write failed"); return G_SOURCE_REMOVE; }
    if (!vp->connected) {
        int error = 0; socklen_t len = sizeof(error);
        if (getsockopt(vp->tcp_fd, SOL_SOCKET, SO_ERROR, &error, &len) || error) {
            _vnc_proxy_close(vp, "VNC connect failed"); return G_SOURCE_REMOVE;
        }
        vp->connected = TRUE; _vnc_source_clear(&vp->deadline);
        pcv_ws_send_text_if_open(vp->ws, "{\"type\":\"auth_ok\"}");
        _vnc_read_watch(vp);
    }
    gsize written = 0;
                                                                  
    for (guint attempts = 0; attempts < 16 && vp->pending->len && written < VNC_BATCH; attempts++) {
        gsize size = MIN((gsize)vp->pending->len, VNC_BATCH - written);
        ssize_t n = send(vp->tcp_fd, vp->pending->data, size, MSG_NOSIGNAL);
        if (n > 0) { g_byte_array_remove_range(vp->pending, 0, n); written += n; _vnc_progress(vp); }
        else if (n < 0 && errno == EINTR) continue;
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return G_SOURCE_CONTINUE;
        else { _vnc_proxy_close(vp, "VNC write failed"); return G_SOURCE_REMOVE; }
    }
    if (!vp->pending->len) { _vnc_source_clear(&vp->write_watch); return G_SOURCE_REMOVE; }
    return G_SOURCE_CONTINUE;
}

                                                           
static const char *_vnc_string(JsonObject *object, const char *name)
{
    JsonNode *node = json_object_get_member(object, name);
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING ? json_node_get_string(node) : NULL;
}

                                                              
                                                                     
                                                       
static int _vnc_xml_port(const char *xml, const char *subject, PcvRole role)
{
    if (!xml || strlen(xml) > 1024 * 1024) return 0;
    xmlDocPtr doc = xmlReadMemory(xml, strlen(xml), "vnc.xml", NULL, XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    if (!doc) return 0;
    int port = 0;
    if (doc->intSubset || doc->extSubset) { xmlFreeDoc(doc); return 0; }
    xmlXPathContextPtr ctx = xmlXPathNewContext(doc);
    xmlXPathObjectPtr graphics = ctx ? xmlXPathEvalExpression(BAD_CAST "/domain/devices/graphics[@type='vnc']", ctx) : NULL;
    if (graphics && graphics->nodesetval && graphics->nodesetval->nodeNr == 1) {
        xmlNodePtr node = graphics->nodesetval->nodeTab[0];
        xmlChar *value = xmlGetProp(node, BAD_CAST "port"), *listen = xmlGetProp(node, BAD_CAST "listen");
        gboolean loopback = listen && !xmlStrcmp(listen, BAD_CAST "127.0.0.1");
        guint listen_count = 0;
        for (xmlNodePtr child = node->children; child; child = child->next) {
            if (child->type != XML_ELEMENT_NODE || xmlStrcmp(child->name, BAD_CAST "listen")) continue;
            listen_count++;
            xmlChar *address = xmlGetProp(child, BAD_CAST "address"), *type = xmlGetProp(child, BAD_CAST "type");
            loopback = address && type && !xmlStrcmp(address, BAD_CAST "127.0.0.1") && !xmlStrcmp(type, BAD_CAST "address") && (!listen || !xmlStrcmp(listen, address));
            xmlFree(address); xmlFree(type);
        }
        if (value && loopback && listen_count <= 1) {
            gchar *end = NULL; gint64 n = g_ascii_strtoll((char *)value, &end, 10);
            if (end && !*end && n >= 5900 && n <= 6100) port = n;
        }
        xmlFree(value); xmlFree(listen);
    }
    if (port && role != PCV_ROLE_ADMIN) {
        xmlXPathObjectPtr owner = xmlXPathEvalExpression(BAD_CAST "/domain/metadata/*[local-name()='owner' and namespace-uri()='urn:purecvisor:metadata']", ctx);
        xmlChar *name = owner && owner->nodesetval && owner->nodesetval->nodeNr == 1 ? xmlNodeGetContent(owner->nodesetval->nodeTab[0]) : NULL;
        if (!name || !subject || strcmp((char *)name, subject)) port = 0;
        xmlFree(name); if (owner) xmlXPathFreeObject(owner);
    }
    if (graphics) xmlXPathFreeObject(graphics);
    if (ctx) xmlXPathFreeContext(ctx);
    xmlFreeDoc(doc); return port;
}

static void _vnc_auth_free(gpointer data)
{
    VncAuth *auth = data;
    if (auth->token) explicit_bzero(auth->token, strlen(auth->token));
    g_free(auth->token); g_free(auth->uuid); g_free(auth->client_ip); g_free(auth);
    g_atomic_int_add(&vnc_auth_tasks, -1);
}

                                                      
                                                        
                                                             
                                                       
static void _vnc_auth_result_free(gpointer data)
{
    VncAuthResult *result = data;
    if (!result) return;
    g_free(result->subject); g_free(result);
}

                                                                                                 
                                                                           
static void _vnc_auth_worker(GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
    VncAuth *auth = data; (void)source; (void)cancel;
    GError *error = NULL;
    gchar *subject = pcv_jwt_verify_with_ip(auth->token, auth->client_ip, &error);
    int port = 0;
    PcvRole role = PCV_ROLE_VIEWER;
    if (subject && pcv_rbac_subject_check_token(subject, auth->token, &role) == PCV_SUBJECT_OK
        && pcv_rbac_role_permits(role, "get_vnc_info")) {
        if (role == PCV_ROLE_OPERATOR || role == PCV_ROLE_ADMIN) {
            virConnectPtr conn = virt_conn_pool_acquire();
            if (conn) {
                virDomainPtr domain = virDomainLookupByUUIDString(conn, auth->uuid);
                if (domain && virDomainIsActive(domain) == 1) {
                    int state, reason;
                    if (!virDomainGetState(domain, &state, &reason, 0) && state == VIR_DOMAIN_RUNNING) {
                        char *xml = virDomainGetXMLDesc(domain, 0);
                        port = _vnc_xml_port(xml, subject, role);
                        if (port != auth->requested_port) port = 0;
                        g_free(xml);
                    }
                }
                if (domain) virDomainFree(domain);
                virt_conn_pool_release(conn);
            }
        }
    }
    g_clear_error(&error);
    VncAuthResult *result = g_new0(VncAuthResult, 1);
    result->port = port;
    result->subject = port ? subject : NULL;
    if (!port) g_free(subject);
    g_task_return_pointer(task, result, _vnc_auth_result_free);
}

static void _vnc_write_watch(VncProxy *vp)
{
    if (vp->write_watch) return;
    vp->write_watch = g_io_create_watch(vp->tcp_chan, G_IO_OUT | G_IO_HUP | G_IO_ERR | G_IO_NVAL);
    g_source_set_callback(vp->write_watch, G_SOURCE_FUNC(_vnc_tcp_writable), vp, NULL);
    g_source_attach(vp->write_watch, vp->context);
}

                                                           
                                                  
static void _vnc_auth_done(GObject *source, GAsyncResult *result, gpointer data)
{
    (void)data;
    VncAuthResult *outcome = g_task_propagate_pointer(G_TASK(result), NULL);
    VncAuth *auth = g_task_get_task_data(G_TASK(result));
    int port = outcome ? outcome->port : 0;
    VncProxy *vp = g_object_get_data(source, "pcv-vnc-proxy");
    if (!vp) { _vnc_auth_result_free(outcome); return; }
                                                                                  
                                                                      
    if (port && (!auth || pcv_rbac_subject_check_token(outcome->subject, auth->token, NULL) != PCV_SUBJECT_OK))
        port = 0;
    if (port) vp->auth_subject = g_steal_pointer(&outcome->subject);
    _vnc_auth_result_free(outcome);
    if (vnc_stopping || pcv_drain_is_shutdown() || !port || soup_websocket_connection_get_state(vp->ws) != SOUP_WEBSOCKET_STATE_OPEN) {
        _vnc_proxy_close(vp, "VNC authorization failed"); return;
    }
    _vnc_source_clear(&vp->deadline);
    vp->tcp_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (vp->tcp_fd < 0) { _vnc_proxy_close(vp, "VNC socket failed"); return; }
    struct sockaddr_in address = {.sin_family=AF_INET,.sin_port=htons(port),.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    if (connect(vp->tcp_fd, (void *)&address, sizeof(address)) && errno != EINPROGRESS) {
        _vnc_proxy_close(vp, "VNC connect failed"); return;
    }
    vp->tcp_chan = g_io_channel_unix_new(vp->tcp_fd);
    g_io_channel_set_encoding(vp->tcp_chan, NULL, NULL);
    g_io_channel_set_buffered(vp->tcp_chan, FALSE);
    g_io_channel_set_close_on_unref(vp->tcp_chan, FALSE);
    _vnc_deadline(vp, 3000);
    _vnc_write_watch(vp);
}

static void _vnc_ws_message_impl(SoupWebsocketConnection *conn, SoupWebsocketDataType type, GBytes *message, gpointer data)
{
    VncProxy *vp = data; (void)conn;
    gsize length; const void *bytes = g_bytes_get_data(message, &length);
    if (vp->closing) return;
    if (vnc_stopping || pcv_drain_is_shutdown()) { _vnc_proxy_close(vp, "VNC server shutdown"); return; }
    if (type == SOUP_WEBSOCKET_DATA_BINARY) {
        if (!vp->connected || length > VNC_FRAME_CAP || length > VNC_QUEUE_CAP - vp->pending->len) {
            _vnc_proxy_close(vp, "VNC input limit or authentication required"); return;
        }
        if (!length) return;
        if (!vp->pending->len) _vnc_progress(vp);
        g_byte_array_append(vp->pending, bytes, length);
        vp->input_highwater = MAX(vp->input_highwater, (gsize)vp->pending->len);
        _vnc_write_watch(vp); return;
    }
    if (type != SOUP_WEBSOCKET_DATA_TEXT || !length || length > 8192) {
        _vnc_proxy_close(vp, "VNC control rejected"); return;
    }
    JsonParser *parser = NULL;
    if (!pcv_rpc_parse_guarded(bytes, length, &parser, NULL) || !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
        g_clear_object(&parser); _vnc_proxy_close(vp, "VNC control rejected"); return;
    }
    JsonObject *object = json_node_get_object(json_parser_get_root(parser));
    const char *kind = _vnc_string(object, "type");
    if (!vp->connected && !vp->authenticating && !g_strcmp0(kind, "auth")) {
        const char *token = _vnc_string(object, "token"), *uuid = _vnc_string(object, "uuid");
        gchar *lower = uuid ? g_ascii_strdown(uuid, -1) : NULL;
        if (!token || !*token || !uuid || !g_uuid_string_is_valid(uuid) || strcmp(uuid, lower) || json_object_get_size(object) != 3) {
            g_free(lower); g_object_unref(parser); _vnc_proxy_close(vp, "VNC auth rejected"); return;
        }
        g_free(lower);
        VncAuth *auth = g_new0(VncAuth, 1);
        auth->token=g_strdup(token);auth->uuid=g_strdup(uuid);auth->client_ip=g_strdup(vp->client_ip);auth->requested_port=vp->port;
        vp->authenticating = TRUE;
        g_atomic_int_inc(&vnc_auth_tasks);
        GTask *task = pcv_drain_task_new(vp->ws, NULL, _vnc_auth_done, NULL);
        g_task_set_task_data(task, auth, _vnc_auth_free);
        pcv_worker_pool_push(task, _vnc_auth_worker); g_object_unref(task);
    } else if (vp->connected && vp->awaiting_ack && !g_strcmp0(kind, "ack")) {
        const char *nonce=_vnc_string(object,"nonce"),*seq=_vnc_string(object,"sequence");
        gchar *expected=g_strdup_printf("%" G_GUINT64_FORMAT,vp->sequence);
        gboolean match=!g_strcmp0(nonce,vp->nonce) && !g_strcmp0(seq,expected) && json_object_get_size(object)==3;
        g_free(expected);
        if (!match) { g_object_unref(parser); _vnc_proxy_close(vp,"VNC ACK rejected");return; }
        vp->awaiting_ack=FALSE;_vnc_source_clear(&vp->deadline);
        if (vp->eof) {g_object_unref(parser);_vnc_proxy_close(vp,"VNC closed");return;}
        _vnc_read_watch(vp);
    } else { g_object_unref(parser); _vnc_proxy_close(vp, "VNC control rejected"); return; }
    g_object_unref(parser);
}

                                         
                                              
static void _vnc_ws_message(SoupWebsocketConnection *conn, SoupWebsocketDataType type,
                            GBytes *message, gpointer data)
{
    if (!pcv_drain_admit_rest()) { _vnc_proxy_close(data, "VNC server shutdown"); return; }
    _vnc_ws_message_impl(conn, type, message, data);
    pcv_drain_work_release();
}

static void _vnc_ws_closed(SoupWebsocketConnection *conn, gpointer data)
{
    (void)conn; VncProxy *vp=data;
    PCV_LOG_INFO(WS_LOG_DOM,"VNC WebSocket closed");
    _vnc_proxy_free(vp);
}

                                                  
                                                            
static void _on_vnc_connected(SoupServer *server, SoupServerMessage *msg, const char *path, SoupWebsocketConnection *conn, gpointer data)
{
    (void)server;(void)path;(void)data;
    if (g_strcmp0(soup_websocket_connection_get_protocol(conn),VNC_PROTOCOL)) {
        soup_websocket_connection_close(conn,1008,"VNC viewer upgrade required");return;
    }
    GUri *uri=soup_server_message_get_uri(msg);
    const char *query=g_uri_get_query(uri);
    GHashTable *params=query?g_uri_parse_params(query,-1,"&",G_URI_PARAMS_NONE,NULL):NULL;
    const char *port_s=params?g_hash_table_lookup(params,"port"):NULL;
    gchar *end=NULL;gint64 port=port_s?g_ascii_strtoll(port_s,&end,10):0;
    gboolean digits = port_s && *port_s && strspn(port_s, "0123456789") == strlen(port_s);
    gboolean valid=params && g_hash_table_size(params)==1 && digits && end && !*end && port>=5900 && port<=6100;
    if(params)g_hash_table_unref(params);
    if(!valid){soup_websocket_connection_close(conn,1008,"VNC query rejected");return;}
    if (vnc_stopping || pcv_drain_is_shutdown()) { soup_websocket_connection_close(conn, 1001, "VNC server shutdown"); return; }
    gint count=g_atomic_int_get(&vnc_sessions);
    while(count<VNC_MAX_SESSIONS && !g_atomic_int_compare_and_exchange(&vnc_sessions,count,count+1))count=g_atomic_int_get(&vnc_sessions);
    if(count>=VNC_MAX_SESSIONS){soup_websocket_connection_close(conn,1008,"VNC session limit");return;}
    g_object_weak_ref(G_OBJECT(conn),_vnc_budget_release,NULL);
    VncProxy *vp=g_new0(VncProxy,1);
    vp->ws=g_object_ref(conn);vp->tcp_fd=-1;vp->port=port;
    vp->context=g_main_context_ref_thread_default();vp->pending=g_byte_array_new();
    const PcvRestClientIdentity *identity=pcv_rest_client_identity_get(msg);
    vp->client_ip=g_strdup(pcv_rest_client_identity_get_client_ip(identity));
    guint8 random[16];ssize_t n=getrandom(random,sizeof(random),0);
    if(n!=sizeof(random)){_vnc_proxy_close(vp,"VNC initialization failed");return;}
    vp->nonce=g_base64_encode(random,sizeof(random));
    if(!vnc_proxies) vnc_proxies=g_ptr_array_new();
    g_ptr_array_add(vnc_proxies,vp);
    g_object_set_data(G_OBJECT(conn),"pcv-vnc-proxy",vp);
    soup_websocket_connection_set_max_incoming_payload_size(conn,VNC_FRAME_CAP);
    g_signal_connect(conn,"message",G_CALLBACK(_vnc_ws_message),vp);
    g_signal_connect(conn,"closed",G_CALLBACK(_vnc_ws_closed),vp);
    _vnc_deadline(vp,PCV_VNC_AUTH_TIMEOUT_MS);
    vp->progress_watch=g_timeout_source_new(100);
    g_source_set_callback(vp->progress_watch,_vnc_write_timeout,vp,NULL);
    g_source_attach(vp->progress_watch,vp->context);
}

                                                                                
                                                                         
static void _vnc_close_subject(const gchar *user)
{
    for (;;) {
        VncProxy *victim = NULL;
        for (guint i = 0; vnc_proxies && i < vnc_proxies->len && !victim; i++) {
            VncProxy *candidate = g_ptr_array_index(vnc_proxies, i);
            if (candidate->auth_subject && !g_strcmp0(candidate->auth_subject, user)) victim = candidate;
        }
        if (!victim) return;
        _vnc_proxy_close(victim, "VNC subject removed");
    }
}

static void _vnc_shutdown(void)
{
    vnc_stopping = TRUE;
    while(vnc_proxies && vnc_proxies->len)_vnc_proxy_close(g_ptr_array_index(vnc_proxies,0),"VNC server shutdown");
    g_clear_pointer(&vnc_proxies,g_ptr_array_unref);
}
#endif
