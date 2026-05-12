






#ifndef TUI_RPC_H
#define TUI_RPC_H

#include <glib.h>
#include <json-glib/json-glib.h>


#define SOCKET_PATH     "/var/run/purecvisor/daemon.sock"
#define SOCK_TIMEOUT_S  10



const gchar *safe_str(JsonObject *obj, const gchar *key, const gchar *def);
double safe_double(JsonObject *obj, const gchar *key);
gint64 safe_int(JsonObject *obj, const gchar *key);
void format_bytes(unsigned long long b, char *out, int sz);



gchar *tui_send_request(const gchar *method, JsonObject *params, GError **error);

#endif
