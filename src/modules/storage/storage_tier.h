#ifndef PURECVISOR_STORAGE_TIER_H
#define PURECVISOR_STORAGE_TIER_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
    PCV_TIER_NVME = 0,
    PCV_TIER_SSD  = 1,
    PCV_TIER_HDD  = 2,
    PCV_TIER_AUTO = 99
} PcvStorageTier;

void pcv_storage_tier_init(void);

G_END_DECLS

#endif
