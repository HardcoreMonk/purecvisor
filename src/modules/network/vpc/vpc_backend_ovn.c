   
                          
                                                                 
  
                       
                                                           
                                                   
  
                           
                                                                   
                                                                                  
                                                                         
                                                                           
  
             
                                                                            
                                                                           
                                                                      
                                                                                     
                                                                    
                                                                        
  
          
                                                                                 
                                                           
                                                     
                                                                            
  
                      
                                                                 
                                                                 
                                                                      
                               
  
                       
                                                 
                                                
                                                          
                                            
                                                                      
  
             
                                                                   
                                                                       
                                                                       
           
  
                                                            
                                                       
                                                                   
                                                          
                                                                   
   
#include "vpc_backend_ovn.h"

#include "vpc_model.h"
#include "modules/network/ovn_manager.h"
#include "modules/network/security_group.h"
#include "utils/pcv_spawn.h"
#include "utils/pcv_validate.h"

#include <string.h>

static gboolean
_run(const gchar * const *argv, GError **error)
{
    g_autofree gchar *stderr_text = NULL;
    g_autoptr(GError) child_error = NULL;
    if (pcv_spawn_sync(argv, NULL, &stderr_text, &child_error))
        return TRUE;
    g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_IO, "%s 실패: %s",
                argv && argv[0] ? argv[0] : "OVN command",
                stderr_text && *stderr_text ? g_strstrip(stderr_text)
                : child_error ? child_error->message : "unknown");
    return FALSE;
}

static gboolean
_run_capture(const gchar * const *argv, gchar **stdout_text, GError **error)
{
    g_autofree gchar *stderr_text = NULL;
    g_autoptr(GError) child_error = NULL;
    if (stdout_text)
        *stdout_text = NULL;
    if (pcv_spawn_sync(argv, stdout_text, &stderr_text, &child_error))
        return TRUE;
    g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_IO, "%s 실패: %s",
                argv && argv[0] ? argv[0] : "OVN command",
                stderr_text && *stderr_text ? g_strstrip(stderr_text)
                : child_error ? child_error->message : "unknown");
    return FALSE;
}

typedef enum {
    PCV_OVN_RESOURCE_ABSENT,
    PCV_OVN_RESOURCE_OWNED,
    PCV_OVN_RESOURCE_FOREIGN,
} PcvOvnResourceOwnership;

static gchar *_owner_arg(const gchar *key, const gchar *value);

static gchar *
_unquote_ovn_value(const gchar *text)
{
    if (!text)
        return g_strdup("");
    g_autofree gchar *copy = g_strdup(text);
    gchar *value = g_strstrip(copy);
    gsize length = strlen(value);
    if (length >= 2 && value[0] == '"' && value[length - 1] == '"') {
        value[length - 1] = '\0';
        return g_strdup(value + 1);
    }
    return g_strdup(value);
}

static PcvOvnResourceOwnership
_resource_ownership(const gchar *table,
                    const gchar *name,
                    const gchar *vpc_id,
                    GError **error)
{
                                                           
                                                                 
    const gchar *uuid_argv[] = { "ovn-nbctl", "--timeout=10", "--if-exists", "get",
                                  table, name, "_uuid", NULL };
    g_autofree gchar *uuid_out = NULL;
    if (!_run_capture(uuid_argv, &uuid_out, error))
        return PCV_OVN_RESOURCE_FOREIGN;
    g_autofree gchar *uuid = _unquote_ovn_value(uuid_out);
    if (!uuid || !*uuid || g_strcmp0(uuid, "[]") == 0)
        return PCV_OVN_RESOURCE_ABSENT;

    const gchar *owner_argv[] = { "ovn-nbctl", "--timeout=10", "--if-exists", "get",
                                   table, name, "external_ids:purecvisor-owner", NULL };
    const gchar *id_argv[] = { "ovn-nbctl", "--timeout=10", "--if-exists", "get",
                                table, name, "external_ids:purecvisor-vpc-id", NULL };
    g_autofree gchar *owner_out = NULL;
    g_autofree gchar *id_out = NULL;
    if (!_run_capture(owner_argv, &owner_out, error) ||
        !_run_capture(id_argv, &id_out, error))
        return PCV_OVN_RESOURCE_FOREIGN;
    g_autofree gchar *owner = _unquote_ovn_value(owner_out);
    g_autofree gchar *actual_id = _unquote_ovn_value(id_out);
    return g_strcmp0(owner, "local-vpc") == 0 && g_strcmp0(actual_id, vpc_id) == 0
        ? PCV_OVN_RESOURCE_OWNED : PCV_OVN_RESOURCE_FOREIGN;
}

static gboolean
_require_absent_or_owned(const gchar *table,
                         const gchar *name,
                         const gchar *vpc_id,
                         GError **error)
{
    g_autoptr(GError) inspect_error = NULL;
    PcvOvnResourceOwnership ownership = _resource_ownership(
        table, name, vpc_id, &inspect_error);
    if (inspect_error) {
        g_propagate_error(error, g_steal_pointer(&inspect_error));
        return FALSE;
    }
    if (ownership != PCV_OVN_RESOURCE_FOREIGN)
        return TRUE;
    g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_CONFLICT,
                "Local VPC 소유가 아닌 OVN %s '%s'와 이름이 충돌합니다",
                table, name);
    return FALSE;
}

static gboolean
_ensure_port_group(const gchar *name,
                   const gchar *vpc_id,
                   gint64 generation,
                   GError **error)
{
    g_autoptr(GError) inspect_error = NULL;
    PcvOvnResourceOwnership ownership = _resource_ownership(
        "Port_Group", name, vpc_id, &inspect_error);
    if (inspect_error) {
        g_propagate_error(error, g_steal_pointer(&inspect_error));
        return FALSE;
    }
    if (ownership == PCV_OVN_RESOURCE_FOREIGN) {
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_CONFLICT,
                    "Local VPC 소유가 아닌 OVN Port_Group '%s'와 이름이 충돌합니다", name);
        return FALSE;
    }
    if (ownership == PCV_OVN_RESOURCE_OWNED)
        return TRUE;

    g_autofree gchar *owner = _owner_arg("purecvisor-owner", "local-vpc");
    g_autofree gchar *owner_id = _owner_arg("purecvisor-vpc-id", vpc_id);
    g_autofree gchar *gen = g_strdup_printf(
        "external_ids:purecvisor-generation=%" G_GINT64_FORMAT, generation);
                                                                
                                                                  
    const gchar *argv[] = { "ovn-nbctl", "--timeout=10", "--wait=hv",
        "--", "pg-add", name,
        "--", "set", "Port_Group", name, owner, owner_id, gen, NULL };
    return _run(argv, error);
}

static gboolean
_require_ovs_iface_absent_or_owned(const gchar *name,
                                   const gchar *vpc_id,
                                   GError **error)
{
    const gchar *uuid_argv[] = { "ovs-vsctl", "--timeout=10", "--if-exists", "get",
                                  "Interface", name, "_uuid", NULL };
    g_autofree gchar *uuid_out = NULL;
    if (!_run_capture(uuid_argv, &uuid_out, error))
        return FALSE;
    g_autofree gchar *uuid = _unquote_ovn_value(uuid_out);
    if (!uuid || !*uuid || g_strcmp0(uuid, "[]") == 0)
        return TRUE;

    const gchar *owner_argv[] = { "ovs-vsctl", "--timeout=10", "--if-exists", "get",
                                   "Interface", name, "external_ids:purecvisor-owner", NULL };
    const gchar *id_argv[] = { "ovs-vsctl", "--timeout=10", "--if-exists", "get",
                                "Interface", name, "external_ids:purecvisor-vpc-id", NULL };
    g_autofree gchar *owner_out = NULL;
    g_autofree gchar *id_out = NULL;
    if (!_run_capture(owner_argv, &owner_out, error) ||
        !_run_capture(id_argv, &id_out, error))
        return FALSE;
    g_autofree gchar *owner = _unquote_ovn_value(owner_out);
    g_autofree gchar *actual_id = _unquote_ovn_value(id_out);
    if (g_strcmp0(owner, "local-vpc") == 0 && g_strcmp0(actual_id, vpc_id) == 0)
        return TRUE;
    g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_CONFLICT,
                "Local VPC 소유가 아닌 OVS Interface '%s'와 이름이 충돌합니다", name);
    return FALSE;
}

static gboolean
_line_uses_managed_edge(const gchar *line)
{
    g_auto(GStrv) tokens = g_strsplit_set(line ? line : "", " \t\r\n", -1);
    for (guint i = 0; tokens[i]; i++) {
        gchar *token = tokens[i];
        if (!g_str_has_prefix(token, "pcve"))
            continue;
        gchar *at = strchr(token, '@');
        gsize length = at ? (gsize)(at - token) : strlen(token);
        if (length != 15)
            continue;
        gboolean suffix_ok = TRUE;
        for (gsize j = 4; j < length; j++)
            if (!g_ascii_isxdigit(token[j]) || g_ascii_isupper(token[j])) {
                suffix_ok = FALSE;
                break;
            }
        if (!suffix_ok)
            continue;
        g_autofree gchar *iface = g_strndup(token, length);
        const gchar *argv[] = { "ovs-vsctl", "--timeout=10", "--if-exists", "get",
            "Interface", iface, "external_ids:purecvisor-owner", NULL };
        g_autofree gchar *owner_out = NULL;
        if (pcv_spawn_sync(argv, &owner_out, NULL, NULL)) {
            g_autofree gchar *owner = _unquote_ovn_value(owner_out);
            if (g_strcmp0(owner, "local-vpc") == 0)
                return TRUE;
        }
    }
    return FALSE;
}

static gboolean
_host_pool_line_overlaps(const gchar *line, const PcvVpcIpv4Cidr *pool)
{
                                                               
                                                                        
    if (!line || _line_uses_managed_edge(line))
        return FALSE;
    g_auto(GStrv) tokens = g_strsplit_set(line, " \t\r\n", -1);
    for (guint i = 0; tokens[i]; i++) {
        if (!strchr(tokens[i], '/') || g_strcmp0(tokens[i], "0.0.0.0/0") == 0)
            continue;
        PcvVpcIpv4Cidr actual = {0};
        if (pcv_vpc_cidr_parse(tokens[i], &actual, NULL, NULL) &&
            pcv_vpc_cidr_overlaps(&actual, pool))
            return TRUE;
    }
    return FALSE;
}

static gboolean
_host_pool_is_free(const PcvVpcIpv4Cidr *pool)
{
    const gchar *addr_argv[] = { "ip", "-4", "-o", "addr", "show", NULL };
    const gchar *route_argv[] = { "ip", "-4", "route", "show", "table", "all", NULL };
    g_autofree gchar *addresses = NULL;
    g_autofree gchar *routes = NULL;
    if (!pcv_spawn_sync(addr_argv, &addresses, NULL, NULL) ||
        !pcv_spawn_sync(route_argv, &routes, NULL, NULL))
        return FALSE;
    g_auto(GStrv) address_lines = g_strsplit(addresses ? addresses : "", "\n", -1);
    g_auto(GStrv) route_lines = g_strsplit(routes ? routes : "", "\n", -1);
    for (guint i = 0; address_lines[i]; i++)
        if (_host_pool_line_overlaps(address_lines[i], pool))
            return FALSE;
    for (guint i = 0; route_lines[i]; i++)
        if (_host_pool_line_overlaps(route_lines[i], pool))
            return FALSE;
    return TRUE;
}

static gchar *
_gateway_cidr(JsonObject *subnet)
{
    const gchar *cidr = json_object_get_string_member(subnet, "cidr");
    const gchar *slash = cidr ? strrchr(cidr, '/') : NULL;
    return slash ? g_strdup_printf("%s%s",
        json_object_get_string_member(subnet, "gateway"), slash) : NULL;
}

static gchar *
_owner_arg(const gchar *key, const gchar *value)
{
    return g_strdup_printf("external_ids:%s=%s", key, value);
}

JsonObject *
pcv_vpc_ovn_backend_capability(const gchar *transit_pool)
{
                               
                                                                        
                                                                                   
                                                                     
      
                           
                                                      
                                                         
    g_autoptr(JsonObject) status = pcv_ovn_status();
    const gchar *br_argv[] = { "ovs-vsctl", "br-exists", "br-int", NULL };
    gboolean control = json_object_get_boolean_member_with_default(status, "available", FALSE);
    gboolean br_int = control && pcv_spawn_sync(br_argv, NULL, NULL, NULL);
    PcvVpcIpv4Cidr pool = {0};
    g_autofree gchar *canonical = NULL;
    gboolean pool_ok = pcv_vpc_cidr_parse(transit_pool, &pool, &canonical, NULL) &&
        g_strcmp0(canonical, transit_pool) == 0 && pool.prefix >= 16 && pool.prefix <= 28;
    PcvVpcIpv4Cidr shared = {0};
    gboolean rfc6598 = pool_ok &&
        pcv_vpc_cidr_parse("100.64.0.0/10", &shared, NULL, NULL) &&
        pool.network >= shared.network && pool.broadcast <= shared.broadcast;
    gboolean host_pool_free = rfc6598 && _host_pool_is_free(&pool);

    JsonObject *result = json_object_new();
    json_object_set_string_member(result, "id", "ovn");
    json_object_set_string_member(result, "label", "OVN (Open vSwitch)");
    json_object_set_boolean_member(result, "ready",
        control && br_int && pool_ok && rfc6598 && host_pool_free);
    json_object_set_boolean_member(result, "installed",
        json_object_get_boolean_member_with_default(status, "installed", FALSE));
    JsonObject *checks = json_object_new();
    json_object_set_boolean_member(checks, "northbound",
        json_object_get_boolean_member_with_default(status, "northbound_connected", FALSE));
    json_object_set_boolean_member(checks, "southbound",
        json_object_get_boolean_member_with_default(status, "southbound_connected", FALSE));
    json_object_set_boolean_member(checks, "northd_synced",
        json_object_get_boolean_member_with_default(status, "northd_synced", FALSE));
    json_object_set_boolean_member(checks, "controller",
        json_object_get_boolean_member_with_default(status, "controller_configured", FALSE));
    json_object_set_boolean_member(checks, "chassis",
        json_object_get_boolean_member_with_default(status, "chassis_registered", FALSE));
    json_object_set_boolean_member(checks, "control_plane", control);
    json_object_set_boolean_member(checks, "br_int", br_int);
    json_object_set_boolean_member(checks, "transit_pool", pool_ok && rfc6598);
    json_object_set_boolean_member(checks, "host_pool_free", host_pool_free);
                                                                      
                                                            
    json_object_set_boolean_member(checks, "feature_parity", TRUE);
    json_object_set_object_member(result, "checks", checks);
    json_object_set_string_member(result, "transit_pool", transit_pool);
    if (pool_ok && rfc6598)
        json_object_set_int_member(result, "allocatable_edge_links",
            ((guint64)pool.broadcast - pool.network + 1) / 4);
    else
        json_object_set_int_member(result, "allocatable_edge_links", 0);
    if (!(control && br_int && pool_ok && rfc6598 && host_pool_free))
        json_object_set_string_member(result, "reason",
            !control ? "OVN NB/SB/controller/chassis가 준비되지 않았습니다"
            : !br_int ? "OVS integration bridge br-int가 없습니다"
            : !(pool_ok && rfc6598) ? "OVN edge transit pool은 canonical RFC6598 /16~/28이어야 합니다"
            : "OVN edge transit pool이 host 주소 또는 route와 충돌합니다");
    return result;
}

gboolean
pcv_vpc_ovn_require_ready(const gchar *transit_pool, GError **error)
{
    g_autoptr(JsonObject) capability = pcv_vpc_ovn_backend_capability(transit_pool);
    if (json_object_get_boolean_member(capability, "ready"))
        return TRUE;
    g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_STATE, "OVN backend 사용 불가: %s",
        json_object_get_string_member_with_default(capability, "reason", "unknown"));
    return FALSE;
}

gboolean
pcv_vpc_ovn_set_quarantine(JsonObject *vpc, gboolean enabled, GError **error)
{
                               
                                                                                
                                                                 
                          
      
                                                       
    g_return_val_if_fail(vpc != NULL, FALSE);
    const gchar *vpc_id = json_object_get_string_member(vpc, "id");
    g_autofree gchar *digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, vpc_id, -1);
    g_autofree gchar *pg = g_strdup_printf("pcvv_pg_%.*s", 16, digest);
    const gchar *from_match = "inport == @PLACEHOLDER && (ip || arp)";
    const gchar *to_match = "outport == @PLACEHOLDER && (ip || arp)";
    g_autofree gchar *from = g_strdup(from_match);
    g_autofree gchar *to = g_strdup(to_match);
    gchar *marker = strstr(from, "PLACEHOLDER");
    if (marker) {
        g_autofree gchar *prefix = g_strndup(from, (gsize)(marker - from));
        g_autofree gchar *suffix = g_strdup(marker + strlen("PLACEHOLDER"));
        g_free(g_steal_pointer(&from));
        from = g_strdup_printf("%s%s%s", prefix, pg, suffix);
    }
    marker = strstr(to, "PLACEHOLDER");
    if (marker) {
        g_autofree gchar *prefix = g_strndup(to, (gsize)(marker - to));
        g_autofree gchar *suffix = g_strdup(marker + strlen("PLACEHOLDER"));
        g_free(g_steal_pointer(&to));
        to = g_strdup_printf("%s%s%s", prefix, pg, suffix);
    }
    const gchar *from_add[] = { "ovn-nbctl", "--timeout=10", "--wait=hv", "--may-exist",
        "--type=port-group", "acl-add", pg, "from-lport", "30000", from, "drop", NULL };
    const gchar *to_add[] = { "ovn-nbctl", "--timeout=10", "--wait=hv", "--may-exist",
        "--type=port-group", "acl-add", pg, "to-lport", "30000", to, "drop", NULL };
    const gchar *from_del[] = { "ovn-nbctl", "--timeout=10", "--wait=hv",
        "--type=port-group", "acl-del", pg, "from-lport", "30000", from, NULL };
    const gchar *to_del[] = { "ovn-nbctl", "--timeout=10", "--wait=hv",
        "--type=port-group", "acl-del", pg, "to-lport", "30000", to, NULL };
    return _run(enabled ? from_add : from_del, error) &&
           _run(enabled ? to_add : to_del, error);
}

static void
_argv_add(GPtrArray *args, const gchar *value)
{
    g_ptr_array_add(args, g_strdup(value));
}

static void
_append_pg_acl(GPtrArray *args,
               const gchar *pg,
               const gchar *direction,
               gint priority,
               const gchar *match,
               const gchar *action)
{
    g_autofree gchar *priority_text = g_strdup_printf("%d", priority);
    _argv_add(args, "--");
    _argv_add(args, "--may-exist");
    _argv_add(args, "--type=port-group");
    _argv_add(args, "acl-add");
    _argv_add(args, pg);
    _argv_add(args, direction);
    _argv_add(args, priority_text);
    _argv_add(args, match);
    _argv_add(args, action);
}

static gchar *
_security_group_match(const gchar *lsp, JsonObject *rule)
{
    const gchar *direction = json_object_get_string_member(rule, "direction");
    const gchar *protocol = json_object_get_string_member(rule, "protocol");
    const gchar *source = json_object_get_string_member(rule, "source");
    gint port_start = (gint)json_object_get_int_member(rule, "port_start");
    gint port_end = (gint)json_object_get_int_member(rule, "port_end");
    gboolean egress = g_strcmp0(direction, "egress") == 0;
    GString *match = g_string_new(NULL);
    g_string_append_printf(match, "%s == \"%s\" && ip4",
        egress ? "inport" : "outport", lsp);
    if (source && g_strcmp0(source, "0.0.0.0/0") != 0)
        g_string_append_printf(match, " && ip4.%s == %s",
            egress ? "dst" : "src", source);
    if (g_strcmp0(protocol, "icmp") == 0) {
        g_string_append(match, " && icmp4");
    } else {
        g_string_append_printf(match, " && %s", protocol);
        if (port_start > 0 && port_end > port_start)
            g_string_append_printf(match, " && %s.dst >= %d && %s.dst <= %d",
                protocol, port_start, protocol, port_end);
        else if (port_start > 0)
            g_string_append_printf(match, " && %s.dst == %d", protocol, port_start);
    }
    return g_string_free(match, FALSE);
}

gboolean
pcv_vpc_ovn_sync_policy(JsonObject *vpc,
                        JsonArray *attachments,
                        gboolean quarantine,
                        GError **error)
{
                               
                                                                             
                                                                                  
                                                                     
      
                                                                   
    g_return_val_if_fail(vpc && attachments, FALSE);
    const gchar *vpc_id = json_object_get_string_member(vpc, "id");
    g_autofree gchar *digest = g_compute_checksum_for_string(
        G_CHECKSUM_SHA256, vpc_id, -1);
    g_autofree gchar *pg = g_strdup_printf("pcvv_pg_%.*s", 16, digest);
    if (!_require_absent_or_owned("Port_Group", pg, vpc_id, error))
        return FALSE;

                                                                
                                                               
    g_autoptr(GPtrArray) args = g_ptr_array_new_with_free_func(g_free);
    _argv_add(args, "ovn-nbctl");
    _argv_add(args, "--timeout=10");
    _argv_add(args, "--wait=hv");
    _argv_add(args, "--");
    _argv_add(args, "--type=port-group");
    _argv_add(args, "acl-del");
    _argv_add(args, pg);

                                                             
                                                                        
                                              
                                                          
    g_autofree gchar *from_arp = g_strdup_printf("inport == @%s && arp", pg);
    g_autofree gchar *to_arp = g_strdup_printf("outport == @%s && arp", pg);
    g_autofree gchar *from_dhcp = g_strdup_printf(
        "inport == @%s && udp && udp.src == 68 && udp.dst == 67", pg);
    g_autofree gchar *to_dhcp = g_strdup_printf(
        "outport == @%s && udp && udp.src == 67 && udp.dst == 68", pg);
    g_autofree gchar *from_established = g_strdup_printf(
        "inport == @%s && ip && (ct.est || ct.rel)", pg);
    g_autofree gchar *to_established = g_strdup_printf(
        "outport == @%s && ip && (ct.est || ct.rel)", pg);
    _append_pg_acl(args, pg, "from-lport", 2200, from_arp, "allow-stateless");
    _append_pg_acl(args, pg, "to-lport", 2200, to_arp, "allow-stateless");
    _append_pg_acl(args, pg, "from-lport", 2150, from_dhcp, "allow-stateless");
    _append_pg_acl(args, pg, "to-lport", 2150, to_dhcp, "allow-stateless");
    _append_pg_acl(args, pg, "from-lport", 2100,
        from_established, "allow-related");
    _append_pg_acl(args, pg, "to-lport", 2100,
        to_established, "allow-related");

    for (guint i = 0; i < json_array_get_length(attachments); i++) {
        JsonObject *attachment = json_array_get_object_element(attachments, i);
        if (g_strcmp0(json_object_get_string_member_with_default(
                attachment, "state", ""), "ACTIVE") != 0)
            continue;
        g_autofree gchar *lsp = pcv_vpc_ovn_port_name_from_id(
            json_object_get_string_member(attachment, "id"));
        g_autoptr(JsonObject) policy = pcv_security_group_policy_for_vm(
            json_object_get_string_member(attachment, "vm_name"));
        gboolean bound = json_object_get_boolean_member(policy, "bound");
        gboolean egress_enforced = json_object_get_boolean_member(
            policy, "egress_enforced");
        JsonArray *rules = json_object_get_array_member(policy, "rules");

        if (!bound) {
            g_autofree gchar *from = g_strdup_printf(
                "inport == \"%s\" && ip", lsp);
            g_autofree gchar *to = g_strdup_printf(
                "outport == \"%s\" && ip", lsp);
            _append_pg_acl(args, pg, "from-lport", 1000, from, "allow-related");
            _append_pg_acl(args, pg, "to-lport", 1000, to, "allow-related");
            continue;
        }
        for (guint j = 0; rules && j < json_array_get_length(rules); j++) {
            JsonObject *rule = json_array_get_object_element(rules, j);
            const gchar *direction = json_object_get_string_member(rule, "direction");
            g_autofree gchar *match = _security_group_match(lsp, rule);
            _append_pg_acl(args, pg,
                g_strcmp0(direction, "egress") == 0 ? "from-lport" : "to-lport",
                1000, match, "allow-related");
        }
        if (!egress_enforced) {
            g_autofree gchar *from = g_strdup_printf(
                "inport == \"%s\" && ip", lsp);
            _append_pg_acl(args, pg, "from-lport", 900, from, "allow-related");
        }
    }

    g_autofree gchar *from_default_drop = g_strdup_printf(
        "inport == @%s && (ip || arp)", pg);
    g_autofree gchar *to_default_drop = g_strdup_printf(
        "outport == @%s && (ip || arp)", pg);
    _append_pg_acl(args, pg, "from-lport", 100, from_default_drop, "drop");
    _append_pg_acl(args, pg, "to-lport", 100, to_default_drop, "drop");
    if (quarantine) {
        g_autofree gchar *from = g_strdup_printf("inport == @%s && (ip || arp)", pg);
        g_autofree gchar *to = g_strdup_printf("outport == @%s && (ip || arp)", pg);
        _append_pg_acl(args, pg, "from-lport", 30000, from, "drop");
        _append_pg_acl(args, pg, "to-lport", 30000, to, "drop");
    }
    g_ptr_array_add(args, NULL);
    return _run((const gchar * const *)args->pdata, error);
}

gboolean
pcv_vpc_ovn_ensure_vpc(JsonObject *vpc, JsonObject *binding, GError **error)
{
                               
                                                                           
                                                                             
                                                              
      
                                                                 
    g_return_val_if_fail(vpc && binding, FALSE);
    const gchar *id = json_object_get_string_member(vpc, "id");
    const gchar *lr = json_object_get_string_member(binding, "backend_ref");
    const gchar *host_ip = json_object_get_string_member(binding, "host_edge_ip");
    const gchar *router_ip = json_object_get_string_member(binding, "router_edge_ip");
    const gchar *edge_cidr = json_object_get_string_member(binding, "edge_cidr");
    gint64 generation = json_object_get_int_member(vpc, "revision");
    g_autofree gchar *edge = pcv_vpc_ovn_edge_switch_name_from_id(id);
    g_autofree gchar *iface = pcv_vpc_ovn_edge_iface_name_from_id(id);
    g_autofree gchar *host_lsp = g_strdup_printf("%s_host", edge);
    g_autofree gchar *lrp = g_strdup_printf("%s_lrp", edge);
    g_autofree gchar *link = g_strdup_printf("%s_lnk", edge);
    g_autofree gchar *digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, id, -1);
    g_autofree gchar *pg = g_strdup_printf("pcvv_pg_%.*s", 16, digest);
    g_autofree gchar *router_seed = g_strdup_printf("edge-router:%s", id);
    g_autofree gchar *host_seed = g_strdup_printf("edge-host:%s", id);
    g_autofree gchar *router_mac = pcv_vpc_mac_from_id(router_seed);
    g_autofree gchar *host_mac = pcv_vpc_mac_from_id(host_seed);
    const gchar *slash = strrchr(edge_cidr, '/');
    g_autofree gchar *router_cidr = g_strdup_printf("%s%s", router_ip, slash);
    g_autofree gchar *host_cidr = g_strdup_printf("%s%s", host_ip, slash);
    g_autofree gchar *router_opt = g_strdup_printf("router-port=%s", lrp);
    g_autofree gchar *host_addr = g_strdup_printf("%s %s", host_mac, host_ip);
    g_autofree gchar *owner = _owner_arg("purecvisor-owner", "local-vpc");
    g_autofree gchar *owner_id = _owner_arg("purecvisor-vpc-id", id);
    g_autofree gchar *gen = g_strdup_printf("external_ids:purecvisor-generation=%" G_GINT64_FORMAT,
                                             generation);
    if (!_require_absent_or_owned("Logical_Router", lr, id, error) ||
        !_require_absent_or_owned("Logical_Switch", edge, id, error) ||
        !_require_absent_or_owned("Logical_Switch_Port", host_lsp, id, error) ||
        !_require_absent_or_owned("Logical_Switch_Port", link, id, error) ||
        !_require_absent_or_owned("Logical_Router_Port", lrp, id, error) ||
        !_require_ovs_iface_absent_or_owned(iface, id, error) ||
        !_ensure_port_group(pg, id, generation, error))
        return FALSE;
    const gchar *nb[] = {
        "ovn-nbctl", "--timeout=10", "--wait=hv",
        "--", "--may-exist", "lr-add", lr,
        "--", "set", "Logical_Router", lr, owner, owner_id, gen,
        "--", "--may-exist", "ls-add", edge,
        "--", "set", "Logical_Switch", edge, owner, owner_id, gen,
        "--", "--may-exist", "lrp-add", lr, lrp, router_mac, router_cidr,
        "--", "set", "Logical_Router_Port", lrp, owner, owner_id, gen,
        "--", "--may-exist", "lsp-add", edge, link,
        "--", "lsp-set-type", link, "router",
        "--", "lsp-set-addresses", link, "router",
        "--", "lsp-set-options", link, router_opt,
        "--", "set", "Logical_Switch_Port", link, owner, owner_id, gen,
        "--", "--may-exist", "lsp-add", edge, host_lsp,
        "--", "lsp-set-addresses", host_lsp, host_addr,
                                                                         
                                                                         
                                       
        "--", "lsp-set-port-security", host_lsp, host_mac,
        "--", "set", "Logical_Switch_Port", host_lsp, owner, owner_id, gen,
        "--", "set", "Port_Group", pg, owner, owner_id, gen,
        NULL
    };
    if (!_run(nb, error)) return FALSE;

    g_autofree gchar *iface_id = g_strdup_printf("external_ids:iface-id=%s", host_lsp);
    const gchar *ovs[] = { "ovs-vsctl", "--timeout=10", "--may-exist", "add-port", "br-int", iface,
        "--", "set", "Interface", iface, "type=internal", iface_id, owner, owner_id, NULL };
    const gchar *link_mac[] = { "ip", "link", "set", "dev", iface, "address", host_mac, NULL };
    const gchar *addr[] = { "ip", "addr", "replace", host_cidr, "dev", iface, NULL };
    const gchar *up[] = { "ip", "link", "set", "dev", iface, "up", NULL };
    if (!_run(ovs, error) || !_run(link_mac, error) || !_run(addr, error) || !_run(up, error))
        return FALSE;
    return pcv_vpc_ovn_set_egress(vpc, binding, error) &&
           pcv_vpc_ovn_set_quarantine(vpc, TRUE, error);
}

gboolean
pcv_vpc_ovn_set_egress(JsonObject *vpc, JsonObject *binding, GError **error)
{
    const gchar *lr = json_object_get_string_member(binding, "backend_ref");
    const gchar *host_ip = json_object_get_string_member(binding, "host_edge_ip");
    const gchar *mode = json_object_get_string_member(vpc, "egress_mode");
    const gchar *add[] = { "ovn-nbctl", "--timeout=10", "--wait=hv", "--may-exist",
        "lr-route-add", lr, "0.0.0.0/0", host_ip, NULL };
    const gchar *del[] = { "ovn-nbctl", "--timeout=10", "--wait=hv", "--if-exists",
        "lr-route-del", lr, "0.0.0.0/0", NULL };
    return _run(g_strcmp0(mode, "nat") == 0 ? add : del, error);
}

gboolean
pcv_vpc_ovn_ensure_subnet(JsonObject *vpc, JsonObject *binding,
                          JsonObject *subnet, GError **error)
{
                               
                                                                                                
                                                                                 
      
                                                              
    const gchar *vpc_id = json_object_get_string_member(vpc, "id");
    const gchar *subnet_id = json_object_get_string_member(subnet, "id");
    const gchar *lr = json_object_get_string_member(binding, "backend_ref");
    const gchar *sw = json_object_get_string_member(subnet, "backend_ref");
    const gchar *cidr = json_object_get_string_member(subnet, "cidr");
    const gchar *gateway = json_object_get_string_member(subnet, "gateway");
    const gchar *router_edge_ip = json_object_get_string_member(binding, "router_edge_ip");
    g_autofree gchar *gateway_cidr = _gateway_cidr(subnet);
    g_autofree gchar *mac = pcv_vpc_mac_from_id(subnet_id);
    g_autofree gchar *rport = g_strdup_printf("rtr-%s", sw);
    g_autofree gchar *lport = g_strdup_printf("lnk-%s", sw);
    g_autofree gchar *ropt = g_strdup_printf("router-port=%s", rport);
    g_autofree gchar *owner = _owner_arg("purecvisor-owner", "local-vpc");
    g_autofree gchar *owner_vpc = _owner_arg("purecvisor-vpc-id", vpc_id);
    g_autofree gchar *owner_subnet = _owner_arg("purecvisor-subnet-id", subnet_id);
    g_autofree gchar *gen = g_strdup_printf("external_ids:purecvisor-generation=%" G_GINT64_FORMAT,
        json_object_get_int_member(vpc, "revision"));
    if (!_require_absent_or_owned("Logical_Switch", sw, vpc_id, error) ||
        !_require_absent_or_owned("Logical_Router_Port", rport, vpc_id, error) ||
        !_require_absent_or_owned("Logical_Switch_Port", lport, vpc_id, error))
        return FALSE;
    const gchar *nb[] = {
        "ovn-nbctl", "--timeout=10", "--wait=hv",
        "--", "--may-exist", "ls-add", sw,
        "--", "set", "Logical_Switch", sw, owner, owner_vpc, owner_subnet, gen,
        "--", "--may-exist", "lrp-add", lr, rport, mac, gateway_cidr,
        "--", "set", "Logical_Router_Port", rport, owner, owner_vpc, owner_subnet, gen,
        "--", "--may-exist", "lsp-add", sw, lport,
        "--", "lsp-set-type", lport, "router",
        "--", "lsp-set-addresses", lport, "router",
        "--", "lsp-set-options", lport, ropt,
        "--", "set", "Logical_Switch_Port", lport, owner, owner_vpc, owner_subnet, gen,
        NULL
    };
    if (!_run(nb, error) || !pcv_ovn_dhcp_enable_for_switch(sw, cidr, gateway, error))
        return FALSE;
    g_autofree gchar *dhcp_filter = g_strdup_printf("external_ids:logical_switch=%s", sw);
    const gchar *dhcp_find[] = { "ovn-nbctl", "--timeout=10", "--data=bare",
        "--no-heading", "--columns=_uuid", "find", "DHCP_Options", dhcp_filter, NULL };
    g_autofree gchar *dhcp_rows = NULL;
    if (!_run_capture(dhcp_find, &dhcp_rows, error))
        return FALSE;
    g_auto(GStrv) dhcp_uuids = g_strsplit(g_strstrip(dhcp_rows ? dhcp_rows : ""), "\n", -1);
    for (guint i = 0; dhcp_uuids[i]; i++) {
        if (!*dhcp_uuids[i])
            continue;
        const gchar *tag_dhcp[] = { "ovn-nbctl", "--timeout=10", "set", "DHCP_Options",
            dhcp_uuids[i], owner, owner_vpc, owner_subnet, gen, NULL };
        if (!_run(tag_dhcp, error))
            return FALSE;
    }
    g_autofree gchar *iface = pcv_vpc_ovn_edge_iface_name_from_id(vpc_id);
    const gchar *route[] = { "ip", "route", "replace", cidr, "via", router_edge_ip,
                             "dev", iface, NULL };
    return _run(route, error);
}

gboolean
pcv_vpc_ovn_ensure_attachment(JsonObject *vpc, JsonObject *subnet,
                              JsonObject *attachment, GError **error)
{
                               
                                                                                       
                                                                                    
                                                            
      
                                                          
    const gchar *vpc_id = json_object_get_string_member(vpc, "id");
    const gchar *subnet_id = json_object_get_string_member(subnet, "id");
    const gchar *attachment_id = json_object_get_string_member(attachment, "id");
    const gchar *sw = json_object_get_string_member(subnet, "backend_ref");
    g_autofree gchar *lsp = pcv_vpc_ovn_port_name_from_id(attachment_id);
    if (!_require_absent_or_owned("Logical_Switch_Port", lsp, vpc_id, error))
        return FALSE;
    if (!pcv_ovn_port_add(sw, lsp,
            json_object_get_string_member(attachment, "mac_address"),
            json_object_get_string_member(attachment, "ip_address"), error))
        return FALSE;
    g_autofree gchar *digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, vpc_id, -1);
    g_autofree gchar *pg = g_strdup_printf("pcvv_pg_%.*s", 16, digest);
    g_autofree gchar *owner = _owner_arg("purecvisor-owner", "local-vpc");
    g_autofree gchar *owner_vpc = _owner_arg("purecvisor-vpc-id", vpc_id);
    g_autofree gchar *owner_subnet = _owner_arg("purecvisor-subnet-id", subnet_id);
    g_autofree gchar *owner_attachment = _owner_arg("purecvisor-attachment-id", attachment_id);
                                                                  
                                                         
                     
    const gchar *tag[] = { "ovn-nbctl", "--timeout=10", "--wait=hv",
        "--", "set", "Logical_Switch_Port", lsp,
        owner, owner_vpc, owner_subnet, owner_attachment,
        "--", "--id=@lsp", "get", "Logical_Switch_Port", lsp, "_uuid",
        "--", "add", "Port_Group", pg, "ports", "@lsp", NULL };
    return _run(tag, error);
}

gboolean
pcv_vpc_ovn_remove_attachment(JsonObject *vpc,
                              JsonObject *subnet G_GNUC_UNUSED,
                              JsonObject *attachment, GError **error)
{
    const gchar *vpc_id = json_object_get_string_member(vpc, "id");
    g_autofree gchar *lsp = pcv_vpc_ovn_port_name_from_id(
        json_object_get_string_member(attachment, "id"));
    if (!_require_absent_or_owned("Logical_Switch_Port", lsp, vpc_id, error))
        return FALSE;
    const gchar *del[] = { "ovn-nbctl", "--timeout=10", "--wait=hv",
                           "--if-exists", "lsp-del", lsp, NULL };
    return _run(del, error);
}

gboolean
pcv_vpc_ovn_remove_subnet(JsonObject *vpc, JsonObject *binding,
                          JsonObject *subnet, GError **error)
{
                               
                                                                                 
                                                                          
      
                                                                
    const gchar *sw = json_object_get_string_member(subnet, "backend_ref");
    const gchar *cidr = json_object_get_string_member(subnet, "cidr");
    const gchar *vpc_id = json_object_get_string_member(vpc, "id");
    if (!_require_absent_or_owned("Logical_Switch", sw, vpc_id, error))
        return FALSE;
    g_autofree gchar *iface = pcv_vpc_ovn_edge_iface_name_from_id(
        json_object_get_string_member(vpc, "id"));
    const gchar *route_show[] = { "ip", "-4", "route", "show", "exact", cidr, NULL };
    g_autofree gchar *route_out = NULL;
    if (!_run_capture(route_show, &route_out, error))
        return FALSE;
    const gchar *route[] = { "ip", "route", "del", cidr, "via",
        json_object_get_string_member(binding, "router_edge_ip"), "dev", iface, NULL };
                                                                 
                                                                 
                            
    if (route_out && *g_strstrip(route_out) && !_run(route, error))
        return FALSE;
    g_autofree gchar *dhcp_vpc = g_strdup_printf("external_ids:purecvisor-vpc-id=%s", vpc_id);
    g_autofree gchar *dhcp_subnet = g_strdup_printf("external_ids:purecvisor-subnet-id=%s",
        json_object_get_string_member(subnet, "id"));
    const gchar *find[] = { "ovn-nbctl", "--timeout=10", "--data=bare", "--no-heading",
        "--columns=_uuid", "find", "DHCP_Options", dhcp_vpc, dhcp_subnet, NULL };
    g_autofree gchar *rows = NULL;
    if (!_run_capture(find, &rows, error))
        return FALSE;
    g_autoptr(GPtrArray) args = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(args, g_strdup("ovn-nbctl"));
    g_ptr_array_add(args, g_strdup("--timeout=10"));
    g_ptr_array_add(args, g_strdup("--wait=hv"));
    g_auto(GStrv) uuids = g_strsplit(g_strstrip(rows ? rows : ""), "\n", -1);
    for (guint i = 0; uuids[i]; i++) {
        if (!*uuids[i])
            continue;
        g_ptr_array_add(args, g_strdup("--"));
        g_ptr_array_add(args, g_strdup("destroy"));
        g_ptr_array_add(args, g_strdup("DHCP_Options"));
        g_ptr_array_add(args, g_strdup(uuids[i]));
    }
    g_ptr_array_add(args, g_strdup("--"));
    g_ptr_array_add(args, g_strdup("--if-exists"));
    g_ptr_array_add(args, g_strdup("ls-del"));
    g_ptr_array_add(args, g_strdup(sw));
    g_ptr_array_add(args, NULL);
    return _run((const gchar * const *)args->pdata, error);
}

gboolean
pcv_vpc_ovn_remove_vpc(JsonObject *vpc, JsonObject *binding, GError **error)
{
                               
                                                                                
                                                                    
      
                                                                
    const gchar *id = json_object_get_string_member(vpc, "id");
    g_autofree gchar *iface = pcv_vpc_ovn_edge_iface_name_from_id(id);
    g_autofree gchar *edge = pcv_vpc_ovn_edge_switch_name_from_id(id);
    g_autofree gchar *digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, id, -1);
    g_autofree gchar *pg = g_strdup_printf("pcvv_pg_%.*s", 16, digest);
    const gchar *lr = json_object_get_string_member(binding, "backend_ref");
    if (!_require_absent_or_owned("Logical_Switch", edge, id, error) ||
        !_require_absent_or_owned("Logical_Router", lr, id, error) ||
        !_require_absent_or_owned("Port_Group", pg, id, error) ||
        !_require_ovs_iface_absent_or_owned(iface, id, error))
        return FALSE;
    const gchar *ovs[] = { "ovs-vsctl", "--timeout=10", "--if-exists", "del-port",
                           "br-int", iface, NULL };
    const gchar *nb[] = { "ovn-nbctl", "--timeout=10", "--wait=hv",
        "--", "--if-exists", "ls-del", edge,
        "--", "--if-exists", "destroy", "Port_Group", pg,
        "--", "--if-exists", "lr-del", lr,
        NULL };
    return _run(ovs, error) && _run(nb, error);
}
