
#include <glib.h>
#include <libvirt/libvirt.h>
#include <string.h>

#include "modules/core/cpu_allocator.h"
#include "modules/ai/self_healing.h"
#include "../network/security_group.h"
#include "../../utils/pcv_worker_pool.h"

#define REBOOT_LOOP_WINDOW_SEC  600
#define REBOOT_LOOP_THRESHOLD   5
#define REBOOT_LOOP_RING        5

typedef struct {
    gint64 stop_us[REBOOT_LOOP_RING];
    gint   pos;
    gint   count;
    gint64 last_alert_us;
} VmRebootTracker;

static GHashTable *g_reboot_trackers = NULL;
static GMutex      g_reboot_mu;
static gboolean    g_reboot_init = FALSE;

static void
_reboot_tracker_init_once(void)
{
    if (g_reboot_init) return;
    g_mutex_init(&g_reboot_mu);
    g_reboot_trackers = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                g_free, g_free);
    g_reboot_init = TRUE;
}

static void
_track_vm_stop(const gchar *uuid, const gchar *vm_name)
{
    if (!uuid) return;
    _reboot_tracker_init_once();

    g_mutex_lock(&g_reboot_mu);
    VmRebootTracker *t = g_hash_table_lookup(g_reboot_trackers, uuid);
    if (!t) {
        t = g_new0(VmRebootTracker, 1);
        g_hash_table_insert(g_reboot_trackers, g_strdup(uuid), t);
    }

    gint64 now = g_get_monotonic_time();
    gint64 window_us = (gint64)REBOOT_LOOP_WINDOW_SEC * G_USEC_PER_SEC;

    t->stop_us[t->pos] = now;
    t->pos = (t->pos + 1) % REBOOT_LOOP_RING;
    if (t->count < REBOOT_LOOP_RING) t->count++;

    gint recent = 0;
    for (gint i = 0; i < t->count; i++) {
        if (t->stop_us[i] > 0 && (now - t->stop_us[i]) <= window_us) recent++;
    }

    gboolean alert = (recent >= REBOOT_LOOP_THRESHOLD) &&
                     (now - t->last_alert_us > 300 * G_USEC_PER_SEC);
    if (alert) t->last_alert_us = now;
    g_mutex_unlock(&g_reboot_mu);

    if (alert) {
        g_warning("🔁 [vm-reboot-loop] VM %s (%s) stopped %d times within %ds — possible boot failure or OOM",
                  vm_name ? vm_name : "(unknown)", uuid, recent, REBOOT_LOOP_WINDOW_SEC);

        pcv_healing_on_anomaly("vm-reboot-loop", (gdouble)recent, 99.0, 0.0, NULL);
    }
}

void init_virt_events_daemon(void);

static gboolean handle_vm_core_release_in_main_thread(gpointer user_data) {
    gchar *vm_name = (gchar *)user_data;
    if (global_allocator != NULL && vm_name != NULL) {
        cpu_allocator_free_vm_cores(global_allocator, vm_name);
    }
    g_free(vm_name);
    return G_SOURCE_REMOVE;
}

static gboolean handle_vm_death_in_main_thread(gpointer user_data) {
    gchar *vm_id = (gchar *)user_data;

    g_warning("🚨 [Self-Healing] Detected CRASHED-like event for VM %s. Triggering healing...", vm_id);

    pcv_healing_on_anomaly("vm-unresponsive", 1.0, 99.0, 0.0, vm_id);

    g_free(vm_id);

    return G_SOURCE_REMOVE;
}

static void
_sg_event_sync_worker(GTask *task, gpointer src, gpointer task_data, GCancellable *c)
{
    (void)src; (void)c;
    const gchar *vm = task_data;
    if (vm) pcv_security_group_sync_vm(vm);
    g_task_return_boolean(task, TRUE);
}

static void
_schedule_sg_sync(const char *vm_name)
{
    if (!vm_name || !pcv_security_group_vm_is_bound(vm_name))
        return;
    GTask *sgt = g_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(sgt, g_strdup(vm_name), g_free);
    pcv_worker_pool_push(sgt, _sg_event_sync_worker);
    g_object_unref(sgt);
}

static int domain_lifecycle_cb(virConnectPtr conn, virDomainPtr dom,
                               int event, int detail, void *opaque)
{
    (void)conn; (void)opaque;

    char uuid[VIR_UUID_STRING_BUFLEN];
    const char *vm_name = virDomainGetName(dom);
    virDomainGetUUIDString(dom, uuid);

    if (event == VIR_DOMAIN_EVENT_STARTED ||
        event == VIR_DOMAIN_EVENT_STOPPED ||
        event == VIR_DOMAIN_EVENT_SHUTDOWN ||
        event == VIR_DOMAIN_EVENT_CRASHED)
        _schedule_sg_sync(vm_name);

    if (event == VIR_DOMAIN_EVENT_STARTED) {
        g_log("signal_probe", G_LOG_LEVEL_DEBUG,
              "[GIO P6] vm-started RECEIVED — vm_name='%s' uuid='%s'",
              vm_name ? vm_name : "(unknown)", uuid);
        return 0;
    }

    if (event == VIR_DOMAIN_EVENT_STOPPED || event == VIR_DOMAIN_EVENT_SHUTDOWN) {
        g_log("signal_probe", G_LOG_LEVEL_DEBUG,
              "[GIO P6] vm-stopped RECEIVED — vm_name='%s' uuid='%s'",
              vm_name ? vm_name : "(unknown)", uuid);

        _track_vm_stop(uuid, vm_name);
    }

    if ((event == VIR_DOMAIN_EVENT_STOPPED ||
         event == VIR_DOMAIN_EVENT_CRASHED) && vm_name) {
        g_main_context_invoke(NULL, handle_vm_core_release_in_main_thread,
                              g_strdup(vm_name));
    }

    gboolean crash_like =
        (event == VIR_DOMAIN_EVENT_CRASHED) ||
        (event == VIR_DOMAIN_EVENT_STOPPED &&
         (detail == VIR_DOMAIN_EVENT_STOPPED_FAILED ||
          detail == VIR_DOMAIN_EVENT_STOPPED_CRASHED));
    if (crash_like) {
        if (virDomainGetUUIDString(dom, uuid) == 0) {

            gchar *uuid_copy = g_strdup(uuid);

            g_main_context_invoke(NULL, handle_vm_death_in_main_thread, uuid_copy);
        }
    }

    return 0;
}

static void
_domain_device_cb(virConnectPtr conn, virDomainPtr dom,
                  const char *devAlias, void *opaque)
{
    (void)conn; (void)devAlias; (void)opaque;

    _schedule_sg_sync(virDomainGetName(dom));
}

static void
_register_device_callbacks(virConnectPtr conn, int *added_id, int *removed_id)
{
    *added_id = virConnectDomainEventRegisterAny(conn, NULL,
        VIR_DOMAIN_EVENT_ID_DEVICE_ADDED,
        VIR_DOMAIN_EVENT_CALLBACK(_domain_device_cb), NULL, NULL);
    *removed_id = virConnectDomainEventRegisterAny(conn, NULL,
        VIR_DOMAIN_EVENT_ID_DEVICE_REMOVED,
        VIR_DOMAIN_EVENT_CALLBACK(_domain_device_cb), NULL, NULL);
    if (*added_id < 0 || *removed_id < 0)
        g_warning("⚠️ [Events] device 콜백 등록 실패 [ADDED=%s REMOVED=%s] — NIC 핫플러그 "
                  "즉시성 저하 (I2-R1 주기 resync 로 fallback)",
                  *added_id   < 0 ? "FAIL" : "ok",
                  *removed_id < 0 ? "FAIL" : "ok");
    else
        g_message("🛡️ [Events] device add/remove 리스너 등록 "
                  "(NIC 핫플러그 즉시 SG 재동기화)");
}

static void
_deregister_device_callbacks(virConnectPtr conn, int *added_id, int *removed_id)
{
    if (*added_id >= 0) {
        virConnectDomainEventDeregisterAny(conn, *added_id);
        *added_id = -1;
    }
    if (*removed_id >= 0) {
        virConnectDomainEventDeregisterAny(conn, *removed_id);
        *removed_id = -1;
    }
}

static gpointer libvirt_event_loop_thread(gpointer data) {
    (void)data;

    virEventRegisterDefaultImpl();

    virConnectPtr event_conn = virConnectOpen("qemu:///system");
    if (!event_conn) {
        g_critical("🚨 [Events] Failed to open Libvirt connection for events. Self-Healing disabled.");
        return NULL;
    }

    virConnectSetKeepAlive(event_conn, 5, 3);

    int callback_id = virConnectDomainEventRegisterAny(
        event_conn,
        NULL,
        VIR_DOMAIN_EVENT_ID_LIFECYCLE,
        VIR_DOMAIN_EVENT_CALLBACK(domain_lifecycle_cb),
        NULL,
        NULL
    );

    if (callback_id < 0) {
        g_critical("🚨 [Events] Failed to register Libvirt lifecycle callback.");
        virConnectClose(event_conn);
        return NULL;
    }

    g_message("🛡️ [Events] Libvirt Lifecycle Listener & Self-Healing Daemon Started.");

    int dev_added_id = -1, dev_removed_id = -1;
    _register_device_callbacks(event_conn, &dev_added_id, &dev_removed_id);

    while (TRUE) {
        if (virEventRunDefaultImpl() < 0) {
            g_warning("⚠️ [Events] Error running Libvirt event loop. Checking connection...");
        }

        if (!virConnectIsAlive(event_conn)) {
            g_warning("⚠️ [Events] libvirtd connection lost — attempting reconnect");

            if (callback_id >= 0) {
                virConnectDomainEventDeregisterAny(event_conn, callback_id);
                callback_id = -1;
            }
            _deregister_device_callbacks(event_conn, &dev_added_id, &dev_removed_id);
            virConnectClose(event_conn);
            event_conn = NULL;

            for (int retry = 0; retry < 6; retry++) {
                g_usleep(5 * G_USEC_PER_SEC);
                event_conn = virConnectOpen("qemu:///system");
                if (event_conn && virConnectIsAlive(event_conn)) {
                    virConnectSetKeepAlive(event_conn, 5, 3);
                    callback_id = virConnectDomainEventRegisterAny(
                        event_conn, NULL, VIR_DOMAIN_EVENT_ID_LIFECYCLE,
                        VIR_DOMAIN_EVENT_CALLBACK(domain_lifecycle_cb),
                        NULL, NULL);
                    if (callback_id >= 0) {
                        g_message("🛡️ [Events] Reconnected to libvirtd after %ds (callback_id=%d)",
                                  (retry + 1) * 5, callback_id);
                        _register_device_callbacks(event_conn, &dev_added_id, &dev_removed_id);
                        break;
                    }

                    virConnectClose(event_conn);
                    event_conn = NULL;
                }
                g_warning("⚠️ [Events] Reconnect attempt %d/6 failed", retry + 1);
            }

            if (!event_conn || !virConnectIsAlive(event_conn)) {
                g_warning("⚠️ [Events] Failed to reconnect after 30s — will retry next loop iteration");
                g_usleep(5 * G_USEC_PER_SEC);
                continue;
            }
        }
    }

    virConnectDomainEventDeregisterAny(event_conn, callback_id);
    _deregister_device_callbacks(event_conn, &dev_added_id, &dev_removed_id);
    virConnectClose(event_conn);
    return NULL;
}

void init_virt_events_daemon(void) {
    GError *error = NULL;
    GThread *thread = g_thread_try_new("libvirt-events", libvirt_event_loop_thread, NULL, &error);

    if (!thread) {
        g_critical("Failed to create Libvirt events daemon thread: %s", error->message);
        g_error_free(error);
    }
}
