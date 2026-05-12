































































#include "cloud_migration.h"
#include "../../utils/pcv_spawn.h"
#include "../../utils/pcv_log.h"
#include "../../utils/pcv_config.h"
#include "../../utils/pcv_validate.h"
#include "../audit/pcv_audit.h"
#include "../virt/vm_config_builder.h"

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <libvirt-gconfig/libvirt-gconfig.h>
#include <sqlite3.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>























































#define CLOUD_LOG "cloud_migration"


constexpr int CUTOVER_TIMEOUT_SEC = 7200;

constexpr int CUTOVER_SWEEP_INTERVAL_SEC = 300;


static void _ensure_cutover_sweeper(void);








extern gboolean pcv_aws_check_credentials(const gchar *region, GError **error);
extern gchar   *pcv_aws_export_image(const gchar *ami_id, const gchar *s3_bucket,
                                       const gchar *region, GError **error);
extern gint     pcv_aws_check_export_progress(const gchar *task_id, const gchar *region,
                                                gchar **out_status, gchar **out_s3_key,
                                                GError **error);
extern gboolean pcv_aws_s3_download(const gchar *s3_bucket, const gchar *s3_key,
                                      const gchar *local_dir, const gchar *region,
                                      GError **error);
extern gboolean pcv_aws_s3_upload(const gchar *local_path, const gchar *s3_bucket,
                                    const gchar *s3_prefix, const gchar *region,
                                    GError **error);
extern gchar   *pcv_aws_import_image(const gchar *s3_bucket, const gchar *s3_key,
                                       const gchar *region, const gchar *description,
                                       GError **error);
extern gint     pcv_aws_check_import_progress(const gchar *task_id, const gchar *region,
                                                gchar **out_status, gchar **out_ami_id,
                                                GError **error);
extern gchar   *pcv_disk_convert_raw_to_qcow2(const gchar *raw_path, const gchar *vm_name,
                                                 const gchar *output_dir, GError **error);
extern gchar   *pcv_disk_convert_qcow2_to_raw(const gchar *qcow2_path, const gchar *vm_name,
                                                 const gchar *output_dir, GError **error);
extern gchar   *pcv_disk_find_vm_disk(const gchar *vm_name, GError **error);
extern gboolean pcv_disk_inject_virtio(const gchar *disk_path, GError **error);
extern gboolean pcv_aws_stop_instance(const gchar *instance_id, const gchar *region, GError **error);
extern gchar   *pcv_aws_create_snapshot(const gchar *volume_id, const gchar *description,
                                          const gchar *region, GError **error);
extern gboolean pcv_aws_wait_snapshot(const gchar *snapshot_id, const gchar *region, GError **error);
extern gboolean pcv_disk_apply_delta(const gchar *base_qcow2, const gchar *delta_raw,
                                       GError **error);








static const gchar *_status_strs[] = {
    "queued",
    "validating",
    "exporting",
    "downloading",
    "uploading",
    "converting",
    "importing",
    "defining",
    "done",
    "failed",
    "pre_syncing",
    "awaiting_cutover",
    "finalizing"
};

const gchar *
pcv_cloud_status_str(PcvCloudStatus s)
{
    if (s >= 0 && s <= PCV_CLOUD_STATUS_FINALIZING)
        return _status_strs[s];
    return "unknown";
}








static GHashTable *g_jobs = nullptr;
static GHashTable *g_cancellables = nullptr;
static GMutex      g_jobs_mu;
static gint        g_job_seq = 0;







static sqlite3 *g_cloud_db = nullptr;
#define CLOUD_DB_PATH "/var/lib/purecvisor/cloud_jobs.db"





static void
_cloud_db_init(void)
{
    if (g_cloud_db) return;

    g_mkdir_with_parents("/var/lib/purecvisor", 0700);
    int rc = sqlite3_open(CLOUD_DB_PATH, &g_cloud_db);
    if (rc != SQLITE_OK) {
        PCV_LOG_WARN(CLOUD_LOG, "Failed to open cloud jobs DB: %s",
                     g_cloud_db ? sqlite3_errmsg(g_cloud_db) : "unknown");
        g_cloud_db = nullptr;
        return;
    }
    sqlite3_exec(g_cloud_db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(g_cloud_db, "PRAGMA busy_timeout=3000;", NULL, NULL, NULL);
    sqlite3_exec(g_cloud_db,
        "CREATE TABLE IF NOT EXISTS cloud_jobs ("
        "  id TEXT PRIMARY KEY,"
        "  type TEXT NOT NULL,"
        "  vm_name TEXT NOT NULL,"
        "  status TEXT NOT NULL DEFAULT 'pending',"
        "  progress INTEGER DEFAULT 0,"
        "  error TEXT,"
        "  created_at INTEGER,"
        "  updated_at INTEGER);",
        NULL, NULL, NULL);



    sqlite3_exec(g_cloud_db,
        "CREATE INDEX IF NOT EXISTS idx_cloud_jobs_vm_name ON cloud_jobs(vm_name);",
        NULL, NULL, NULL);
    sqlite3_exec(g_cloud_db,
        "CREATE INDEX IF NOT EXISTS idx_cloud_jobs_status ON cloud_jobs(status);",
        NULL, NULL, NULL);


    sqlite3_exec(g_cloud_db,
        "UPDATE cloud_jobs SET status='failed', error='Daemon restarted' "
        "WHERE status NOT IN ('done','failed');",
        NULL, NULL, NULL);

    PCV_LOG_INFO(CLOUD_LOG, "Cloud jobs DB initialized at %s", CLOUD_DB_PATH);
}




static void
_cloud_db_save_job(const gchar *job_id, const gchar *type, const gchar *vm_name)
{
    if (!g_cloud_db) _cloud_db_init();
    if (!g_cloud_db) return;

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(g_cloud_db,
        "INSERT OR REPLACE INTO cloud_jobs (id, type, vm_name, status, progress, created_at, updated_at) "
        "VALUES (?, ?, ?, 'pending', 0, ?, ?);",
        -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        gint64 now = (gint64)g_get_real_time() / G_USEC_PER_SEC;
        sqlite3_bind_text(stmt, 1, job_id, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, type, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, vm_name, -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 4, now);
        sqlite3_bind_int64(stmt, 5, now);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            g_warning("[cloud] insert job %s failed: %s", job_id, sqlite3_errmsg(g_cloud_db));
        }
        sqlite3_finalize(stmt);
    }
}




static void
_cloud_db_update_status(const gchar *job_id, const gchar *status_str,
                        gint progress, const gchar *error_detail)
{
    if (!g_cloud_db) return;

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(g_cloud_db,
        "UPDATE cloud_jobs SET status=?, progress=?, error=?, updated_at=? "
        "WHERE id=?;",
        -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        gint64 now = (gint64)g_get_real_time() / G_USEC_PER_SEC;
        sqlite3_bind_text(stmt, 1, status_str, -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 2, progress);
        sqlite3_bind_text(stmt, 3, error_detail, -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 4, now);
        sqlite3_bind_text(stmt, 5, job_id, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            g_warning("[cloud] update job %s failed: %s", job_id, sqlite3_errmsg(g_cloud_db));
        }
        sqlite3_finalize(stmt);
    }
}









static gboolean
_validate_ami_id(const gchar *ami_id)
{
    if (!ami_id || !g_str_has_prefix(ami_id, "ami-"))
        return FALSE;
    const gchar *hex = ami_id + 4;
    gsize len = strlen(hex);
    if (len < 8 || len > 17)
        return FALSE;
    for (gsize i = 0; i < len; i++) {
        if (!g_ascii_isxdigit(hex[i]))
            return FALSE;
    }
    return TRUE;
}







static void _ensure_jobs(void) {
    if (!g_jobs)
        g_jobs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                         (GDestroyNotify)pcv_cloud_job_status_free);
    if (!g_cancellables)
        g_cancellables = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                                 g_object_unref);
}















static void
_update_status(const gchar *name, const gchar *job_id,
               const gchar *direction, PcvCloudStatus status,
               gint progress, const gchar *detail)
{
    g_mutex_lock(&g_jobs_mu);
    _ensure_jobs();

    PcvCloudJobStatus *s = g_new0(PcvCloudJobStatus, 1);
    s->name       = g_strdup(name);
    s->job_id     = g_strdup(job_id);
    s->direction  = g_strdup(direction);
    s->status     = status;
    s->progress   = progress;
    s->detail     = g_strdup(detail);
    s->updated_at = g_get_real_time() / G_USEC_PER_SEC;


    PcvCloudJobStatus *old = g_hash_table_lookup(g_jobs, name);
    s->started_at = old ? old->started_at : s->updated_at;
    if (old) {
        if (!s->base_image_path) s->base_image_path = g_strdup(old->base_image_path);
        if (!s->instance_id)     s->instance_id     = g_strdup(old->instance_id);
        if (!s->volume_id)       s->volume_id       = g_strdup(old->volume_id);
    }

    g_hash_table_replace(g_jobs, g_strdup(name), s);
    g_mutex_unlock(&g_jobs_mu);

    PCV_LOG_INFO(CLOUD_LOG, "[%s] %s — %s (%d%%) %s",
                 job_id, name, pcv_cloud_status_str(status), progress, detail ?: "");


    _cloud_db_update_status(job_id, pcv_cloud_status_str(status), progress, detail);




    if (status == PCV_CLOUD_STATUS_QUEUED ||
        status == PCV_CLOUD_STATUS_DONE ||
        status == PCV_CLOUD_STATUS_FAILED) {
        const gchar *audit_direction = direction;
        if (g_strcmp0(direction, "import") == 0 &&
            job_id && g_str_has_prefix(job_id, "finalize-")) {
            audit_direction = "import.finalize";
        }
        gchar *method = g_strdup_printf("cloud.%s", audit_direction ?: "unknown");
        pcv_audit_log("system", method, name,
                      pcv_cloud_status_str(status),
                      status == PCV_CLOUD_STATUS_FAILED ? -1 : 0,
                      (s->updated_at - s->started_at) * 1000, "local");
        g_free(method);
    }
}







static void _update_job_metadata(const gchar *name, const gchar *base_path,
                                  const gchar *instance_id, const gchar *volume_id)
{
    g_mutex_lock(&g_jobs_mu);
    _ensure_jobs();
    PcvCloudJobStatus *st = g_hash_table_lookup(g_jobs, name);
    if (st) {
        if (base_path) { g_free(st->base_image_path); st->base_image_path = g_strdup(base_path); }
        if (instance_id) { g_free(st->instance_id); st->instance_id = g_strdup(instance_id); }
        if (volume_id) { g_free(st->volume_id); st->volume_id = g_strdup(volume_id); }
    }
    g_mutex_unlock(&g_jobs_mu);
}





void pcv_cloud_job_status_free(PcvCloudJobStatus *s) {
    if (!s) return;
    g_free(s->name); g_free(s->job_id); g_free(s->direction);
    g_free(s->detail);
    g_free(s->base_image_path); g_free(s->instance_id); g_free(s->volume_id);
    g_free(s);
}

void pcv_cloud_import_params_free(PcvCloudImportParams *p) {
    if (!p) return;
    g_free(p->name); g_free(p->ami_id); g_free(p->aws_region);
    g_free(p->s3_bucket); g_free(p->network_bridge); g_free(p->disk_format);
    g_free(p->mode); g_free(p->instance_id); g_free(p->volume_id);
    g_free(p);
}

void pcv_cloud_export_params_free(PcvCloudExportParams *p) {
    if (!p) return;
    g_free(p->name); g_free(p->aws_region); g_free(p->s3_bucket);
    g_free(p->ami_name); g_free(p->ami_description); g_free(p);
}


















typedef struct {
    PcvCloudImportParams *params;
    gchar *job_id;
} ImportTaskData;

static void _import_data_free(gpointer p) {
    ImportTaskData *d = p;
    if (!d) return;
    pcv_cloud_import_params_free(d->params);
    g_free(d->job_id);
    g_free(d);
}

static void
_import_worker(GTask *task, gpointer src __attribute__((unused)),
               gpointer task_data, GCancellable *cancel)
{
    ImportTaskData *d = task_data;
    PcvCloudImportParams *p = d->params;
    const gchar *job = d->job_id;
    const gchar *region = p->aws_region ?: pcv_config_get_string("aws", "default_region", "ap-northeast-2");
    const gchar *bucket = p->s3_bucket ?: pcv_config_get_string("aws", "s3_bucket", "pcv-migration");
    GError *err = nullptr;
    const gboolean near_live = p->mode && g_strcmp0(p->mode, "near-live") == 0;


    _update_status(p->name, job, "import", PCV_CLOUD_STATUS_VALIDATING, 2,
                   "Checking AWS credentials");
    if (!pcv_aws_check_credentials(region, &err)) {
        _update_status(p->name, job, "import", PCV_CLOUD_STATUS_FAILED, 0,
                       err ? err->message : "AWS credentials not configured");
        if (err) g_error_free(err);
        g_task_return_boolean(task, FALSE);
        return;
    }


    _update_status(p->name, job, "import", PCV_CLOUD_STATUS_EXPORTING, 5,
                   "Requesting AWS image export");
    gchar *export_task_id = pcv_aws_export_image(p->ami_id, bucket, region, &err);
    if (!export_task_id) {
        _update_status(p->name, job, "import", PCV_CLOUD_STATUS_FAILED, 5,
                       err ? err->message : "export-image failed");
        if (err) g_error_free(err);
        g_task_return_boolean(task, FALSE);
        return;
    }


    _update_status(p->name, job, "import", PCV_CLOUD_STATUS_EXPORTING, 10,
                   "Waiting for AWS export...");
    gchar *s3_key = nullptr;
    for (int i = 0; i < 120; i++) {
        if (g_cancellable_is_cancelled(cancel)) {
            _update_status(p->name, job, "import", PCV_CLOUD_STATUS_FAILED, 0,
                           "Cancelled by user");
            g_free(export_task_id); g_free(s3_key);
            g_task_return_boolean(task, FALSE);
            return;
        }
        gchar *status = nullptr;
        gint prog = pcv_aws_check_export_progress(export_task_id, region,
                                                     &status, &s3_key, NULL);
        if (prog >= 100 || g_strcmp0(status, "completed") == 0) {
            g_free(status);
            break;
        }
        if (g_strcmp0(status, "failed") == 0 || g_strcmp0(status, "cancelled") == 0) {
            _update_status(p->name, job, "import", PCV_CLOUD_STATUS_FAILED, prog,
                           status ?: "export failed");
            g_free(status); g_free(export_task_id); g_free(s3_key);
            g_task_return_boolean(task, FALSE);
            return;
        }
        gchar *detail = g_strdup_printf("AWS export: %s (%d%%)", status ?: "active", prog);
        _update_status(p->name, job, "import", PCV_CLOUD_STATUS_EXPORTING,
                       10 + prog / 5, detail);
        g_free(detail); g_free(status);
        g_usleep(30 * G_USEC_PER_SEC);
    }
    g_free(export_task_id);

    if (!s3_key || !*s3_key) {
        _update_status(p->name, job, "import", PCV_CLOUD_STATUS_FAILED, 30,
                       "S3 key not found after export");
        g_free(s3_key);
        g_task_return_boolean(task, FALSE);
        return;
    }


    _update_status(p->name, job, "import", PCV_CLOUD_STATUS_DOWNLOADING, 35,
                   "Downloading from S3");
    g_mkdir_with_parents(PCV_CLOUD_IMPORT_DIR, 0755);
    if (!pcv_aws_s3_download(bucket, s3_key, PCV_CLOUD_IMPORT_DIR, region, &err)) {
        _update_status(p->name, job, "import", PCV_CLOUD_STATUS_FAILED, 35,
                       err ? err->message : "S3 download failed");
        if (err) g_error_free(err);
        g_free(s3_key);
        g_task_return_boolean(task, FALSE);
        return;
    }

    gchar *raw_filename = g_path_get_basename(s3_key);
    gchar *raw_path = g_strdup_printf("%s/%s", PCV_CLOUD_IMPORT_DIR, raw_filename);
    g_free(raw_filename); g_free(s3_key);


    _update_status(p->name, job, "import", PCV_CLOUD_STATUS_CONVERTING, 70,
                   "Converting RAW to qcow2");
    gchar *disk_path = pcv_disk_convert_raw_to_qcow2(raw_path, p->name, NULL, &err);
    if (!disk_path) {
        _update_status(p->name, job, "import", PCV_CLOUD_STATUS_FAILED, 70,
                       err ? err->message : "Disk conversion failed");
        if (err) g_error_free(err);
        unlink(raw_path); g_free(raw_path);
        g_task_return_boolean(task, FALSE);
        return;
    }


    pcv_disk_inject_virtio(disk_path, NULL);


    unlink(raw_path);
    g_free(raw_path);


    if (near_live) {
        _update_job_metadata(p->name, disk_path, p->instance_id, p->volume_id);
        _update_status(p->name, job, "import", PCV_CLOUD_STATUS_AWAITING_CUTOVER, 80,
                       "Base image ready. Call finalize to complete import.");
        g_free(disk_path);
        g_task_return_boolean(task, TRUE);
        return;
    }


    _update_status(p->name, job, "import", PCV_CLOUD_STATUS_DEFINING, 90,
                   "Creating VM definition");
    {
        gint vcpu = p->vcpu > 0 ? p->vcpu : 2;
        gint mem = p->memory_mb > 0 ? p->memory_mb : 2048;
        const gchar *bridge = p->network_bridge ?: "pcvbr0";
        gboolean vm_defined = FALSE;


        gchar *mem_str = g_strdup_printf("%d", mem);
        gchar *vcpu_str = g_strdup_printf("%d", vcpu);
        gchar *net_arg = g_strdup_printf("bridge=%s,model=virtio", bridge);
        const gchar *argv[] = {
            "virt-install",
            "--name", p->name,
            "--memory", mem_str,
            "--vcpus", vcpu_str,
            "--disk", disk_path,
            "--network", net_arg,
            "--import",
            "--os-variant", "generic",
            "--graphics", "vnc,listen=0.0.0.0",
            "--noautoconsole",
            NULL
        };
        gchar *out = NULL, *verr = nullptr;
        if (pcv_spawn_sync(argv, &out, &verr, &err)) {
            vm_defined = TRUE;
        } else {
            PCV_LOG_WARN(CLOUD_LOG, "virt-install failed: %s — trying XML fallback",
                         verr ?: (err ? err->message : "unknown"));
            if (err) { g_error_free(err); err = nullptr; }
        }
        g_free(out); g_free(verr);
        g_free(mem_str); g_free(vcpu_str); g_free(net_arg);


        if (!vm_defined) {
            PureCVisorVmConfig *cfg = purecvisor_vm_config_new(p->name, vcpu, mem);
            purecvisor_vm_config_set_disk(cfg, disk_path);
            purecvisor_vm_config_set_network_bridge(cfg, bridge);
            GVirConfigDomain *dom = purecvisor_vm_config_build(cfg);
            if (dom) {
                gchar *xml = gvir_config_object_to_xml(
                    GVIR_CONFIG_OBJECT(dom));



                gchar *tmpfile = g_strdup_printf("/tmp/pcv-import-%s.xml", p->name);
                if (g_file_set_contents(tmpfile, xml, -1, NULL)) {
                    if (chmod(tmpfile, 0600) != 0) {
                        PCV_LOG_WARN(CLOUD_LOG, "chmod 0600 failed on %s: %s",
                                     tmpfile, g_strerror(errno));
                    }
                    const gchar *def_argv[] = {"virsh", "define", tmpfile, NULL};
                    gchar *dout = NULL, *derr = nullptr;
                    if (pcv_spawn_sync(def_argv, &dout, &derr, &err)) {
                        vm_defined = TRUE;

                        const gchar *start_argv[] = {"virsh", "start", p->name, NULL};
                        pcv_spawn_sync(start_argv, NULL, NULL, NULL);
                    } else {
                        PCV_LOG_ERROR(CLOUD_LOG, "virsh define fallback failed: %s",
                                      derr ?: (err ? err->message : "unknown"));
                        if (err) { g_error_free(err); err = nullptr; }
                    }
                    g_free(dout); g_free(derr);
                }
                unlink(tmpfile);
                g_free(tmpfile); g_free(xml);
                g_object_unref(dom);
            }
            purecvisor_vm_config_free(cfg);
        }

        if (!vm_defined) {


            unlink(disk_path);
            _update_status(p->name, job, "import", PCV_CLOUD_STATUS_FAILED, 90,
                           "VM definition failed (virt-install + XML fallback)");
            g_free(disk_path);
            g_task_return_boolean(task, FALSE);
            return;
        }
    }

    _update_status(p->name, job, "import", PCV_CLOUD_STATUS_DONE, 100,
                   "Import complete");
    g_free(disk_path);
    g_task_return_boolean(task, TRUE);
}














typedef struct {
    PcvCloudExportParams *params;
    gchar *job_id;
} ExportTaskData;

static void _export_data_free(gpointer p) {
    ExportTaskData *d = p;
    if (!d) return;
    pcv_cloud_export_params_free(d->params);
    g_free(d->job_id);
    g_free(d);
}

static void
_export_worker(GTask *task, gpointer src __attribute__((unused)),
               gpointer task_data, GCancellable *cancel)
{
    ExportTaskData *d = task_data;
    PcvCloudExportParams *p = d->params;
    const gchar *job = d->job_id;
    const gchar *region = p->aws_region ?: pcv_config_get_string("aws", "default_region", "ap-northeast-2");
    const gchar *bucket = p->s3_bucket ?: pcv_config_get_string("aws", "s3_bucket", "pcv-migration");
    GError *err = nullptr;


    _update_status(p->name, job, "export", PCV_CLOUD_STATUS_VALIDATING, 2,
                   "Checking AWS credentials");
    if (!pcv_aws_check_credentials(region, &err)) {
        _update_status(p->name, job, "export", PCV_CLOUD_STATUS_FAILED, 0,
                       err ? err->message : "AWS credentials not configured");
        if (err) g_error_free(err);
        g_task_return_boolean(task, FALSE);
        return;
    }


    _update_status(p->name, job, "export", PCV_CLOUD_STATUS_CONVERTING, 10,
                   "Finding VM disk and converting to RAW");
    gchar *disk_path = pcv_disk_find_vm_disk(p->name, &err);
    if (!disk_path) {
        _update_status(p->name, job, "export", PCV_CLOUD_STATUS_FAILED, 10,
                       err ? err->message : "VM disk not found");
        if (err) g_error_free(err);
        g_task_return_boolean(task, FALSE);
        return;
    }

    gchar *raw_path = pcv_disk_convert_qcow2_to_raw(disk_path, p->name, NULL, &err);
    g_free(disk_path);
    if (!raw_path) {
        _update_status(p->name, job, "export", PCV_CLOUD_STATUS_FAILED, 20,
                       err ? err->message : "Disk conversion failed");
        if (err) g_error_free(err);
        g_task_return_boolean(task, FALSE);
        return;
    }


    _update_status(p->name, job, "export", PCV_CLOUD_STATUS_UPLOADING, 30,
                   "Uploading to S3");
    if (!pcv_aws_s3_upload(raw_path, bucket, "pcv-export/", region, &err)) {
        _update_status(p->name, job, "export", PCV_CLOUD_STATUS_FAILED, 30,
                       err ? err->message : "S3 upload failed");
        if (err) g_error_free(err);
        unlink(raw_path); g_free(raw_path);
        g_task_return_boolean(task, FALSE);
        return;
    }

    gchar *raw_basename = g_path_get_basename(raw_path);
    gchar *s3_key = g_strdup_printf("pcv-export/%s", raw_basename);
    g_free(raw_basename);


    unlink(raw_path); g_free(raw_path);


    _update_status(p->name, job, "export", PCV_CLOUD_STATUS_IMPORTING, 60,
                   "Registering AMI in AWS");
    gchar *desc = p->ami_description
        ? g_strdup(p->ami_description)
        : g_strdup_printf("PureCVisor export: %s", p->name);
    gchar *import_task_id = pcv_aws_import_image(bucket, s3_key, region, desc, &err);
    g_free(desc); g_free(s3_key);

    if (!import_task_id) {
        _update_status(p->name, job, "export", PCV_CLOUD_STATUS_FAILED, 60,
                       err ? err->message : "import-image failed");
        if (err) g_error_free(err);
        g_task_return_boolean(task, FALSE);
        return;
    }


    for (int i = 0; i < 120; i++) {
        if (g_cancellable_is_cancelled(cancel)) {
            _update_status(p->name, job, "export", PCV_CLOUD_STATUS_FAILED, 0,
                           "Cancelled by user");
            g_free(import_task_id);
            g_task_return_boolean(task, FALSE);
            return;
        }
        gchar *status = nullptr; gchar *ami_id = nullptr;
        gint prog = pcv_aws_check_import_progress(import_task_id, region,
                                                     &status, &ami_id, NULL);
        if (prog >= 100 || g_strcmp0(status, "completed") == 0) {
            gchar *detail = g_strdup_printf("AMI created: %s", ami_id ?: "unknown");
            _update_status(p->name, job, "export", PCV_CLOUD_STATUS_DONE, 100, detail);
            g_free(detail); g_free(status); g_free(ami_id); g_free(import_task_id);
            g_task_return_boolean(task, TRUE);
            return;
        }
        if (g_strcmp0(status, "failed") == 0 || g_strcmp0(status, "cancelled") == 0) {
            _update_status(p->name, job, "export", PCV_CLOUD_STATUS_FAILED, prog,
                           status ?: "import-image failed");
            g_free(status); g_free(ami_id); g_free(import_task_id);
            g_task_return_boolean(task, FALSE);
            return;
        }
        gchar *detail = g_strdup_printf("AMI import: %s (%d%%)", status ?: "active", prog);
        _update_status(p->name, job, "export", PCV_CLOUD_STATUS_IMPORTING,
                       60 + prog * 4 / 10, detail);
        g_free(detail); g_free(status); g_free(ami_id);
        g_usleep(30 * G_USEC_PER_SEC);
    }
    g_free(import_task_id);

    _update_status(p->name, job, "export", PCV_CLOUD_STATUS_FAILED, 90,
                   "AMI import timed out (1 hour)");
    g_task_return_boolean(task, FALSE);
}

















gchar *
pcv_cloud_import_ec2(const PcvCloudImportParams *params, GError **error)
{
    if (!params || !params->name || !params->ami_id) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "name and ami_id are required");
        return NULL;
    }
    if (!pcv_validate_vm_name(params->name)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Invalid VM name: %s", params->name);
        return NULL;
    }
    if (!_validate_ami_id(params->ami_id)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Invalid AMI ID format (expected ami-[a-f0-9]{8,17}): %s",
                    params->ami_id);
        return NULL;
    }

    gint seq = g_atomic_int_add(&g_job_seq, 1);
    gchar *job_id = g_strdup_printf("import-%04d", seq);


    _cloud_db_save_job(job_id, "import", params->name);
    _ensure_cutover_sweeper();

    _update_status(params->name, job_id, "import", PCV_CLOUD_STATUS_QUEUED, 0,
                   "Import job queued");


    PcvCloudImportParams *copy = g_new0(PcvCloudImportParams, 1);
    copy->name           = g_strdup(params->name);
    copy->ami_id         = g_strdup(params->ami_id);
    copy->aws_region     = g_strdup(params->aws_region);
    copy->s3_bucket      = g_strdup(params->s3_bucket);
    copy->vcpu           = params->vcpu;
    copy->memory_mb      = params->memory_mb;
    copy->network_bridge = g_strdup(params->network_bridge);
    copy->disk_format    = g_strdup(params->disk_format);
    copy->mode           = g_strdup(params->mode);
    copy->finalize       = params->finalize;
    copy->instance_id    = g_strdup(params->instance_id);
    copy->volume_id      = g_strdup(params->volume_id);

    ImportTaskData *d = g_new0(ImportTaskData, 1);
    d->params = copy;
    d->job_id = g_strdup(job_id);

    GCancellable *cancel = g_cancellable_new();
    g_mutex_lock(&g_jobs_mu);
    _ensure_jobs();
    g_hash_table_replace(g_cancellables, g_strdup(params->name), g_object_ref(cancel));
    g_mutex_unlock(&g_jobs_mu);

    GTask *task = g_task_new(NULL, cancel, NULL, NULL);
    g_task_set_task_data(task, d, _import_data_free);
    g_task_run_in_thread(task, _import_worker);
    g_object_unref(task);
    g_object_unref(cancel);

    return job_id;
}












gchar *
pcv_cloud_export_ec2(const PcvCloudExportParams *params, GError **error)
{
    if (!params || !params->name) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "name is required");
        return NULL;
    }
    if (!pcv_validate_vm_name(params->name)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Invalid VM name: %s", params->name);
        return NULL;
    }

    gint seq = g_atomic_int_add(&g_job_seq, 1);
    gchar *job_id = g_strdup_printf("export-%04d", seq);


    _cloud_db_save_job(job_id, "export", params->name);
    _ensure_cutover_sweeper();

    _update_status(params->name, job_id, "export", PCV_CLOUD_STATUS_QUEUED, 0,
                   "Export job queued");

    PcvCloudExportParams *copy = g_new0(PcvCloudExportParams, 1);
    copy->name            = g_strdup(params->name);
    copy->aws_region      = g_strdup(params->aws_region);
    copy->s3_bucket       = g_strdup(params->s3_bucket);
    copy->ami_name        = g_strdup(params->ami_name);
    copy->ami_description = g_strdup(params->ami_description);
    copy->stop_vm         = params->stop_vm;

    ExportTaskData *d = g_new0(ExportTaskData, 1);
    d->params = copy;
    d->job_id = g_strdup(job_id);

    GCancellable *cancel = g_cancellable_new();
    g_mutex_lock(&g_jobs_mu);
    _ensure_jobs();
    g_hash_table_replace(g_cancellables, g_strdup(params->name), g_object_ref(cancel));
    g_mutex_unlock(&g_jobs_mu);

    GTask *task = g_task_new(NULL, cancel, NULL, NULL);
    g_task_set_task_data(task, d, _export_data_free);
    g_task_run_in_thread(task, _export_worker);
    g_object_unref(task);
    g_object_unref(cancel);

    return job_id;
}











PcvCloudJobStatus *
pcv_cloud_get_status(const gchar *name)
{
    if (!name) return NULL;
    g_mutex_lock(&g_jobs_mu);
    _ensure_jobs();
    PcvCloudJobStatus *orig = g_hash_table_lookup(g_jobs, name);


    if (orig && orig->status == PCV_CLOUD_STATUS_AWAITING_CUTOVER) {
        gint64 now_sec = g_get_real_time() / G_USEC_PER_SEC;
        if ((now_sec - orig->updated_at) > CUTOVER_TIMEOUT_SEC) {
            PCV_LOG_WARN(CLOUD_LOG,
                "AWAITING_CUTOVER timeout for '%s' (age=%ld sec, limit=%d sec) — auto-cancelling",
                name, (long)(now_sec - orig->updated_at), CUTOVER_TIMEOUT_SEC);
            orig->status = PCV_CLOUD_STATUS_FAILED;
            g_free(orig->detail);
            orig->detail = g_strdup_printf(
                "AWAITING_CUTOVER timed out after %d seconds — finalize was not called",
                CUTOVER_TIMEOUT_SEC);
            orig->updated_at = now_sec;
            _cloud_db_update_status(orig->job_id, "failed", orig->progress, orig->detail);
        }
    }

    PcvCloudJobStatus *copy = nullptr;
    if (orig) {
        copy = g_new0(PcvCloudJobStatus, 1);
        copy->name       = g_strdup(orig->name);
        copy->job_id     = g_strdup(orig->job_id);
        copy->direction  = g_strdup(orig->direction);
        copy->status     = orig->status;
        copy->progress   = orig->progress;
        copy->detail     = g_strdup(orig->detail);
        copy->started_at = orig->started_at;
        copy->updated_at = orig->updated_at;
        copy->base_image_path = g_strdup(orig->base_image_path);
        copy->instance_id     = g_strdup(orig->instance_id);
        copy->volume_id       = g_strdup(orig->volume_id);
    }
    g_mutex_unlock(&g_jobs_mu);
    return copy;
}












static void
_sweep_cutover_timeouts_locked(void)
{
    if (!g_jobs) return;
    gint64 now_sec = g_get_real_time() / G_USEC_PER_SEC;
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, g_jobs);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        PcvCloudJobStatus *st = v;
        if (st->status == PCV_CLOUD_STATUS_AWAITING_CUTOVER &&
            (now_sec - st->updated_at) > CUTOVER_TIMEOUT_SEC) {
            PCV_LOG_WARN(CLOUD_LOG,
                "AWAITING_CUTOVER timeout for '%s' (age=%ld sec) — auto-cancelling",
                st->name ?: "?", (long)(now_sec - st->updated_at));
            st->status = PCV_CLOUD_STATUS_FAILED;
            g_free(st->detail);
            st->detail = g_strdup_printf(
                "AWAITING_CUTOVER timed out after %d seconds — finalize was not called",
                CUTOVER_TIMEOUT_SEC);
            st->updated_at = now_sec;
            _cloud_db_update_status(st->job_id, "failed", st->progress, st->detail);
        }
    }
}


static gboolean
_cutover_sweeper_tick(gpointer user_data __attribute__((unused)))
{
    g_mutex_lock(&g_jobs_mu);
    _sweep_cutover_timeouts_locked();
    g_mutex_unlock(&g_jobs_mu);
    return G_SOURCE_CONTINUE;
}

static guint g_cutover_sweeper_id = 0;
static void
_ensure_cutover_sweeper(void)
{
    if (g_cutover_sweeper_id != 0) return;
    g_cutover_sweeper_id = g_timeout_add_seconds(
        CUTOVER_SWEEP_INTERVAL_SEC, _cutover_sweeper_tick, NULL);
}

GPtrArray *
pcv_cloud_list_jobs(void)
{
    GPtrArray *list = g_ptr_array_new_with_free_func(
        (GDestroyNotify)pcv_cloud_job_status_free);
    g_mutex_lock(&g_jobs_mu);
    _ensure_jobs();
    _sweep_cutover_timeouts_locked();
    GHashTableIter iter;
    gpointer key, val;
    g_hash_table_iter_init(&iter, g_jobs);
    while (g_hash_table_iter_next(&iter, &key, &val)) {
        PcvCloudJobStatus *orig = val;
        PcvCloudJobStatus *copy = g_new0(PcvCloudJobStatus, 1);
        copy->name       = g_strdup(orig->name);
        copy->job_id     = g_strdup(orig->job_id);
        copy->direction  = g_strdup(orig->direction);
        copy->status     = orig->status;
        copy->progress   = orig->progress;
        copy->detail     = g_strdup(orig->detail);
        copy->started_at = orig->started_at;
        copy->updated_at = orig->updated_at;
        copy->base_image_path = g_strdup(orig->base_image_path);
        copy->instance_id     = g_strdup(orig->instance_id);
        copy->volume_id       = g_strdup(orig->volume_id);
        g_ptr_array_add(list, copy);
    }
    g_mutex_unlock(&g_jobs_mu);
    return list;
}













gboolean
pcv_cloud_cancel_job(const gchar *name, GError **error)
{
    if (!name) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "name is required");
        return FALSE;
    }

    g_mutex_lock(&g_jobs_mu);
    _ensure_jobs();


    PcvCloudJobStatus *st = g_hash_table_lookup(g_jobs, name);
    if (!st) {
        g_mutex_unlock(&g_jobs_mu);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "No migration job for VM: %s", name);
        return FALSE;
    }


    if (st->status == PCV_CLOUD_STATUS_DONE ||
        st->status == PCV_CLOUD_STATUS_FAILED) {
        g_mutex_unlock(&g_jobs_mu);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Job already finished (status: %s)",
                    pcv_cloud_status_str(st->status));
        return FALSE;
    }


    GCancellable *cancel = g_hash_table_lookup(g_cancellables, name);
    if (cancel)
        g_cancellable_cancel(cancel);

    g_mutex_unlock(&g_jobs_mu);

    PCV_LOG_INFO(CLOUD_LOG, "Cancel requested for job: %s (VM: %s)",
                 st->job_id ?: "?", name);
    return TRUE;
}

















typedef struct {
    gchar *name;
    gchar *job_id;
    gchar *instance_id;
    gchar *volume_id;
    gchar *base_image_path;
    gchar *aws_region;
    gchar *s3_bucket;
} FinalizeTaskData;

static void _finalize_data_free(gpointer p) {
    FinalizeTaskData *d = p;
    if (!d) return;
    g_free(d->name); g_free(d->job_id);
    g_free(d->instance_id); g_free(d->volume_id);
    g_free(d->base_image_path);
    g_free(d->aws_region); g_free(d->s3_bucket);
    g_free(d);
}

static void
_finalize_worker(GTask *task, gpointer src __attribute__((unused)),
                 gpointer task_data, GCancellable *cancel)
{
    FinalizeTaskData *d = task_data;
    const gchar *region = d->aws_region ?: pcv_config_get_string("aws", "default_region", "ap-northeast-2");
    const gchar *bucket = d->s3_bucket ?: pcv_config_get_string("aws", "s3_bucket", "pcv-migration");
    GError *err = nullptr;


    _update_status(d->name, d->job_id, "import", PCV_CLOUD_STATUS_FINALIZING, 5,
                   "Stopping EC2 instance");
    if (!pcv_aws_stop_instance(d->instance_id, region, &err)) {
        _update_status(d->name, d->job_id, "import", PCV_CLOUD_STATUS_FAILED, 5,
                       err ? err->message : "Failed to stop EC2 instance");
        if (err) g_error_free(err);
        g_task_return_boolean(task, FALSE);
        return;
    }

    if (g_cancellable_is_cancelled(cancel)) {
        _update_status(d->name, d->job_id, "import", PCV_CLOUD_STATUS_FAILED, 0,
                       "Cancelled by user");
        g_task_return_boolean(task, FALSE);
        return;
    }


    _update_status(d->name, d->job_id, "import", PCV_CLOUD_STATUS_FINALIZING, 20,
                   "Creating delta snapshot");
    gchar *desc = g_strdup_printf("PureCVisor near-live delta: %s", d->name);
    gchar *snap_id = pcv_aws_create_snapshot(d->volume_id, desc, region, &err);
    g_free(desc);
    if (!snap_id) {
        _update_status(d->name, d->job_id, "import", PCV_CLOUD_STATUS_FAILED, 20,
                       err ? err->message : "Failed to create snapshot");
        if (err) g_error_free(err);
        g_task_return_boolean(task, FALSE);
        return;
    }


    _update_status(d->name, d->job_id, "import", PCV_CLOUD_STATUS_FINALIZING, 30,
                   "Waiting for snapshot completion");
    if (!pcv_aws_wait_snapshot(snap_id, region, &err)) {
        _update_status(d->name, d->job_id, "import", PCV_CLOUD_STATUS_FAILED, 30,
                       err ? err->message : "Snapshot wait failed");
        if (err) g_error_free(err);
        g_free(snap_id);
        g_task_return_boolean(task, FALSE);
        return;
    }

    if (g_cancellable_is_cancelled(cancel)) {
        _update_status(d->name, d->job_id, "import", PCV_CLOUD_STATUS_FAILED, 0,
                       "Cancelled by user");
        g_free(snap_id);
        g_task_return_boolean(task, FALSE);
        return;
    }


    _update_status(d->name, d->job_id, "import", PCV_CLOUD_STATUS_FINALIZING, 40,
                   "Exporting delta snapshot to S3");
    gchar *export_task_id = pcv_aws_export_image(snap_id, bucket, region, &err);
    g_free(snap_id);
    if (!export_task_id) {
        _update_status(d->name, d->job_id, "import", PCV_CLOUD_STATUS_FAILED, 40,
                       err ? err->message : "Delta export failed");
        if (err) g_error_free(err);
        g_task_return_boolean(task, FALSE);
        return;
    }


    gchar *s3_key = nullptr;
    for (int i = 0; i < 60; i++) {
        if (g_cancellable_is_cancelled(cancel)) {
            _update_status(d->name, d->job_id, "import", PCV_CLOUD_STATUS_FAILED, 0,
                           "Cancelled by user");
            g_free(export_task_id); g_free(s3_key);
            g_task_return_boolean(task, FALSE);
            return;
        }
        gchar *status = nullptr;
        gint prog = pcv_aws_check_export_progress(export_task_id, region,
                                                     &status, &s3_key, NULL);
        if (prog >= 100 || g_strcmp0(status, "completed") == 0) {
            g_free(status);
            break;
        }
        if (g_strcmp0(status, "failed") == 0 || g_strcmp0(status, "cancelled") == 0) {
            _update_status(d->name, d->job_id, "import", PCV_CLOUD_STATUS_FAILED, 50,
                           status ?: "delta export failed");
            g_free(status); g_free(export_task_id); g_free(s3_key);
            g_task_return_boolean(task, FALSE);
            return;
        }
        g_free(status);
        g_usleep(30 * G_USEC_PER_SEC);
    }
    g_free(export_task_id);

    if (!s3_key || !*s3_key) {
        _update_status(d->name, d->job_id, "import", PCV_CLOUD_STATUS_FAILED, 55,
                       "Delta S3 key not found after export");
        g_free(s3_key);
        g_task_return_boolean(task, FALSE);
        return;
    }


    _update_status(d->name, d->job_id, "import", PCV_CLOUD_STATUS_FINALIZING, 60,
                   "Downloading delta image");
    g_mkdir_with_parents(PCV_CLOUD_IMPORT_DIR, 0755);
    if (!pcv_aws_s3_download(bucket, s3_key, PCV_CLOUD_IMPORT_DIR, region, &err)) {
        _update_status(d->name, d->job_id, "import", PCV_CLOUD_STATUS_FAILED, 60,
                       err ? err->message : "Delta S3 download failed");
        if (err) g_error_free(err);
        g_free(s3_key);
        g_task_return_boolean(task, FALSE);
        return;
    }

    gchar *delta_filename = g_path_get_basename(s3_key);
    gchar *delta_raw_path = g_strdup_printf("%s/%s", PCV_CLOUD_IMPORT_DIR, delta_filename);
    g_free(delta_filename); g_free(s3_key);


    _update_status(d->name, d->job_id, "import", PCV_CLOUD_STATUS_FINALIZING, 80,
                   "Applying delta to base image");
    if (!pcv_disk_apply_delta(d->base_image_path, delta_raw_path, &err)) {
        _update_status(d->name, d->job_id, "import", PCV_CLOUD_STATUS_FAILED, 80,
                       err ? err->message : "Delta apply failed");
        if (err) g_error_free(err);
        unlink(delta_raw_path); g_free(delta_raw_path);
        g_task_return_boolean(task, FALSE);
        return;
    }
    unlink(delta_raw_path); g_free(delta_raw_path);


    _update_status(d->name, d->job_id, "import", PCV_CLOUD_STATUS_FINALIZING, 95,
                   "Starting VM");
    {
        const gchar *start_argv[] = {"virsh", "start", d->name, NULL};
        gchar *out = NULL, *verr = nullptr;
        if (!pcv_spawn_sync(start_argv, &out, &verr, &err)) {
            PCV_LOG_WARN(CLOUD_LOG, "virsh start failed during finalize: %s",
                         verr ?: (err ? err->message : "unknown"));
            if (err) { g_error_free(err); err = nullptr; }
        }
        g_free(out); g_free(verr);
    }

    _update_status(d->name, d->job_id, "import", PCV_CLOUD_STATUS_DONE, 100,
                   "Near-live import finalized");
    g_task_return_boolean(task, TRUE);
}













gchar *
pcv_cloud_finalize_import(const gchar *name, GError **error)
{
    if (!name) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "name is required");
        return NULL;
    }

    g_mutex_lock(&g_jobs_mu);
    _ensure_jobs();

    PcvCloudJobStatus *st = g_hash_table_lookup(g_jobs, name);
    if (!st) {
        g_mutex_unlock(&g_jobs_mu);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "No migration job for VM: %s", name);
        return NULL;
    }
    if (st->status != PCV_CLOUD_STATUS_AWAITING_CUTOVER) {
        g_mutex_unlock(&g_jobs_mu);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Job not in AWAITING_CUTOVER state (current: %s)",
                    pcv_cloud_status_str(st->status));
        return NULL;
    }
    if (!st->base_image_path || !st->instance_id || !st->volume_id) {
        g_mutex_unlock(&g_jobs_mu);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Missing near-live metadata (base_image/instance_id/volume_id)");
        return NULL;
    }

    gint seq = g_atomic_int_add(&g_job_seq, 1);
    gchar *job_id = g_strdup_printf("finalize-%04d", seq);


    _cloud_db_save_job(job_id, "finalize", name);

    FinalizeTaskData *d = g_new0(FinalizeTaskData, 1);
    d->name            = g_strdup(name);
    d->job_id          = g_strdup(job_id);
    d->instance_id     = g_strdup(st->instance_id);
    d->volume_id       = g_strdup(st->volume_id);
    d->base_image_path = g_strdup(st->base_image_path);


    d->aws_region = g_strdup(pcv_config_get_string("aws", "default_region", "ap-northeast-2"));
    d->s3_bucket  = g_strdup(pcv_config_get_string("aws", "s3_bucket", "pcv-migration"));

    g_mutex_unlock(&g_jobs_mu);

    _update_status(name, job_id, "import", PCV_CLOUD_STATUS_FINALIZING, 0,
                   "Finalize started — stopping EC2 instance");

    GCancellable *cancel = g_cancellable_new();
    g_mutex_lock(&g_jobs_mu);
    g_hash_table_replace(g_cancellables, g_strdup(name), g_object_ref(cancel));
    g_mutex_unlock(&g_jobs_mu);

    GTask *task = g_task_new(NULL, cancel, NULL, NULL);
    g_task_set_task_data(task, d, _finalize_data_free);
    g_task_run_in_thread(task, _finalize_worker);
    g_object_unref(task);
    g_object_unref(cancel);

    return job_id;
}
