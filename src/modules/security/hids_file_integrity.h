#ifndef PURECVISOR_HIDS_FILE_INTEGRITY_H
#define PURECVISOR_HIDS_FILE_INTEGRITY_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* Unknown is the safe default: no baseline exists until an admin refreshes one. */
typedef enum {
    PCV_HIDS_BASELINE_UNKNOWN,
    PCV_HIDS_BASELINE_TRUSTED,
    PCV_HIDS_BASELINE_STALE
} PcvHidsBaselineStatus;

PcvHidsBaselineStatus pcv_hids_baseline_status(const gchar *db_path);
gboolean pcv_hids_baseline_refresh(const gchar *db_path,
                                    const gchar * const *paths,
                                    gsize n_paths,
                                    const gchar *admin_user,
                                    GError **error);
GPtrArray *pcv_hids_file_integrity_scan(const gchar *db_path,
                                         const gchar * const *paths,
                                         gsize n_paths);

G_END_DECLS

#endif
