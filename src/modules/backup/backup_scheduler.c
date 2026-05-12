





























































#include "backup_scheduler.h"
#include "../../utils/pcv_spawn.h"
#include "../../utils/pcv_config.h"
#include "../../utils/pcv_log.h"
#include "../../utils/pcv_validate.h"
#include "../../modules/virt/virt_conn_pool.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <libvirt/libvirt.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#define BACKUP_LOG_DOM   "backup"
#define POLICY_FILE_PATH "/etc/purecvisor/backup_policies.json"
#define CHECK_INTERVAL   300
#define SNAP_PREFIX      "pcv-auto-"





static GPtrArray *g_policies  = nullptr;
static guint      g_timer_id  = 0;
static GMutex     g_policy_mutex;




static GHashTable *g_vm_inflight = nullptr;












void pcv_backup_policy_free(PcvBackupPolicy *p)
{
    if (!p) return;
    g_free(p->vm_name);
    g_free(p);
}










static PcvBackupPolicy *_policy_dup(const PcvBackupPolicy *src)
{
    PcvBackupPolicy *p = g_new0(PcvBackupPolicy, 1);
    p->vm_name         = g_strdup(src->vm_name);
    p->interval_hours  = src->interval_hours;
    p->retention_count = src->retention_count;
    p->enabled         = src->enabled;
    return p;
}















static gboolean _vm_backup_try_lock(const gchar *vm_name)
{
    if (!vm_name || *vm_name == '\0') return FALSE;

    g_mutex_lock(&g_policy_mutex);
    if (!g_vm_inflight) {
        g_vm_inflight = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, NULL);
    }
    if (g_hash_table_contains(g_vm_inflight, vm_name)) {
        g_mutex_unlock(&g_policy_mutex);
        return FALSE;
    }
    g_hash_table_add(g_vm_inflight, g_strdup(vm_name));
    g_mutex_unlock(&g_policy_mutex);
    return TRUE;
}





static void _vm_backup_unlock(const gchar *vm_name)
{
    if (!vm_name) return;
    g_mutex_lock(&g_policy_mutex);
    if (g_vm_inflight) {
        g_hash_table_remove(g_vm_inflight, vm_name);
    }
    g_mutex_unlock(&g_policy_mutex);
}










static void _check_backup_disk_usage(const gchar *path)
{
    if (!path || !*path) return;
    struct statvfs st;
    if (statvfs(path, &st) != 0) return;
    if (st.f_blocks == 0) return;

    guint64 total = (guint64)st.f_blocks * st.f_frsize;
    guint64 avail = (guint64)st.f_bavail * st.f_frsize;
    if (total == 0) return;
    gdouble used_pct = 100.0 * (gdouble)(total - avail) / (gdouble)total;

    if (used_pct >= 90.0) {
        PCV_LOG_WARN(BACKUP_LOG_DOM,
                     "Backup storage CRITICAL: %s %.1f%% used (avail=%"
                     G_GUINT64_FORMAT " bytes)",
                     path, used_pct, avail);
    } else if (used_pct >= 80.0) {
        PCV_LOG_WARN(BACKUP_LOG_DOM,
                     "Backup storage high usage: %s %.1f%% used",
                     path, used_pct);
    }
}















static void _policies_save_unlocked(void)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "policies");
    json_builder_begin_array(b);

    for (guint i = 0; i < g_policies->len; i++) {
        PcvBackupPolicy *p = g_ptr_array_index(g_policies, i);
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "vm_name");
        json_builder_add_string_value(b, p->vm_name);
        json_builder_set_member_name(b, "interval_hours");
        json_builder_add_int_value(b, p->interval_hours);
        json_builder_set_member_name(b, "retention_count");
        json_builder_add_int_value(b, p->retention_count);
        json_builder_set_member_name(b, "enabled");
        json_builder_add_boolean_value(b, p->enabled);
        json_builder_end_object(b);
    }

    json_builder_end_array(b);
    json_builder_end_object(b);

    JsonGenerator *gen = json_generator_new();
    json_generator_set_pretty(gen, TRUE);
    JsonNode *root = json_builder_get_root(b);
    json_generator_set_root(gen, root);

    GError *err = nullptr;
    if (!json_generator_to_file(gen, POLICY_FILE_PATH, &err)) {
        PCV_LOG_WARN(BACKUP_LOG_DOM, "Failed to save policies: %s",
                     err->message);
        g_error_free(err);
    }

    json_node_free(root);
    g_object_unref(gen);
    g_object_unref(b);
}










static void _policies_load(void)
{
    JsonParser *parser = json_parser_new();
    GError *err = nullptr;

    if (!json_parser_load_from_file(parser, POLICY_FILE_PATH, &err)) {

        PCV_LOG_INFO(BACKUP_LOG_DOM,
                     "No policy file found (%s), starting with empty policies",
                     POLICY_FILE_PATH);
        g_error_free(err);
        g_object_unref(parser);
        return;
    }

    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_object_unref(parser);
        return;
    }

    JsonObject *obj = json_node_get_object(root);
    if (!json_object_has_member(obj, "policies")) {
        g_object_unref(parser);
        return;
    }

    JsonArray *arr = json_object_get_array_member(obj, "policies");
    guint len = json_array_get_length(arr);

    for (guint i = 0; i < len; i++) {
        JsonObject *po = json_array_get_object_element(arr, i);
        if (!po) continue;

        PcvBackupPolicy *p = g_new0(PcvBackupPolicy, 1);
        p->vm_name = g_strdup(
            json_object_get_string_member_with_default(po, "vm_name", "*"));
        p->interval_hours = (gint)
            json_object_get_int_member_with_default(po, "interval_hours", 24);
        p->retention_count = (gint)
            json_object_get_int_member_with_default(po, "retention_count", 7);
        p->enabled =
            json_object_get_boolean_member_with_default(po, "enabled", TRUE);

        g_ptr_array_add(g_policies, p);
    }

    PCV_LOG_INFO(BACKUP_LOG_DOM, "Loaded %u backup policies", g_policies->len);
    g_object_unref(parser);
}













static void
_fsfreeze_vm(const gchar *vm_name)
{
    const gchar *argv[] = {"virsh", "domfsfreeze", vm_name, NULL};
    gchar *out = nullptr;
    GError *err = nullptr;
    if (pcv_spawn_sync(argv, &out, NULL, &err)) {
        PCV_LOG_INFO(BACKUP_LOG_DOM, "Froze filesystem for VM '%s'", vm_name);
    } else {
        PCV_LOG_WARN(BACKUP_LOG_DOM,
                     "fsfreeze failed for '%s' (guest agent may not be running): %s",
                     vm_name, err ? err->message : "unknown");
        if (err) g_error_free(err);
    }
    g_free(out);
}








static void
_fsthaw_vm(const gchar *vm_name)
{
    const gchar *argv[] = {"virsh", "domfsthaw", vm_name, NULL};
    (void)pcv_spawn_sync(argv, NULL, NULL, NULL);
    PCV_LOG_INFO(BACKUP_LOG_DOM, "Thawed filesystem for VM '%s'", vm_name);
}














static gboolean
_verify_snapshot(const gchar *snapshot_name)
{
    const gchar *argv[] = {
        "zfs", "list", "-H", "-o", "used,referenced",
        "-t", "snapshot", snapshot_name, NULL
    };
    gchar *out = nullptr;
    GError *err = nullptr;
    gboolean ok = pcv_spawn_sync(argv, &out, NULL, &err);
    if (!ok || !out || !*out) {
        PCV_LOG_WARN(BACKUP_LOG_DOM, "Snapshot verification failed for '%s': %s",
                     snapshot_name, err ? err->message : "not found");
        if (err) g_error_free(err);
        g_free(out);
        return FALSE;
    }
    PCV_LOG_INFO(BACKUP_LOG_DOM, "Snapshot verified: %s (%s)",
                 snapshot_name, g_strstrip(out));
    g_free(out);
    return TRUE;
}












static gboolean
_verify_replication(const gchar *latest,
                    const gchar *peer_ssh,
                    const gchar *ssh_user)
{
    gchar *remote = g_strdup_printf("%s@%s", ssh_user, peer_ssh);
    const gchar *check_argv[] = {
        "ssh",
        "-o", "ConnectTimeout=10",
        "-o", "StrictHostKeyChecking=no",
        "-o", "UserKnownHostsFile=/dev/null",
        "-i", "/etc/purecvisor/cluster_id_ed25519",
        remote,
        "zfs", "list", "-H", "-o", "name", "-t", "snapshot", latest,
        NULL
    };
    gchar *out = nullptr;
    gboolean ok = pcv_spawn_sync(check_argv, &out, NULL, NULL);

    if (ok && out && *out) {
        PCV_LOG_INFO(BACKUP_LOG_DOM,
                     "Replication verified: remote has %s",
                     g_strstrip(out));
    } else {
        PCV_LOG_WARN(BACKUP_LOG_DOM,
                     "Replication verification failed for %s on %s",
                     latest, peer_ssh);
        ok = FALSE;
    }

    g_free(out);
    g_free(remote);
    return ok;
}


















static GPtrArray *_list_auto_snapshots(const gchar *vm_name)
{
    GPtrArray *result = g_ptr_array_new_with_free_func(g_free);
    const gchar *pool = pcv_config_get_zvol_pool();
    gchar *dataset = g_strdup_printf("%s/%s", pool, vm_name);

    const gchar *argv[] = {
        "zfs", "list", "-H", "-o", "name", "-s", "creation",
        "-t", "snapshot", "-r", dataset, NULL
    };

    gchar *stdout_buf = nullptr;
    gchar *stderr_buf = nullptr;
    GError *err = nullptr;

    gboolean ok = pcv_spawn_sync(argv, &stdout_buf, &stderr_buf, &err);
    g_free(dataset);

    if (!ok) {
        g_free(stdout_buf);
        g_free(stderr_buf);
        if (err) g_error_free(err);
        return result;
    }

    if (stdout_buf) {
        gchar **lines = g_strsplit(g_strstrip(stdout_buf), "\n", -1);
        for (gchar **l = lines; *l; l++) {


            const gchar *at = strrchr(*l, '@');
            if (at && g_str_has_prefix(at + 1, SNAP_PREFIX)) {
                g_ptr_array_add(result, g_strdup(at + 1));
            }
        }
        g_strfreev(lines);
    }

    g_free(stdout_buf);
    g_free(stderr_buf);
    if (err) g_error_free(err);
    return result;
}










static gboolean _create_snapshot(const gchar *vm_name, const gchar *snap_name)
{
    const gchar *pool = pcv_config_get_zvol_pool();
    gchar *target = g_strdup_printf("%s/%s@%s", pool, vm_name, snap_name);

    const gchar *argv[] = {"zfs", "snapshot", target, NULL};
    gchar *stderr_buf = nullptr;
    GError *err = nullptr;

    gboolean ok = pcv_spawn_sync(argv, NULL, &stderr_buf, &err);
    if (!ok) {
        PCV_LOG_WARN(BACKUP_LOG_DOM, "Snapshot create failed: %s — %s",
                     target, err ? err->message : (stderr_buf ? stderr_buf : "unknown"));
    } else {
        PCV_LOG_INFO(BACKUP_LOG_DOM, "Snapshot created: %s", target);
    }

    g_free(target);
    g_free(stderr_buf);
    if (err) g_error_free(err);
    return ok;
}









static void _destroy_snapshot(const gchar *vm_name, const gchar *snap_name)
{
    const gchar *pool = pcv_config_get_zvol_pool();
    gchar *target = g_strdup_printf("%s/%s@%s", pool, vm_name, snap_name);

    const gchar *argv[] = {"zfs", "destroy", target, NULL};
    gchar *stderr_buf = nullptr;
    GError *err = nullptr;


    gboolean ok = pcv_spawn_sync(argv, NULL, &stderr_buf, &err);
    if (!ok) {

        if (stderr_buf && strstr(stderr_buf, "does not exist")) {
            PCV_LOG_INFO(BACKUP_LOG_DOM, "Snapshot already gone (TOCTOU safe): %s", target);
        } else {
            PCV_LOG_WARN(BACKUP_LOG_DOM, "Snapshot destroy failed: %s — %s",
                         target, err ? err->message : (stderr_buf ? stderr_buf : "unknown"));
        }
    } else {
        PCV_LOG_INFO(BACKUP_LOG_DOM, "Snapshot destroyed (retention): %s", target);
    }

    g_free(target);
    g_free(stderr_buf);
    if (err) g_error_free(err);
}

















static GPtrArray *_get_all_vm_names(void)
{
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) return names;

    virDomainPtr *domains = nullptr;
    int n = virConnectListAllDomains(conn, &domains,
                                     VIR_CONNECT_LIST_DOMAINS_PERSISTENT);
    if (n > 0 && domains) {
        for (int i = 0; i < n; i++) {
            const char *name = virDomainGetName(domains[i]);
            if (name) g_ptr_array_add(names, g_strdup(name));
            virDomainFree(domains[i]);
        }
        free(domains);
    }

    virt_conn_pool_release(conn);
    return names;
}

















static time_t _parse_snap_time(const gchar *snap_name)
{

    if (!g_str_has_prefix(snap_name, SNAP_PREFIX)) return 0;

    const gchar *ts = snap_name + strlen(SNAP_PREFIX);

    if (strlen(ts) < 15) return 0;

    struct tm tm_val;
    memset(&tm_val, 0, sizeof(tm_val));


    gchar buf[5];


    memcpy(buf, ts, 4); buf[4] = '\0';
    tm_val.tm_year = atoi(buf) - 1900;


    memcpy(buf, ts + 4, 2); buf[2] = '\0';
    tm_val.tm_mon = atoi(buf) - 1;


    memcpy(buf, ts + 6, 2); buf[2] = '\0';
    tm_val.tm_mday = atoi(buf);




    memcpy(buf, ts + 9, 2); buf[2] = '\0';
    tm_val.tm_hour = atoi(buf);


    memcpy(buf, ts + 11, 2); buf[2] = '\0';
    tm_val.tm_min = atoi(buf);


    memcpy(buf, ts + 13, 2); buf[2] = '\0';
    tm_val.tm_sec = atoi(buf);

    tm_val.tm_isdst = -1;
    return mktime(&tm_val);
}


















static void _apply_policy_for_vm(const PcvBackupPolicy *policy,
                                  const gchar *vm_name)
{
    GPtrArray *snaps = _list_auto_snapshots(vm_name);


    time_t last_snap_time = 0;
    if (snaps->len > 0) {
        const gchar *newest = g_ptr_array_index(snaps, snaps->len - 1);
        last_snap_time = _parse_snap_time(newest);
    }

    time_t now = time(NULL);
    gdouble diff_hours = difftime(now, last_snap_time) / 3600.0;


    if (diff_hours >= (gdouble)policy->interval_hours) {
        struct tm *tm_now = localtime(&now);
        gchar snap_name[64];
        g_snprintf(snap_name, sizeof(snap_name),
                   SNAP_PREFIX "%04d%02d%02d-%02d%02d%02d",
                   tm_now->tm_year + 1900, tm_now->tm_mon + 1,
                   tm_now->tm_mday, tm_now->tm_hour,
                   tm_now->tm_min, tm_now->tm_sec);


        _fsfreeze_vm(vm_name);

        gboolean snap_ok = _create_snapshot(vm_name, snap_name);


        _fsthaw_vm(vm_name);

        if (snap_ok) {

            const gchar *pool = pcv_config_get_zvol_pool();
            gchar *snap_full = g_strdup_printf("%s/%s@%s", pool, vm_name, snap_name);
            _verify_snapshot(snap_full);
            g_free(snap_full);

            g_ptr_array_add(snaps, g_strdup(snap_name));
        }
    }


    while ((gint)snaps->len > policy->retention_count) {
        const gchar *oldest = g_ptr_array_index(snaps, 0);
        _destroy_snapshot(vm_name, oldest);
        g_ptr_array_remove_index(snaps, 0);
    }

    g_ptr_array_unref(snaps);
}
















static void
_backup_worker(GTask *task, gpointer src, gpointer data, GCancellable *c)
{
    (void)task; (void)src; (void)data; (void)c;


    g_mutex_lock(&g_policy_mutex);
    GPtrArray *snapshot = g_ptr_array_new_with_free_func(
        (GDestroyNotify)pcv_backup_policy_free);
    for (guint i = 0; i < g_policies->len; i++) {
        PcvBackupPolicy *p = g_ptr_array_index(g_policies, i);
        if (p->enabled)
            g_ptr_array_add(snapshot, _policy_dup(p));
    }
    g_mutex_unlock(&g_policy_mutex);


    for (guint i = 0; i < snapshot->len; i++) {
        PcvBackupPolicy *policy = g_ptr_array_index(snapshot, i);

        if (g_strcmp0(policy->vm_name, "*") == 0) {

            GPtrArray *vms = _get_all_vm_names();
            for (guint j = 0; j < vms->len; j++) {
                const gchar *vm = g_ptr_array_index(vms, j);
                _apply_policy_for_vm(policy, vm);
            }
            g_ptr_array_unref(vms);
        } else {
            _apply_policy_for_vm(policy, policy->vm_name);
        }
    }

    g_ptr_array_unref(snapshot);
}










static gboolean _backup_check_cb(gpointer user_data __attribute__((unused)))
{
    GTask *task = g_task_new(NULL, NULL, NULL, NULL);
    g_task_run_in_thread(task, _backup_worker);
    g_object_unref(task);
    return G_SOURCE_CONTINUE;
}
















void pcv_backup_scheduler_init(void)
{
    g_mutex_init(&g_policy_mutex);
    g_policies = g_ptr_array_new_with_free_func(
        (GDestroyNotify)pcv_backup_policy_free);

    _policies_load();

    g_timer_id = g_timeout_add_seconds(CHECK_INTERVAL, _backup_check_cb, NULL);
    PCV_LOG_INFO(BACKUP_LOG_DOM,
                 "Backup scheduler started (%ds interval, %u policies)",
                 CHECK_INTERVAL, g_policies->len);
}







void pcv_backup_scheduler_shutdown(void)
{
    if (g_timer_id > 0) {
        g_source_remove(g_timer_id);
        g_timer_id = 0;
    }

    g_mutex_lock(&g_policy_mutex);
    if (g_policies) {
        g_ptr_array_unref(g_policies);
        g_policies = nullptr;
    }
    if (g_vm_inflight) {
        g_hash_table_destroy(g_vm_inflight);
        g_vm_inflight = nullptr;
    }
    g_mutex_unlock(&g_policy_mutex);
    g_mutex_clear(&g_policy_mutex);

    PCV_LOG_INFO(BACKUP_LOG_DOM, "Backup scheduler shut down");
}

















gboolean pcv_backup_policy_set(const gchar *vm_name,
                                gint         interval_hours,
                                gint         retention_count,
                                GError     **error)
{
    if (!vm_name || *vm_name == '\0') {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "vm_name is required");
        return FALSE;
    }
    if (interval_hours < 1) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "interval_hours must be >= 1");
        return FALSE;
    }
    if (retention_count < 1) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "retention_count must be >= 1");
        return FALSE;
    }

    g_mutex_lock(&g_policy_mutex);


    gboolean found = FALSE;
    for (guint i = 0; i < g_policies->len; i++) {
        PcvBackupPolicy *p = g_ptr_array_index(g_policies, i);
        if (g_strcmp0(p->vm_name, vm_name) == 0) {
            p->interval_hours  = interval_hours;
            p->retention_count = retention_count;
            p->enabled         = TRUE;
            found = TRUE;
            break;
        }
    }

    if (!found) {
        PcvBackupPolicy *p = g_new0(PcvBackupPolicy, 1);
        p->vm_name         = g_strdup(vm_name);
        p->interval_hours  = interval_hours;
        p->retention_count = retention_count;
        p->enabled         = TRUE;
        g_ptr_array_add(g_policies, p);
    }

    _policies_save_unlocked();
    g_mutex_unlock(&g_policy_mutex);

    PCV_LOG_INFO(BACKUP_LOG_DOM,
                 "Policy %s: vm=%s interval=%dh retention=%d",
                 found ? "updated" : "created",
                 vm_name, interval_hours, retention_count);
    return TRUE;
}













gboolean pcv_backup_policy_delete(const gchar *vm_name, GError **error)
{
    if (!vm_name || *vm_name == '\0') {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "vm_name is required");
        return FALSE;
    }

    g_mutex_lock(&g_policy_mutex);

    gboolean found = FALSE;
    for (guint i = 0; i < g_policies->len; i++) {
        PcvBackupPolicy *p = g_ptr_array_index(g_policies, i);
        if (g_strcmp0(p->vm_name, vm_name) == 0) {
            g_ptr_array_remove_index(g_policies, i);
            found = TRUE;
            break;
        }
    }

    if (found) {
        _policies_save_unlocked();
    }

    g_mutex_unlock(&g_policy_mutex);

    if (!found) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "No policy found for vm '%s'", vm_name);
        return FALSE;
    }

    PCV_LOG_INFO(BACKUP_LOG_DOM, "Policy deleted: vm=%s", vm_name);
    return TRUE;
}










GPtrArray *pcv_backup_policy_list(void)
{
    GPtrArray *result = g_ptr_array_new_with_free_func(
        (GDestroyNotify)pcv_backup_policy_free);

    g_mutex_lock(&g_policy_mutex);
    for (guint i = 0; i < g_policies->len; i++) {
        PcvBackupPolicy *src = g_ptr_array_index(g_policies, i);
        g_ptr_array_add(result, _policy_dup(src));
    }
    g_mutex_unlock(&g_policy_mutex);

    return result;
}














GPtrArray *pcv_backup_history(const gchar *vm_name)
{
    if (!vm_name || *vm_name == '\0') {
        return g_ptr_array_new_with_free_func(g_free);
    }
    return _list_auto_snapshots(vm_name);
}
















GPtrArray *pcv_backup_history_paged(const gchar *vm_name,
                                     guint        offset,
                                     guint        limit,
                                     guint       *total_out)
{
    GPtrArray *all = pcv_backup_history(vm_name);

    if (total_out)
        *total_out = all->len;


    if (limit == 0 && offset == 0) {
        return all;
    }

    GPtrArray *result = g_ptr_array_new_with_free_func(g_free);

    guint start = (offset < all->len) ? offset : all->len;
    guint end   = (limit > 0) ? MIN(start + limit, all->len) : all->len;

    for (guint i = start; i < end; i++) {
        const gchar *snap = g_ptr_array_index(all, i);
        g_ptr_array_add(result, g_strdup(snap));
    }

    g_ptr_array_unref(all);
    return result;
}





















gboolean pcv_backup_restore(const gchar *vm_name,
                             const gchar *snapshot_name,
                             GError     **error)
{
    if (!vm_name || !snapshot_name) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "vm_name and snapshot_name are required");
        return FALSE;
    }


    if (!_vm_backup_try_lock(vm_name)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "Another backup operation is in progress for VM '%s'",
                    vm_name);
        return FALSE;
    }

    const gchar *pool = pcv_config_get_zvol_pool();
    gchar *target = g_strdup_printf("%s/%s@%s", pool, vm_name, snapshot_name);

    PCV_LOG_INFO(BACKUP_LOG_DOM, "Restoring: %s", target);



    gboolean was_running = FALSE;
    virConnectPtr conn = virt_conn_pool_acquire();
    if (conn) {
        virDomainPtr dom = virDomainLookupByName(conn, vm_name);
        if (dom) {
            int state = 0, reason = 0;
            if (virDomainGetState(dom, &state, &reason, 0) == 0) {
                was_running = (state == VIR_DOMAIN_RUNNING ||
                               state == VIR_DOMAIN_PAUSED);
            }
            if (was_running) {
                PCV_LOG_INFO(BACKUP_LOG_DOM,
                             "Restore: shutting down VM '%s' before rollback",
                             vm_name);
                virDomainShutdown(dom);
                for (int i = 0; i < 50; i++) {
                    g_usleep(100 * 1000);
                    if (virDomainGetState(dom, &state, &reason, 0) != 0) break;
                    if (state == VIR_DOMAIN_SHUTOFF) break;
                }
                if (virDomainGetState(dom, &state, &reason, 0) == 0 &&
                    state != VIR_DOMAIN_SHUTOFF) {
                    PCV_LOG_WARN(BACKUP_LOG_DOM,
                                 "Restore: graceful shutdown timeout — "
                                 "force-destroying VM '%s'", vm_name);
                    virDomainDestroy(dom);
                    for (int i = 0; i < 50; i++) {
                        g_usleep(100 * 1000);
                        if (virDomainGetState(dom, &state, &reason, 0) != 0) break;
                        if (state == VIR_DOMAIN_SHUTOFF) break;
                    }
                }
            }
            virDomainFree(dom);
        }

    }

    const gchar *argv[] = {"zfs", "rollback", "-r", target, NULL};
    gchar *stderr_buf = nullptr;
    GError *local_err = nullptr;

    gboolean ok = pcv_spawn_sync(argv, NULL, &stderr_buf, &local_err);
    if (!ok) {
        const gchar *msg = local_err ? local_err->message
                         : (stderr_buf ? stderr_buf : "ZFS rollback failed");
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", msg);
        PCV_LOG_WARN(BACKUP_LOG_DOM, "Restore failed: %s — %s", target, msg);
    } else {
        PCV_LOG_INFO(BACKUP_LOG_DOM, "Restore complete: %s", target);
    }


    if (conn && was_running) {
        gboolean restart_ok = FALSE;
        virDomainPtr dom = virDomainLookupByName(conn, vm_name);
        if (dom) {
            PCV_LOG_INFO(BACKUP_LOG_DOM,
                         "Restore: restarting VM '%s'", vm_name);
            for (int attempt = 0; attempt < 10; attempt++) {
                if (virDomainCreate(dom) == 0) {
                    restart_ok = TRUE;
                    break;
                }
                g_usleep(200 * 1000);
            }
            if (!restart_ok) {
                PCV_LOG_WARN(BACKUP_LOG_DOM,
                             "Restore: failed to restart VM '%s' after rollback",
                             vm_name);
                if (ok) {
                    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Rollback completed but VM '%s' did not restart",
                                vm_name);
                    ok = FALSE;
                }
            }
            virDomainFree(dom);
        }
    }
    if (conn) virt_conn_pool_release(conn);

    g_free(target);
    g_free(stderr_buf);
    if (local_err) g_error_free(local_err);

    _vm_backup_unlock(vm_name);
    return ok;
}





#define BACKUP_DIR_DEFAULT "/var/lib/purecvisor/backups"
#define INCR_SNAP_PREFIX   "incr-"







static const gchar *_ensure_backup_dir(void)
{
    const gchar *dir = pcv_config_get_string("backup", "backup_dir",
                                              BACKUP_DIR_DEFAULT);
    g_mkdir_with_parents(dir, 0750);
    return dir;
}










static GPtrArray *_list_all_snapshots(const gchar *vm_name)
{
    GPtrArray *result = g_ptr_array_new_with_free_func(g_free);
    const gchar *pool = pcv_config_get_zvol_pool();
    gchar *dataset = g_strdup_printf("%s/%s", pool, vm_name);

    const gchar *argv[] = {
        "zfs", "list", "-H", "-o", "name", "-s", "creation",
        "-t", "snapshot", "-r", dataset, NULL
    };

    gchar *stdout_buf = nullptr;
    gchar *stderr_buf = nullptr;
    GError *err = nullptr;

    gboolean ok = pcv_spawn_sync(argv, &stdout_buf, &stderr_buf, &err);
    g_free(dataset);

    if (!ok) {
        g_free(stdout_buf);
        g_free(stderr_buf);
        if (err) g_error_free(err);
        return result;
    }

    if (stdout_buf) {
        gchar **lines = g_strsplit(g_strstrip(stdout_buf), "\n", -1);
        for (gchar **l = lines; *l && **l; l++) {
            g_ptr_array_add(result, g_strdup(*l));
        }
        g_strfreev(lines);
    }

    g_free(stdout_buf);
    g_free(stderr_buf);
    if (err) g_error_free(err);
    return result;
}















JsonObject *pcv_backup_incremental(const gchar *vm_name, GError **error)
{
    if (!vm_name || *vm_name == '\0') {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "vm_name is required");
        return NULL;
    }


    if (!_vm_backup_try_lock(vm_name)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "Another backup operation is in progress for VM '%s'",
                    vm_name);
        return NULL;
    }

    const gchar *pool = pcv_config_get_zvol_pool();
    const gchar *backup_dir = _ensure_backup_dir();


    _check_backup_disk_usage(backup_dir);


    GPtrArray *snaps = _list_all_snapshots(vm_name);
    const gchar *prev_snap = nullptr;
    if (snaps->len > 0) {
        prev_snap = g_ptr_array_index(snaps, snaps->len - 1);
    }


    time_t now = time(NULL);
    struct tm *tm_now = localtime(&now);
    gchar snap_name[64];
    g_snprintf(snap_name, sizeof(snap_name),
               INCR_SNAP_PREFIX "%04d%02d%02d-%02d%02d%02d",
               tm_now->tm_year + 1900, tm_now->tm_mon + 1,
               tm_now->tm_mday, tm_now->tm_hour,
               tm_now->tm_min, tm_now->tm_sec);

    gchar *new_snap = g_strdup_printf("%s/%s@%s", pool, vm_name, snap_name);

    const gchar *snap_argv[] = {"zfs", "snapshot", new_snap, NULL};
    gchar *stderr_buf = nullptr;
    GError *local_err = nullptr;

    gboolean ok = pcv_spawn_sync(snap_argv, NULL, &stderr_buf, &local_err);
    if (!ok) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Failed to create snapshot %s: %s",
                    new_snap, local_err ? local_err->message
                                        : (stderr_buf ? stderr_buf : "unknown"));
        g_free(new_snap);
        g_free(stderr_buf);
        if (local_err) g_error_free(local_err);
        g_ptr_array_unref(snaps);
        _vm_backup_unlock(vm_name);
        return NULL;
    }
    g_free(stderr_buf);
    if (local_err) { g_error_free(local_err); local_err = nullptr; }


    gchar ts_buf[32];
    g_snprintf(ts_buf, sizeof(ts_buf), "%04d%02d%02d-%02d%02d%02d",
               tm_now->tm_year + 1900, tm_now->tm_mon + 1,
               tm_now->tm_mday, tm_now->tm_hour,
               tm_now->tm_min, tm_now->tm_sec);

    gchar *out_file = g_strdup_printf("%s/%s_incr_%s.zfs",
                                       backup_dir, vm_name, ts_buf);

    gchar *cmd = nullptr;
    gchar *base_snap_name = nullptr;
    if (prev_snap) {

        gchar *q_prev = g_shell_quote(prev_snap);
        gchar *q_new  = g_shell_quote(new_snap);
        gchar *q_file = g_shell_quote(out_file);
        cmd = g_strdup_printf("zfs send -i %s %s > %s", q_prev, q_new, q_file);

        const gchar *at = strrchr(prev_snap, '@');
        base_snap_name = g_strdup(at ? at + 1 : prev_snap);
        g_free(q_prev);
        g_free(q_new);
        g_free(q_file);
    } else {

        gchar *q_new  = g_shell_quote(new_snap);
        gchar *q_file = g_shell_quote(out_file);
        cmd = g_strdup_printf("zfs send %s > %s", q_new, q_file);
        g_free(q_new);
        g_free(q_file);
    }

    stderr_buf = nullptr;
    const gchar *sh_argv[] = {"/bin/sh", "-c", cmd, NULL};
    ok = pcv_spawn_sync(sh_argv, NULL, &stderr_buf, &local_err);
    if (!ok) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "zfs send failed: %s",
                    local_err ? local_err->message
                              : (stderr_buf ? stderr_buf : "unknown"));
        g_free(cmd);
        g_free(out_file);
        g_free(new_snap);
        g_free(base_snap_name);
        g_free(stderr_buf);
        if (local_err) g_error_free(local_err);
        g_ptr_array_unref(snaps);
        _vm_backup_unlock(vm_name);
        return NULL;
    }
    g_free(cmd);
    g_free(stderr_buf);
    if (local_err) { g_error_free(local_err); local_err = nullptr; }


    struct stat st;
    gint64 file_size = 0;
    if (stat(out_file, &st) == 0) {
        file_size = (gint64)st.st_size;
    }

    PCV_LOG_INFO(BACKUP_LOG_DOM,
                 "Incremental backup: %s → %s (%s, %" G_GINT64_FORMAT " bytes)",
                 vm_name, snap_name,
                 prev_snap ? "incremental" : "full",
                 file_size);


    JsonObject *result = json_object_new();
    json_object_set_string_member(result, "snapshot", snap_name);
    json_object_set_string_member(result, "base_snapshot",
                                  base_snap_name ? base_snap_name : "none");
    json_object_set_string_member(result, "file", out_file);
    json_object_set_int_member(result, "size_bytes", file_size);
    json_object_set_string_member(result, "mode",
                                  prev_snap ? "incremental" : "full");

    g_free(new_snap);
    g_free(out_file);
    g_free(base_snap_name);
    g_ptr_array_unref(snaps);
    _vm_backup_unlock(vm_name);
    return result;
}


















JsonObject *pcv_backup_verify(const gchar *vm_name,
                              const gchar *snapshot_name,
                              GError     **error)
{
    if (!vm_name || !snapshot_name) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "vm_name and snapshot_name are required");
        return NULL;
    }

    const gchar *pool = pcv_config_get_zvol_pool();
    gchar *snap_full = g_strdup_printf("%s/%s@%s", pool, vm_name, snapshot_name);


    const gchar *list_argv[] = {
        "zfs", "list", "-t", "snapshot", "-H", snap_full, NULL
    };
    gchar *stderr_buf = nullptr;
    GError *local_err = nullptr;

    gboolean ok = pcv_spawn_sync(list_argv, NULL, &stderr_buf, &local_err);
    if (!ok) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "Snapshot not found: %s", snap_full);
        g_free(snap_full);
        g_free(stderr_buf);
        if (local_err) g_error_free(local_err);
        return NULL;
    }
    g_free(stderr_buf);
    if (local_err) { g_error_free(local_err); local_err = nullptr; }


    const gchar *send_argv[] = {
        "zfs", "send", "-n", snap_full, NULL
    };
    stderr_buf = nullptr;
    ok = pcv_spawn_sync(send_argv, NULL, &stderr_buf, &local_err);

    const gchar *integrity = ok ? "ok" : "failed";

    if (!ok) {
        PCV_LOG_WARN(BACKUP_LOG_DOM,
                     "Integrity check failed for %s: %s",
                     snap_full,
                     local_err ? local_err->message
                               : (stderr_buf ? stderr_buf : "unknown"));
    }
    g_free(stderr_buf);
    if (local_err) { g_error_free(local_err); local_err = nullptr; }


    const gchar *size_argv[] = {
        "zfs", "get", "-H", "-o", "value", "-p", "used", snap_full, NULL
    };
    gchar *stdout_buf = nullptr;
    stderr_buf = nullptr;
    gint64 size_bytes = 0;

    if (pcv_spawn_sync(size_argv, &stdout_buf, &stderr_buf, &local_err)) {
        if (stdout_buf) {
            size_bytes = g_ascii_strtoll(g_strstrip(stdout_buf), NULL, 10);
        }
    }
    g_free(stdout_buf);
    g_free(stderr_buf);
    if (local_err) { g_error_free(local_err); local_err = nullptr; }

    PCV_LOG_INFO(BACKUP_LOG_DOM,
                 "Verify: %s — integrity=%s size=%" G_GINT64_FORMAT,
                 snap_full, integrity, size_bytes);


    JsonObject *result = json_object_new();
    json_object_set_boolean_member(result, "verified", ok);
    json_object_set_string_member(result, "snapshot", snapshot_name);
    json_object_set_int_member(result, "size_bytes", size_bytes);
    json_object_set_string_member(result, "integrity", integrity);

    g_free(snap_full);
    return result;
}













static gboolean _remote_snapshot_exists(const gchar *ssh_user,
                                         const gchar *target,
                                         const gchar *snap)
{
    gchar *remote = g_strdup_printf("%s@%s", ssh_user, target);
    const gchar *argv[] = {
        "ssh",
        "-o", "ConnectTimeout=10",
        "-o", "StrictHostKeyChecking=no",
        "-o", "UserKnownHostsFile=/dev/null",
        "-i", "/etc/purecvisor/cluster_id_ed25519",
        remote,
        "zfs", "list", "-t", "snapshot", "-H", snap,
        NULL
    };
    GError *err = nullptr;
    gboolean ok = pcv_spawn_sync(argv, NULL, NULL, &err);
    if (err) g_error_free(err);
    g_free(remote);
    return ok;
}















static void
_enforce_remote_retention(const gchar *pool, const gchar *vm_name,
                          const gchar *peer_ssh, const gchar *ssh_user,
                          gint remote_retention)
{
    if (remote_retention <= 0) return;

    gchar *dataset_str = g_strdup_printf("%s/%s", pool, vm_name);
    gchar *dataset_prefix = g_strdup_printf("%s@", dataset_str);
    gchar *remote = g_strdup_printf("%s@%s", ssh_user, peer_ssh);
    const gchar *list_argv[] = {
        "ssh",
        "-o", "ConnectTimeout=10",
        "-o", "StrictHostKeyChecking=no",
        "-o", "UserKnownHostsFile=/dev/null",
        "-i", "/etc/purecvisor/cluster_id_ed25519",
        remote,
        "zfs", "list", "-H", "-o", "name", "-t", "snapshot",
        "-S", "creation", dataset_str,
        NULL
    };
    gchar *out = nullptr;
    GError *err = nullptr;

    if (!pcv_spawn_sync(list_argv, &out, NULL, &err)) {
        PCV_LOG_WARN(BACKUP_LOG_DOM,
                     "Failed to enforce remote retention for %s/%s on %s: %s",
                     pool, vm_name, peer_ssh,
                     err ? err->message : "unknown");
        if (err) g_error_free(err);
        g_free(out);
        g_free(remote);
        g_free(dataset_prefix);
        g_free(dataset_str);
        return;
    }

    gchar **lines = g_strsplit(out ? out : "", "\n", -1);
    gint seen = 0;
    gint destroyed = 0;
    gint failed = 0;

    for (guint i = 0; lines && lines[i]; i++) {
        gchar *snap = g_strstrip(lines[i]);
        if (!snap || *snap == '\0')
            continue;

        if (!g_str_has_prefix(snap, dataset_prefix)) {
            PCV_LOG_WARN(BACKUP_LOG_DOM,
                         "Skipping unexpected remote snapshot outside %s: %s",
                         dataset_str, snap);
            continue;
        }

        seen++;
        if (seen <= remote_retention)
            continue;

        const gchar *destroy_argv[] = {
            "ssh",
            "-o", "ConnectTimeout=10",
            "-o", "StrictHostKeyChecking=no",
            "-o", "UserKnownHostsFile=/dev/null",
            "-i", "/etc/purecvisor/cluster_id_ed25519",
            remote,
            "sudo", "zfs", "destroy", snap,
            NULL
        };
        GError *destroy_err = nullptr;
        if (pcv_spawn_sync(destroy_argv, NULL, NULL, &destroy_err)) {
            destroyed++;
        } else {
            failed++;
            PCV_LOG_WARN(BACKUP_LOG_DOM,
                         "Failed to destroy old remote snapshot %s on %s: %s",
                         snap, peer_ssh,
                         destroy_err ? destroy_err->message : "unknown");
        }
        if (destroy_err) g_error_free(destroy_err);
    }

    if (failed == 0) {
        PCV_LOG_INFO(BACKUP_LOG_DOM,
                     "Enforced remote retention (%d) for %s/%s on %s, destroyed=%d",
                     remote_retention, pool, vm_name, peer_ssh, destroyed);
    }

    if (err) g_error_free(err);
    g_strfreev(lines);
    g_free(out);
    g_free(remote);
    g_free(dataset_prefix);
    g_free(dataset_str);
}
















gboolean pcv_backup_replicate(const gchar *vm_name,
                              const gchar *target_node,
                              const gchar *ssh_user,
                              GError     **error)
{
    if (!pcv_validate_vm_name(vm_name)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Invalid VM name for replication");
        return FALSE;
    }
    if (!pcv_validate_remote_host(target_node)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Invalid replication target node");
        return FALSE;
    }

    const gchar *user = ssh_user;
    if (!user || *user == '\0') {
        user = pcv_config_get_ssh_user();
    }
    if (!pcv_validate_ssh_user(user)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Invalid replication SSH user");
        return FALSE;
    }


    if (!_vm_backup_try_lock(vm_name)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "Another backup operation is in progress for VM '%s'",
                    vm_name);
        return FALSE;
    }

    const gchar *pool = pcv_config_get_zvol_pool();
    gchar *dataset = g_strdup_printf("%s/%s", pool, vm_name);


    GPtrArray *snaps = _list_all_snapshots(vm_name);
    if (snaps->len == 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "No snapshots found for VM '%s'", vm_name);
        g_ptr_array_unref(snaps);
        g_free(dataset);
        _vm_backup_unlock(vm_name);
        return FALSE;
    }

    const gchar *latest = g_ptr_array_index(snaps, snaps->len - 1);
    const gchar *base = (snaps->len >= 2)
                        ? (const gchar *)g_ptr_array_index(snaps, snaps->len - 2)
                        : NULL;

    gint bw_mbps = pcv_config_get_int("cluster", "repl_bandwidth_mbps", 0);
    if (bw_mbps > 0) {
        PCV_LOG_WARN(BACKUP_LOG_DOM,
                     "Replication bandwidth limit (%d Mbps) ignored in shell-free path",
                     bw_mbps);
    }


    gboolean incremental = base && _remote_snapshot_exists(user, target_node, base);
    gchar *remote = g_strdup_printf("%s@%s", user, target_node);
    const gchar *producer_full_argv[] = {
        "zfs", "send", latest,
        NULL
    };
    const gchar *producer_incremental_argv[] = {
        "zfs", "send", "-i", base, latest,
        NULL
    };
    const gchar *consumer_argv[] = {
        "ssh",
        "-o", "ConnectTimeout=10",
        "-o", "StrictHostKeyChecking=no",
        "-o", "UserKnownHostsFile=/dev/null",
        "-i", "/etc/purecvisor/cluster_id_ed25519",
        remote,
        "sudo", "zfs", "recv", "-F", dataset,
        NULL
    };
    const gchar * const *producer_argv = incremental
                                         ? producer_incremental_argv
                                         : producer_full_argv;

    GTimer *timer = g_timer_new();
    gchar *stderr_buf = nullptr;
    GError *local_err = nullptr;

    gboolean ok = pcv_spawn_pipe_sync(producer_argv, consumer_argv,
                                      NULL, &stderr_buf, &local_err);
    gdouble elapsed = g_timer_elapsed(timer, NULL);
    g_timer_destroy(timer);

    if (!ok) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "%s replication failed for %s → %s: %s",
                    incremental ? "Incremental" : "Full",
                    vm_name, target_node,
                    local_err ? local_err->message
                              : (stderr_buf ? stderr_buf : "unknown"));
        if (local_err) g_error_free(local_err);
    } else {
        PCV_LOG_INFO(BACKUP_LOG_DOM,
                     "Replication complete: %s → %s@%s (%s, %.1fs)",
                     vm_name, user, target_node,
                     incremental ? "incremental" : "full",
                     elapsed);


        _verify_replication(latest, target_node, user);


        gint remote_retention = pcv_config_get_int("backup", "remote_retention", 0);
        if (remote_retention > 0) {
            _enforce_remote_retention(pool, vm_name, target_node, user, remote_retention);
        }
    }

    g_free(stderr_buf);
    g_free(remote);
    g_free(dataset);
    g_ptr_array_unref(snaps);
    _vm_backup_unlock(vm_name);
    return ok;
}






















#define S3_SNAP_PREFIX "s3-"
#define S3_TEMP_DIR    "/tmp"










static gchar **
_s3_build_env(const gchar *region)
{
    gchar *ak = pcv_config_get_secret("backup", "s3_access_key", NULL);
    gchar *sk = pcv_config_get_secret("backup", "s3_secret_key", NULL);
    const gchar *reg = (region && *region) ? region
                       : pcv_config_get_string("backup", "s3_region", "ap-northeast-2");


    GPtrArray *env = g_ptr_array_new();
    for (gchar **e = g_get_environ(); e && *e; e++) {
        g_ptr_array_add(env, g_strdup(*e));
    }
    if (ak) { g_ptr_array_add(env, g_strdup_printf("AWS_ACCESS_KEY_ID=%s", ak)); g_free(ak); }
    if (sk) { g_ptr_array_add(env, g_strdup_printf("AWS_SECRET_ACCESS_KEY=%s", sk)); g_free(sk); }
    g_ptr_array_add(env, g_strdup_printf("AWS_DEFAULT_REGION=%s", reg));
    g_ptr_array_add(env, NULL);
    return (gchar **)g_ptr_array_free(env, FALSE);
}









#define S3_MULTIPART_THRESHOLD  (100 * 1024 * 1024)













static gboolean
_s3_upload_multipart(const gchar *local_path, const gchar *s3_path,
                     const gchar *endpoint, const gchar *bucket,
                     const gchar *region, GError **error)
{
    gchar *s3_url = g_strdup_printf("s3://%s/%s", bucket, s3_path);


    struct stat st;
    gchar size_str[32] = "5368709120";
    if (g_stat(local_path, &st) == 0) {
        g_snprintf(size_str, sizeof(size_str), "%" G_GINT64_FORMAT, (gint64)st.st_size);
    }

    GPtrArray *argv = g_ptr_array_new();
    g_ptr_array_add(argv, (gchar *)"aws");
    g_ptr_array_add(argv, (gchar *)"s3");
    g_ptr_array_add(argv, (gchar *)"cp");
    g_ptr_array_add(argv, (gchar *)local_path);
    g_ptr_array_add(argv, s3_url);
    if (endpoint && *endpoint) {
        g_ptr_array_add(argv, (gchar *)"--endpoint-url");
        g_ptr_array_add(argv, (gchar *)endpoint);
    }
    if (region && *region) {
        g_ptr_array_add(argv, (gchar *)"--region");
        g_ptr_array_add(argv, (gchar *)region);
    }
    g_ptr_array_add(argv, (gchar *)"--expected-size");
    g_ptr_array_add(argv, size_str);
    g_ptr_array_add(argv, (gchar *)"--no-progress");
    g_ptr_array_add(argv, NULL);

    gchar *std_err = nullptr;
    GError *local_err = nullptr;
    gboolean ok = pcv_spawn_sync((const gchar * const *)argv->pdata,
                                  NULL, &std_err, &local_err);
    if (!ok) {
        PCV_LOG_WARN(BACKUP_LOG_DOM,
                     "S3 multipart upload failed for %s: %s",
                     s3_url,
                     local_err ? local_err->message
                               : (std_err ? std_err : "unknown"));
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "S3 multipart upload failed: %s — %s",
                    s3_url,
                    local_err ? local_err->message
                              : (std_err ? std_err : "unknown"));
        if (local_err) g_error_free(local_err);
    } else {
        PCV_LOG_INFO(BACKUP_LOG_DOM,
                     "S3 multipart upload complete: %s (size=%s)",
                     s3_url, size_str);
    }

    g_free(std_err);
    g_free(s3_url);
    g_ptr_array_free(argv, TRUE);
    return ok;
}













static gboolean
_s3_upload_file(const gchar *endpoint, const gchar *bucket,
                const gchar *s3_key, const gchar *local_path,
                const gchar *content_type,
                GError **error)
{

    struct stat mp_st;
    if (g_stat(local_path, &mp_st) == 0 && mp_st.st_size > S3_MULTIPART_THRESHOLD) {
        PCV_LOG_INFO(BACKUP_LOG_DOM,
                     "File %s exceeds 100MB (%" G_GINT64_FORMAT "), using multipart upload",
                     local_path, (gint64)mp_st.st_size);

        return _s3_upload_multipart(local_path, s3_key, endpoint, bucket,
                                     g_getenv("AWS_DEFAULT_REGION"), error);
    }

    gchar *s3_uri = g_strdup_printf("s3://%s/%s", bucket, s3_key);


    GPtrArray *argv = g_ptr_array_new();
    g_ptr_array_add(argv, (gchar *)"aws");
    g_ptr_array_add(argv, (gchar *)"s3");
    g_ptr_array_add(argv, (gchar *)"cp");
    g_ptr_array_add(argv, (gchar *)local_path);
    g_ptr_array_add(argv, s3_uri);
    if (endpoint && *endpoint) {
        g_ptr_array_add(argv, (gchar *)"--endpoint-url");
        g_ptr_array_add(argv, (gchar *)endpoint);
    }
    if (content_type && *content_type) {
        g_ptr_array_add(argv, (gchar *)"--content-type");
        g_ptr_array_add(argv, (gchar *)content_type);
    }
    g_ptr_array_add(argv, NULL);

    gchar *stderr_buf = nullptr;
    GError *local_err = nullptr;
    gboolean ok = pcv_spawn_sync((const gchar * const *)argv->pdata,
                                  NULL, &stderr_buf, &local_err);
    if (!ok) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "S3 upload failed: %s — %s",
                    s3_uri,
                    local_err ? local_err->message
                              : (stderr_buf ? stderr_buf : "unknown"));
        if (local_err) g_error_free(local_err);
    }

    g_free(stderr_buf);
    g_free(s3_uri);
    g_ptr_array_free(argv, TRUE);
    return ok;
}

gboolean
pcv_backup_export_s3(const gchar *vm_name,
                      const gchar *s3_endpoint,
                      const gchar *s3_bucket,
                      const gchar *s3_key_prefix,
                      GError     **error)
{
    if (!vm_name || !*vm_name) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "vm_name is required");
        return FALSE;
    }


    if (!_vm_backup_try_lock(vm_name)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "Another backup operation is in progress for VM '%s'",
                    vm_name);
        return FALSE;
    }


    const gchar *endpoint = (s3_endpoint && *s3_endpoint) ? s3_endpoint
        : pcv_config_get_string("backup", "s3_endpoint", "");
    const gchar *bucket = (s3_bucket && *s3_bucket) ? s3_bucket
        : pcv_config_get_string("backup", "s3_bucket", "");
    const gchar *prefix = (s3_key_prefix && *s3_key_prefix) ? s3_key_prefix
        : pcv_config_get_string("backup", "s3_key_prefix", "pcv-backup/");

    if (!bucket || !*bucket) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "S3 bucket not configured (daemon.conf [backup] s3_bucket)");
        _vm_backup_unlock(vm_name);
        return FALSE;
    }


    time_t now = time(NULL);
    struct tm *tm_now = localtime(&now);
    gchar ts[32];
    g_snprintf(ts, sizeof(ts), "%04d%02d%02d-%02d%02d%02d",
               tm_now->tm_year + 1900, tm_now->tm_mon + 1,
               tm_now->tm_mday, tm_now->tm_hour,
               tm_now->tm_min, tm_now->tm_sec);


    _check_backup_disk_usage(S3_TEMP_DIR);


    const gchar *pool = pcv_config_get_zvol_pool();
    gchar *snap_name = g_strdup_printf("%s%s", S3_SNAP_PREFIX, ts);
    gchar *snap_full = g_strdup_printf("%s/%s@%s", pool, vm_name, snap_name);

    const gchar *snap_argv[] = {"zfs", "snapshot", snap_full, NULL};
    gchar *stderr_buf = nullptr;
    GError *local_err = nullptr;

    if (!pcv_spawn_sync(snap_argv, NULL, &stderr_buf, &local_err)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "ZFS snapshot failed: %s — %s",
                    snap_full,
                    local_err ? local_err->message
                              : (stderr_buf ? stderr_buf : "unknown"));
        g_free(snap_name);
        g_free(snap_full);
        g_free(stderr_buf);
        if (local_err) g_error_free(local_err);
        _vm_backup_unlock(vm_name);
        return FALSE;
    }
    g_free(stderr_buf);
    if (local_err) { g_error_free(local_err); local_err = nullptr; }

    PCV_LOG_INFO(BACKUP_LOG_DOM, "S3 backup: snapshot created %s", snap_full);


    gchar *tmp_file = g_strdup_printf("%s/pcv-s3-%s-%s.zfs.gz",
                                       S3_TEMP_DIR, vm_name, ts);


    gchar *q_snap = g_shell_quote(snap_full);
    gchar *q_tmp  = g_shell_quote(tmp_file);
    gchar *send_cmd = g_strdup_printf("zfs send %s | gzip -1 > %s", q_snap, q_tmp);
    g_free(q_snap);
    g_free(q_tmp);

    const gchar *sh_argv[] = {"/bin/sh", "-c", send_cmd, NULL};
    stderr_buf = nullptr;
    local_err = nullptr;

    gboolean ok = pcv_spawn_sync(sh_argv, NULL, &stderr_buf, &local_err);
    g_free(send_cmd);

    if (!ok) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "ZFS send+gzip failed: %s",
                    local_err ? local_err->message
                              : (stderr_buf ? stderr_buf : "unknown"));
        g_free(stderr_buf);
        if (local_err) g_error_free(local_err);

        g_unlink(tmp_file);
        g_free(tmp_file);
        g_free(snap_name);
        g_free(snap_full);
        _vm_backup_unlock(vm_name);
        return FALSE;
    }
    g_free(stderr_buf);
    if (local_err) { g_error_free(local_err); local_err = nullptr; }


    struct stat st;
    gint64 file_size = 0;
    if (stat(tmp_file, &st) == 0) {
        file_size = (gint64)st.st_size;
    }

    PCV_LOG_INFO(BACKUP_LOG_DOM, "S3 backup: stream created %s (%" G_GINT64_FORMAT " bytes)",
                 tmp_file, file_size);


    gchar **s3_env = _s3_build_env(NULL);

    for (gchar **e = s3_env; e && *e; e++) {
        gchar *eq = strchr(*e, '=');
        if (eq) {
            gchar *key = g_strndup(*e, (gsize)(eq - *e));
            g_setenv(key, eq + 1, TRUE);
            g_free(key);
        }
    }
    g_strfreev(s3_env);


    gchar *s3_data_key = g_strdup_printf("%s%s/%s/backup.zfs.gz", prefix, vm_name, ts);
    ok = _s3_upload_file(endpoint, bucket, s3_data_key, tmp_file,
                          "application/gzip", error);
    g_free(s3_data_key);

    if (!ok) {
        g_unlink(tmp_file);
        g_free(tmp_file);
        g_free(snap_name);
        g_free(snap_full);
        _vm_backup_unlock(vm_name);
        return FALSE;
    }

    PCV_LOG_INFO(BACKUP_LOG_DOM, "S3 backup: data uploaded for %s", vm_name);


    gchar *meta_file = g_strdup_printf("%s/pcv-s3-%s-%s-meta.json",
                                        S3_TEMP_DIR, vm_name, ts);
    {
        JsonBuilder *b = json_builder_new();
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "vm_name");
        json_builder_add_string_value(b, vm_name);
        json_builder_set_member_name(b, "snapshot");
        json_builder_add_string_value(b, snap_name);
        json_builder_set_member_name(b, "timestamp");
        json_builder_add_string_value(b, ts);
        json_builder_set_member_name(b, "size_bytes");
        json_builder_add_int_value(b, file_size);
        json_builder_set_member_name(b, "compression");
        json_builder_add_string_value(b, "gzip");
        json_builder_set_member_name(b, "pool");
        json_builder_add_string_value(b, pool);
        json_builder_end_object(b);

        JsonGenerator *gen = json_generator_new();
        json_generator_set_pretty(gen, TRUE);
        JsonNode *root = json_builder_get_root(b);
        json_generator_set_root(gen, root);

        GError *write_err = nullptr;
        json_generator_to_file(gen, meta_file, &write_err);
        if (write_err) g_error_free(write_err);

        json_node_free(root);
        g_object_unref(gen);
        g_object_unref(b);
    }

    gchar *s3_meta_key = g_strdup_printf("%s%s/%s/metadata.json", prefix, vm_name, ts);
    GError *meta_err = nullptr;
    _s3_upload_file(endpoint, bucket, s3_meta_key, meta_file,
                     "application/json", &meta_err);
    if (meta_err) {
        PCV_LOG_WARN(BACKUP_LOG_DOM, "S3 metadata upload warning: %s", meta_err->message);
        g_error_free(meta_err);
    }
    g_free(s3_meta_key);


    g_unlink(tmp_file);
    g_unlink(meta_file);

    PCV_LOG_INFO(BACKUP_LOG_DOM,
                 "S3 backup complete: vm=%s snap=%s bucket=%s size=%" G_GINT64_FORMAT,
                 vm_name, snap_name, bucket, file_size);

    g_free(tmp_file);
    g_free(meta_file);
    g_free(snap_name);
    g_free(snap_full);
    _vm_backup_unlock(vm_name);
    return TRUE;
}








JsonObject *
pcv_snapshot_schedule_status(void)
{
    JsonObject *result = json_object_new();


    gboolean snap_enabled = g_strcmp0(
        pcv_config_get_string("snapshot", "enabled", "true"), "true") == 0;
    gint default_interval = pcv_config_get_int("snapshot", "interval_hours", 24);
    gint default_retention = pcv_config_get_int("snapshot", "retention_count", 7);
    const gchar *default_prefix = pcv_config_get_string("snapshot", "name_prefix", "pcv-auto-");

    json_object_set_boolean_member(result, "enabled", snap_enabled);
    json_object_set_int_member(result, "default_interval_hours", default_interval);
    json_object_set_int_member(result, "default_retention_count", default_retention);
    json_object_set_string_member(result, "name_prefix", default_prefix);
    json_object_set_int_member(result, "check_interval_sec", CHECK_INTERVAL);


    JsonArray *arr = json_array_new();
    g_mutex_lock(&g_policy_mutex);
    guint policy_count = g_policies ? g_policies->len : 0;

    for (guint i = 0; i < policy_count; i++) {
        PcvBackupPolicy *p = g_ptr_array_index(g_policies, i);

        JsonObject *po = json_object_new();
        json_object_set_string_member(po, "vm_name", p->vm_name);
        json_object_set_boolean_member(po, "enabled", p->enabled);
        json_object_set_int_member(po, "interval_hours", p->interval_hours);
        json_object_set_int_member(po, "retention_count", p->retention_count);


        if (g_strcmp0(p->vm_name, "*") != 0) {
            GPtrArray *snaps = _list_auto_snapshots(p->vm_name);
            json_object_set_int_member(po, "snapshot_count", (gint64)snaps->len);

            if (snaps->len > 0) {
                const gchar *newest = g_ptr_array_index(snaps, snaps->len - 1);
                json_object_set_string_member(po, "last_snapshot", newest);

                time_t last_t = _parse_snap_time(newest);
                if (last_t > 0) {
                    time_t next_due = last_t + (time_t)p->interval_hours * 3600;
                    json_object_set_int_member(po, "next_due_epoch", (gint64)next_due);
                }
            } else {
                json_object_set_null_member(po, "last_snapshot");
                json_object_set_int_member(po, "next_due_epoch", 0);
            }

            g_ptr_array_unref(snaps);
        } else {
            json_object_set_string_member(po, "scope", "all_vms");
        }

        json_array_add_object_element(arr, po);
    }

    g_mutex_unlock(&g_policy_mutex);

    json_object_set_int_member(result, "policy_count", (gint64)policy_count);
    json_object_set_array_member(result, "policies", arr);

    return result;
}
