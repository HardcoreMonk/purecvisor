#include "rest_auth.h"

#include "utils/pcv_crypto.h"

gboolean
pcv_rest_auth_should_fallback_bootstrap(const gchar *username,
                                        const gchar *password,
                                        const gchar *cfg_user,
                                        const gchar *cfg_pass,
                                        gboolean user_in_db)
{
    if (!username || !password || !cfg_user || !cfg_pass) return FALSE;
    if (g_strcmp0(username, cfg_user) != 0 ||
        !pcv_secret_str_eq(password, cfg_pass)) return FALSE;
    return !user_in_db;
}
