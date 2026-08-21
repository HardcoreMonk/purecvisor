   
                       
                                                
  
                           
                                                   
                                                    
                                        
  
          
                                                      
                                                     
                                                
                                                      
  
                    
                                                          
                                                             
                                                         
  
                                                                       
            
                                              
  
                                                             
                               
                                  
  
             
                                                             
                                            
                                                
                               
                                                               
                                            
                                         
  
                    
                                              
                                                   
                             
                              
                           
                                           
                                        
  
                                                               
                                                     
                                                           
                                                          
                                                 
  
          
                                                            
                                 
  
          
                                
                                           
  
         
                                                               
                                                           
                                                
                                                  
                                                 
                                                                       
   
#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <string.h>
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>

#include "modules/network/network_dhcp.h"
#include "../../utils/pcv_validate.h"
#include "../../utils/pcv_spawn.h"
#include "../../utils/pcv_log.h"

                                                         
                                                                
gboolean network_dhcp_start(const gchar *bridge_name, const gchar *cidr, GError **error) {
    return network_dhcp_start_ex(bridge_name, cidr, FALSE, NULL, error);
}

   
                                                     
  
                                               
                                                       
                                        
  
                                                    
                                                  
   
gboolean network_dhcp_stop(const gchar *bridge_name, GError **error) {
    if (!pcv_validate_bridge_name(bridge_name)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Invalid bridge name for DHCP stop: %s",
                    bridge_name ? bridge_name : "(null)");
        return FALSE;
    }

    gchar *pid_path = g_strdup_printf(
        PCV_NETWORK_RUNDIR "/dnsmasq-%s.pid", bridge_name);
    gchar *pid_text = NULL;
    if (!g_file_get_contents(pid_path, &pid_text, NULL, NULL)) {
        g_free(pid_path);
        return TRUE;                                     
    }

    gchar *end = NULL;
    gint64 parsed = g_ascii_strtoll(pid_text, &end, 10);
    while (end && g_ascii_isspace(*end)) end++;
    if (end == pid_text || (end && *end != '\0') || parsed <= 1 || parsed > G_MAXINT) {
        PCV_LOG_WARN("network", "DHCP stale PID file removed for %s: invalid PID", bridge_name);
        g_unlink(pid_path);
        g_free(pid_text);
        g_free(pid_path);
        return TRUE;
    }
    pid_t pid = (pid_t)parsed;

    gchar *comm_path = g_strdup_printf("/proc/%d/comm", (gint)pid);
    gchar *comm = NULL;
    gboolean have_comm = g_file_get_contents(comm_path, &comm, NULL, NULL);
    if (comm) g_strstrip(comm);
    if (!have_comm || g_strcmp0(comm, "dnsmasq") != 0) {
                                                         
        PCV_LOG_WARN("network",
                     "DHCP stale PID file removed for %s: pid=%d owner=%s",
                     bridge_name, (gint)pid, comm ? comm : "absent");
        g_unlink(pid_path);
        g_free(comm);
        g_free(comm_path);
        g_free(pid_text);
        g_free(pid_path);
        return TRUE;
    }
    g_free(comm);
    g_free(comm_path);

                                                             
                                                                     
                                                         
    gchar pid_arg[32];
    g_snprintf(pid_arg, sizeof(pid_arg), "%d", (gint)pid);
    const gchar *term_argv[] = {"kill", "-TERM", pid_arg, NULL};
    GError *term_error = NULL;
    if (!pcv_spawn_sync_timeout(term_argv, NULL, NULL, 5, &term_error)) {
                                             
        if (!(kill(pid, 0) != 0 && errno == ESRCH)) {
            if (error)
                g_propagate_prefixed_error(error, term_error,
                    "Failed to stop dnsmasq for %s (pid %d): ",
                    bridge_name, (gint)pid);
            else
                g_clear_error(&term_error);
            g_free(pid_text);
            g_free(pid_path);
            return FALSE;
        }
        g_clear_error(&term_error);
    }

    gboolean stopped = FALSE;
    for (guint attempt = 0; attempt < 100; attempt++) {
        if (kill(pid, 0) != 0 && errno == ESRCH) {
            stopped = TRUE;
            break;
        }
        g_usleep(10 * 1000);                                    
    }
    if (!stopped) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                    "dnsmasq for %s (pid %d) did not stop within 1 second",
                    bridge_name, (gint)pid);
        g_free(pid_text);
        g_free(pid_path);
        return FALSE;
    }

    g_unlink(pid_path);
    g_free(pid_text);
    g_free(pid_path);
    return TRUE;
}

                                                  
  
                                                         
                                                   
  
                                                                    
                                                                    
                                                                     
gboolean network_dhcp_start_ex(const gchar *bridge_name,
                                const gchar *cidr,
                                gboolean     dns_enabled,
                                const gchar *upstream_dns,
                                GError     **error) {
                                                              
    gchar **parts = g_strsplit(cidr, ".", 4);
    if (!parts || g_strv_length(parts) != 4) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Invalid CIDR for DHCP");
        g_strfreev(parts);
        return FALSE;
    }
    gchar *base_ip = g_strdup_printf("%s.%s.%s", parts[0], parts[1], parts[2]);
    g_strfreev(parts);

                                          
                                                          
                                                         
                                                             
    const gchar *slash = g_strrstr(cidr, "/");
    int prefix    = slash ? atoi(slash + 1) : 24;
                                                       
                                     
    int max_host  = (prefix <= 30) ? ((1 << (32 - prefix)) - 2) : 1;
    int dhcp_s    = 2;                          
    int dhcp_e    = dhcp_s + max_host - 1;
    if (dhcp_e > 254) dhcp_e = 254;                    
    if (dhcp_e < dhcp_s) dhcp_e = dhcp_s;
    gchar *dhcp_start = g_strdup_printf("%s.%d", base_ip, dhcp_s);
    gchar *dhcp_end   = g_strdup_printf("%s.%d", base_ip, dhcp_e);

                                               
    gchar *conf_path  = g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-%s.conf",   bridge_name);
    gchar *pid_path   = g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-%s.pid",    bridge_name);
    gchar *lease_path = g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-%s.leases", bridge_name);

                          
                                                                                       
                                                                
                                                                     
                                                                           
                                                                       
                                                        
                                       
                                                                    
                                                                                        
    const gchar *safe_dns = "8.8.8.8";
    if (dns_enabled && upstream_dns) {
        struct in_addr addr;
        if (inet_pton(AF_INET, upstream_dns, &addr) == 1) {
            safe_dns = upstream_dns;
        } else {
            g_warning("[DHCP] Invalid upstream_dns '%s' — falling back to 8.8.8.8", upstream_dns);
        }
    }
    gchar *dns_section = dns_enabled
        ? g_strdup_printf("server=%s\n", safe_dns)
        : g_strdup("port=0\nno-resolv\n");

    gchar *conf_content = g_strdup_printf(
        "%s"
        "bind-interfaces\n"
        "interface=%s\n"
        "dhcp-range=%s,%s,12h\n"
        "dhcp-leasefile=%s\n"
        "pid-file=%s\n",
        dns_section, bridge_name, dhcp_start, dhcp_end, lease_path, pid_path
    );
    g_free(dns_section);

                       
    GError *write_err = NULL;
    if (!g_file_set_contents(conf_path, conf_content, -1, &write_err)) {
        g_propagate_error(error, write_err);
        goto cleanup;
    }

                                           
                                                             
    if (!network_dhcp_stop(bridge_name, error))
        goto cleanup;

                          
                                                        
                                                                   
                                                          
                                                    
    {
        gchar *conf_arg = g_strdup_printf("--conf-file=%s", conf_path);
        const gchar *dns_argv[] = {"dnsmasq", conf_arg, NULL};
        gchar *std_err = NULL;
        gboolean ok = pcv_spawn_sync(dns_argv, NULL, &std_err, error);
        g_free(conf_arg);
        if (!ok) {
            if (error && !*error)
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "dnsmasq failed: %s", std_err ? std_err : "unknown");
            g_free(std_err);
            goto cleanup;
        }
        g_free(std_err);
    }

cleanup:
    g_free(base_ip); g_free(dhcp_start); g_free(dhcp_end);
    g_free(conf_path); g_free(pid_path); g_free(lease_path); g_free(conf_content);

                                                              
    return (error == NULL || *error == NULL);
}

                                                                         
                                            
  
                                                       
                                                    
  
       
                                        
                                                     
                                                                      
  
          
                                                               
                                                                 
                                                            
  
         
                                           
                                                   
  
          
                                          
                                           
  
         
                                       
                                                 
                                                                            
gboolean network_dhcp_start_v6(const gchar *bridge_name,
                                const gchar *ipv6_prefix,
                                GError     **error)
{
    if (!bridge_name || !ipv6_prefix) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "bridge_name and ipv6_prefix are required");
        return FALSE;
    }

                                                 
                                                           
                                                          
                                              
                                                 
    if (!pcv_validate_ipv6_prefix(ipv6_prefix)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Invalid ipv6_prefix (rejected by whitelist validator): %s",
                    ipv6_prefix);
        return FALSE;
    }

                                                                           
    const gchar *slash = g_strrstr(ipv6_prefix, "/");
    if (!slash) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "IPv6 prefix must include prefix length (e.g., fd00:1::/64)");
        return FALSE;
    }
    gint prefix_len = atoi(slash + 1);
    if (prefix_len < 48 || prefix_len > 128) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "IPv6 prefix length must be 48-128, got %d", prefix_len);
        return FALSE;
    }

                               
    gchar *prefix_base = g_strndup(ipv6_prefix, (gsize)(slash - ipv6_prefix));

                                                 
    gchar *v6_start = NULL;
    gchar *v6_end   = NULL;
    gchar *v6_gw    = NULL;

                                        
    if (g_str_has_suffix(prefix_base, "::")) {
        v6_start = g_strdup_printf("%s100", prefix_base);
        v6_end   = g_strdup_printf("%s1ff", prefix_base);
        v6_gw    = g_strdup_printf("%s1", prefix_base);
    } else if (g_str_has_suffix(prefix_base, ":")) {
        v6_start = g_strdup_printf("%s:100", prefix_base);
        v6_end   = g_strdup_printf("%s:1ff", prefix_base);
        v6_gw    = g_strdup_printf("%s:1", prefix_base);
    } else {
        v6_start = g_strdup_printf("%s::100", prefix_base);
        v6_end   = g_strdup_printf("%s::1ff", prefix_base);
        v6_gw    = g_strdup_printf("%s::1", prefix_base);
    }

                                               
    gchar *conf_path = g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-%s.conf", bridge_name);
    gchar *pid_path  = g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-%s.pid",  bridge_name);

    gchar *v6_config = g_strdup_printf(
        "\n# IPv6 RA + DHCPv6 (auto-generated)\n"
        "enable-ra\n"
        "dhcp-range=%s,%s,%d,12h\n"
        "dhcp-option=option6:dns-server,[%s]\n",
        v6_start, v6_end, prefix_len, v6_gw
    );

                                   
    gchar *existing = NULL;
    gsize existing_len = 0;
    if (g_file_get_contents(conf_path, &existing, &existing_len, NULL)) {
        gchar *merged = g_strdup_printf("%s%s", existing, v6_config);
        GError *write_err = NULL;
        if (!g_file_set_contents(conf_path, merged, -1, &write_err)) {
            g_propagate_error(error, write_err);
            g_free(merged); g_free(existing);
            goto cleanup_v6;
        }
        g_free(merged); g_free(existing);
    } else {
                                         
        gchar *lease_path = g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-%s.leases", bridge_name);
        gchar *full = g_strdup_printf(
            "port=0\nno-resolv\n"
            "bind-interfaces\n"
            "interface=%s\n"
            "dhcp-leasefile=%s\n"
            "pid-file=%s\n"
            "%s",
            bridge_name, lease_path, pid_path, v6_config
        );
        GError *write_err = NULL;
        if (!g_file_set_contents(conf_path, full, -1, &write_err)) {
            g_propagate_error(error, write_err);
            g_free(full); g_free(lease_path);
            goto cleanup_v6;
        }
        g_free(full); g_free(lease_path);
    }

                                                            
    if (!network_dhcp_stop(bridge_name, error))
        goto cleanup_v6;
    {
        gchar *conf_arg = g_strdup_printf("--conf-file=%s", conf_path);
        const gchar *dns_argv[] = {"dnsmasq", conf_arg, NULL};
        gchar *std_err = NULL;
        gboolean ok = pcv_spawn_sync(dns_argv, NULL, &std_err, error);
        g_free(conf_arg);
        if (!ok) {
            if (error && !*error)
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "dnsmasq IPv6 restart failed: %s", std_err ? std_err : "unknown");
            g_free(std_err);
            goto cleanup_v6;
        }
        g_free(std_err);
    }

    g_message("[DHCP] IPv6 RA+DHCPv6 enabled on %s: %s-%s/%d",
              bridge_name, v6_start, v6_end, prefix_len);

cleanup_v6:
    g_free(prefix_base);
    g_free(v6_start); g_free(v6_end); g_free(v6_gw);
    g_free(v6_config);
    g_free(conf_path); g_free(pid_path);

    return (error == NULL || *error == NULL);
}

                                                                         
                                                                          
  
       
                                                               
                                                              
                                                        
                                                            
                                                                 
                                                                  
  
                           
                                                                         
                                                               
                                                           
                                                                
  
                                
                                                             
                                                  
                                                                   
                                                  
                                                             
                                                            
                                                                 
                                                              
                                                                            

   
                                                                
                                                         
                               
  
                                                       
                                                   
                                                   
   
gboolean network_dhcp_reserve_overlay_ip(const gchar *ep_name,
                                          const gchar *tap_iface,
                                          const gchar *gw_cidr,
                                          const gchar *guest_mac,
                                          const gchar *overlay_ip,
                                          GError     **error) {
    if (!ep_name || !tap_iface || !gw_cidr || !guest_mac || !overlay_ip) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "ep_name, tap_iface, gw_cidr, guest_mac, overlay_ip are required");
        return FALSE;
    }

                                                         
                                                                         
                                                           
                                                                           
    if (!pcv_validate_bridge_name(ep_name)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Invalid ep_name: %s", ep_name);
        return FALSE;
    }
    if (!pcv_validate_bridge_name(tap_iface)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Invalid tap_iface: %s", tap_iface);
        return FALSE;
    }
    if (!pcv_validate_cidr(gw_cidr)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Invalid gw_cidr: %s", gw_cidr);
        return FALSE;
    }
    if (!pcv_validate_mac(guest_mac)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Invalid guest_mac: %s", guest_mac);
        return FALSE;
    }
    if (!pcv_validate_ip_literal(overlay_ip)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Invalid overlay_ip: %s", overlay_ip);
        return FALSE;
    }

                                                           
                                                    
    gchar **parts = g_strsplit(gw_cidr, ".", 4);
    if (!parts || g_strv_length(parts) != 4) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Cannot derive DHCP range from gw_cidr: %s", gw_cidr);
        g_strfreev(parts);
        return FALSE;
    }
    gchar *net_base = g_strdup_printf("%s.%s.%s", parts[0], parts[1], parts[2]);
    g_strfreev(parts);

                                                                            
    gchar *conf_path  = g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-ovl-%s.conf",   ep_name);
    gchar *pid_path   = g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-ovl-%s.pid",    ep_name);
    gchar *lease_path = g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-ovl-%s.leases", ep_name);

                                        
    g_mkdir_with_parents(PCV_NETWORK_RUNDIR, 0700);

                                                                    
                                             
    gchar *conf_content = g_strdup_printf(
        "port=0\n"
        "no-resolv\n"
        "bind-interfaces\n"
        "interface=%s\n"
        "except-interface=lo\n"
        "dhcp-authoritative\n"
        "dhcp-range=%s.2,%s.254,255.255.255.0,12h\n"
        "dhcp-host=%s,%s\n"
        "dhcp-option-force=26,1420\n"
        "dhcp-leasefile=%s\n"
        "pid-file=%s\n",
        tap_iface, net_base, net_base, guest_mac, overlay_ip, lease_path, pid_path);

    gboolean ok = FALSE;

                                                       
    {
        const gchar *kill_argv[] = {"pkill", "-F", pid_path, NULL};
        (void)pcv_spawn_sync_timeout(kill_argv, NULL, NULL, 30, NULL);
    }

                       
    GError *write_err = NULL;
    if (!g_file_set_contents(conf_path, conf_content, -1, &write_err)) {
        g_propagate_error(error, write_err);
        goto out;
    }

                                                            
                                     
    {
        gchar *conf_arg = g_strdup_printf("--conf-file=%s", conf_path);
        const gchar *dns_argv[] = {
            "ip", "netns", "exec", ep_name, "dnsmasq", conf_arg, NULL
        };
        gchar *std_err = NULL;
        gboolean spawned = pcv_spawn_sync_timeout(dns_argv, NULL, &std_err, 30, error);
        g_free(conf_arg);
        if (!spawned) {
            if (error && !*error)
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "netns dnsmasq failed: %s", std_err ? std_err : "unknown");
            g_free(std_err);
                                                                 
            (void)network_dhcp_release_overlay_ip(ep_name, NULL);
            goto out;
        }
        g_free(std_err);
    }
    ok = TRUE;

out:
    g_free(net_base);
    g_free(conf_path); g_free(pid_path); g_free(lease_path);
    g_free(conf_content);
    return ok;
}

   
                                                                              
                                   
  
                                                        
                         
   
gboolean network_dhcp_release_overlay_ip(const gchar *ep_name,
                                          GError     **error) {
    if (!ep_name) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "ep_name is required");
        return FALSE;
    }
    if (!pcv_validate_bridge_name(ep_name)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Invalid ep_name: %s", ep_name);
        return FALSE;
    }

    gchar *conf_path  = g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-ovl-%s.conf",   ep_name);
    gchar *pid_path   = g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-ovl-%s.pid",    ep_name);
    gchar *lease_path = g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-ovl-%s.leases", ep_name);

                                                                             
    {
        const gchar *kill_argv[] = {"pkill", "-F", pid_path, NULL};
        (void)pcv_spawn_sync_timeout(kill_argv, NULL, NULL, 30, NULL);
    }

                                          
    g_unlink(conf_path);
    g_unlink(pid_path);
    g_unlink(lease_path);

    g_free(conf_path); g_free(pid_path); g_free(lease_path);
    return TRUE;
}
