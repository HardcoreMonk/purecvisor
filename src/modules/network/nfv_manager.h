
#ifndef PURECVISOR_NFV_MANAGER_H
#define PURECVISOR_NFV_MANAGER_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

void pcv_nfv_init(void);
void pcv_nfv_shutdown(void);

gboolean    pcv_nfv_lb_create(const gchar *name, const gchar *vip, gint port,
                               const gchar *backends, GError **error);

gboolean    pcv_nfv_lb_delete(const gchar *name, GError **error);

JsonArray  *pcv_nfv_lb_list(void);

gboolean    pcv_nfv_fw_policy_create(const gchar *name, const gchar *sw, GError **error);

gboolean    pcv_nfv_fw_policy_delete(const gchar *name, GError **error);

JsonArray  *pcv_nfv_fw_policy_list(const gchar *sw);

gboolean    pcv_nfv_chain_create(const gchar *name, const gchar *steps_json, GError **error);

gboolean    pcv_nfv_chain_delete(const gchar *name, GError **error);

JsonArray  *pcv_nfv_chain_list(void);

G_END_DECLS
#endif
