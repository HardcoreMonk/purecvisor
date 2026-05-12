





































#include "pcv_plugin_manager.h"
#include "utils/pcv_log.h"
#include "../audit/pcv_audit.h"
#include <gmodule.h>
#include <string.h>
#include <sys/stat.h>

#define PLUG_LOG_DOM "plugin_mgr"
constexpr int MAX_PLUGINS = 16;
constexpr int MAX_METHODS = 64;


static_assert(MAX_PLUGINS >= 1);
static_assert(MAX_METHODS >= 1);









typedef struct {
    gchar          name[64];
    gchar          version[32];
    GModule       *module;
    PcvPluginShutdownFunc shutdown_fn;
} LoadedPlugin;







typedef struct {
    gchar          method[128];
    PcvRpcHandler  handler;
    gchar          owner[64];
} MethodEntry;







struct _PcvPluginRegistry {
    MethodEntry methods[MAX_METHODS];
    gint        count;
};



static struct {
    LoadedPlugin      plugins[MAX_PLUGINS];
    gint              plugin_count;
    PcvPluginRegistry registry;
    gchar            *plugin_dir;
    gboolean          initialized;

    gchar             current_loading_plugin[64];
} G = {0};











void pcv_plugin_registry_add(PcvPluginRegistry *reg, const gchar *method_name,
                              PcvRpcHandler handler)
{
    if (!reg || !method_name || !handler) return;

    if (reg->count >= MAX_METHODS) {
        PCV_LOG_WARN(PLUG_LOG_DOM,
            "Plugin registry full (%d) — cannot register '%s' from '%s'",
            MAX_METHODS, method_name, G.current_loading_plugin);
        return;
    }

    for (gint j = 0; j < reg->count; j++) {
        if (g_strcmp0(reg->methods[j].method, method_name) == 0) {
            PCV_LOG_WARN(PLUG_LOG_DOM,
                "Duplicate plugin method '%s' (owner '%s') — keeping first",
                method_name, reg->methods[j].owner);
            return;
        }
    }
    g_strlcpy(reg->methods[reg->count].method, method_name, sizeof(reg->methods[0].method));
    reg->methods[reg->count].handler = handler;

    g_strlcpy(reg->methods[reg->count].owner, G.current_loading_plugin,
              sizeof(reg->methods[0].owner));
    reg->count++;
    PCV_LOG_INFO(PLUG_LOG_DOM, "Registered plugin method: %s (owner: %s)",
                 method_name, G.current_loading_plugin);
}








void pcv_plugin_manager_init(const gchar *plugin_dir)
{
    G.registry.count = 0;
    G.plugin_count = 0;
    G.plugin_dir = g_strdup(plugin_dir ? plugin_dir : "/etc/purecvisor/plugins.d");
    G.initialized = TRUE;

    if (!g_module_supported()) {
        PCV_LOG_WARN(PLUG_LOG_DOM, "GModule not supported on this platform");
        return;
    }


    GError *dir_err = NULL;
    GDir *dir = g_dir_open(G.plugin_dir, 0, &dir_err);
    if (!dir) {
        PCV_LOG_INFO(PLUG_LOG_DOM, "Plugin dir '%s' unavailable: %s",
                     G.plugin_dir, dir_err ? dir_err->message : "not found");
        g_clear_error(&dir_err);
        return;
    }
    const gchar *name;
    gint loaded = 0;
    while ((name = g_dir_read_name(dir)) != NULL) {
        if (!g_str_has_suffix(name, ".so")) continue;
        gchar *path = g_build_filename(G.plugin_dir, name, NULL);
        GError *err = NULL;
        if (pcv_plugin_load(path, &err))
            loaded++;
        else if (err) {
            PCV_LOG_WARN(PLUG_LOG_DOM, "Failed to load %s: %s", name, err->message);
            g_error_free(err);
        }
        g_free(path);
    }
    g_dir_close(dir);

    PCV_LOG_INFO(PLUG_LOG_DOM, "Plugin manager initialized (%d plugins, %d methods)",
                 loaded, G.registry.count);
}









void pcv_plugin_manager_shutdown(void)
{
    for (gint i = 0; i < G.plugin_count; i++) {
        if (G.plugins[i].shutdown_fn)
            G.plugins[i].shutdown_fn();
        if (G.plugins[i].module)
            g_module_close(G.plugins[i].module);
    }
    g_free(G.plugin_dir);
    G.initialized = FALSE;
}










gboolean pcv_plugin_has_handler(const gchar *method)
{
    if (!G.initialized || !method) return FALSE;
    for (gint i = 0; i < G.registry.count; i++)
        if (g_strcmp0(G.registry.methods[i].method, method) == 0) return TRUE;
    return FALSE;
}












void pcv_plugin_dispatch(const gchar *method, JsonObject *params,
                          const gchar *rpc_id, gpointer server,
                          GSocketConnection *connection)
{
    for (gint i = 0; i < G.registry.count; i++) {
        if (g_strcmp0(G.registry.methods[i].method, method) == 0) {
            G.registry.methods[i].handler(params, rpc_id, server, connection);
            return;
        }
    }
}






















gboolean pcv_plugin_load(const gchar *path, GError **error)
{
    if (G.plugin_count >= MAX_PLUGINS) {
        g_set_error(error, g_quark_from_static_string("plugin"), 1, "max plugins reached");
        return FALSE;
    }


    struct stat st;
    if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        g_set_error(error, g_quark_from_static_string("plugin"), 6,
                    "not a regular file (symlink?)");

        gchar *bn = g_path_get_basename(path);
        PCV_LOG_WARN(PLUG_LOG_DOM, "Skipping '%s': not a regular file (symlink?)", bn);
        g_free(bn);
        pcv_audit_log(NULL, "plugin.load", path, "fail", 6, 0, "local");
        return FALSE;
    }

    GModule *mod = g_module_open(path, G_MODULE_BIND_LOCAL);
    if (!mod) {
        g_set_error(error, g_quark_from_static_string("plugin"), 2, "%s", g_module_error());
        pcv_audit_log(NULL, "plugin.load", path, "fail", 2, 0, "local");
        return FALSE;
    }

    PcvPluginGetMetaFunc get_meta = NULL;
    PcvPluginRegisterFunc reg_fn = NULL;
    PcvPluginShutdownFunc shut_fn = NULL;

    if (!g_module_symbol(mod, "pcv_plugin_get_meta", (gpointer*)&get_meta) ||
        !get_meta ||
        !g_module_symbol(mod, "pcv_plugin_register", (gpointer*)&reg_fn) ||
        !reg_fn) {
        g_set_error(error, g_quark_from_static_string("plugin"), 3, "missing required symbols");
        g_module_close(mod);
        pcv_audit_log(NULL, "plugin.load", path, "fail", 3, 0, "local");
        return FALSE;
    }

    if (!g_module_symbol(mod, "pcv_plugin_shutdown", (gpointer*)&shut_fn)) {
        shut_fn = NULL;
    }

    const PcvPluginMeta *meta = get_meta();
    if (!meta || meta->abi_version != PCV_PLUGIN_ABI_VERSION) {
        g_set_error(error, g_quark_from_static_string("plugin"), 4, "ABI version mismatch");
        g_module_close(mod);
        pcv_audit_log(NULL, "plugin.load", path, "fail", 4, 0, "local");
        return FALSE;
    }

    gint idx = G.plugin_count++;
    g_strlcpy(G.plugins[idx].name, meta->name, sizeof(G.plugins[idx].name));
    g_strlcpy(G.plugins[idx].version, meta->version, sizeof(G.plugins[idx].version));
    G.plugins[idx].module = mod;
    G.plugins[idx].shutdown_fn = shut_fn;


    g_strlcpy(G.current_loading_plugin, meta->name, sizeof(G.current_loading_plugin));
    reg_fn(&G.registry);
    G.current_loading_plugin[0] = '\0';


    gchar *bn = g_path_get_basename(path);
    PCV_LOG_INFO(PLUG_LOG_DOM, "Loaded plugin: %s v%s (%s)", meta->name, meta->version, bn);
    g_free(bn);
    pcv_audit_log(NULL, "plugin.load", meta->name, "ok", 0, 0, "local");
    return TRUE;
}















gboolean pcv_plugin_unload(const gchar *name, GError **error)
{
    for (gint i = 0; i < G.plugin_count; i++) {
        if (g_strcmp0(G.plugins[i].name, name) == 0) {



            gint removed = 0;
            for (gint j = G.registry.count - 1; j >= 0; j--) {
                if (g_strcmp0(G.registry.methods[j].owner, name) == 0) {
                    if (j < G.registry.count - 1) {
                        G.registry.methods[j] = G.registry.methods[G.registry.count - 1];
                    }
                    G.registry.count--;
                    removed++;
                }
            }
            if (G.plugins[i].shutdown_fn) {
                G.plugins[i].shutdown_fn();
                G.plugins[i].shutdown_fn = NULL;
            }
            if (G.plugins[i].module) g_module_close(G.plugins[i].module);

            if (i < G.plugin_count - 1) {
                G.plugins[i] = G.plugins[G.plugin_count - 1];
            }
            memset(&G.plugins[G.plugin_count - 1], 0, sizeof(LoadedPlugin));
            G.plugin_count--;
            PCV_LOG_INFO(PLUG_LOG_DOM,
                "Unloaded plugin: %s (removed %d methods from registry)",
                name, removed);
            pcv_audit_log(NULL, "plugin.unload", name, "ok", 0, 0, "local");
            return TRUE;
        }
    }
    g_set_error(error, g_quark_from_static_string("plugin"), 5, "plugin '%s' not found", name);
    pcv_audit_log(NULL, "plugin.unload", name, "fail", 5, 0, "local");
    return FALSE;
}









JsonArray *pcv_plugin_list(void)
{
    JsonArray *arr = json_array_new();
    for (gint i = 0; i < G.plugin_count; i++) {
        JsonObject *p = json_object_new();
        json_object_set_string_member(p, "name", G.plugins[i].name);
        json_object_set_string_member(p, "version", G.plugins[i].version);
        json_array_add_object_element(arr, p);
    }

    for (gint i = 0; i < G.registry.count; i++) {

    }
    return arr;
}
