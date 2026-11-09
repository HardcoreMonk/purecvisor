   
                           
                                                                         
  
                           
                                                                                     
                                                                                                   
                                                                                          
                                                                                       
                                                                                                 
                                                                                 
                                                                                              
                                                                                                        
                                                                                              
                                                                                                   
                                                                                
                               
  
                                                                                
                                                        
   
#include "modules/dispatcher/handler_resource.h"

#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>
#include <stdlib.h>
#include <string.h>

#include "modules/audit/pcv_audit.h"
#include "modules/dispatcher/dispatcher_shared.h"
#include "modules/dispatcher/rpc_completion.h"
#include "modules/dispatcher/rpc_utils.h"
#include "modules/network/vpc/vpc_manager.h"                                          
#include "modules/network/vpc/vpc_mgmt_guard.h"                                
#include "modules/resource/resource_domain_refs.h"
#include "modules/resource/resource_gate.h"
#include "modules/resource/resource_registry.h"
#include "modules/virt/virt_conn_pool.h"

                                                   
#define RES_IN_USE_MAX_NAMES 20

                                                                     

                    
static void
_send_error(const gchar *rpc_id, UdsServer *server, GSocketConnection *connection, PureRpcErrorCode code,
            const gchar *message)
{
    gchar *resp = pure_rpc_build_error_response(rpc_id, code, message);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

                                
static void
_send_result(const gchar *rpc_id, UdsServer *server, GSocketConnection *connection, JsonNode *result)
{
    gchar *resp = pure_rpc_build_success_response(rpc_id, result);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

                             
static void
_send_object(const gchar *rpc_id, UdsServer *server, GSocketConnection *connection, JsonObject *obj)
{
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, obj);
    _send_result(rpc_id, server, connection, node);
}

                                                                                        
static PureRpcErrorCode
_res_error_code(const GError *e)
{
    if (!e || e->domain != PCV_RES_ERROR)
        return PURE_RPC_ERR_INTERNAL_ERROR;
    switch (e->code) {
    case PCV_RES_ERROR_INVALID:
        return PURE_RPC_ERR_INVALID_PARAMS;
    case PCV_RES_ERROR_NOT_FOUND:
        return PURE_RPC_ERR_NOT_FOUND;
    case PCV_RES_ERROR_CONFLICT:
        return PURE_RPC_ERR_CONFLICT;
    default:
        return PURE_RPC_ERR_INTERNAL_ERROR;
    }
}

                                                                    
static void
_send_gerror(const gchar *rpc_id, UdsServer *server, GSocketConnection *connection, GError *e)
{
    _send_error(rpc_id, server, connection, _res_error_code(e), e ? e->message : "resource registry error");
    if (e)
        g_error_free(e);
}

                                                        
static const gchar *
_required_string(JsonObject *params, const gchar *key, const gchar *rpc_id, UdsServer *server,
                 GSocketConnection *connection)
{
    const gchar *v = pcv_disp_json_string_member(params, key);
    if (!v || !*v) {
        g_autofree gchar *msg = g_strdup_printf("Missing required parameter: %s (non-empty string)", key);
        _send_error(rpc_id, server, connection, PURE_RPC_ERR_INVALID_PARAMS, msg);
        return NULL;
    }
    return v;
}

                                                                                            
static gboolean
_optional_string(JsonObject *params, const gchar *key, const gchar **out, const gchar *rpc_id, UdsServer *server,
                 GSocketConnection *connection)
{
    *out = NULL;
    JsonNode *n = (params && json_object_has_member(params, key)) ? json_object_get_member(params, key) : NULL;
    if (!n || JSON_NODE_HOLDS_NULL(n))
        return TRUE;
    const gchar *v = pcv_disp_json_string_member(params, key);
    if (!v || !*v) {
        g_autofree gchar *msg = g_strdup_printf("Invalid parameter: %s (non-empty string)", key);
        _send_error(rpc_id, server, connection, PURE_RPC_ERR_INVALID_PARAMS, msg);
        return FALSE;
    }
    *out = v;
    return TRUE;
}

                                                
static gboolean
_parse_kind(const gchar *s, PcvResKind *out, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection)
{
    if (pcv_res_kind_from_string(s, out))
        return TRUE;
    _send_error(rpc_id, server, connection, PURE_RPC_ERR_INVALID_PARAMS,
                "kind must be one of image, iso, pci, usb, storage, bridge");
    return FALSE;
}

                                              
static gboolean
_check_tenant(const gchar *tenant, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection)
{
    if (pcv_res_name_is_valid(tenant))
        return TRUE;
    _send_error(rpc_id, server, connection, PURE_RPC_ERR_INVALID_PARAMS,
                "tenant must match [A-Za-z0-9][A-Za-z0-9._-]{0,62}");
    return FALSE;
}

                                                                                 
static gboolean
_registry_ready(const gchar *rpc_id, UdsServer *server, GSocketConnection *connection)
{
    GError *e = NULL;
    if (pcv_res_registry_open(NULL, &e))
        return TRUE;
    _send_gerror(rpc_id, server, connection, e);
    return FALSE;
}

                                                                      

                                             
typedef struct {
    gchar            *name;
    PcvResDomainRefs  refs;
} ResVmRefs;

static void
_vm_refs_free(gpointer p)
{
    ResVmRefs *v = p;
    if (!v)
        return;
    g_free(v->name);
    pcv_res_domain_refs_clear(&v->refs);
    g_free(v);
}

                                               
static gint
_vm_refs_cmp(gconstpointer a, gconstpointer b)
{
    const ResVmRefs *x = *(ResVmRefs *const *)a;
    const ResVmRefs *y = *(ResVmRefs *const *)b;
    return g_strcmp0(x->name, y->name);
}

                                                                                           
static PcvResDomState
_domain_state(virDomainPtr dom)
{
    int rc = virDomainIsActive(dom);
    return pcv_res_domain_active_state(rc, rc < 0 && virGetLastErrorCode() == VIR_ERR_NO_DOMAIN);
}

                                                                                               
                                                                                               
                                   
static gboolean
_merge_domain_xml(virDomainPtr dom, unsigned int flags, const gchar *name, PcvResDomainRefs *into, gboolean *skipped,
                  GError **error)
{
    char *xml = virDomainGetXMLDesc(dom, flags);
    if (!xml) {
        PcvResDomState st = (virGetLastErrorCode() == VIR_ERR_NO_DOMAIN) ? PCV_RES_DOM_GONE
                            : (flags == 0) ? _domain_state(dom) : PCV_RES_DOM_UNKNOWN;
        if (st == PCV_RES_DOM_GONE || st == PCV_RES_DOM_INACTIVE) {
            *skipped = TRUE;
            return TRUE;
        }
        g_set_error(error, PCV_RES_ERROR, PCV_RES_ERROR_IO, "cannot read the definition of VM '%s'", name);
        return FALSE;
    }
    PcvResDomainRefs one = { 0 };
    gboolean ok = pcv_res_domain_refs_parse(xml, &one, NULL);
    free(xml);
    if (ok)
        pcv_res_domain_refs_merge(into, &one);
    else
        g_set_error(error, PCV_RES_ERROR, PCV_RES_ERROR_IO, "cannot parse the definition of VM '%s'", name);
    pcv_res_domain_refs_clear(&one);
    return ok;
}

                                                                                                   
                                                                                       
                                                                       
static gboolean
_scan_domains(GPtrArray **out_refs_by_vm, GError **error)
{
    *out_refs_by_vm = NULL;
    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        g_set_error_literal(error, PCV_RES_ERROR, PCV_RES_ERROR_IO, "libvirt connection is not available");
        return FALSE;
    }
    virDomainPtr *doms = NULL;
    int n = virConnectListAllDomains(conn, &doms, 0);
    if (n < 0) {
        virt_conn_pool_release(conn);
        g_set_error_literal(error, PCV_RES_ERROR, PCV_RES_ERROR_IO, "cannot list VMs");
        return FALSE;
    }
    GPtrArray *out = g_ptr_array_new_with_free_func(_vm_refs_free);
    gboolean ok = TRUE;
    for (int i = 0; i < n; i++) {
        if (ok) {
            const char *dn = virDomainGetName(doms[i]);
            ResVmRefs *v = g_new0(ResVmRefs, 1);
            v->name = g_strdup(dn ? dn : "");
            gboolean gone = FALSE;
            ok = _merge_domain_xml(doms[i], VIR_DOMAIN_XML_INACTIVE, v->name, &v->refs, &gone, error);
            PcvResDomState st = (ok && !gone) ? _domain_state(doms[i]) : PCV_RES_DOM_INACTIVE;
            if (st == PCV_RES_DOM_GONE) {
                gone = TRUE;
            } else if (st == PCV_RES_DOM_UNKNOWN) {                                             
                g_set_error(error, PCV_RES_ERROR, PCV_RES_ERROR_IO, "cannot determine whether VM '%s' is running", v->name);
                ok = FALSE;
            } else if (st == PCV_RES_DOM_ACTIVE) {
                gboolean stopped = FALSE;                                            
                ok = _merge_domain_xml(doms[i], 0, v->name, &v->refs, &stopped, error);
            }
            if (ok && !gone) {
                pcv_res_domain_refs_canonicalize(&v->refs);
                g_ptr_array_add(out, v);
            } else {
                _vm_refs_free(v);
            }
        }
        virDomainFree(doms[i]);
    }
    free(doms);
    virt_conn_pool_release(conn);
    if (!ok) {
        g_ptr_array_unref(out);
        return FALSE;
    }
    g_ptr_array_sort(out, _vm_refs_cmp);
    *out_refs_by_vm = out;
    return TRUE;
}

                                                                                                      
                                                                                     
                                                                  
static gboolean
_in_use_vms(PcvResKind kind, const gchar *item_ref, const gchar *only_tenant, GPtrArray **vms_out, GError **error)
{
    GPtrArray *scan = NULL;
    *vms_out = NULL;
    if (!_scan_domains(&scan, error))
        return FALSE;
    GPtrArray *vms = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < scan->len; i++) {
        const ResVmRefs *v = g_ptr_array_index(scan, i);
        if (!pcv_res_domain_refs_uses(&v->refs, kind, item_ref))
            continue;
        if (only_tenant) {
                                                                                          
            const gchar *owner_tenant = pcv_res_owner_tenant(v->refs.owner);
            if (!pcv_res_vm_counts_for_revoke(owner_tenant, only_tenant))
                continue;
        }
        g_ptr_array_add(vms, g_strdup(v->name));
    }
    g_ptr_array_unref(scan);
    *vms_out = vms;
    return TRUE;
}

                                                                                            
static gchar *
_in_use_message(const gchar *name, GPtrArray *vms)
{
    GString *s = g_string_new(NULL);
    g_string_printf(s, "resource '%s' is in use by VM(s): ", name);
    for (guint i = 0; i < vms->len && i < RES_IN_USE_MAX_NAMES; i++)
        g_string_append_printf(s, "%s%s", i ? ", " : "", (const gchar *)g_ptr_array_index(vms, i));
    if (vms->len > RES_IN_USE_MAX_NAMES)
        g_string_append(s, ", …");
    return g_string_free(s, FALSE);
}

                                                                                              
                                               
static gboolean
_reject_if_in_use(const gchar *name, const gchar *only_tenant, const gchar *rpc_id, UdsServer *server,
                  GSocketConnection *connection)
{
    PcvResKind kind = PCV_RES_IMAGE;
    gchar *ref = NULL;
    GPtrArray *vms = NULL;
    GError *e = NULL;
    gboolean ok = pcv_res_lookup(name, &kind, &ref, &e) && _in_use_vms(kind, ref, only_tenant, &vms, &e);
    g_free(ref);
    if (!ok) {
        _send_gerror(rpc_id, server, connection, e);
        return FALSE;
    }
    gboolean unused = vms->len == 0;
    if (!unused) {
        g_autofree gchar *msg = _in_use_message(name, vms);
        _send_error(rpc_id, server, connection, PURE_RPC_ERR_CONFLICT, msg);
    }
    g_ptr_array_unref(vms);
    return unused;
}

                                                                           

                                                                                 
                                                                       
void
handle_resource_register(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection)
{
    const gchar *kind_s = _required_string(params, "kind", rpc_id, server, connection);
    const gchar *name = kind_s ? _required_string(params, "name", rpc_id, server, connection) : NULL;
    const gchar *ref = name ? _required_string(params, "ref", rpc_id, server, connection) : NULL;
    PcvResKind kind = PCV_RES_IMAGE;
    if (!ref || !_parse_kind(kind_s, &kind, rpc_id, server, connection) || !_registry_ready(rpc_id, server, connection))
        return;
    GError *e = NULL;
    gboolean refreshed = FALSE;
    gchar *canon = NULL;
    if (!pcv_res_register(kind, name, ref, pcv_rpc_completion_actor(), &refreshed, &e) ||
        !pcv_res_lookup(name, NULL, &canon, &e)) {
        _send_gerror(rpc_id, server, connection, e);
        return;
    }
    JsonObject *o = json_object_new();
    json_object_set_string_member(o, "name", name);
    json_object_set_string_member(o, "kind", pcv_res_kind_to_string(kind));
    json_object_set_string_member(o, "ref", canon);
    json_object_set_boolean_member(o, "refreshed", refreshed);
    g_free(canon);
    _send_object(rpc_id, server, connection, o);
}

                                                                                       
                                        
void
handle_resource_unregister(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = _required_string(params, "name", rpc_id, server, connection);
    if (!name || !_registry_ready(rpc_id, server, connection) ||
        !_reject_if_in_use(name, NULL, rpc_id, server, connection))
        return;
    GError *e = NULL;
    if (!pcv_res_unregister(name, &e)) {
        _send_gerror(rpc_id, server, connection, e);
        return;
    }
    JsonObject *o = json_object_new();
    json_object_set_string_member(o, "name", name);
    _send_object(rpc_id, server, connection, o);
}

                                                                                            
                                                                                   
void
handle_resource_grant(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = _required_string(params, "name", rpc_id, server, connection);
    const gchar *tenant = name ? _required_string(params, "tenant", rpc_id, server, connection) : NULL;
    if (!tenant || !_check_tenant(tenant, rpc_id, server, connection) || !_registry_ready(rpc_id, server, connection))
        return;
    GError *e = NULL;
    if (!pcv_res_grant(name, tenant, &e)) {
        _send_gerror(rpc_id, server, connection, e);
        return;
    }
    g_autofree gchar *target = g_strdup_printf("%s@%s", name, tenant);
    pcv_audit_log(pcv_rpc_completion_actor(), "resource.grant", target, "ok", 0, 0, "local");
    JsonObject *o = json_object_new();
    json_object_set_string_member(o, "name", name);
    json_object_set_string_member(o, "tenant", tenant);
    _send_object(rpc_id, server, connection, o);
}

                                                                                                    
                                                                                  
void
handle_resource_revoke(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = _required_string(params, "name", rpc_id, server, connection);
    const gchar *tenant = name ? _required_string(params, "tenant", rpc_id, server, connection) : NULL;
    if (!tenant || !_check_tenant(tenant, rpc_id, server, connection) || !_registry_ready(rpc_id, server, connection) ||
        !_reject_if_in_use(name, tenant, rpc_id, server, connection))
        return;
    GError *e = NULL;
    if (!pcv_res_revoke(name, tenant, &e)) {
        _send_gerror(rpc_id, server, connection, e);
        return;
    }
    g_autofree gchar *target = g_strdup_printf("%s@%s", name, tenant);
    pcv_audit_log(pcv_rpc_completion_actor(), "resource.revoke", target, "ok", 0, 0, "local");
    JsonObject *o = json_object_new();
    json_object_set_string_member(o, "name", name);
    json_object_set_string_member(o, "tenant", tenant);
    _send_object(rpc_id, server, connection, o);
}

                                                                                     
void
handle_resource_list(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection)
{
    const gchar *kind_s = NULL;
    PcvResKind kind = PCV_RES_IMAGE;
    if (!_optional_string(params, "kind", &kind_s, rpc_id, server, connection) ||
        (kind_s && !_parse_kind(kind_s, &kind, rpc_id, server, connection)) ||
        !_registry_ready(rpc_id, server, connection))
        return;
    GError *e = NULL;
    JsonArray *arr = pcv_res_list(kind_s != NULL, kind, &e);
    if (!arr) {
        _send_gerror(rpc_id, server, connection, e);
        return;
    }
    JsonNode *node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(node, arr);
    _send_result(rpc_id, server, connection, node);
}

                                               
typedef struct {
    PcvResKind  kind;
    gchar      *ref;
    JsonArray  *vms;
} ResUnregItem;

static void
_unreg_item_free(gpointer p)
{
    ResUnregItem *it = p;
    if (!it)
        return;
    g_free(it->ref);
    if (it->vms)
        json_array_unref(it->vms);
    g_free(it);
}

                        
static gint
_unreg_item_cmp(gconstpointer a, gconstpointer b)
{
    const ResUnregItem *x = *(ResUnregItem *const *)a;
    const ResUnregItem *y = *(ResUnregItem *const *)b;
    gint c = g_strcmp0(pcv_res_kind_to_string(x->kind), pcv_res_kind_to_string(y->kind));
    return c ? c : g_strcmp0(x->ref, y->ref);
}

                                                             
static JsonObject *
_unreg_vm_object(const ResVmRefs *v, const gchar *owner_tenant)
{
    JsonObject *o = json_object_new();
    json_object_set_string_member(o, "name", v->name);
    if (v->refs.owner)
        json_object_set_string_member(o, "owner", v->refs.owner);
    else
        json_object_set_null_member(o, "owner");
    if (owner_tenant && *owner_tenant)
        json_object_set_string_member(o, "owner_tenant", owner_tenant);
    else
        json_object_set_null_member(o, "owner_tenant");
    return o;
}

                                                                                           
                                                     
static GPtrArray *
_collect_unregistered(GPtrArray *scan)
{
    GPtrArray *items = g_ptr_array_new_with_free_func(_unreg_item_free);
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (guint i = 0; i < scan->len; i++) {
        const ResVmRefs *v = g_ptr_array_index(scan, i);
        g_autofree gchar *owner_tenant = g_strdup(pcv_res_owner_tenant(v->refs.owner));                     
        for (guint j = 0; v->refs.refs && j < v->refs.refs->len; j++) {
            const PcvResRef *r = g_ptr_array_index(v->refs.refs, j);
            if (r->kind == PCV_RES_IMAGE)                                       
                continue;
            gchar *key = g_strdup_printf("%s\n%s", pcv_res_kind_to_string(r->kind), r->ref);
            gpointer found = NULL;
            if (!g_hash_table_lookup_extended(seen, key, NULL, &found)) {
                ResUnregItem *it = NULL;
                if (!pcv_res_covered(r->kind, r->ref)) {
                    it = g_new0(ResUnregItem, 1);
                    it->kind = r->kind;
                    it->ref = g_strdup(r->ref);
                    it->vms = json_array_new();
                    g_ptr_array_add(items, it);
                }
                g_hash_table_insert(seen, key, it);
                found = it;
            } else {
                g_free(key);
            }
            if (found)
                json_array_add_object_element(((ResUnregItem *)found)->vms, _unreg_vm_object(v, owner_tenant));
        }
    }
    g_hash_table_unref(seen);
    g_ptr_array_sort(items, _unreg_item_cmp);
    return items;
}

                                                                                                       
                                                                                                        
                                                                                 
                                                                             
static gboolean
_append_outside_pool(JsonObject *vpc, GPtrArray *pool, JsonArray *out, GError **error)
{
    const gchar *vpc_id = json_object_get_string_member_with_default(vpc, "id", NULL);
    GError *ve = NULL;
    JsonArray *subnets = vpc_id ? pcv_vpc_subnet_list(vpc_id, NULL, &ve) : NULL;
    if (!subnets) {
        g_set_error(error, PCV_RES_ERROR, PCV_RES_ERROR_IO, "cannot list the subnets of VPC %s: %s",
                    vpc_id ? vpc_id : "(no id)", ve ? ve->message : "unknown");
        g_clear_error(&ve);
        return FALSE;
    }
    for (guint i = 0; i < json_array_get_length(subnets); i++) {
        const gchar *cidr = json_object_get_string_member_with_default(json_array_get_object_element(subnets, i),
                                                                       "cidr", NULL);
        if (!cidr || pcv_vpc_mgmt_pool_covers(pool, cidr))
            continue;
        JsonObject *o = json_object_new();
        json_object_set_string_member(o, "tenant", json_object_get_string_member_with_default(vpc, "tenant", ""));
        json_object_set_string_member(o, "vpc_id", vpc_id);
        json_object_set_string_member(o, "vpc_name", json_object_get_string_member_with_default(vpc, "name", ""));
        json_object_set_string_member(o, "cidr", cidr);
        json_array_add_object_element(out, o);
    }
    json_array_unref(subnets);
    return TRUE;
}

                                                                                     
                                                                             
                                                                                        
                                                                                                 
void
handle_resource_unregistered_list(JsonObject *params, const gchar *rpc_id, UdsServer *server,
                                  GSocketConnection *connection)
{
    const gchar *tenant = NULL;
    if (!_optional_string(params, "tenant", &tenant, rpc_id, server, connection) ||
        (tenant && !_check_tenant(tenant, rpc_id, server, connection)) || !_registry_ready(rpc_id, server, connection))
        return;
    GError *e = NULL;
    GPtrArray *scan = NULL;
    JsonArray *existing = pcv_res_list(FALSE, PCV_RES_IMAGE, &e);
    if (!existing || !_scan_domains(&scan, &e)) {
        if (existing)
            json_array_unref(existing);
        _send_gerror(rpc_id, server, connection, e);
        return;
    }
                                                                                            
                                                                               
    JsonArray *vpc_subnets = json_array_new();
    GHashTable *pools = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_ptr_array_unref);
    GError *ve = NULL;
    JsonArray *vpcs = pcv_vpc_list(NULL, &ve);
    gboolean vpc_ok = vpcs != NULL;
    if (!vpcs)
        g_set_error(&e, PCV_RES_ERROR, PCV_RES_ERROR_IO, "cannot list VPCs: %s", ve ? ve->message : "unknown");
    g_clear_error(&ve);
    for (guint i = 0; vpc_ok && i < json_array_get_length(vpcs); i++) {
        JsonObject *vpc = json_array_get_object_element(vpcs, i);
        const gchar *vt = json_object_get_string_member_with_default(vpc, "tenant", "");
        GPtrArray *pool = g_hash_table_lookup(pools, vt);
        if (!pool && (pool = pcv_res_pool_list(vt)) != NULL)
            g_hash_table_insert(pools, g_strdup(vt), pool);
        if (!pool)
            g_set_error_literal(&e, PCV_RES_ERROR, PCV_RES_ERROR_IO, "vpc pool registry is unavailable");
        vpc_ok = pool && _append_outside_pool(vpc, pool, vpc_subnets, &e);
    }
    if (vpcs)
        json_array_unref(vpcs);
    g_hash_table_unref(pools);
    if (!vpc_ok) {
        json_array_unref(vpc_subnets);
        json_array_unref(existing);
        g_ptr_array_unref(scan);
        _send_gerror(rpc_id, server, connection, e);
        return;
    }
    GHashTable *taken = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (guint i = 0; i < json_array_get_length(existing); i++) {
        JsonObject *row = json_array_get_object_element(existing, i);
        g_hash_table_add(taken, g_strdup(json_object_get_string_member_with_default(row, "name", "")));
    }
    json_array_unref(existing);
    GPtrArray *items = _collect_unregistered(scan);
    g_ptr_array_unref(scan);

    JsonArray *out = json_array_new();
    for (guint i = 0; i < items->len; i++) {
        ResUnregItem *it = g_ptr_array_index(items, i);
        JsonObject *o = json_object_new();
        json_object_set_string_member(o, "kind", pcv_res_kind_to_string(it->kind));
        json_object_set_string_member(o, "ref", it->ref);
        gchar *suggested = pcv_res_unregistered_suggest_name(it->kind, it->ref, taken);
        json_object_set_string_member(o, "suggested_name", suggested);
        g_free(suggested);
        JsonArray *tenants = json_array_new();
        if (tenant)
            json_array_add_string_element(tenants, tenant);
        json_object_set_array_member(o, "tenants", tenants);
        json_object_set_array_member(o, "vms", json_array_ref(it->vms));
        json_array_add_object_element(out, o);
    }
    g_ptr_array_unref(items);
    g_hash_table_unref(taken);
    JsonObject *res = json_object_new();
    json_object_set_array_member(res, "items", out);
    json_object_set_array_member(res, "vpc_subnets", vpc_subnets);
    _send_object(rpc_id, server, connection, res);
}

static void
_import_item_free(gpointer p)
{
    PcvResImportItem *it = p;
    if (!it)
        return;
    g_free(it->name);
    g_free(it->ref);
    if (it->tenants)
        g_ptr_array_unref(it->tenants);
    g_free(it);
}

                                                                                                    
static PcvResImportItem *
_import_item_from_json(JsonNode *node, guint idx, gchar **msg)
{
    JsonObject *o = (node && JSON_NODE_HOLDS_OBJECT(node)) ? json_node_get_object(node) : NULL;
    const gchar *kind_s = o ? pcv_disp_json_string_member(o, "kind") : NULL;
    const gchar *name = o ? pcv_disp_json_string_member(o, "name") : NULL;
    const gchar *ref = o ? pcv_disp_json_string_member(o, "ref") : NULL;
    PcvResKind kind = PCV_RES_IMAGE;
    if (!o || !name || !*name || !ref || !*ref) {
        *msg = g_strdup_printf("item %u: kind, name and ref must be non-empty strings", idx);
        return NULL;
    }
    if (!pcv_res_kind_from_string(kind_s, &kind)) {
        *msg = g_strdup_printf("item %u: kind must be one of image, iso, pci, usb, storage, bridge", idx);
        return NULL;
    }
    JsonNode *tn = json_object_has_member(o, "tenants") ? json_object_get_member(o, "tenants") : NULL;
    GPtrArray *tenants = g_ptr_array_new_with_free_func(g_free);
    if (tn && !JSON_NODE_HOLDS_NULL(tn)) {
        JsonArray *ta = JSON_NODE_HOLDS_ARRAY(tn) ? json_node_get_array(tn) : NULL;
        for (guint t = 0; ta && t < json_array_get_length(ta); t++) {
            JsonNode *e = json_array_get_element(ta, t);
            if (!JSON_NODE_HOLDS_VALUE(e) || json_node_get_value_type(e) != G_TYPE_STRING) {
                ta = NULL;
                break;
            }
            g_ptr_array_add(tenants, g_strdup(json_node_get_string(e)));
        }
        if (!ta) {
            g_ptr_array_unref(tenants);
            *msg = g_strdup_printf("item %u: tenants must be an array of strings", idx);
            return NULL;
        }
    }
    PcvResImportItem *it = g_new0(PcvResImportItem, 1);
    it->kind = kind;
    it->name = g_strdup(name);
    it->ref = g_strdup(ref);
    it->tenants = tenants;
    it->index = idx;                                                           
    return it;
}

                                                                                                       
typedef struct {
    guint  idx;                                               
    gchar *tenant;
    gchar *cidr;
} ResPoolItem;

static void
_pool_item_free(gpointer p)
{
    ResPoolItem *it = p;
    if (!it)
        return;
    g_free(it->tenant);
    g_free(it->cidr);
    g_free(it);
}

                                                                      
static gboolean
_is_pool_item(JsonNode *node)
{
    JsonObject *o = (node && JSON_NODE_HOLDS_OBJECT(node)) ? json_node_get_object(node) : NULL;
    return o && g_strcmp0(pcv_disp_json_string_member(o, "kind"), "vpc_pool") == 0;
}

                                                                                                  
                                                                                          
static ResPoolItem *
_pool_item_from_json(JsonNode *node, guint idx, gchar **msg)
{
    JsonObject *o = json_node_get_object(node);
    const gchar *tenant = pcv_disp_json_string_member(o, "tenant");
    const gchar *cidr = pcv_disp_json_string_member(o, "cidr");
    if (!pcv_res_name_is_valid(tenant)) {
        *msg = g_strdup_printf("item %u: vpc_pool tenant must match [A-Za-z0-9][A-Za-z0-9._-]{0,62}", idx);
        return NULL;
    }
    if (!pcv_res_pool_cidr_is_valid(cidr)) {
        *msg = g_strdup_printf("item %u: vpc_pool cidr must be a canonical private IPv4 network (RFC1918 or 100.64.0.0/10)",
                               idx);
        return NULL;
    }
    ResPoolItem *it = g_new0(ResPoolItem, 1);
    it->idx = idx;
    it->tenant = g_strdup(tenant);
    it->cidr = g_strdup(cidr);
    return it;
}

                                                                                                
                                                                                                 
                                                                                                  
                                                            
                                                                      
static gchar *
_pool_items_precheck(GPtrArray *pools, PureRpcErrorCode *code)
{
    GHashTable *cur = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_ptr_array_unref);
    GHashTable *from = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);                                    
    gchar *msg = NULL;
    for (guint i = 0; !msg && i < pools->len; i++) {
        const ResPoolItem *p = g_ptr_array_index(pools, i);
        GPtrArray *pool = g_hash_table_lookup(cur, p->tenant);
        if (!pool && (pool = pcv_res_pool_list(p->tenant)) != NULL)
            g_hash_table_insert(cur, g_strdup(p->tenant), pool);
        const gchar *hit = pool ? pcv_res_pool_conflict(pool, p->cidr) : NULL;
        if (!pool) {
            *code = PURE_RPC_ERR_INTERNAL_ERROR;
            msg = g_strdup("vpc pool registry is unavailable");
        } else if (hit) {
            g_autofree gchar *key = g_strdup_printf("%s %s", p->tenant, hit);
            guint at = GPOINTER_TO_UINT(g_hash_table_lookup(from, key));
            *code = PURE_RPC_ERR_INVALID_PARAMS;
            msg = at ? g_strdup_printf("item %u: vpc_pool cidr %s overlaps item %u cidr %s", p->idx, p->cidr, at, hit)
                     : g_strdup_printf("item %u: vpc_pool cidr %s overlaps existing pool cidr %s", p->idx, p->cidr, hit);
        } else if (!g_ptr_array_find_with_equal_func(pool, p->cidr, g_str_equal, NULL)) {
            g_ptr_array_add(pool, g_strdup(p->cidr));                                
            g_hash_table_insert(from, g_strdup_printf("%s %s", p->tenant, p->cidr), GUINT_TO_POINTER(p->idx));
        }
    }
    g_hash_table_unref(from);
    g_hash_table_unref(cur);
    return msg;
}

                                                                                                     
                                                                                          
                                                                          
                                                                                                            
                                                                            
                                                          
                                                                  
void
handle_resource_import(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection)
{
    JsonNode *in = (params && json_object_has_member(params, "items")) ? json_object_get_member(params, "items") : NULL;
    JsonArray *arr = (in && JSON_NODE_HOLDS_ARRAY(in)) ? json_node_get_array(in) : NULL;
    if (!arr) {
        _send_error(rpc_id, server, connection, PURE_RPC_ERR_INVALID_PARAMS, "Missing required parameter: items (array)");
        return;
    }
    GPtrArray *items = g_ptr_array_new_with_free_func(_import_item_free);
    GPtrArray *pools = g_ptr_array_new_with_free_func(_pool_item_free);
    for (guint i = 0; i < json_array_get_length(arr); i++) {
        gchar *msg = NULL;
        JsonNode *node = json_array_get_element(arr, i);
        gpointer it = _is_pool_item(node) ? (gpointer)_pool_item_from_json(node, i + 1, &msg)
                                          : (gpointer)_import_item_from_json(node, i + 1, &msg);
        if (!it) {
            _send_error(rpc_id, server, connection, PURE_RPC_ERR_INVALID_PARAMS, msg);
            g_free(msg);
            g_ptr_array_unref(items);
            g_ptr_array_unref(pools);
            return;
        }
        g_ptr_array_add(_is_pool_item(node) ? pools : items, it);
    }
    if (!_registry_ready(rpc_id, server, connection)) {
        g_ptr_array_unref(items);
        g_ptr_array_unref(pools);
        return;
    }
    PureRpcErrorCode code = PURE_RPC_ERR_INVALID_PARAMS;
    gchar *pre = _pool_items_precheck(pools, &code);
    if (pre) {
        _send_error(rpc_id, server, connection, code, pre);
        g_free(pre);
        g_ptr_array_unref(items);
        g_ptr_array_unref(pools);
        return;
    }
    const gchar *actor = pcv_rpc_completion_actor();
    GError *e = NULL;
    guint registered = 0, granted = 0;
    if (!pcv_res_import(items, actor, &registered, &granted, &e)) {
        g_ptr_array_unref(items);
        g_ptr_array_unref(pools);
        _send_gerror(rpc_id, server, connection, e);
        return;
    }
    for (guint i = 0; i < items->len; i++) {                                                    
        const PcvResImportItem *it = g_ptr_array_index(items, i);
        g_autofree gchar *target = g_strdup_printf("%s:%s", pcv_res_kind_to_string(it->kind), it->name);
        pcv_audit_log(actor, "resource.import", target, "ok", 0, 0, "local");
    }
    for (guint i = 0; i < pools->len; i++) {
        const ResPoolItem *p = g_ptr_array_index(pools, i);
        g_autofree gchar *target = g_strdup_printf("vpc_pool:%s/%s", p->tenant, p->cidr);
        if (!pcv_res_pool_add(p->tenant, p->cidr, &e)) {
            g_prefix_error(&e, "item %u: vpc_pool add failed after the resource items and earlier vpc_pool items were imported: ",
                           p->idx);
            pcv_audit_log(actor, "resource.import", target, "fail", _res_error_code(e), 0, "local");
            g_ptr_array_unref(items);
            g_ptr_array_unref(pools);
            _send_gerror(rpc_id, server, connection, e);
            return;
        }
        pcv_audit_log(actor, "resource.import", target, "ok", 0, 0, "local");
    }
    JsonObject *o = json_object_new();
    json_object_set_int_member(o, "registered", registered);
    json_object_set_int_member(o, "granted", granted);
    json_object_set_int_member(o, "pool_items", pools->len);
    g_ptr_array_unref(items);
    g_ptr_array_unref(pools);
    _send_object(rpc_id, server, connection, o);
}
