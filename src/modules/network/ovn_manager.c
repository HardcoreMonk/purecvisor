
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
        PCV_LOG_WARN(OVN_LOG_DOM, "ovn-nbctl failed: %s → %s", argv[0], std_err);
    g_free(std_err);
    return ok;
}

void pcv_ovn_init(void)
{

    const gchar *argv[] = {"ovn-nbctl", "--version", NULL};
    gchar *out = NULL, *errout = NULL;
    g_ovn_available = pcv_spawn_sync(argv, &out, &errout, NULL);
    g_free(out);
    g_free(errout);
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
    if (!_valid_ovn_id(sw) || !_valid_ovn_id(port)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "invalid switch or port name");
        return FALSE;
    }
    const gchar *argv1[] = {"ovn-nbctl", "--may-exist", "lsp-add", sw, port, NULL};
    gboolean ok = _run_argv(argv1, NULL, error);
    if (!ok) return FALSE;

    if (mac && ip) {
        if (!pcv_validate_mac(mac) || !pcv_validate_ip_literal(ip)) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "invalid mac or ip");
            return FALSE;
        }

        gchar *addr = g_strdup_printf("%s %s", mac, ip);
        const gchar *argv2[] = {"ovn-nbctl", "lsp-set-addresses", port, addr, NULL};
        ok = _run_argv(argv2, NULL, error);
        g_free(addr);
    }
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
pcv_ovn_dhcp_enable(const gchar *subnet, const gchar *gw, GError **error)
{
    if (!g_ovn_available) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "OVN not available");
        return FALSE;
    }
    if (!pcv_validate_cidr(subnet) || !pcv_validate_ip_literal(gw)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "invalid subnet or gateway");
        return FALSE;
    }
    const gchar *argv[] = {"ovn-nbctl", "dhcp-options-create", subnet, NULL};
    gchar *out = NULL;
    gboolean ok = _run_argv(argv, &out, error);
    if (ok && out && *out) {
        gchar *uuid = g_strstrip(out);
        gchar *router_opt   = g_strdup_printf("router=%s", gw);
        gchar *serverid_opt = g_strdup_printf("server_id=%s", gw);
        const gchar *argv2[] = {"ovn-nbctl", "dhcp-options-set-options", uuid,
                                "lease_time=3600", router_opt, serverid_opt,
                                "server_mac=00:00:00:00:00:01", NULL};
        _run_argv(argv2, NULL, NULL);
        g_free(router_opt);
        g_free(serverid_opt);
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
    const gchar *argv1[] = {"ovn-nbctl", "--may-exist", "lrp-add", router, rport, mac, cidr, NULL};
    gboolean ok = _run_argv(argv1, NULL, error);

    if (ok) {
        gchar *lport = g_strdup_printf("lnk-%s", sw);
        const gchar *argv2[] = {"ovn-nbctl", "--may-exist", "lsp-add", sw, lport, NULL};
        _run_argv(argv2, NULL, NULL);
        const gchar *argv3[] = {"ovn-nbctl", "lsp-set-type", lport, "router", NULL};
        _run_argv(argv3, NULL, NULL);
        const gchar *argv4[] = {"ovn-nbctl", "lsp-set-addresses", lport, "router", NULL};
        _run_argv(argv4, NULL, NULL);
        gchar *ropt = g_strdup_printf("router-port=%s", rport);
        const gchar *argv5[] = {"ovn-nbctl", "lsp-set-options", lport, ropt, NULL};
        _run_argv(argv5, NULL, NULL);
        g_free(ropt);
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

    if (mac && ip && pcv_validate_mac(mac) && pcv_validate_ip_literal(ip)) {
        gchar *secval = g_strdup_printf("%s %s", mac, ip);
        const gchar *argv[] = {"ovn-nbctl", "lsp-set-port-security", port, secval, NULL};
        _run_argv(argv, NULL, NULL);
        g_free(secval);
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

JsonObject *
pcv_ovn_status(void)
{
    JsonObject *obj = json_object_new();
    json_object_set_boolean_member(obj, "available", g_ovn_available);

    if (g_ovn_available) {
        const gchar *va[] = {"ovn-nbctl", "--version", NULL};
        gchar *out = NULL;
        if (_run_argv(va, &out, NULL) && out) {
            gchar *nl = strchr(out, '\n');
            if (nl) *nl = '\0';
            json_object_set_string_member(obj, "version", g_strstrip(out));
        }
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
