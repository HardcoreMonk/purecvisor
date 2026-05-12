#ifndef PURECVISOR_HIPS_ACTIONS_H
#define PURECVISOR_HIPS_ACTIONS_H

#include "modules/security/security_event.h"

G_BEGIN_DECLS






gboolean pcv_hips_action_is_executable(const gchar *action);
JsonObject *pcv_hips_action_build_pending(const PcvSecurityEvent *ev);
JsonArray *pcv_hips_action_list_pending(void);
gboolean pcv_hips_action_approve(const gchar *event_id,
                                  const gchar *admin_user,
                                  GError **error);
gboolean pcv_hips_action_dismiss(const gchar *event_id,
                                  const gchar *admin_user,
                                  const gchar *reason,
                                  GError **error);
gboolean pcv_hips_action_execute(const gchar *action,
                                 const gchar *target,
                                 GError **error);
gboolean pcv_hips_action_ensure_nft_input_chain(GError **error);
gboolean pcv_hips_action_build_block_ip_argv(const gchar *ip,
                                             const gchar **argv,
                                             gsize argv_len);
gboolean pcv_hips_action_validate_api_key_target(const gchar *client_name);

G_END_DECLS

#endif
