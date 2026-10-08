






#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "src/api/ws_server.c"
#include "api/drain.h"
#include "api/uds_server.h"
#include "modules/dispatcher/rpc_utils.h"
#include "modules/core/cpu_allocator.h"
#include "modules/core/vm_state.h"
#include "modules/virt/vm_manager.h"
#include "modules/virt/vm_start_capacity.h"
#include "modules/virt/virt_conn_pool.h"
#include "modules/network/pcv_qos.h"
#include "modules/network/network_manager.h"
#include "utils/pcv_validate.h"
#include <libvirt/virterror.h>

static const gchar *mode, *state_path, *effects_path;
static sqlite3 *audit_db;
static GPtrArray *frames;
static gchar *rpc_response;
static GThread *main_thread;
static guint wake_queries;
CpuAllocator *global_allocator;

static void effect(const gchar *name)
{
    FILE *stream = fopen(effects_path, "a");
    g_assert_nonnull(stream);
    fprintf(stream, "%s\n", name);
    g_assert_cmpint(fclose(stream), ==, 0);
}

static gint read_state(void)
{
    gchar *text = NULL;
    g_assert_true(g_file_get_contents(state_path, &text, NULL, NULL));
    gint state = atoi(text);
    g_free(text);
    return state;
}

static void write_state(gint state)
{
    gchar *text = g_strdup_printf("%d\n", state);
    g_assert_true(g_file_set_contents(state_path, text, -1, NULL));
    g_free(text);
}


virConnectPtr virt_conn_pool_acquire(void) { return (virConnectPtr)1; }
void virt_conn_pool_release(virConnectPtr connection) { (void)connection; effect("RELEASE"); }
virDomainPtr pure_virt_get_domain(virConnectPtr connection, const gchar *id)
{ (void)connection; (void)id; return g_str_equal(mode, "missing") ? NULL : (virDomainPtr)2; }
const char *virDomainGetName(virDomainPtr domain) { (void)domain; return "sleep-vm"; }
int virDomainFree(virDomainPtr domain) { (void)domain; effect("FREE"); return 0; }
int virDomainIsActive(virDomainPtr domain)
{
    (void)domain;
    effect("ACTIVE");
    if (g_str_equal(mode, "active-error")) return -1;
    return read_state() != VIR_DOMAIN_SHUTOFF;
}
int virDomainGetInfo(virDomainPtr domain, virDomainInfoPtr info)
{ (void)domain; memset(info, 0, sizeof(*info)); info->state = read_state(); return 0; }
int virDomainGetState(virDomainPtr domain, int *state, int *reason, unsigned int flags)
{
    (void)domain;
    g_assert_cmpuint(flags, ==, 0);
    effect("STATE");
    if (g_str_equal(mode, "state-error") ||
        (wake_queries && g_str_equal(mode, "post-state-error"))) return -1;
    if (!wake_queries && g_str_equal(mode, "inactive-race")) write_state(VIR_DOMAIN_SHUTOFF);
    if (wake_queries && g_str_equal(mode, "delayed") && ++wake_queries == 4)
        write_state(VIR_DOMAIN_RUNNING);
    *state = read_state(); if (reason) *reason = 0;
    return 0;
}
int virDomainPMWakeup(virDomainPtr domain, unsigned int flags)
{
    (void)domain;
    g_assert_cmpuint(flags, ==, 0);
    effect("WAKE");
    if (g_str_equal(mode, "wake-error")) return -1;
    wake_queries = 1;
    if (g_str_equal(mode, "wrong-state")) write_state(VIR_DOMAIN_PAUSED);
    else if (!g_str_equal(mode, "stuck") && !g_str_equal(mode, "delayed"))
        write_state(VIR_DOMAIN_RUNNING);
    return 0;
}
virErrorPtr virGetLastError(void)
{ static virError error = {.message = "fixture libvirt failure"}; return &error; }
char *virDomainGetXMLDesc(virDomainPtr domain, unsigned int flags)
{ (void)domain; (void)flags; effect("XML"); return strdup("<domain/>"); }
virDomainPtr virDomainDefineXML(virConnectPtr connection, const char *xml)
{ (void)connection; (void)xml; effect("DEFINE"); return (virDomainPtr)2; }
int virDomainPinVcpuFlags(virDomainPtr domain, unsigned int cpu,
                        unsigned char *map, int len, unsigned int flags)
{ (void)domain; (void)cpu; (void)map; (void)len; (void)flags; effect("PIN"); return 0; }
int virDomainAttachDeviceFlags(virDomainPtr domain, const char *xml, unsigned int flags)
{ (void)domain; (void)xml; (void)flags; effect("HOTPLUG"); return 0; }
int virDomainDestroy(virDomainPtr domain) { (void)domain; effect("DESTROY"); return 0; }


static gboolean _reconcile_dpdk_vhost_for_start(virConnectPtr connection, virDomainPtr *domain,
                                                const gchar *name, gboolean active, GError **error)
{
    (void)connection; (void)domain; (void)name;
    effect(active ? "RECONCILE_ACTIVE" : "RECONCILE_COLD");
    if (g_str_equal(mode, "reconcile-error")) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "fixture reconcile failed");
        return FALSE;
    }
    return TRUE;
}
gboolean pcv_vm_start_with_capacity(virDomainPtr *domain, const gchar *root,
                                   PcvVmStartPrepare prepare, gpointer data, GError **error)
{
    (void)root; effect("CAPACITY");
    if (g_str_equal(mode, "cold-error")) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "fixture capacity failed");
        return FALSE;
    }
    if (!prepare(domain, data, error)) return FALSE;
    effect("CREATE"); write_state(VIR_DOMAIN_RUNNING);
    return TRUE;
}
static gboolean _orchestrate_tenant_overlay(gpointer context, virDomainPtr domain,
                                            const gchar *name, GError **error)
{ (void)context; (void)domain; (void)name; (void)error; effect("OVERLAY"); return TRUE; }
void pcv_security_group_sync_vm(const gchar *name) { (void)name; effect("SYNC"); }
gint pcv_bridge_mtu_read(const gchar *name, const gchar *root) { (void)name; (void)root; return 0; }
gboolean pcv_network_bridge_uplink_mode(const gchar *name, gchar **mode_out, GError **error)
{ (void)name; (void)error; *mode_out = g_strdup("shared"); return TRUE; }
gboolean pcv_vm_qos_derive_context(virDomainPtr domain, const gchar *name, gchar **tenant,
                                  gchar **iface, PcvQosSla *sla)
{ (void)domain; (void)name; (void)tenant; (void)iface; (void)sla; return FALSE; }
gboolean pcv_qos_apply_vm(const gchar *iface, const PcvQosSla *sla, GError **error)
{ (void)iface; (void)sla; (void)error; return TRUE; }
gboolean pcv_qos_ids_save(const gchar *path, GError **error) { (void)path; (void)error; return TRUE; }

gboolean lock_vm_operation(const gchar *id, gint operation, gchar **error)
{ (void)id; (void)operation; (void)error; effect("LOCK"); return TRUE; }
void unlock_vm_operation(const gchar *id) { (void)id; effect("UNLOCK"); }
void _pcv_log(GLogLevelFlags level, const gchar *domain, const gchar *fmt, ...)
{ (void)level; (void)domain; (void)fmt; }
const gchar *pcv_config_get_string(const gchar *section, const gchar *key, const gchar *fallback)
{ (void)section; (void)key; return fallback; }



void pcv_audit_log(const gchar *user, const gchar *method, const gchar *target,
                   const gchar *result, gint code, gint64 duration, const gchar *source)
{
    (void)user; (void)source;
    g_assert_true(g_thread_self() == main_thread);
    sqlite3_stmt *statement = NULL;
    g_assert_cmpint(sqlite3_prepare_v2(audit_db, "INSERT INTO audit VALUES(?,?,?,?,?)", -1,
                                      &statement, NULL), ==, SQLITE_OK);
    sqlite3_bind_text(statement, 1, method, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, target, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 3, result, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 4, code); sqlite3_bind_int64(statement, 5, duration);
    g_assert_cmpint(sqlite3_step(statement), ==, SQLITE_DONE);
    sqlite3_finalize(statement);
}
void pure_uds_server_send_response(UdsServer *server, GSocketConnection *connection, const gchar *text)
{ (void)server; (void)connection; g_free(rpc_response); rpc_response = g_strdup(text); effect("RESPONSE"); }
void __wrap_soup_websocket_connection_send_text(SoupWebsocketConnection *connection, const char *text)
{
    (void)connection;
    g_assert_true(g_thread_self() == main_thread);
    g_ptr_array_add(frames, g_strdup(text));
}
gboolean __real_cpu_allocator_allocate_exclusive(CpuAllocator *, const gchar *, guint, guint, GArray **, gint *);
gboolean __wrap_cpu_allocator_allocate_exclusive(CpuAllocator *alloc, const gchar *name,
                                                 guint node, guint count, GArray **cpus, gint *actual)
{ effect("ALLOCATE"); return __real_cpu_allocator_allocate_exclusive(alloc, name, node, count, cpus, actual); }




#define PCV_PRODUCTION_FUNCTIONS

static GIOStream *peer;
static void setup_ws(void)
{
    int descriptors[2]; g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors), ==, 0);
    GSocket *first = g_socket_new_from_fd(descriptors[0], NULL);
    GSocket *second = g_socket_new_from_fd(descriptors[1], NULL);
    GSocketConnection *stream = g_socket_connection_factory_create_connection(first);
    peer = G_IO_STREAM(g_socket_connection_factory_create_connection(second));
    GUri *uri = g_uri_parse("ws://localhost/fixture", G_URI_FLAGS_NONE, NULL);
    SoupWebsocketConnection *connection = soup_websocket_connection_new(
        G_IO_STREAM(stream), uri, SOUP_WEBSOCKET_CONNECTION_SERVER, NULL, NULL, NULL);
    g_uri_unref(uri); g_object_unref(stream); g_object_unref(first); g_object_unref(second);
    g_mutex_init(&G.mu); G.clients = g_ptr_array_new_with_free_func(g_object_unref);
    g_ptr_array_add(G.clients, connection); G.initialized = TRUE;
}

int main(int argc, char **argv)
{
    g_assert_cmpint(argc, ==, 5);
    mode = argv[1]; state_path = argv[2]; effects_path = argv[3];
    main_thread = g_thread_self();
    frames = g_ptr_array_new_with_free_func(g_free);
    g_assert_cmpint(sqlite3_open(argv[4], &audit_db), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_exec(audit_db, "CREATE TABLE audit(method,target,result,code,duration)",
                               NULL, NULL, NULL), ==, SQLITE_OK);
    global_allocator = cpu_allocator_new();
    cpu_allocator_add_core(global_allocator, 0, 0, 0, TRUE);
    if (read_state() != VIR_DOMAIN_SHUTOFF) {
        GArray *cpus = NULL; gint node;
        g_assert_true(__real_cpu_allocator_allocate_exclusive(global_allocator, "sleep-vm", 0, 1, &cpus, &node));
        g_array_unref(cpus);
    }
    pcv_drain_init(); setup_ws();
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "vm_id", "11111111-2222-3333-4444-555555555555");
    GObject *server = g_object_new(G_TYPE_OBJECT, NULL);
    GSocketConnection *connection = g_object_ref(G_SOCKET_CONNECTION(peer));
    handle_vm_start_request(params, "probe-request", (UdsServer *)server, connection);
    json_object_unref(params); g_object_unref(server); g_object_unref(connection);
    gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (!frames->len && g_get_monotonic_time() < deadline) {
        g_main_context_iteration(NULL, FALSE); g_usleep(1000);
    }
    g_assert_cmpuint(frames->len, ==, 1);
    while (g_main_context_pending(NULL)) g_main_context_iteration(NULL, FALSE);
    printf("FRAME %s\n", (gchar *)g_ptr_array_index(frames, 0));
    printf("RESPONSE %s\n", rpc_response);
    GArray *competitor = NULL; gint node;
    gboolean acquired = __real_cpu_allocator_allocate_exclusive(global_allocator, "other-vm", 0, 1, &competitor, &node);
    if (acquired) effect("COMPETITOR_ACQUIRED"); else effect("COMPETITOR_DENIED");
    if (competitor) g_array_unref(competitor);
    cpu_allocator_free(global_allocator); global_allocator = NULL;
    sqlite3_close(audit_db); g_free(rpc_response);
    pcv_drain_shutdown();
    G.initialized = FALSE; g_ptr_array_unref(G.clients); g_mutex_clear(&G.mu);
    g_object_unref(peer); g_ptr_array_unref(frames);
    return 0;
}
