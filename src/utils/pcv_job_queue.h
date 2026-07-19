
#ifndef PCV_JOB_QUEUE_H
#define PCV_JOB_QUEUE_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

typedef enum {
    PCV_JOB_PENDING   = 0,
    PCV_JOB_RUNNING   = 1,
    PCV_JOB_COMPLETED = 2,
    PCV_JOB_FAILED    = 3,
    PCV_JOB_CANCELLED = 4
} PcvJobStatus;

void pcv_job_queue_init(void);

void pcv_job_queue_shutdown(void);

void pcv_job_queue_cleanup_old(gint max_age_hours);

gchar *pcv_job_create(const gchar *type, const gchar *target,
                       const gchar *params_json);

void pcv_job_update_status(const gchar *job_id, PcvJobStatus status,
                            gint progress_pct, const gchar *detail);

void pcv_job_set_result(const gchar *job_id, PcvJobStatus status,
                         const gchar *result_json);

JsonArray *pcv_job_list(gint limit);

JsonObject *pcv_job_get(const gchar *job_id);

gboolean pcv_job_cancel(const gchar *job_id);

G_END_DECLS

#endif
