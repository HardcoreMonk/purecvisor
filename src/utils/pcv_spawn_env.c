#include "pcv_spawn_env.h"

#include <string.h>

static const gchar * const _spawn_env_allow[] = {
    "USER", "TZ", "TMPDIR",
    "LIBVIRT_DEFAULT_URI", "LIBVIRT_LOG_OUTPUTS", "LIBVIRT_LOG_FILTERS",
    "ZFS_MODULE_TIMEOUT",
    "http_proxy", "https_proxy", "no_proxy", "HTTP_PROXY", "HTTPS_PROXY", "NO_PROXY",
    "SSL_CERT_FILE", "SSL_CERT_DIR", "CURL_CA_BUNDLE",
    NULL
};

static gboolean
_env_entry_allowed(const gchar *entry)
{
    const gchar *eq = strchr(entry, '=');
    if (!eq)
        return FALSE;
    gsize klen = (gsize)(eq - entry);
    for (const gchar * const *k = _spawn_env_allow; *k; k++) {
        if (strlen(*k) == klen && strncmp(*k, entry, klen) == 0)
            return TRUE;
    }
    return FALSE;
}

void
pcv_spawn_env_apply(GSubprocessLauncher *launcher)
{
    gchar **cur = g_get_environ();
    GPtrArray *base = g_ptr_array_new_with_free_func(g_free);
    for (gchar **e = cur; e && *e; e++) {
        if (_env_entry_allowed(*e))
            g_ptr_array_add(base, g_strdup(*e));
    }
    g_ptr_array_add(base, NULL);
    g_strfreev(cur);

    g_subprocess_launcher_set_environ(launcher, (gchar **)base->pdata);
    g_ptr_array_free(base, TRUE);

    g_subprocess_launcher_setenv(launcher, "PATH", "/usr/sbin:/usr/bin:/sbin:/bin", TRUE);
    g_subprocess_launcher_setenv(launcher, "HOME", "/root", TRUE);
    g_subprocess_launcher_setenv(launcher, "LANG", "C.UTF-8", TRUE);
}
