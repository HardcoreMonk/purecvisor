






































#include <glib.h>
#include <libvirt/libvirt.h>
#include <string.h>


#include "../virt/vm_manager.h"
#include "telemetry.h"






static GHashTable *global_metrics_cache = NULL;





static GWeakRef g_signal_emitter_ref;











static gboolean update_metrics_cache_in_main_thread(gpointer user_data) {
    GHashTable *new_cache = (GHashTable *)user_data;


    if (global_metrics_cache != NULL) {
        g_hash_table_destroy(global_metrics_cache);
    }


    global_metrics_cache = new_cache;



    PureCVisorVmManager *mgr =
        PURECVISOR_VM_MANAGER(g_weak_ref_get(&g_signal_emitter_ref));
    if (mgr) {
        purecvisor_vm_manager_emit_metrics_updated(mgr, new_cache);
        g_object_unref(mgr);
    }














    {
        extern void pcv_ws_broadcast(const gchar *type, const gchar *payload_json);
        extern gint pcv_ws_client_count(void);
        if (pcv_ws_client_count() > 0 && new_cache) {
            GString *payload = g_string_new("{\"vm_count\":");
            g_string_append_printf(payload, "%u", g_hash_table_size(new_cache));
            g_string_append(payload, "}");
            pcv_ws_broadcast("metric", payload->str);
            g_string_free(payload, TRUE);
        }
    }

    return G_SOURCE_REMOVE;
}





VmMetrics* get_vm_metrics(const gchar *vm_id) {
    if (G_UNLIKELY(global_metrics_cache == NULL)) return NULL;
    return (VmMetrics*)g_hash_table_lookup(global_metrics_cache, vm_id);
}









static gpointer telemetry_worker_thread(gpointer data) {
    (void)data;











    virConnectPtr conn = virConnectOpen("qemu:///system");
    if (!conn) {
        g_critical("🚨 [Telemetry] Failed to connect to Libvirt. Telemetry daemon shutting down.");
        return NULL;
    }






    virConnectSetKeepAlive(conn, 5, 3);

    g_message("📡 [Telemetry] Background Daemon Thread started successfully.");


    while (TRUE) {
        virDomainStatsRecordPtr *stats = NULL;













        unsigned int stats_flags = VIR_DOMAIN_STATS_CPU_TOTAL | VIR_DOMAIN_STATS_INTERFACE;
        int ret = virConnectGetAllDomainStats(conn, stats_flags, &stats, 0);

        if (ret >= 0 && stats != NULL) {



            GHashTable *new_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);


            for (int i = 0; stats[i] != NULL; i++) {
                virDomainStatsRecordPtr record = stats[i];
                virDomainPtr dom = record->dom;

                char uuid[VIR_UUID_STRING_BUFLEN];
                if (virDomainGetUUIDString(dom, uuid) < 0) continue;

                VmMetrics *metrics = g_new0(VmMetrics, 1);



                for (int j = 0; j < record->nparams; j++) {
                    virTypedParameterPtr param = &record->params[j];

                    if (g_strcmp0(param->field, "cpu.time") == 0) {
                        metrics->cpu_time_ns = param->value.ul;
                    } else if (g_str_has_prefix(param->field, "net.") &&
                               g_str_has_suffix(param->field, ".rx.bytes")) {
                        metrics->rx_bytes += param->value.ul;
                    } else if (g_str_has_prefix(param->field, "net.") &&
                               g_str_has_suffix(param->field, ".tx.bytes")) {
                        metrics->tx_bytes += param->value.ul;
                    } else if (g_str_has_prefix(param->field, "net.") &&
                               g_str_has_suffix(param->field, ".rx.pkts")) {
                        metrics->rx_packets += param->value.ul;
                    } else if (g_str_has_prefix(param->field, "net.") &&
                               g_str_has_suffix(param->field, ".tx.pkts")) {
                        metrics->tx_packets += param->value.ul;
                    } else if (g_str_has_prefix(param->field, "net.") &&
                               g_str_has_suffix(param->field, ".rx.errs")) {
                        metrics->rx_errs += param->value.ul;
                    } else if (g_str_has_prefix(param->field, "net.") &&
                               g_str_has_suffix(param->field, ".tx.errs")) {
                        metrics->tx_errs += param->value.ul;
                    } else if (g_str_has_prefix(param->field, "net.") &&
                               g_str_has_suffix(param->field, ".rx.drop")) {
                        metrics->rx_drop += param->value.ul;
                    } else if (g_str_has_prefix(param->field, "net.") &&
                               g_str_has_suffix(param->field, ".tx.drop")) {
                        metrics->tx_drop += param->value.ul;
                    }
                }


                g_hash_table_insert(new_cache, g_strdup(uuid), metrics);
            }


            virDomainStatsRecordListFree(stats);



            g_main_context_invoke(NULL, update_metrics_cache_in_main_thread, new_cache);
        } else {
            g_warning("⚠️ [Telemetry] Failed to fetch domain stats from Libvirt.");
        }


        g_usleep(1000000);
    }


    virConnectClose(conn);
    return NULL;
}












void init_telemetry_daemon(PureCVisorVmManager *vm_manager) {

    g_weak_ref_init(&g_signal_emitter_ref, vm_manager);

    GError *error = NULL;
    GThread *thread = g_thread_try_new("telemetry-daemon",
                                       telemetry_worker_thread, NULL, &error);
    if (!thread) {
        g_critical("Failed to create telemetry daemon thread: %s", error->message);
        g_error_free(error);
    }
}
