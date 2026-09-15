












#ifndef PURECVISOR_HOST_HARDWARE_INVENTORY_H
#define PURECVISOR_HOST_HARDWARE_INVENTORY_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS











JsonObject *pcv_host_hardware_inventory_collect(const gchar *root);

G_END_DECLS

#endif
