
#ifndef PURECVISOR_UDS_SERVER_H
#define PURECVISOR_UDS_SERVER_H

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _PureCVisorDispatcher PureCVisorDispatcher;

#define PURECVISOR_TYPE_UDS_SERVER (uds_server_get_type())

G_DECLARE_FINAL_TYPE(UdsServer, uds_server, PURECVISOR, UDS_SERVER, GObject)

UdsServer *uds_server_new(const gchar *socket_path);

void uds_server_set_dispatcher(UdsServer *self, PureCVisorDispatcher *dispatcher);

gboolean uds_server_start(UdsServer *self, GError **error);

void uds_server_stop(UdsServer *self);

void pure_uds_server_send_response(UdsServer *self, GSocketConnection *connection, const gchar *response);

G_END_DECLS

#endif
