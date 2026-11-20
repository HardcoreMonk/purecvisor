#ifndef PURECVISOR_VM_CONFIG_HISTORY_VIEW_H
#define PURECVISOR_VM_CONFIG_HISTORY_VIEW_H

#include <glib.h>
#include <json-glib/json-glib.h>

#include "modules/resource/vm_read_scope.h"

G_BEGIN_DECLS

#define PCV_VMCH_SUMMARY_CHANGES 8

#define PCV_VMCH_RESP_LIMIT (256U * 1024U)

#define PCV_VMCH_OTHER_OWNER "(다른 소유자)"

#define PCV_VMCH_OPAQUE_MASK "(지문 비공개)"

typedef struct {
    gint64   id;
    gchar   *entry_id;
    gchar   *vm_uuid;
    gchar   *vm_name;
    gchar   *owner;
    gchar   *owner_tenant;
    gchar   *kind;
    gchar   *scope;
    gchar   *cause;
    gint64   observed_at;
    gint64   window_start;
    gchar   *attribution;
    gchar   *actor;
    gchar   *actor_tenant;
    gboolean actor_expired;
    gchar   *actor_method;
    gchar   *actor_result;
    gchar   *config_digest;
    gchar   *live_digest;
    gchar   *snapshot_json;
    gchar   *changes_json;
    gint64   prev_id;
    gchar   *prev_entry_id;
    gint     normalizer_version;
    gchar   *salt_id;
    gint64   epoch_id;
} PcvVmCfgRow;

void pcv_vm_cfg_row_free(PcvVmCfgRow *row);

gboolean pcv_vm_cfg_row_visible(const PcvVmReadScope *s, const gchar *owner, const gchar *owner_tenant);

JsonNode *pcv_vm_cfg_row_project(const PcvVmCfgRow *row, gint role, const gchar *subject, const gchar *caller_tenant,
                                 gboolean full);

gchar *pcv_vm_cfg_cursor_encode(gint64 observed_at, const gchar *entry_id);

gboolean pcv_vm_cfg_cursor_decode(const gchar *cursor, gint64 *observed_at, gchar entry_id_out[33]);

gboolean pcv_vm_cfg_entry_id_valid(const gchar *id);

G_END_DECLS

#endif
