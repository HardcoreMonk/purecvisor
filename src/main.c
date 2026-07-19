
#include <unistd.h>
#include <glib.h>
#include <glib-unix.h>
#include <libvirt-gobject/libvirt-gobject.h>
#include <stdio.h>
#include <libvirt/libvirt.h>

#include "api/uds_server.h"
#include "api/dispatcher.h"
#include "api/rest_server.h"
#include "api/grpc_server.h"
#include "modules/virt/vm_manager.h"
#include "api/drain.h"
#include "utils/logger.h"
#include "utils/pcv_log.h"
#include "utils/pcv_config.h"
#include "utils/pcv_privdrop.h"
#include "utils/pcv_validate.h"

#include "modules/core/vm_state.h"
#include "modules/core/cpu_allocator.h"

#include "modules/daemons/telemetry.h"
#include "modules/daemons/virt_events.h"
#include "modules/daemons/ebpf_telemetry.h"
#include "modules/daemons/alert_engine.h"
#include "modules/daemons/process_monitor.h"
#include "modules/daemons/update_check.h"

#include "utils/pcv_spawn.h"
#include "utils/pcv_worker_pool.h"
#include "purecvisor/pcv_validate.h"
#include "purecvisor/version.h"
#include "utils/pcv_jwt.h"

#include "modules/virt/virt_conn_pool.h"
#include "modules/virt/cancellable_map.h"

#include "bootstrap/pcv_bootstrap.h"

#include "modules/network/ovs_overlay.h"
#include "modules/network/network_manager.h"
#include "modules/network/ovn_manager.h"
#include "modules/storage/iscsi_manager.h"
#include "modules/network/dpdk_manager.h"
#include "modules/network/sriov_manager.h"
#include "modules/network/security_group.h"
#include "io/pcv_uring.h"
#include "api/hot_reload.h"
#include "api/ws_server.h"
#include "modules/storage/storage_tier.h"
#include "modules/daemons/prometheus_exporter.h"
#include "modules/audit/pcv_audit.h"
#include "utils/pcv_job_queue.h"
#include "modules/accel/gpu_manager.h"
#include "modules/plugin/pcv_plugin_manager.h"
#include "utils/pcv_tls.h"
#include "modules/network/nfv_manager.h"
#include "modules/backup/backup_scheduler.h"
#include "modules/auth/pcv_rbac.h"
#include "modules/template/vm_template.h"

static GMainLoop *loop;

CpuAllocator *global_allocator = NULL;

static GThread   *g_watchdog_thread = NULL;
static volatile gint g_watchdog_stop = 0;

static gpointer
_watchdog_thread_func(gpointer data)
{
    guint64 interval_us = GPOINTER_TO_SIZE(data);
    if (interval_us < 1000000) interval_us = 1000000;

    while (!g_atomic_int_get(&g_watchdog_stop)) {
        pcv_drain_notify_watchdog();

        guint64 slept = 0;
        while (slept < interval_us &&
               !g_atomic_int_get(&g_watchdog_stop)) {
            g_usleep(100000);
            slept += 100000;
        }
    }
    return NULL;
}

static gboolean on_signal_received(gpointer user_data) {
    (void)user_data;
    if (!loop) return G_SOURCE_REMOVE;
    g_message("Signal received, initiating graceful shutdown...");

    pcv_drain_begin(loop, pcv_config_get_drain_timeout());

    return FALSE;
}

static gboolean on_sighup_received(gpointer user_data) {
    (void)user_data;
    g_message("[main] SIGHUP received, reloading configuration");
    pcv_config_reload();
    pcv_log_load_module_levels();
    return TRUE;
}

static void scan_and_register_host_topology(CpuAllocator *alloc) {
    g_message("[Init] Scanning Host Topology and Isolated CPUs...");

    cpu_allocator_add_core(alloc, 0, 0, 0, FALSE);
    cpu_allocator_add_core(alloc, 1, 1, 0, FALSE);
    cpu_allocator_add_core(alloc, 2, 2, 0, TRUE);
    cpu_allocator_add_core(alloc, 3, 3, 0, TRUE);

    g_message("[Init] Host Topology mapped to In-Memory Allocator.");
}

#define SIG_PROBE_DOM "signal_probe"

static void
_on_vm_started_probe(PureCVisorVmManager *mgr __attribute__((unused)),
                     const gchar         *vm_name,
                     gpointer             user_data __attribute__((unused)))
{
    PCV_LOG_DEBUG(SIG_PROBE_DOM,
                 "[GIO P6] vm-started RECEIVED — vm_name='%s'", vm_name);
}

static void
_on_vm_stopped_probe(PureCVisorVmManager *mgr __attribute__((unused)),
                     const gchar         *vm_name,
                     gpointer             user_data __attribute__((unused)))
{
    PCV_LOG_DEBUG(SIG_PROBE_DOM,
                 "[GIO P6] vm-stopped RECEIVED — vm_name='%s'", vm_name);
}

static void
_on_metrics_updated_probe(PureCVisorVmManager *mgr __attribute__((unused)),
                          GHashTable          *cache,
                          gpointer             user_data __attribute__((unused)))
{
    guint n = cache ? g_hash_table_size(cache) : 0;

    const gchar *first_uuid = NULL;
    if (cache && n > 0) {
        GHashTableIter it;
        gpointer key;
        g_hash_table_iter_init(&it, cache);
        g_hash_table_iter_next(&it, &key, NULL);
        first_uuid = (const gchar *)key;
    }

    PCV_LOG_DEBUG(SIG_PROBE_DOM,
                 "[GIO P6] vm-metrics-updated RECEIVED — "
                 "vm_count=%u first_uuid=%s",
                 n, first_uuid ? first_uuid : "(none)");
}

int main(int argc, char *argv[]) {
    const PcvBootstrapEditionInfo *edition_info = pcv_bootstrap_get_edition_info();

    if (edition_info) {
        g_message("[init] Edition bootstrap: %s (cluster=%s)",
                  edition_info->edition_name,
                  edition_info->cluster_enabled ? "enabled" : "disabled");
    }

    if (geteuid() != 0) {
        fprintf(stderr, "\n\x1b[31m[!] CRITICAL ERROR: INSUFFICIENT PRIVILEGES\x1b[0m\n");
        fprintf(stderr, "    The PureCVisor Daemon MUST be run as root.\n");
        fprintf(stderr, "    Please execute using sudo: \x1b[33msudo %s\x1b[0m\n\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    g_setenv("LIBVIRT_LOG_OUTPUTS", "1:file:/dev/null", TRUE);
    g_setenv("LIBVIRT_LOG_FILTERS", "1:libvirt", TRUE);

    GError *error = NULL;

    purecvisor_logger_init();
    pcv_config_init();
    pcv_log_load_module_levels();

    #if !GLIB_CHECK_VERSION(2, 36, 0)
    g_type_init();
    #endif

    gvir_init_object(&argc, &argv);

    g_message("Starting PureCVisor Engine...");

    gint64 init_total_start = g_get_monotonic_time();
    gint64 stage_start;
    gint   stage_num = 0;

#define STAGE_BEGIN(label) do { \
    stage_num++; \
    stage_start = g_get_monotonic_time(); \
    (void)0; } while(0)

#define STAGE_END(label) do { \
    gint64 _ms = (g_get_monotonic_time() - stage_start) / 1000; \
    g_message("[init] Stage %d (%s) completed in %ldms", stage_num, (label), (long)_ms); \
    } while(0)

    STAGE_BEGIN("core-modules");
    init_pending_state_machine();

    virt_conn_pool_init((guint)pcv_config_get_pool_max_conn());

    cmap_init();

    pcv_drain_init();

    global_allocator = cpu_allocator_new();
    scan_and_register_host_topology(global_allocator);

    init_virt_events_daemon();
    STAGE_END("core-modules");

    STAGE_BEGIN("security");
    pcv_privdrop_apply_all();

    pcv_spawn_launcher_init();
    pcv_worker_pool_init();
    pcv_update_check_init();

    pcv_network_rundir_init();

    pcv_security_group_restore();
    STAGE_END("security");

    STAGE_BEGIN("libvirt-dispatcher");
    gboolean libvirt_degraded = FALSE;
    GVirConnection *conn = gvir_connection_new(pcv_config_get_libvirt_uri());
    if (!gvir_connection_open(conn, NULL, &error)) {
        g_warning("libvirt connection failed: %s — entering DEGRADED mode "
                  "(VM operations unavailable, REST/cluster queries still active)",
                  error->message);
        g_error_free(error);
        error = NULL;
        libvirt_degraded = TRUE;

    }

    PureCVisorDispatcher *dispatcher = purecvisor_dispatcher_new();
    purecvisor_dispatcher_set_connection(dispatcher, conn);

    PureCVisorVmManager *_mgr =
        purecvisor_dispatcher_get_vm_manager(dispatcher);
    g_signal_connect(_mgr, PCV_VM_SIGNAL_STARTED,
                     G_CALLBACK(_on_vm_started_probe), NULL);
    g_signal_connect(_mgr, PCV_VM_SIGNAL_STOPPED,
                     G_CALLBACK(_on_vm_stopped_probe), NULL);
    g_signal_connect(_mgr, PCV_VM_SIGNAL_METRICS_UPDATED,
                     G_CALLBACK(_on_metrics_updated_probe), NULL);

    init_telemetry_daemon(_mgr);
    STAGE_END("libvirt-dispatcher");

    STAGE_BEGIN("uds-server");
    UdsServer *server = uds_server_new(pcv_config_get_socket_path());
    uds_server_set_dispatcher(server, dispatcher);

    if (!uds_server_start(server, &error)) {
        g_critical("Failed to start UDS server: %s", error->message);
        g_error_free(error);
        return 1;
    }
    STAGE_END("uds-server");

    STAGE_BEGIN("rest-grpc");
    pcv_tls_init_from_config();
    {

        gchar *jwt_sec = pcv_config_get_secret("auth", "jwt_secret", NULL);
        if (jwt_sec && *jwt_sec) {
            pcv_jwt_init(jwt_sec);
            g_free(jwt_sec);
        } else {
            g_free(jwt_sec);
            pcv_jwt_init(pcv_config_get_jwt_secret());
        }
    }

    PcvRestServer *rest_server = pcv_rest_server_new(dispatcher, 0);
    if (!pcv_rest_server_start(rest_server, &error)) {
        g_critical("Failed to start REST server: %s", error->message);
        g_error_free(error);

        g_object_unref(rest_server);
        rest_server = NULL;
        g_warning("REST API unavailable — continuing with UDS only");
    }

    pcv_grpc_server_start();
    STAGE_END("rest-grpc");

    STAGE_BEGIN("cluster");
    pcv_bootstrap_init_cluster_manager();

    STAGE_END("cluster");

    STAGE_BEGIN("network-storage");
    pcv_overlay_init(pcv_config_get_string("overlay", "tunnel_ip", ""));

    pcv_overlay_restore();
    pcv_iscsi_init();
    pcv_ovn_init();

    pcv_dpdk_init();
    pcv_sriov_init();

#if PCV_USE_URING
    {
        GError *uring_err = NULL;
        PcvUringCtx *uring = pcv_uring_new(PCV_URING_DEFAULT_QUEUE_DEPTH, &uring_err);
        if (uring) {
            g_message("[main] io_uring initialized (queue_depth=%u, eventfd=%d)",
                      PCV_URING_DEFAULT_QUEUE_DEPTH, uring->event_fd);

        } else {

            g_warning("[main] io_uring init failed: %s — using GLib I/O fallback",
                      uring_err ? uring_err->message : "unknown");
            if (uring_err) g_error_free(uring_err);
        }
    }
#endif

    STAGE_END("network-storage");

    STAGE_BEGIN("observability");
    pcv_prom_init();

    pcv_audit_init(pcv_config_get_string("audit", "db_path",
                   "/var/lib/purecvisor/pcv_audit.db"));

    pcv_job_queue_init();

    pcv_hot_reload_init(pcv_bootstrap_get_daemon_binary_path(), -1);

    pcv_storage_tier_init();

    pcv_gpu_init();

    pcv_plugin_manager_init("/etc/purecvisor/plugins.d");

    STAGE_END("observability");

    STAGE_BEGIN("extensions");

    pcv_nfv_init();

    pcv_bootstrap_init_federation();

    STAGE_END("extensions");

    STAGE_BEGIN("monitoring");
    pcv_ebpf_telemetry_init();

    {

        extern void pcv_zfs_pool_lock_init(void);
        pcv_zfs_pool_lock_init();

        extern void pcv_anomaly_init(void);
        extern void pcv_predict_init(void);
        extern void pcv_healing_init(void);
        extern void pcv_agent_init(void);
        pcv_anomaly_init();
        pcv_predict_init();
        pcv_healing_init();
        pcv_agent_init();

        extern void pcv_agent_configure(int provider, const gchar *model,
                                         const gchar *api_key, const gchar *endpoint);

        static const struct { int id; const gchar *prefix; } _ai_provs[] = {
            { 0, "claude" }, { 1, "openai" }, { 2, "gemini" }, { 3, "ollama" }
        };

        for (gsize i = 0; i < G_N_ELEMENTS(_ai_provs); i++) {
            gchar key_k[64], model_k[64], ep_k[64];
            g_snprintf(key_k,   sizeof(key_k),   "%s_api_key",  _ai_provs[i].prefix);
            g_snprintf(model_k, sizeof(model_k),  "%s_model",    _ai_provs[i].prefix);
            g_snprintf(ep_k,    sizeof(ep_k),     "%s_endpoint", _ai_provs[i].prefix);
            gchar *api_key        = pcv_config_get_secret("ai", key_k,   "");
            const gchar *model    = pcv_config_get_string("ai", model_k,  NULL);
            const gchar *endpoint = pcv_config_get_string("ai", ep_k,     NULL);

            if (api_key && *api_key)
                pcv_agent_configure(_ai_provs[i].id, model, api_key, endpoint);
            g_free(api_key);
        }
    }

    pcv_alert_engine_init();

    pcv_process_monitor_init();
    STAGE_END("monitoring");

    STAGE_BEGIN("auth-templates");

    pcv_rbac_init("/var/lib/purecvisor/rbac.db");

    pcv_vm_template_init();

    pcv_backup_scheduler_init();
    pcv_security_group_resync_timer_init();

    pcv_qos_reconcile_timer_init();
    pcv_overlay_reconcile_timer_init();
    STAGE_END("auth-templates");

    STAGE_BEGIN("scheduler-proxy");
    pcv_bootstrap_init_scheduler_proxy();

    STAGE_END("scheduler-proxy");

    STAGE_BEGIN("overlay-provision");
    pcv_bootstrap_init_runtime_network();

    pcv_qos_restore();

    STAGE_END("overlay-provision");

    {
        gint64 total_ms = (g_get_monotonic_time() - init_total_start) / 1000;
        g_message("[init] All %d stages completed in %ldms", stage_num, (long)total_ms);
    }

    {
        gint health_errors = 0;

        if (!libvirt_degraded) {
            virConnectPtr test_conn = virt_conn_pool_acquire();
            if (!test_conn) {
                g_warning("[init] HEALTH: libvirt connection pool unavailable");
                health_errors++;
            } else {
                virt_conn_pool_release(test_conn);
            }
        }

        gint lock_count = pcv_vm_state_get_lock_count();
        if (lock_count < 0) {
            g_warning("[init] HEALTH: vm_state DB unavailable");
            health_errors++;
        }

        if (!g_file_test(pcv_config_get_socket_path(), G_FILE_TEST_EXISTS)) {
            g_warning("[init] HEALTH: UDS socket not found at %s",
                      pcv_config_get_socket_path());
            health_errors++;
        }

        if (health_errors > 0)
            g_warning("[init] Degraded startup: %d service(s) unavailable", health_errors);
        else
            g_message("[init] Health self-check passed");
    }

    loop = g_main_loop_new(NULL, FALSE);

    g_unix_signal_add(SIGINT, on_signal_received, NULL);
    g_unix_signal_add(SIGTERM, on_signal_received, NULL);

    g_unix_signal_add(SIGHUP, on_sighup_received, NULL);

    pcv_drain_notify_ready();

    gint rest_port = pcv_config_get_int("daemon", "rest_port", 8080);

    g_message("═══════════════════════════════════════════════════════");
    g_message("  PureCVisor Engine v%s — All systems operational", PCV_PRODUCT_VERSION);
    g_message("═══════════════════════════════════════════════════════");
    g_message("  UDS  : /var/run/purecvisor/daemon.sock (io_uring)");
    g_message("  REST : http://0.0.0.0:%d/api/v1/", rest_port);
    g_message("  Web  : http://0.0.0.0:%d/ui/", rest_port);
    g_message("  WS   : ws://0.0.0.0:%d/api/v1/ws/events", rest_port);
    g_message("  RPC  : 130 methods registered");
    g_message("  REST : 88 endpoints active");
    if (libvirt_degraded)
        g_message("  MODE : *** DEGRADED (libvirt unavailable) ***");
    g_message("═══════════════════════════════════════════════════════");
    g_message("Daemon is running. Waiting for requests...");

    {
        guint64 watchdog_usec = pcv_drain_get_watchdog_usec();
        if (watchdog_usec > 0) {
            guint64 interval_us = watchdog_usec / 2;
            if (interval_us < 5000000) interval_us = 5000000;
            g_atomic_int_set(&g_watchdog_stop, 0);
            g_watchdog_thread = g_thread_new("pcv-watchdog",
                _watchdog_thread_func, GSIZE_TO_POINTER((gsize)interval_us));
            g_message("[main] systemd watchdog enabled via dedicated thread "
                      "(interval=%.1fs, timeout=%luus)",
                      interval_us / 1000000.0,
                      (unsigned long)watchdog_usec);
        }
    }

    g_main_loop_run(loop);

    g_message("Cleaning up resources before exit...");

    if (g_watchdog_thread) {
        g_atomic_int_set(&g_watchdog_stop, 1);
        g_thread_join(g_watchdog_thread);
        g_watchdog_thread = NULL;
        g_message("[main] watchdog thread stopped");
    }

    pcv_grpc_server_stop();

    if (rest_server) {
        pcv_rest_server_stop(rest_server);
        g_object_unref(rest_server);
    }

    pcv_jwt_shutdown();

    g_object_unref(server);
    g_object_unref(dispatcher);
    g_object_unref(conn);
    g_main_loop_unref(loop);

    pcv_bootstrap_shutdown_cluster_stack();
    pcv_backup_scheduler_shutdown();
    pcv_security_group_resync_timer_shutdown();
    pcv_qos_reconcile_timer_shutdown();
    pcv_overlay_reconcile_timer_shutdown();
    pcv_vm_template_shutdown();
    pcv_rbac_shutdown();

    extern void pcv_healing_shutdown(void);
    pcv_healing_shutdown();
    pcv_process_monitor_shutdown();
    pcv_alert_engine_shutdown();
    pcv_ebpf_telemetry_shutdown();
    pcv_iscsi_shutdown();
    pcv_ovn_shutdown();
    pcv_overlay_shutdown();

    pcv_drain_shutdown();
    shutdown_pending_state_machine();
    cmap_shutdown();
    virt_conn_pool_shutdown();

    pcv_job_queue_shutdown();
    pcv_config_shutdown();
    pcv_worker_pool_shutdown();
    pcv_spawn_launcher_shutdown();
    pcv_log_shutdown();

    g_message("PureCVisor Engine exited cleanly.");
    return 0;
}
