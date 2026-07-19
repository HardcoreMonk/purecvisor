
#pragma once
#include <glib.h>

typedef struct {
    const gchar *direction;
    const gchar *protocol;
    gint         port_start;
    gint         port_end;
    const gchar *source;
} SgNftRule;

typedef struct {
    const gchar *vnet;
    GPtrArray   *groups;
    gboolean     egress_enforced;
} SgNftBinding;

gchar *pcv_sg_nft_build_ensure_script(void);
gchar *pcv_sg_nft_build_group_script(const gchar *group, GPtrArray *rules);
gchar *pcv_sg_nft_build_group_delete_script(const gchar *group);
gchar *pcv_sg_nft_build_dispatch_script(GPtrArray *bindings);
