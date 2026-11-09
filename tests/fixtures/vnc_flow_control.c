   
                           
                                                                     
                                                             
                                                        
                                                                                                
                                                                      
                                                                           
                                                                                 
                                                                                 
                                                  
                                                                      
   
#include <fcntl.h>
#include <dirent.h>
#include <poll.h>
#include <stdio.h>
#include <libvirt/libvirt.h>
static unsigned fixture_scale = 1;
#define PCV_VNC_ACK_TIMEOUT_MS (150 * fixture_scale)
#define PCV_VNC_AUTH_TIMEOUT_MS (150 * fixture_scale)
#define PCV_VNC_WRITE_TIMEOUT_MS (1500 * fixture_scale)
#define PCV_VNC_CLOSE_TIMEOUT_MS (50 * fixture_scale)
#ifndef PCV_VNC_SOURCE
#define PCV_VNC_SOURCE "src/api/ws_server.c"
#endif
                                                                                     
#include <libsoup/soup.h>
static GMainContext *probe_owner;                              
static gint probe_off_owner, probe_on_owner;
static void probe_send_text(SoupWebsocketConnection *conn, const char *text);
#define soup_websocket_connection_send_text(conn, text) probe_send_text((conn), (text))
#include PCV_VNC_SOURCE
#undef soup_websocket_connection_send_text
#include "src/api/rest_shutdown.h"

static void probe_send_text(SoupWebsocketConnection *conn, const char *text)
{
    if (probe_owner && !g_main_context_is_owner(probe_owner)) {
        g_atomic_int_inc(&probe_off_owner);                                           
        return;
    }
    if (probe_owner) g_atomic_int_inc(&probe_on_owner);
    soup_websocket_connection_send_text(conn, text);
}

void _pcv_log(GLogLevelFlags level, const gchar *domain, const gchar *fmt, ...)
{ (void)level; (void)domain; (void)fmt; }

gint pcv_config_get_int(const gchar *section, const gchar *key, gint fallback)
{ (void)section; (void)key; return fallback; }

                                                         
                                                  
static virConnectPtr test_conn;
static gboolean slow_auth;
static gint auth_entered;
gchar *pcv_jwt_verify_with_ip(const gchar *token,const gchar *ip,GError **error)
{ (void)ip;(void)error;g_atomic_int_inc(&auth_entered);if(slow_auth)g_usleep(300000 * fixture_scale);return !strcmp(token,"admin") || !strcmp(token,"operator") || !strcmp(token,"viewer") || !strcmp(token,"ghost") ? g_strdup(token) : NULL; }
gchar *pcv_jwt_verify(const gchar *token,GError **error)
{ return pcv_jwt_verify_with_ip(token,"127.0.0.1",error); }
                                                                     
                                                                                  
static GMutex subject_mu;
static gchar *deleted_subject;
static gboolean delete_after_check;
static gint subject_checks;
PcvSubjectCheck pcv_rbac_subject_check_token(const gchar *user,const gchar *token,PcvRole *role)
{
    (void)token; g_mutex_lock(&subject_mu); subject_checks++;
    PcvSubjectCheck verdict = deleted_subject && !g_strcmp0(user, deleted_subject) ? PCV_SUBJECT_REJECT : PCV_SUBJECT_OK;
    if (verdict == PCV_SUBJECT_OK && role)
        *role = !strcmp(user,"admin")?PCV_ROLE_ADMIN:!strcmp(user,"operator")?PCV_ROLE_OPERATOR:PCV_ROLE_VIEWER;
    if (verdict == PCV_SUBJECT_OK && delete_after_check) { delete_after_check = FALSE; g_free(deleted_subject); deleted_subject = g_strdup(user); }
    g_mutex_unlock(&subject_mu); return verdict;
}
gboolean pcv_rbac_role_permits(PcvRole role,const gchar *method)
{ (void)method;return role!=PCV_ROLE_VIEWER; }
gboolean pcv_client_identity_admission_try(GHashTable *counts,const gchar *key,gint limit)
{ (void)counts;(void)key;(void)limit;return TRUE; }
virConnectPtr virt_conn_pool_acquire(void){return test_conn;}
void virt_conn_pool_release(virConnectPtr conn){(void)conn;}
const PcvRestClientIdentity *pcv_rest_client_identity_get(SoupServerMessage *msg)
{(void)msg;return NULL;}
const gchar *pcv_rest_client_identity_get_client_ip(const PcvRestClientIdentity *identity)
{(void)identity;return "127.0.0.1";}

typedef struct {
    GMainContext *context;
    SoupServer *server;
    SoupSession *session;
    SoupWebsocketConnection *browser, *proxy;
    int listener, peer, port;
    GByteArray *bytes;
    gchar *checkpoint;
    guint checkpoints;
    gboolean ready, closed;
    gchar *close_reason;
} Fixture;

static void step(Fixture *f, guint ms)
{
    gint64 until = g_get_monotonic_time() + ms * fixture_scale * 1000;
    do { g_main_context_iteration(f->context, FALSE); g_usleep(1000); }
    while (g_get_monotonic_time() < until);
}

static void received(SoupWebsocketConnection *conn, SoupWebsocketDataType type,
                     GBytes *bytes, gpointer data)
{
    Fixture *f = data; (void)conn;
    gsize size; const void *buf = g_bytes_get_data(bytes, &size);
    if (type == SOUP_WEBSOCKET_DATA_BINARY) g_byte_array_append(f->bytes, buf, size);
    else {
        gchar *text = g_strndup(buf, size);
        if (strstr(text, "auth_ok")) f->ready = TRUE;
        if (strstr(text, "checkpoint")) { f->checkpoints++; g_free(f->checkpoint); f->checkpoint = text; }
        else g_free(text);
    }
}

static void closed(SoupWebsocketConnection *conn, gpointer data)
{
    Fixture *f = data;
    f->closed = TRUE;
    f->close_reason = g_strdup(soup_websocket_connection_get_close_data(conn));
}

static void upgraded(SoupServer *server, SoupServerMessage *msg, const char *path,
                     SoupWebsocketConnection *conn, gpointer data)
{
    Fixture *f = data; f->proxy = g_object_ref(conn);
    _on_vnc_connected(server, msg, path, conn, NULL);
}

static void connected(GObject *session, GAsyncResult *result, gpointer data)
{
    Fixture *f = data; GError *error = NULL;
    f->browser = soup_session_websocket_connect_finish(SOUP_SESSION(session), result, &error);
    g_assert_no_error(error); g_assert_nonnull(f->browser);
    g_signal_connect(f->browser, "message", G_CALLBACK(received), f);
    g_signal_connect(f->browser, "closed", G_CALLBACK(closed), f);
}

static void setup(Fixture *f)
{
    memset(f, 0, sizeof(*f)); f->peer = -1;
    f->context = g_main_context_new(); g_main_context_push_thread_default(f->context);
    f->bytes = g_byte_array_new();
    f->listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    g_assert_cmpint(f->listener, >=, 0);
    struct sockaddr_in a = {.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    for (f->port=5900; f->port<=6100; f->port++) {
        a.sin_port=htons(f->port); if (!bind(f->listener,(void *)&a,sizeof(a))) break;
    }
    g_assert_cmpint(f->port,<=,6100); g_assert_cmpint(listen(f->listener,8),==,0);
    f->server=soup_server_new(NULL,NULL);
    char *protocols[]={"pcv-vnc-flow-v1",NULL};
    soup_server_add_websocket_handler(f->server,"/vnc",NULL,protocols,upgraded,f,NULL);
    GError *error=NULL;
    g_assert_true(soup_server_listen_local(f->server,0,SOUP_SERVER_LISTEN_IPV4_ONLY,&error));
    g_assert_no_error(error);
    GSList *uris=soup_server_get_uris(f->server);
    gchar *url=g_strdup_printf("ws://127.0.0.1:%d/vnc?port=%d",g_uri_get_port(uris->data),f->port);
    g_slist_free_full(uris,(GDestroyNotify)g_uri_unref);
    f->session=soup_session_new();g_object_set(f->session,"proxy-resolver",NULL,NULL);
    SoupMessage *msg=soup_message_new("GET",url);
    soup_session_websocket_connect_async(f->session,msg,NULL,protocols,G_PRIORITY_DEFAULT,NULL,connected,f);
    g_object_unref(msg);g_free(url);
    for(guint i=0;i<1000 && !f->browser;i++)step(f,1);
    g_assert_nonnull(f->browser);g_assert_nonnull(f->proxy);
}

static void teardown(Fixture *f)
{
    if (soup_websocket_connection_get_state(f->browser)==SOUP_WEBSOCKET_STATE_OPEN)
        soup_websocket_connection_close(f->browser,1000,"done");
    step(f,250);soup_session_abort(f->session);soup_server_disconnect(f->server);
    g_clear_object(&f->browser);g_clear_object(&f->proxy);g_clear_object(&f->session);g_clear_object(&f->server);
    if(f->peer>=0)close(f->peer);close(f->listener);
    g_byte_array_unref(f->bytes);g_free(f->checkpoint);g_free(f->close_reason);
    step(f,100);g_main_context_pop_thread_default(f->context);g_main_context_unref(f->context);
}

                                                                  
                                                                
static void define_domain(Fixture *f)
{
    test_conn=virConnectOpen("test:///default");g_assert_nonnull(test_conn);
    gchar *xml=g_strdup_printf("<domain type='test'><name>vnc-flow-owned</name><uuid>11111111-1111-1111-1111-111111111111</uuid><memory unit='KiB'>65536</memory><vcpu>1</vcpu><os><type>hvm</type></os><metadata><owner xmlns='urn:purecvisor:metadata'>operator</owner></metadata><devices><graphics type='vnc' port='%d' listen='127.0.0.1'/></devices></domain>",f->port);
    virDomainPtr domain=virDomainCreateXML(test_conn,xml,0);g_assert_nonnull(domain);
    virDomainFree(domain);g_free(xml);
}

static void authenticate(Fixture *f,const char *token)
{
    gchar *auth=g_strdup_printf("{\"type\":\"auth\",\"token\":\"%s\",\"uuid\":\"11111111-1111-1111-1111-111111111111\"}",token);
    soup_websocket_connection_send_text(f->browser,auth);g_free(auth);
    for(guint i=0;i<150 && !f->ready && !f->closed;i++)step(f,1);
}

static void accept_peer(Fixture *f)
{
    g_assert_true(f->ready);
    f->peer=accept4(f->listener,NULL,NULL,SOCK_CLOEXEC|SOCK_NONBLOCK);g_assert_cmpint(f->peer,>=,0);
}

static int socket_fd(Fixture *f)
{
    DIR *dir=opendir("/proc/self/fd");g_assert_nonnull(dir);struct dirent *entry;int result=-1;
    while((entry=readdir(dir))) {
        char *end;int fd=strtol(entry->d_name,&end,10);if(*end)continue;
        struct sockaddr_in peer;socklen_t len=sizeof(peer);
        if(!getpeername(fd,(void *)&peer,&len) && peer.sin_family==AF_INET && ntohs(peer.sin_port)==f->port)result=fd;
    }
    closedir(dir);g_assert_cmpint(result,>=,0);return result;
}

static void acknowledge(Fixture *f,gboolean wrong)
{
    g_assert_nonnull(f->checkpoint);
    JsonParser *parser=json_parser_new();g_assert_true(json_parser_load_from_data(parser,f->checkpoint,-1,NULL));
    JsonObject *o=json_node_get_object(json_parser_get_root(parser));
    json_object_set_string_member(o,"type","ack");
    if(wrong)json_object_set_string_member(o,"nonce","wrong");
    gchar *text=json_to_string(json_parser_get_root(parser),FALSE);
    soup_websocket_connection_send_text(f->browser,text);g_free(text);g_object_unref(parser);
}

static void binary_relay(Fixture *f)
{
    guint8 screen[]={0,17,255,7,8,0,9},keys[]={9,0,3,250,4},buffer[5];
    g_assert_cmpint(send(f->peer,screen,sizeof(screen),MSG_NOSIGNAL),==,sizeof(screen));
    for(guint i=0;i<100 && !f->checkpoint;i++)step(f,1);
    g_assert_cmpmem(f->bytes->data,f->bytes->len,screen,sizeof(screen));
    acknowledge(f,FALSE);soup_websocket_connection_send_binary(f->browser,keys,sizeof(keys));
    gsize size=0;
    for(guint i=0;i<100 && size<sizeof(keys);i++) {
        step(f,1);ssize_t n=recv(f->peer,buffer+size,sizeof(buffer)-size,MSG_DONTWAIT);
        if(n>0)size+=n;else g_assert_true(errno==EAGAIN || errno==EWOULDBLOCK);
    }
    g_assert_cmpmem(buffer,size,keys,sizeof(keys));
}

                                                                                   
typedef struct { SoupWebsocketConnection *conn; gboolean ok, failed, closed; gchar *reason; GPtrArray *frames; } EventClient;
static void event_received(SoupWebsocketConnection *conn, SoupWebsocketDataType type, GBytes *bytes, gpointer data)
{
    EventClient *c = data; (void)conn; (void)type;
    gsize size; const gchar *buf = g_bytes_get_data(bytes, &size);
    gchar *text = g_strndup(buf, size);
    if (strstr(text, "auth_ok")) c->ok = TRUE;
    if (strstr(text, "auth_fail")) c->failed = TRUE;
    if (c->frames && !strstr(text, "auth_ok")) { g_ptr_array_add(c->frames, text); return; }
    g_free(text);
}
static void event_closed(SoupWebsocketConnection *conn, gpointer data)
{ EventClient *c = data; c->closed = TRUE; c->reason = g_strdup(soup_websocket_connection_get_close_data(conn)); }
static void event_connected(GObject *session, GAsyncResult *result, gpointer data)
{
    EventClient *c = data; GError *error = NULL;
    c->conn = soup_session_websocket_connect_finish(SOUP_SESSION(session), result, &error);
    g_assert_no_error(error);
    g_signal_connect(c->conn, "message", G_CALLBACK(event_received), c);
    g_signal_connect(c->conn, "closed", G_CALLBACK(event_closed), c);
}
static void events_upgraded(SoupServer *server, SoupServerMessage *msg, const char *path,
                            SoupWebsocketConnection *conn, gpointer data)
{ (void)data; _on_ws_connected(server, msg, path, conn, NULL); }
                                                                                            
static void owner_open(Fixture *f)
{
    pcv_drain_init();
    G.clients = g_ptr_array_new_with_free_func(g_object_unref);
    G.ip_counts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    G.ctx = g_main_context_ref(f->context);
    g_atomic_int_set(&G.initialized, 1);
}
static void owner_close(void)
{
    g_atomic_int_set(&G.initialized, 0);
    g_clear_pointer(&G.ctx, g_main_context_unref);
    g_clear_pointer(&G.clients, g_ptr_array_unref);
    g_clear_pointer(&G.ip_counts, g_hash_table_unref);
    pcv_drain_shutdown();
}
static void event_auth(Fixture *f, EventClient *c, const char *token)
{
    GSList *uris = soup_server_get_uris(f->server);
    gchar *url = g_strdup_printf("ws://127.0.0.1:%d/events", g_uri_get_port(uris->data));
    g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
    SoupMessage *msg = soup_message_new("GET", url);
    soup_session_websocket_connect_async(f->session, msg, NULL, NULL, G_PRIORITY_DEFAULT, NULL, event_connected, c);
    g_object_unref(msg); g_free(url);
    for (guint i = 0; i < 1000 && !c->conn; i++) step(f, 1);
    g_assert_nonnull(c->conn);
    gchar *auth = g_strdup_printf("{\"type\":\"auth\",\"token\":\"%s\"}", token);
    soup_websocket_connection_send_text(c->conn, auth); g_free(auth);
    for (guint i = 0; i < 200 && !c->ok && !c->failed && !c->closed; i++) step(f, 1);
}
static gpointer close_subject_thread(gpointer data) { pcv_ws_close_subject(data); return NULL; }
                                                                  
static void close_subject_off_thread(const gchar *user)
{ GThread *t = g_thread_new("delete-handler", close_subject_thread, (gpointer)user); g_thread_join(t); }

                                                                
typedef struct { GMainContext *ctx; guint count; gboolean job_complete; gboolean worker_is_owner; } BroadcastJob;
static gpointer broadcast_worker(gpointer data)
{
    BroadcastJob *job = data;
    job->worker_is_owner = g_main_context_is_owner(job->ctx);
    for (guint i = 0; i < job->count; i++) {
        gchar *payload = g_strdup_printf("{\"seq\":%u}", i);
        pcv_ws_broadcast("anomaly", payload);
        g_free(payload);
    }
    if (job->job_complete) {
        pcv_ws_broadcast_job_complete("job-a", "vm.start", "completed", NULL);
        pcv_ws_broadcast_job_complete_mt("job-b", "vm.clone", "failed", "disk \"full\"");
    }
    return NULL;
}
static void broadcast_off_thread(BroadcastJob *job)
{ GThread *t = g_thread_new("ebpf-telem", broadcast_worker, job); g_thread_join(t); }
                                            
static void drain_broadcasts(Fixture *f, gint base_work)
{ for (guint i = 0; i < 2000 && pcv_drain_get_work() > base_work; i++) step(f, 1); }

static gboolean busy_default(gpointer data) { (*(guint *)data)++; return G_SOURCE_CONTINUE; }
static gboolean stop_watchdog(gpointer data) { g_main_loop_quit(data); return G_SOURCE_REMOVE; }

int main(int argc,char **argv)
{
    g_assert_cmpint(argc,==,2); const char *mode=argv[1];
    const char *scale = getenv("PCV_VNC_TEST_SCALE");
    if (scale) fixture_scale = CLAMP(atoi(scale), 1, 5);
    if(!strcmp(mode,"session-cap")) {
        Fixture all[9];for(guint i=0;i<9;i++)setup(&all[i]);
        step(&all[8],30);g_assert_true(all[8].closed);
                                                                      
        for(int i=8;i>=0;i--)teardown(&all[i]);
        g_assert_cmpint(g_atomic_int_get(&vnc_sessions),==,0);printf("PASS %s\n",mode);return 0;
    }
    Fixture f;setup(&f);
    if(!strcmp(mode,"unauthenticated")) {
        step(&f,30);
        int peer=accept4(f.listener,NULL,NULL,SOCK_CLOEXEC|SOCK_NONBLOCK);
        g_assert_cmpint(peer,==,-1);g_assert_true(errno==EAGAIN || errno==EWOULDBLOCK);
        step(&f,250);g_assert_true(f.closed);
    } else if(!strcmp(mode,"auth-rejected") || !strcmp(mode,"viewer-rejected")) {
        define_domain(&f);authenticate(&f,!strcmp(mode,"viewer-rejected")?"viewer":"invalid");
        step(&f,50);g_assert_true(f.closed);g_assert_false(f.ready);
        int peer=accept4(f.listener,NULL,NULL,SOCK_CLOEXEC|SOCK_NONBLOCK);
        g_assert_cmpint(peer,==,-1);
    } else if(!strcmp(mode,"binary") || !strcmp(mode,"operator")) {
        define_domain(&f);authenticate(&f,!strcmp(mode,"operator")?"operator":"admin");accept_peer(&f);binary_relay(&f);
    } else if(!strcmp(mode,"checkpoint-pause") || !strcmp(mode,"wrong-ack") || !strcmp(mode,"ack-timeout")) {
        define_domain(&f);authenticate(&f,"admin");accept_peer(&f);
        guint8 first=1,second=2;
        g_assert_cmpint(send(f.peer,&first,1,MSG_NOSIGNAL),==,1);
        for(guint i=0;i<100 && !f.checkpoint;i++)step(&f,1);
        g_assert_cmpuint(f.bytes->len,==,1);g_assert_cmpuint(f.checkpoints,==,1);
        g_assert_cmpint(send(f.peer,&second,1,MSG_NOSIGNAL),==,1);step(&f,30);
        g_assert_cmpuint(f.bytes->len,==,1);                                      
        if(!strcmp(mode,"checkpoint-pause")) {
            acknowledge(&f,FALSE);step(&f,30);g_assert_cmpuint(f.bytes->len,==,2);g_assert_cmpuint(f.checkpoints,==,2);acknowledge(&f,FALSE);
        } else {
            if(!strcmp(mode,"wrong-ack"))acknowledge(&f,TRUE);
            step(&f,250);g_assert_true(f.closed);g_assert_cmpuint(f.bytes->len,==,1);
        }
    } else if(!strcmp(mode,"close-during-auth")) {
        define_domain(&f);slow_auth=TRUE;authenticate(&f,"admin");step(&f,450);
        g_assert_true(f.closed);g_assert_false(f.ready);
        int peer=accept4(f.listener,NULL,NULL,SOCK_CLOEXEC|SOCK_NONBLOCK);g_assert_cmpint(peer,==,-1);
    } else if(!strcmp(mode,"shutdown-active") || !strcmp(mode,"shutdown-auth") || !strcmp(mode,"shutdown-busy")) {
        define_domain(&f);
        pcv_drain_init(); pcv_worker_pool_init();
        int fd = -1;
        if (strcmp(mode,"shutdown-auth")) {
            authenticate(&f,"admin"); accept_peer(&f); fd=socket_fd(&f);
        } else {
            slow_auth=TRUE;
            soup_websocket_connection_send_text(f.browser,"{\"type\":\"auth\",\"token\":\"admin\",\"uuid\":\"11111111-1111-1111-1111-111111111111\"}");
            for(guint i=0;i<100 && !g_atomic_int_get(&auth_entered);i++)step(&f,1);
            g_assert_cmpint(pcv_drain_get_work(),>,0);
            g_assert_cmpint(g_atomic_int_get(&auth_entered),==,1);
        }
                                                                   
                                                              
        g_clear_object(&f.proxy);
        GMainLoop *loop=g_main_loop_new(f.context,FALSE);
        guint dispatches=0; GSource *busy=NULL,*watchdog=NULL;
        if(!strcmp(mode,"shutdown-busy")) {
            busy=g_idle_source_new();g_source_set_priority(busy,G_PRIORITY_DEFAULT);
            g_source_set_callback(busy,busy_default,&dispatches,NULL);g_source_attach(busy,f.context);
            watchdog=g_timeout_source_new(500*fixture_scale);g_source_set_priority(watchdog,G_PRIORITY_HIGH);
            g_source_set_callback(watchdog,stop_watchdog,loop,NULL);g_source_attach(watchdog,f.context);
        }
        pcv_rest_shutdown_schedule(f.server,loop);
        g_main_loop_run(loop); g_main_loop_unref(loop);
        if(busy){g_assert_cmpuint(dispatches,>,0);_vnc_source_clear(&busy);_vnc_source_clear(&watchdog);}
        g_assert_true(pcv_ws_server_is_quiescent());
        g_assert_true(f.closed); g_assert_cmpint(pcv_drain_get_work(),==,0);
        g_assert_true(!vnc_proxies || vnc_proxies->len==0);
        if(fd>=0)g_assert_cmpint(fcntl(fd,F_GETFD),==,-1);
        else {int peer=accept4(f.listener,NULL,NULL,SOCK_CLOEXEC|SOCK_NONBLOCK);g_assert_cmpint(peer,==,-1);}
        pcv_worker_pool_shutdown(); pcv_drain_shutdown();
    } else if(!strcmp(mode,"queue-cap") || !strcmp(mode,"partial-write")) {
        define_domain(&f);authenticate(&f,"admin");accept_peer(&f);
        int fd=socket_fd(&f),size=4096;
        g_assert_true(fcntl(fd,F_GETFL)&O_NONBLOCK);
        g_assert_cmpint(setsockopt(fd,SOL_SOCKET,SO_SNDBUF,&size,sizeof(size)),==,0);
        g_assert_cmpint(setsockopt(f.peer,SOL_SOCKET,SO_RCVBUF,&size,sizeof(size)),==,0);
        guint8 *data=g_malloc0(VNC_FRAME_CAP);memset(data,37,VNC_FRAME_CAP);
        guint frames=!strcmp(mode,"queue-cap")?256:2;
        for(guint i=0;i<frames;i++)soup_websocket_connection_send_binary(f.browser,data,VNC_FRAME_CAP);
        if(frames==256){step(&f,300);g_assert_true(f.closed);g_assert_cmpstr(f.close_reason,==,"VNC input limit or authentication required");}
        else {
                                                                 
                                                                
            gsize received=0;guint8 buffer[16384];gint64 until=g_get_monotonic_time()+3*G_TIME_SPAN_SECOND;
            while(received<frames*VNC_FRAME_CAP && g_get_monotonic_time()<until) {
                step(&f,1);ssize_t n=recv(f.peer,buffer,sizeof(buffer),MSG_DONTWAIT);
                if(n>0){for(ssize_t i=0;i<n;i++)g_assert_cmpuint(buffer[i],==,37);received+=n;}
            }
            g_assert_cmpuint(received,==,frames*VNC_FRAME_CAP);g_assert_false(f.closed);
        }
        g_free(data);
    } else if(!strcmp(mode,"oversized-frame")) {
        define_domain(&f);authenticate(&f,"admin");accept_peer(&f);
        guint8 *large=g_malloc0(VNC_FRAME_CAP+1);soup_websocket_connection_send_binary(f.browser,large,VNC_FRAME_CAP+1);g_free(large);
        step(&f,150);g_assert_true(f.closed);
        guint8 byte;g_assert_cmpint(recv(f.peer,&byte,1,MSG_DONTWAIT),==,0);
    } else if(!strcmp(mode,"subject-close")) {
                                                                              
        define_domain(&f);authenticate(&f,"operator");accept_peer(&f);owner_open(&f);
        close_subject_off_thread("admin");step(&f,50);
        g_assert_false(f.closed);g_assert_cmpuint(vnc_proxies->len,==,1);
        close_subject_off_thread("operator");
        g_assert_cmpuint(vnc_proxies->len,==,1);                              
        step(&f,100);g_assert_true(f.closed);g_assert_cmpstr(f.close_reason,==,"VNC subject removed");
        g_assert_cmpuint(vnc_proxies->len,==,0);
        guint8 byte;g_assert_cmpint(recv(f.peer,&byte,1,MSG_DONTWAIT),==,0);                 
        owner_close();
    } else if(!strcmp(mode,"subject-auth-race")) {
                                                                    
        define_domain(&f);delete_after_check=TRUE;authenticate(&f,"operator");step(&f,100);
        g_assert_true(f.closed);g_assert_false(f.ready);
        int peer=accept4(f.listener,NULL,NULL,SOCK_CLOEXEC|SOCK_NONBLOCK);g_assert_cmpint(peer,==,-1);
        g_mutex_lock(&subject_mu);g_assert_cmpint(subject_checks,==,2);g_mutex_unlock(&subject_mu);
    } else if(!strcmp(mode,"events-subject")) {
                                                                          
        owner_open(&f);
        soup_server_add_websocket_handler(f.server,"/events",NULL,NULL,events_upgraded,NULL,NULL);
        g_mutex_lock(&subject_mu);deleted_subject=g_strdup("ghost");g_mutex_unlock(&subject_mu);
        EventClient op={0},adm={0},gone={0};
        event_auth(&f,&op,"operator");event_auth(&f,&adm,"admin");event_auth(&f,&gone,"ghost");
        g_assert_true(op.ok);g_assert_true(adm.ok);
        step(&f,50);g_assert_true(gone.failed);g_assert_true(gone.closed);g_assert_false(gone.ok);
        close_subject_off_thread("operator");
        g_assert_false(op.closed);
        step(&f,100);g_assert_true(op.closed);g_assert_cmpstr(op.reason,==,"subject removed");g_assert_false(adm.closed);
        soup_websocket_connection_close(adm.conn,1000,"done");step(&f,100);
        g_assert_cmpuint(G.clients->len,==,0);
        g_clear_object(&op.conn);g_clear_object(&adm.conn);g_clear_object(&gone.conn);
        g_free(op.reason);g_free(adm.reason);g_free(gone.reason);
        owner_close();
    } else if(!strcmp(mode,"broadcast-affinity")) {
                                                                        
        owner_open(&f);
        soup_server_add_websocket_handler(f.server,"/events",NULL,NULL,events_upgraded,NULL,NULL);
        EventClient c={0}; c.frames=g_ptr_array_new_with_free_func(g_free);
        event_auth(&f,&c,"admin"); g_assert_true(c.ok);
        probe_owner=f.context;
        gint base_work=pcv_drain_get_work();
        BroadcastJob job={.ctx=f.context,.count=200,.job_complete=TRUE};
        broadcast_off_thread(&job);
        g_assert_false(job.worker_is_owner);
        g_assert_cmpint(g_atomic_int_get(&probe_off_owner),==,0);                         
        g_assert_cmpint(g_atomic_int_get(&probe_on_owner),==,0);                         
        g_assert_cmpint(pcv_drain_get_work(),>,base_work);                                  
        drain_broadcasts(&f,base_work);
        for (guint i = 0; i < 2000 && c.frames->len < 202; i++) step(&f,1);
        g_assert_cmpuint(c.frames->len,==,202);
        g_assert_cmpint(g_atomic_int_get(&probe_on_owner),==,202);
        g_assert_cmpint(g_atomic_int_get(&probe_off_owner),==,0);
        g_assert_cmpint(pcv_drain_get_work(),==,base_work);
        for (guint i = 0; i < 200; i++) {
            gchar *want = g_strdup_printf("\"payload\":{\"seq\":%u}}", i);
            g_assert_nonnull(strstr(g_ptr_array_index(c.frames,i),want));
            g_assert_nonnull(strstr(g_ptr_array_index(c.frames,i),"\"type\":\"anomaly\""));
            g_free(want);
        }
                                                   
        g_assert_nonnull(strstr(g_ptr_array_index(c.frames,200),"\"job_id\":\"job-a\""));
        g_assert_nonnull(strstr(g_ptr_array_index(c.frames,201),"\"job_id\":\"job-b\""));
        g_assert_nonnull(strstr(g_ptr_array_index(c.frames,201),"\"status\":\"failed\""));
        g_assert_nonnull(strstr(g_ptr_array_index(c.frames,201),"disk \\\"full\\\""));
                                                
        g_assert_true(g_main_context_acquire(f.context));
        pcv_ws_broadcast("inline","{}");
        g_assert_cmpint(g_atomic_int_get(&probe_on_owner),==,203);
        g_assert_cmpint(pcv_drain_get_work(),==,base_work);
        g_main_context_release(f.context);
        probe_owner=NULL;
        soup_websocket_connection_close(c.conn,1000,"done");step(&f,100);
        g_clear_object(&c.conn);g_free(c.reason);g_ptr_array_unref(c.frames);
        owner_close();
    } else if(!strcmp(mode,"broadcast-flood")) {
                                                                  
        owner_open(&f);
        gint base_work=pcv_drain_get_work();
        BroadcastJob job={.ctx=f.context,.count=1524};
        broadcast_off_thread(&job);
        g_assert_cmpint(pcv_drain_get_work()-base_work,==,1024);
        drain_broadcasts(&f,base_work);
        g_assert_cmpint(pcv_drain_get_work(),==,base_work);
                           
        BroadcastJob again={.ctx=f.context,.count=3};
        broadcast_off_thread(&again);
        g_assert_cmpint(pcv_drain_get_work()-base_work,==,3);
        drain_broadcasts(&f,base_work);
        g_assert_cmpint(pcv_drain_get_work(),==,base_work);
        owner_close();
    } else if(!strcmp(mode,"broadcast-shutdown")) {
                                                                
        owner_open(&f);
        soup_server_add_websocket_handler(f.server,"/events",NULL,NULL,events_upgraded,NULL,NULL);
        EventClient c={0}; c.frames=g_ptr_array_new_with_free_func(g_free);
        event_auth(&f,&c,"admin"); g_assert_true(c.ok);
        gint base_work=pcv_drain_get_work();
        BroadcastJob job={.ctx=f.context,.count=50};
        broadcast_off_thread(&job);
        g_assert_cmpint(pcv_drain_get_work()-base_work,==,50);
        pcv_ws_server_shutdown();                                        
        drain_broadcasts(&f,base_work);
        g_assert_cmpint(pcv_drain_get_work(),==,base_work);
        g_assert_cmpuint(c.frames->len,==,0);                                 
                                 
        BroadcastJob late={.ctx=f.context,.count=5};
        broadcast_off_thread(&late);
        g_assert_cmpint(pcv_drain_get_work(),==,base_work);
        step(&f,100);
        g_clear_object(&c.conn);g_free(c.reason);g_ptr_array_unref(c.frames);
        pcv_drain_shutdown();
    } else if(!strcmp(mode,"owner-projection")) {
        const char *xml="<domain><metadata><owner xmlns='urn:purecvisor:metadata'>operator</owner></metadata><devices><graphics type='vnc' port='5905' listen='127.0.0.1'/></devices></domain>";
        g_assert_cmpint(_vnc_xml_port(xml,"operator",PCV_ROLE_OPERATOR),==,5905);
        g_assert_cmpint(_vnc_xml_port(xml,"other",PCV_ROLE_OPERATOR),==,0);
        g_assert_cmpint(_vnc_xml_port(xml,"admin",PCV_ROLE_ADMIN),==,5905);
        g_assert_cmpint(_vnc_xml_port("<!DOCTYPE domain [<!ENTITY owner 'operator'>]><domain/>","operator",PCV_ROLE_OPERATOR),==,0);
        g_assert_cmpint(_vnc_xml_port("<domain><devices><graphics type='vnc' port='5905' listen='0.0.0.0'/></devices></domain>","admin",PCV_ROLE_ADMIN),==,0);
        g_assert_cmpint(_vnc_xml_port("<domain><metadata><owner>operator</owner></metadata><devices><graphics type='vnc' port='5905' listen='127.0.0.1'/></devices></domain>","operator",PCV_ROLE_OPERATOR),==,0);
    } else g_assert_not_reached();
    teardown(&f);if(test_conn){virConnectClose(test_conn);test_conn=NULL;}
    g_assert_cmpint(g_atomic_int_get(&vnc_sessions),==,0);
    printf("PASS %s\n",mode);return 0;
}
