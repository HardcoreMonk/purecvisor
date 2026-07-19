
#include "snapshot_verify_probe.h"
#include "../utils/pcv_spawn.h"

gboolean pcv_snapshot_verify_probe(const gchar *snap)
{
    if (!snap || !*snap)
        return FALSE;
    const gchar *argv[] = {
        "zfs", "list", "-t", "snapshot", "-H", "-o", "name", snap, NULL
    };

    return pcv_spawn_sync(argv, NULL, NULL, NULL);
}
