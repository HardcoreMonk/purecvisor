
#include "storage_tier.h"
#include "utils/pcv_log.h"
#include "utils/pcv_config.h"

#define TIER_LOG_DOM  "storage_tier"

#define MAX_TIERS     8

typedef struct {
    gchar          name[32];
    gchar          pool[64];
    PcvStorageTier type;
    gboolean       active;
} TierDef;

static struct {
    TierDef   tiers[MAX_TIERS];
    gint      count;
    GMutex    mu;
    gboolean  initialized;
} G = {0};

void
pcv_storage_tier_init(void)
{
    g_mutex_init(&G.mu);
    G.count = 0;

    g_strlcpy(G.tiers[0].name, "ssd", sizeof(G.tiers[0].name));
    g_strlcpy(G.tiers[0].pool, pcv_config_get_zvol_pool(), sizeof(G.tiers[0].pool));
    G.tiers[0].type = PCV_TIER_SSD;
    G.tiers[0].active = TRUE;
    G.count = 1;

    G.initialized = TRUE;
    PCV_LOG_INFO(TIER_LOG_DOM, "Storage tier initialized (default: ssd → %s)",
                 G.tiers[0].pool);
}
