#ifndef PURECVISOR_STORAGE_TIER_H
#define PURECVISOR_STORAGE_TIER_H









#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS


typedef enum {
    PCV_TIER_NVME = 0,
    PCV_TIER_SSD  = 1,
    PCV_TIER_HDD  = 2,
    PCV_TIER_AUTO = 99
} PcvStorageTier;


void pcv_storage_tier_init(void);
void pcv_storage_tier_shutdown(void);


JsonArray  *pcv_storage_tier_list(void);
JsonObject *pcv_storage_tier_info(const gchar *tier_name);
gboolean    pcv_storage_tier_create(const gchar *name, const gchar *pool,
                                     PcvStorageTier type, GError **error);


const gchar *pcv_storage_tier_auto_select(gint vcpu, gint ram_mb);


gboolean pcv_storage_tier_migrate(const gchar *vm_name, const gchar *target_tier,
                                   GError **error);


gboolean pcv_storage_qos_set(const gchar *vm_name, gint64 read_bps, gint64 write_bps,
                              gint64 read_iops, gint64 write_iops, GError **error);
JsonObject *pcv_storage_qos_get(const gchar *vm_name);
gboolean pcv_storage_qos_delete(const gchar *vm_name, GError **error);

G_END_DECLS

#endif
