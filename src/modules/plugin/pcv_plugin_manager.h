



























#ifndef PURECVISOR_PLUGIN_MANAGER_H
#define PURECVISOR_PLUGIN_MANAGER_H

#include <glib.h>
#include <json-glib/json-glib.h>
#include <gio/gio.h>
#include "pcv_plugin_api.h"

G_BEGIN_DECLS


void       pcv_plugin_manager_init(const gchar *plugin_dir);


void       pcv_plugin_manager_shutdown(void);


gboolean   pcv_plugin_has_handler(const gchar *method);


void       pcv_plugin_dispatch(const gchar *method, JsonObject *params,
                                const gchar *rpc_id, gpointer server,
                                GSocketConnection *connection);


JsonArray *pcv_plugin_list(void);


gboolean   pcv_plugin_load(const gchar *path, GError **error);


gboolean   pcv_plugin_unload(const gchar *name, GError **error);

G_END_DECLS

#endif
