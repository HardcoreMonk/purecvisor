                                                                             
                                        
                                                                                                    
  
                           
                                                                                                             
                                                                                  
                                                                                        
  
                       
                                                         
                                                                                
#include "modules/resource/vm_read_filter.h"

#include <string.h>

#include "modules/daemons/cgroup_psi.h"                                     
#include "modules/auth/pcv_rbac.h"
#include "modules/resource/resource_gate.h"
#include "modules/security/security_store.h"                     
#include "modules/virt/vm_owner.h"
#include "utils/pcv_validate.h"                                   

                                            
static const gchar *
_str_member(JsonObject *o, const gchar *key)
{
    JsonNode *n = o ? json_object_get_member(o, key) : NULL;
    return (n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING) ? json_node_get_string(n) : NULL;
}

                                                                         
static gboolean
_proc_row_is_vm(JsonObject *row)
{
    return g_strcmp0(_str_member(row, "type"), "vm") == 0;
}

                                                                     
static gboolean
_kind_is_vm(const gchar *kind)
{
    return g_strcmp0(kind, "vm") == 0;
}

                                                                 
static gboolean
_alert_is_security(JsonObject *row)
{
    return g_strcmp0(_str_member(row, "metric"), "Security") == 0;
}

                                                
static gboolean
_is_star(const gchar *v)
{
    return g_strcmp0(v, "*") == 0;
}

                                                                            
gboolean
pcv_vm_read_xml_visible(PcvVmReadScope *s, const gchar *domain_xml, gchar **owner_out)
{
    if (owner_out)
        *owner_out = NULL;
    if (!s)
        return FALSE;
    if (pcv_vm_read_scope_all(s) && !owner_out)
        return TRUE;                                            
    gchar *owner = domain_xml ? pcv_vm_owner_from_domain_xml(domain_xml) : NULL;
    if (pcv_vm_read_scope_all(s)) {
        if (owner_out)
            *owner_out = owner;                         
        else
            g_free(owner);
        return TRUE;
    }
    gboolean visible = pcv_vm_read_visible(s, owner);                                
    g_free(owner);
    return visible;
}

                                                                  
void
pcv_vm_read_view_init(PcvVmReadView *v, JsonObject *params, GSocketConnection *connection)
{
    PcvResCaller rc = { 0 };
    pcv_res_caller_init(&rc, params, connection);
    v->scope = pcv_vm_read_scope_new(&rc);
    pcv_res_caller_clear(&rc);
    v->map = pcv_vm_read_scope_all(v->scope) ? NULL : pcv_vm_owner_map_current(g_get_monotonic_time());                      
}

                       
void
pcv_vm_read_view_clear(PcvVmReadView *v)
{
    if (!v)
        return;
    pcv_vm_owner_map_free(v->map);
    pcv_vm_read_scope_free(v->scope);
    v->map = NULL;
    v->scope = NULL;
}

                                                                               
                                                                                  
static gint
_proc_row_domain_id(JsonObject *row)
{
    static const gchar SLICE[] = "/machine.slice/";
    const gchar *cg = _str_member(row, "cgroup");
    const gchar *p = cg ? strstr(cg, SLICE) : NULL;
    if (!p)
        return -1;
    p += sizeof(SLICE) - 1;
    const gchar *end = strchr(p, '/');
    gchar *dir = end ? g_strndup(p, (gsize)(end - p)) : g_strdup(p);
    gint id = pcv_cgroup_psi_domain_id_from_scope(dir);
    g_free(dir);
    return id;
}

                                                                  
void
pcv_vm_read_filter_processes(PcvVmReadScope *s, const PcvVmOwnerMap *m, JsonArray *rows, gint top_n)
{
    if (!rows)
        return;
    if (!pcv_vm_read_scope_all(s)) {
        for (guint i = json_array_get_length(rows); i-- > 0;) {
            JsonObject *row = json_array_get_object_element(rows, i);
            if (row && _proc_row_is_vm(row) && !pcv_vm_read_visible_domain_id(s, m, _proc_row_domain_id(row)))
                json_array_remove_element(rows, i);
        }
    }
    if (top_n > 0)
        while (json_array_get_length(rows) > (guint)top_n)
            json_array_remove_element(rows, json_array_get_length(rows) - 1);
}

                                                                                                 
void
pcv_vm_read_mask_numa(PcvVmReadScope *s, const PcvVmOwnerMap *m, JsonObject *numa)
{
    JsonNode *cn = numa ? json_object_get_member(numa, "cores") : NULL;
    JsonArray *cores = (cn && JSON_NODE_HOLDS_ARRAY(cn)) ? json_node_get_array(cn) : NULL;
    for (guint i = 0; cores && i < json_array_get_length(cores); i++) {
        JsonObject *c = json_array_get_object_element(cores, i);
        if (!c)
            continue;
        const gchar *vm = _str_member(c, "owner");
        const gboolean visible = !vm || pcv_vm_read_visible_name(s, m, vm);                  
        if (!visible)
            json_object_set_null_member(c, "owner");
        json_object_set_boolean_member(c, "owner_visible", visible);
    }
}

                                                        
JsonArray *
pcv_vm_read_filter_by_member(PcvVmReadScope *s, const PcvVmOwnerMap *m, JsonArray *take, const gchar *member,
                             gboolean keep_star)
{
    if (!take || pcv_vm_read_scope_all(s))
        return take;
    JsonArray *out = json_array_new();
    for (guint i = 0; i < json_array_get_length(take); i++) {
        JsonNode *n = json_array_get_element(take, i);
        JsonObject *o = (n && JSON_NODE_HOLDS_OBJECT(n)) ? json_node_get_object(n) : NULL;
        const gchar *vm = o ? _str_member(o, member) : NULL;
        if (vm && ((keep_star && _is_star(vm)) || pcv_vm_read_visible_name(s, m, vm)))
            json_array_add_element(out, json_node_copy(n));
    }
    json_array_unref(take);
    return out;
}

                                                                          
void
pcv_vm_read_filter_sg_vms(PcvVmReadScope *s, const PcvVmOwnerMap *m, JsonArray *groups)
{
    if (!groups || pcv_vm_read_scope_all(s))
        return;
    for (guint i = 0; i < json_array_get_length(groups); i++) {
        JsonObject *g = json_array_get_object_element(groups, i);
        JsonNode *vn = g ? json_object_get_member(g, "vms") : NULL;
        JsonArray *vms = (vn && JSON_NODE_HOLDS_ARRAY(vn)) ? json_node_get_array(vn) : NULL;
        for (guint k = vms ? json_array_get_length(vms) : 0; k-- > 0;) {
            JsonNode *e = json_array_get_element(vms, k);
            const gchar *vm = (e && JSON_NODE_HOLDS_VALUE(e) && json_node_get_value_type(e) == G_TYPE_STRING)
                              ? json_node_get_string(e) : NULL;
            if (!pcv_vm_read_visible_name(s, m, vm))
                json_array_remove_element(vms, k);
        }
    }
}

                                                         
JsonArray *
pcv_vm_read_filter_healing_history(PcvVmReadScope *s, const PcvVmOwnerMap *m, JsonArray *take)
{
                                                                                                                 
                                                                                  
    if (!take || pcv_vm_read_scope_all(s))
        return take;
    JsonArray *out = json_array_new();
    for (guint i = 0; i < json_array_get_length(take); i++) {
        JsonNode *n = json_array_get_element(take, i);
        const gchar *t = (n && JSON_NODE_HOLDS_OBJECT(n)) ? _str_member(json_node_get_object(n), "target") : NULL;
        gboolean visible = FALSE;
        if (t && pcv_validate_is_uuid_like(t)) {
            g_autofree gchar *u = g_ascii_strdown(t, -1);
            visible = pcv_vm_read_visible_uuid(s, m, g_strstrip(u));
        } else if (t) {
            visible = pcv_vm_read_visible_name(s, m, t);
        }
        if (visible)
            json_array_add_element(out, json_node_copy(n));
    }
    json_array_unref(take);
    return out;
}

                                                                   
gboolean
pcv_vm_read_security_row_visible(PcvVmReadScope *s, const PcvVmOwnerMap *m, JsonObject *row)
{
    if (pcv_vm_read_scope_all(s))
        return TRUE;
    if (!s || !row)
        return FALSE;
    if (!_kind_is_vm(_str_member(row, "target_kind")))
        return TRUE;                                    
    return pcv_vm_read_visible_name(s, m, _str_member(row, "target"));
}

                       
JsonArray *
pcv_vm_read_filter_security_rows(PcvVmReadScope *s, const PcvVmOwnerMap *m, JsonArray *take)
{
    if (!take || pcv_vm_read_scope_all(s))
        return take;
    JsonArray *out = json_array_new();
    for (guint i = 0; i < json_array_get_length(take); i++) {
        JsonNode *n = json_array_get_element(take, i);
        if (n && JSON_NODE_HOLDS_OBJECT(n) && pcv_vm_read_security_row_visible(s, m, json_node_get_object(n)))
            json_array_add_element(out, json_node_copy(n));
    }
    json_array_unref(take);
    return out;
}

                                                                    
JsonArray *
pcv_vm_read_collect_security_rows(PcvVmReadScope *s, const PcvVmOwnerMap *m, PcvVmReadRowSource src, gpointer ud,
                                  gint offset, gint limit)
{
    if (pcv_vm_read_scope_all(s))
        return src(offset, limit, ud);
    offset = MAX(offset, 0);
    limit = MAX(limit, 0);
    const gint want_total = offset + limit;
    JsonArray *kept = json_array_new();
    gint read = 0;
    while (read < PCV_VM_READ_SOURCE_CAP && (gint)json_array_get_length(kept) < want_total) {
        const gint want = MIN(PCV_VM_READ_BATCH, PCV_VM_READ_SOURCE_CAP - read);
        JsonArray *batch = src(read, want, ud);
        const gint got = batch ? (gint)json_array_get_length(batch) : 0;
        read += got;
        batch = pcv_vm_read_filter_security_rows(s, m, batch);
        for (guint i = 0; batch && i < json_array_get_length(batch); i++)
            json_array_add_element(kept, json_node_copy(json_array_get_element(batch, i)));
        if (batch)
            json_array_unref(batch);
        if (got < want)
            break;             
    }
    JsonArray *out = json_array_new();
    for (gint i = offset; i < want_total && i < (gint)json_array_get_length(kept); i++)
        json_array_add_element(out, json_node_copy(json_array_get_element(kept, (guint)i)));
    json_array_unref(kept);
    return out;
}

                                                     
static gboolean
_store_event_target(const gchar *event_id, gchar **kind_out, gchar **target_out, gpointer ud)
{
    (void)ud;
    if (!pcv_security_store_ensure_open())
        return FALSE;
    JsonObject *ev = pcv_security_store_get_event(event_id);
    if (!ev)
        return FALSE;
    *kind_out = g_strdup(_str_member(ev, "target_kind"));
    *target_out = g_strdup(_str_member(ev, "target"));
    json_object_unref(ev);
    return TRUE;
}

                                                                                                     
                                      
static void
_parse_security_message(const gchar *msg, gchar **sev_out, gchar **id_out)
{
    static const gchar MARK[] = "] Security event ";
    *sev_out = NULL;
    *id_out = NULL;
    const gchar *close = (msg && msg[0] == '[') ? strchr(msg, ']') : NULL;
    if (close && close > msg + 1)
        *sev_out = g_strndup(msg + 1, (gsize)(close - msg - 1));
    const gchar *p = close ? strstr(close, MARK) : NULL;
    if (p == close && p) {
        p += sizeof(MARK) - 1;
        const gchar *end = strchr(p, ':');
        if (end && end > p)
            *id_out = g_strndup(p, (gsize)(end - p));
    }
    if (!*sev_out)
        *sev_out = g_strdup("warn");                           
}

                                                                  
void
pcv_vm_read_mask_security_alerts(PcvVmReadScope *s, const PcvVmOwnerMap *m, JsonArray *alerts, PcvVmReadEventTargetFn lookup,
                                 gpointer ud)
{
    if (!alerts || pcv_vm_read_scope_all(s))
        return;
    if (!lookup)
        lookup = _store_event_target;
    for (guint i = 0; i < json_array_get_length(alerts); i++) {
        JsonObject *row = json_array_get_object_element(alerts, i);
        if (!row || !_alert_is_security(row))
            continue;                                    
        gchar *sev = NULL, *id = NULL, *kind = NULL, *target = NULL;
        _parse_security_message(_str_member(row, "message"), &sev, &id);
        gboolean visible = FALSE;
        if (id && lookup(id, &kind, &target, ud))
            visible = !_kind_is_vm(kind) || pcv_vm_read_visible_name(s, m, target);                        
        if (!visible) {
            gchar *masked = g_strdup_printf("[%s] Security event: details hidden", sev);
            json_object_set_string_member(row, "message", masked);
            g_free(masked);
        }
        g_free(sev);
        g_free(id);
        g_free(kind);
        g_free(target);
    }
}

                                                                         
JsonNode *
pcv_vm_read_filter_agent_comparison(PcvVmReadScope *s, const PcvVmOwnerMap *m, JsonNode *take)
{
    if (!take || pcv_vm_read_scope_all(s) || !JSON_NODE_HOLDS_OBJECT(take))
        return take;
    JsonNode *pn = json_object_get_member(json_node_get_object(take), "providers");
    JsonArray *providers = (pn && JSON_NODE_HOLDS_ARRAY(pn)) ? json_node_get_array(pn) : NULL;
    for (guint i = providers ? json_array_get_length(providers) : 0; i-- > 0;) {
        JsonObject *p = json_array_get_object_element(providers, i);
        const gchar *vm = p ? _str_member(p, "target_vm") : NULL;
        if (!p)
            json_array_remove_element(providers, i);
        else if (!vm || !*vm)
            json_object_set_string_member(p, "reason", "");                           
        else if (!pcv_vm_read_visible_name(s, m, vm))
            json_array_remove_element(providers, i);
    }
    return take;
}

                                                     
PcvWsScopeKind
pcv_ws_scope_kind(const gchar *type)
{
    if (!type)
        return PCV_WS_SCOPE_ADMIN;
    if (g_strcmp0(type, "healing") == 0 || g_strcmp0(type, "healing-request") == 0 || g_strcmp0(type, "anomaly") == 0)
        return PCV_WS_SCOPE_ADMIN;
    if (g_strcmp0(type, "agent-comparison") == 0)
        return PCV_WS_SCOPE_FILTER;
    if (g_strcmp0(type, "job.complete") == 0)
        return PCV_WS_SCOPE_JOB;
    if (g_strcmp0(type, "metric") == 0 || g_strcmp0(type, "metrics-update") == 0 || g_strcmp0(type, "forecast") == 0)
        return PCV_WS_SCOPE_ALL;
    return PCV_WS_SCOPE_ALL;                                          
}

                                          
gboolean
pcv_ws_scope_should_send(PcvWsScopeKind kind, gint conn_role, const gchar *conn_subject, const gchar *conn_tenant,
                         const gchar *actor, const gchar *actor_tenant)
{
    switch (kind) {
    case PCV_WS_SCOPE_ADMIN:
        return conn_role >= PCV_ROLE_ADMIN;
    case PCV_WS_SCOPE_JOB:
        return pcv_vm_read_decide(conn_role, conn_subject, conn_tenant, actor, actor_tenant);
    case PCV_WS_SCOPE_ALL:
    case PCV_WS_SCOPE_FILTER:
    default:
        return TRUE;
    }
}

                                       
typedef struct {
    gboolean present;
    gint     role;
    gchar   *tenant;
} WsSubjectEntry;

struct _PcvWsSubjectCache {
    GHashTable *t;                                     
};

                                                 
static gboolean (*g_ws_lookup_fn)(const gchar *subject, gint *role, const gchar **tenant);
static gint g_ws_lookups;

static void
_ws_entry_free(gpointer p)
{
    WsSubjectEntry *e = p;
    if (!e) return;
    g_free(e->tenant);
    g_free(e);
}

                    
PcvWsSubjectCache *
pcv_ws_subject_cache_new(void)
{
    PcvWsSubjectCache *c = g_new0(PcvWsSubjectCache, 1);
    c->t = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, _ws_entry_free);
    return c;
}

                                                            
gboolean
pcv_ws_subject_cache_lookup(PcvWsSubjectCache *c, const gchar *subject, gint *role_out, const gchar **tenant_out)
{
    if (!c || !subject || !*subject)
        return FALSE;
    WsSubjectEntry *e = g_hash_table_lookup(c->t, subject);
    if (!e) {
        e = g_new0(WsSubjectEntry, 1);
        g_atomic_int_inc(&g_ws_lookups);
        gint role = PCV_ROLE_VIEWER;
        const gchar *tenant = NULL;
        if (g_ws_lookup_fn) {
            e->present = g_ws_lookup_fn(subject, &role, &tenant);
        } else {
            PcvRole r = PCV_ROLE_VIEWER;
            e->present = pcv_rbac_lookup_subject(subject, &r, NULL) == PCV_USER_PRESENT;
            role = (gint)r;
            tenant = (e->present && role < PCV_ROLE_ADMIN) ? pcv_rbac_get_tenant(subject) : NULL;                           
        }
        e->role = role;
        e->tenant = (e->present && role < PCV_ROLE_ADMIN && tenant && *tenant && pcv_res_name_is_valid(tenant)) ? g_strdup(tenant) : NULL;
        g_hash_table_insert(c->t, g_strdup(subject), e);
    }
    if (role_out) *role_out = e->role;
    if (tenant_out) *tenant_out = e->tenant;
    return e->present;
}

                    
void
pcv_ws_subject_cache_free(PcvWsSubjectCache *c)
{
    if (!c) return;
    g_hash_table_unref(c->t);
    g_free(c);
}

                                                                                           
void pcv_ws_subject_cache_set_lookup_for_test(gboolean (*fn)(const gchar *subject, gint *role, const gchar **tenant));
void
pcv_ws_subject_cache_set_lookup_for_test(gboolean (*fn)(const gchar *subject, gint *role, const gchar **tenant))
{
    g_ws_lookup_fn = fn;
}

                                                                       
guint pcv_ws_subject_lookups_for_test(void);
guint
pcv_ws_subject_lookups_for_test(void)
{
    return (guint)g_atomic_int_get(&g_ws_lookups);
}
