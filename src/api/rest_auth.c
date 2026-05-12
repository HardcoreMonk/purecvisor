#include "rest_auth.h"

#include <gio/gio.h>









gboolean
pcv_rest_auth_should_fallback_bootstrap(const gchar *username,
                                        const gchar *password,
                                        const gchar *cfg_user,
                                        const gchar *cfg_pass,
                                        const GError *rbac_error)
{
    if (!username || !password || !cfg_user || !cfg_pass) {
        return FALSE;
    }

    if (g_strcmp0(username, cfg_user) != 0 ||
        g_strcmp0(password, cfg_pass) != 0) {
        return FALSE;
    }

    if (!rbac_error) {
        return TRUE;
    }

    if (rbac_error->domain != G_IO_ERROR ||
        rbac_error->code != G_IO_ERROR_PERMISSION_DENIED) {
        return FALSE;
    }

    return g_strcmp0(rbac_error->message, "Invalid credentials") == 0;
}
