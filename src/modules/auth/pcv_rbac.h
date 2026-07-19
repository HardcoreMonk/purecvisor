
#ifndef PCV_RBAC_H
#define PCV_RBAC_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

typedef enum {
    PCV_ROLE_VIEWER   = 0,
    PCV_ROLE_OPERATOR = 1,
    PCV_ROLE_ADMIN    = 2
} PcvRole;

typedef struct {
    gchar  *username;
    PcvRole role;
    gchar  *tenant;
} PcvUser;

void pcv_rbac_init(const gchar *db_path);

void pcv_rbac_shutdown(void);

gboolean pcv_rbac_user_create(const gchar *username,
                              const gchar *password,
                              PcvRole      role,
                              const gchar *tenant,
                              GError     **error);

typedef enum {
    PCV_USER_ABSENT  = 0,
    PCV_USER_PRESENT = 1,
    PCV_USER_UNKNOWN = 2
} PcvUserExistence;

PcvUserExistence pcv_rbac_user_exists(const gchar *username);

gboolean pcv_rbac_user_delete(const gchar *username, GError **error);

GPtrArray *pcv_rbac_user_list(void);

gboolean pcv_rbac_user_set_role(const gchar *username,
                                PcvRole      role,
                                GError     **error);

gboolean pcv_rbac_change_password(const gchar *username,
                                  const gchar *old_password,
                                  const gchar *new_password,
                                  GError     **error);

gchar *pcv_rbac_authenticate(const gchar *username,
                             const gchar *password,
                             GError     **error);

gchar *pcv_rbac_authenticate_v2(const gchar *username,
                                const gchar *password,
                                gchar      **out_refresh_token,
                                GError     **error);

gchar *pcv_rbac_refresh_token(const gchar *refresh_token,
                              gchar      **out_new_refresh,
                              GError     **error);

gboolean pcv_rbac_revoke_session(const gchar *username, GError **error);

gint pcv_rbac_cleanup_expired_sessions(void);

JsonArray *pcv_rbac_list_sessions(const gchar *username);

gboolean pcv_rbac_revoke_session_by_id(const gchar *username,
                                       gint64       session_id);

[[nodiscard]] gboolean pcv_rbac_check_permission(const gchar *username,
                                   const gchar *method);

PcvRole pcv_rbac_get_role(const gchar *username);

const gchar *pcv_rbac_get_tenant(const gchar *username);

gchar *pcv_rbac_verify_api_key(const gchar *api_key,
                               PcvRole     *out_role,
                               GError     **error);

gboolean pcv_rbac_is_locked(const gchar *username);

gint pcv_rbac_get_remaining_lockout(const gchar *username);

gint pcv_rbac_get_ip_remaining_lockout(const gchar *ip);
void pcv_rbac_ip_record_auth_failure(const gchar *ip);
void pcv_rbac_ip_record_auth_success(const gchar *ip);

const gchar *pcv_rbac_role_to_str(PcvRole role);

PcvRole      pcv_rbac_str_to_role(const gchar *str);

void pcv_user_free(PcvUser *u);

gboolean   pcv_rbac_apikey_create(const gchar *client_name, PcvRole role,
                                  const gchar *description, gint64 expires_at,
                                  gchar **out_key, GError **error);
gint       pcv_rbac_apikey_validate(const gchar *api_key);
JsonArray *pcv_rbac_apikey_list(void);
gboolean   pcv_rbac_apikey_revoke(const gchar *client_name, GError **error);

void pcv_rbac_perm_cache_init(void);
void pcv_rbac_perm_cache_invalidate(const gchar *username);
gint pcv_rbac_perm_cache_check(const gchar *username, const gchar *method);
void pcv_rbac_perm_cache_set(const gchar *username, const gchar *method, gboolean allowed);

gboolean pcv_rbac_check_user_rate(const gchar *username);

gboolean pcv_rbac_check_quota(const gchar *username, gint current_vm_count);

gboolean pcv_rbac_set_quota(const gchar *username, gint vm_count, gint storage_gb);

G_END_DECLS

#endif
