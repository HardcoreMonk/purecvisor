#ifndef PURECVISOR_SERVER_H
#define PURECVISOR_SERVER_H

#include <stdbool.h> // [수정] 필수 추가!
#include <glib.h>
#include <gio/gio.h>

/* Opaque Pointer */
typedef struct UdsServer UdsServer;

/* Server API */
UdsServer* uds_server_new(const char *socket_path);
bool uds_server_start(UdsServer *self, GError **error);
void uds_server_stop(UdsServer *self);

#endif