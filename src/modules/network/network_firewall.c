




















































































#include <glib.h>
#include <gio/gio.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "modules/network/network_firewall.h"
#include "../../utils/pcv_spawn.h"
#include "../../utils/pcv_validate.h"






















static void _ensure_table(void) {

    { const gchar *a[] = {"nft","add","table","inet","purecvisor",NULL};          pcv_spawn_fire(a); }

    { const gchar *a[] = {"nft","add","chain","inet","purecvisor","postrouting",
                           "{ type nat hook postrouting priority srcnat; }",NULL}; pcv_spawn_fire(a); }

    { const gchar *a[] = {"nft","add","chain","inet","purecvisor","forward",
                           "{ type filter hook forward priority filter; }",NULL};  pcv_spawn_fire(a); }
}
















static gchar *_cidr_to_subnet(const gchar *cidr, GError **error) {

    if (!cidr) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "CIDR is NULL");
        return NULL;
    }

    gchar **parts = g_strsplit(cidr, ".", 4);
    if (!parts || g_strv_length(parts) != 4) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Invalid CIDR: %s", cidr);
        g_strfreev(parts);
        return NULL;
    }

    gchar **last = g_strsplit(parts[3], "/", 2);

    if (!last || g_strv_length(last) < 2 || !last[1]) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Invalid CIDR format (missing /prefix): %s", cidr);
        g_strfreev(last);
        g_strfreev(parts);
        return NULL;
    }

    gchar *subnet = g_strdup_printf("%s.%s.%s.0/%s",
                                    parts[0], parts[1], parts[2], last[1]);
    g_strfreev(parts);
    g_strfreev(last);
    return subnet;
}
































gboolean network_firewall_setup_nat(const gchar *bridge_name, const gchar *cidr,
                                    GError **error) {

    if (!pcv_validate_bridge_name(bridge_name)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Invalid bridge name for firewall rule: %s",
                    bridge_name ? bridge_name : "(null)");
        return FALSE;
    }


    { const gchar *a[] = {"sysctl","-w","net.ipv4.ip_forward=1",NULL}; pcv_spawn_fire(a); }
    _ensure_table();


    gchar *subnet = _cidr_to_subnet(cidr, error);
    if (!subnet) return FALSE;



    gchar *oif_ne = g_strdup_printf("!= \"%s\"", bridge_name);
    { const gchar *a[] = {"nft","add","rule","inet","purecvisor","postrouting",
                           "oifname", oif_ne, "ip","saddr", subnet,
                           "masquerade", NULL};
      pcv_spawn_fire(a); }
    g_free(oif_ne);



    gchar *iif = g_strdup_printf("\"%s\"", bridge_name);
    { const gchar *a[] = {"nft","add","rule","inet","purecvisor","forward",
                           "iifname", iif, "accept", NULL};
      pcv_spawn_fire(a); }





    gchar *oif = g_strdup_printf("\"%s\"", bridge_name);
    { const gchar *a[] = {"nft","add","rule","inet","purecvisor","forward",
                           "oifname", oif,
                           "ct","state","established,related","accept", NULL};
      pcv_spawn_fire(a); }

    g_free(iif); g_free(oif); g_free(subnet);
    return TRUE;
}






















gboolean network_firewall_setup_isolated(const gchar *bridge_name,
                                         const gchar *cidr __attribute__((unused)),
                                         GError **error) {

    if (!pcv_validate_bridge_name(bridge_name)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Invalid bridge name for firewall rule: %s",
                    bridge_name ? bridge_name : "(null)");
        return FALSE;
    }



    { const gchar *a[] = {"sysctl","-w","net.ipv4.ip_forward=1",NULL};
      pcv_spawn_fire(a); }
    _ensure_table();

    gchar *iif = g_strdup_printf("\"%s\"", bridge_name);
    gchar *oif = g_strdup_printf("\"%s\"", bridge_name);



    { const gchar *a[] = {"nft","add","rule","inet","purecvisor","forward",
                           "iifname", iif, "oifname", oif, "accept", NULL};
      pcv_spawn_fire(a); }



    { const gchar *a[] = {"nft","add","rule","inet","purecvisor","forward",
                           "iifname", iif, "drop", NULL};
      pcv_spawn_fire(a); }



    { const gchar *a[] = {"nft","add","rule","inet","purecvisor","forward",
                           "oifname", oif, "drop", NULL};
      pcv_spawn_fire(a); }

    g_free(iif); g_free(oif);
    return TRUE;
}














gboolean network_firewall_setup_routed(const gchar *bridge_name,
                                       const gchar *cidr __attribute__((unused)),
                                       GError **error) {

    if (!pcv_validate_bridge_name(bridge_name)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Invalid bridge name for firewall rule: %s",
                    bridge_name ? bridge_name : "(null)");
        return FALSE;
    }

    { const gchar *a[] = {"sysctl","-w","net.ipv4.ip_forward=1",NULL}; pcv_spawn_fire(a); }
    _ensure_table();


    gchar *iif = g_strdup_printf("\"%s\"", bridge_name);
    { const gchar *a[] = {"nft","add","rule","inet","purecvisor","forward",
                           "iifname", iif, "accept", NULL};
      pcv_spawn_fire(a); }


    gchar *oif = g_strdup_printf("\"%s\"", bridge_name);
    { const gchar *a[] = {"nft","add","rule","inet","purecvisor","forward",
                           "oifname", oif,
                           "ct","state","established,related","accept", NULL};
      pcv_spawn_fire(a); }

    g_free(iif); g_free(oif);
    return TRUE;
}






























gboolean network_firewall_teardown(const gchar *bridge_name,
                                   GError **error) {

    if (!pcv_validate_bridge_name(bridge_name)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Invalid bridge name for firewall teardown: %s",
                    bridge_name ? bridge_name : "(null)");
        return FALSE;
    }


    const gchar *chains[] = {"postrouting", "forward", NULL};


    for (int ci = 0; chains[ci]; ci++) {

        const gchar *list_argv[] = {
            "nft", "-a", "list", "chain", "inet", "purecvisor", chains[ci], NULL};
        gchar *stdout_buf = NULL;

        pcv_spawn_sync(list_argv, &stdout_buf, NULL, NULL);
        if (!stdout_buf) continue;


        GList *handles = NULL;
        gchar **lines  = g_strsplit(stdout_buf, "\n", -1);
        g_free(stdout_buf);

        for (gchar **l = lines; *l; l++) {

            if (!strstr(*l, bridge_name)) continue;

            gchar *h_ptr = strstr(*l, "# handle ");
            if (!h_ptr) continue;
            gint handle = atoi(h_ptr + 9);
            if (handle > 0)
                handles = g_list_append(handles, GINT_TO_POINTER(handle));
        }
        g_strfreev(lines);


        handles = g_list_reverse(handles);
        for (GList *lp = handles; lp; lp = lp->next) {
            gchar *h_str = g_strdup_printf("%d", GPOINTER_TO_INT(lp->data));
            const gchar *del[] = {"nft","delete","rule","inet","purecvisor",
                                   chains[ci], "handle", h_str, NULL};
            pcv_spawn_fire(del);
            g_free(h_str);
        }
        g_list_free(handles);
    }
    return TRUE;
}
