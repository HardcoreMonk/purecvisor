#ifndef PCV_REST_AUTH_H
#define PCV_REST_AUTH_H

#include <glib.h>

gboolean pcv_rest_auth_should_fallback_bootstrap(const gchar *username,
                                                 const gchar *password,
                                                 const gchar *cfg_user,
                                                 const gchar *cfg_pass,
                                                 gboolean user_in_db);

#endif
