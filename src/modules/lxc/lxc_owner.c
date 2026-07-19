
#include "lxc_owner.h"
#include "utils/pcv_config.h"
#include "utils/pcv_log.h"

#include <glib/gstdio.h>

#define LXC_OWNER_LOG_DOM "lxc_owner"

static gchar *
_pcv_lxc_owner_path(const gchar *name)
{
    if (!name || !*name)
        return NULL;
    return g_strdup_printf("%s/%s/purecvisor.owner",
                           pcv_config_get_container_path(), name);
}

gboolean
pcv_lxc_stamp_owner(const gchar *name, const gchar *owner_sub)
{

    if (!name || !*name || !owner_sub || !*owner_sub)
        return FALSE;

    gchar *path = _pcv_lxc_owner_path(name);
    if (!path)
        return FALSE;

    GError *err = NULL;
    gboolean ok = g_file_set_contents(path, owner_sub, -1, &err);
    if (!ok) {
        PCV_LOG_WARN(LXC_OWNER_LOG_DOM,
                     "owner 스탬프 실패 '%s': %s",
                     name, err ? err->message : "unknown");
        g_clear_error(&err);
    }
    g_free(path);
    return ok;
}

gchar *
pcv_lxc_read_owner(const gchar *name)
{
    gchar *path = _pcv_lxc_owner_path(name);
    if (!path)
        return NULL;

    gchar *content = NULL;
    if (g_file_get_contents(path, &content, NULL, NULL) && content) {
        g_strstrip(content);
        if (content[0]) {
            g_free(path);
            return content;
        }
        g_free(content);
    }
    g_free(path);
    return NULL;
}
