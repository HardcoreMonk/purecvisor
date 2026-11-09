   
                                
                                                                                  
  
                           
                                                                                        
                                                           
                                                             
                                                                          
                                                                      
                                                             
                                                                           
                                                                        
                                                                                      
                                                                            
                           
                                            
  
                       
                                                           
                                                            
                        
   
#include <glib.h>
#include <string.h>

#include "api/drain.h"
#include "modules/dispatcher/guest_agent_exec.h"
#include "modules/dispatcher/guest_ip.h"
#include "modules/dispatcher/rpc_utils.h"
#include "modules/virt/virt_conn_pool.h"

#include <libvirt/libvirt.h>

                                
guint pcv_test_uds_response_count(void);
void pcv_test_uds_responses_reset(void);
const gchar *pcv_test_last_uds_response(void);
void pcv_test_audit_reset(void);
gint pcv_test_audit_call_count(void);
const gchar *pcv_test_audit_last_method(void);
const gchar *pcv_test_audit_last_target(void);
const gchar *pcv_test_audit_last_result(void);
const gchar *pcv_test_audit_last_source(void);
gint pcv_test_audit_last_error_code(void);

                                                              
#define QGA_IFACES_CMD "{\"execute\":\"guest-network-get-interfaces\"}"
static const gchar k_ok_json[] =
    "{\"return\":[{\"name\":\"eth0\",\"hardware-address\":\"52:54:00:AA:BB:CC\","
    "\"ip-addresses\":[{\"ip-address-type\":\"ipv4\",\"ip-address\":\"10.0.0.5\",\"prefix\":24}]}]}";

                                                                   
#define FAKE_BASE_S ((gint64)1000)

typedef enum { FAKE_OK, FAKE_FAIL } FakeMode;
                                           
typedef enum { FAKE_STATUS_PING_OK, FAKE_STATUS_PING_FAIL, FAKE_STATUS_NOT_FOUND } FakeStatusMode;

                                                     
                                                          
static struct {
    GMutex   mu;
    GCond    cond;
    gboolean gate_open;
    FakeMode mode;
    guint    calls;
    guint    fail_call;                                       
    gint     running;
    gint     max_running;
    gint64   now_us;
    gchar   *last_cmd;
    gint     last_timeout_s;
    FakeStatusMode status_mode;
    guint    status_calls;
    gchar   *last_status_name;
} g_fake;

                                                  
static gint  g_warn_count;
static gint  g_info_count;
static gchar g_last_warn[512];
static gchar g_last_info[512];

                                                                            
                                                            
static gchar *
_fake_agent_command(const gchar *uuid, const gchar *name, const gchar *cmd,
                    gint timeout_s, GError **error)
{
    (void)uuid; (void)name;
    g_mutex_lock(&g_fake.mu);
    guint call_no = ++g_fake.calls;
    g_fake.running++;
    if (g_fake.running > g_fake.max_running)
        g_fake.max_running = g_fake.running;
    g_free(g_fake.last_cmd);
    g_fake.last_cmd = g_strdup(cmd);
    g_fake.last_timeout_s = timeout_s;
    g_cond_broadcast(&g_fake.cond);
    while (!g_fake.gate_open)
        g_cond_wait(&g_fake.cond, &g_fake.mu);
    g_fake.running--;
    gboolean fail = g_fake.mode == FAKE_FAIL || g_fake.fail_call == call_no;
    g_mutex_unlock(&g_fake.mu);
    if (fail) {
        g_set_error_literal(error, g_quark_from_static_string("fake-agent"), 1,
                            "fake: agent not responding");
        return NULL;
    }
    return g_strdup(k_ok_json);
}

                                                         
static gint64
_fake_now_us(void)
{
    g_mutex_lock(&g_fake.mu);
    gint64 v = g_fake.now_us;
    g_mutex_unlock(&g_fake.mu);
    return v;
}

                                                                                     
                                                              
static gboolean
_fake_status_probe(const gchar *name, PcvGuestAgentStatus *out, GError **error)
{
    g_mutex_lock(&g_fake.mu);
    g_fake.status_calls++;
    g_fake.running++;
    if (g_fake.running > g_fake.max_running)
        g_fake.max_running = g_fake.running;
    g_free(g_fake.last_status_name);
    g_fake.last_status_name = g_strdup(name);
    g_cond_broadcast(&g_fake.cond);
    while (!g_fake.gate_open)
        g_cond_wait(&g_fake.cond, &g_fake.mu);
    g_fake.running--;
    FakeStatusMode mode = g_fake.status_mode;
    g_mutex_unlock(&g_fake.mu);
    if (mode == FAKE_STATUS_NOT_FOUND) {
        g_set_error_literal(error, g_quark_from_static_string("fake-status"),
                            PURE_RPC_ERR_VM_NOT_FOUND, "VM not found.");
        return FALSE;
    }
    out->running = TRUE;
    out->channel_configured = TRUE;
    out->channel_live = TRUE;
    out->agent_ping = mode == FAKE_STATUS_PING_OK;
    out->agent_error = mode == FAKE_STATUS_PING_OK ? NULL : g_strdup("fake: guest agent is not connected");
    return TRUE;
}

static const PcvGuestAgentOps k_fake_ops = {
    .agent_command = _fake_agent_command,
    .now_us        = _fake_now_us,
    .status_probe  = _fake_status_probe,
};

                                                      
static void
_fake_set_gate(gboolean open)
{
    g_mutex_lock(&g_fake.mu);
    g_fake.gate_open = open;
    g_cond_broadcast(&g_fake.cond);
    g_mutex_unlock(&g_fake.mu);
}

                                                     
static void
_fake_set_now_s(gint64 s)
{
    g_mutex_lock(&g_fake.mu);
    g_fake.now_us = (FAKE_BASE_S + s) * G_TIME_SPAN_SECOND;
    g_mutex_unlock(&g_fake.mu);
}

static void
_fake_set_mode(FakeMode mode)
{
    g_mutex_lock(&g_fake.mu);
    g_fake.mode = mode;
    g_mutex_unlock(&g_fake.mu);
}

static guint
_fake_calls(void)
{
    g_mutex_lock(&g_fake.mu);
    guint v = g_fake.calls;
    g_mutex_unlock(&g_fake.mu);
    return v;
}

                                                
                                    
static gint
_fake_wait_running(gint n)
{
    gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    g_mutex_lock(&g_fake.mu);
    while (g_fake.running < n) {
        if (!g_cond_wait_until(&g_fake.cond, &g_fake.mu, deadline))
            break;
    }
    gint got = g_fake.running;
    g_mutex_unlock(&g_fake.mu);
    return got;
}

                                                                      
static void
_capture_log(const gchar *domain, GLogLevelFlags level, const gchar *message, gpointer unused)
{
    (void)domain; (void)unused;
    if (level & G_LOG_LEVEL_WARNING) {
        g_atomic_int_inc(&g_warn_count);
        g_strlcpy(g_last_warn, message ? message : "", sizeof g_last_warn);
    } else if (level & G_LOG_LEVEL_MESSAGE) {
        g_atomic_int_inc(&g_info_count);
        g_strlcpy(g_last_info, message ? message : "", sizeof g_last_info);
    }
}

typedef struct {
    guint log_handler;
} Fx;

                                                                             
static void
fx_setup(Fx *fx, gconstpointer unused)
{
    (void)unused;
    g_assert_true(g_main_context_acquire(NULL));
    g_mutex_lock(&g_fake.mu);
    g_fake.gate_open = TRUE;
    g_fake.mode = FAKE_OK;
    g_fake.calls = 0;
    g_fake.fail_call = 0;
    g_fake.running = 0;
    g_fake.max_running = 0;
    g_fake.now_us = FAKE_BASE_S * G_TIME_SPAN_SECOND;
    g_clear_pointer(&g_fake.last_cmd, g_free);
    g_fake.last_timeout_s = 0;
    g_fake.status_mode = FAKE_STATUS_PING_OK;
    g_fake.status_calls = 0;
    g_clear_pointer(&g_fake.last_status_name, g_free);
    g_mutex_unlock(&g_fake.mu);
    pcv_test_uds_responses_reset();
    pcv_test_audit_reset();
    g_atomic_int_set(&g_warn_count, 0);
    g_atomic_int_set(&g_info_count, 0);
    g_last_warn[0] = '\0';
    g_last_info[0] = '\0';
    fx->log_handler = g_log_set_handler("guest_agent", G_LOG_LEVEL_WARNING | G_LOG_LEVEL_MESSAGE,
                                        _capture_log, NULL);
    pcv_guest_agent_set_ops_for_test(&k_fake_ops);
}

                                                                        
static void
fx_teardown(Fx *fx, gconstpointer unused)
{
    (void)unused;
    _fake_set_gate(TRUE);
    pcv_guest_agent_flush_for_test();
    pcv_guest_agent_set_ops_for_test(NULL);
    g_log_remove_handler("guest_agent", fx->log_handler);
    g_main_context_release(NULL);
}

                                                                           
static void
test_probe_fills_cache(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    const gchar *uuid = "6b1f0c2e-0000-4000-8000-0000000000a1";
    g_assert_null(pcv_guest_agent_cache_lookup(uuid));
    pcv_guest_agent_probe_request(uuid, "vm-a1");
    pcv_guest_agent_flush_for_test();
    g_assert_cmpuint(_fake_calls(), ==, 1);
    g_assert_cmpstr(g_fake.last_cmd, ==, QGA_IFACES_CMD);
    g_assert_cmpint(g_fake.last_timeout_s, ==, 5);

    GPtrArray *ifs = pcv_guest_agent_cache_lookup(uuid);
    g_assert_nonnull(ifs);
    g_assert_cmpuint(ifs->len, ==, 1);
    const PcvGuestIface *f = g_ptr_array_index(ifs, 0);
    g_assert_cmpstr(f->mac, ==, "52:54:00:aa:bb:cc");
    g_assert_cmpuint(f->ipv4->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(f->ipv4, 0), ==, "10.0.0.5");
    g_ptr_array_unref(ifs);                              

    _fake_set_now_s(59);
    pcv_guest_agent_probe_request(uuid, "vm-a1");
    pcv_guest_agent_flush_for_test();
    g_assert_cmpuint(_fake_calls(), ==, 1);
    ifs = pcv_guest_agent_cache_lookup(uuid);
    g_assert_nonnull(ifs);
    g_ptr_array_unref(ifs);

    _fake_set_now_s(60);
    g_assert_null(pcv_guest_agent_cache_lookup(uuid));
    pcv_guest_agent_probe_request(uuid, "vm-a1");
    pcv_guest_agent_flush_for_test();
    g_assert_cmpuint(_fake_calls(), ==, 2);
    ifs = pcv_guest_agent_cache_lookup(uuid);
    g_assert_nonnull(ifs);
    g_ptr_array_unref(ifs);
    g_assert_cmpint(g_atomic_int_get(&g_warn_count), ==, 0);
    g_assert_cmpint(g_atomic_int_get(&g_info_count), ==, 0);
    pcv_guest_agent_invalidate(uuid);
}

                                                                            
                                       
static void
test_probe_failure_sets_backoff(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    const gchar *uuid = "6b1f0c2e-0000-4000-8000-0000000000b1";
    _fake_set_mode(FAKE_FAIL);
    pcv_guest_agent_probe_request(uuid, "vm-b1");
    pcv_guest_agent_flush_for_test();
    g_assert_cmpuint(_fake_calls(), ==, 1);
    g_assert_null(pcv_guest_agent_cache_lookup(uuid));
    g_assert_cmpint(g_atomic_int_get(&g_warn_count), ==, 1);
    g_assert_cmpstr(g_last_warn, ==,
                    "guest agent of VM 'vm-b1' did not answer guest-network-get-interfaces "
                    "(fake: agent not responding); next try in 60 s");

    _fake_set_now_s(59);
    pcv_guest_agent_probe_request(uuid, "vm-b1");
    pcv_guest_agent_flush_for_test();
    g_assert_cmpuint(_fake_calls(), ==, 1);

    _fake_set_now_s(60);
    pcv_guest_agent_probe_request(uuid, "vm-b1");
    pcv_guest_agent_flush_for_test();
    g_assert_cmpuint(_fake_calls(), ==, 2);
    g_assert_null(pcv_guest_agent_cache_lookup(uuid));
    g_assert_cmpint(g_atomic_int_get(&g_warn_count), ==, 1);                                    

                                                              
    _fake_set_mode(FAKE_OK);
    _fake_set_now_s(179);
    pcv_guest_agent_probe_request(uuid, "vm-b1");
    pcv_guest_agent_flush_for_test();
    g_assert_cmpuint(_fake_calls(), ==, 2);
    _fake_set_now_s(180);
    pcv_guest_agent_probe_request(uuid, "vm-b1");
    pcv_guest_agent_flush_for_test();
    g_assert_cmpuint(_fake_calls(), ==, 3);
    GPtrArray *ifs = pcv_guest_agent_cache_lookup(uuid);
    g_assert_nonnull(ifs);
    g_ptr_array_unref(ifs);
    g_assert_cmpint(g_atomic_int_get(&g_info_count), ==, 1);
    g_assert_cmpstr(g_last_info, ==, "guest agent of VM 'vm-b1' answered again");
    g_assert_cmpint(g_atomic_int_get(&g_warn_count), ==, 1);
    pcv_guest_agent_invalidate(uuid);
}

                                                                     
static void
test_single_flight_per_vm(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    const gchar *uuid = "6b1f0c2e-0000-4000-8000-0000000000c1";
    _fake_set_gate(FALSE);
    for (gint i = 0; i < 5; i++)
        pcv_guest_agent_probe_request(uuid, "vm-c1");
    g_assert_cmpint(_fake_wait_running(1), ==, 1);
    g_usleep(50 * G_TIME_SPAN_MILLISECOND);
    g_assert_cmpuint(_fake_calls(), ==, 1);
    _fake_set_gate(TRUE);
    pcv_guest_agent_flush_for_test();
    g_assert_cmpuint(_fake_calls(), ==, 1);
    GPtrArray *ifs = pcv_guest_agent_cache_lookup(uuid);
    g_assert_nonnull(ifs);
    g_ptr_array_unref(ifs);
    pcv_guest_agent_invalidate(uuid);
}

                                                                    
static void
test_global_cap_two(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    static const gchar *uuids[] = {
        "6b1f0c2e-0000-4000-8000-0000000000d1", "6b1f0c2e-0000-4000-8000-0000000000d2",
        "6b1f0c2e-0000-4000-8000-0000000000d3", "6b1f0c2e-0000-4000-8000-0000000000d4",
    };
    _fake_set_gate(FALSE);
    for (guint i = 0; i < G_N_ELEMENTS(uuids); i++)
        pcv_guest_agent_probe_request(uuids[i], "vm-d");
    g_assert_cmpint(_fake_wait_running(2), ==, 2);
    g_usleep(100 * G_TIME_SPAN_MILLISECOND);                      
    g_mutex_lock(&g_fake.mu);
    gint running = g_fake.running;
    gint max_running = g_fake.max_running;
    g_mutex_unlock(&g_fake.mu);
    g_assert_cmpint(running, ==, 2);
    g_assert_cmpint(max_running, ==, 2);
    _fake_set_gate(TRUE);
    pcv_guest_agent_flush_for_test();
    g_assert_cmpuint(_fake_calls(), ==, 4);
    g_mutex_lock(&g_fake.mu);
    max_running = g_fake.max_running;
    g_mutex_unlock(&g_fake.mu);
    g_assert_cmpint(max_running, ==, 2);
    for (guint i = 0; i < G_N_ELEMENTS(uuids); i++) {
        GPtrArray *ifs = pcv_guest_agent_cache_lookup(uuids[i]);
        g_assert_nonnull(ifs);
        g_ptr_array_unref(ifs);
        pcv_guest_agent_invalidate(uuids[i]);
    }
}

                                                                                
static void
test_invalidate_on_lifecycle_stop(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    const gchar *uuid = "6b1f0c2e-0000-4000-8000-0000000000e1";
    pcv_guest_agent_probe_request(uuid, "vm-e1");
    pcv_guest_agent_flush_for_test();
    GPtrArray *ifs = pcv_guest_agent_cache_lookup(uuid);
    g_assert_nonnull(ifs);
    g_ptr_array_unref(ifs);

    pcv_guest_agent_invalidate(uuid);
    g_assert_null(pcv_guest_agent_cache_lookup(uuid));
    pcv_guest_agent_probe_request(uuid, "vm-e1");                                 
    pcv_guest_agent_flush_for_test();
    g_assert_cmpuint(_fake_calls(), ==, 2);
    ifs = pcv_guest_agent_cache_lookup(uuid);
    g_assert_nonnull(ifs);
    g_ptr_array_unref(ifs);
    pcv_guest_agent_invalidate(uuid);
    pcv_guest_agent_invalidate("6b1f0c2e-0000-4000-8000-0000000000ef");                    
    pcv_guest_agent_invalidate(NULL);
}

                                                                  
static void
test_terminating_refuses_new(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    const gchar *uuid = "6b1f0c2e-0000-4000-8000-0000000000f1";
    pcv_drain_init();
    g_assert_cmpint(pcv_drain_get_work(), ==, 0);                                        
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    pcv_drain_begin(loop, 5);
    g_assert_true(pcv_drain_is_terminating());
    pcv_guest_agent_probe_request(uuid, "vm-f1");
    g_main_loop_run(loop);                                                       
    pcv_guest_agent_flush_for_test();
    g_assert_cmpuint(_fake_calls(), ==, 0);
    g_assert_null(pcv_guest_agent_cache_lookup(uuid));
    pcv_drain_shutdown();
    g_main_loop_unref(loop);
    pcv_drain_init();

    pcv_guest_agent_probe_request(uuid, "vm-f1");
    pcv_guest_agent_flush_for_test();
    g_assert_cmpuint(_fake_calls(), ==, 1);
    pcv_guest_agent_invalidate(uuid);
}

                                                         
                                                         
static void
test_result_after_seal_is_dropped(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    const gchar *uuid = "6b1f0c2e-0000-4000-8000-0000000000f2";
    pcv_drain_init();
    _fake_set_gate(FALSE);
    pcv_guest_agent_probe_request(uuid, "vm-f2");
    g_assert_cmpint(_fake_wait_running(1), ==, 1);
    g_assert_cmpint(pcv_drain_get_work(), ==, 0);
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    pcv_drain_begin(loop, 5);
    g_main_loop_run(loop);                                               

    _fake_set_gate(TRUE);                                                                    
    pcv_guest_agent_flush_for_test();
    g_assert_cmpuint(_fake_calls(), ==, 1);
    g_assert_null(pcv_guest_agent_cache_lookup(uuid));
    g_assert_cmpint(g_atomic_int_get(&g_warn_count), ==, 0);
    g_assert_cmpint(g_atomic_int_get(&g_info_count), ==, 0);
    g_assert_cmpint(pcv_drain_get_work(), ==, 0);
    pcv_drain_shutdown();
    g_main_loop_unref(loop);
    pcv_drain_init();
    pcv_guest_agent_invalidate(uuid);
}

                                                  
                                                               
                                                               
                                                 
static void
test_invalidate_discards_inflight_result(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    const gchar *uuid = "6b1f0c2e-0000-4000-8000-0000000000f3";
    _fake_set_gate(FALSE);
    pcv_guest_agent_probe_request(uuid, "vm-f3");
    g_assert_cmpint(_fake_wait_running(1), ==, 1);
    pcv_guest_agent_invalidate(uuid);
    _fake_set_gate(TRUE);
    pcv_guest_agent_flush_for_test();
    g_assert_cmpuint(_fake_calls(), ==, 1);
    g_assert_null(pcv_guest_agent_cache_lookup(uuid));
    g_assert_cmpint(g_atomic_int_get(&g_warn_count), ==, 0);
    g_assert_cmpint(g_atomic_int_get(&g_info_count), ==, 0);

    const gchar *uuid2 = "6b1f0c2e-0000-4000-8000-0000000000f4";
    g_mutex_lock(&g_fake.mu);
    g_fake.fail_call = g_fake.calls + 1;                                       
    g_mutex_unlock(&g_fake.mu);
    _fake_set_gate(FALSE);
    pcv_guest_agent_probe_request(uuid2, "vm-f4");
    g_assert_cmpint(_fake_wait_running(1), ==, 1);
    pcv_guest_agent_invalidate(uuid2);
    pcv_guest_agent_probe_request(uuid2, "vm-f4");                                    
    g_assert_cmpint(_fake_wait_running(2), ==, 2);
    _fake_set_gate(TRUE);
    pcv_guest_agent_flush_for_test();
    g_assert_cmpuint(_fake_calls(), ==, 3);
    GPtrArray *ifs = pcv_guest_agent_cache_lookup(uuid2);                     
    g_assert_nonnull(ifs);
    g_ptr_array_unref(ifs);
    g_assert_cmpint(g_atomic_int_get(&g_warn_count), ==, 0);
    g_assert_cmpint(g_atomic_int_get(&g_info_count), ==, 0);
    pcv_guest_agent_invalidate(uuid2);

    pcv_guest_agent_probe_request(uuid, "vm-f3");                             
    pcv_guest_agent_flush_for_test();
    g_assert_cmpuint(_fake_calls(), ==, 4);
    ifs = pcv_guest_agent_cache_lookup(uuid);
    g_assert_nonnull(ifs);
    g_ptr_array_unref(ifs);
    pcv_guest_agent_invalidate(uuid);
}

                                                                              

static guint
_fake_status_calls(void)
{
    g_mutex_lock(&g_fake.mu);
    guint v = g_fake.status_calls;
    g_mutex_unlock(&g_fake.mu);
    return v;
}

static void
_fake_set_status_mode(FakeStatusMode mode)
{
    g_mutex_lock(&g_fake.mu);
    g_fake.status_mode = mode;
    g_mutex_unlock(&g_fake.mu);
}

                                                      
                                                     
typedef struct {
    GObject *server;
    GObject *connection;
} FakePeer;

static void
_peer_new(FakePeer *p)
{
    p->server = g_object_new(G_TYPE_OBJECT, NULL);
    p->connection = g_object_new(G_TYPE_OBJECT, NULL);
    g_object_add_weak_pointer(p->server, (gpointer *)&p->server);
    g_object_add_weak_pointer(p->connection, (gpointer *)&p->connection);
}

static void
_peer_submit(FakePeer *p, const gchar *name, const gchar *rpc_id)
{
    pcv_guest_agent_status_submit(name, rpc_id, (UdsServer *)p->server,
                                  (GSocketConnection *)p->connection);
}

                                                      
static void
_peer_release_and_assert_gone(FakePeer *p)
{
    g_object_unref(p->server);
    g_object_unref(p->connection);
    g_assert_null(p->server);
    g_assert_null(p->connection);
}

                                                                           
static JsonObject *
_last_response_member(JsonParser **parser_out, const gchar *member)
{
    JsonParser *parser = json_parser_new();
    g_assert_true(json_parser_load_from_data(parser, pcv_test_last_uds_response(), -1, NULL));
    JsonObject *root = json_node_get_object(json_parser_get_root(parser));
    g_assert_true(json_object_has_member(root, member));
    *parser_out = parser;
    return json_object_get_object_member(root, member);
}

                                                             
                                                        
static void
test_status_coalesces_per_vm(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    FakePeer peer[3];
    _fake_set_gate(FALSE);
    for (guint i = 0; i < G_N_ELEMENTS(peer); i++) {
        gchar id[8];
        g_snprintf(id, sizeof id, "%u", i + 1);
        _peer_new(&peer[i]);
        _peer_submit(&peer[i], "vm-g1", id);
    }
    g_assert_cmpint(_fake_wait_running(1), ==, 1);
    g_usleep(50 * G_TIME_SPAN_MILLISECOND);
    g_assert_cmpuint(_fake_status_calls(), ==, 1);
    g_assert_cmpuint(pcv_test_uds_response_count(), ==, 0);                              
    _fake_set_gate(TRUE);
    pcv_guest_agent_flush_for_test();
    g_assert_cmpuint(_fake_status_calls(), ==, 1);
    g_assert_cmpuint(pcv_test_uds_response_count(), ==, 3);
    g_assert_cmpint(pcv_test_audit_call_count(), ==, 3);
    g_mutex_lock(&g_fake.mu);
    g_assert_cmpstr(g_fake.last_status_name, ==, "vm-g1");
    g_mutex_unlock(&g_fake.mu);
    for (guint i = 0; i < G_N_ELEMENTS(peer); i++)
        _peer_release_and_assert_gone(&peer[i]);

    FakePeer late;
    _peer_new(&late);
    _peer_submit(&late, "vm-g1", "4");
    pcv_guest_agent_flush_for_test();
    g_assert_cmpuint(_fake_status_calls(), ==, 2);
    g_assert_cmpuint(pcv_test_uds_response_count(), ==, 4);
    _peer_release_and_assert_gone(&late);
}

                                                                                      
                                      
static void
test_status_response_shape(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    static const gchar *keys[] = {
        "name", "status", "message", "running", "channel_present", "channel_configured", "channel_live",
        "agent_ping", "reboot_required", "package_required", "can_ensure_channel", "install_commands",
    };
    FakePeer peer;
    _peer_new(&peer);
    _peer_submit(&peer, "vm-h1", "7");
    pcv_guest_agent_flush_for_test();
    JsonParser *parser = NULL;
    JsonObject *res = _last_response_member(&parser, "result");
    for (guint i = 0; i < G_N_ELEMENTS(keys); i++)
        g_assert_true(json_object_has_member(res, keys[i]));
    g_assert_cmpstr(json_object_get_string_member(res, "name"), ==, "vm-h1");
    g_assert_cmpstr(json_object_get_string_member(res, "status"), ==, "ok");
    g_assert_cmpstr(json_object_get_string_member(res, "message"), ==, "Guest agent is responding.");
    g_assert_true(json_object_get_boolean_member(res, "running"));
    g_assert_true(json_object_get_boolean_member(res, "channel_present"));
    g_assert_true(json_object_get_boolean_member(res, "agent_ping"));
    g_assert_false(json_object_get_boolean_member(res, "reboot_required"));
    g_assert_false(json_object_get_boolean_member(res, "package_required"));
    g_assert_true(json_object_get_boolean_member(res, "can_ensure_channel"));
    g_assert_false(json_object_has_member(res, "agent_error"));
    JsonObject *cmds = json_object_get_object_member(res, "install_commands");
    g_assert_true(json_object_has_member(cmds, "debian_ubuntu"));
    g_assert_true(json_object_has_member(cmds, "rhel_rocky_fedora"));
    g_assert_true(json_object_has_member(cmds, "suse"));
    g_object_unref(parser);

    _fake_set_status_mode(FAKE_STATUS_PING_FAIL);
    _peer_submit(&peer, "vm-h1", "8");
    pcv_guest_agent_flush_for_test();
    res = _last_response_member(&parser, "result");
    g_assert_cmpstr(json_object_get_string_member(res, "status"), ==, "agent_unavailable");
    g_assert_false(json_object_get_boolean_member(res, "agent_ping"));
    g_assert_true(json_object_get_boolean_member(res, "package_required"));
    g_assert_cmpstr(json_object_get_string_member(res, "agent_error"), ==,
                    "fake: guest agent is not connected");
    g_object_unref(parser);

    _fake_set_status_mode(FAKE_STATUS_NOT_FOUND);
    _peer_submit(&peer, "vm-h1", "9");
    pcv_guest_agent_flush_for_test();
    JsonObject *err = _last_response_member(&parser, "error");
    g_assert_cmpint(json_object_get_int_member(err, "code"), ==, PURE_RPC_ERR_VM_NOT_FOUND);
    g_assert_cmpstr(json_object_get_string_member(err, "message"), ==, "VM not found.");
    g_object_unref(parser);
    g_assert_cmpuint(pcv_test_uds_response_count(), ==, 3);
    _peer_release_and_assert_gone(&peer);
}

                                                                                           
static void
test_status_audits_with_literal_method(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    FakePeer peer;
    _peer_new(&peer);
    _peer_submit(&peer, "vm-i1", "1");
    pcv_guest_agent_flush_for_test();
    g_assert_cmpint(pcv_test_audit_call_count(), ==, 1);
    g_assert_cmpstr(pcv_test_audit_last_method(), ==, "vm.guest.agent.status");
    g_assert_cmpstr(pcv_test_audit_last_target(), ==, "vm-i1");
    g_assert_cmpstr(pcv_test_audit_last_result(), ==, "ok");
    g_assert_cmpstr(pcv_test_audit_last_source(), ==, "local");
    g_assert_cmpint(pcv_test_audit_last_error_code(), ==, 0);

    _fake_set_status_mode(FAKE_STATUS_NOT_FOUND);
    _peer_submit(&peer, "vm-i1", "2");
    pcv_guest_agent_flush_for_test();
    g_assert_cmpint(pcv_test_audit_call_count(), ==, 2);
    g_assert_cmpstr(pcv_test_audit_last_method(), ==, "vm.guest.agent.status");
    g_assert_cmpstr(pcv_test_audit_last_result(), ==, "fail");
    g_assert_cmpint(pcv_test_audit_last_error_code(), ==, PURE_RPC_ERR_VM_NOT_FOUND);
    _peer_release_and_assert_gone(&peer);
}

                                                                               
static void
test_status_refuses_while_terminating(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    pcv_drain_init();
    g_assert_cmpint(pcv_drain_get_work(), ==, 0);
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    pcv_drain_begin(loop, 5);
    FakePeer peer;
    _peer_new(&peer);
    _peer_submit(&peer, "vm-j1", "1");
    g_assert_cmpuint(pcv_test_uds_response_count(), ==, 1);
    JsonParser *parser = NULL;
    JsonObject *err = _last_response_member(&parser, "error");
    g_assert_cmpint(json_object_get_int_member(err, "code"), ==, PURE_RPC_ERR_INTERNAL_ERROR);
    g_assert_cmpstr(json_object_get_string_member(err, "message"), ==, "daemon is shutting down");
    g_object_unref(parser);
    g_main_loop_run(loop);
    pcv_guest_agent_flush_for_test();
    g_assert_cmpuint(_fake_status_calls(), ==, 0);
    pcv_drain_shutdown();
    g_main_loop_unref(loop);
    pcv_drain_init();
    _peer_release_and_assert_gone(&peer);
}

                                                                                
                                                                   
                                                                               
                                                                                      
                                                                    
                                                  
static void
test_status_real_ops_test_driver(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    virConnectPtr probe = virConnectOpen("test:///default");
    if (!probe) {
        g_test_skip("test:///default 연결 불가");
        return;
    }
    g_assert_cmpint(virConnectIsAlive(probe), ==, 1);
    virConnectClose(probe);

    g_setenv("PCV_LIBVIRT_URI", "test:///default", TRUE);
    g_assert_cmpstr(virt_conn_pool_uri(), ==, "test:///default");
    pcv_guest_agent_set_ops_for_test(NULL);
    FakePeer peer;
    _peer_new(&peer);
    _peer_submit(&peer, "test", "1");
    pcv_guest_agent_flush_for_test();
    JsonParser *parser = NULL;
    JsonObject *res = _last_response_member(&parser, "result");
    g_assert_cmpstr(json_object_get_string_member(res, "name"), ==, "test");
    g_assert_cmpstr(json_object_get_string_member(res, "status"), ==, "channel_missing");
    g_assert_true(json_object_get_boolean_member(res, "running"));
    g_assert_false(json_object_get_boolean_member(res, "channel_present"));
    g_assert_false(json_object_get_boolean_member(res, "agent_ping"));
    g_assert_false(json_object_has_member(res, "agent_error"));
    g_object_unref(parser);

    _peer_submit(&peer, "pcv-no-such-vm", "2");
    pcv_guest_agent_flush_for_test();
    JsonObject *err = _last_response_member(&parser, "error");
    g_assert_cmpint(json_object_get_int_member(err, "code"), ==, PURE_RPC_ERR_VM_NOT_FOUND);
    g_assert_cmpstr(json_object_get_string_member(err, "message"), ==, "VM not found.");
    g_object_unref(parser);
    g_assert_cmpuint(pcv_test_uds_response_count(), ==, 2);

    pcv_guest_agent_set_ops_for_test(&k_fake_ops);
    g_unsetenv("PCV_LIBVIRT_URI");
    _peer_release_and_assert_gone(&peer);
}

void
test_guest_agent_exec_register(void)
{
    g_test_add("/guest_agent_exec/probe_fills_cache", Fx, NULL,
               fx_setup, test_probe_fills_cache, fx_teardown);
    g_test_add("/guest_agent_exec/probe_failure_sets_backoff", Fx, NULL,
               fx_setup, test_probe_failure_sets_backoff, fx_teardown);
    g_test_add("/guest_agent_exec/single_flight_per_vm", Fx, NULL,
               fx_setup, test_single_flight_per_vm, fx_teardown);
    g_test_add("/guest_agent_exec/global_cap_two", Fx, NULL,
               fx_setup, test_global_cap_two, fx_teardown);
    g_test_add("/guest_agent_exec/invalidate_on_lifecycle_stop", Fx, NULL,
               fx_setup, test_invalidate_on_lifecycle_stop, fx_teardown);
    g_test_add("/guest_agent_exec/terminating_refuses_new", Fx, NULL,
               fx_setup, test_terminating_refuses_new, fx_teardown);
    g_test_add("/guest_agent_exec/result_after_seal_is_dropped", Fx, NULL,
               fx_setup, test_result_after_seal_is_dropped, fx_teardown);
    g_test_add("/guest_agent_exec/invalidate_discards_inflight_result", Fx, NULL,
               fx_setup, test_invalidate_discards_inflight_result, fx_teardown);
    g_test_add("/guest_agent_exec/status_coalesces_per_vm", Fx, NULL,
               fx_setup, test_status_coalesces_per_vm, fx_teardown);
    g_test_add("/guest_agent_exec/status_response_shape", Fx, NULL,
               fx_setup, test_status_response_shape, fx_teardown);
    g_test_add("/guest_agent_exec/status_audits_with_literal_method", Fx, NULL,
               fx_setup, test_status_audits_with_literal_method, fx_teardown);
    g_test_add("/guest_agent_exec/status_refuses_while_terminating", Fx, NULL,
               fx_setup, test_status_refuses_while_terminating, fx_teardown);
    g_test_add("/guest_agent_exec/status_real_ops_test_driver", Fx, NULL,
               fx_setup, test_status_real_ops_test_driver, fx_teardown);
}
