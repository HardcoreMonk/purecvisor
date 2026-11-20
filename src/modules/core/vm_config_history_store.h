#ifndef PURECVISOR_VM_CONFIG_HISTORY_STORE_H
#define PURECVISOR_VM_CONFIG_HISTORY_STORE_H

#include <glib.h>

#include "modules/virt/vm_config_history_decide.h"
#include "modules/virt/vm_config_history_view.h"

G_BEGIN_DECLS

#define PCV_VMCH_DEFAULT_DB_PATH "/var/lib/purecvisor/vm_config_history.db"
#define PCV_VMCH_KEY_NAME        "vm_config_history.key"
#define PCV_VMCH_USER_VERSION    1
#define PCV_VMCH_SALT_LEN        32
#define PCV_VMCH_PRUNE_BATCH     500

#define PCV_VMCH_PRUNE_BUDGET_MS 2000

typedef struct _PcvVmCfgStore PcvVmCfgStore;

PcvVmCfgStore *pcv_vm_cfg_store_open(const gchar *db_path, gboolean writer, GError **err);

void pcv_vm_cfg_store_close(PcvVmCfgStore *st);

gchar *pcv_vm_cfg_store_key_path(const gchar *db_path);

const guint8 *pcv_vm_cfg_store_salt(const PcvVmCfgStore *st, gsize *len);

const gchar *pcv_vm_cfg_store_salt_id(const PcvVmCfgStore *st);

gboolean pcv_vm_cfg_store_created(const PcvVmCfgStore *st);

typedef struct {
    gboolean     have_row;
    gboolean     present;
    gint64       last_id;
    gint64       last_confirmed_at;
    gchar       *current_name;
    gint64       run_id;
    gint64       run_started_at;
    gchar       *run_live_digest;
    PcvVmCfgRow *last;
} PcvVmCfgLatestRow;

void pcv_vm_cfg_latest_clear(PcvVmCfgLatestRow *l);

gboolean pcv_vm_cfg_store_latest(PcvVmCfgStore *st, const gchar *uuid, PcvVmCfgLatestRow *out, GError **err);

typedef struct {
    gboolean          present;
    const gchar      *current_name;
    PcvVmCfgRunAction run_action;
    gint64            run_id;
    gint64            run_started_at;
    const gchar      *run_live_digest;
    gint64            confirmed_at;
} PcvVmCfgLatestUpdate;

gboolean pcv_vm_cfg_store_insert(PcvVmCfgStore *st, PcvVmCfgRow *row, const PcvVmCfgLatestUpdate *lu, GError **err);

gboolean pcv_vm_cfg_store_touch(PcvVmCfgStore *st, const gchar *uuid, const PcvVmCfgLatestUpdate *lu, GError **err);

gboolean pcv_vm_cfg_store_confirm(PcvVmCfgStore *st, const gchar *const *uuids, guint n, gint64 now, GError **err);

gboolean pcv_vm_cfg_store_fill_actor(PcvVmCfgStore *st, const gchar *entry_id, const gchar *actor,
                                     const gchar *actor_tenant, const gchar *method, const gchar *result, GError **err);

guint pcv_vm_cfg_store_rows_since(PcvVmCfgStore *st, const gchar *uuid, gint64 since);

GPtrArray *pcv_vm_cfg_store_present_uuids(PcvVmCfgStore *st);

gint64 pcv_vm_cfg_store_epoch_begin(PcvVmCfgStore *st, gint64 now, GError **err);

gboolean pcv_vm_cfg_store_epoch_heartbeat(PcvVmCfgStore *st, gint64 id, gint64 now, GError **err);

gboolean pcv_vm_cfg_store_epoch_end(PcvVmCfgStore *st, gint64 id, gint64 now, const gchar *reason, GError **err);

guint pcv_vm_cfg_store_close_open_epochs_crash(PcvVmCfgStore *st, GError **err);

gint64 pcv_vm_cfg_store_last_epoch_end(PcvVmCfgStore *st);

GArray *pcv_vm_cfg_store_epochs(PcvVmCfgStore *st, gint64 since, gint64 until);

typedef gboolean (*PcvVmCfgVisibleFn)(const gchar *owner, const gchar *owner_tenant, gpointer ud);

typedef struct {
    const gchar *uuid;
    gint64       since;
    gint64       until;
    gint64       cursor_at;
    gchar        cursor_id[33];
    gboolean     narrow;
    const gchar *narrow_subject;
    const gchar *narrow_tenant;
} PcvVmCfgListFilter;

gboolean pcv_vm_cfg_store_list(PcvVmCfgStore *st, const PcvVmCfgListFilter *f, guint limit, PcvVmCfgVisibleFn visible,
                               gpointer ud, GPtrArray *rows_out, gboolean *more, GError **err);

PcvVmCfgRow *pcv_vm_cfg_store_get(PcvVmCfgStore *st, const gchar *entry_id);

PcvVmCfgRow *pcv_vm_cfg_store_get_prev(PcvVmCfgStore *st, const PcvVmCfgRow *row);

gint64 pcv_vm_cfg_store_last_resized(PcvVmCfgStore *st, const gchar *uuid, const gchar *dev);

gchar *pcv_vm_cfg_store_resolve_name(PcvVmCfgStore *st, const gchar *name);

typedef struct {
    gint64 rows;
    gint64 oldest_at;
    gint64 pruned_before;
    gint64 bytes;
} PcvVmCfgStoreInfo;

gboolean pcv_vm_cfg_store_info(PcvVmCfgStore *st, PcvVmCfgStoreInfo *out);

gint64 pcv_vm_cfg_store_bytes(PcvVmCfgStore *st);

typedef struct {
    guint    deleted_age;
    guint    deleted_cap;
    guint    actor_expired;
    guint    epochs_deleted;
    gboolean over_cap;
    gboolean interrupted;
} PcvVmCfgPruneResult;

gboolean pcv_vm_cfg_store_prune(PcvVmCfgStore *st, gint64 now, gint retention_days, gint actor_retention_days,
                                gint64 max_bytes, guint batch, const gint *stop, gint64 budget_ms,
                                PcvVmCfgPruneResult *out, GError **err);

G_END_DECLS

#endif
