#include "pcv_bootstrap.h"









static const PcvBootstrapEditionInfo g_bootstrap_info = {
    .edition_name = "single",
    .cluster_enabled = FALSE,
};

const PcvBootstrapEditionInfo *
pcv_bootstrap_get_edition_info(void)
{
    return &g_bootstrap_info;
}
