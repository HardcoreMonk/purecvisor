                                                                             
                                       
                                                                                 
  
                           
                                                                                                  
                                                                                                                                                 
                                                                                       
                                                                                         
                                                                   
                                                                                           
                                                                                 
                                                                                                  
                                             
                                                                                         
                                 
  
                                                                             
                                                           
                                                                                
#include "resource_gate.h"

#include "resource_registry.h"

#include <string.h>

#include "api/grpc_server.h"
#include "api/uds_server.h"
#include "modules/audit/pcv_audit.h"
#include "modules/auth/pcv_rbac.h"
#include "modules/dispatcher/dispatcher_shared.h"
#include "modules/dispatcher/rpc_completion.h"
#include "modules/dispatcher/rpc_utils.h"
#include "utils/pcv_config.h"

                                                                                      
static gboolean
_injected_tenant(JsonObject *params, const gchar **out)
{
    *out = NULL;
    if (!params || !json_object_has_member(params, "_pcv_caller_tenant"))
        return FALSE;
    JsonNode *n = json_object_get_member(params, "_pcv_caller_tenant");
    if (n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING)
        *out = json_node_get_string(n);
    return TRUE;
}

                                                                      
void
pcv_res_caller_init(PcvResCaller *c, JsonObject *params, GSocketConnection *connection)
{
    if (!c)
        return;
    c->role = pcv_disp_dispatcher_caller_role(params, connection);
    const gchar *sub = pcv_disp_dispatcher_caller_subject(params, connection);
    c->subject = (sub && *sub) ? g_strdup(sub) : NULL;
    c->tenant = NULL;
    if (c->role >= PCV_ROLE_ADMIN)
        return;                                                

    const gchar *t = NULL;
    if (!_injected_tenant(params, &t))
        t = pcv_rbac_get_tenant(c->subject);                                                  
    if (t && *t && pcv_res_name_is_valid(t))
        c->tenant = g_strdup(t);
}

                                                                                            
                                                                                      
const gchar *
pcv_res_owner_tenant(const gchar *owner)
{
    if (!owner || !*owner)
        return NULL;
    if (g_strcmp0(owner, PCV_GRPC_CALLER_SUBJECT) == 0) {
                                                                             
        const gchar *t = pcv_config_get_string("grpc", "tenant", NULL);
        return (t && pcv_res_name_is_valid(t)) ? t : NULL;
    }
    return pcv_rbac_get_tenant(owner);                                  
}

                                                                       
void
pcv_res_caller_clear(PcvResCaller *c)
{
    if (!c)
        return;
    g_clear_pointer(&c->subject, g_free);
    g_clear_pointer(&c->tenant, g_free);
    c->role = PCV_ROLE_VIEWER;
}

                                                               
PcvResDecision
pcv_res_decide(gint role, gboolean has_tenant, gboolean normalized, gboolean allowed)
{
    if (role >= PCV_ROLE_ADMIN)
        return PCV_RES_ALLOW;
    if (!has_tenant)
        return PCV_RES_DENY_NO_TENANT;
    return (normalized && allowed) ? PCV_RES_ALLOW : PCV_RES_DENY_NOT_ALLOWED;
}

                                                                                     
PcvResDecision
pcv_res_check(const PcvResCaller *c, PcvResKind kind, const gchar *value, gchar **effective_out, PcvResPin *pin_out)
{
    if (effective_out)
        *effective_out = NULL;
    if (pin_out)
        memset(pin_out, 0, sizeof *pin_out);
    if (!c)
        return PCV_RES_DENY_NO_TENANT;
    if (c->role >= PCV_ROLE_ADMIN) {
        if (effective_out)
            *effective_out = g_strdup(value);
        return PCV_RES_ALLOW;                                              
    }
    if (!c->tenant)
        return pcv_res_decide(c->role, FALSE, FALSE, FALSE);

    PcvResNorm norm = { 0 };
    g_autoptr(GError) error = NULL;
    if (!value || !pcv_res_normalize(kind, value, &norm, &error))
        return pcv_res_decide(c->role, TRUE, FALSE, FALSE);                                 

    PcvResDecision d = pcv_res_decide(c->role, TRUE, TRUE, pcv_res_tenant_allows(kind, &norm, c->tenant));
    if (d == PCV_RES_ALLOW) {
        if (pin_out && norm.has_identity && (kind == PCV_RES_IMAGE || kind == PCV_RES_ISO)) {
            pin_out->dev = norm.dev;
            pin_out->ino = norm.ino;
            pin_out->gen = norm.gen;
        }
        if (effective_out) {
            *effective_out = norm.ref;                 
            norm.ref = NULL;
        }
    }
    pcv_res_norm_clear(&norm);
    return d;
}

                                                          
PcvResDecision
pcv_res_check_tenant_name(const PcvResCaller *c, const gchar *requested)
{
    if (!requested || !*requested)
        return PCV_RES_ALLOW;
    if (!c)
        return PCV_RES_DENY_NO_TENANT;
    if (c->role >= PCV_ROLE_ADMIN)
        return PCV_RES_ALLOW;
    if (!c->tenant)
        return PCV_RES_DENY_NO_TENANT;
    return g_strcmp0(c->tenant, requested) == 0 ? PCV_RES_ALLOW : PCV_RES_DENY_NOT_ALLOWED;
}

                                                           
gchar *
pcv_res_decision_message(PcvResDecision d, const gchar *param_name)
{
    switch (d) {
    case PCV_RES_DENY_NO_TENANT:
        return g_strdup("Permission denied: this account has no tenant — host resource references are disabled until an "
                        "admin assigns one");
    case PCV_RES_DENY_NOT_ALLOWED:
        return g_strdup_printf("Permission denied: %s is not a resource allowed for this tenant",
                               param_name ? param_name : "value");
    case PCV_RES_ALLOW:
    default:
        return NULL;
    }
}

                                                                                         
void
pcv_res_audit_denied(const gchar *method, const gchar *param_name, const gchar *value)
{
    g_autofree gchar *valid = g_utf8_make_valid(value ? value : "", -1);
    glong chars = g_utf8_strlen(valid, -1);                                                 
    g_autofree gchar *head = g_utf8_substring(valid, 0, MIN(chars, 200));
    g_autofree gchar *target = g_strdup_printf("%s=%s", param_name ? param_name : "value", head);
    const gchar *actor = pcv_rpc_completion_actor();
    pcv_audit_log(actor ? actor : PCV_AUDIT_ACTOR_LOCAL, method ? method : "", target, "denied", PURE_RPC_ERR_FORBIDDEN,
                  0, "resource");
}

                                                                                                           
                                                                                        
gboolean
pcv_res_gate(JsonObject *params, GSocketConnection *connection, const gchar *method, const gchar *param_name,
             PcvResKind kind, const gchar *value, gchar **effective_out, PcvResPin *pin_out, const gchar *rpc_id,
             UdsServer *server)
{
    if (effective_out)
        *effective_out = NULL;
    if (pin_out)
        memset(pin_out, 0, sizeof *pin_out);
    if (!value || !*value)
        return TRUE;

    PcvResCaller c = { 0 };
    pcv_res_caller_init(&c, params, connection);
    PcvResDecision d = pcv_res_check(&c, kind, value, effective_out, pin_out);
    pcv_res_caller_clear(&c);
    if (d == PCV_RES_ALLOW)
        return TRUE;

    g_autofree gchar *msg = pcv_res_decision_message(d, param_name);
    g_autofree gchar *response = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_FORBIDDEN, msg);
    pure_uds_server_send_response(server, connection, response);
    pcv_res_audit_denied(method, param_name, value);
    return FALSE;
}

                                                 
gboolean
pcv_res_visible(const PcvResCaller *c, PcvResKind kind, const gchar *value)
{
    if (c && c->role >= PCV_ROLE_ADMIN)
        return TRUE;
    return pcv_res_check(c, kind, value, NULL, NULL) == PCV_RES_ALLOW;
}

                                                                
JsonArray *
pcv_res_filter_array(const PcvResCaller *c, PcvResKind kind, JsonArray *array, const gchar *member)
{
    if (!array || (c && c->role >= PCV_ROLE_ADMIN))
        return array;
    JsonArray *out = json_array_new();
    guint n = json_array_get_length(array);
    for (guint i = 0; i < n; i++) {
        JsonNode *el = json_array_get_element(array, i);
        if (!el || !JSON_NODE_HOLDS_OBJECT(el))
            continue;
        JsonObject *o = json_node_get_object(el);
        if (!member || !json_object_has_member(o, member))
            continue;
        JsonNode *v = json_object_get_member(o, member);
        if (!v || !JSON_NODE_HOLDS_VALUE(v) || json_node_get_value_type(v) != G_TYPE_STRING)
            continue;
        if (pcv_res_visible(c, kind, json_node_get_string(v)))
            json_array_add_element(out, json_node_copy(el));
    }
    json_array_unref(array);
    return out;
}

                                                                                         
JsonObject *
pcv_res_template_view(const PcvResCaller *c, JsonObject *template_obj)
{
    if (!template_obj || (c && c->role >= PCV_ROLE_ADMIN))
        return template_obj;
    static const struct { const gchar *member; PcvResKind kind; } refs[] = {
        { "iso_path", PCV_RES_ISO }, { "network_bridge", PCV_RES_BRIDGE },
    };
    gboolean show = c && c->tenant;                                          
    for (guint i = 0; show && i < G_N_ELEMENTS(refs); i++) {
        if (!json_object_has_member(template_obj, refs[i].member))
            continue;
        JsonNode *v = json_object_get_member(template_obj, refs[i].member);
        const gchar *s = v && JSON_NODE_HOLDS_VALUE(v) && json_node_get_value_type(v) == G_TYPE_STRING
                             ? json_node_get_string(v) : NULL;
        show = s && pcv_res_visible(c, refs[i].kind, s);                                
    }
    if (!show) {
        json_object_unref(template_obj);
        return NULL;
    }
    json_object_remove_member(template_obj, "iso_path");                         
    return template_obj;
}

                                                                                
gboolean
pcv_res_request_can_see_usb(JsonObject *params, GSocketConnection *connection, const gchar *vendor,
                            const gchar *product)
{
    g_autofree gchar *ref = g_strdup_printf("%s:%s", vendor ? vendor : "", product ? product : "");
    PcvResCaller c = { 0 };
    pcv_res_caller_init(&c, params, connection);
    gboolean ok = pcv_res_visible(&c, PCV_RES_USB, ref);
    pcv_res_caller_clear(&c);
    return ok;
}
