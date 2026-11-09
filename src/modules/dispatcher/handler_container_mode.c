                                                                             
                                                  
                                                                                      
                                                                           
                                                                                                                             
  
                           
                                                                               
                                                                                               
                                                                       
                                                                                         
                                                                                     
                                                                                                        
                                                                                            
                                                                                                        
                                     
                                                                                                         
                                                                              
  
                                                     
                                                                                

#include "handler_container_mode.h"
#include "modules/dispatcher/dispatcher_shared.h"
#include "modules/auth/pcv_rbac.h"
#include "modules/audit/pcv_audit.h"
#include "modules/resource/resource_gate.h"
#include "api/uds_server.h"
#include "utils/pcv_log.h"

#include <string.h>                                                  

                                                         
                                                   
static gboolean
_mode_reject(PcvContainerCreateMode *out, PureRpcErrorCode code, gchar *message)
{
    out->code = code;
    out->message = message;
    return FALSE;
}

                                                                                                        
                                                                                                        
                                                                                                            
                                                                                          
            
                                                                              
static gboolean
_bridge_allowed(JsonObject *params, GSocketConnection *conn, PcvContainerCreateMode *out)
{
    const gchar *bridge = params ? pcv_disp_json_string_member(params, "network_bridge") : NULL;
    if (!bridge || !*bridge)
        return TRUE;
    PcvResCaller c = { 0 };
    pcv_res_caller_init(&c, params, conn);
    PcvResDecision d = pcv_res_check(&c, PCV_RES_BRIDGE, bridge, NULL, NULL);
    pcv_res_caller_clear(&c);
    if (d == PCV_RES_ALLOW)
        return TRUE;
    pcv_res_audit_denied("container.create", "network_bridge", bridge);
    return _mode_reject(out, PURE_RPC_ERR_FORBIDDEN, pcv_res_decision_message(d, "network_bridge"));
}

                                                                                                
                                                                                   
gboolean
pcv_container_create_bridge_alias(JsonObject *params, gchar **message)
{
    if (!params || !json_object_has_member(params, "bridge"))
        return TRUE;
    const gchar *alias = pcv_rpc_params_get_string(params, "bridge");
    if (!alias) {
        *message = g_strdup("bridge must be a string");
        return FALSE;
    }
    if (json_object_has_member(params, "network_bridge")) {
        if (g_strcmp0(pcv_rpc_params_get_string(params, "network_bridge"), alias) != 0) {
            *message = g_strdup("bridge and network_bridge must match");
            return FALSE;
        }
        return TRUE;
    }
    json_object_set_string_member(params, "network_bridge", alias);                                        
    return TRUE;
}

                                                                                      
                                                                                       
gboolean
pcv_container_create_mode_check(JsonObject *params, GSocketConnection *conn,
                                const PcvLxcUsernsSettings *settings,
                                const gchar *subuid_path, const gchar *subgid_path,
                                PcvContainerCreateMode *out)
{
    *out = (PcvContainerCreateMode){ .rootless = -1 };
    PcvLxcUsernsSettings loaded;
    if (!settings) {
        loaded = pcv_lxc_userns_settings_load();
        settings = &loaded;
    }
                                                                                  
    gint requested = -1;
    if (params && json_object_has_member(params, "rootless")) {
        gboolean v = TRUE;
        if (!pcv_rpc_params_get_bool(params, "rootless", TRUE, &v))
            return _mode_reject(out, PURE_RPC_ERR_INVALID_PARAMS, g_strdup("rootless must be a boolean"));
        requested = v ? 1 : 0;
    }
                                                                               
    if (requested == 0 && pcv_disp_dispatcher_caller_role(params, conn) < PCV_ROLE_ADMIN)
        return _mode_reject(out, PURE_RPC_ERR_FORBIDDEN, g_strdup("privileged containers require admin role"));
                                                                                          
    if (!_bridge_allowed(params, conn, out))
        return FALSE;
                                           
    gboolean wants_userns = requested == 1 || (requested < 0 && settings->default_on);
    PcvLxcUsernsHost host = wants_userns
        ? pcv_lxc_userns_host_check(subuid_path, subgid_path, settings->start, settings->count)
        : PCV_LXC_USERNS_READY;
    PcvLxcRootlessDecision d = pcv_lxc_rootless_decide(requested, settings->default_on,
                                                       host == PCV_LXC_USERNS_READY);
    if (d == PCV_LXC_ROOTLESS_REJECT)
        return _mode_reject(out, PURE_RPC_ERR_CONFLICT,
                            pcv_lxc_userns_host_message(host, settings->start, settings->count));
    out->rootless = d == PCV_LXC_ROOTLESS_ON ? 1 : 0;                     
    return TRUE;
}

                                                                         
                                                         
JsonObject *
pcv_container_info_json(const PcvLxcInfo *info)
{
    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "name",       info->name);
    json_object_set_string_member(obj, "state",      info->state_str);
    json_object_set_string_member(obj, "ip_addr",    info->ip_addr);
    json_object_set_string_member(obj, "image",      info->image);
    json_object_set_boolean_member(obj, "privileged", info->privileged);                          
    return obj;
}

                                                                      
                                                                         
                                                          
                                                                                                  
                                                                               
                                                    
gboolean
pcv_container_config_token_safe(const gchar *value, gboolean allow_space)
{
    if (!value || !*value)
        return FALSE;
    for (const guchar *p = (const guchar *)value; *p; p++) {
        if (*p < 0x20 || *p == 0x7f)
            return FALSE;
        if (!allow_space && (*p == ' ' || *p == '\\'))                                          
            return FALSE;
    }
    return TRUE;
}

                                                                                    
                                                      
gboolean
pcv_container_volume_hook_ready(const gchar *rpc_id, UdsServer *server, GSocketConnection *conn)
{
    if (pcv_lxc_volume_guard_hook_installed(PCV_LXC_VOLUME_GUARD_HOOK))
        return TRUE;
    g_autofree gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_CONFLICT,
                                                           "volume guard hook not installed: " PCV_LXC_VOLUME_GUARD_HOOK);
    pure_uds_server_send_response(server, conn, resp);
    return FALSE;
}

                                                                                            
                                                                                 
gboolean
pcv_container_volume_start_check(const gchar *name, gchar **msg_out)
{
    g_autofree gchar *bad = NULL;
    if (pcv_lxc_volume_guard_check(PCV_LXC_PATH, name, &bad)) {
        return TRUE;                                                                            
    }
    if (msg_out)
        *msg_out = pcv_lxc_volume_guard_message(bad);
    return FALSE;
}

                                                                           
                                                                                      
                                                       
static gboolean
_restart_check(const gchar *name, gboolean before_start)
{
    g_autofree gchar *bad = NULL;
    if (before_start ? pcv_lxc_volume_guard_check_start(PCV_LXC_PATH, name, &bad) : pcv_lxc_volume_guard_check(PCV_LXC_PATH, name, &bad))
        return TRUE;
    g_autofree gchar *msg = pcv_lxc_volume_guard_message(bad);
    PCV_LOG_WARN("container", "health auto-restart of '%s' refused (container left as it is, running or stopped): %s; "
                 "detach that volume and attach the real path", name, msg);
    pcv_audit_log(PCV_AUDIT_ACTOR_SYSTEM, "container.start", name, "denied", PURE_RPC_ERR_CONFLICT, 0, "local");
    return FALSE;
}

                                                                                   
                                
gboolean
pcv_container_volume_restart_allowed(const gchar *name)
{
    return _restart_check(name, FALSE);
}

                                                                                 
                                                      
gboolean
pcv_container_volume_restart_after_stop(const gchar *name, gint stop_rc)
{
    g_autofree gchar *state = pcv_lxc_get_state(name);
    if ((stop_rc == 0 || stop_rc == 2) && g_strcmp0(state, "STOPPED") == 0)                                          
        return _restart_check(name, TRUE);
    PCV_LOG_WARN("container", "health auto-restart of '%s' refused: lxc-stop exit %d, state %s — not confirmed stopped, so "
                 "no fingerprint and no restart; start it through purecvisor", name, stop_rc, state ? state : "unknown");
    pcv_audit_log(PCV_AUDIT_ACTOR_SYSTEM, "container.start", name, "denied", PURE_RPC_ERR_CONFLICT, 0, "local");
    return FALSE;
}

                                                                                         
                                                                                            
                                     
                                                                               
gboolean
pcv_container_clone_volume_gate(gint role, const gchar *config_text, gint *volume_entries_out, gchar **msg_out)
{
    gint count = -1;                                                     
    if (config_text) {
        GPtrArray *entries = pcv_lxc_config_mount_entries(config_text);
        count = 0;
        for (guint i = 0; i < entries->len; i++)
            if (pcv_lxc_mount_entry_is_volume(entries->pdata[i]))                                         
                count++;
        g_ptr_array_unref(entries);
    }
    if (volume_entries_out)
        *volume_entries_out = count;
    if (role >= PCV_ROLE_ADMIN || count == 0)
        return TRUE;                                                                           
    if (msg_out)                                                                                     
        *msg_out = g_strdup("cloning a container with host volumes requires admin role");
    return FALSE;
}

                                                                                                                
                                                                            
static gchar *
_clone_link_value(const gchar *line)
{
    const gchar *p = line + strspn(line, " \t");
    const gchar *eq = strchr(p, '=');
    if (*p == '#' || !eq)
        return NULL;
    g_autofree gchar *key = g_strndup(p, (gsize)(eq - p));
    g_strchomp(key);
    gboolean link = g_str_equal(key, "lxc.network.link");
    static const gchar pre[] = "lxc.net.", suf[] = ".link";
    const gsize kl = strlen(key), pl = sizeof pre - 1, sl = sizeof suf - 1;
    if (!link && kl > pl + sl && g_str_has_prefix(key, pre) && g_str_has_suffix(key, suf)) {
        link = TRUE;
        for (gsize i = pl; link && i < kl - sl; i++)
            link = g_ascii_isdigit(key[i]);                                                                   
    }
    return link ? g_strstrip(g_strdup(eq + 1)) : NULL;
}

                                                                                                     
                                                                                                   
                                                             
                                                                                  
gboolean
pcv_container_clone_bridge_gate(gint role, const gchar *config_text, PcvContainerCloneBridgeCheck check, gpointer data,
                                gchar **bridge_out, gchar **msg_out)
{
    if (bridge_out)
        *bridge_out = NULL;
    if (role >= PCV_ROLE_ADMIN)
        return TRUE;                                                                 
    if (!config_text) {
        if (msg_out)
            *msg_out = g_strdup("cloning a container whose config cannot be read requires admin role");
        return FALSE;
    }
    g_auto(GStrv) lines = g_strsplit(config_text, "\n", -1);
    for (guint i = 0; lines[i]; i++) {
        g_autofree gchar *br = _clone_link_value(lines[i]);
        if (!br || !*br || g_str_equal(br, PCV_LXC_DEFAULT_BRIDGE))
            continue;                                                                                     
        if (check && check(br, data))
            continue;
        if (msg_out) {
            g_autofree gchar *shown = g_utf8_make_valid(br, (gssize)MIN(strlen(br), (gsize)64));
            *msg_out = g_strdup_printf("cloning a container connected to bridge '%s' requires admin role: "
                                       "the bridge is not allowed for this account", shown);
        }
        if (bridge_out)
            *bridge_out = g_steal_pointer(&br);
        return FALSE;
    }
    return TRUE;
}
