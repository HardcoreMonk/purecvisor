
#ifndef PCV_REST_MIDDLEWARE_H
#define PCV_REST_MIDDLEWARE_H

#include <glib.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

gint pcv_get_endpoint_rate_limit(const gchar *path, const gchar *http_method);

gchar *pcv_build_rate_limit_key(const gchar *client_ip,
                                const gchar *path,
                                const gchar *http_method);

gint pcv_get_rpc_timeout(const gchar *rpc_method);

gchar *pcv_compute_etag(const gchar *body, gsize len);

gboolean pcv_validate_required(SoupServerMessage *msg, JsonObject *body,
                               const gchar *fields[], gint count);

void pcv_rest_error(SoupServerMessage *msg, guint status,
                    const gchar *code, const gchar *detail);

#endif
