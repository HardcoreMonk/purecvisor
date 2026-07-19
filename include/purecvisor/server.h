
#ifndef PURECVISOR_SERVER_H
#define PURECVISOR_SERVER_H

#include <stdbool.h>
#include <glib.h>
#include <gio/gio.h>

typedef struct UdsServer UdsServer;

UdsServer* uds_server_new(const char *socket_path);
bool uds_server_start(UdsServer *self, GError **error);
void uds_server_stop(UdsServer *self);

#endif
