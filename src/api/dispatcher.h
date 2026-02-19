/* src/api/dispatcher.h */

#ifndef PURECVISOR_DISPATCHER_H
#define PURECVISOR_DISPATCHER_H

#include <glib-object.h>
#include <json-glib/json-glib.h>
#include <libvirt-gobject/libvirt-gobject.h>
#include "uds_server.h" // UdsServer 타입 인식을 위해 필요

G_BEGIN_DECLS

#define PURECVISOR_TYPE_DISPATCHER (purecvisor_dispatcher_get_type())

G_DECLARE_FINAL_TYPE(PureCVisorDispatcher, purecvisor_dispatcher, PURECVISOR, DISPATCHER, GObject)

PureCVisorDispatcher *purecvisor_dispatcher_new(void);

void purecvisor_dispatcher_set_connection(PureCVisorDispatcher *self, GVirConnection *conn);

/* [Phase 5 Updated Signature] 
 * 기존: (Dispatcher, JsonNode, OutputStream)
 * 변경: (Dispatcher, UdsServer, SocketConnection, RawString)
 */
void purecvisor_dispatcher_dispatch(PureCVisorDispatcher *self, 
                                   UdsServer *server, 
                                   GSocketConnection *connection, 
                                   const gchar *request_json);

G_END_DECLS

#endif /* PURECVISOR_DISPATCHER_H */