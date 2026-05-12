





























#ifndef PURECVISOR_PLUGIN_API_H
#define PURECVISOR_PLUGIN_API_H

#include <glib.h>
#include <json-glib/json-glib.h>
#include <gio/gio.h>


#define PCV_PLUGIN_ABI_VERSION 1










typedef struct {
    const gchar *name;
    const gchar *version;
    const gchar *author;
    guint        abi_version;
} PcvPluginMeta;










typedef void (*PcvRpcHandler)(JsonObject *params, const gchar *rpc_id,
                               gpointer server, GSocketConnection *connection);


typedef struct _PcvPluginRegistry PcvPluginRegistry;




typedef const PcvPluginMeta* (*PcvPluginGetMetaFunc)(void);


typedef void (*PcvPluginRegisterFunc)(PcvPluginRegistry *registry);


typedef void (*PcvPluginShutdownFunc)(void);












void pcv_plugin_registry_add(PcvPluginRegistry *reg,
                              const gchar *method_name,
                              PcvRpcHandler handler);

#endif
