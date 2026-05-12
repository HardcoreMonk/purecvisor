























































#ifndef PURECVISOR_HANDLER_TEMPLATE_H
#define PURECVISOR_HANDLER_TEMPLATE_H

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS


typedef struct _UdsServer UdsServer;






void handle_template_list  (JsonObject *params, const gchar *rpc_id,
                            UdsServer *server, GSocketConnection *conn);







void handle_template_get   (JsonObject *params, const gchar *rpc_id,
                            UdsServer *server, GSocketConnection *conn);








void handle_template_create(JsonObject *params, const gchar *rpc_id,
                            UdsServer *server, GSocketConnection *conn);







void handle_template_delete(JsonObject *params, const gchar *rpc_id,
                            UdsServer *server, GSocketConnection *conn);

G_END_DECLS

#endif
