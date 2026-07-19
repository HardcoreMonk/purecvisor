
#include "modules/storage/zfs_driver.h"
#include <string.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <time.h>
#include "../../utils/pcv_spawn.h"
#include "../../utils/pcv_zfs_lock.h"
#include "../../utils/pcv_config.h"
#include "../../utils/pcv_log.h"
#include "../audit/pcv_audit.h"
#include "../daemons/prometheus_exporter.h"
#if PCV_CLUSTER_ENABLED
#include "../cluster/cluster_manager.h"
#include "../cluster/etcd_client.h"
#endif

#if PCV_CLUSTER_ENABLED
static gdouble
_elapsed_ms_since(gint64 start_us)
{
    return (gdouble)(g_get_monotonic_time() - start_us) / 1000.0;
}

static gboolean
_zfs_acquire_inflight_lock_with_metrics(PcvEtcdClient *etcd,
                                        const gchar *pool_name,
                                        const gchar *node_name,
                                        const gchar *op,
                                        gint ttl_sec)
{
    gint64 start_us = g_get_monotonic_time();
    if (!etcd) {
        pcv_prom_zfs_inflight_lock_observe(pool_name, op, "error", 0.0);
        return FALSE;
    }

    GError *dist_err = NULL;
    if (pcv_etcd_acquire_inflight_lock(etcd, pool_name, node_name,
                                       op, ttl_sec, &dist_err)) {
        pcv_prom_zfs_inflight_lock_observe(pool_name, op, "ok",
                                           _elapsed_ms_since(start_us));
        return TRUE;
    }

    gboolean first_error = (dist_err != NULL);
    if (dist_err) g_error_free(dist_err);

    g_usleep(5 * G_USEC_PER_SEC);
    GError *retry_err = NULL;
    if (pcv_etcd_acquire_inflight_lock(etcd, pool_name, node_name,
                                       op, ttl_sec, &retry_err)) {
        pcv_prom_zfs_inflight_lock_observe(pool_name, op, "ok",
                                           _elapsed_ms_since(start_us));
        return TRUE;
    }

    pcv_prom_zfs_inflight_lock_observe(pool_name, op,
        (first_error || retry_err) ? "error" : "busy",
        _elapsed_ms_since(start_us));
    if (retry_err) g_error_free(retry_err);
    return FALSE;
}
#endif

typedef struct {
    GCancellable *cancellable;
    guint timeout_id;
} ZfsOpCtx;

static void zfs_op_ctx_free(gpointer data) {
    if (!data) return;
    ZfsOpCtx *ctx = (ZfsOpCtx *)data;

    if (ctx->timeout_id > 0) {
        g_source_remove(ctx->timeout_id);
    }
    if (ctx->cancellable) {
        g_object_unref(ctx->cancellable);
    }
    g_free(ctx);
}

static gboolean on_zfs_timeout(gpointer user_data) {
    GTask *task = G_TASK(user_data);
    ZfsOpCtx *ctx = g_task_get_task_data(task);

    g_warning("[ZFS Driver] Process hang detected! Sending kill signal...");
    g_cancellable_cancel(ctx->cancellable);
    ctx->timeout_id = 0;

    return G_SOURCE_REMOVE;
}

static void on_zfs_command_ready(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GSubprocess *proc = G_SUBPROCESS(source_object);
    GTask *task = G_TASK(user_data);
    ZfsOpCtx *ctx = g_task_get_task_data(task);
    GError *error = NULL;

    gchar *stdout_buf = NULL;
    gchar *stderr_buf = NULL;

    g_subprocess_communicate_utf8_finish(proc, res, &stdout_buf, &stderr_buf, &error);

    if (ctx->timeout_id > 0) {
        g_source_remove(ctx->timeout_id);
        ctx->timeout_id = 0;
    }

    if (error != NULL) {

        g_task_return_error(task, error);
    } else if (!g_subprocess_get_successful(proc)) {

        gchar *clean_err = stderr_buf ? g_strstrip(g_strdup(stderr_buf)) : g_strdup("Unknown ZFS failure");
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "ZFS Error: %s", clean_err);
        g_free(clean_err);
    } else {
        g_task_return_boolean(task, TRUE);
    }

    g_free(stdout_buf);
    g_free(stderr_buf);
    g_object_unref(task);
}

static void
_zfs_async_command(const gchar * const *argv,
                   GCancellable        *cancellable,
                   GAsyncReadyCallback  callback,
                   gpointer             user_data)
{
    GTask *task = g_task_new(NULL, cancellable, callback, user_data);
    ZfsOpCtx *ctx = g_new0(ZfsOpCtx, 1);
    ctx->cancellable = cancellable ? g_object_ref(cancellable) : g_cancellable_new();
    g_task_set_task_data(task, ctx, zfs_op_ctx_free);

    GError *error = NULL;
    GSubprocess *proc = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDERR_PIPE, &error);

    if (!proc || error) {
        if (!error)
            g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to spawn ZFS process");
        g_task_return_error(task, error);
        g_object_unref(task);
        return;
    }

    ctx->timeout_id = g_timeout_add_seconds(30, on_zfs_timeout, task);
    g_subprocess_communicate_utf8_async(proc, NULL, ctx->cancellable, on_zfs_command_ready, task);
    g_object_unref(proc);
}

void purecvisor_zfs_snapshot_create_async(const gchar *pool_name, const gchar *vm_name, const gchar *snap_name, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    gchar *target = g_strdup_printf("%s/vms/%s@%s", pool_name, vm_name, snap_name);
    const gchar *argv[] = {"zfs", "snapshot", target, NULL};
    _zfs_async_command((const gchar * const *)argv, cancellable, callback, user_data);
    g_free(target);
}

gboolean purecvisor_zfs_snapshot_create_finish(GAsyncResult *res, GError **error) {
    return g_task_propagate_boolean(G_TASK(res), error);
}

void purecvisor_zfs_snapshot_rollback_async(const gchar *pool_name, const gchar *vm_name, const gchar *snap_name, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    gchar *target = g_strdup_printf("%s/vms/%s@%s", pool_name, vm_name, snap_name);
    const gchar *argv[] = {"zfs", "rollback", "-r", target, NULL};
    _zfs_async_command((const gchar * const *)argv, cancellable, callback, user_data);
    g_free(target);
}

gboolean purecvisor_zfs_snapshot_rollback_finish(GAsyncResult *res, GError **error) {
    return g_task_propagate_boolean(G_TASK(res), error);
}

void purecvisor_zfs_snapshot_delete_async(const gchar *pool_name, const gchar *vm_name, const gchar *snap_name, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    gchar *target = g_strdup_printf("%s/vms/%s@%s", pool_name, vm_name, snap_name);
    const gchar *argv[] = {"zfs", "destroy", target, NULL};
    _zfs_async_command((const gchar * const *)argv, cancellable, callback, user_data);
    g_free(target);
}

gboolean purecvisor_zfs_snapshot_delete_finish(GAsyncResult *res, GError **error) {
    return g_task_propagate_boolean(G_TASK(res), error);
}

static void on_zfs_list_ready(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GSubprocess *proc = G_SUBPROCESS(source_object);
    GTask *task = G_TASK(user_data);
    ZfsOpCtx *ctx = g_task_get_task_data(task);
    GError *error = NULL;

    gchar *stdout_buf = NULL;
    gchar *stderr_buf = NULL;

    g_subprocess_communicate_utf8_finish(proc, res, &stdout_buf, &stderr_buf, &error);

    if (ctx->timeout_id > 0) {
        g_source_remove(ctx->timeout_id);
        ctx->timeout_id = 0;
    }

    if (error) {
        g_task_return_error(task, error);
    } else if (!g_subprocess_get_successful(proc)) {
        gchar *clean_err = stderr_buf ? g_strstrip(g_strdup(stderr_buf)) : g_strdup("Unknown error");
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "ZFS List Error: %s", clean_err);
        g_free(clean_err);
    } else {
        GPtrArray *snapshots = g_ptr_array_new_with_free_func(g_free);
        if (stdout_buf) {

            gchar **lines = g_strsplit(stdout_buf, "\n", -1);
            for (gint i = 0; lines[i] != NULL; i++) {
                gchar *line = g_strstrip(lines[i]);
                if (strlen(line) > 0) {

                    gchar *at_sign = g_strrstr(line, "@");
                    if (at_sign && *(at_sign + 1) != '\0') g_ptr_array_add(snapshots, g_strdup(at_sign + 1));
                }
            }
            g_strfreev(lines);
        }
        g_task_return_pointer(task, snapshots, (GDestroyNotify)g_ptr_array_unref);
    }

    g_free(stdout_buf);
    g_free(stderr_buf);
    g_object_unref(task);
}

void purecvisor_zfs_snapshot_list_async(const gchar *pool_name, const gchar *vm_name, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    GTask *task = g_task_new(NULL, cancellable, callback, user_data);
    ZfsOpCtx *ctx = g_new0(ZfsOpCtx, 1);
    ctx->cancellable = cancellable ? g_object_ref(cancellable) : g_cancellable_new();
    g_task_set_task_data(task, ctx, zfs_op_ctx_free);

    GError *error = NULL;
    gchar *target = g_strdup_printf("%s/vms/%s", pool_name, vm_name);
    const gchar *argv[] = {"zfs", "list", "-t", "snapshot", "-H", "-o", "name", target, NULL};

    GSubprocess *proc = g_subprocess_newv((const gchar * const *)argv,
                                          G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE,
                                          &error);
    g_free(target);

    if (error) { g_task_return_error(task, error); g_object_unref(task); return; }

    ctx->timeout_id = g_timeout_add_seconds(30, on_zfs_timeout, task);
    g_subprocess_communicate_utf8_async(proc, NULL, ctx->cancellable, on_zfs_list_ready, task);
    g_object_unref(proc);
}

GPtrArray* purecvisor_zfs_snapshot_list_finish(GAsyncResult *res, GError **error) {
    return g_task_propagate_pointer(G_TASK(res), error);
}

gboolean purecvisor_zfs_create_volume(const gchar *pool_name, const gchar *vm_name, const gchar *size_str, GError **error)
{
    gboolean success;
    gchar *target_dataset = g_strdup_printf("%s/%s", pool_name, vm_name);
    gchar *stderr_buf = NULL;

    if (!pcv_zfs_pool_lock(pool_name, "create", 30000, error)) {
        g_free(target_dataset);
        return FALSE;
    }

#if PCV_CLUSTER_ENABLED
    PcvEtcdClient *etcd = pcv_cluster_get_etcd();
    const gchar *node_name = pcv_config_get_string("cluster", "node_name", "local");

    gint size_gb = 0;
    if (size_str && *size_str) {
        size_gb = (gint)g_ascii_strtoll(size_str, NULL, 10);
        if (size_gb < 1) size_gb = 1;
    }
    gint dyn_ttl = pcv_etcd_compute_inflight_ttl("create", size_gb);
    gboolean dist_locked = _zfs_acquire_inflight_lock_with_metrics(
        etcd, pool_name, node_name, "create", dyn_ttl);

#endif

    const gchar *argv[] = {"zfs", "create", "-s", "-V", size_str, target_dataset, NULL};

    success = pcv_spawn_sync(argv, NULL, &stderr_buf, error);
    if (!success && error && !*error) {
        gchar *clean_err = stderr_buf ? g_strstrip(g_strdup(stderr_buf)) : g_strdup("Unknown ZFS creation error");
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "ZFS Create Error: %s", clean_err);
        g_free(clean_err);
    }

#if PCV_CLUSTER_ENABLED
    if (dist_locked && etcd) {
        pcv_etcd_release_inflight_lock(etcd, pool_name, NULL);
    }
#endif
    pcv_zfs_pool_unlock(pool_name);
    g_free(stderr_buf);
    g_free(target_dataset);
    return success;
}

gboolean purecvisor_zfs_destroy_volume(const gchar *pool_name, const gchar *vm_name, GError **error)
{
    gboolean success;
    gchar *target_dataset = g_strdup_printf("%s/%s", pool_name, vm_name);
    gchar *stderr_buf = NULL;

    if (!pcv_zfs_pool_lock(pool_name, "destroy", 30000, error)) {
        g_free(target_dataset);
        return FALSE;
    }

#if PCV_CLUSTER_ENABLED
    PcvEtcdClient *etcd = pcv_cluster_get_etcd();
    const gchar *node_name = pcv_config_get_string("cluster", "node_name", "local");
    gint dyn_ttl = pcv_etcd_compute_inflight_ttl("destroy", 0);
    gboolean dist_locked = _zfs_acquire_inflight_lock_with_metrics(
        etcd, pool_name, node_name, "destroy", dyn_ttl);

#endif

    const gchar *argv[] = {"zfs", "destroy", "-r", target_dataset, NULL};

    success = pcv_spawn_sync(argv, NULL, &stderr_buf, error);
    if (!success && error && !*error) {
        gchar *clean_err = stderr_buf ? g_strstrip(g_strdup(stderr_buf)) : g_strdup("Unknown ZFS destroy error");
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "ZFS Destroy Error: %s", clean_err);
        g_free(clean_err);
    }

#if PCV_CLUSTER_ENABLED
    if (dist_locked && etcd) {
        pcv_etcd_release_inflight_lock(etcd, pool_name, NULL);
    }
#endif
    pcv_zfs_pool_unlock(pool_name);
    g_free(stderr_buf);
    g_free(target_dataset);
    return success;
}

gboolean purecvisor_zfs_create_pool(const gchar *name, const gchar *vdev_type,
                                     const gchar **disks, gint n_disks,
                                     const gchar *compression, GError **error)
{
    const gchar *comp = (compression && *compression) ? compression : "lz4";
    gchar *comp_opt = g_strdup_printf("compression=%s", comp);

    GPtrArray *argv = g_ptr_array_new();
    g_ptr_array_add(argv, (gchar *)"zpool");
    g_ptr_array_add(argv, (gchar *)"create");
    g_ptr_array_add(argv, (gchar *)"-f");
    g_ptr_array_add(argv, (gchar *)"-o");
    g_ptr_array_add(argv, (gchar *)"ashift=12");
    g_ptr_array_add(argv, (gchar *)"-O");
    g_ptr_array_add(argv, comp_opt);
    g_ptr_array_add(argv, (gchar *)"-O");
    g_ptr_array_add(argv, (gchar *)"atime=off");
    g_ptr_array_add(argv, (gchar *)name);
    if (vdev_type && *vdev_type)
        g_ptr_array_add(argv, (gchar *)vdev_type);
    for (gint i = 0; i < n_disks; i++)
        g_ptr_array_add(argv, (gchar *)disks[i]);
    g_ptr_array_add(argv, NULL);

    gchar *std_err = NULL;
    gboolean ok = pcv_spawn_sync((const gchar * const *)argv->pdata, NULL, &std_err, error);
    if (!ok && error && !*error) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "zpool create failed: %s", std_err ? std_err : "unknown");
    }
    g_free(std_err);
    g_free(comp_opt);
    g_ptr_array_free(argv, TRUE);
    return ok;
}

gboolean purecvisor_zfs_destroy_pool(const gchar *name, GError **error)
{
    const gchar *argv[] = {"zpool", "destroy", "-f", name, NULL};
    gchar *std_err = NULL;

    gboolean ok = pcv_spawn_sync(argv, NULL, &std_err, error);
    if (!ok && error && !*error) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "zpool destroy failed: %s", std_err ? std_err : "unknown");
    }
    g_free(std_err);
    return ok;
}

gboolean purecvisor_zfs_scrub_pool(const gchar *name, GError **error)
{
    const gchar *argv[] = {"zpool", "scrub", name, NULL};
    gchar *std_err = NULL;

    gboolean ok = pcv_spawn_sync(argv, NULL, &std_err, error);
    if (!ok && error && !*error) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "zpool scrub failed: %s", std_err ? std_err : "unknown");
    }
    g_free(std_err);
    return ok;
}

gboolean purecvisor_zfs_clone_volume(const gchar *pool_name, const gchar *source_vm,
                                      const gchar *snap_name, const gchar *clone_vm,
                                      GError **error)
{
    gchar *snap_path  = g_strdup_printf("%s/%s@%s", pool_name, source_vm, snap_name);
    gchar *clone_path = g_strdup_printf("%s/%s", pool_name, clone_vm);

    const gchar *argv[] = {"zfs", "clone", snap_path, clone_path, NULL};
    gchar *stderr_buf = NULL;

    gboolean ok = pcv_spawn_sync(argv, NULL, &stderr_buf, error);
    if (!ok && error && !*error) {
        gchar *clean_err = stderr_buf ? g_strstrip(g_strdup(stderr_buf)) : g_strdup("Unknown ZFS clone error");
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "ZFS Clone Error: %s", clean_err);
        g_free(clean_err);
    }

    g_free(stderr_buf);
    g_free(snap_path);
    g_free(clone_path);
    return ok;
}

gboolean purecvisor_zfs_full_copy(const gchar *pool_name, const gchar *source_vm,
                                   const gchar *snap_name, const gchar *clone_vm,
                                   GError **error)
{
    gchar *snap_path  = g_strdup_printf("%s/%s@%s", pool_name, source_vm, snap_name);
    gchar *clone_path = g_strdup_printf("%s/%s", pool_name, clone_vm);
    const gchar *send_argv[] = {"zfs", "send", snap_path, NULL};
    const gchar *recv_argv[] = {"zfs", "recv", clone_path, NULL};
    gchar *stderr_buf = NULL;

    gboolean result = pcv_spawn_pipe_sync(send_argv, recv_argv,
                                          NULL, &stderr_buf, error);
    if (!result && error && !*error) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "ZFS full copy failed: %s",
                    (stderr_buf && *stderr_buf) ? stderr_buf : "unknown");
    }

    g_free(stderr_buf);
    g_free(snap_path);
    g_free(clone_path);
    return result;
}

static gint
_parse_error_count(const gchar *s)
{
    if (!s || !*s) return 0;
    gchar *endp = NULL;
    glong val = strtol(s, &endp, 10);
    if (endp == s) return 0;
    return (gint)val;
}

gboolean
pcv_zfs_pool_health(const gchar *pool_name, ZfsPoolHealth *out)
{
    if (!pool_name || !out) return FALSE;
    memset(out, 0, sizeof(ZfsPoolHealth));
    out->scrub_age_sec = -1;
    g_strlcpy(out->state, "UNKNOWN", sizeof(out->state));

    const gchar *argv[] = {"zpool", "status", "-p", pool_name, NULL};
    gchar *stdout_buf = NULL;
    gchar *stderr_buf = NULL;
    GError *err = NULL;

    if (!pcv_spawn_sync_timeout(argv, &stdout_buf, &stderr_buf, 10, &err)) {
        gboolean timed_out = g_error_matches(err, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
        g_free(stdout_buf);
        g_free(stderr_buf);
        if (err) g_error_free(err);
        if (timed_out) {

            g_strlcpy(out->state, "SUSPENDED", sizeof(out->state));
            return TRUE;
        }
        return FALSE;
    }

    if (stdout_buf) {
        gchar **lines = g_strsplit(stdout_buf, "\n", -1);
        for (gint i = 0; lines[i]; i++) {
            gchar *line = g_strstrip(lines[i]);

            if (g_str_has_prefix(line, "state:")) {
                gchar *val = g_strstrip(line + 6);
                g_strlcpy(out->state, val, sizeof(out->state));
            }

            if (g_str_has_prefix(line, "errors:")) {
                gchar *val = g_strstrip(line + 7);
                if (strstr(val, "No known")) {
                    out->errors_read  = 0;
                    out->errors_write = 0;
                    out->errors_cksum = 0;
                }
            }

            if (strstr(line, pool_name) && !g_str_has_prefix(line, "pool:")) {
                gchar **tokens = g_strsplit_set(line, " \t", -1);

                GPtrArray *parts = g_ptr_array_new();
                for (gint t = 0; tokens[t]; t++) {
                    if (tokens[t][0]) g_ptr_array_add(parts, tokens[t]);
                }

                if (parts->len >= 5) {
                    const gchar *name_tok = g_ptr_array_index(parts, 0);
                    if (g_strcmp0(name_tok, pool_name) == 0) {
                        out->errors_read  = _parse_error_count(g_ptr_array_index(parts, 2));
                        out->errors_write = _parse_error_count(g_ptr_array_index(parts, 3));
                        out->errors_cksum = _parse_error_count(g_ptr_array_index(parts, 4));
                    }
                }
                g_ptr_array_free(parts, TRUE);
                g_strfreev(tokens);
            }

            if (g_str_has_prefix(line, "scan:")) {
                if (strstr(line, "scrub in progress")) {
                    out->scrub_running = TRUE;
                    out->scrub_age_sec = 0;
                } else if (strstr(line, "scrub repaired") || strstr(line, "scrub")) {

                    const gchar *on_str = strstr(line, " on ");
                    if (on_str) {
                        on_str += 4;
                        struct tm tm_val;
                        memset(&tm_val, 0, sizeof(tm_val));

                        gchar *rest = strptime(on_str, "%a %b %d %H:%M:%S %Y", &tm_val);
                        if (rest) {
                            tm_val.tm_isdst = -1;
                            time_t scrub_time = mktime(&tm_val);
                            if (scrub_time != (time_t)-1) {
                                out->scrub_age_sec = (gint64)difftime(time(NULL), scrub_time);
                                if (out->scrub_age_sec < 0) out->scrub_age_sec = 0;
                            }
                        }
                    }
                }
            }
        }
        g_strfreev(lines);
    }

    g_free(stdout_buf);
    g_free(stderr_buf);
    if (err) g_error_free(err);

    const gchar *cap_argv[] = {"zpool", "list", "-Hp", "-o", "capacity", pool_name, NULL};
    gchar *cap_out = NULL;
    err = NULL;

    if (pcv_spawn_sync_timeout(cap_argv, &cap_out, NULL, 10, &err) && cap_out) {
        gchar *trimmed = g_strstrip(cap_out);
        out->capacity_pct = g_ascii_strtod(trimmed, NULL);
    }
    g_free(cap_out);
    if (err) g_error_free(err);

    return TRUE;
}

#define ZFS_RECOVER_LOG_DOM "zfs_recover"

gdouble
pcv_zfs_pool_state_metric_val(const gchar *state)
{
    if (!state) return 0.0;
    if (g_strcmp0(state, "DEGRADED")  == 0) return 1.0;
    if (g_strcmp0(state, "FAULTED")   == 0) return 2.0;
    if (g_strcmp0(state, "UNAVAIL")   == 0) return 3.0;
    if (g_strcmp0(state, "SUSPENDED") == 0) return 4.0;
    return 0.0;
}

gboolean
pcv_zfs_recover_guard_allow(ZfsRecoverGuard *g, gint64 now_us,
                            gint64 window_us, gint max_attempts)
{
    if (!g) return FALSE;

    if (g->window_start_us == 0 || (now_us - g->window_start_us) >= window_us) {
        g->window_start_us = now_us;
        g->attempts = 0;
    }
    if (g->attempts >= max_attempts) {
        return FALSE;
    }
    g->attempts++;
    return TRUE;
}

static gboolean
_zfs_pool_vdev_path(const gchar *pool_name, gchar *out, gsize outlen)
{
    if (!pool_name || !out || outlen == 0) return FALSE;
    out[0] = '\0';

    const gchar *cfg_dev = pcv_config_get_string("storage", "pool_device", "");
    if (cfg_dev && *cfg_dev) {
        g_strlcpy(out, cfg_dev, outlen);
        return TRUE;
    }

    const gchar *argv[] = {"zpool", "status", "-P", pool_name, NULL};
    gchar *sout = NULL;
    GError *err = NULL;
    gboolean spawn_ok = pcv_spawn_sync_timeout(argv, &sout, NULL, 10, &err);
    if (err) g_error_free(err);
    if (!spawn_ok || !sout) {
        g_free(sout);
        return FALSE;
    }

    gboolean found = FALSE;
    gchar **lines = g_strsplit(sout, "\n", -1);
    for (gint i = 0; lines[i] && !found; i++) {
        gchar **tok = g_strsplit_set(g_strstrip(lines[i]), " \t", -1);
        for (gint t = 0; tok[t] && !found; t++) {
            if (g_str_has_prefix(tok[t], "/dev/")) {
                g_strlcpy(out, tok[t], outlen);
                found = TRUE;
            }
        }
        g_strfreev(tok);
    }
    g_strfreev(lines);
    g_free(sout);
    return found;
}

static gboolean
_zfs_vdev_readable(const gchar *dev_path)
{
    if (!dev_path || !*dev_path) return FALSE;

    gchar if_arg[600];
    g_snprintf(if_arg, sizeof if_arg, "if=%s", dev_path);
    const gchar *argv[] = {"dd", if_arg, "bs=4096", "count=1", "of=/dev/null", NULL};
    GError *err = NULL;
    gboolean ok = pcv_spawn_sync_timeout(argv, NULL, NULL, 8, &err);
    if (err) g_error_free(err);
    return ok;
}

PcvZfsRecoverResult
pcv_zfs_pool_recover_suspended(const gchar *pool_name, ZfsRecoverGuard *guard)
{
    if (!pool_name) return PCV_ZFS_RECOVER_NOT_SUSPENDED;

    const gchar *ar = pcv_config_get_string("storage", "auto_pool_recover", "true");
    gboolean enabled = !(g_ascii_strcasecmp(ar, "false") == 0 || g_strcmp0(ar, "0") == 0);
    if (!enabled) {
        PCV_LOG_WARN(ZFS_RECOVER_LOG_DOM,
                     "pool '%s' SUSPENDED — auto_pool_recover=false, clear 생략(수동복구 필요)",
                     pool_name);
        pcv_audit_log("system", "zpool.auto_recover", pool_name, "disabled", 0, 0, "local");
        return PCV_ZFS_RECOVER_DISABLED;
    }

    gchar dev[600] = {0};
    gboolean have_dev = _zfs_pool_vdev_path(pool_name, dev, sizeof dev);
    if (!have_dev || !_zfs_vdev_readable(dev)) {
        PCV_LOG_WARN(ZFS_RECOVER_LOG_DOM,
                     "pool '%s' SUSPENDED — vdev %s 읽기불가 → clear 생략(수동개입 필요)",
                     pool_name, have_dev ? dev : "(미해석)");
        pcv_audit_log("system", "zpool.auto_recover", pool_name,
                      have_dev ? "device-unreadable" : "device-missing", 0, 0, "local");
        return PCV_ZFS_RECOVER_DEV_UNREADABLE;
    }

    gint64 now_us = g_get_monotonic_time();
    const gint64 window_us = (gint64)3600 * G_USEC_PER_SEC;
    const gint   max_attempts = 3;
    if (guard && !pcv_zfs_recover_guard_allow(guard, now_us, window_us, max_attempts)) {
        PCV_LOG_WARN(ZFS_RECOVER_LOG_DOM,
                     "pool '%s' SUSPENDED — 자동복구 상한(%d/1h) 초과 → clear 중단(flapping)",
                     pool_name, max_attempts);
        pcv_audit_log("system", "zpool.auto_recover", pool_name, "circuit-open", 0, 0, "local");
        return PCV_ZFS_RECOVER_CB_TRIPPED;
    }

    const gchar *argv[] = {"zpool", "clear", pool_name, NULL};
    GError *err = NULL;
    gboolean ok = pcv_spawn_sync_timeout(argv, NULL, NULL, 40, &err);
    if (ok) {
        PCV_LOG_INFO(ZFS_RECOVER_LOG_DOM, "pool '%s' SUSPENDED — zpool clear 성공", pool_name);
        pcv_audit_log("system", "zpool.clear", pool_name, "ok", 0, 0, "local");
    } else {
        PCV_LOG_WARN(ZFS_RECOVER_LOG_DOM, "pool '%s' SUSPENDED — zpool clear 실패: %s",
                     pool_name, (err && err->message) ? err->message : "unknown");
        pcv_audit_log("system", "zpool.clear", pool_name, "failed", 0, 0, "local");
    }
    if (err) g_error_free(err);
    return ok ? PCV_ZFS_RECOVER_CLEARED : PCV_ZFS_RECOVER_CLEAR_FAILED;
}

JsonObject *
pcv_zfs_pool_health_to_json(const ZfsPoolHealth *h)
{
    JsonObject *obj = json_object_new();
    if (!h) return obj;

    json_object_set_string_member(obj, "state", h->state);
    json_object_set_int_member(obj, "errors_read", h->errors_read);
    json_object_set_int_member(obj, "errors_write", h->errors_write);
    json_object_set_int_member(obj, "errors_cksum", h->errors_cksum);
    json_object_set_int_member(obj, "scrub_age_seconds", h->scrub_age_sec);
    json_object_set_boolean_member(obj, "scrub_running", h->scrub_running);
    json_object_set_double_member(obj, "capacity_percent", h->capacity_pct);

    const gchar *health = "healthy";
    if (g_strcmp0(h->state, "FAULTED") == 0 || g_strcmp0(h->state, "UNAVAIL") == 0)
        health = "critical";
    else if (g_strcmp0(h->state, "DEGRADED") == 0)
        health = "degraded";
    else if (h->errors_read > 0 || h->errors_write > 0 || h->errors_cksum > 0)
        health = "degraded";
    else if (h->capacity_pct > 85.0)
        health = "warning";
    json_object_set_string_member(obj, "health", health);

    return obj;
}

#define CAP_HISTORY_MAX  168

typedef struct {
    gint64 ts;
    gint64 used_bytes;
    gint64 total_bytes;
} CapacitySample;

static struct {
    CapacitySample samples[CAP_HISTORY_MAX];
    gint           count;
    gint           head;
    GMutex         mu;
    gboolean       mu_init;
} g_cap_hist = {0};

static void
_cap_hist_ensure_init(void)
{
    if (!g_cap_hist.mu_init) {
        g_mutex_init(&g_cap_hist.mu);
        g_cap_hist.mu_init = TRUE;
    }
}

void
pcv_zfs_capacity_record(const gchar *pool_name)
{
    if (!pool_name) return;
    _cap_hist_ensure_init();

    const gchar *argv_used[] = {"zfs", "get", "-Hp", "-o", "value", "used", pool_name, NULL};
    const gchar *argv_avail[] = {"zfs", "get", "-Hp", "-o", "value", "available", pool_name, NULL};
    gchar *used_str = NULL, *avail_str = NULL;

    pcv_spawn_sync(argv_used, &used_str, NULL, NULL);
    pcv_spawn_sync(argv_avail, &avail_str, NULL, NULL);

    if (!used_str || !avail_str) {
        g_free(used_str);
        g_free(avail_str);
        return;
    }

    gint64 used = g_ascii_strtoll(g_strstrip(used_str), NULL, 10);
    gint64 avail = g_ascii_strtoll(g_strstrip(avail_str), NULL, 10);
    g_free(used_str);
    g_free(avail_str);

    if (used <= 0 && avail <= 0) return;

    CapacitySample s = {
        .ts = (gint64)time(NULL),
        .used_bytes = used,
        .total_bytes = used + avail
    };

    g_mutex_lock(&g_cap_hist.mu);
    g_cap_hist.samples[g_cap_hist.head] = s;
    g_cap_hist.head = (g_cap_hist.head + 1) % CAP_HISTORY_MAX;
    g_cap_hist.count++;
    g_mutex_unlock(&g_cap_hist.mu);
}

JsonObject *
pcv_zfs_pool_forecast(const gchar *pool_name)
{
    JsonObject *result = json_object_new();
    if (!pool_name) {
        json_object_set_string_member(result, "error", "pool_name required");
        return result;
    }
    _cap_hist_ensure_init();

    json_object_set_string_member(result, "pool", pool_name);

    const gchar *argv_used[] = {"zfs", "get", "-Hp", "-o", "value", "used", pool_name, NULL};
    const gchar *argv_avail[] = {"zfs", "get", "-Hp", "-o", "value", "available", pool_name, NULL};
    gchar *used_str = NULL, *avail_str = NULL;
    pcv_spawn_sync(argv_used, &used_str, NULL, NULL);
    pcv_spawn_sync(argv_avail, &avail_str, NULL, NULL);

    gint64 cur_used = 0, cur_total = 0;
    if (used_str) { cur_used = g_ascii_strtoll(g_strstrip(used_str), NULL, 10); g_free(used_str); }
    if (avail_str) {
        gint64 avail = g_ascii_strtoll(g_strstrip(avail_str), NULL, 10);
        cur_total = cur_used + avail;
        g_free(avail_str);
    }

    json_object_set_int_member(result, "used_bytes", cur_used);
    json_object_set_int_member(result, "total_bytes", cur_total);

    gdouble used_pct = (cur_total > 0) ? (gdouble)cur_used / cur_total * 100.0 : 0.0;
    json_object_set_double_member(result, "used_pct",
        (double)((gint)(used_pct * 100)) / 100.0);

    g_mutex_lock(&g_cap_hist.mu);
    gint n = (g_cap_hist.count < CAP_HISTORY_MAX) ? g_cap_hist.count : CAP_HISTORY_MAX;

    if (n < 2) {
        g_mutex_unlock(&g_cap_hist.mu);
        json_object_set_int_member(result, "history_points", n);
        json_object_set_string_member(result, "forecast", "insufficient data (need >= 2 hourly samples)");
        json_object_set_double_member(result, "daily_growth_bytes", 0.0);
        json_object_set_int_member(result, "days_to_full", -1);
        json_object_set_string_member(result, "predicted_full_date", "N/A");
        json_object_set_string_member(result, "alert_level", "ok");
        return result;
    }

    CapacitySample *buf = g_new(CapacitySample, (gsize)n);
    gint oldest_idx;
    if (g_cap_hist.count <= CAP_HISTORY_MAX)
        oldest_idx = 0;
    else
        oldest_idx = g_cap_hist.head;

    for (gint i = 0; i < n; i++) {
        gint idx = (oldest_idx + i) % CAP_HISTORY_MAX;
        buf[i] = g_cap_hist.samples[idx];
    }
    g_mutex_unlock(&g_cap_hist.mu);

    json_object_set_int_member(result, "history_points", n);

    gint64 t0 = buf[0].ts;
    gdouble sum_x = 0, sum_y = 0, sum_xy = 0, sum_x2 = 0;
    for (gint i = 0; i < n; i++) {
        gdouble x = (gdouble)(buf[i].ts - t0);
        gdouble y = (gdouble)buf[i].used_bytes;
        sum_x  += x;
        sum_y  += y;
        sum_xy += x * y;
        sum_x2 += x * x;
    }
    g_free(buf);

    gdouble denom = (gdouble)n * sum_x2 - sum_x * sum_x;
    gdouble slope_per_sec = 0.0;
    if (denom > 1e-6)
        slope_per_sec = ((gdouble)n * sum_xy - sum_x * sum_y) / denom;

    gdouble daily_growth = slope_per_sec * 86400.0;
    json_object_set_double_member(result, "daily_growth_bytes", daily_growth);

    gint64 remaining = cur_total - cur_used;
    gint days_to_full = -1;
    if (slope_per_sec > 1e-3 && remaining > 0)
        days_to_full = (gint)((gdouble)remaining / (slope_per_sec * 86400.0));

    json_object_set_int_member(result, "days_to_full", days_to_full);

    if (days_to_full > 0) {
        time_t full_time = time(NULL) + (time_t)(days_to_full * 86400);
        struct tm tm_val;
        gmtime_r(&full_time, &tm_val);
        gchar date_buf[32];
        strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", &tm_val);
        json_object_set_string_member(result, "predicted_full_date", date_buf);
    } else {
        json_object_set_string_member(result, "predicted_full_date",
            slope_per_sec <= 0 ? "not growing" : "N/A");
    }

    const gchar *alert = "ok";
    if (days_to_full > 0 && days_to_full < 7)
        alert = "critical";
    else if (days_to_full > 0 && days_to_full < 30)
        alert = "warn";
    json_object_set_string_member(result, "alert_level", alert);

    return result;
}

JsonObject *
purecvisor_zfs_pool_health_detail(const gchar *pool_name)
{
    JsonObject *result = json_object_new();

    const gchar *argv[] = {"zpool", "list", "-H", "-o",
        "name,health,allocated,size,free,fragmentation,capacity",
        pool_name, NULL};
    gchar *out = NULL;
    GError *err = NULL;
    if (pcv_spawn_sync(argv, &out, NULL, &err)) {
        if (out && *out) {
            gchar **fields = g_strsplit(g_strstrip(out), "\t", -1);
            gint nf = g_strv_length(fields);
            if (nf >= 7) {
                json_object_set_string_member(result, "name", fields[0]);
                json_object_set_string_member(result, "health", fields[1]);
                json_object_set_string_member(result, "allocated", fields[2]);
                json_object_set_string_member(result, "size", fields[3]);
                json_object_set_string_member(result, "free", fields[4]);
                json_object_set_string_member(result, "fragmentation", fields[5]);
                json_object_set_string_member(result, "capacity", fields[6]);
            }
            g_strfreev(fields);
        }
    } else {
        json_object_set_string_member(result, "error",
            err ? err->message : "zpool list failed");
        if (err) g_error_free(err);
    }
    g_free(out);

    const gchar *status_argv[] = {"zpool", "status", "-p", pool_name, NULL};
    gchar *status_out = NULL;
    if (pcv_spawn_sync(status_argv, &status_out, NULL, NULL) && status_out) {
        gboolean scrub_active = (strstr(status_out, "scrub in progress") != NULL);
        json_object_set_boolean_member(result, "scrub_in_progress", scrub_active);

        if (scrub_active) {
            gchar *pct = strstr(status_out, "done");
            if (pct) {

                gchar *scan = pct - 1;
                while (scan > status_out &&
                       (*scan == ' ' || *scan == '%' ||
                        g_ascii_isdigit(*scan) || *scan == '.'))
                    scan--;
                gchar progress_str[16] = {0};
                g_strlcpy(progress_str, scan + 1,
                          MIN((gsize)(pct - scan), sizeof(progress_str)));
                json_object_set_string_member(result, "scrub_progress",
                    g_strstrip(progress_str));
            }
        }
    }
    g_free(status_out);

    return result;
}

gboolean
purecvisor_zfs_promote(const gchar *clone_name)
{
    if (!clone_name || !*clone_name) return FALSE;
    const gchar *argv[] = {"zfs", "promote", clone_name, NULL};
    gchar *std_err = NULL;
    GError *err = NULL;
    gboolean ok = pcv_spawn_sync(argv, NULL, &std_err, &err);
    if (!ok) {
        g_warning("[ZFS] zfs promote '%s' failed: %s", clone_name,
                  err ? err->message : (std_err ? std_err : "unknown"));
        if (err) g_error_free(err);
    } else {
        g_message("[ZFS] Promoted clone '%s' to independent dataset", clone_name);
    }
    g_free(std_err);
    return ok;
}

gboolean
purecvisor_zfs_create_zvol_encrypted(const gchar *name, const gchar *size,
                                      const gchar *passphrase, GError **error)
{
    if (!name || !size || !passphrase || !*passphrase) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "name, size, and passphrase are required");
        return FALSE;
    }

    const gchar *argv[] = {
        "zfs", "create",
        "-V", size,
        "-o", "encryption=aes-256-gcm",
        "-o", "keyformat=passphrase",
        "-o", "keylocation=prompt",
        "-s",
        name, NULL
    };

    gchar *input = g_strdup_printf("%s\n%s\n", passphrase, passphrase);

    GSubprocess *proc = g_subprocess_newv(argv,
        G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE, error);
    if (!proc) {
        g_free(input);
        return FALSE;
    }

    gchar *stderr_out = NULL;
    gboolean ok = g_subprocess_communicate_utf8(proc, input, NULL, NULL, &stderr_out, error);
    gint exit_status = g_subprocess_get_exit_status(proc);
    g_free(input);
    g_object_unref(proc);

    if (!ok || exit_status != 0) {
        if (stderr_out && *stderr_out && error && !*error)
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", g_strstrip(stderr_out));
        g_free(stderr_out);
        return FALSE;
    }
    g_free(stderr_out);

    g_message("[ZFS] Created encrypted zvol: %s (aes-256-gcm)", name);
    return TRUE;
}

gboolean
purecvisor_zfs_check_snapshot_quota(const gchar *dataset, gint max_snapshots)
{
    if (max_snapshots <= 0) return TRUE;

    const gchar *argv[] = {"zfs", "list", "-H", "-t", "snapshot", "-o", "name", "-r", dataset, NULL};
    gchar *out = NULL;
    if (!pcv_spawn_sync(argv, &out, NULL, NULL) || !out) {
        g_free(out);
        return TRUE;
    }

    gchar *trimmed = g_strstrip(out);
    gint count = 0;
    if (trimmed[0] != '\0') {
        gchar **lines = g_strsplit(trimmed, "\n", -1);
        for (gchar **l = lines; *l && **l; l++) count++;
        g_strfreev(lines);
    }
    g_free(out);

    if (count >= max_snapshots) {
        g_warning("[ZFS] Snapshot quota exceeded for '%s': %d/%d",
                  dataset, count, max_snapshots);
        return FALSE;
    }
    return TRUE;
}
