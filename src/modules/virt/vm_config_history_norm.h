#ifndef PURECVISOR_VM_CONFIG_HISTORY_NORM_H
#define PURECVISOR_VM_CONFIG_HISTORY_NORM_H

#include <glib.h>

G_BEGIN_DECLS

#define PCV_VMCH_NORMALIZER_VERSION 1

#define PCV_VMCH_XML_LIMIT (1024U * 1024U)

#define PCV_VMCH_QEMU_NS_URI "http://libvirt.org/schemas/domain/qemu/1.0"

typedef enum {
    PCV_VMCH_DIFF_ALL = 0,
    PCV_VMCH_DIFF_IGNORE_OPAQUE = 1 << 0,
    PCV_VMCH_DIFF_IGNORE_NEW_PATHS = 1 << 1
} PcvVmCfgDiffFlags;

typedef struct {
    gchar *config_json;
    gchar *config_digest;
    gchar *live_overlay_json;
    gchar *live_digest;
    gchar *owner;
    gchar *name;
    gchar *uuid;
    gint   other_device_count;
} PcvVmCfgSnapshot;

void pcv_vm_cfg_snapshot_clear(PcvVmCfgSnapshot *s);

gboolean pcv_vm_cfg_normalize(const gchar *inactive_xml, gsize inactive_len, const gchar *live_xml, gsize live_len,
                              gboolean autostart, const guint8 *salt, gsize salt_len, PcvVmCfgSnapshot *out,
                              const gchar **err_code);

gchar *pcv_vm_cfg_fingerprint(const guint8 *salt, gsize salt_len, const guint8 *bytes, gsize len);

gchar *pcv_vm_cfg_sha256_hex(const gchar *text);

gchar *pcv_vm_cfg_diff(const gchar *before_json, const gchar *after_json, guint max_paths, guint *count_out);

gchar *pcv_vm_cfg_diff_filtered(const gchar *before_json, const gchar *after_json, guint max_paths, guint flags,
                                guint *count_out);

gboolean pcv_vm_cfg_path_is_host_path(const gchar *path);

G_END_DECLS

#endif
