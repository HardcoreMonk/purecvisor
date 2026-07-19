
#pragma once

#include <glib.h>

typedef struct {
    gchar *name;
    guint vcpu;
    guint memory_mb;
    guint disk_size_gb;
    gchar *iso_path;
} PureCVisorVmConfig;
