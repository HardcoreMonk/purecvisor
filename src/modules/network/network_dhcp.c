



















































#include <glib.h>
#include <gio/gio.h>
#include <string.h>
#include <arpa/inet.h>

#include "modules/network/network_dhcp.h"
#include "../../utils/pcv_validate.h"
#include "../../utils/pcv_spawn.h"


gboolean network_dhcp_start(const gchar *bridge_name, const gchar *cidr, GError **error) {
    return network_dhcp_start_ex(bridge_name, cidr, FALSE, NULL, error);
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




    {
        const gchar *kill_argv[] = {"pkill", "-F", pid_path, NULL};
        pcv_spawn_sync(kill_argv, NULL, NULL, NULL);
    }






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

    return (*error == NULL);
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


    {
        const gchar *kill_argv[] = {"pkill", "-F", pid_path, NULL};
        pcv_spawn_sync(kill_argv, NULL, NULL, NULL);
    }
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