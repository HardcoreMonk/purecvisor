







































#ifndef PCV_BACKUP_SCHEDULER_H
#define PCV_BACKUP_SCHEDULER_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS








typedef struct {
    gchar    *vm_name;
    gint      interval_hours;
    gint      retention_count;
    gboolean  enabled;
} PcvBackupPolicy;






void pcv_backup_scheduler_init(void);





void pcv_backup_scheduler_shutdown(void);













gboolean   pcv_backup_policy_set(const gchar *vm_name,
                                  gint         interval_hours,
                                  gint         retention_count,
                                  GError     **error);









gboolean   pcv_backup_policy_delete(const gchar *vm_name,
                                     GError     **error);






GPtrArray *pcv_backup_policy_list(void);









GPtrArray *pcv_backup_history(const gchar *vm_name);











GPtrArray *pcv_backup_history_paged(const gchar *vm_name,
                                     guint        offset,
                                     guint        limit,
                                     guint       *total_out);







[[nodiscard]] gboolean   pcv_backup_restore(const gchar *vm_name,
                               const gchar *snapshot_name,
                               GError     **error);





void pcv_backup_policy_free(PcvBackupPolicy *p);















JsonObject *pcv_backup_incremental(const gchar *vm_name, GError **error);















JsonObject *pcv_backup_verify(const gchar *vm_name,
                              const gchar *snapshot_name,
                              GError     **error);
















gboolean pcv_backup_replicate(const gchar *vm_name,
                              const gchar *target_node,
                              const gchar *ssh_user,
                              GError     **error);


























gboolean pcv_backup_export_s3(const gchar *vm_name,
                               const gchar *s3_endpoint,
                               const gchar *s3_bucket,
                               const gchar *s3_key_prefix,
                               GError     **error);














JsonObject *pcv_snapshot_schedule_status(void);

G_END_DECLS

#endif
