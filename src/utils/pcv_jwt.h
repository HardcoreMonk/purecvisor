
#ifndef PCV_JWT_H
#define PCV_JWT_H

#include <glib.h>

G_BEGIN_DECLS

void pcv_jwt_init(const gchar *secret);

void pcv_jwt_shutdown(void);

void pcv_jwt_update_secret(const gchar *new_secret);

gchar *pcv_jwt_sign(const gchar *subject,
                    guint        expires_in,
                    GError     **error);

[[nodiscard]] gchar *pcv_jwt_verify(const gchar *token,
                      GError     **error);

gchar *pcv_jwt_sign_with_ip(const gchar *subject,
                            guint        expires_in,
                            const gchar *client_ip,
                            GError     **error);

gchar *pcv_jwt_verify_with_ip(const gchar *token,
                              const gchar *client_ip,
                              GError     **error);

void pcv_jwt_blacklist_add(const gchar *jti, gint64 expiry_unix);

gboolean pcv_jwt_blacklist_check(const gchar *jti);

void pcv_jwt_blacklist_sweep(void);

G_END_DECLS

#endif
