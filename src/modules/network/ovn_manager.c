   
                      
                                     
  
                           
                                                   
                                                    
                                        
  
          
                                            
                                             
                                                    
                                                
                                                   
                                               
                                                 
  
         
                                                   
                                              
                                                
                                      
        
                                                          
                                                                
                                                               
                             
         
                                                  
                                         
        
                                                        
                                                          
                                            
        
                                                     
                                                                  
                                                                       
            
                                             
                                     
                                                          
  
                                                    
                                                        
                                          
  
                                           
                                                      
                                                
                                   
                                                   
                                      
                                                      
                                      
                                                      
                                   
                                   
                                                             
                                                 
                                                             
                                   
                                                       
                                        
  
                
                                                       
                                            
                                                         
  
                         
                                                            
                                                             
                                   
                                         
                                           
                                   
  
             
                               
                                                                     
                                                                
  
             
                                                
                                            
                                         
                     
  
                 
                                   
                                                         
                 
                                             
  
          
                                                                   
                                                  
                                                              
  
          
                                              
                                      
                                           
  
         
                                                
                                 
                                                                  
                                                
                                            
                                                       
                                                                       
   
#include "ovn_manager.h"
#include "utils/pcv_spawn.h"
#include "utils/pcv_log.h"
#include "utils/pcv_validate.h"
#include "../../utils/pcv_config.h"
#include <string.h>

#define OVN_LOG_DOM "ovn_mgr"

static gboolean g_ovn_available = FALSE;

                                                                     

   
                                                       
              
  
                                             
                                                  
  
                                               
                                
                                                
                                           
  
                     
   
static gboolean
_valid_ovn_id(const gchar *s)
{
    if (!s || !*s) return FALSE;                                           
    if (s[0] == '-') return FALSE;                           
                                                        
    for (const gchar *p = s; *p; p++) {
        if (!(g_ascii_isalnum((guchar)*p) ||
              *p == '_' || *p == '.' || *p == ':' || *p == '-'))
            return FALSE;
    }
    return TRUE;
}

                                                               
                                                    
gboolean pcv_ovn_valid_id(const gchar *s) { return _valid_ovn_id(s); }

   
                                                  
                                                   
                                                   
                                  
                                
  
                                                                
                                                
                                    
  
                                                   
                                         
  
                                              
                                                           
  
                     
   
static gboolean
_run_argv(const gchar * const *argv, gchar **out, GError **error)
{
    gchar *std_err = NULL;
    gboolean ok = pcv_spawn_sync(argv, out, &std_err, error);
                                                    
    if (!ok && std_err && *std_err)
        PCV_LOG_WARN(OVN_LOG_DOM, "OVN command failed: %s → %s", argv[0], std_err);
    g_free(std_err);                                      
    return ok;
}

                                                         
                                                          
static gboolean
_find_switch_dhcp_uuid(const gchar *sw, gchar **uuid_out, GError **error)
{
    *uuid_out = NULL;
    if (!sw)
        return TRUE;

    gchar *match = g_strdup_printf("external_ids:logical_switch=%s", sw);
    const gchar *argv[] = {
        "ovn-nbctl", "--data=bare", "--no-heading", "--columns=_uuid",
        "find", "DHCP_Options", match, NULL
    };
    gchar *out = NULL;
    gboolean ok = _run_argv(argv, &out, error);
    g_free(match);
    if (!ok) {
        g_free(out);
        return FALSE;
    }
    if (!out || !*g_strstrip(out)) {
        g_free(out);
        return TRUE;
    }

    gchar **lines = g_strsplit(out, "\n", -1);
    guint count = 0;
    for (guint i = 0; lines[i]; i++)
        if (*g_strstrip(lines[i])) count++;
    if (count != 1) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "switch '%s' has %u DHCP option records; exactly one is required",
                    sw, count);
        g_strfreev(lines);
        g_free(out);
        return FALSE;
    }
    for (guint i = 0; lines[i]; i++) {
        if (*g_strstrip(lines[i])) {
            *uuid_out = g_strdup(lines[i]);
            break;
        }
    }
    g_strfreev(lines);
    g_free(out);
    return TRUE;
}

                                                                     

   
                             
  
                                              
                                               
  
                                                     
                                                                   
                                                                 
                       
                                                      
   
void pcv_ovn_init(void)
{
    const gchar *version_argv[] = {"ovn-nbctl", "--version", NULL};
    const gchar *northbound_argv[] = {
        "ovn-nbctl", "--timeout=5", "list", "NB_Global", NULL
    };
    gchar *out = NULL, *errout = NULL;
    gboolean installed = pcv_spawn_sync(version_argv, &out, &errout, NULL);
    g_free(out);                                       
    g_free(errout);
    out = NULL;
    errout = NULL;
    gboolean northbound_ready = installed &&
        pcv_spawn_sync(northbound_argv, &out, &errout, NULL);
    g_free(out);
    g_free(errout);
    g_ovn_available = installed && northbound_ready;
    if (g_ovn_available)
        PCV_LOG_INFO(OVN_LOG_DOM, "OVN Northbound control plane available");
    else if (installed)
        PCV_LOG_INFO(OVN_LOG_DOM, "OVN installed but Northbound DB unavailable — OVN features disabled");
    else
        PCV_LOG_INFO(OVN_LOG_DOM, "OVN not installed — OVN features disabled");
}

                                                                  
                                                     
void pcv_ovn_shutdown(void)
{
    if (g_ovn_available)                                         
        PCV_LOG_INFO(OVN_LOG_DOM, "OVN manager shutdown");
    g_ovn_available = FALSE;
}

                                                                    
                                                       
gboolean pcv_ovn_is_available(void) { return g_ovn_available; }

                                                                

   
                                             
                               
                                            
                    
  
                                             
                                             
  
                                                        
                                                                 
                                             
  
                    
   
gboolean
pcv_ovn_switch_create(const gchar *name, const gchar *subnet, GError **error)
{
    if (!g_ovn_available) {                                     
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "OVN not available");
        return FALSE;
    }
    if (!_valid_ovn_id(name)) {                                  
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "invalid switch name");
        return FALSE;
    }
                                                           
    const gchar *argv[] = {"ovn-nbctl", "--may-exist", "ls-add", name, NULL};
    gboolean ok = _run_argv(argv, NULL, error);
    if (ok)
        PCV_LOG_INFO(OVN_LOG_DOM, "Logical switch '%s' created (subnet=%s)", name, subnet ? subnet : "-");
    return ok;
}

   
                                             
                    
                    
  
                                                
  
                                         
                                            
                                                         
                                                       
  
                    
   
gboolean
pcv_ovn_switch_delete(const gchar *name, GError **error)
{
    if (!g_ovn_available) return TRUE;                            
    if (!_valid_ovn_id(name)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "invalid switch name");
        return FALSE;
    }
                                                  
    const gchar *argv[] = {"ovn-nbctl", "--if-exists", "ls-del", name, NULL};
    return _run_argv(argv, NULL, error);
}

   
                                             
  
                                         
                                            
  
                                                
                          
                     
  
                                                                   
                                  
   
JsonArray *
pcv_ovn_switch_list(void)
{
    JsonArray *arr = json_array_new();
    if (!g_ovn_available) return arr;                                 

    gchar *out = NULL;
    const gchar *argv[] = {"ovn-nbctl", "ls-list", NULL};
    if (_run_argv(argv, &out, NULL) && out) {
                                                 
        gchar **lines = g_strsplit(g_strstrip(out), "\n", -1);                
        for (gchar **l = lines; *l; l++) {
            if (!**l) continue;                           
            gchar *lp = strchr(*l, '(');                   
            gchar *rp = lp ? strchr(lp, ')') : NULL;                     
                                                           
            if (lp && rp) {
                gchar *name = g_strndup(lp + 1, rp - lp - 1);                         
                JsonObject *obj = json_object_new();
                json_object_set_string_member(obj, "name", name);                     
                json_array_add_object_element(arr, obj);
                g_free(name);                     
            }
        }
        g_strfreev(lines);
    }
    g_free(out);
    return arr;
}

   
                    
                 
               
                                                    
                                            
                    
  
                                                  
                                       
  
                      
                                                
                                                    
                          
                                                  
                                                  
   
gboolean
pcv_ovn_port_add(const gchar *sw, const gchar *port, const gchar *mac, const gchar *ip, GError **error)
{
    if (!g_ovn_available) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "OVN not available");
        return FALSE;
    }
    if (!_valid_ovn_id(sw) || !_valid_ovn_id(port) || ((mac == NULL) != (ip == NULL))) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "invalid switch or port name");
        return FALSE;
    }
    if (mac && (!pcv_validate_mac(mac) || !pcv_validate_ip_literal(ip))) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "invalid mac or ip");
        return FALSE;
    }

    gchar *dhcp_uuid = NULL;
    if (!_find_switch_dhcp_uuid(sw, &dhcp_uuid, error))
        return FALSE;
    gchar *addr = mac ? g_strdup_printf("%s %s", mac, ip) : NULL;
    const gchar *with_address_and_dhcp[] = {
        "ovn-nbctl", "--may-exist", "lsp-add", sw, port,
        "--", "lsp-set-addresses", port, addr,
        "--", "lsp-set-port-security", port, addr,
        "--", "lsp-set-dhcpv4-options", port, dhcp_uuid, NULL
    };
    const gchar *with_address[] = {
        "ovn-nbctl", "--may-exist", "lsp-add", sw, port,
        "--", "lsp-set-addresses", port, addr,
        "--", "lsp-set-port-security", port, addr, NULL
    };
    const gchar *with_dhcp[] = {
        "ovn-nbctl", "--may-exist", "lsp-add", sw, port,
        "--", "lsp-set-dhcpv4-options", port, dhcp_uuid, NULL
    };
    const gchar *port_only[] = {
        "ovn-nbctl", "--may-exist", "lsp-add", sw, port, NULL
    };
    const gchar * const *argv = addr && dhcp_uuid ? with_address_and_dhcp
                               : addr ? with_address
                               : dhcp_uuid ? with_dhcp : port_only;
    gboolean ok = _run_argv(argv, NULL, error);
    g_free(addr);
    g_free(dhcp_uuid);
    return ok;
}

   
                                              
                                          
                   
                    
  
                                                
  
                                                  
                                                     
  
                    
   
gboolean
pcv_ovn_port_remove(const gchar *sw, const gchar *port, GError **error)
{
    (void)sw;                                             
    if (!g_ovn_available) return TRUE;
    if (!_valid_ovn_id(port)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "invalid port name");
        return FALSE;
    }
    const gchar *argv[] = {"ovn-nbctl", "--if-exists", "lsp-del", port, NULL};
    return _run_argv(argv, NULL, error);
}

                                                                    

   
                                            
                    
                                              
                                        
                                                           
                                                           
                    
  
                                              
                                              
  
                                             
                                                                                
                                                        
  
                    
   
gboolean
pcv_ovn_acl_add(const gchar *sw, const gchar *direction, gint priority,
                 const gchar *match, const gchar *action, GError **error)
{
    if (!g_ovn_available) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "OVN not available");
        return FALSE;
    }
                                                                 
                                                        
                                                                 
                                                 
    if (!_valid_ovn_id(sw) || !match ||
        !(g_strcmp0(direction, "to-lport") == 0 || g_strcmp0(direction, "from-lport") == 0)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "invalid switch/direction/match");
        return FALSE;
    }
                                             
    if (!(g_strcmp0(action, "allow") == 0 || g_strcmp0(action, "allow-related") == 0 ||
          g_strcmp0(action, "drop") == 0 || g_strcmp0(action, "reject") == 0)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "invalid action");
        return FALSE;
    }
    gchar *pri = g_strdup_printf("%d", priority);                           
    const gchar *argv[] = {"ovn-nbctl", "acl-add", sw, direction, pri, match, action, NULL};
    gboolean ok = _run_argv(argv, NULL, error);
    g_free(pri);
    return ok;
}

   
                                          
                    
                 
                  
                
                    
  
                                                
  
                                            
                                                           
  
                    
   
gboolean
pcv_ovn_acl_delete(const gchar *sw, const gchar *direction, gint priority,
                    const gchar *match, GError **error)
{
    if (!g_ovn_available) return TRUE;                            
                                                          
    if (!_valid_ovn_id(sw) || !match ||
        !(g_strcmp0(direction, "to-lport") == 0 || g_strcmp0(direction, "from-lport") == 0)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "invalid switch/direction/match");
        return FALSE;
    }
    gchar *pri = g_strdup_printf("%d", priority);
    const gchar *argv[] = {"ovn-nbctl", "acl-del", sw, direction, pri, match, NULL};
    gboolean ok = _run_argv(argv, NULL, error);
    g_free(pri);
    return ok;
}

   
                                              
                    
  
                                                   
  
                                                
                                
  
                                                                           
                                  
   
JsonArray *
pcv_ovn_acl_list(const gchar *sw)
{
    JsonArray *arr = json_array_new();
    if (!g_ovn_available || !sw) return arr;                       
    if (!_valid_ovn_id(sw)) return arr;                                   
    const gchar *argv[] = {"ovn-nbctl", "acl-list", sw, NULL};
    gchar *out = NULL;
    if (_run_argv(argv, &out, NULL) && out) {
        gchar **lines = g_strsplit(g_strstrip(out), "\n", -1);
        for (gchar **l = lines; *l; l++) {
            if (!**l) continue;                                  
            json_array_add_string_element(arr, *l);                             
        }
        g_strfreev(lines);
    }
    g_free(out);
    return arr;
}

   
                                          
                                         
                                  
                    
  
         
                                                           
                                                             
                                                                        
  
                                                   
                                                     
             
  
                                                            
                                            
                                                      
  
                    
   
gboolean
pcv_ovn_dhcp_enable_for_switch(const gchar *sw,
                               const gchar *subnet,
                               const gchar *gw,
                               GError **error)
{
    if (!g_ovn_available) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "OVN not available");
        return FALSE;
    }
    if ((sw && !_valid_ovn_id(sw)) || !pcv_validate_cidr(subnet) ||
        !pcv_validate_ip_literal(gw)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "invalid switch, subnet or gateway");
        return FALSE;
    }

    gchar *existing_uuid = NULL;
    if (!_find_switch_dhcp_uuid(sw, &existing_uuid, error))
        return FALSE;
    gchar *cidr_col = g_strdup_printf("cidr=%s", subnet);
    gchar *external_id = sw
        ? g_strdup_printf("external_ids:logical_switch=%s", sw)
        : g_strdup("external_ids:managed_by=purecvisor");
    gchar *router_opt = g_strdup_printf("options:router=%s", gw);
    gchar *serverid_opt = g_strdup_printf("options:server_id=%s", gw);
    const gchar *create_argv[] = {
        "ovn-nbctl", "create", "DHCP_Options", cidr_col, external_id,
        "options:lease_time=3600", router_opt, serverid_opt,
        "options:server_mac=02:00:00:00:00:01", NULL
    };
    const gchar *update_argv[] = {
        "ovn-nbctl", "set", "DHCP_Options", existing_uuid, cidr_col,
        "options:lease_time=3600", router_opt, serverid_opt,
        "options:server_mac=02:00:00:00:00:01", NULL
    };
    gchar *out = NULL;
    gboolean ok = _run_argv(existing_uuid ? update_argv : create_argv,
                            existing_uuid ? NULL : &out, error);
    gchar *uuid = existing_uuid ? g_strdup(existing_uuid)
                                 : g_strdup(out ? g_strstrip(out) : NULL);
    if (ok && (!uuid || !*uuid)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "OVN did not return a DHCP option UUID");
        ok = FALSE;
    }

                                                                     
    gchar *ports_out = NULL;
    if (ok && sw) {
        const gchar *list_argv[] = {"ovn-nbctl", "lsp-list", sw, NULL};
        ok = _run_argv(list_argv, &ports_out, error);
    }
    if (ok && ports_out && *ports_out) {
        GPtrArray *args = g_ptr_array_new_with_free_func(g_free);
        g_ptr_array_add(args, g_strdup("ovn-nbctl"));
        gchar **lines = g_strsplit(ports_out, "\n", -1);
        guint attached = 0;
        for (guint i = 0; lines[i]; i++) {
            gchar *open = strchr(lines[i], '(');
            gchar *close = open ? strchr(open + 1, ')') : NULL;
            if (!open || !close) continue;
            gchar *port = g_strndup(open + 1, (gsize)(close - open - 1));
            if (g_str_has_prefix(port, "lnk-")) {
                g_free(port);
                continue;
            }
            if (attached++) g_ptr_array_add(args, g_strdup("--"));
            g_ptr_array_add(args, g_strdup("lsp-set-dhcpv4-options"));
            g_ptr_array_add(args, port);
            g_ptr_array_add(args, g_strdup(uuid));
        }
        g_strfreev(lines);
        if (attached) {
            g_ptr_array_add(args, NULL);
            ok = _run_argv((const gchar * const *)args->pdata, NULL, error);
        }
        g_ptr_array_free(args, TRUE);
    }
    if (!ok && !existing_uuid && uuid && *uuid) {
        const gchar *rollback[] = {
            "ovn-nbctl", "destroy", "DHCP_Options", uuid, NULL
        };
        _run_argv(rollback, NULL, NULL);
    }

    g_free(ports_out);
    g_free(uuid);
    g_free(existing_uuid);
    g_free(out);
    g_free(cidr_col);
    g_free(external_id);
    g_free(router_opt);
    g_free(serverid_opt);
    return ok;
}

                                                                

   
                                             
                               
                    
  
                                                
                                         
  
                                                        
                                      
  
                    
   
gboolean
pcv_ovn_router_create(const gchar *name, GError **error)
{
    if (!g_ovn_available) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "OVN not available");
        return FALSE;
    }
    if (!_valid_ovn_id(name)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "invalid router name");
        return FALSE;
    }
                                         
    const gchar *argv[] = {"ovn-nbctl", "--may-exist", "lr-add", name, NULL};
    return _run_argv(argv, NULL, error);
}

   
                                             
                    
                    
  
                                       
  
                                                 
  
                    
   
gboolean
pcv_ovn_router_delete(const gchar *name, GError **error)
{
    if (!g_ovn_available) return TRUE;                            
    if (!_valid_ovn_id(name)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "invalid router name");
        return FALSE;
    }
    const gchar *argv[] = {"ovn-nbctl", "--if-exists", "lr-del", name, NULL};
    return _run_argv(argv, NULL, error);
}

   
                           
                     
                     
                      
                                               
                    
  
                              
  
                                     
                                                    
                                       
                                                    
                                                  
                                                   
  
                                             
                                               
                   
  
                                             
                                            
   
gboolean
pcv_ovn_router_add_port(const gchar *router, const gchar *sw,
                         const gchar *mac, const gchar *cidr, GError **error)
{
    if (!g_ovn_available) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "OVN not available");
        return FALSE;
    }
                                                      
    if (!_valid_ovn_id(router) || !_valid_ovn_id(sw) ||
        !pcv_validate_mac(mac) || !pcv_validate_cidr(cidr)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "invalid router/switch/mac/cidr");
        return FALSE;
    }
                                                                    
    gchar *rport = g_strdup_printf("rtr-%s", sw);
    gchar *lport = g_strdup_printf("lnk-%s", sw);
    gchar *ropt = g_strdup_printf("router-port=%s", rport);
    const gchar *argv[] = {
        "ovn-nbctl", "--may-exist", "lrp-add", router, rport, mac, cidr,
        "--", "--may-exist", "lsp-add", sw, lport,
        "--", "lsp-set-type", lport, "router",
        "--", "lsp-set-addresses", lport, "router",
        "--", "lsp-set-options", lport, ropt, NULL
    };
    gboolean ok = _run_argv(argv, NULL, error);
    g_free(ropt);
    g_free(lport);
    g_free(rport);
    return ok;
}

                                                                     

   
                              
                                   
                       
                    
  
                                             
  
                                       
                          
                                                     
   
gboolean
pcv_ovn_router_remove_port(const gchar *router, const gchar *port, GError **error)
{
    (void)router;                                         
    if (!g_ovn_available) return TRUE;
    if (!_valid_ovn_id(port)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "invalid port name");
        return FALSE;
    }
    const gchar *argv[] = {"ovn-nbctl", "--if-exists", "lrp-del", port, NULL};
    return _run_argv(argv, NULL, error);
}

   
                       
  
                                       
  
                                           
                                                
                                                         
  
                                                                
                                  
   
JsonArray *
pcv_ovn_router_list(void)
{
    JsonArray *arr = json_array_new();
    if (!g_ovn_available) return arr;                  

    gchar *out = NULL;
    const gchar *argv[] = {"ovn-nbctl", "lr-list", NULL};
    if (_run_argv(argv, &out, NULL) && out) {
        gchar **lines = g_strsplit(g_strstrip(out), "\n", -1);
        for (gchar **l = lines; *l; l++) {
            if (!**l) continue;                                      
            gchar *lp = strchr(*l, '(');                          
            gchar *rp = lp ? strchr(lp, ')') : NULL;              
            if (lp && rp) {                          
                gchar *name = g_strndup(lp + 1, rp - lp - 1);
                JsonObject *obj = json_object_new();
                json_object_set_string_member(obj, "name", name);
                json_array_add_object_element(arr, obj);
                g_free(name);
            }
        }
        g_strfreev(lines);
    }
    g_free(out);
    return arr;
}

                                                                     

   
                   
                  
                                                  
                                  
                                        
                    
  
                                                    
                                                 
                                     
  
                                            
                                
                                 
                                                                  
                                                                  
   
gboolean
pcv_ovn_nat_add(const gchar *router, const gchar *type,
                 const gchar *external_ip, const gchar *logical_ip, GError **error)
{
    if (!g_ovn_available) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "OVN not available");
        return FALSE;
    }
                                                   
                                                              
    if (!_valid_ovn_id(router) ||
        !(g_strcmp0(type, "snat") == 0 || g_strcmp0(type, "dnat") == 0 ||
          g_strcmp0(type, "dnat_and_snat") == 0) ||
        !pcv_validate_ip_literal(external_ip) ||
        !(pcv_validate_ip_literal(logical_ip) || pcv_validate_cidr(logical_ip))) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "invalid router/type/ip");
        return FALSE;
    }
    const gchar *argv[] = {"ovn-nbctl", "lr-nat-add", router, type, external_ip, logical_ip, NULL};
    gboolean ok = _run_argv(argv, NULL, error);
    if (ok)
        PCV_LOG_INFO(OVN_LOG_DOM, "NAT %s added: router=%s ext=%s log=%s",
                     type, router, external_ip, logical_ip);
    return ok;
}

   
                      
                  
                
                      
                                                       
                    
  
                                             
  
                          
                                                           
   
gboolean
pcv_ovn_nat_delete(const gchar *router, const gchar *type,
                    const gchar *external_ip, const gchar *logical_ip, GError **error)
{
    (void)logical_ip;                                                   
    if (!g_ovn_available) return TRUE;                            
    if (!_valid_ovn_id(router) ||
        !(g_strcmp0(type, "snat") == 0 || g_strcmp0(type, "dnat") == 0 ||
          g_strcmp0(type, "dnat_and_snat") == 0) ||
        !pcv_validate_ip_literal(external_ip)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "invalid router/type/ip");
        return FALSE;
    }
    const gchar *argv[] = {"ovn-nbctl", "lr-nat-del", router, type, external_ip, NULL};
    return _run_argv(argv, NULL, error);
}

   
                                          
                  
  
                                                  
  
                                                 
                                    
  
                                                                      
   
JsonArray *
pcv_ovn_nat_list(const gchar *router)
{
    JsonArray *arr = json_array_new();
    if (!g_ovn_available || !router) return arr;                       
    if (!_valid_ovn_id(router)) return arr;                           

    const gchar *argv[] = {"ovn-nbctl", "lr-nat-list", router, NULL};
    gchar *out = NULL;
    if (_run_argv(argv, &out, NULL) && out) {
        gchar **lines = g_strsplit(g_strstrip(out), "\n", -1);
        for (gchar **l = lines; *l; l++) {
            if (!**l) continue;                                  
            json_array_add_string_element(arr, *l);                     
        }
        g_strfreev(lines);
    }
    g_free(out);
    return arr;
}

                                                                     

   
                                        
  
                                                 
  
                                            
                     
  
                                                                      
   
JsonArray *
pcv_ovn_dhcp_list(void)
{
    JsonArray *arr = json_array_new();
    if (!g_ovn_available) return arr;                  

    gchar *out = NULL;
    const gchar *argv[] = {"ovn-nbctl", "dhcp-options-list", NULL};
    if (_run_argv(argv, &out, NULL) && out) {
        gchar **lines = g_strsplit(g_strstrip(out), "\n", -1);
        for (gchar **l = lines; *l; l++) {
            if (!**l) continue;                                  
            json_array_add_string_element(arr, *l);                  
        }
        g_strfreev(lines);
    }
    g_free(out);
    return arr;
}

                                                                     

   
                         
                                    
                                                
                    
  
                     
  
         
                                    
                                                                   
                                              
  
                                                 
                                                    
                            
  
                                                
                                                
                                       
   
gboolean
pcv_ovn_tenant_create(const gchar *tenant, const gchar *subnet, GError **error)
{
    if (!g_ovn_available) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "OVN not available");
        return FALSE;
    }
    if (!tenant || !subnet) {                  
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "tenant and subnet are required");
        return FALSE;
    }
    if (!_valid_ovn_id(tenant) || !pcv_validate_cidr(subnet)) {                     
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "invalid tenant or subnet");
        return FALSE;
    }

                                                           
    gchar *sw_name = g_strdup_printf("tenant-%s-ls", tenant);
    gboolean ok = pcv_ovn_switch_create(sw_name, subnet, error);
    if (!ok) {
        g_free(sw_name);
        return FALSE;
    }

                                                                  
                                                                
    ok = pcv_ovn_acl_add(sw_name, "to-lport", 1000, "ip", "allow", error) &&
         pcv_ovn_acl_add(sw_name, "from-lport", 1000, "ip", "allow", error);

                    
                                         
                                                               
                                          
    gchar **parts = g_strsplit(subnet, "/", 2);                           
    if (parts[0]) {
        gchar **octets = g_strsplit(parts[0], ".", 4);                        
        if (octets[0] && octets[1] && octets[2]) {                            
            gchar *gw = g_strdup_printf("%s.%s.%s.1", octets[0], octets[1], octets[2]);
            if (ok)
                ok = pcv_ovn_dhcp_enable_for_switch(sw_name, subnet, gw, error);
            g_free(gw);
        }
        g_strfreev(octets);
    }
    g_strfreev(parts);

    if (!ok) {
        pcv_ovn_switch_delete(sw_name, NULL);
        g_free(sw_name);
        return FALSE;
    }

    PCV_LOG_INFO(OVN_LOG_DOM, "Tenant '%s' created: sw=%s subnet=%s", tenant, sw_name, subnet);
    g_free(sw_name);
    return TRUE;
}

   
                         
                      
                    
  
                                              
                                           
  
                                           
                                      
                                 
   
gboolean
pcv_ovn_tenant_delete(const gchar *tenant, GError **error)
{
    if (!g_ovn_available) return TRUE;                            
    if (!tenant) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "tenant is required");
        return FALSE;
    }

                                               
    gchar *sw_name = g_strdup_printf("tenant-%s-ls", tenant);
    gboolean ok = pcv_ovn_switch_delete(sw_name, error);
    g_free(sw_name);

    if (ok)
        PCV_LOG_INFO(OVN_LOG_DOM, "Tenant '%s' deleted", tenant);
    return ok;
}

                                                                     

   
                         
                     
                                           
                           
                         
                                                            
                    
  
                            
  
         
                                                                         
                                               
  
                                                    
                                                       
  
                                                  
                                                      
   
gboolean
pcv_ovn_vm_port_setup(const gchar *sw, const gchar *vm_name,
                       const gchar *mac, const gchar *ip,
                       gchar **iface_id_out, GError **error)
{
    if (!g_ovn_available) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "OVN not available");
        return FALSE;
    }
    if (!sw || !vm_name) {                     
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "sw and vm_name are required");
        return FALSE;
    }

    gchar *port = g_strdup_printf("vm-%s", vm_name);                              

                                                      
    gboolean ok = pcv_ovn_port_add(sw, port, mac, ip, error);
    if (!ok) {
        g_free(port);
        return FALSE;
    }

                                                        
    if (iface_id_out)
        *iface_id_out = g_strdup(port);                                      

    PCV_LOG_INFO(OVN_LOG_DOM, "VM port setup: sw=%s port=%s mac=%s ip=%s",
                 sw, port, mac ? mac : "-", ip ? ip : "-");
    g_free(port);
    return TRUE;
}

   
                           
                      
                    
  
                                                     
  
                                        
                           
                                    
   
gboolean
pcv_ovn_vm_port_cleanup(const gchar *vm_name, GError **error)
{
    if (!g_ovn_available) return TRUE;                               
    if (!vm_name) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "vm_name is required");
        return FALSE;
    }

                                                              
    gchar *port = g_strdup_printf("vm-%s", vm_name);
    gboolean ok = pcv_ovn_port_remove(NULL, port, error);
    g_free(port);

    if (ok)
        PCV_LOG_INFO(OVN_LOG_DOM, "VM port cleanup: vm=%s", vm_name);
    return ok;
}

                                                                

   
                         
                   
  
                          
  
                    
      
                         
                       
                                                   
                      
                                               
      
  
                                                
                                                    
                                                
  
                                                 
                                  
  
                                                          
                                             
   
JsonObject *
pcv_ovn_switch_detail(const gchar *name)
{
    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "name", name ? name : "");                      
    if (!g_ovn_available || !name) return obj;                            
    if (!_valid_ovn_id(name)) return obj;                           

                                            
    {
        const gchar *argv[] = {"ovn-nbctl", "lsp-list", name, NULL};
        gchar *out = NULL;
        JsonArray *ports = json_array_new();
        if (_run_argv(argv, &out, NULL) && out) {
            gchar **lines = g_strsplit(g_strstrip(out), "\n", -1);
            for (gchar **l = lines; *l; l++) {
                if (!**l) continue;                            
                                          
                gchar *lp = strchr(*l, '(');
                gchar *rp = lp ? strchr(lp, ')') : NULL;
                if (lp && rp) {                             
                    gchar *pname = g_strndup(lp + 1, rp - lp - 1);
                    json_array_add_string_element(ports, pname);
                    g_free(pname);
                }
            }
            g_strfreev(lines);
        }
        g_free(out);
                                            
        json_object_set_int_member(obj, "port_count", (gint64)json_array_get_length(ports));
        json_object_set_array_member(obj, "ports", ports);                      
    }

                                      
    {
        JsonArray *acls = pcv_ovn_acl_list(name);
        json_object_set_int_member(obj, "acl_count", (gint64)json_array_get_length(acls));
        json_object_set_array_member(obj, "acls", acls);               
    }

    return obj;
}

   
                         
                   
  
                          
  
                    
      
                         
                       
                                                                        
                      
                                                     
      
  
                                                 
                                                        
  
                                                          
                               
  
                                                          
                                        
   
JsonObject *
pcv_ovn_router_detail(const gchar *name)
{
    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "name", name ? name : "");                      
    if (!g_ovn_available || !name) return obj;                            
    if (!_valid_ovn_id(name)) return obj;

                                            
    {
        const gchar *argv[] = {"ovn-nbctl", "lrp-list", name, NULL};
        gchar *out = NULL;
        JsonArray *ports = json_array_new();
        if (_run_argv(argv, &out, NULL) && out) {
            gchar **lines = g_strsplit(g_strstrip(out), "\n", -1);
            for (gchar **l = lines; *l; l++) {
                if (!**l) continue;                            
                                          
                gchar *lp = strchr(*l, '(');
                gchar *rp = lp ? strchr(lp, ')') : NULL;
                if (lp && rp) {                             
                    gchar *pname = g_strndup(lp + 1, rp - lp - 1);
                    JsonObject *pobj = json_object_new();
                    json_object_set_string_member(pobj, "name", pname);

                                                          
                                                                  
                                                           
                    const gchar *margv[] = {"ovn-nbctl", "get", "Logical_Router_Port", pname, "mac", NULL};
                    gchar *mac_out = NULL;
                    if (_run_argv(margv, &mac_out, NULL) && mac_out)
                        json_object_set_string_member(pobj, "mac", g_strstrip(mac_out));                      
                    g_free(mac_out);

                                                         
                    const gchar *nargv[] = {"ovn-nbctl", "get", "Logical_Router_Port", pname, "networks", NULL};
                    gchar *net_out = NULL;
                    if (_run_argv(nargv, &net_out, NULL) && net_out)
                        json_object_set_string_member(pobj, "networks", g_strstrip(net_out));
                    g_free(net_out);

                    json_array_add_object_element(ports, pobj);                           
                    g_free(pname);
                }
            }
            g_strfreev(lines);
        }
        g_free(out);
        json_object_set_int_member(obj, "port_count", (gint64)json_array_get_length(ports));
        json_object_set_array_member(obj, "ports", ports);               
    }

                                      
    {
        JsonArray *nats = pcv_ovn_nat_list(name);
        json_object_set_int_member(obj, "nat_count", (gint64)json_array_get_length(nats));
        json_object_set_array_member(obj, "nats", nats);               
    }

    return obj;
}

                                                                     

gboolean
pcv_ovn_resource_is_local_vpc_owned(const gchar *table,
                                    const gchar *name,
                                    GError **error)
{
                                                             
                                                             
    const gchar *allowed[] = {
        "Logical_Switch", "Logical_Switch_Port", "Logical_Router",
        "Logical_Router_Port", "Port_Group", "DHCP_Options", NULL
    };
    gboolean table_ok = FALSE;
    for (guint i = 0; allowed[i]; i++)
        if (g_strcmp0(table, allowed[i]) == 0) {
            table_ok = TRUE;
            break;
        }
    if (!table_ok || !_valid_ovn_id(name)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "invalid OVN ownership lookup");
        return FALSE;
    }
    if (!g_ovn_available)
        return FALSE;

    const gchar *argv[] = { "ovn-nbctl", "--timeout=10", "--if-exists", "get",
        table, name, "external_ids:purecvisor-owner", NULL };
    g_autofree gchar *out = NULL;
    if (!_run_argv(argv, &out, error))
        return FALSE;
    gchar *value = g_strstrip(out ? out : "");
    gsize length = strlen(value);
    if (length >= 2 && value[0] == '"' && value[length - 1] == '"') {
        value[length - 1] = '\0';
        value++;
    }
    return g_strcmp0(value, "local-vpc") == 0;
}

   
                  
  
                          
                                                           
  
                 
      
                         
                         
                                    
                                    
                             
                                     
                                  
                            
                         
                        
      
  
                                               
                                                     
  
                                                                      
                                                         
                              
  
                                                          
   
JsonObject *
pcv_ovn_status(void)
{
    JsonObject *obj = json_object_new();
    const gchar *version_argv[] = {"ovn-nbctl", "--version", NULL};
    const gchar *nb_argv[] = {"ovn-nbctl", "--timeout=5", "list", "NB_Global", NULL};
    const gchar *sb_argv[] = {"ovn-sbctl", "--timeout=5", "list", "SB_Global", NULL};
    const gchar *sync_argv[] = {"ovn-nbctl", "--timeout=5", "--wait=sb", "sync", NULL};
    const gchar *system_id_argv[] = {
        "ovs-vsctl", "--if-exists", "get", "Open_vSwitch", ".", "external_ids:system-id", NULL
    };
    const gchar *remote_argv[] = {
        "ovs-vsctl", "--if-exists", "get", "Open_vSwitch", ".", "external_ids:ovn-remote", NULL
    };
    gchar *out = NULL, *errout = NULL;
    gboolean installed = pcv_spawn_sync(version_argv, &out, &errout, NULL);
    json_object_set_boolean_member(obj, "installed", installed);
    if (installed && out) {
            gchar *nl = strchr(out, '\n');                             
            if (nl) *nl = '\0';                                                    
            json_object_set_string_member(obj, "version", g_strstrip(out));
    }
    g_free(out);
    g_free(errout);

    gboolean northbound = installed && pcv_spawn_sync(nb_argv, NULL, NULL, NULL);
    gboolean southbound = installed && pcv_spawn_sync(sb_argv, NULL, NULL, NULL);
    gboolean northd_synced = northbound && southbound &&
        pcv_spawn_sync(sync_argv, NULL, NULL, NULL);

    gchar *system_id = NULL;
    gchar *remote = NULL;
    gboolean have_system_id = pcv_spawn_sync(system_id_argv, &system_id, NULL, NULL) &&
        system_id && *g_strstrip(system_id) && g_strcmp0(system_id, "[]") != 0;
    gboolean have_remote = pcv_spawn_sync(remote_argv, &remote, NULL, NULL) &&
        remote && *g_strstrip(remote) && g_strcmp0(remote, "[]") != 0;
    if (have_system_id && system_id[0] == '"') {
        gsize length = strlen(system_id);
        if (length > 1 && system_id[length - 1] == '"') {
            system_id[length - 1] = '\0';
            memmove(system_id, system_id + 1, length - 1);
        }
    }
    gboolean chassis_registered = FALSE;
    if (southbound && have_system_id) {
        gchar *match = g_strdup_printf("name=%s", system_id);
        const gchar *chassis_argv[] = {
            "ovn-sbctl", "--timeout=5", "--data=bare", "--no-heading",
            "--columns=name", "find", "Chassis", match, NULL
        };
        gchar *chassis = NULL;
        chassis_registered = pcv_spawn_sync(chassis_argv, &chassis, NULL, NULL) &&
            chassis && *g_strstrip(chassis);
        g_free(chassis);
        g_free(match);
    }

    gboolean controller_configured = have_system_id && have_remote;
    gboolean available = northbound && southbound && northd_synced &&
        controller_configured && chassis_registered;
    json_object_set_boolean_member(obj, "northbound_connected", northbound);
    json_object_set_boolean_member(obj, "southbound_connected", southbound);
    json_object_set_boolean_member(obj, "northd_synced", northd_synced);
    json_object_set_boolean_member(obj, "controller_configured", controller_configured);
    json_object_set_boolean_member(obj, "chassis_registered", chassis_registered);
    json_object_set_boolean_member(obj, "available", available);
    if (have_system_id)
        json_object_set_string_member(obj, "system_id", system_id);
    g_free(system_id);
    g_free(remote);

    if (northbound) {
                                                           
        JsonArray *switches = pcv_ovn_switch_list();
        json_object_set_int_member(obj, "switch_count", json_array_get_length(switches));
        json_array_unref(switches);

        JsonArray *routers = pcv_ovn_router_list();
        json_object_set_int_member(obj, "router_count", json_array_get_length(routers));
        json_array_unref(routers);
    }
    return obj;
}
