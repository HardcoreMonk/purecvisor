                                                                             
                                            
                                                                         
  
                           
                                                                            
                                                                                 
                                                                                         
                                                                                         
                                                                    
                                                                         
                                                               
                                                                
                                                                    
                                                                       
                                                                                
                                                                      
                                                      
                                                       
                                                              
                                                                                    
                                                                               
                                                                               
                                                                              
                                                                 
                              
  
                                                               
                                                             
                                                            
                               
                                                                                
#include "guest_agent_exec.h"
#include "guest_ip.h"
#include "rpc_completion.h"
#include "rpc_singleflight.h"
#include "rpc_utils.h"

#include <stdlib.h>
#include <string.h>

#include <libvirt/libvirt.h>
#include <libvirt/libvirt-qemu.h>
#include <libvirt/virterror.h>

#include "api/drain.h"
#include "modules/audit/pcv_audit.h"
#include "modules/virt/virt_conn_pool.h"
#include "modules/virt/pcv_virt_open.h"
#include "utils/pcv_log.h"

#define LOG_DOM "guest_agent"

                                                 
#define GA_QGA_IFACES_CMD     "{\"execute\":\"guest-network-get-interfaces\"}"
#define GA_PROBE_TIMEOUT_S    5
#define GA_POOL_THREADS       2
                                     
#define GA_QGA_PING_CMD       "{\"execute\":\"guest-ping\"}"
#define GA_STATUS_PING_TIMEOUT_S 3
                      
#define GA_FLUSH_DEADLINE_US  ((gint64)10 * G_TIME_SPAN_SECOND)

                                                    
                                
typedef struct {
    PcvGuestAgentSlot slot;
    GPtrArray        *ifaces;                                          
    guint             gen;                             
} GaEntry;

                                                  
                                                           
                                                    
typedef struct {
    gchar     *uuid;
    gchar     *name;                                                        
    guint      gen;                     
    GPtrArray *ifaces;             
    gchar     *error;                            
    gboolean   is_status;
    gboolean   status_ok;                          
    gint       status_code;                                         
    PcvGuestAgentStatus st;                 
} GaJob;

                                                                                       
                                                                
typedef struct {
    gchar             *rpc_id;
    UdsServer         *server;               
    GSocketConnection *connection;           
    gchar             *actor;                                                       
} GaStatusWaiter;

                                                                     
static PcvSingleflight *g_ga_status_sf;

                                                  
static struct {
    GMutex       mutex;
    GHashTable  *entries;                                                
    guint        next_gen;
    GThreadPool *pool;
} g_ga;

static const PcvGuestAgentOps *g_ga_ops_test;                             
static gint g_ga_outstanding;                                                  

                                                                      
                                    
static void
_ga_conn_destroy(gpointer conn)
{
    if (conn)
        virConnectClose(conn);
}

static GPrivate g_ga_conn = G_PRIVATE_INIT(_ga_conn_destroy);

                                                                 
static GQuark
_ga_error_quark(void)
{
    return g_quark_from_static_string("pcv-guest-agent-exec");
}

                                                                         
                                                        
static void
_ga_set_virt_error(GError **error)
{
    gchar *msg = g_utf8_make_valid(virGetLastErrorMessage() ?: "unknown error", -1);
    g_set_error_literal(error, _ga_error_quark(), 1, msg);
    g_free(msg);
}

                                                                             
                                                                               
                                                
static void
_ga_conn_drop_if_dead(void)
{
    virConnectPtr conn = g_private_get(&g_ga_conn);
    if (conn && virConnectIsAlive(conn) != 1) {
        g_private_set(&g_ga_conn, NULL);
        virConnectClose(conn);
    }
}

                                                                        
                                                                         
                                                                           
                                                                 
static virConnectPtr
_ga_conn_get(GError **error)
{
    _ga_conn_drop_if_dead();
    virConnectPtr conn = g_private_get(&g_ga_conn);
    if (conn)
        return conn;
    GError *open_err = NULL;
    conn = pcv_virt_open_bounded(virt_conn_pool_uri(), FALSE, PCV_VIRT_OPEN_TIMEOUT_SEC, &open_err);                     
    if (!conn) {
        g_set_error_literal(error, _ga_error_quark(), 1, open_err->message);
        g_error_free(open_err);
        return NULL;
    }
    g_private_set(&g_ga_conn, conn);
    return conn;
}

                                                                                                              
                                                            
                                                                                 
                                                     
                                                                                
                                                                    
static gchar *
_ga_real_agent_command(const gchar *uuid, const gchar *name, const gchar *cmd,
                       gint timeout_s, GError **error)
{
    (void)name;
    virConnectPtr conn = _ga_conn_get(error);
    if (!conn)
        return NULL;
    virDomainPtr dom = virDomainLookupByUUIDString(conn, uuid);
    if (!dom) {
        _ga_set_virt_error(error);
        _ga_conn_drop_if_dead();
        return NULL;
    }
    char *raw = virDomainQemuAgentCommand(dom, cmd, timeout_s, 0);
    if (!raw)
        _ga_set_virt_error(error);
    virDomainFree(dom);
    if (!raw) {
        _ga_conn_drop_if_dead();
        return NULL;
    }
    gchar *out = g_strdup(raw);
    free(raw);
    return out;
}

                                                                          
                                                                              
                                                                                               
                                                                        
static gboolean
_ga_real_status_probe(const gchar *name, PcvGuestAgentStatus *out, GError **error)
{
    GError *conn_err = NULL;
    virConnectPtr conn = _ga_conn_get(&conn_err);
    if (!conn) {
        PCV_LOG_WARN(LOG_DOM, "guest agent status: libvirt connection failed (%s)", conn_err->message);
        g_clear_error(&conn_err);
        g_set_error_literal(error, _ga_error_quark(), PURE_RPC_ERR_ZFS_OPERATION,
                            "Failed to connect to Libvirt.");
        return FALSE;
    }
    virDomainPtr dom = virDomainLookupByUUIDString(conn, name);
    if (!dom) {
        virResetLastError();
        dom = virDomainLookupByName(conn, name);
    }
    if (!dom) {
        virResetLastError();
                                                                         
        gboolean conn_dead = virConnectIsAlive(conn) != 1;
        _ga_conn_drop_if_dead();
        if (conn_dead)
            g_set_error_literal(error, _ga_error_quark(), PURE_RPC_ERR_ZFS_OPERATION,
                                "Failed to connect to Libvirt.");
        else
            g_set_error_literal(error, _ga_error_quark(), PURE_RPC_ERR_VM_NOT_FOUND, "VM not found.");
        return FALSE;
    }
    out->running = virDomainIsActive(dom) == 1;
                                                               
    char *config_xml = virDomainGetXMLDesc(dom, VIR_DOMAIN_XML_INACTIVE);
    char *live_xml = virDomainGetXMLDesc(dom, 0);
    out->channel_configured = pcv_guest_agent_xml_has_channel(config_xml);
    out->channel_live = pcv_guest_agent_xml_has_channel(live_xml);
    free(config_xml);
    free(live_xml);
    if (out->running && out->channel_live) {                                     
        char *result = virDomainQemuAgentCommand(dom, GA_QGA_PING_CMD, GA_STATUS_PING_TIMEOUT_S, 0);
        if (result) {
            out->agent_ping = TRUE;
            free(result);
        } else {
            const char *vir_err = virGetLastErrorMessage();
            out->agent_error = g_utf8_make_valid(vir_err ? vir_err : "unknown error", -1);
            virResetLastError();
        }
    }
    virDomainFree(dom);
    if (out->agent_error)
        _ga_conn_drop_if_dead();
    return TRUE;
}

static const PcvGuestAgentOps k_ga_real_ops = {
    .agent_command = _ga_real_agent_command,
    .now_us        = g_get_monotonic_time,
    .status_probe  = _ga_real_status_probe,
};

                                                                          
static const PcvGuestAgentOps *
_ga_ops(void)
{
    const PcvGuestAgentOps *ops = g_atomic_pointer_get(&g_ga_ops_test);
    return ops ? ops : &k_ga_real_ops;
}

                                                                                  
static GPtrArray *
_ga_str_array_copy(const GPtrArray *src)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; src && i < src->len; i++)
        g_ptr_array_add(out, g_strdup(g_ptr_array_index((GPtrArray *)src, i)));
    return out;
}

                                                                     
                                                         
static GPtrArray *
_ga_ifaces_copy(const GPtrArray *src)
{
    GPtrArray *out = g_ptr_array_new_full(src->len, pcv_guest_iface_free);
    for (guint i = 0; i < src->len; i++) {
        const PcvGuestIface *f = g_ptr_array_index((GPtrArray *)src, i);
        PcvGuestIface *c = g_new0(PcvGuestIface, 1);
        c->mac  = g_strdup(f->mac);
        c->ipv4 = _ga_str_array_copy(f->ipv4);
        c->ipv6 = _ga_str_array_copy(f->ipv6);
        g_ptr_array_add(out, c);
    }
    return out;
}

                                                                 
static void
_ga_entry_free(gpointer data)
{
    GaEntry *e = data;
    if (e->ifaces)
        g_ptr_array_unref(e->ifaces);
    g_free(e);
}

                                                                            
                                               
static GaEntry *
_ga_entry_get_or_create_locked(const gchar *uuid)
{
    if (!g_ga.entries)
        g_ga.entries = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, _ga_entry_free);
    GaEntry *e = g_hash_table_lookup(g_ga.entries, uuid);
    if (!e) {
        e = g_new0(GaEntry, 1);
        if (++g_ga.next_gen == 0)                           
            ++g_ga.next_gen;
        e->gen = g_ga.next_gen;
        g_hash_table_insert(g_ga.entries, g_strdup(uuid), e);
    }
    return e;
}

                                                                    
                                               
static void
_ga_job_free(gpointer data)
{
    GaJob *job = data;
    g_free(job->uuid);
    g_free(job->name);
    g_free(job->error);
    g_free(job->st.agent_error);
    if (job->ifaces)
        g_ptr_array_unref(job->ifaces);
    g_free(job);
    (void)g_atomic_int_dec_and_test(&g_ga_outstanding);
}

                                                              
                                                                      
                                                              
static gboolean
_ga_deliver(gpointer data)
{
    GaJob *job = data;
    gint64 now = _ga_ops()->now_us();
    gboolean warn = FALSE, info = FALSE;
    gint backoff_s = 0;

    g_mutex_lock(&g_ga.mutex);
    GaEntry *e = g_ga.entries ? g_hash_table_lookup(g_ga.entries, job->uuid) : NULL;
    if (e && e->gen == job->gen) {
        PcvGuestAgentState prev = e->slot.state;
        if (job->ifaces) {
            pcv_guest_agent_slot_on_success(&e->slot, now);
            if (e->ifaces)
                g_ptr_array_unref(e->ifaces);
            e->ifaces = g_steal_pointer(&job->ifaces);
            info = prev == PCV_GUEST_AGENT_UNRESPONSIVE;
        } else {
            backoff_s = pcv_guest_agent_slot_on_failure(&e->slot, now);
            g_clear_pointer(&e->ifaces, g_ptr_array_unref);
            warn = prev != PCV_GUEST_AGENT_UNRESPONSIVE;
        }
    }
    g_mutex_unlock(&g_ga.mutex);

    if (warn)
        PCV_LOG_WARN(LOG_DOM,
                     "guest agent of VM '%s' did not answer guest-network-get-interfaces (%s); "
                     "next try in %d s", job->name, job->error ? job->error : "unknown error",
                     backoff_s);
    if (info)
        PCV_LOG_INFO(LOG_DOM, "guest agent of VM '%s' answered again", job->name);
    return G_SOURCE_REMOVE;
}

                                                                                 
static void
_ga_status_waiter_free(gpointer data)
{
    GaStatusWaiter *w = data;
    g_free(w->rpc_id);
    g_clear_object(&w->server);
    g_clear_object(&w->connection);
    g_free(w->actor);
    g_free(w);
}

                                                                           
                                                                                              
                                                                   
static JsonNode *
_ga_status_result_node(const gchar *name, const PcvGuestAgentStatus *st)
{
    const gchar *status = "channel_missing";                                
    const gchar *message = "Guest agent channel is not configured.";
    if (st->agent_ping) {                                                       
        status = "ok";
        message = "Guest agent is responding.";
    } else if (!st->running && (st->channel_configured || st->channel_live)) {                          
        status = "vm_stopped";
        message = "Guest agent channel is configured; start the VM to verify the agent.";
    } else if (st->running && st->channel_configured && !st->channel_live) {                            
        status = "reboot_required";
        message = "Guest agent channel is configured for the next boot; restart the VM or attach it live.";
    } else if (st->running && st->channel_live) {                                          
        status = "agent_unavailable";
        message = "Guest agent channel exists, but qemu-guest-agent is not responding in the guest.";
    }

    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "name", name);
    json_object_set_string_member(obj, "status", status);
    json_object_set_string_member(obj, "message", message);
    json_object_set_boolean_member(obj, "running", st->running);
    json_object_set_boolean_member(obj, "channel_present", st->channel_configured || st->channel_live);
    json_object_set_boolean_member(obj, "channel_configured", st->channel_configured);
    json_object_set_boolean_member(obj, "channel_live", st->channel_live);
    json_object_set_boolean_member(obj, "agent_ping", st->agent_ping);
    json_object_set_boolean_member(obj, "reboot_required",
                                   st->running && st->channel_configured && !st->channel_live);
    json_object_set_boolean_member(obj, "package_required", st->running && st->channel_live && !st->agent_ping);
    json_object_set_boolean_member(obj, "can_ensure_channel", TRUE);
    if (st->agent_error)
        json_object_set_string_member(obj, "agent_error", st->agent_error);
    pcv_guest_agent_add_install_commands(obj);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, obj);
    return node;
}

                                                                      
                                                                  
                                                           
                                                                   
static gboolean
_ga_status_deliver(gpointer data)
{
    GaJob *job = data;
    GPtrArray *waiters = g_ga_status_sf ? pcv_singleflight_complete(g_ga_status_sf, job->name) : NULL;
    for (guint i = 0; waiters && i < waiters->len; i++) {
        GaStatusWaiter *w = g_ptr_array_index(waiters, i);
        pcv_audit_log(w->actor, "vm.guest.agent.status", job->name, job->status_ok ? "ok" : "fail",
                      job->status_ok ? 0 : job->status_code, 0, "local");
        gchar *resp = job->status_ok
            ? pure_rpc_build_success_response(w->rpc_id, _ga_status_result_node(job->name, &job->st))
            : pure_rpc_build_error_response(w->rpc_id, (PureRpcErrorCode)job->status_code, job->error);
        pure_uds_server_send_response(w->server, w->connection, resp);
        g_free(resp);
    }
    if (waiters)
        g_ptr_array_unref(waiters);
    return G_SOURCE_REMOVE;
}

                                                                                
                                                                           
                                                      
                                                                 
static void
_ga_exec_status(GaJob *job)
{
    GError *err = NULL;
    if (pcv_drain_is_terminating())
        g_set_error_literal(&err, _ga_error_quark(), PURE_RPC_ERR_INTERNAL_ERROR, "daemon is shutting down");
    else
        job->status_ok = _ga_ops()->status_probe(job->name, &job->st, &err);
    if (!job->status_ok) {
        job->status_code = err ? err->code : PURE_RPC_ERR_INTERNAL_ERROR;
        job->error = g_strdup(err && err->message ? err->message : "unknown error");
    }
    g_clear_error(&err);
    if (!pcv_drain_try_invoke(NULL, G_PRIORITY_DEFAULT, _ga_status_deliver, job, _ga_job_free))
        _ga_job_free(job);                                                      
}

                                                                              
                                                             
                                              
                                                          
static void
_ga_exec_job(gpointer data, gpointer unused)
{
    GaJob *job = data;
    (void)unused;
    if (job->is_status) {
        _ga_exec_status(job);
        return;
    }
    if (pcv_drain_is_terminating()) {
        _ga_job_free(job);
        return;
    }
    const PcvGuestAgentOps *ops = _ga_ops();
    GError *err = NULL;
    gchar *json = ops->agent_command(job->uuid, job->name, GA_QGA_IFACES_CMD,
                                     GA_PROBE_TIMEOUT_S, &err);
    if (json) {
        job->ifaces = pcv_guest_ip_parse_qga_interfaces(json, &err);
        g_free(json);
    }
    if (!job->ifaces)
        job->error = g_strdup(err && err->message ? err->message : "unknown error");
    g_clear_error(&err);
    if (!pcv_drain_try_invoke(NULL, G_PRIORITY_DEFAULT, _ga_deliver, job, _ga_job_free))
        _ga_job_free(job);                                    
}

                                                                          
                                                               
                                                      
static GThreadPool *
_ga_pool_locked(void)
{
    if (g_ga.pool)
        return g_ga.pool;
    GError *err = NULL;
    GThreadPool *pool = g_thread_pool_new(_ga_exec_job, NULL, GA_POOL_THREADS, TRUE, &err);
    if (err) {
        PCV_LOG_WARN(LOG_DOM, "guest agent executor threads could not start (%s)", err->message);
        g_clear_error(&err);
    }
    g_ga.pool = pool;
    return pool;
}

                                                                             
                                        
                                                            
void
pcv_guest_agent_set_ops_for_test(const PcvGuestAgentOps *ops)
{
    g_atomic_pointer_set(&g_ga_ops_test, ops);
}

                                                                                       
                                                                              
                                                               
GPtrArray *
pcv_guest_agent_cache_lookup(const gchar *uuid)
{
    if (!uuid || !*uuid)
        return NULL;
    gint64 now = _ga_ops()->now_us();
    GPtrArray *copy = NULL;
    g_mutex_lock(&g_ga.mutex);
    GaEntry *e = g_ga.entries ? g_hash_table_lookup(g_ga.entries, uuid) : NULL;
    if (e && e->ifaces && pcv_guest_agent_slot_fresh(&e->slot, now))
        copy = _ga_ifaces_copy(e->ifaces);
    g_mutex_unlock(&g_ga.mutex);
    return copy;
}

                                                                                      
                                                   
                                                                                  
                                                                   
void
pcv_guest_agent_probe_request(const gchar *uuid, const gchar *name)
{
    if (!uuid || !*uuid)
        return;
    if (pcv_drain_is_terminating())
        return;
    gint64 now = _ga_ops()->now_us();
    gboolean push_failed = FALSE;
    gchar *push_error = NULL;

    g_mutex_lock(&g_ga.mutex);
    GaEntry *e = _ga_entry_get_or_create_locked(uuid);
    GThreadPool *pool = NULL;
    if (pcv_guest_agent_slot_should_probe(&e->slot, now))
        pool = _ga_pool_locked();
    if (pool) {
        GaJob *job = g_new0(GaJob, 1);
        job->uuid = g_strdup(uuid);
        job->name = g_strdup(name && *name ? name : uuid);
        job->gen  = e->gen;
        e->slot.inflight = TRUE;
        g_atomic_int_inc(&g_ga_outstanding);
        GError *err = NULL;
        if (!g_thread_pool_push(pool, job, &err)) {
            push_failed = TRUE;
            push_error = g_strdup(err ? err->message : "unknown error");
        }
        g_clear_error(&err);
    }
    g_mutex_unlock(&g_ga.mutex);

    if (push_failed)
        PCV_LOG_WARN(LOG_DOM, "guest agent executor could not start a thread (%s); probe stays queued",
                     push_error);
    g_free(push_error);
}

                                                                            
                                                   
void
pcv_guest_agent_invalidate(const gchar *uuid)
{
    if (!uuid || !*uuid)
        return;
    g_mutex_lock(&g_ga.mutex);
    if (g_ga.entries)
        g_hash_table_remove(g_ga.entries, uuid);
    g_mutex_unlock(&g_ga.mutex);
}

                                                                 
                                                                                      
                                                                         
                                                                                              
                                                                          
void
pcv_guest_agent_status_submit(const gchar *name, const gchar *rpc_id,
                              UdsServer *server, GSocketConnection *connection)
{
    g_return_if_fail(name != NULL);
    if (pcv_drain_is_terminating()) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INTERNAL_ERROR, "daemon is shutting down");
        pure_uds_server_send_response(server, connection, e);
        g_free(e);
        return;
    }
    GaStatusWaiter *w = g_new0(GaStatusWaiter, 1);
    const gchar *actor = pcv_rpc_completion_actor();
    w->rpc_id = g_strdup(rpc_id);
    w->server = g_object_ref(server);
    w->connection = g_object_ref(connection);
    w->actor = g_strdup(actor ? actor : PCV_AUDIT_ACTOR_LOCAL);
    if (!g_ga_status_sf)
        g_ga_status_sf = pcv_singleflight_new(_ga_status_waiter_free);
    if (!pcv_singleflight_join(g_ga_status_sf, name, w))
        return;                                     

    GaJob *job = g_new0(GaJob, 1);
    job->is_status = TRUE;
    job->name = g_strdup(name);
    g_atomic_int_inc(&g_ga_outstanding);
    gboolean push_failed = FALSE;
    gchar *push_error = NULL;
    g_mutex_lock(&g_ga.mutex);
    GThreadPool *pool = _ga_pool_locked();
    if (pool) {
        GError *err = NULL;
        if (!g_thread_pool_push(pool, job, &err)) {
            push_failed = TRUE;
            push_error = g_strdup(err ? err->message : "unknown error");
        }
        g_clear_error(&err);
    }
    g_mutex_unlock(&g_ga.mutex);

    if (!pool) {                                                  
        job->status_code = PURE_RPC_ERR_INTERNAL_ERROR;
        job->error = g_strdup("guest agent executor unavailable");
        _ga_status_deliver(job);
        _ga_job_free(job);
    }
    if (push_failed)
        PCV_LOG_WARN(LOG_DOM, "guest agent executor could not start a thread (%s); status check stays queued",
                     push_error);
    g_free(push_error);
}

                                                                                   
                                                                                       
                                                            
gboolean
pcv_guest_agent_xml_has_channel(const gchar *xml)
{
    return xml && strstr(xml, "org.qemu.guest_agent.0") != NULL;
}

                                                                                  
                                                                                        
                                                          
void
pcv_guest_agent_add_install_commands(JsonObject *obj)
{
    JsonObject *cmds = json_object_new();
    json_object_set_string_member(cmds, "debian_ubuntu",
        "sudo apt update && sudo apt install -y qemu-guest-agent && sudo systemctl enable --now qemu-guest-agent");
    json_object_set_string_member(cmds, "rhel_rocky_fedora",
        "sudo dnf install -y qemu-guest-agent && sudo systemctl enable --now qemu-guest-agent");
    json_object_set_string_member(cmds, "suse",
        "sudo zypper install -y qemu-guest-agent && sudo systemctl enable --now qemu-guest-agent");
    json_object_set_object_member(obj, "install_commands", cmds);
}

                                                                            
                                                           
                                                            
void
pcv_guest_agent_flush_for_test(void)
{
    gint64 deadline = g_get_monotonic_time() + GA_FLUSH_DEADLINE_US;
    while (g_atomic_int_get(&g_ga_outstanding) > 0) {
        if (g_get_monotonic_time() > deadline) {
            PCV_LOG_ERROR(LOG_DOM, "flush_for_test: %d probe(s) still outstanding after 10 s",
                          g_atomic_int_get(&g_ga_outstanding));
            return;
        }
        if (!g_main_context_iteration(NULL, FALSE))
            g_usleep(1000);
    }
}
