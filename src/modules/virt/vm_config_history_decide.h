#ifndef PURECVISOR_VM_CONFIG_HISTORY_DECIDE_H
#define PURECVISOR_VM_CONFIG_HISTORY_DECIDE_H

#include <glib.h>

G_BEGIN_DECLS

#define PCV_VMCH_RUN_SETTLE_MS        10000
#define PCV_VMCH_INTENT_MAX           1024
#define PCV_VMCH_INTENT_CLOSED_TTL_MS 30000
#define PCV_VMCH_INTENT_OPEN_MAX_MS   900000
#define PCV_VMCH_LATE_FILL_MS         30000
#define PCV_VMCH_COALESCE_ROWS_PER_H  30
#define PCV_VMCH_COALESCE_MS          60000

typedef enum {
    PCV_VMCH_RUN_KEEP = 0,
    PCV_VMCH_RUN_SET,
    PCV_VMCH_RUN_UPDATE_LIVE,
    PCV_VMCH_RUN_CLEAR
} PcvVmCfgRunAction;

typedef struct {
    gboolean     have_row;
    gboolean     present;
    const gchar *config_digest;
    gint64       run_id;
    gint64       run_started_at;
    const gchar *run_live_digest;
    const gchar *salt_id;
    gint         normalizer_version;
} PcvVmCfgLatest;

typedef struct {
    gboolean     exists;
    gboolean     persistent;
    const gchar *config_digest;
    const gchar *live_digest;
    gint64       domain_id;
    gint64       now_ms;
    gboolean     settle_close;
    const gchar *salt_id;
    gint         normalizer_version;
    guint        rebase_changes;
    gint         settle_ms;
} PcvVmCfgObservation;

typedef struct {
    gboolean          write_row;
    const gchar      *kind;
    const gchar      *scope;
    const gchar      *cause_suffix;
    guint             diff_flags;
    gboolean          transient;
    gboolean          present_after;
    PcvVmCfgRunAction run_action;
    gint64            window_start;
} PcvVmCfgDecision;

PcvVmCfgDecision pcv_vm_cfg_decide(const PcvVmCfgLatest *latest, const PcvVmCfgObservation *obs, gboolean first_db_boot,
                                   gint64 window_floor_ms);

typedef enum { PCV_VMCH_ATTR_NONE = 0, PCV_VMCH_ATTR_RPC, PCV_VMCH_ATTR_AMBIGUOUS } PcvVmCfgAttrKind;

typedef struct {
    PcvVmCfgAttrKind kind;
    gchar           *method;
    gchar           *actor;
    gchar           *actor_tenant;
    gchar           *result;
    gchar           *resize_dev;
    gint64           resize_bytes;
} PcvVmCfgAttribution;

void pcv_vm_cfg_attribution_clear(PcvVmCfgAttribution *a);

const gchar *pcv_vm_cfg_attribution_name(PcvVmCfgAttrKind k);

typedef struct _PcvVmCfgIntents PcvVmCfgIntents;

PcvVmCfgIntents *pcv_vm_cfg_intents_new(guint max);

void pcv_vm_cfg_intents_free(PcvVmCfgIntents *t);

void pcv_vm_cfg_intents_open(PcvVmCfgIntents *t, const gchar *method, const gchar *target, const gchar *actor,
                             const gchar *actor_tenant, gint64 start_ms, const gchar *resize_dev, gint64 resize_bytes);

gboolean pcv_vm_cfg_intents_close(PcvVmCfgIntents *t, const gchar *method, const gchar *actor, const gchar *target,
                                  const gchar *result, gint64 end_ms, gchar **tenant_out);

void pcv_vm_cfg_intents_match(PcvVmCfgIntents *t, const gchar *const *names, gint64 window_start, gint64 observed_at,
                              PcvVmCfgAttribution *out);

gboolean pcv_vm_cfg_intents_take_resize(PcvVmCfgIntents *t, const gchar *method, const gchar *actor, const gchar *target,
                                        PcvVmCfgAttribution *out);

void pcv_vm_cfg_intents_expire(PcvVmCfgIntents *t, gint64 now_ms);

guint pcv_vm_cfg_intents_size(const PcvVmCfgIntents *t);
guint64 pcv_vm_cfg_intents_dropped(const PcvVmCfgIntents *t);

gboolean pcv_vm_cfg_late_fill_allowed(const gchar *attribution, gint64 observed_at, gboolean already_filled, gint64 now_ms);

typedef struct {
    gint64 id;
    gint64 started_at;
    gint64 last_alive_at;
    gint64 ended_at;
    gchar  end_reason[24];
} PcvVmCfgEpoch;

typedef struct {
    gint64       start;
    gint64       end;
    const gchar *reason;
} PcvVmCfgGap;

GArray *pcv_vm_cfg_gaps(const PcvVmCfgEpoch *epochs, guint n, gint64 since, gint64 until, gint64 pruned_before);

typedef struct {
    const gchar *uuid;
    guint        rows;
} PcvVmCfgUuidCount;

gint pcv_vm_cfg_prune_pick(const PcvVmCfgUuidCount *counts, guint n, guint *take);

guint pcv_vm_cfg_prune_plan(const PcvVmCfgUuidCount *counts, guint n, guint budget, guint *plan);

gboolean pcv_vm_cfg_coalesce_active(guint rows_last_hour);

G_END_DECLS

#endif
