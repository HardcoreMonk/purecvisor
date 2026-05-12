









































#ifndef PCV_REST_SERVER_H
#define PCV_REST_SERVER_H

#include <glib-object.h>
#include "dispatcher.h"

G_BEGIN_DECLS

#define PCV_TYPE_REST_SERVER (pcv_rest_server_get_type())

G_DECLARE_FINAL_TYPE(PcvRestServer, pcv_rest_server,
                     PCV, REST_SERVER, GObject)








PcvRestServer *pcv_rest_server_new(PureCVisorDispatcher *dispatcher,
                                   guint16               port);





gboolean pcv_rest_server_start(PcvRestServer *self, GError **error);





void pcv_rest_server_stop(PcvRestServer *self);

G_END_DECLS

#endif
