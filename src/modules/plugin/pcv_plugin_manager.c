   
                             
                                                      
  
                           
                                                   
                                                    
                                        
  
          
                                                              
                                                   
                                   
  
          
                                                 
                                                    
                                                  
                                              
                                                                    
                                                                 
  
            
                   
                                                              
                                                                     
                           
                                                                          
                                  
  
                     
                                      
                                                                       
                                                                  
                                                                                 
  
                                                          
  
                                                                                             
                                                                                                          
                                                                                 
                                              
  
          
                                                                 
                                                     
                                                                   
  
           
                                              
                                                   
                                       
                                      
  
        
                                                                               
                                                           
                                       
         
                                                         
                                                                     
        
                                                                                
                                                         
                                                                                              
                                                                                             
                                                                                                         
                                                                             
                                                                                                
                                                                                       
              
  
                                 
   
#include "pcv_plugin_manager.h"
#include "utils/pcv_log.h"
#include "../audit/pcv_audit.h"                    
#include "modules/dispatcher/rpc_utils.h"                                                                 
#include "api/uds_server.h"                                                                 
#include <gmodule.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
    gint           fd;                                                                                   
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
                                                                                
                                                                                 
    gboolean          in_register;
                                                                                                
                                    
    GArray           *held_fds;
} G = {0};

                                                                                      
static void
_hold_fd(gint fd)
{
    if (fd < 0) return;
    if (!G.held_fds) G.held_fds = g_array_new(FALSE, FALSE, sizeof(gint));
    g_array_append_val(G.held_fds, fd);
}

   
                           
                                                  
                                                   
                                                     
                           
  
                                   
                                             
                                  
                                                
                                                      
                                                   
   
                                                                                                
void pcv_plugin_registry_add(PcvPluginRegistry *reg, const gchar *method_name,
                              PcvRpcHandler handler)
{
    if (!reg || !method_name || !handler) return;                                  
                                                                                            
                                                                                        
                                                                
    if (!G.in_register) {
        PCV_LOG_WARN(PLUG_LOG_DOM, "Plugin method '%s' rejected: pcv_plugin_registry_add called outside pcv_plugin_register",
                     method_name);
        return;
    }
                                                                    
                                                        
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

                                                          
                                                             
                                                                                   
                                                                                           
                                                                                
                            
static gboolean
_respond_target_ok(GSocketConnection *connection, const gchar *what)
{
    if (!connection || !G_IS_SOCKET_CONNECTION(connection)) {                               
        PCV_LOG_WARN(PLUG_LOG_DOM, "Plugin %s ignored: no client connection", what);
        return FALSE;
    }
    if (g_io_stream_is_closed(G_IO_STREAM(connection))) {                               
        PCV_LOG_WARN(PLUG_LOG_DOM, "Plugin %s ignored: response already sent on this connection", what);
        return FALSE;
    }
    return TRUE;
}

   
                              
                                                                 
                                                             
                                                                                             
                                       
   
gboolean pcv_plugin_respond_success(const gchar *rpc_id, gpointer server,
                                    GSocketConnection *connection, JsonNode *result)
{
    if (!_respond_target_ok(connection, "success response")) {
        if (result) json_node_free(result);                                             
        return FALSE;
    }
    gchar *resp = pure_rpc_build_success_response(rpc_id, result);                                        
    pure_uds_server_send_response((UdsServer *)server, connection, resp);                   
    g_free(resp);
    return TRUE;
}

   
                            
                                                          
                                                                        
                                                                                             
                                       
   
gboolean pcv_plugin_respond_error(const gchar *rpc_id, gpointer server,
                                  GSocketConnection *connection, gint code, const gchar *message)
{
    if (!_respond_target_ok(connection, "error response"))
        return FALSE;
    if (code == 0) code = PURE_RPC_ERR_INTERNAL_ERROR;
    gchar *resp = pure_rpc_build_error_response(rpc_id, (PureRpcErrorCode)code, message);                           
    pure_uds_server_send_response((UdsServer *)server, connection, resp);
    g_free(resp);
    return TRUE;
}

                                                                                           
                                                                              
                                                                             
                                                                                     
                                                                                             
                                                      

                                                                                                  
static gchar *
_trust_problem(const struct stat *st)
{
    uid_t me = geteuid();
    if (st->st_uid != 0 && st->st_uid != me)
        return g_strdup_printf("owner uid %u is neither root nor the daemon user (uid %u)", (guint)st->st_uid, (guint)me);
    if (st->st_mode & (S_IWGRP | S_IWOTH))
        return g_strdup_printf("group/other writable (mode %04o)", (guint)(st->st_mode & 07777));
    return NULL;
}

                                                                                     
                                                                                     
                                                     
                                                                                      
                                                                                 
                                                                                      
static gchar *
_dir_chain_problem(const gchar *dir, gchar **out_real)
{
    *out_real = NULL;
    char *real = realpath(dir, NULL);
    if (!real)
        return g_strdup_printf("%s: cannot resolve path: %s", dir, g_strerror(errno));
    gchar *cur = g_strdup(real);
    gchar *problem = NULL;
    for (;;) {
        struct stat st;
        if (lstat(cur, &st) != 0)
            problem = g_strdup_printf("%s: cannot stat: %s", cur, g_strerror(errno));
        else if (!S_ISDIR(st.st_mode))
            problem = g_strdup_printf("%s: not a directory", cur);
        else {
            gchar *why = _trust_problem(&st);
            if (why) { problem = g_strdup_printf("%s: %s", cur, why); g_free(why); }
        }
        if (problem || strcmp(cur, "/") == 0)
            break;
        gchar *parent = g_path_get_dirname(cur);
        g_free(cur);
        cur = parent;
    }
    g_free(cur);
    if (!problem)
        *out_real = g_strdup(real);
    free(real);
    return problem;
}

                                                                                   
                                                   
static gboolean
_load_fail(GError **error, gint code, const gchar *path, const gchar *reason)
{
    g_set_error(error, g_quark_from_static_string("plugin"), code, "%s", reason);
    gchar *target = g_strdup_printf("%s (%s)", path, reason);
    pcv_audit_log(PCV_AUDIT_ACTOR_SYSTEM, "plugin.load", target, "fail", code, 0, "local");
    g_free(target);
    return FALSE;
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
    g_dir_close(dir);
                                                                         
                                                        
    gchar *real = NULL;
    gchar *problem = _dir_chain_problem(G.plugin_dir, &real);
    if (problem) {
        PCV_LOG_WARN(PLUG_LOG_DOM, "Plugin dir '%s' rejected — no plugins loaded: %s", G.plugin_dir, problem);
        gchar *target = g_strdup_printf("%s (%s)", G.plugin_dir, problem);
        pcv_audit_log(PCV_AUDIT_ACTOR_SYSTEM, "plugin.load", target, "fail", 7, 0, "local");
        g_free(target);
        g_free(problem);
        PCV_LOG_INFO(PLUG_LOG_DOM, "Plugin manager initialized (0 plugins, 0 methods)");
        return;
    }
    dir = g_dir_open(real, 0, &dir_err);
    if (!dir) {                                       
        PCV_LOG_INFO(PLUG_LOG_DOM, "Plugin dir '%s' unavailable: %s", real, dir_err ? dir_err->message : "not found");
        g_clear_error(&dir_err);
        g_free(real);
        return;
    }
    const gchar *name;
    gint loaded = 0;                            
                                                
    while ((name = g_dir_read_name(dir)) != NULL) {                     
        if (!g_str_has_suffix(name, ".so")) continue;                        
        gchar *path = g_build_filename(real, name, NULL);                                        
        GError *err = NULL;
        if (pcv_plugin_load(path, &err))                 
            loaded++;
        else if (err) {                                                   
            PCV_LOG_WARN(PLUG_LOG_DOM, "Failed to load %s: %s", name, err->message);
            g_error_free(err);                    
        }
        g_free(path);                         
    }
    g_free(real);
    g_dir_close(dir);

    PCV_LOG_INFO(PLUG_LOG_DOM, "Plugin manager initialized (%d plugins, %d methods)",
                 loaded, G.registry.count);
}

   
                               
                                                    
  
                                                                        
                                                        
                                                                                     
                                                                                        
                                        
                                     
   
void pcv_plugin_manager_shutdown(void)
{
    if (!G.initialized) return;                                            
    G.initialized = FALSE;                                                            
    G.registry.count = 0;                                                   
    gint n = G.plugin_count, called = 0;
    for (gint i = 0; i < n; i++) {                                        
        if (G.plugins[i].shutdown_fn) {                          
            G.plugins[i].shutdown_fn();                                                          
            called++;
        }
        if (G.plugins[i].module)
            g_module_close(G.plugins[i].module);                         
    }
                                                                                          
    for (gint i = 0; i < n; i++)
        if (G.plugins[i].fd >= 0)
            close(G.plugins[i].fd);
    if (G.held_fds) {
        for (guint k = 0; k < G.held_fds->len; k++)
            close(g_array_index(G.held_fds, gint, k));
        g_clear_pointer(&G.held_fds, g_array_unref);
    }
    memset(G.plugins, 0, sizeof(G.plugins));                 
    G.plugin_count = 0;
    g_clear_pointer(&G.plugin_dir, g_free);                              
    if (n > 0)                                               
        PCV_LOG_INFO(PLUG_LOG_DOM, "Plugin manager shut down (%d plugins, %d shutdown callbacks)", n, called);
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
    if (!G.initialized || !method) return;                                                      
    for (gint i = 0; i < G.registry.count; i++) {                 
        if (g_strcmp0(G.registry.methods[i].method, method) == 0) {                       
            G.registry.methods[i].handler(params, rpc_id, server, connection);                           
                                                                              
                                                                  
            if (connection && G_IS_SOCKET_CONNECTION(connection) && !g_io_stream_is_closed(G_IO_STREAM(connection))) {
                PCV_LOG_WARN(PLUG_LOG_DOM, "Plugin method '%s' (owner '%s') returned without a response — sending -32603",
                             method, G.registry.methods[i].owner);
                gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
                                                           "plugin method returned without a response");
                pure_uds_server_send_response((UdsServer *)server, connection, err);
                g_free(err);
            }
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
                                                                                    
    if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode))                                                       
        return _load_fail(error, 6, path, "not a regular file (symlink?)");

                                                                                     
    gint fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        gint e = errno;
        if (e == ELOOP)
            return _load_fail(error, 6, path, "became a symbolic link during the check");
        gchar *why = g_strdup_printf("cannot open: %s", g_strerror(e));
        g_set_error(error, g_quark_from_static_string("plugin"), 2, "%s", why);
        g_free(why);
        pcv_audit_log(PCV_AUDIT_ACTOR_SYSTEM, "plugin.load", path, "fail", 2, 0, "local");
        return FALSE;
    }
    struct stat fst;
    if (fstat(fd, &fst) != 0 || !S_ISREG(fst.st_mode) || fst.st_dev != st.st_dev || fst.st_ino != st.st_ino) {
        close(fd);
        return _load_fail(error, 6, path, "file changed during the check");
    }
    gchar *untrusted = _trust_problem(&fst);
    if (untrusted) {
        close(fd);
        gboolean r = _load_fail(error, 8, path, untrusted);
        g_free(untrusted);
        return r;
    }
                                                                                          
    gchar *modpath = g_strdup_printf("/proc/self/fd/%d", fd);
    struct stat pst;
    if (stat(modpath, &pst) != 0 || pst.st_dev != fst.st_dev || pst.st_ino != fst.st_ino) {
        struct stat again;
        g_free(modpath);
        if (lstat(path, &again) != 0 || !S_ISREG(again.st_mode) || again.st_dev != fst.st_dev || again.st_ino != fst.st_ino) {
            close(fd);
            return _load_fail(error, 6, path, "file changed during the check");
        }
        modpath = g_strdup(path);
    }

                                                               
    GModule *mod = g_module_open(modpath, G_MODULE_BIND_LOCAL);
    g_free(modpath);
    if (!mod) {                                  
        g_set_error(error, g_quark_from_static_string("plugin"), 2, "%s", g_module_error());
        _hold_fd(fd);                                                              
        pcv_audit_log(PCV_AUDIT_ACTOR_SYSTEM, "plugin.load", path, "fail", 2, 0, "local");
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
        _hold_fd(fd);                                                              
        pcv_audit_log(PCV_AUDIT_ACTOR_SYSTEM, "plugin.load", path, "fail", 3, 0, "local");
        return FALSE;
    }
                                                                
    if (!g_module_symbol(mod, "pcv_plugin_shutdown", (gpointer*)&shut_fn)) {
        shut_fn = NULL;                                           
    }

    const PcvPluginMeta *meta = get_meta();                              
                                                         
    if (!meta || meta->abi_version != PCV_PLUGIN_ABI_VERSION) {
        g_set_error(error, g_quark_from_static_string("plugin"), 4, "ABI version mismatch");
        g_module_close(mod);
        _hold_fd(fd);                                                              
        pcv_audit_log(PCV_AUDIT_ACTOR_SYSTEM, "plugin.load", path, "fail", 4, 0, "local");
        return FALSE;
    }
                                                                              
    if (!meta->name || !meta->name[0]) {
                                                                         
        const gchar *why = meta->name ? "plugin name is empty" : "plugin name is NULL";
        g_module_close(mod);
        _hold_fd(fd);                                                              
        return _load_fail(error, 9, path, why);
    }

    gint idx = G.plugin_count++;                                           
    g_strlcpy(G.plugins[idx].name, meta->name, sizeof(G.plugins[idx].name));                         
    g_strlcpy(G.plugins[idx].version, meta->version ? meta->version : "", sizeof(G.plugins[idx].version));                          
    G.plugins[idx].module = mod;                                              
    G.plugins[idx].shutdown_fn = shut_fn;                        
    G.plugins[idx].fd = fd;                                                                     

                                                
                                                               
    g_strlcpy(G.current_loading_plugin, meta->name, sizeof(G.current_loading_plugin));
    G.in_register = TRUE;                                                         
    reg_fn(&G.registry);                                                                  
    G.in_register = FALSE;                              
    G.current_loading_plugin[0] = '\0';                       

                                   
    gchar *bn = g_path_get_basename(path);                    
    PCV_LOG_INFO(PLUG_LOG_DOM, "Loaded plugin: %s v%s (%s)", G.plugins[idx].name, G.plugins[idx].version, bn);
    g_free(bn);
    pcv_audit_log(PCV_AUDIT_ACTOR_SYSTEM, "plugin.load", G.plugins[idx].name, "ok", 0, 0, "local");                     
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
            _hold_fd(G.plugins[i].fd);                                                                           
                                   
            if (i < G.plugin_count - 1) {                                   
                G.plugins[i] = G.plugins[G.plugin_count - 1];
            }
            memset(&G.plugins[G.plugin_count - 1], 0, sizeof(LoadedPlugin));                              
            G.plugin_count--;                     
            PCV_LOG_INFO(PLUG_LOG_DOM,
                "Unloaded plugin: %s (removed %d methods from registry)",
                name, removed);
            pcv_audit_log(PCV_AUDIT_ACTOR_SYSTEM, "plugin.unload", name, "ok", 0, 0, "local");                  
            return TRUE;
        }
    }
                                        
    g_set_error(error, g_quark_from_static_string("plugin"), 5, "plugin '%s' not found", name);
    pcv_audit_log(PCV_AUDIT_ACTOR_SYSTEM, "plugin.unload", name, "fail", 5, 0, "local");
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
