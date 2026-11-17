                                                                             
                                       
                                                                                               
  
                           
                                                                                               
                                                                             
                                                                                                          
                                                                     
                                                                                                      
                                                                                 
                                        
                                                                                
                               
  
                       
                                                                                 
                                             
                                                                                
#include "modules/resource/vm_read_scope.h"

#include <string.h>

#include "modules/auth/pcv_rbac.h"
#include "modules/resource/resource_registry.h"
#include "modules/daemons/vm_diag_view.h"                          

                                                         
struct _PcvVmReadScope {
    gint        role;
    gchar      *subject;                           
    gchar      *tenant;                                           
    GHashTable *owner_tenant;                                             
};

                                                                 
static const gchar *(*g_owner_tenant_fn)(const gchar *owner);
static gint g_owner_lookups;
                                                                                                
static const gchar *(*g_caller_tenant_fn)(const gchar *subject);
static gint g_caller_lookups;

                                                                                  
                                                   
static gboolean
_tenant_present(const gchar *t)
{
    return t && *t && pcv_res_name_is_valid(t);
}

                                                 
                                                                          
gboolean
pcv_vm_read_decide(gint role, const gchar *subject, const gchar *caller_tenant, const gchar *owner,
                   const gchar *owner_tenant)
{
    if (role >= PCV_ROLE_ADMIN)
        return TRUE;
    if (!owner || !*owner)
        return FALSE;
    if (subject && *subject && strcmp(owner, subject) == 0)
        return TRUE;
    if (!_tenant_present(owner_tenant) || !_tenant_present(caller_tenant))
        return FALSE;
    return strcmp(owner_tenant, caller_tenant) == 0;
}

                                                          
PcvVmReadScope *
pcv_vm_read_scope_new(const PcvResCaller *caller)
{
    PcvVmReadScope *s = g_new0(PcvVmReadScope, 1);
    s->role = PCV_ROLE_VIEWER;                       
    if (caller) {
        s->role = caller->role;
        s->subject = (caller->subject && *caller->subject) ? g_strdup(caller->subject) : NULL;
        s->tenant = (s->role < PCV_ROLE_ADMIN && caller->tenant && *caller->tenant) ? g_strdup(caller->tenant) : NULL;
    }
    return s;
}

                                                                                
PcvVmReadScope *
pcv_vm_read_scope_new_parts(gint role, const gchar *subject, gboolean tenant_known, const gchar *tenant)
{
    PcvVmReadScope *s = g_new0(PcvVmReadScope, 1);
    s->role = role;
    s->subject = (subject && *subject) ? g_strdup(subject) : NULL;
    if (role >= PCV_ROLE_ADMIN)
        return s;                                   
    if (!tenant_known) {
        g_atomic_int_inc(&g_caller_lookups);
        tenant = g_caller_tenant_fn ? g_caller_tenant_fn(s->subject) : pcv_rbac_get_tenant(s->subject);                 
    }
    s->tenant = (tenant && *tenant && pcv_res_name_is_valid(tenant)) ? g_strdup(tenant) : NULL;
    return s;
}

                                                  
gboolean
pcv_vm_read_scope_all(const PcvVmReadScope *s)
{
    return s && s->role >= PCV_ROLE_ADMIN;
}

                                                         
                                                         
static const gchar *
_owner_tenant_cached(PcvVmReadScope *s, const gchar *owner)
{
    if (!s->owner_tenant)
        s->owner_tenant = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    const gchar *t = g_hash_table_lookup(s->owner_tenant, owner);
    if (t)
        return t;
    g_atomic_int_inc(&g_owner_lookups);
    const gchar *borrowed = g_owner_tenant_fn ? g_owner_tenant_fn(owner) : pcv_res_owner_tenant(owner);
    gchar *copy = g_strdup(borrowed ? borrowed : "");                                            
    g_hash_table_insert(s->owner_tenant, g_strdup(owner), copy);
    return copy;
}

                                                                             
gboolean
pcv_vm_read_visible(PcvVmReadScope *s, const gchar *owner)
{
    if (!s)
        return FALSE;
    if (pcv_vm_read_scope_all(s))
        return TRUE;
    if (!owner || !*owner)
        return FALSE;
                                                                        
    if (pcv_vm_read_decide(s->role, s->subject, s->tenant, owner, NULL))
        return TRUE;
    if (!_tenant_present(s->tenant))
        return FALSE;                                                          
    return pcv_vm_read_decide(s->role, s->subject, s->tenant, owner, _owner_tenant_cached(s, owner));
}

                                                                               
struct _PcvVmOwnerMap {
    GPtrArray  *rows;                                  
    GHashTable *by_name;
    GHashTable *by_uuid;
    GHashTable *by_id;
};

                                                              
PcvVmOwnerMap *
pcv_vm_owner_map_current(gint64 now_mono_us)
{
    GPtrArray *rows = pcv_vm_diag_store_owner_rows(now_mono_us);
    if (!rows)
        return NULL;                                  
    PcvVmOwnerMap *m = g_new0(PcvVmOwnerMap, 1);
    m->rows = rows;
    m->by_name = g_hash_table_new(g_str_hash, g_str_equal);
    m->by_uuid = g_hash_table_new(g_str_hash, g_str_equal);
    m->by_id = g_hash_table_new(g_direct_hash, g_direct_equal);
    for (guint i = 0; i < rows->len; i++) {
        PcvVmDiagOwnerRow *r = g_ptr_array_index(rows, i);
        if (r->name)
            g_hash_table_insert(m->by_name, r->name, r);
        if (r->uuid)
            g_hash_table_insert(m->by_uuid, r->uuid, r);
        if (r->domain_id >= 0)
            g_hash_table_insert(m->by_id, GINT_TO_POINTER(r->domain_id + 1), r);
    }
    return m;
}

                                                             
                                                        
static gboolean
_map_row_visible(PcvVmReadScope *s, GHashTable *index, gconstpointer key)
{
    if (pcv_vm_read_scope_all(s))
        return TRUE;
    if (!s || !index || !key)
        return FALSE;
    const PcvVmDiagOwnerRow *r = g_hash_table_lookup(index, key);
    return r && pcv_vm_read_visible(s, r->owner);
}

                   
gboolean
pcv_vm_read_visible_name(PcvVmReadScope *s, const PcvVmOwnerMap *m, const gchar *name)
{
    return _map_row_visible(s, m ? m->by_name : NULL, name);
}

                     
gboolean
pcv_vm_read_visible_uuid(PcvVmReadScope *s, const PcvVmOwnerMap *m, const gchar *uuid)
{
    return _map_row_visible(s, m ? m->by_uuid : NULL, uuid);
}

                                                      
gboolean
pcv_vm_read_visible_domain_id(PcvVmReadScope *s, const PcvVmOwnerMap *m, gint domain_id)
{
    return _map_row_visible(s, m ? m->by_id : NULL, domain_id >= 0 ? GINT_TO_POINTER(domain_id + 1) : NULL);
}

                                                          
void
pcv_vm_read_job_scope(const PcvVmReadScope *s, PcvJobScope *out)
{
    *out = (PcvJobScope){ .all = pcv_vm_read_scope_all(s), .role = s ? s->role : PCV_ROLE_VIEWER,
                          .subject = s ? s->subject : NULL, .tenant = s ? s->tenant : NULL };
    out->decide = pcv_vm_read_decide;
}

                                            
void
pcv_vm_owner_map_free(PcvVmOwnerMap *m)
{
    if (!m)
        return;
    g_hash_table_unref(m->by_name);
    g_hash_table_unref(m->by_uuid);
    g_hash_table_unref(m->by_id);
    g_ptr_array_unref(m->rows);
    g_free(m);
}

                                                     
void
pcv_vm_read_scope_free(PcvVmReadScope *s)
{
    if (!s)
        return;
    g_free(s->subject);
    g_free(s->tenant);
    if (s->owner_tenant)
        g_hash_table_destroy(s->owner_tenant);
    g_free(s);
}

                                                                                              
                                                                                                   
                                                      
void pcv_vm_read_set_owner_tenant_fn_for_test(const gchar *(*fn)(const gchar *owner));
void
pcv_vm_read_set_owner_tenant_fn_for_test(const gchar *(*fn)(const gchar *owner))
{
    g_owner_tenant_fn = fn;
}

                                                                                                                  
                                                                  
void pcv_vm_read_set_caller_tenant_fn_for_test(const gchar *(*fn)(const gchar *subject));
void
pcv_vm_read_set_caller_tenant_fn_for_test(const gchar *(*fn)(const gchar *subject))
{
    g_caller_tenant_fn = fn;
}

                                                                                                      
guint pcv_vm_read_caller_lookups_for_test(void);
guint
pcv_vm_read_caller_lookups_for_test(void)
{
    return (guint)g_atomic_int_get(&g_caller_lookups);
}

                                                                               
                                              
guint pcv_vm_read_owner_lookups_for_test(void);
guint
pcv_vm_read_owner_lookups_for_test(void)
{
    return (guint)g_atomic_int_get(&g_owner_lookups);
}
