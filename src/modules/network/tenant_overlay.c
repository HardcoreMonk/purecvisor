   
                         
                                                      
  
                           
                                                   
                                                    
                                        
  
                                                    
  
          
                                                
                                               
                                                     
                                               
                                                           
                                                
                                    
  
             
                                                          
                                                       
                                                          
                               
  
         
                                                                
                                                   
                           
  
                                                         
                                                        
                                                           
  
         
                                                         
                                                          
                                                    
                                                
                                                
  
           
                                                          
                                                        
  
                  
                                                                      
                                                                    
                                                    
                                                                      
                                                        
  
                                            
                                                           
                                                                            
                                                                            
                                                            
                                                
                                                                           
                                                                
                                                                
                                                                   
                                       
  
                                   
                                                                    
                                                        
                                                                        
                                                     
                                                             
                                                             
                   
   

#include "modules/network/tenant_overlay.h"

#include "modules/network/network_dhcp.h"                                                          
#include "modules/network/security_group.h"                                            
#include "modules/security/security_store.h"                                  
#include "utils/pcv_config.h"                                                       
#include "utils/pcv_log.h"                                                           
#include "utils/pcv_validate.h"                                                   
#include "utils/pcv_secure.h"                                                

#include <glib.h>
#include <stdio.h>
#include <string.h>                                                   

                                                                 
                                                                     
extern gboolean pcv_vpc_vm_is_attached(const gchar *vm_identifier)
    __attribute__((weak));

                                                                        
#define TOVL_LOG_DOM "TOVL"

                                     
#define PCV_TOVL_INDEX_MIN   1
#define PCV_TOVL_INDEX_MAX   255
                                                              
#define PCV_TOVL_HOST_MIN    10
#define PCV_TOVL_HOST_MAX    254

                                            
enum {
    PCV_TOVL_ERR_ALREADY_EXISTS = 1,
    PCV_TOVL_ERR_NOT_FOUND,
    PCV_TOVL_ERR_POOL_EXHAUSTED,
    PCV_TOVL_ERR_SUBNET_FULL,
    PCV_TOVL_ERR_HAS_MEMBERS,
    PCV_TOVL_ERR_STORE_UNAVAILABLE,
    PCV_TOVL_ERR_ENCRYPT_FAILED,
    PCV_TOVL_ERR_DECRYPT_FAILED,
                                              
    PCV_TOVL_ERR_MEMBER_EXISTS,                                      
    PCV_TOVL_ERR_MEMBER_NOT_FOUND,                                   
    PCV_TOVL_ERR_SLOT_EXHAUSTED,                                       
    PCV_TOVL_ERR_SG_BOUND,                                                                 
    PCV_TOVL_ERR_VPC_BOUND,                                                                 
                           
    PCV_TOVL_ERR_PARTIAL_CLEANUP,                                             
    PCV_TOVL_ERR_MEMBER_IN_OTHER_TENANT,                                    
};

   
                         
                                                    
                                      
                                                    
                                                                         
                                                                     
                                                                   
                                                            
                                                                         
                                                                      
   
typedef struct {
    gchar *vm;
    gchar *overlay_ip;
    gchar *pubkey;
    gchar *ep_name;
    gchar *transport_ip;
    guint  slot;
} TenantMember;

                                                               
  
                                                        
                                              
                                                       
                                               
                     
  
                                             
                                                     
                          
  
                                               
                                                    
static void
_member_free(gpointer data)
{
    TenantMember *m = data;
    if (!m) return;
    g_free(m->vm);
    g_free(m->overlay_ip);
    g_free(m->pubkey);
    g_free(m->ep_name);
    g_free(m->transport_ip);
    g_free(m);
}

   
          
                                                 
                                   
                                                              
                                                                 
                                                           
                                            
                                                                             
                                                                          
                                                   
   
typedef struct {
    gchar      *name;
    guint8      index;
    gchar      *subnet_cidr;
    GHashTable *used_ips;
    GPtrArray  *members;
} Tenant;

                                                               

static GHashTable *g_tenant_map = NULL;                        
static GMutex      g_tenant_mu;

                                                         
                                                                   
                                                              
                                                           
                                                           
                                                          
                                                      
                                                             
                                                
                                           
static GMutex      g_tenant_mesh_mu;

                                                            
                                                             
                                                                         
                                                                   
                                                              
                                                    
                         
                                                 
                                             
#define PCV_TOVL_SLOT_PER_OCTET  245u                                        
#define PCV_TOVL_SLOT_C_MIN      10u
#define PCV_TOVL_SLOT_MAX        (256u * PCV_TOVL_SLOT_PER_OCTET)
#define PCV_TOVL_WG_PORT         51820u                               
                                                                  
                                                          

                                                                         
                                                                
                                                 
_Static_assert(PCV_TOVL_SLOT_MAX <= 0xFFFFF,
              "ep_name(ovl+5-hex) 포맷이 IFNAMSIZ 가드를 넘습니다");

static GHashTable *g_slot_used = NULL;                                            
static GMutex      g_slot_mu;

                                                          
                            
static guint g_slot_cap_for_test = 0;

                                                    
  
                                                         
                                                                
                                                                 
                            
  
                                               
                                                 
                                                   
                                             
                                         
  
                                                        
                              
static void
_tenant_free(gpointer data)
{
    Tenant *t = data;
    if (!t) return;
    g_free(t->name);
    g_free(t->subnet_cidr);
    if (t->used_ips) g_hash_table_unref(t->used_ips);
    if (t->members)  g_ptr_array_unref(t->members);
    g_free(t);
}

                                       
  
                                               
                                          
                                                            
                                          
  
                                                      
                                                 
static void
_ensure_init(void)
{
    if (!g_tenant_map) {
        g_tenant_map = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             NULL, _tenant_free);
    }
}

                                                            
                                                 
                  
static TenantMember *
_member_find_locked(Tenant *t, const gchar *vm)
{
    for (guint i = 0; i < t->members->len; i++) {
        TenantMember *m = g_ptr_array_index(t->members, i);
        if (g_strcmp0(m->vm, vm) == 0) return m;
    }
    return NULL;
}

                                                                           

static void
_slot_ensure_init(void)
{
    if (!g_slot_used) {
        g_slot_used = g_hash_table_new(g_direct_hash, g_direct_equal);
    }
}

                                                          
static gchar *
_slot_ep_name(guint slot)
{
    return g_strdup_printf("ovl%05x", slot);
}

                                                                          
static gchar *
_slot_transport_ip(guint slot)
{
    guint b = slot / PCV_TOVL_SLOT_PER_OCTET;
    guint c = PCV_TOVL_SLOT_C_MIN + (slot % PCV_TOVL_SLOT_PER_OCTET);
    return g_strdup_printf("172.31.%u.%u", b, c);
}

                                                        
                                                   
static gboolean
_slot_alloc(guint *slot_out, GError **error)
{
    g_mutex_lock(&g_slot_mu);
    _slot_ensure_init();

    guint cap = g_slot_cap_for_test ? g_slot_cap_for_test : PCV_TOVL_SLOT_MAX;
    guint slot = cap;
    for (guint s = 0; s < cap; s++) {
        if (!g_hash_table_contains(g_slot_used, GUINT_TO_POINTER(s))) {
            slot = s;
            break;
        }
    }
    if (slot == cap) {
        g_mutex_unlock(&g_slot_mu);
        g_set_error(error, g_quark_from_static_string("tenant_overlay"),
                    PCV_TOVL_ERR_SLOT_EXHAUSTED,
                    "전역 transport/엔드포인트 슬롯 풀이 소진되었습니다 (상한 %u)",
                    cap);
        return FALSE;
    }

    g_hash_table_add(g_slot_used, GUINT_TO_POINTER(slot));
    g_mutex_unlock(&g_slot_mu);
    *slot_out = slot;
    return TRUE;
}

                                                      
                        
  
                                                
                                                     
                                                  
                                                
                                                     
                                        
  
                                                    
                                          
static void
_slot_free(guint slot)
{
    g_mutex_lock(&g_slot_mu);
    _slot_ensure_init();
    g_hash_table_remove(g_slot_used, GUINT_TO_POINTER(slot));
    g_mutex_unlock(&g_slot_mu);
}

                                                                            
         
                                                                               

   
                                                             
                                                     
                                                      
                                                                    
                                             
                                                                  
                                            
                                 
   
gboolean
pcv_tenant_overlay_create(const gchar *name, GError **error)
{
    g_return_val_if_fail(name != NULL, FALSE);

    g_mutex_lock(&g_tenant_mu);
    _ensure_init();

    if (g_hash_table_contains(g_tenant_map, name)) {
        g_mutex_unlock(&g_tenant_mu);
        g_set_error(error, g_quark_from_static_string("tenant_overlay"),
                    PCV_TOVL_ERR_ALREADY_EXISTS,
                    "테넌트 '%s' 가 이미 존재합니다", name);
        return FALSE;
    }

                                                     
                             
    gboolean used[PCV_TOVL_INDEX_MAX + 1] = { FALSE };
    GHashTableIter it;
    gpointer key, val;
    g_hash_table_iter_init(&it, g_tenant_map);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        Tenant *existing = val;
        used[existing->index] = TRUE;
    }

    guint index = 0;
    for (guint i = PCV_TOVL_INDEX_MIN; i <= PCV_TOVL_INDEX_MAX; i++) {
        if (!used[i]) { index = i; break; }
    }
    if (index == 0) {
        g_mutex_unlock(&g_tenant_mu);
        g_set_error(error, g_quark_from_static_string("tenant_overlay"),
                    PCV_TOVL_ERR_POOL_EXHAUSTED,
                    "오버레이 풀(10.100.0.0/16)이 소진되었습니다 (테넌트 %d개 상한)",
                    PCV_TOVL_INDEX_MAX);
        return FALSE;
    }

    Tenant *t = g_new0(Tenant, 1);
    t->name        = g_strdup(name);
    t->index       = (guint8)index;
    t->subnet_cidr = g_strdup_printf("10.100.%u.0/24", index);
    t->used_ips    = g_hash_table_new(g_direct_hash, g_direct_equal);
    t->members     = g_ptr_array_new_with_free_func(_member_free);

    g_hash_table_insert(g_tenant_map, t->name, t);

                                                                    
                                                                         
                                                     
                                                        
                                                                 
                                                               
                                                     
                                                        
    if (pcv_security_store_ensure_open()) {
        GError *dberr = NULL;
        if (!pcv_security_store_put_tenant(name, (gint)index, &dberr)) {
            g_hash_table_remove(g_tenant_map, name);                             
            g_mutex_unlock(&g_tenant_mu);
            g_propagate_error(error, dberr);
            return FALSE;
        }
    } else {
        PCV_LOG_DEBUG(TOVL_LOG_DOM,
                      "테넌트 '%s' 영속 생략 — security_store 미개방(in-memory 전용)",
                      name);
    }

    g_mutex_unlock(&g_tenant_mu);
    return TRUE;
}

   
                                                   
                                                      
                               
                                                  
                                
                                                        
                                                                    
   
gboolean
pcv_tenant_overlay_delete(const gchar *name, GError **error)
{
    g_return_val_if_fail(name != NULL, FALSE);

                                                                     
                                                          
                                                                        
    g_mutex_lock(&g_tenant_mesh_mu);
    g_mutex_lock(&g_tenant_mu);
    _ensure_init();

    Tenant *t = g_hash_table_lookup(g_tenant_map, name);
    if (!t) {
        g_mutex_unlock(&g_tenant_mu);
        g_mutex_unlock(&g_tenant_mesh_mu);
        g_set_error(error, g_quark_from_static_string("tenant_overlay"),
                    PCV_TOVL_ERR_NOT_FOUND,
                    "테넌트 '%s' 가 존재하지 않습니다", name);
        return FALSE;
    }

    if (t->members->len > 0) {
        guint n = t->members->len;
        g_mutex_unlock(&g_tenant_mu);
        g_mutex_unlock(&g_tenant_mesh_mu);
        g_set_error(error, g_quark_from_static_string("tenant_overlay"),
                    PCV_TOVL_ERR_HAS_MEMBERS,
                    "테넌트 '%s' 에 멤버 %u개가 남아있어 삭제할 수 없습니다",
                    name, n);
        return FALSE;
    }

                                                             
                                                      
                                                                     
                                        
    if (pcv_security_store_ensure_open()) {
        GError *dberr = NULL;
        if (!pcv_security_store_del_tenant(name, &dberr)) {
            g_mutex_unlock(&g_tenant_mu);
            g_mutex_unlock(&g_tenant_mesh_mu);
            g_propagate_error(error, dberr);
            return FALSE;
        }
    } else {
        PCV_LOG_DEBUG(TOVL_LOG_DOM,
                      "테넌트 '%s' 삭제 영속 생략 — security_store 미개방(in-memory 전용)",
                      name);
    }

    g_hash_table_remove(g_tenant_map, name);                          
    g_mutex_unlock(&g_tenant_mu);
    g_mutex_unlock(&g_tenant_mesh_mu);
    return TRUE;
}

   
                                                                 
                                                 
                           
                                              
                                                                    
                                                   
   
gchar *
pcv_tenant_overlay_alloc_ip(const gchar *name, GError **error)
{
    g_return_val_if_fail(name != NULL, NULL);

    g_mutex_lock(&g_tenant_mu);
    _ensure_init();

    Tenant *t = g_hash_table_lookup(g_tenant_map, name);
    if (!t) {
        g_mutex_unlock(&g_tenant_mu);
        g_set_error(error, g_quark_from_static_string("tenant_overlay"),
                    PCV_TOVL_ERR_NOT_FOUND,
                    "테넌트 '%s' 가 존재하지 않습니다", name);
        return NULL;
    }

                                                             
                                          
    guint host = 0;
    for (guint h = PCV_TOVL_HOST_MIN; h <= PCV_TOVL_HOST_MAX; h++) {
        if (!g_hash_table_contains(t->used_ips, GUINT_TO_POINTER(h))) {
            host = h;
            break;
        }
    }
    if (host == 0) {
        guint8 index = t->index;
        g_mutex_unlock(&g_tenant_mu);
        g_set_error(error, g_quark_from_static_string("tenant_overlay"),
                    PCV_TOVL_ERR_SUBNET_FULL,
                    "테넌트 '%s' 서브넷(10.100.%u.0/24)이 포화되었습니다",
                    name, index);
        return NULL;
    }

    g_hash_table_add(t->used_ips, GUINT_TO_POINTER(host));
    gchar *ip = g_strdup_printf("10.100.%u.%u", t->index, host);
    g_mutex_unlock(&g_tenant_mu);
    return ip;
}

   
                                                                  
                                                       
                                                            
                                
   
void
pcv_tenant_overlay_free_ip(const gchar *name, const gchar *ip)
{
    if (!name || !ip) return;

    g_mutex_lock(&g_tenant_mu);
    _ensure_init();

    Tenant *t = g_hash_table_lookup(g_tenant_map, name);
    if (!t) {
        g_mutex_unlock(&g_tenant_mu);
        return;
    }

                                                   
                                                      
    guint index = 0, host = 0;
    if (sscanf(ip, "10.100.%u.%u", &index, &host) != 2 || index != t->index) {
        g_mutex_unlock(&g_tenant_mu);
        return;
    }

    g_hash_table_remove(t->used_ips, GUINT_TO_POINTER(host));
    g_mutex_unlock(&g_tenant_mu);
}

   
                                             
                                      
                                                                       
                    
   
GPtrArray *
pcv_tenant_overlay_list(void)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);

    g_mutex_lock(&g_tenant_mu);
    _ensure_init();

    GHashTableIter it;
    gpointer key, val;
    g_hash_table_iter_init(&it, g_tenant_map);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        g_ptr_array_add(out, g_strdup((const gchar *)key));
    }

    g_mutex_unlock(&g_tenant_mu);
    return out;
}

   
                                                        
                                                   
                                   
                                                            
                                                                            
   
gboolean
pcv_tenant_overlay_get_subnet(const gchar *name, gchar **cidr_out)
{
    g_return_val_if_fail(cidr_out != NULL, FALSE);
    *cidr_out = NULL;
    if (!name) return FALSE;

    g_mutex_lock(&g_tenant_mu);
    _ensure_init();

    Tenant *t = g_hash_table_lookup(g_tenant_map, name);
    if (t) *cidr_out = g_strdup(t->subnet_cidr);

    g_mutex_unlock(&g_tenant_mu);
    return t != NULL;
}

                                                                            
                                                                            
                                                                               

   
                                                                     
                                                
                                         
                                                                 
                                                                   
                                                                   
                                   
                                                                   
   
gboolean
pcv_tenant_overlay_gen_and_store_key(const gchar *tenant, const gchar *vm,
                                     const gchar *overlay_ip,
                                     gchar **pubkey_out, GError **error)
{
    g_return_val_if_fail(tenant != NULL, FALSE);
    g_return_val_if_fail(vm != NULL, FALSE);
    if (pubkey_out) *pubkey_out = NULL;

    gchar *priv = NULL, *pub = NULL;
    if (!pcv_tenant_overlay_wg_genkeys(&priv, &pub, error)) {
        return FALSE;
    }

                                                       
                                                      
    gchar *enc = pcv_config_encrypt_value(priv);
    pcv_secure_free_str(&priv);
    if (!enc) {
        g_free(pub);
        g_set_error(error, g_quark_from_static_string("tenant_overlay"),
                    PCV_TOVL_ERR_ENCRYPT_FAILED,
                    "개인키 AES-256-GCM 암호화 실패 (테넌트 '%s' VM '%s')",
                    tenant, vm);
        return FALSE;
    }

    if (!pcv_security_store_ensure_open()) {
        g_free(pub);
        g_free(enc);
        g_set_error(error, g_quark_from_static_string("tenant_overlay"),
                    PCV_TOVL_ERR_STORE_UNAVAILABLE,
                    "security_store 를 열 수 없습니다 (테넌트 '%s' VM '%s')",
                    tenant, vm);
        return FALSE;
    }

    gboolean ok = pcv_security_store_put_wg_key(tenant, vm, pub, enc,
                                                overlay_ip, error);
    g_free(enc);
    if (!ok) {
        g_free(pub);
        return FALSE;
    }

    if (pubkey_out) {
        *pubkey_out = pub;
    } else {
        g_free(pub);
    }
    return TRUE;
}

                                                      
                                                                   
                                                        
                                             
   
                                                       
                                              
                                                                             
                                                            
                                                   
                                                                 
   
gboolean
pcv_tenant_overlay_load_privkey(const gchar *tenant, const gchar *vm,
                                gchar **privkey_out, GError **error)
{
    g_return_val_if_fail(tenant != NULL, FALSE);
    g_return_val_if_fail(vm != NULL, FALSE);
    g_return_val_if_fail(privkey_out != NULL, FALSE);
    *privkey_out = NULL;

    if (!pcv_security_store_ensure_open()) {
        g_set_error(error, g_quark_from_static_string("tenant_overlay"),
                    PCV_TOVL_ERR_STORE_UNAVAILABLE,
                    "security_store 를 열 수 없습니다 (테넌트 '%s' VM '%s')",
                    tenant, vm);
        return FALSE;
    }

    gchar *enc = NULL;
    if (!pcv_security_store_get_wg_key(tenant, vm, NULL, &enc, NULL, error)) {
                                                             
                                                      
        if (error && !*error) {
            g_set_error(error, g_quark_from_static_string("tenant_overlay"),
                        PCV_TOVL_ERR_NOT_FOUND,
                        "테넌트 '%s' VM '%s' 의 WG 키가 없습니다", tenant, vm);
        }
        return FALSE;
    }

    gchar *priv = pcv_config_decrypt_value(enc);
    g_free(enc);
    if (!priv) {
        g_set_error(error, g_quark_from_static_string("tenant_overlay"),
                    PCV_TOVL_ERR_DECRYPT_FAILED,
                    "개인키 복호화 실패 (테넌트 '%s' VM '%s')", tenant, vm);
        return FALSE;
    }

    *privkey_out = priv;
    return TRUE;
}

                                                                            
                                                              
                                                                               

                                                             
                                                         
                                                          
static gboolean
_teardown_absent_ok(const gchar *stderr_s)
{
    return stderr_s && (strstr(stderr_s, "No such file")
                     || strstr(stderr_s, "Cannot open network namespace")
                     || strstr(stderr_s, "does not exist")
                     || strstr(stderr_s, "Cannot find device"));
}

                                                         
                                                               
                                                              
                                                
                                    
                                                        
                                                              
static void
_mesh_cleanup_warn(gboolean ok, GError *err, const gchar *what, const gchar *ep)
{
    if (ok) return;
    const gchar *msg = (err && err->message) ? err->message : NULL;
    if (_teardown_absent_ok(msg)) return;
    PCV_LOG_WARN(TOVL_LOG_DOM, "%s(ep='%s') 실패(dangling 가능): %s",
                 what, ep, msg ? msg : "unknown");
}

                                                           
                                                 
                         
static void
_cleanup_fail_append(GString *fails, const gchar *what, const gchar *ep,
                     GError **perr)
{
    const gchar *msg = (perr && *perr && (*perr)->message)
                           ? (*perr)->message : "unknown";
    if (fails->len) g_string_append(fails, "; ");
    g_string_append_printf(fails, "%s(ep='%s'): %s", what, ep, msg);
    if (perr) g_clear_error(perr);
}

                                                           
                                                        
                                                 
static void
_slot_release_after_teardown(guint slot, const gchar *ep_name)
{
    GError *terr = NULL;
    if (pcv_tenant_overlay_wg_endpoint_down(ep_name, &terr)) {
        _slot_free(slot);
    } else {
        PCV_LOG_WARN(TOVL_LOG_DOM,
                     "롤백: slot %u 보류(quarantine) — endpoint '%s' 정리 실패: %s",
                     slot, ep_name, terr && terr->message ? terr->message : "unknown");
        g_clear_error(&terr);
    }
}

                                                             
                                                      
                         
static const gchar *_find_owning_tenant_locked(const gchar *vm);

   
                                                                  
                                                   
                                                
                                              
                                                                  
                                                                     
                                                             
                                                        
                                                                  
   
gchar *
pcv_tenant_overlay_attach_vm(const gchar *tenant, const gchar *vm, GError **error)
{
    g_return_val_if_fail(tenant != NULL, NULL);
    g_return_val_if_fail(vm != NULL, NULL);

                                                         
                                                                
                                                                  
    if (pcv_security_group_vm_is_bound(vm)) {
        g_set_error(error, g_quark_from_static_string("tenant_overlay"),
                    PCV_TOVL_ERR_SG_BOUND,
                    "VM '%s' 는 SG 바인딩 상태 — SG와 tenant-overlay는 상호 배타입니다 "
                    "(암호 격리=tenant-overlay, per-VM L3/L4 정책=SG(bridge 모드))", vm);
        return NULL;
    }
    if (pcv_vpc_vm_is_attached && pcv_vpc_vm_is_attached(vm)) {
        g_set_error(error, g_quark_from_static_string("tenant_overlay"),
                    PCV_TOVL_ERR_VPC_BOUND,
                    "VM '%s' 는 Local VPC attachment 상태 — VPC와 tenant-overlay는 상호 배타입니다",
                    vm);
        return NULL;
    }

    g_mutex_lock(&g_tenant_mesh_mu);

                                                         
                                                        
                                                             
    g_mutex_lock(&g_tenant_mu);
    _ensure_init();
    Tenant *t = g_hash_table_lookup(g_tenant_map, tenant);
    if (!t) {
        g_mutex_unlock(&g_tenant_mu);
        g_mutex_unlock(&g_tenant_mesh_mu);
        g_set_error(error, g_quark_from_static_string("tenant_overlay"),
                    PCV_TOVL_ERR_NOT_FOUND,
                    "테넌트 '%s' 가 존재하지 않습니다", tenant);
        return NULL;
    }
    if (_member_find_locked(t, vm)) {
        g_mutex_unlock(&g_tenant_mu);
        g_mutex_unlock(&g_tenant_mesh_mu);
        g_set_error(error, g_quark_from_static_string("tenant_overlay"),
                    PCV_TOVL_ERR_MEMBER_EXISTS,
                    "VM '%s' 는 이미 테넌트 '%s' 에 조인되어 있습니다", vm, tenant);
        return NULL;
    }

                                                            
                                                         
                                                
                                                         
                 
    {
        const gchar *owner = _find_owning_tenant_locked(vm);
        if (owner && g_strcmp0(owner, tenant) != 0) {
            gchar *owner_cp = g_strdup(owner);
            g_mutex_unlock(&g_tenant_mu);
            g_mutex_unlock(&g_tenant_mesh_mu);
            g_set_error(error, g_quark_from_static_string("tenant_overlay"),
                        PCV_TOVL_ERR_MEMBER_IN_OTHER_TENANT,
                        "VM '%s' 는 이미 테넌트 '%s' 에 조인되어 있습니다 — "
                        "cross-tenant 다중 소속은 금지입니다", vm, owner_cp);
            g_free(owner_cp);
            return NULL;
        }
    }

    GPtrArray *existing = g_ptr_array_new_with_free_func(_member_free);
    for (guint i = 0; i < t->members->len; i++) {
        TenantMember *src = g_ptr_array_index(t->members, i);
        TenantMember *cp = g_new0(TenantMember, 1);
        cp->vm           = g_strdup(src->vm);
        cp->overlay_ip   = g_strdup(src->overlay_ip);
        cp->pubkey       = g_strdup(src->pubkey);
        cp->ep_name      = g_strdup(src->ep_name);
        cp->transport_ip = g_strdup(src->transport_ip);
        g_ptr_array_add(existing, cp);
    }
    g_mutex_unlock(&g_tenant_mu);

    GError *lerr = NULL;

                              
    gchar *overlay_ip = pcv_tenant_overlay_alloc_ip(tenant, &lerr);
    if (!overlay_ip) {
        g_ptr_array_unref(existing);
        g_mutex_unlock(&g_tenant_mesh_mu);
        g_propagate_error(error, lerr);
        return NULL;
    }

                                     
    gchar *pubkey = NULL;
    if (!pcv_tenant_overlay_gen_and_store_key(tenant, vm, overlay_ip, &pubkey, &lerr)) {
        pcv_tenant_overlay_free_ip(tenant, overlay_ip);
        g_free(overlay_ip);
        g_ptr_array_unref(existing);
        g_mutex_unlock(&g_tenant_mesh_mu);
        g_propagate_error(error, lerr);
        return NULL;
    }

                                                             
    guint slot = 0;
    if (!_slot_alloc(&slot, &lerr)) {
        pcv_security_store_del_wg_key(tenant, vm, NULL);
        pcv_tenant_overlay_free_ip(tenant, overlay_ip);
        g_free(overlay_ip); g_free(pubkey);
        g_ptr_array_unref(existing);
        g_mutex_unlock(&g_tenant_mesh_mu);
        g_propagate_error(error, lerr);
        return NULL;
    }

                                                                          
                                                          
                                                            
                                                                       
                                                                
                                                            
    if (!pcv_security_store_set_wg_key_slot(tenant, vm, (gint)slot, &lerr)) {
        _slot_free(slot);
        pcv_security_store_del_wg_key(tenant, vm, NULL);
        pcv_tenant_overlay_free_ip(tenant, overlay_ip);
        g_free(overlay_ip); g_free(pubkey);
        g_ptr_array_unref(existing);
        g_mutex_unlock(&g_tenant_mesh_mu);
        g_propagate_error(error, lerr);
        return NULL;
    }

    gchar *ep_name      = _slot_ep_name(slot);
    gchar *transport_ip = _slot_transport_ip(slot);

                                                              
                                                           
                                                        
                                                        
                                  
    network_dhcp_release_overlay_ip(ep_name, NULL);
    GError *pcerr = NULL;
    if (!pcv_tenant_overlay_wg_endpoint_down(ep_name, &pcerr)) {
        PCV_LOG_WARN(TOVL_LOG_DOM,
                     "attach 선정리 실패 — slot %u 보류(quarantine), ep '%s': %s",
                     slot, ep_name,
                     pcerr && pcerr->message ? pcerr->message : "unknown");
        pcv_security_store_del_wg_key(tenant, vm, NULL);
        pcv_tenant_overlay_free_ip(tenant, overlay_ip);
        g_free(overlay_ip); g_free(pubkey); g_free(ep_name); g_free(transport_ip);
        g_ptr_array_unref(existing);
        g_mutex_unlock(&g_tenant_mesh_mu);
        g_propagate_error(error, pcerr);
        return NULL;
    }

                                                              
    gchar *privkey = NULL;
    if (!pcv_tenant_overlay_load_privkey(tenant, vm, &privkey, &lerr)) {
        _slot_free(slot);
        pcv_security_store_del_wg_key(tenant, vm, NULL);
        pcv_tenant_overlay_free_ip(tenant, overlay_ip);
        g_free(overlay_ip); g_free(pubkey); g_free(ep_name); g_free(transport_ip);
        g_ptr_array_unref(existing);
        g_mutex_unlock(&g_tenant_mesh_mu);
        g_propagate_error(error, lerr);
        return NULL;
    }

    gchar *overlay_cidr   = g_strdup_printf("%s/32", overlay_ip);
                                                                        
                                                             
                                                          
    gchar *transport_cidr = g_strdup_printf("%s/16", transport_ip);
    gboolean up_ok = pcv_tenant_overlay_wg_endpoint_up(ep_name, privkey, overlay_cidr,
                                                       PCV_TOVL_WG_PORT, transport_cidr,
                                                       &lerr);
    pcv_secure_free_str(&privkey);                                         
    g_free(overlay_cidr);
    g_free(transport_cidr);

    if (!up_ok) {
        _slot_release_after_teardown(slot, ep_name);
        pcv_security_store_del_wg_key(tenant, vm, NULL);
        pcv_tenant_overlay_free_ip(tenant, overlay_ip);
        g_free(overlay_ip); g_free(pubkey); g_free(ep_name); g_free(transport_ip);
        g_ptr_array_unref(existing);
        g_mutex_unlock(&g_tenant_mesh_mu);
        g_propagate_error(error, lerr);
        return NULL;
    }

                                                          
                                                          
                                  
                                                     
                                      
    gchar *new_endpoint = g_strdup_printf("%s:%u", transport_ip, PCV_TOVL_WG_PORT);
    guint mesh_added = 0;
    gboolean mesh_ok = TRUE;

    for (guint i = 0; i < existing->len; i++) {
        TenantMember *m = g_ptr_array_index(existing, i);
        gchar *m_endpoint = g_strdup_printf("%s:%u", m->transport_ip, PCV_TOVL_WG_PORT);

                                             
        if (!pcv_tenant_overlay_wg_peer_add(ep_name, m->pubkey, m->overlay_ip,
                                           m_endpoint, &lerr)) {
            g_free(m_endpoint);
            mesh_ok = FALSE;
            break;
        }
                                                     
        if (!pcv_tenant_overlay_wg_peer_add(m->ep_name, pubkey, overlay_ip,
                                           new_endpoint, &lerr)) {
                                   
            GError *rerr = NULL;
            gboolean rok = pcv_tenant_overlay_wg_peer_remove(ep_name, m->pubkey,
                                                             m->overlay_ip, &rerr);
            _mesh_cleanup_warn(rok, rerr, "attach 롤백 peer_remove", ep_name);
            g_clear_error(&rerr);
            g_free(m_endpoint);
            mesh_ok = FALSE;
            break;
        }
        g_free(m_endpoint);
        mesh_added++;
    }
    g_free(new_endpoint);

    if (!mesh_ok) {
                                                        
        for (guint i = 0; i < mesh_added; i++) {
            TenantMember *m = g_ptr_array_index(existing, i);
            GError *rerr = NULL;
            gboolean rok = pcv_tenant_overlay_wg_peer_remove(m->ep_name, pubkey,
                                                             overlay_ip, &rerr);
            _mesh_cleanup_warn(rok, rerr, "attach 롤백 peer_remove", m->ep_name);
            g_clear_error(&rerr);

            rok = pcv_tenant_overlay_wg_peer_remove(ep_name, m->pubkey,
                                                    m->overlay_ip, &rerr);
            _mesh_cleanup_warn(rok, rerr, "attach 롤백 peer_remove", ep_name);
            g_clear_error(&rerr);
        }
        _slot_release_after_teardown(slot, ep_name);
        pcv_security_store_del_wg_key(tenant, vm, NULL);
        pcv_tenant_overlay_free_ip(tenant, overlay_ip);
        g_free(overlay_ip); g_free(pubkey); g_free(ep_name); g_free(transport_ip);
        g_ptr_array_unref(existing);
        g_mutex_unlock(&g_tenant_mesh_mu);
        g_propagate_error(error, lerr);
        return NULL;
    }
    g_ptr_array_unref(existing);

                                                    
                                                          
                                                                 
                              
    g_mutex_lock(&g_tenant_mu);
    TenantMember *nm = g_new0(TenantMember, 1);
    nm->vm           = g_strdup(vm);
    nm->overlay_ip   = overlay_ip;               
    nm->pubkey       = pubkey;                   
    nm->ep_name      = ep_name;                  
    nm->transport_ip = transport_ip;             
    nm->slot         = slot;
    g_ptr_array_add(t->members, nm);
    g_mutex_unlock(&g_tenant_mu);

    gchar *ret = g_strdup(overlay_ip);
    g_mutex_unlock(&g_tenant_mesh_mu);
    return ret;
}

   
                                                       
                                                  
                                                      
                                                                               
                                
                                                                  
                                                                       
   
gboolean
pcv_tenant_overlay_detach_vm(const gchar *tenant, const gchar *vm, GError **error)
{
    g_return_val_if_fail(tenant != NULL, FALSE);
    g_return_val_if_fail(vm != NULL, FALSE);

    g_mutex_lock(&g_tenant_mesh_mu);

                                                            
    g_mutex_lock(&g_tenant_mu);
    _ensure_init();
    Tenant *t = g_hash_table_lookup(g_tenant_map, tenant);
    if (!t) {
        g_mutex_unlock(&g_tenant_mu);
        g_mutex_unlock(&g_tenant_mesh_mu);
        g_set_error(error, g_quark_from_static_string("tenant_overlay"),
                    PCV_TOVL_ERR_NOT_FOUND,
                    "테넌트 '%s' 가 존재하지 않습니다", tenant);
        return FALSE;
    }
    TenantMember *self = _member_find_locked(t, vm);
    if (!self) {
        g_mutex_unlock(&g_tenant_mu);
        g_mutex_unlock(&g_tenant_mesh_mu);
                                                     
                                                    
                                                      
        g_set_error(error, g_quark_from_static_string("tenant_overlay"),
                    PCV_TOVL_ERR_MEMBER_NOT_FOUND,
                    "VM '%s' 는 테넌트 '%s' 에 조인되어 있지 않습니다", vm, tenant);
        return FALSE;
    }

    gchar *self_ep         = g_strdup(self->ep_name);
    gchar *self_pubkey     = g_strdup(self->pubkey);
    gchar *self_overlay_ip = g_strdup(self->overlay_ip);
    guint  self_slot       = self->slot;

                                                 
                                                  
    GPtrArray *remaining_eps = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < t->members->len; i++) {
        TenantMember *m = g_ptr_array_index(t->members, i);
        if (g_strcmp0(m->vm, vm) == 0) continue;
        g_ptr_array_add(remaining_eps, g_strdup(m->ep_name));
    }
    g_mutex_unlock(&g_tenant_mu);

                                                       
                                                          
                                                      
                                                          
                                                              
                                                           
                                                             
                                    
                                                     
                                                         
    GError *kerr = NULL;
    if (!pcv_security_store_del_wg_key(tenant, vm, &kerr)) {
        g_ptr_array_unref(remaining_eps);
        g_free(self_ep); g_free(self_pubkey); g_free(self_overlay_ip);
        g_mutex_unlock(&g_tenant_mesh_mu);
        g_propagate_error(error, kerr);
        return FALSE;
    }

                                                            
                                                                    
                                                                  
                                            
                                                 
    g_mutex_lock(&g_tenant_mu);
    for (guint i = 0; i < t->members->len; i++) {
        TenantMember *m = g_ptr_array_index(t->members, i);
        if (g_strcmp0(m->vm, vm) == 0) {
            g_ptr_array_remove_index_fast(t->members, i);                    
            break;
        }
    }
    g_mutex_unlock(&g_tenant_mu);
    pcv_tenant_overlay_free_ip(tenant, self_overlay_ip);

                                                    
                                                                  
    GString *fails = g_string_new(NULL);

    for (guint i = 0; i < remaining_eps->len; i++) {
        const gchar *ep = g_ptr_array_index(remaining_eps, i);
        GError *perr = NULL;
        if (!pcv_tenant_overlay_wg_peer_remove(ep, self_pubkey,
                                               self_overlay_ip, &perr)) {
            _mesh_cleanup_warn(FALSE, perr, "detach peer_remove", ep);
            if (!_teardown_absent_ok(perr ? perr->message : NULL))
                _cleanup_fail_append(fails, "peer_remove", ep, &perr);
            g_clear_error(&perr);
        }
    }
    g_ptr_array_unref(remaining_eps);

    gboolean self_clean = TRUE;

                                                              
                                                   
    GError *derr = NULL;
    if (!network_dhcp_release_overlay_ip(self_ep, &derr)) {
        self_clean = FALSE;
        _cleanup_fail_append(fails, "dhcp_release", self_ep, &derr);
    }

                                                   
    GError *eerr = NULL;
    if (!pcv_tenant_overlay_wg_endpoint_down(self_ep, &eerr)) {
        self_clean = FALSE;
        _cleanup_fail_append(fails, "endpoint_down", self_ep, &eerr);
    }

                                                       
                                                                 
                                                         
                                                   
                                                              
    if (self_clean) {
        _slot_free(self_slot);
    } else {
        PCV_LOG_WARN(TOVL_LOG_DOM,
                     "detach: slot %u 보류(quarantine) — endpoint '%s' 정리 실패, "
                     "세션 내 재사용 차단(재시작 후 잔존물은 attach 선정리가 수렴)",
                     self_slot, self_ep);
    }

    g_free(self_ep);
    g_free(self_pubkey);
    g_free(self_overlay_ip);

    g_mutex_unlock(&g_tenant_mesh_mu);

    if (fails->len > 0) {
        g_set_error(error, g_quark_from_static_string("tenant_overlay"),
                    PCV_TOVL_ERR_PARTIAL_CLEANUP,
                    "detach 커널 정리 부분 실패(멤버십 해제는 확정 — 재-detach 는 "
                    "MEMBER_NOT_FOUND 가 되며, 잔재는 운영자 확인 필요): %s",
                    fails->str);
        g_string_free(fails, TRUE);
        return FALSE;
    }
    g_string_free(fails, TRUE);
    return TRUE;
}

   
                                                                  
                                     
                                                                                 
                                                          
   
gboolean
pcv_tenant_overlay_get_member_ep(const gchar *tenant, const gchar *vm,
                                 gchar **ep_name_out)
{
    g_return_val_if_fail(ep_name_out != NULL, FALSE);
    *ep_name_out = NULL;
    if (!tenant || !vm) return FALSE;

    g_mutex_lock(&g_tenant_mu);
    _ensure_init();
    Tenant *t = g_hash_table_lookup(g_tenant_map, tenant);
    TenantMember *m = t ? _member_find_locked(t, vm) : NULL;
    if (m) *ep_name_out = g_strdup(m->ep_name);
    g_mutex_unlock(&g_tenant_mu);

    return m != NULL;
}

                                                                            
                                                      
                                                                               

                                          
                                                   
                                 
static const gchar *
_find_owning_tenant_locked(const gchar *vm)
{
    GHashTableIter it;
    gpointer key, val;
    g_hash_table_iter_init(&it, g_tenant_map);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        Tenant *t = val;
        if (_member_find_locked(t, vm)) return t->name;
    }
    return NULL;
}

   
                                                               
                                                   
                                           
                                                                        
   
gboolean
pcv_tenant_overlay_vm_in_any_tenant(const gchar *vm)
{
    if (!vm) return FALSE;

    g_mutex_lock(&g_tenant_mu);
    _ensure_init();
    gboolean found = _find_owning_tenant_locked(vm) != NULL;
    g_mutex_unlock(&g_tenant_mu);

    return found;
}

   
                                                              
                                                      
                                                                           
   
GPtrArray *
pcv_tenant_overlay_list_member_vms(void)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);

    g_mutex_lock(&g_tenant_mu);
    _ensure_init();
    GHashTableIter it;
    gpointer key, val;
    g_hash_table_iter_init(&it, g_tenant_map);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        Tenant *t = val;
        for (guint i = 0; i < t->members->len; i++) {
            TenantMember *m = g_ptr_array_index(t->members, i);
            gboolean dup = FALSE;
            for (guint j = 0; j < out->len && !dup; j++) {
                if (g_strcmp0(g_ptr_array_index(out, j), m->vm) == 0)
                    dup = TRUE;
            }
            if (!dup) g_ptr_array_add(out, g_strdup(m->vm));
        }
    }
    g_mutex_unlock(&g_tenant_mu);
    return out;
}

                                                             
                                                        
                                
                                                
                                         
gboolean
pcv_tenant_overlay_ep_name_is_overlay(const gchar *name)
{
    if (!name || strlen(name) != 8) return FALSE;
    if (strncmp(name, "ovl", 3) != 0) return FALSE;
    for (guint i = 3; i < 8; i++) {
        gchar c = name[i];
        if (!g_ascii_isdigit(c) && (c < 'a' || c > 'f')) return FALSE;
    }
    return TRUE;
}

                                                              
                                                              
                                                   
                                                
                                                     
                                                      
                                                  
                                                    
guint
pcv_tenant_overlay_sweep_orphan_endpoints(guint *fail_out)
{
    if (fail_out) *fail_out = 0;

    GHashTable *owned = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_mutex_lock(&g_tenant_mu);
    _ensure_init();
    GHashTableIter it;
    gpointer key, val;
    g_hash_table_iter_init(&it, g_tenant_map);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        Tenant *t = val;
        for (guint i = 0; i < t->members->len; i++) {
            TenantMember *m = g_ptr_array_index(t->members, i);
            g_hash_table_add(owned, g_strdup(m->ep_name));
        }
    }
    g_mutex_unlock(&g_tenant_mu);

    GError *lerr = NULL;
    GPtrArray *netns = pcv_tenant_overlay_wg_list_netns(&lerr);
    if (!netns) {
        PCV_LOG_WARN(TOVL_LOG_DOM, "고아 endpoint 스윕: netns 열거 실패 — 스킵: %s",
                     lerr && lerr->message ? lerr->message : "unknown");
        g_clear_error(&lerr);
        g_hash_table_unref(owned);
        return 0;
    }

    guint swept = 0, failed = 0;
    for (guint i = 0; i < netns->len; i++) {
        const gchar *ns = g_ptr_array_index(netns, i);
        if (!pcv_tenant_overlay_ep_name_is_overlay(ns)) continue;                  
        if (g_hash_table_contains(owned, ns)) continue;                            

        network_dhcp_release_overlay_ip(ns, NULL);
        GError *derr = NULL;
        if (pcv_tenant_overlay_wg_endpoint_down(ns, &derr)) {
            swept++;
            PCV_LOG_INFO(TOVL_LOG_DOM, "고아 endpoint 회수: %s", ns);
        } else {
            failed++;
            PCV_LOG_WARN(TOVL_LOG_DOM, "고아 endpoint 회수 실패 '%s': %s",
                         ns, derr && derr->message ? derr->message : "unknown");
            g_clear_error(&derr);
        }
    }
    g_ptr_array_unref(netns);
    g_hash_table_unref(owned);
    if (fail_out) *fail_out = failed;
    return swept;
}

   
                                                            
                                                 
                                                    
                                                               
                                                          
   
void
pcv_tenant_overlay_on_vm_gone(const gchar *vm)
{
    if (!vm) return;

    GQuark dom = g_quark_from_static_string("tenant_overlay");

                                                                        
                                                           
                                              
                                                         
                                                         
                              
    for (guint iter = 0; iter < PCV_TOVL_INDEX_MAX; iter++) {
        g_mutex_lock(&g_tenant_mu);
        _ensure_init();
        const gchar *owner = _find_owning_tenant_locked(vm);
        gchar *tenant_name = owner ? g_strdup(owner) : NULL;
        g_mutex_unlock(&g_tenant_mu);

        if (!tenant_name) return;                                  

        GError *err = NULL;
        if (!pcv_tenant_overlay_detach_vm(tenant_name, vm, &err)) {
                                                             
                                                                   
                                                       
                                                      
                                                 
            gboolean progressed = g_error_matches(err, dom,
                                                  PCV_TOVL_ERR_PARTIAL_CLEANUP);
            gboolean benign = g_error_matches(err, dom, PCV_TOVL_ERR_NOT_FOUND) ||
                              g_error_matches(err, dom,
                                              PCV_TOVL_ERR_MEMBER_NOT_FOUND);
            if (progressed || benign) {
                if (progressed) {
                    PCV_LOG_WARN(TOVL_LOG_DOM,
                                 "on_vm_gone: VM '%s' 테넌트 '%s' 부분 정리"
                                 "(멤버십 해제는 확정, 잔재 있음): %s",
                                 vm, tenant_name,
                                 err && err->message ? err->message : "unknown");
                }
            } else {
                PCV_LOG_WARN(TOVL_LOG_DOM,
                             "on_vm_gone: VM '%s' 오버레이 정리 실패(테넌트 '%s', "
                             "멤버 잔존 — 중단, dangling 가능): %s",
                             vm, tenant_name,
                             err && err->message ? err->message : "unknown");
                g_clear_error(&err);
                g_free(tenant_name);
                return;
            }
            g_clear_error(&err);
        }
        g_free(tenant_name);
    }
    PCV_LOG_WARN(TOVL_LOG_DOM,
                 "on_vm_gone: VM '%s' detach-all 상한(%d) 도달 — 비정상 상태 의심",
                 vm, PCV_TOVL_INDEX_MAX);
}

                                                                            
                                                                
                                                                               

   
                                                         
                                                      
                                               
                                                                              
                                                                   
                                            
   
gboolean
pcv_tenant_overlay_rehydrate(GError **error)
{
    if (!pcv_security_store_ensure_open()) {
        g_set_error(error, g_quark_from_static_string("tenant_overlay"),
                    PCV_TOVL_ERR_STORE_UNAVAILABLE,
                    "security_store 를 열 수 없어 오버레이 레지스트리를 재수화할 수 없습니다");
        return FALSE;
    }

                                                             
                                                              
                                                     
    GPtrArray *trows = NULL, *krows = NULL;
    if (!pcv_security_store_list_tenants(&trows, error)) {
        return FALSE;
    }
    if (!pcv_security_store_list_wg_keys(&krows, error)) {
        g_ptr_array_unref(trows);
        return FALSE;
    }

    g_mutex_lock(&g_tenant_mu);
    _ensure_init();

                                                                    
                                                              
                          
    for (guint i = 0; i < trows->len; i++) {
        PcvOverlayTenantRow *r = g_ptr_array_index(trows, i);
        if (!pcv_validate_vm_name(r->name)) {
            PCV_LOG_WARN(TOVL_LOG_DOM,
                         "재수화: 테넌트 이름 손상('%s') — skip",
                         r->name ? r->name : "(null)");
            continue;
        }
        if (r->subnet_index < PCV_TOVL_INDEX_MIN ||
            r->subnet_index > PCV_TOVL_INDEX_MAX) {
            PCV_LOG_WARN(TOVL_LOG_DOM,
                         "재수화: 테넌트 '%s' subnet_index=%d 범위 밖(%d..%d) — skip",
                         r->name, r->subnet_index,
                         PCV_TOVL_INDEX_MIN, PCV_TOVL_INDEX_MAX);
            continue;
        }
        if (g_hash_table_contains(g_tenant_map, r->name)) continue;           

        Tenant *t = g_new0(Tenant, 1);
        t->name        = g_strdup(r->name);
        t->index       = (guint8)r->subnet_index;
        t->subnet_cidr = g_strdup_printf("10.100.%u.0/24", (guint)r->subnet_index);
        t->used_ips    = g_hash_table_new(g_direct_hash, g_direct_equal);
        t->members     = g_ptr_array_new_with_free_func(_member_free);
        g_hash_table_insert(g_tenant_map, t->name, t);
    }

                                                                          
                                                                      
                                                                 
                                          
                                                    
                                                    
    for (guint i = 0; i < krows->len; i++) {
        PcvOverlayWgKeyRow *k = g_ptr_array_index(krows, i);
        if (!pcv_validate_vm_name(k->tenant) || !pcv_validate_vm_name(k->vm)) {
            PCV_LOG_WARN(TOVL_LOG_DOM,
                         "재수화: WG 키 행의 tenant/vm 이름 손상(tenant='%s' vm='%s') — skip",
                         k->tenant ? k->tenant : "(null)", k->vm ? k->vm : "(null)");
            continue;
        }

        Tenant *t = g_hash_table_lookup(g_tenant_map, k->tenant);
        if (!t) {
            PCV_LOG_WARN(TOVL_LOG_DOM,
                         "재수화: WG 키(테넌트 '%s' VM '%s')의 테넌트가 overlay_tenants "
                         "에 없음 — 멤버 복원 skip", k->tenant, k->vm);
            continue;
        }
        if (k->slot < 0) {
            PCV_LOG_WARN(TOVL_LOG_DOM,
                         "재수화: 테넌트 '%s' VM '%s' slot 미기록(레거시 slot<0) — "
                         "멤버 복원 skip(고아 방지)", k->tenant, k->vm);
            continue;
        }
        if ((guint)k->slot >= PCV_TOVL_SLOT_MAX) {
            PCV_LOG_WARN(TOVL_LOG_DOM,
                         "재수화: 테넌트 '%s' VM '%s' slot=%d 범위 밖(0..%u) — skip",
                         k->tenant, k->vm, k->slot, PCV_TOVL_SLOT_MAX - 1);
            continue;
        }
        if (_member_find_locked(t, k->vm)) continue;           

                                                                     
                                                         
                                                       
                                                            
                                       
        guint pidx = 0, phost = 0;
        if (!k->overlay_ip ||
            sscanf(k->overlay_ip, "10.100.%u.%u", &pidx, &phost) != 2 ||
            pidx != (guint)t->index ||
            phost < PCV_TOVL_HOST_MIN || phost > PCV_TOVL_HOST_MAX) {
            PCV_LOG_WARN(TOVL_LOG_DOM,
                         "재수화: 테넌트 '%s' VM '%s' overlay_ip='%s' 가 서브넷"
                         "(10.100.%u.0/24)과 불일치 — skip",
                         k->tenant, k->vm, k->overlay_ip ? k->overlay_ip : "(null)",
                         (guint)t->index);
            continue;
        }

                                                         
                                                            
                                                                   
        g_mutex_lock(&g_slot_mu);
        _slot_ensure_init();
        gboolean slot_taken =
            g_hash_table_contains(g_slot_used, GUINT_TO_POINTER((guint)k->slot));
        if (!slot_taken) {
            g_hash_table_add(g_slot_used, GUINT_TO_POINTER((guint)k->slot));
        }
        g_mutex_unlock(&g_slot_mu);
        if (slot_taken) {
            PCV_LOG_WARN(TOVL_LOG_DOM,
                         "재수화: 테넌트 '%s' VM '%s' slot=%d 이 이미 다른 멤버가 "
                         "사용 중(데이터 손상 의심) — skip",
                         k->tenant, k->vm, k->slot);
            continue;
        }

        TenantMember *m = g_new0(TenantMember, 1);
        m->vm           = g_strdup(k->vm);
        m->overlay_ip   = g_strdup(k->overlay_ip);
        m->pubkey       = g_strdup(k->pubkey ? k->pubkey : "");
        m->ep_name      = _slot_ep_name((guint)k->slot);
        m->transport_ip = _slot_transport_ip((guint)k->slot);
        m->slot         = (guint)k->slot;
        g_ptr_array_add(t->members, m);
        g_hash_table_add(t->used_ips, GUINT_TO_POINTER(phost));
    }

    g_mutex_unlock(&g_tenant_mu);
    g_ptr_array_unref(trows);
    g_ptr_array_unref(krows);
    return TRUE;
}

                                                                            
                                                              
                                                                     
       
                                                                               

                                                
gint
pcv_tenant_overlay_count_members_for_test(const gchar *tenant)
{
    if (!tenant) return -1;
    g_mutex_lock(&g_tenant_mu);
    _ensure_init();
    Tenant *t = g_hash_table_lookup(g_tenant_map, tenant);
    gint n = t ? (gint)t->members->len : -1;
    g_mutex_unlock(&g_tenant_mu);
    return n;
}

                                                          
                                                     
void
pcv_tenant_overlay_reset_for_test(void)
{
    g_mutex_lock(&g_tenant_mu);
    if (g_tenant_map) g_hash_table_remove_all(g_tenant_map);
    g_mutex_unlock(&g_tenant_mu);

    g_mutex_lock(&g_slot_mu);
    if (g_slot_used) g_hash_table_remove_all(g_slot_used);
    g_mutex_unlock(&g_slot_mu);
}

                                               
gboolean
pcv_tenant_overlay_slot_used_for_test(guint slot)
{
    g_mutex_lock(&g_slot_mu);
    _slot_ensure_init();
    gboolean used = g_hash_table_contains(g_slot_used, GUINT_TO_POINTER(slot));
    g_mutex_unlock(&g_slot_mu);
    return used;
}

                                                              
void
pcv_tenant_overlay_set_slot_cap_for_test(guint cap)
{
    g_mutex_lock(&g_slot_mu);
    g_slot_cap_for_test = cap;
    g_mutex_unlock(&g_slot_mu);
}
