#ifndef PURECVISOR_VM_CONFIG_HISTORY_H
#define PURECVISOR_VM_CONFIG_HISTORY_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define PCV_VMCH_HEARTBEAT_S       300
#define PCV_VMCH_PRUNE_INTERVAL_S  3600
#define PCV_VMCH_PENDING_MAX       256

typedef struct {
    gchar   *inactive_xml;
    gchar   *live_xml;
    gchar   *name;
    gboolean autostart;
    gboolean persistent;
    gint64   domain_id;
} PcvVmCfgRaw;

void pcv_vm_cfg_raw_clear(PcvVmCfgRaw *r);

typedef struct {
    GPtrArray *(*list_domains)(gpointer data, GError **err);
    gboolean (*read)(gpointer data, const gchar *uuid, PcvVmCfgRaw *out, gboolean *exists, GError **err);
    gchar *(*resolve)(gpointer data, const gchar *name_or_uuid);
    gchar *(*owner_tenant)(gpointer data, const gchar *owner);
} PcvVmCfgHistSource;

typedef struct {
    gchar   *db_path;
    gboolean enabled;
    gint     retention_days;
    gint     max_db_mb;
    gint     actor_retention_days;
    gint     reconcile_interval_s;
    gint     debounce_ms;
    gint     undefine_grace_ms;

    gboolean wait_mainloop;
    gboolean wait_coverage;
    gint     scan_gap_ms;
    gint     coalesce_ms;
    gint     settle_ms;
    gint     shutdown_timeout_ms;
    gint     heartbeat_s;
    gint     prune_interval_s;
    gint     pending_max;
} PcvVmCfgHistConfig;

void pcv_vm_cfg_hist_config_load(PcvVmCfgHistConfig *c);

void pcv_vm_cfg_hist_config_clear(PcvVmCfgHistConfig *c);

gboolean pcv_vm_cfg_hist_start_with(const PcvVmCfgHistConfig *c, const PcvVmCfgHistSource *src, gpointer src_data);

void pcv_vm_cfg_hist_start_default(void);

void pcv_vm_cfg_hist_shutdown(void);

void pcv_vm_cfg_hist_note_uuid(const gchar *uuid, const gchar *cause);

void pcv_vm_cfg_hist_note_started(const gchar *uuid);

void pcv_vm_cfg_hist_note_undefined(const gchar *uuid);

void pcv_vm_cfg_hist_coverage(gboolean begin, const gchar *reason);

void pcv_vm_cfg_hist_intent_open(const gchar *method, const gchar *target, const gchar *actor, const gchar *actor_tenant,
                                 const gchar *resize_dev, gint64 resize_bytes);

void pcv_vm_cfg_hist_intent_close(const gchar *method, const gchar *actor, const gchar *target, const gchar *result);

typedef struct {
    gchar   *state;
    gchar   *db_path;
    gint     retention_days;
    gboolean enabled;
} PcvVmCfgHistStatus;

void pcv_vm_cfg_hist_status(PcvVmCfgHistStatus *out);

void pcv_vm_cfg_hist_status_clear(PcvVmCfgHistStatus *s);

JsonArray *pcv_vm_cfg_hist_capture_failures(gint64 since, gint64 until);

gboolean pcv_vm_cfg_hist_sync(guint timeout_ms);

void pcv_vm_cfg_hist_test_fail_writes(gboolean fail);

guint64 pcv_vm_cfg_hist_test_counter(const gchar *name);

G_END_DECLS

#endif
