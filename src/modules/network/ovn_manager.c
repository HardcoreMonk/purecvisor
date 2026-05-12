















































































#include "ovn_manager.h"
#include "utils/pcv_spawn.h"
#include "utils/pcv_log.h"
#include "../../utils/pcv_config.h"
#include <string.h>

#define OVN_LOG_DOM "ovn_mgr"

static gboolean g_ovn_available = FALSE;















static gboolean
_run(const gchar *cmd, gchar **out, GError **error)
{


    gchar **parsed_argv = NULL;
    GError *parse_err = NULL;
    if (!g_shell_parse_argv(cmd, NULL, &parsed_argv, &parse_err)) {
        PCV_LOG_WARN(OVN_LOG_DOM, "cmd parse failed: %s → %s", cmd,
                     parse_err ? parse_err->message : "unknown");
        if (parse_err) {
            if (error) g_propagate_error(error, parse_err);
            else g_error_free(parse_err);
        }
        return FALSE;
    }
    gchar *std_err = NULL;
    gboolean ok = pcv_spawn_sync((const gchar * const *)parsed_argv, out, &std_err, error);
    if (!ok && std_err)
        PCV_LOG_WARN(OVN_LOG_DOM, "cmd failed: %s → %s", cmd, std_err);
    g_free(std_err);
    g_strfreev(parsed_argv);
    return ok;
}



static gboolean
_run_shell(const gchar *cmd, gchar **out, GError **error)
{
    const gchar *argv[] = {"/bin/sh", "-c", cmd, NULL};
    gchar *std_err = NULL;
    gboolean ok = pcv_spawn_sync(argv, out, &std_err, error);
    if (!ok && std_err)
        PCV_LOG_WARN(OVN_LOG_DOM, "cmd failed: %s → %s", cmd, std_err);
    g_free(std_err);
    return ok;
}










void pcv_ovn_init(void)
{
    gchar *out = NULL;
    g_ovn_available = _run_shell("ovn-nbctl --version 2>/dev/null", &out, NULL);
    g_free(out);
    if (g_ovn_available)
        PCV_LOG_INFO(OVN_LOG_DOM, "OVN available");
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
    gchar *cmd = g_strdup_printf("ovn-nbctl --may-exist ls-add %s", name);
    gboolean ok = _run(cmd, NULL, error);
    g_free(cmd);
    if (ok)
        PCV_LOG_INFO(OVN_LOG_DOM, "Logical switch '%s' created (subnet=%s)", name, subnet ? subnet : "-");
    return ok;
}











gboolean
pcv_ovn_switch_delete(const gchar *name, GError **error)
{
    if (!g_ovn_available) return TRUE;
    gchar *cmd = g_strdup_printf("ovn-nbctl --if-exists ls-del %s", name);
    gboolean ok = _run(cmd, NULL, error);
    g_free(cmd);
    return ok;
}










JsonArray *
pcv_ovn_switch_list(void)
{
    JsonArray *arr = json_array_new();
    if (!g_ovn_available) return arr;

    gchar *out = NULL;
    if (_run("ovn-nbctl ls-list", &out, NULL) && out) {

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
    gchar *cmd1 = g_strdup_printf("ovn-nbctl --may-exist lsp-add %s %s", sw, port);
    gboolean ok = _run(cmd1, NULL, error);
    g_free(cmd1);
    if (!ok) return FALSE;

    if (mac && ip) {
        gchar *cmd2 = g_strdup_printf("ovn-nbctl lsp-set-addresses %s \"%s %s\"", port, mac, ip);
        ok = _run(cmd2, NULL, error);
        g_free(cmd2);
    }
    return ok;
}











gboolean
pcv_ovn_port_remove(const gchar *sw, const gchar *port, GError **error)
{
    (void)sw;
    if (!g_ovn_available) return TRUE;
    gchar *cmd = g_strdup_printf("ovn-nbctl --if-exists lsp-del %s", port);
    gboolean ok = _run(cmd, NULL, error);
    g_free(cmd);
    return ok;
}
















gboolean
pcv_ovn_acl_add(const gchar *sw, const gchar *direction, gint priority,
                 const gchar *match, const gchar *action, GError **error)
{
    if (!g_ovn_available) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "OVN not available");
        return FALSE;
    }
    gchar *cmd = g_strdup_printf("ovn-nbctl acl-add %s %s %d '%s' %s",
                                  sw, direction, priority, match, action);
    gboolean ok = _run(cmd, NULL, error);
    g_free(cmd);
    return ok;
}













gboolean
pcv_ovn_acl_delete(const gchar *sw, const gchar *direction, gint priority,
                    const gchar *match, GError **error)
{
    if (!g_ovn_available) return TRUE;
    gchar *cmd = g_strdup_printf("ovn-nbctl acl-del %s %s %d '%s'",
                                  sw, direction, priority, match);
    gboolean ok = _run(cmd, NULL, error);
    g_free(cmd);
    return ok;
}










JsonArray *
pcv_ovn_acl_list(const gchar *sw)
{
    JsonArray *arr = json_array_new();
    if (!g_ovn_available || !sw) return arr;
    gchar *cmd = g_strdup_printf("ovn-nbctl acl-list %s", sw);
    gchar *out = NULL;
    if (_run(cmd, &out, NULL) && out) {
        gchar **lines = g_strsplit(g_strstrip(out), "\n", -1);
        for (gchar **l = lines; *l; l++) {
            if (!**l) continue;
            json_array_add_string_element(arr, *l);
        }
        g_strfreev(lines);
    }
    g_free(cmd);
    g_free(out);
    return arr;
}
















gboolean
pcv_ovn_dhcp_enable(const gchar *subnet, const gchar *gw, GError **error)
{
    if (!g_ovn_available) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "OVN not available");
        return FALSE;
    }
    gchar *cmd = g_strdup_printf("ovn-nbctl dhcp-options-create %s", subnet);
    gchar *out = NULL;
    gboolean ok = _run(cmd, &out, error);
    g_free(cmd);
    if (ok && out && *out) {
        gchar *uuid = g_strstrip(out);
        gchar *opt = g_strdup_printf(
            "ovn-nbctl dhcp-options-set-options %s lease_time=3600 router=%s server_id=%s server_mac=00:00:00:00:00:01",
            uuid, gw, gw);
        _run(opt, NULL, NULL);
        g_free(opt);
    }
    g_free(out);
    return ok;
}













gboolean
pcv_ovn_router_create(const gchar *name, GError **error)
{
    if (!g_ovn_available) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "OVN not available");
        return FALSE;
    }
    gchar *cmd = g_strdup_printf("ovn-nbctl --may-exist lr-add %s", name);
    gboolean ok = _run(cmd, NULL, error);
    g_free(cmd);
    return ok;
}










gboolean
pcv_ovn_router_delete(const gchar *name, GError **error)
{
    if (!g_ovn_available) return TRUE;
    gchar *cmd = g_strdup_printf("ovn-nbctl --if-exists lr-del %s", name);
    gboolean ok = _run(cmd, NULL, error);
    g_free(cmd);
    return ok;
}




















gboolean
pcv_ovn_router_add_port(const gchar *router, const gchar *sw,
                         const gchar *mac, const gchar *cidr, GError **error)
{
    if (!g_ovn_available) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "OVN not available");
        return FALSE;
    }
    gchar *rport = g_strdup_printf("rtr-%s", sw);
    gchar *cmd1 = g_strdup_printf("ovn-nbctl --may-exist lrp-add %s %s %s %s", router, rport, mac, cidr);
    gboolean ok = _run(cmd1, NULL, error);
    g_free(cmd1);

    if (ok) {
        gchar *lport = g_strdup_printf("lnk-%s", sw);
        gchar *cmd2 = g_strdup_printf("ovn-nbctl --may-exist lsp-add %s %s", sw, lport);
        _run(cmd2, NULL, NULL);
        g_free(cmd2);
        gchar *cmd3 = g_strdup_printf("ovn-nbctl lsp-set-type %s router", lport);
        _run(cmd3, NULL, NULL);
        g_free(cmd3);
        gchar *cmd4 = g_strdup_printf("ovn-nbctl lsp-set-addresses %s router", lport);
        _run(cmd4, NULL, NULL);
        g_free(cmd4);
        gchar *cmd5 = g_strdup_printf("ovn-nbctl lsp-set-options %s router-port=%s", lport, rport);
        _run(cmd5, NULL, NULL);
        g_free(cmd5);
        g_free(lport);
    }
    g_free(rport);
    return ok;
}












gboolean
pcv_ovn_router_remove_port(const gchar *router, const gchar *port, GError **error)
{
    (void)router;
    if (!g_ovn_available) return TRUE;
    gchar *cmd = g_strdup_printf("ovn-nbctl --if-exists lrp-del %s", port);
    gboolean ok = _run(cmd, NULL, error);
    g_free(cmd);
    return ok;
}









JsonArray *
pcv_ovn_router_list(void)
{
    JsonArray *arr = json_array_new();
    if (!g_ovn_available) return arr;

    gchar *out = NULL;
    if (_run("ovn-nbctl lr-list", &out, NULL) && out) {
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
    gchar *cmd = g_strdup_printf("ovn-nbctl lr-nat-add %s %s %s %s",
                                  router, type, external_ip, logical_ip);
    gboolean ok = _run(cmd, NULL, error);
    g_free(cmd);
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
    gchar *cmd = g_strdup_printf("ovn-nbctl lr-nat-del %s %s %s",
                                  router, type, external_ip);
    gboolean ok = _run(cmd, NULL, error);
    g_free(cmd);
    return ok;
}










JsonArray *
pcv_ovn_nat_list(const gchar *router)
{
    JsonArray *arr = json_array_new();
    if (!g_ovn_available || !router) return arr;

    gchar *cmd = g_strdup_printf("ovn-nbctl lr-nat-list %s", router);
    gchar *out = NULL;
    if (_run(cmd, &out, NULL) && out) {
        gchar **lines = g_strsplit(g_strstrip(out), "\n", -1);
        for (gchar **l = lines; *l; l++) {
            if (!**l) continue;
            json_array_add_string_element(arr, *l);
        }
        g_strfreev(lines);
    }
    g_free(cmd);
    g_free(out);
    return arr;
}











JsonArray *
pcv_ovn_dhcp_list(void)
{
    JsonArray *arr = json_array_new();
    if (!g_ovn_available) return arr;

    gchar *out = NULL;
    if (_run("ovn-nbctl dhcp-options-list", &out, NULL) && out) {
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


    gchar *sw_name = g_strdup_printf("tenant-%s-ls", tenant);
    gboolean ok = pcv_ovn_switch_create(sw_name, subnet, error);
    if (!ok) {
        g_free(sw_name);
        return FALSE;
    }


    GError *acl_err = NULL;
    gchar *match_in = g_strdup_printf("inport == @%s && ip", sw_name);
    pcv_ovn_acl_add(sw_name, "to-lport", 1000, match_in, "allow", &acl_err);
    g_free(match_in);
    g_clear_error(&acl_err);

    gchar *match_out = g_strdup_printf("outport == @%s && ip", sw_name);
    pcv_ovn_acl_add(sw_name, "from-lport", 1000, match_out, "allow", &acl_err);
    g_free(match_out);
    g_clear_error(&acl_err);



    gchar **parts = g_strsplit(subnet, "/", 2);
    if (parts[0]) {
        gchar **octets = g_strsplit(parts[0], ".", 4);
        if (octets[0] && octets[1] && octets[2]) {
            gchar *gw = g_strdup_printf("%s.%s.%s.1", octets[0], octets[1], octets[2]);
            pcv_ovn_dhcp_enable(subnet, gw, NULL);
            g_free(gw);
        }
        g_strfreev(octets);
    }
    g_strfreev(parts);

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


    if (mac && ip) {
        gchar *cmd = g_strdup_printf("ovn-nbctl lsp-set-port-security %s \"%s %s\"",
                                      port, mac, ip);
        _run(cmd, NULL, NULL);
        g_free(cmd);
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


    {
        gchar *cmd = g_strdup_printf("ovn-nbctl lsp-list %s", name);
        gchar *out = NULL;
        JsonArray *ports = json_array_new();
        if (_run(cmd, &out, NULL) && out) {
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
        g_free(cmd);
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


    {
        gchar *cmd = g_strdup_printf("ovn-nbctl lrp-list %s", name);
        gchar *out = NULL;
        JsonArray *ports = json_array_new();
        if (_run(cmd, &out, NULL) && out) {
            gchar **lines = g_strsplit(g_strstrip(out), "\n", -1);
            for (gchar **l = lines; *l; l++) {
                if (!**l) continue;

                gchar *lp = strchr(*l, '(');
                gchar *rp = lp ? strchr(lp, ')') : NULL;
                if (lp && rp) {
                    gchar *pname = g_strndup(lp + 1, rp - lp - 1);
                    JsonObject *pobj = json_object_new();
                    json_object_set_string_member(pobj, "name", pname);


                    gchar *mac_cmd = g_strdup_printf("ovn-nbctl get Logical_Router_Port %s mac", pname);
                    gchar *mac_out = NULL;
                    if (_run(mac_cmd, &mac_out, NULL) && mac_out)
                        json_object_set_string_member(pobj, "mac", g_strstrip(mac_out));
                    g_free(mac_cmd);
                    g_free(mac_out);


                    gchar *net_cmd = g_strdup_printf("ovn-nbctl get Logical_Router_Port %s networks", pname);
                    gchar *net_out = NULL;
                    if (_run(net_cmd, &net_out, NULL) && net_out)
                        json_object_set_string_member(pobj, "networks", g_strstrip(net_out));
                    g_free(net_cmd);
                    g_free(net_out);

                    json_array_add_object_element(ports, pobj);
                    g_free(pname);
                }
            }
            g_strfreev(lines);
        }
        g_free(out);
        g_free(cmd);
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





















JsonObject *
pcv_ovn_status(void)
{
    JsonObject *obj = json_object_new();
    json_object_set_boolean_member(obj, "available", g_ovn_available);

    if (g_ovn_available) {
        gchar *out = NULL;
        if (_run_shell("ovn-nbctl --version 2>&1 | head -1", &out, NULL) && out)
            json_object_set_string_member(obj, "version", g_strstrip(out));
        g_free(out);

        JsonArray *switches = pcv_ovn_switch_list();
        json_object_set_int_member(obj, "switch_count", json_array_get_length(switches));
        json_array_unref(switches);

        JsonArray *routers = pcv_ovn_router_list();
        json_object_set_int_member(obj, "router_count", json_array_get_length(routers));
        json_array_unref(routers);
    }
    return obj;
}
