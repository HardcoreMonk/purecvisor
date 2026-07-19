
#ifndef PURECVISOR_CLOUD_MIGRATION_H
#define PURECVISOR_CLOUD_MIGRATION_H

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define PCV_CLOUD_IMPORT_DIR   "/pcvpool/import"
#define PCV_CLOUD_EXPORT_DIR   "/pcvpool/export"

typedef enum {
    PCV_CLOUD_STATUS_QUEUED      = 0,
    PCV_CLOUD_STATUS_VALIDATING  = 1,
    PCV_CLOUD_STATUS_EXPORTING   = 2,
    PCV_CLOUD_STATUS_DOWNLOADING = 3,
    PCV_CLOUD_STATUS_UPLOADING   = 4,
    PCV_CLOUD_STATUS_CONVERTING  = 5,
    PCV_CLOUD_STATUS_IMPORTING   = 6,
    PCV_CLOUD_STATUS_DEFINING    = 7,
    PCV_CLOUD_STATUS_DONE        = 8,
    PCV_CLOUD_STATUS_FAILED      = 9,
    PCV_CLOUD_STATUS_PRE_SYNCING     = 10,
    PCV_CLOUD_STATUS_AWAITING_CUTOVER = 11,
    PCV_CLOUD_STATUS_FINALIZING      = 12,
} PcvCloudStatus;

const gchar *pcv_cloud_status_str(PcvCloudStatus s);

typedef struct {
    gchar          *name;
    gchar          *job_id;
    gchar          *direction;
    PcvCloudStatus  status;
    gint            progress;
    gchar          *detail;
    gint64          started_at;
    gint64          updated_at;
    gchar          *base_image_path;
    gchar          *instance_id;
    gchar          *volume_id;
} PcvCloudJobStatus;

void pcv_cloud_job_status_free(PcvCloudJobStatus *s);

typedef struct {
    gchar   *name;
    gchar   *ami_id;
    gchar   *aws_region;
    gchar   *s3_bucket;
    gint     vcpu;
    gint     memory_mb;
    gchar   *network_bridge;
    gchar   *disk_format;
    gchar   *mode;
    gboolean finalize;
    gchar   *instance_id;
    gchar   *volume_id;
} PcvCloudImportParams;

void pcv_cloud_import_params_free(PcvCloudImportParams *p);

typedef struct {
    gchar   *name;
    gchar   *aws_region;
    gchar   *s3_bucket;
    gchar   *ami_name;
    gchar   *ami_description;
    gboolean stop_vm;
} PcvCloudExportParams;

void pcv_cloud_export_params_free(PcvCloudExportParams *p);

gchar *pcv_cloud_import_ec2(const PcvCloudImportParams *params, GError **error);

gchar *pcv_cloud_export_ec2(const PcvCloudExportParams *params, GError **error);

PcvCloudJobStatus *pcv_cloud_get_status(const gchar *name);

GPtrArray *pcv_cloud_list_jobs(void);

gboolean pcv_cloud_cancel_job(const gchar *name, GError **error);

gchar *pcv_cloud_finalize_import(const gchar *name, GError **error);

G_END_DECLS

#endif
