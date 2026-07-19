#ifndef PURECVISOR_WS_SERVER_H
#define PURECVISOR_WS_SERVER_H

#include <glib.h>
#include <libsoup/soup.h>

G_BEGIN_DECLS

void pcv_ws_server_init(SoupServer *soup);

void pcv_ws_server_shutdown(void);

void pcv_ws_broadcast(const gchar *type, const gchar *payload_json);

gint pcv_ws_client_count(void);

void pcv_ws_broadcast_job_complete(const gchar *job_id, const gchar *method,
                                    const gchar *status, const gchar *error_msg);

void pcv_ws_broadcast_job_complete_mt(const gchar *job_id, const gchar *method,
                                       const gchar *status, const gchar *error_msg);

G_END_DECLS

#endif
