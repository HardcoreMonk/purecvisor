   
                                
                                                    
  
                           
                                                   
                                                    
                                        
  
                                                 
                                                   
                                                         
                                                
                                                    
  
                                                                       
      
                                                        
                                                                             
                                                                         
                                                                      
                                                 
  
                 
                                                                  
                                                                     
                                           
                                                   
  
                       
                                  
                                              
                                                          
                                                               
                                                             
  
               
                                                     
                                                     
                                               
  
              
                                                                          
                                                      
                                                                       
   
#include <glib.h>
#include <gio/gio.h>
#include <string.h>

#include "modules/network/network_firewall_host.h"
#include "../../utils/pcv_spawn.h"
#include "../../utils/pcv_validate.h"
#include "../../utils/pcv_log.h"

#define HOST_FW_LOG_DOM "network_fw_host"

                     
                                                     
static const gchar *_state_name(PcvHostFwState st) {
    switch (st) {
    case PCV_HOST_FW_UFW:           return "ufw";
    case PCV_HOST_FW_IPTABLES_DROP: return "iptables_drop";
    case PCV_HOST_FW_FIREWALLD:     return "firewalld";
    case PCV_HOST_FW_OPEN:          return "open";
    default:                        return "unknown";
    }
}

                                                            
  
                                                        
                                           
                                             
                                                 
static gboolean _ufw_enabled(void) {
    gchar *content = NULL;
    if (!g_file_get_contents("/etc/ufw/ufw.conf", &content, NULL, NULL))
        return FALSE;                                                      

    gboolean enabled = FALSE;
    gchar **lines = g_strsplit(content, "\n", -1);
    for (gchar **l = lines; *l; l++) {
        gchar *s = g_strstrip(g_strdup(*l));                           
        if (s[0] == '#' || s[0] == '\0') { g_free(s); continue; }                  
        if (g_str_has_prefix(s, "ENABLED")) {
            const gchar *eq = strchr(s, '=');
            if (eq) {
                gchar *val = g_strstrip(g_strdup(eq + 1));                      
                if (g_ascii_strcasecmp(val, "yes") == 0) enabled = TRUE;                  
                g_free(val);
            }
        }
        g_free(s);
    }
    g_strfreev(lines);
    g_free(content);
    return enabled;
}

                                       
  
                                                               
                                                          
                                                           
static gboolean _iptables_forward_drop(void) {
    const gchar *argv[] = {"iptables", "-S", "FORWARD", NULL};
    gchar *out = NULL;
    if (!pcv_spawn_sync(argv, &out, NULL, NULL) || !out) {
        g_free(out);
        return FALSE;
    }
    gboolean drop = FALSE;
    gchar **lines = g_strsplit(out, "\n", -1);
    if (lines[0]) {
        gchar *first = g_strstrip(g_strdup(lines[0]));
        drop = (g_strcmp0(first, "-P FORWARD DROP") == 0);
        g_free(first);
    }
    g_strfreev(lines);
    g_free(out);
    return drop;
}

                                       
  
                                                                     
                       
                                                             
static gboolean _firewalld_active(void) {
    gchar *path = g_find_program_in_path("firewall-cmd");
    if (!path) return FALSE;
    g_free(path);
    const gchar *argv[] = {"firewall-cmd", "--state", NULL};
    gboolean running = pcv_spawn_sync(argv, NULL, NULL, NULL);
    return running;
}

                                                        
                                                        
                                                           
PcvHostFwState pcv_host_fw_detect(void) {
                                       
    gchar *ufw = g_find_program_in_path("ufw");
    if (ufw) {
        gboolean en = _ufw_enabled();
        g_free(ufw);
        if (en) return PCV_HOST_FW_UFW;
    }
                                                 
    if (_iptables_forward_drop())
        return PCV_HOST_FW_IPTABLES_DROP;
                                           
    if (_firewalld_active())
        return PCV_HOST_FW_FIREWALLD;
                       
    return PCV_HOST_FW_OPEN;
}

                                             
  
                                                   
                                                       
                                                  

                          
static gboolean
_hfw_known_mode(const gchar *mode)
{
    return g_strcmp0(mode, "nat") == 0 || g_strcmp0(mode, "routed") == 0 || g_strcmp0(mode, "isolated") == 0;
}

                                                                                                   
                                                                     
static void
_ufw_rules(GPtrArray *out, const gchar *bridge, const gchar *mode)
{
    if (!_hfw_known_mode(mode))
        return;
    if (g_strcmp0(mode, "isolated") != 0)
        g_ptr_array_add(out, g_strdup_printf("route allow in on %s", bridge));
    if (g_strcmp0(mode, "routed") == 0)
        g_ptr_array_add(out, g_strdup_printf("route allow out on %s", bridge));
    g_ptr_array_add(out, g_strdup_printf("allow in on %s to any port 67 proto udp", bridge));
    g_ptr_array_add(out, g_strdup_printf("allow in on %s to any port 53 proto udp", bridge));
    g_ptr_array_add(out, g_strdup_printf("allow in on %s to any port 53 proto tcp", bridge));
    g_ptr_array_add(out, g_strdup_printf("allow in on %s to any port 547 proto udp", bridge));
}

                                                                                  
                                                                                           
static gchar *
_ufw_delete_cmd(const gchar *rule)
{
    if (g_str_has_prefix(rule, "route "))
        return g_strdup_printf("ufw --force route delete %s", rule + 6);
    return g_strdup_printf("ufw --force delete %s", rule);
}

                                                                                                
static void
_iptables_rules(GPtrArray *out, const gchar *op, const gchar *bridge, const gchar *mode)
{
    if (!_hfw_known_mode(mode))
        return;
    if (g_strcmp0(mode, "isolated") != 0)
        g_ptr_array_add(out, g_strdup_printf("iptables %s FORWARD -i %s -j ACCEPT", op, bridge));
    if (g_strcmp0(mode, "nat") == 0)
        g_ptr_array_add(out, g_strdup_printf(
            "iptables %s FORWARD -o %s -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT", op, bridge));
    if (g_strcmp0(mode, "routed") == 0)
        g_ptr_array_add(out, g_strdup_printf("iptables %s FORWARD -o %s -j ACCEPT", op, bridge));
    g_ptr_array_add(out, g_strdup_printf("iptables %s INPUT -i %s -p udp --dport 67 -j ACCEPT", op, bridge));
    g_ptr_array_add(out, g_strdup_printf("iptables %s INPUT -i %s -p udp --dport 53 -j ACCEPT", op, bridge));
    g_ptr_array_add(out, g_strdup_printf("iptables %s INPUT -i %s -p tcp --dport 53 -j ACCEPT", op, bridge));
    g_ptr_array_add(out, g_strdup_printf("iptables %s INPUT -i %s -p icmp --icmp-type echo-request -j ACCEPT", op, bridge));
}

                      
static gboolean
_hfw_has(GPtrArray *a, const gchar *s)
{
    for (guint i = 0; i < a->len; i++)
        if (g_strcmp0(g_ptr_array_index(a, i), s) == 0)
            return TRUE;
    return FALSE;
}

                                                    
GPtrArray *
pcv_host_fw_plan(PcvHostFwState st, const gchar *bridge, const gchar *mode, gboolean remove)
{
    GPtrArray *cmds = g_ptr_array_new_with_free_func(g_free);
    if (!bridge)
        return cmds;
    if (st == PCV_HOST_FW_UFW) {
        g_autoptr(GPtrArray) rules = g_ptr_array_new_with_free_func(g_free);
        _ufw_rules(rules, bridge, mode);
        for (guint i = 0; i < rules->len; i++) {
            const gchar *rule = g_ptr_array_index(rules, i);
            g_ptr_array_add(cmds, remove ? _ufw_delete_cmd(rule) : g_strdup_printf("ufw %s", rule));
        }
    } else if (st == PCV_HOST_FW_IPTABLES_DROP) {
        _iptables_rules(cmds, remove ? "-D" : "-I", bridge, mode);
    }
    return cmds;                             
}

                                       
static gchar *
_ufw_norm(const gchar *line)
{
    g_auto(GStrv) toks = g_strsplit_set(line, " \t\r", -1);
    GString *s = g_string_new(NULL);
    for (gchar **t = toks; *t; t++) {
        if (**t == '\0')
            continue;
        if (s->len)
            g_string_append_c(s, ' ');
        g_string_append(s, *t);
    }
    return g_string_free(s, FALSE);
}

             
void
pcv_host_fw_ufw_diff(const gchar *show_added, const gchar *bridge, const gchar *mode,
                     GPtrArray **to_delete, GPtrArray **to_add)
{
    *to_delete = g_ptr_array_new_with_free_func(g_free);
    *to_add = g_ptr_array_new_with_free_func(g_free);
    if (!bridge)
        return;
    g_autoptr(GPtrArray) target = g_ptr_array_new_with_free_func(g_free);
    _ufw_rules(target, bridge, mode);
    g_autoptr(GPtrArray) known = g_ptr_array_new_with_free_func(g_free);
    _ufw_rules(known, bridge, "routed");                                                            
    g_ptr_array_add(known, g_strdup_printf("allow in on %s", bridge));                         

    g_autoptr(GPtrArray) current = g_ptr_array_new_with_free_func(g_free);
    g_auto(GStrv) lines = g_strsplit(show_added ? show_added : "", "\n", -1);
    for (gchar **l = lines; *l; l++) {
        g_autofree gchar *n = _ufw_norm(*l);
        if (!g_str_has_prefix(n, "ufw "))
            continue;
        const gchar *rule = n + 4;
        if (_hfw_has(known, rule) && !_hfw_has(current, rule))
            g_ptr_array_add(current, g_strdup(rule));
    }
    for (guint i = 0; i < current->len; i++)
        if (!_hfw_has(target, g_ptr_array_index(current, i)))
            g_ptr_array_add(*to_delete, _ufw_delete_cmd(g_ptr_array_index(current, i)));
    for (guint i = 0; i < target->len; i++)
        if (!_hfw_has(current, g_ptr_array_index(target, i)))
            g_ptr_array_add(*to_add, g_strdup_printf("ufw %s", (const gchar *)g_ptr_array_index(target, i)));
}

             
GPtrArray *
pcv_host_fw_iptables_known(const gchar *bridge, const gchar *op)
{
    GPtrArray *all = g_ptr_array_new_with_free_func(g_free);
    static const gchar *const modes[] = {"nat", "routed", "isolated"};
    for (gsize m = 0; bridge && m < G_N_ELEMENTS(modes); m++) {
        g_autoptr(GPtrArray) one = g_ptr_array_new_with_free_func(g_free);
        _iptables_rules(one, op, bridge, modes[m]);
        for (guint i = 0; i < one->len; i++)
            if (!_hfw_has(all, g_ptr_array_index(one, i)))
                g_ptr_array_add(all, g_strdup(g_ptr_array_index(one, i)));
    }
    return all;
}

                                
  
                                                                
                                               
                                         
                                                        
                                                               
static gboolean _iptables_rule_exists(gchar **argv) {
    gchar *saved = argv[1];                                            
    argv[1] = (gchar *) "-C";                                        
    gboolean exists = pcv_spawn_sync((const gchar * const *) argv, NULL, NULL, NULL);                   
    argv[1] = saved;                                                  
    return exists;
}

                                                                       
                                   
static gint
_hfw_run(const gchar *cmd, gboolean adding)
{
    gchar **argv = g_strsplit(cmd, " ", -1);                                 
    gint rc = 1;
    if (g_strcmp0(argv[0], "iptables") == 0) {
        gboolean exists = _iptables_rule_exists(argv);
        if (adding == exists) {
            g_strfreev(argv);
            return 0;
        }
    }
    GError *cmd_err = NULL;
    if (!pcv_spawn_sync((const gchar * const *)argv, NULL, NULL, &cmd_err)) {
        PCV_LOG_WARN(HOST_FW_LOG_DOM, "host firewall 명령 실패 (계속): '%s' — %s",
                     cmd, cmd_err ? cmd_err->message : "unknown");
        g_clear_error(&cmd_err);
        rc = -1;
    }
    g_strfreev(argv);
    return rc;
}

                                                      
                                                                           
                                                           
static gboolean
_host_fw_apply(const gchar *bridge, const gchar *mode, GError **error)
{
    const gchar *op_name = mode ? "network.host_fw_integrate" : "network.host_fw_remove";
    if (!pcv_validate_bridge_name(bridge)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Invalid bridge name for host firewall: %s", bridge ? bridge : "(null)");
        return FALSE;
    }
    PcvHostFwState st = pcv_host_fw_detect();
    if (st == PCV_HOST_FW_FIREWALLD) {
        PCV_LOG_WARN(HOST_FW_LOG_DOM,
                     "firewalld 활성 호스트 — 자동 개입 비범위. 브릿지 '%s' 를 firewalld zone 에 수동 등록 필요 "
                     "(DHCP 67·DNS 53·DHCPv6 547만 허용 권장)", bridge);
        PCV_LOG_AUDIT(HOST_FW_LOG_DOM, op_name, bridge, "state=firewalld result=skipped (manual firewalld zone required)");
        return TRUE;
    }
    if (st == PCV_HOST_FW_OPEN) {
        PCV_LOG_DEBUG(HOST_FW_LOG_DOM, "host firewall open — no coexistence rules needed (%s %s)", op_name, bridge);
        return TRUE;
    }

    g_autoptr(GPtrArray) dels = NULL;
    g_autoptr(GPtrArray) adds = NULL;
    gboolean all_ok = TRUE;
    if (st == PCV_HOST_FW_UFW) {
        const gchar *argv[] = {"ufw", "show", "added", NULL};
        g_autofree gchar *shown = NULL;
        GError *show_err = NULL;
        if (!pcv_spawn_sync(argv, &shown, NULL, &show_err)) {
                                                                  
            PCV_LOG_WARN(HOST_FW_LOG_DOM, "ufw show added 실패 — 더하기만 한다: %s",
                         show_err ? show_err->message : "unknown");
            g_clear_error(&show_err);
            g_clear_pointer(&shown, g_free);
            all_ok = FALSE;
            dels = g_ptr_array_new_with_free_func(g_free);
            adds = pcv_host_fw_plan(st, bridge, mode, FALSE);
        } else {
            pcv_host_fw_ufw_diff(shown, bridge, mode, &dels, &adds);
        }
    } else {
        g_autoptr(GPtrArray) keep = pcv_host_fw_plan(st, bridge, mode, TRUE);                  
        g_autoptr(GPtrArray) known = pcv_host_fw_iptables_known(bridge, "-D");
        dels = g_ptr_array_new_with_free_func(g_free);
        for (guint i = 0; i < known->len; i++)
            if (!_hfw_has(keep, g_ptr_array_index(known, i)))
                g_ptr_array_add(dels, g_strdup(g_ptr_array_index(known, i)));
        adds = pcv_host_fw_plan(st, bridge, mode, FALSE);
    }

    guint total = dels->len + adds->len, applied = 0, skipped = 0, failed = 0;
    for (guint pass = 0; pass < 2; pass++) {
        GPtrArray *list = pass == 0 ? dels : adds;
        for (guint i = 0; i < list->len; i++) {
            gint rc = _hfw_run(g_ptr_array_index(list, i), pass == 1);
            if (rc > 0) applied++;
            else if (rc == 0) skipped++;
            else { failed++; all_ok = FALSE; }
        }
    }
    PCV_LOG_AUDIT(HOST_FW_LOG_DOM, op_name, bridge,
                  "state=%s mode=%s total=%u applied=%u skipped=%u failed=%u result=%s",
                  _state_name(st), mode ? mode : "-", total, applied, skipped, failed, all_ok ? "ok" : "partial");
    if (!all_ok) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "host firewall %s partial failure (%u of %u commands failed)", op_name, failed, total);
        return FALSE;
    }
    return TRUE;
}

                                                                  
gboolean
pcv_host_fw_integrate(const gchar *bridge, const gchar *mode, GError **error)
{
    if (!mode) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "host firewall integrate needs a mode");
        return FALSE;
    }
    return _host_fw_apply(bridge, mode, error);
}

                                         
gboolean
pcv_host_fw_remove(const gchar *bridge, GError **error)
{
    return _host_fw_apply(bridge, NULL, error);
}
