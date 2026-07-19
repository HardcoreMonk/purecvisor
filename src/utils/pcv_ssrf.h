
#ifndef PURECVISOR_SSRF_H
#define PURECVISOR_SSRF_H

#include <glib.h>

gboolean pcv_url_target_allowed(const gchar *url, GError **error);

#endif
