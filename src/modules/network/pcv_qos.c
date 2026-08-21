                                
                                                                     
  
                           
                                                   
                                                    
                                        
  
                                                  
                                                
                                              
                                                       
                                                    
                                                
                                                      
                          
  
                                                              
  
         
                                                               
                                                             
                                                    
                                                           
                                               
  
                                                    
                                               
                                                     
                                                   
                                                 
   
#include "modules/network/pcv_qos.h"
#include "utils/pcv_log.h"
#include "utils/pcv_spawn.h"
#include "utils/pcv_validate.h"
#include "utils/pcv_config.h"                                             
#include "utils/pcv_worker_pool.h"                                     
#include "modules/daemons/prometheus_exporter.h"                                          
#include <glib/gstdio.h>
#include <string.h>
#include <errno.h>

#define PCV_QOS_TENANT_MIN 0x0010u
#define PCV_QOS_TENANT_MAX 0x0FFFu
#define PCV_QOS_VM_MIN     0x1000u
#define PCV_QOS_VM_MAX     0xFFFDu

#define QOS_LOG_DOM "qos"

static GMutex      g_qos_ids_mu;
static GHashTable *g_tenant_fwd  = NULL;                                                 
static GHashTable *g_tenant_used = NULL;                                                 
static GHashTable *g_vm_fwd      = NULL;                                                      
static GHashTable *g_vm_used     = NULL;                                                 

                                                        
                        
                                                                
                                                                   
                                                            
                                                                               
                                                                
                                                             
                                                           
                                                         
                                                        
                                                       
                                                                 
                                                            
                                                               
                                                                   
                                                            
                                                                        
                                                                 
                                             
                                                            
static GMutex      g_qos_tc_mu;
static guint32     g_qos_uplink_mbps  = 0;
static GHashTable *g_qos_active_vms   = NULL;
static GHashTable *g_qos_tenant_refcnt = NULL;

                                                                         
typedef struct {
    gchar *tenant;
    gchar *iface;
} _QosActiveEntry;

                                                               
                                                           
static void
_qos_active_entry_free(gpointer p)
{
    _QosActiveEntry *e = p;
    if (!e) return;
    g_free(e->tenant);
    g_free(e->iface);
    g_free(e);
}

                                                         
                                                     
static void
_ensure_tc_tables(void)
{
    if (!g_qos_active_vms)
        g_qos_active_vms = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                  g_free, _qos_active_entry_free);
    if (!g_qos_tenant_refcnt)
        g_qos_tenant_refcnt = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
}

                                                          
                                               
                                             
                                                       
static guint32
_djb2(const char *s)
{
    guint32 h = 5381;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        h = ((h << 5) + h) + *p;                                       
    return h;
}

                                                         
                                                  
static void
_ensure_tables(void)
{
    if (!g_tenant_fwd)
        g_tenant_fwd = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    if (!g_tenant_used)
        g_tenant_used = g_hash_table_new(g_direct_hash, g_direct_equal);
    if (!g_vm_fwd)
        g_vm_fwd = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    if (!g_vm_used)
        g_vm_used = g_hash_table_new(g_direct_hash, g_direct_equal);
}

                                                       
                                                
                                                     
                             
  
                                               
                                                
                                          
  
                                                              
                                          
                                                                     
                                       
                                                            
                                                   
static guint16
_alloc_minor(GHashTable *fwd, GHashTable *used,
             const char *name, const char *hash_key,
             guint32 lo, guint32 hi)
{
    gpointer existing;
                                                        
    if (g_hash_table_lookup_extended(fwd, name, NULL, &existing))
        return (guint16)GPOINTER_TO_UINT(existing);

    guint32 range = hi - lo + 1;                                 
    guint32 start = lo + (_djb2(hash_key) % range);                              

    guint32 chosen = start;
    gboolean found = FALSE;
                                                    
                                                   
    for (guint32 tries = 0; tries < range; tries++) {
        guint32 cand = lo + ((start - lo + tries) % range);
        if (!g_hash_table_contains(used, GUINT_TO_POINTER(cand))) {
            chosen = cand;
            found = TRUE;
            break;
        }
    }
    if (!found) {
                                                  
                                                  
                                               
                                                     
                                 
        g_critical("pcv_qos: minor range [0x%x,0x%x] exhausted for '%s'",
                   lo, hi, name);
        chosen = start;
    }

    g_hash_table_add(used, GUINT_TO_POINTER(chosen));
    g_hash_table_insert(fwd, g_strdup(name), GUINT_TO_POINTER(chosen));
    return (guint16)chosen;
}

   
                                                                 
                                                    
                                          
                                                              
                                                       
   
guint16
pcv_qos_tenant_minor(const char *tenant)
{
    g_mutex_lock(&g_qos_ids_mu);
    _ensure_tables();
    guint16 m = _alloc_minor(g_tenant_fwd, g_tenant_used, tenant, tenant,
                              PCV_QOS_TENANT_MIN, PCV_QOS_TENANT_MAX);
    g_mutex_unlock(&g_qos_ids_mu);
    return m;
}

   
                                                                         
                                         
                                                                            
                                                                           
                                            
   
guint16
pcv_qos_vm_minor(const char *tenant, const char *vm)
{
    gchar *key = g_strdup_printf("%s/%s", tenant, vm);                                
    g_mutex_lock(&g_qos_ids_mu);
    _ensure_tables();
    guint16 m = _alloc_minor(g_vm_fwd, g_vm_used, key, key,
                              PCV_QOS_VM_MIN, PCV_QOS_VM_MAX);
    g_mutex_unlock(&g_qos_ids_mu);
    g_free(key);
    return m;
}

                                                                      
                                                                       
gchar *
pcv_qos_tenant_classid(const char *tenant)
{
    return g_strdup_printf("1:%x", pcv_qos_tenant_minor(tenant));                            
}

                                                              
                                                            
gchar *
pcv_qos_classid(const char *tenant, const char *vm)
{
    return g_strdup_printf("1:%x", pcv_qos_vm_minor(tenant, vm));
}

   
                                                                      
                                                   
                                           
                                                                                    
                                                        
                                                         
                                                                               
                                                             
   
gboolean
pcv_qos_sla_from_json(JsonObject *o, PcvQosSla *out, GError **error)
{
    memset(out, 0, sizeof *out);                                      

                                                                
                                                       
                                                   
                                                                
                                                        
                                                               
                                                   
                                   
    gint64 min_raw = json_object_has_member(o, "qos_min_mbps")
        ? json_object_get_int_member(o, "qos_min_mbps") : 0;
    gint64 max_raw = json_object_has_member(o, "qos_max_mbps")
        ? json_object_get_int_member(o, "qos_max_mbps") : 0;
    gint64 burst_raw = json_object_has_member(o, "qos_burst_kb")
        ? json_object_get_int_member(o, "qos_burst_kb") : 256;

    if (min_raw < 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "qos_min_mbps must be >= 0 (got %" G_GINT64_FORMAT ")", min_raw);
        return FALSE;
    }
    if (max_raw < 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "qos_max_mbps must be >= 0 (got %" G_GINT64_FORMAT ")", max_raw);
        return FALSE;
    }
    if (burst_raw < 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "qos_burst_kb must be >= 0 (got %" G_GINT64_FORMAT ")", burst_raw);
        return FALSE;
    }
                                                   
                                                                
                                                 
                                                     
                                                         
                                                            
    if (min_raw > (gint64)G_MAXUINT32) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "qos_min_mbps must be <= %u (got %" G_GINT64_FORMAT ")",
                    G_MAXUINT32, min_raw);
        return FALSE;
    }
    if (max_raw > (gint64)G_MAXUINT32) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "qos_max_mbps must be <= %u (got %" G_GINT64_FORMAT ")",
                    G_MAXUINT32, max_raw);
        return FALSE;
    }
    if (burst_raw > (gint64)G_MAXUINT32) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "qos_burst_kb must be <= %u (got %" G_GINT64_FORMAT ")",
                    G_MAXUINT32, burst_raw);
        return FALSE;
    }

    out->min_mbps = (guint32)min_raw;
    out->max_mbps = (guint32)max_raw;
    out->burst_kb = (guint32)burst_raw;

    if (out->max_mbps == 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "qos_max_mbps must be > 0");
        return FALSE;
    }
    if (out->min_mbps > out->max_mbps) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "qos_min_mbps (%u) > qos_max_mbps (%u)",
                    out->min_mbps, out->max_mbps);
        return FALSE;
    }
    return TRUE;
}

   
                                                                        
                                                      
                                                          
                                         
                                                                        
                                                                             
                                                               
                                        
   
gboolean
pcv_qos_vm_requires_sla(const gchar *network_mode, const gchar *nic_type)
{
                                                               
                                                               
                                           
    if (g_strcmp0(network_mode, "tenant-overlay") == 0)
        return TRUE;

                                                              
    if (g_strcmp0(network_mode, "dpdk") == 0 || g_strcmp0(network_mode, "sriov") == 0)
        return FALSE;

                                                          
                                                                 
    if (g_strcmp0(nic_type, "dpdk") == 0 || g_strcmp0(nic_type, "sriov") == 0)
        return FALSE;

    return TRUE;                                               
}

                                                      
                                          
  
                                                 
                                                  
                                                
                                
  
                                                   
                                                   
                                                 
                                           
                                                  
            
  
                                              
                
static gboolean
_validate_minor_object(JsonObject *obj, guint32 lo, guint32 hi, const char *what)
{
    if (!obj) return TRUE;

    gboolean ok = TRUE;
    GHashTable *seen = g_hash_table_new(g_direct_hash, g_direct_equal);
    GList *keys = json_object_get_members(obj);

    for (GList *it = keys; it && ok; it = it->next) {
        const char *name = it->data;
        JsonNode *node = json_object_get_member(obj, name);

        if (!node || !JSON_NODE_HOLDS_VALUE(node) ||
            json_node_get_value_type(node) != G_TYPE_INT64) {
            PCV_LOG_WARN(QOS_LOG_DOM,
                "qos_ids.json 오염: %s '%s' 의 minor 값이 정수가 아님 — "
                "파일 전체 폐기(부분 로드 금지)", what, name);
            ok = FALSE;
            break;
        }

        gint64 v = json_node_get_int(node);
        if (v < (gint64)lo || v > (gint64)hi) {
            PCV_LOG_WARN(QOS_LOG_DOM,
                "qos_ids.json 오염: %s '%s' minor=%" G_GINT64_FORMAT
                " 가 허용 범위 [0x%x,0x%x] 밖 — 파일 전체 폐기(부분 로드 금지)",
                what, name, v, lo, hi);
            ok = FALSE;
            break;
        }

        guint32 minor = (guint32)v;
        if (g_hash_table_contains(seen, GUINT_TO_POINTER(minor))) {
            PCV_LOG_WARN(QOS_LOG_DOM,
                "qos_ids.json 오염: %s minor=0x%x 가 둘 이상의 이름에 중복 "
                "배정됨(마지막 발견: '%s') — 파일 전체 폐기(부분 로드 금지)",
                what, minor, name);
            ok = FALSE;
            break;
        }
        g_hash_table_add(seen, GUINT_TO_POINTER(minor));
    }

    g_list_free(keys);
    g_hash_table_unref(seen);
    return ok;
}

   
                                                                         
                                                  
                                                    
                                                  
                                                                
                                                     
                                   
   
gboolean
pcv_qos_ids_load(const char *path)
{
    g_mutex_lock(&g_qos_ids_mu);

                                                 
                                            
    if (g_tenant_fwd)  g_hash_table_remove_all(g_tenant_fwd);
    if (g_tenant_used) g_hash_table_remove_all(g_tenant_used);
    if (g_vm_fwd)      g_hash_table_remove_all(g_vm_fwd);
    if (g_vm_used)     g_hash_table_remove_all(g_vm_used);
    _ensure_tables();

    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
        g_mutex_unlock(&g_qos_ids_mu);
        return TRUE;                         
    }

    gboolean ok = FALSE;
    JsonParser *p = json_parser_new();
    if (json_parser_load_from_file(p, path, NULL)) {
        JsonNode *root = json_parser_get_root(p);
        if (root && JSON_NODE_HOLDS_OBJECT(root)) {
            JsonObject *o = json_node_get_object(root);

            JsonObject *tenants = NULL, *vms = NULL;
            gboolean shape_ok = TRUE;

            if (json_object_has_member(o, "tenants")) {
                JsonNode *tn = json_object_get_member(o, "tenants");
                if (tn && JSON_NODE_HOLDS_OBJECT(tn)) {
                    tenants = json_node_get_object(tn);
                } else {
                    PCV_LOG_WARN(QOS_LOG_DOM,
                        "qos_ids.json 오염: 'tenants' 가 object 아님 — "
                        "파일 전체 폐기(부분 로드 금지)");
                    shape_ok = FALSE;
                }
            }
            if (shape_ok && json_object_has_member(o, "vms")) {
                JsonNode *vn = json_object_get_member(o, "vms");
                if (vn && JSON_NODE_HOLDS_OBJECT(vn)) {
                    vms = json_node_get_object(vn);
                } else {
                    PCV_LOG_WARN(QOS_LOG_DOM,
                        "qos_ids.json 오염: 'vms' 가 object 아님 — "
                        "파일 전체 폐기(부분 로드 금지)");
                    shape_ok = FALSE;
                }
            }

                                                     
                                                
                                                      
                                                        
                               
            if (shape_ok &&
                _validate_minor_object(tenants, PCV_QOS_TENANT_MIN, PCV_QOS_TENANT_MAX, "tenant") &&
                _validate_minor_object(vms, PCV_QOS_VM_MIN, PCV_QOS_VM_MAX, "vm")) {

                if (tenants) {
                    GList *keys = json_object_get_members(tenants);
                    for (GList *it = keys; it; it = it->next) {
                        const char *name = it->data;
                        guint32 minor = (guint32)json_object_get_int_member(tenants, name);
                        g_hash_table_insert(g_tenant_fwd, g_strdup(name),
                                            GUINT_TO_POINTER(minor));
                        g_hash_table_add(g_tenant_used, GUINT_TO_POINTER(minor));
                    }
                    g_list_free(keys);
                }
                if (vms) {
                    GList *keys = json_object_get_members(vms);
                    for (GList *it = keys; it; it = it->next) {
                        const char *name = it->data;
                        guint32 minor = (guint32)json_object_get_int_member(vms, name);
                        g_hash_table_insert(g_vm_fwd, g_strdup(name),
                                            GUINT_TO_POINTER(minor));
                        g_hash_table_add(g_vm_used, GUINT_TO_POINTER(minor));
                    }
                    g_list_free(keys);
                }
                ok = TRUE;
            }
        } else {
            PCV_LOG_WARN(QOS_LOG_DOM,
                "qos_ids.json 오염: 최상위가 JSON object 아님 — "
                "파일 전체 폐기(부분 로드 금지)");
        }
    } else {
        PCV_LOG_WARN(QOS_LOG_DOM,
            "qos_ids.json 파싱 실패(JSON 문법 오류: %s) — "
            "파일 전체 폐기(부분 로드 금지)", path);
    }
    g_object_unref(p);

    if (!ok) {
                                              
                                                  
                                                    
        g_hash_table_remove_all(g_tenant_fwd);
        g_hash_table_remove_all(g_tenant_used);
        g_hash_table_remove_all(g_vm_fwd);
        g_hash_table_remove_all(g_vm_used);
    }

    g_mutex_unlock(&g_qos_ids_mu);
    return ok;
}

   
                                                                    
                                                 
                                                   
                 
                                                         
                                        
                                                 
                                                            
                                                                  
   
gboolean
pcv_qos_ids_save(const char *path, GError **error)
{
    g_mutex_lock(&g_qos_ids_mu);
    _ensure_tables();

                                                                            
    JsonObject *root = json_object_new();
    JsonObject *tenants = json_object_new();
    JsonObject *vms = json_object_new();

    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, g_tenant_fwd);
    while (g_hash_table_iter_next(&it, &k, &v))
        json_object_set_int_member(tenants, (const char *)k,
                                   (gint64)GPOINTER_TO_UINT(v));
    g_hash_table_iter_init(&it, g_vm_fwd);
    while (g_hash_table_iter_next(&it, &k, &v))
        json_object_set_int_member(vms, (const char *)k,
                                   (gint64)GPOINTER_TO_UINT(v));

    json_object_set_object_member(root, "tenants", tenants);
    json_object_set_object_member(root, "vms", vms);

    JsonNode *rootn = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(rootn, root);

    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, rootn);
    json_generator_set_pretty(gen, TRUE);

    gsize len = 0;
    gchar *data = json_generator_to_data(gen, &len);
    g_object_unref(gen);
    json_node_free(rootn);                               

                                                     
                                                          
                                                   
                                                     
                                                 
                                                      
                                                  
                               
      
                                                  
                                                           
                                                    
                                               
                                                 
                                                 
                                                
      
                                                                  
                                                          
                                  
    gchar *dir = g_path_get_dirname(path);
    if (g_mkdir_with_parents(dir, 0755) != 0) {
        int saved_errno = errno;
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                    "qos_ids 디렉토리 생성 실패: %s (%s)", dir, g_strerror(saved_errno));
        g_free(dir);
        g_free(data);
        g_mutex_unlock(&g_qos_ids_mu);
        return FALSE;
    }
    g_free(dir);

                                                            
                               
    gboolean ok = g_file_set_contents(path, data, (gssize)len, error);
    g_free(data);
    g_mutex_unlock(&g_qos_ids_mu);
    return ok;
}

                                                       
                                                   
void
pcv_qos_ids_clear(void)
{
    g_mutex_lock(&g_qos_ids_mu);
    if (g_tenant_fwd)  g_hash_table_remove_all(g_tenant_fwd);
    if (g_tenant_used) g_hash_table_remove_all(g_tenant_used);
    if (g_vm_fwd)      g_hash_table_remove_all(g_vm_fwd);
    if (g_vm_used)     g_hash_table_remove_all(g_vm_used);
    g_mutex_unlock(&g_qos_ids_mu);
}

                                                                             
                                       
  
                                                     
  
                                   
                                                                 
                                                   
                        
                                                                
                                                                 
                              
                                                                    
                                                         
                                                              
                         
                                                           
                                               
                                                   
                                                          
                                             
                                           
                                             
                                                                
                                                                     
                                                                         
                                                                    
                                                            
                                                           
                                           
                                                                  
                                               
                                                                         
                                                      
                                
                                                              
                                                        
                                                    
                                                   
                             
                                                                   
                                                              
                                               
                                                    
                                          
                                                                                

#define PCV_QOS_ROOT_HANDLE     "1:"
#define PCV_QOS_LINKCAP_CLASSID "1:1"
#define PCV_QOS_DEFAULT_CLASSID "1:fffe"
#define PCV_QOS_DEFAULT_MINOR   "fffe"                                          

                                                               
                                                   
                                            
                                                
                                     
static gboolean
_qos_run(const gchar * const *argv, const char *what, GError **error)
{
    if (!pcv_spawn_sync(argv, NULL, NULL, error)) {
        PCV_LOG_WARN(QOS_LOG_DOM, "tc 단계 실패(%s): %s", what,
                     (error && *error) ? (*error)->message : "unknown");
        return FALSE;
    }
    return TRUE;
}

                                                   
                                                
                                                     
                                       
                                                  
                                                   
static void
_qos_run_best_effort(const gchar * const *argv)
{
    GError *err = NULL;
    if (!pcv_spawn_sync(argv, NULL, NULL, &err)) {
        PCV_LOG_WARN(QOS_LOG_DOM, "tc 정리 단계 실패(무시 — 이미 없었을 수 있음): %s",
                     err ? err->message : "unknown");
    }
    g_clear_error(&err);
}

                                                         
                                                     
                                                           
                                                              
                                         
static gchar *
_qos_class_show(void)
{
    const gchar *argv[] = {"tc", "class", "show", "dev", PCV_QOS_IFB_DEV, NULL};
    gchar *out = NULL;
    pcv_spawn_sync(argv, &out, NULL, NULL);
    return out ? out : g_strdup("");
}

                                                                    
                                                  
                                                                       
                                                    
                                                            
                                                            
                                                 
                   
static gchar *
_qos_class_show_stats(void)
{
    const gchar *argv[] = {"tc", "-s", "class", "show", "dev", PCV_QOS_IFB_DEV, NULL};
    gchar *out = NULL;
    pcv_spawn_sync(argv, &out, NULL, NULL);
    return out ? out : g_strdup("");
}

                                                   
                                                   
                                                                
                                                  
                                                    
                                                           
static gboolean
_qos_class_output_has_classid(const gchar *out, const gchar *classid)
{
    if (!out || !classid) return FALSE;
    gchar *needle = g_strdup_printf("%s ", classid);
    gboolean found = g_strstr_len(out, -1, needle) != NULL;
    g_free(needle);
    return found;
}

                                                            
                                                     
                                                    
static gboolean
_qos_ensure_ifb_dev(GError **error)
{
    gchar *sys_path = g_strdup_printf("/sys/class/net/%s", PCV_QOS_IFB_DEV);
    gboolean exists = g_file_test(sys_path, G_FILE_TEST_IS_DIR);
    g_free(sys_path);

    if (!exists) {
        const gchar *add_argv[] = {
            "ip", "link", "add", PCV_QOS_IFB_DEV, "type", "ifb", NULL
        };
        if (!_qos_run(add_argv, "ip link add pcvqos0 type ifb", error))
            return FALSE;
    }

    const gchar *up_argv[] = {"ip", "link", "set", PCV_QOS_IFB_DEV, "up", NULL};
    return _qos_run(up_argv, "ip link set pcvqos0 up", error);
}

                                                                             
                                                              
                                                                                

static GMutex      g_qos_tenant_sla_mu;
static GHashTable *g_qos_tenant_sla = NULL;                                                 

                                                                   
                                                            
                                                               
                                                                   
                                                           
                                 
                                                    
                                       
static const gchar *g_tenant_sla_path_override = NULL;

                                                    
                                                        
                                                   
  
                                                       
                                               
                                 
                                                
                                                
void
pcv_qos_set_tenant_sla_path_for_test(const gchar *path)
{
    g_tenant_sla_path_override = path;
}

                                                           
static void
_ensure_tenant_sla_table(void)
{
    if (!g_qos_tenant_sla)
        g_qos_tenant_sla = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
}

                                                            
                                               
                                                         
                                                             
       
                                                 
                                                        
                                         
static gboolean
_qos_tenant_class_replace(const gchar *tenant_cid, guint32 min_mbps, guint32 max_mbps,
                           const char *what, GError **error)
{
    gchar *ls_rate = g_strdup_printf("%uMbit", min_mbps > 0 ? min_mbps : max_mbps);
    gchar *ul_rate = g_strdup_printf("%uMbit", max_mbps);
    const gchar *argv[] = {
        "tc", "class", "replace", "dev", PCV_QOS_IFB_DEV,
        "parent", PCV_QOS_LINKCAP_CLASSID, "classid", tenant_cid,
        "hfsc", "ls", "m2", ls_rate, "ul", "m2", ul_rate, NULL
    };
    gboolean ok = _qos_run(argv, what, error);
    g_free(ls_rate);
    g_free(ul_rate);
    return ok;
}

   
                                                             
                                              
                                      
                                                                
                                                            
   
gboolean
pcv_qos_tenant_sla_get(const char *tenant, PcvQosTenantSla *out)
{
    if (!tenant || !*tenant) return FALSE;

    gboolean found = FALSE;
    g_mutex_lock(&g_qos_tenant_sla_mu);
    _ensure_tenant_sla_table();
    PcvQosTenantSla *v = g_hash_table_lookup(g_qos_tenant_sla, tenant);
    if (v) {
        if (out) *out = *v;
        found = TRUE;
    }
    g_mutex_unlock(&g_qos_tenant_sla_mu);
    return found;
}

   
                                                                          
                                                
                                                      
                                                             
                                                      
                                                                          
   
gboolean
pcv_qos_tenant_sla_load(const char *path)
{
    g_mutex_lock(&g_qos_tenant_sla_mu);
    if (g_qos_tenant_sla) g_hash_table_remove_all(g_qos_tenant_sla);
    _ensure_tenant_sla_table();

    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
        g_mutex_unlock(&g_qos_tenant_sla_mu);
        return TRUE;                                         
    }

    gboolean ok = FALSE;
    JsonParser *p = json_parser_new();
    if (json_parser_load_from_file(p, path, NULL)) {
        JsonNode *root = json_parser_get_root(p);
        if (root && JSON_NODE_HOLDS_OBJECT(root)) {
            JsonObject *o = json_node_get_object(root);
            GList *keys = json_object_get_members(o);
            gboolean shape_ok = TRUE;

                                                                    
                                                      
                                               
            for (GList *it = keys; it && shape_ok; it = it->next) {
                const char *tenant = it->data;
                JsonNode *n = json_object_get_member(o, tenant);
                if (!n || !JSON_NODE_HOLDS_OBJECT(n)) {
                    PCV_LOG_WARN(QOS_LOG_DOM,
                        "qos_tenants.json 오염: '%s' 항목이 object 아님 — "
                        "파일 전체 폐기(부분 로드 금지)", tenant);
                    shape_ok = FALSE;
                    break;
                }
                JsonObject *to = json_node_get_object(n);
                JsonNode *minn = json_object_has_member(to, "min_mbps")
                    ? json_object_get_member(to, "min_mbps") : NULL;
                JsonNode *maxn = json_object_has_member(to, "max_mbps")
                    ? json_object_get_member(to, "max_mbps") : NULL;
                if (!minn || !maxn ||
                    !JSON_NODE_HOLDS_VALUE(minn) || !JSON_NODE_HOLDS_VALUE(maxn) ||
                    json_node_get_value_type(minn) != G_TYPE_INT64 ||
                    json_node_get_value_type(maxn) != G_TYPE_INT64) {
                    PCV_LOG_WARN(QOS_LOG_DOM,
                        "qos_tenants.json 오염: '%s' min_mbps/max_mbps 누락 또는 "
                        "정수 아님 — 파일 전체 폐기(부분 로드 금지)", tenant);
                    shape_ok = FALSE;
                    break;
                }
                gint64 min_v = json_node_get_int(minn);
                gint64 max_v = json_node_get_int(maxn);
                if (max_v <= 0 || min_v < 0 || min_v > max_v || max_v > (gint64)G_MAXUINT32) {
                    PCV_LOG_WARN(QOS_LOG_DOM,
                        "qos_tenants.json 오염: '%s' min=%" G_GINT64_FORMAT " max=%"
                        G_GINT64_FORMAT " 가 유효 범위 밖(0<=min<=max<=UINT32_MAX, max>0) — "
                        "파일 전체 폐기(부분 로드 금지)", tenant, min_v, max_v);
                    shape_ok = FALSE;
                    break;
                }
            }

            if (shape_ok) {
                for (GList *it = keys; it; it = it->next) {
                    const char *tenant = it->data;
                    JsonObject *to = json_node_get_object(json_object_get_member(o, tenant));
                    PcvQosTenantSla *v = g_new(PcvQosTenantSla, 1);
                    v->min_mbps = (guint32)json_object_get_int_member(to, "min_mbps");
                    v->max_mbps = (guint32)json_object_get_int_member(to, "max_mbps");
                    g_hash_table_insert(g_qos_tenant_sla, g_strdup(tenant), v);
                }
                ok = TRUE;
            }
            g_list_free(keys);
        } else {
            PCV_LOG_WARN(QOS_LOG_DOM,
                "qos_tenants.json 오염: 최상위가 JSON object 아님 — "
                "파일 전체 폐기(부분 로드 금지)");
        }
    } else {
        PCV_LOG_WARN(QOS_LOG_DOM,
            "qos_tenants.json 파싱 실패(JSON 문법 오류) — 파일 전체 폐기(부분 로드 금지): %s",
            path);
    }
    g_object_unref(p);

    if (!ok) g_hash_table_remove_all(g_qos_tenant_sla);
    g_mutex_unlock(&g_qos_tenant_sla_mu);
    return ok;
}

   
                                                                     
                                                    
                                                                   
                                                                               
   
gboolean
pcv_qos_tenant_sla_save(const char *path, GError **error)
{
    g_mutex_lock(&g_qos_tenant_sla_mu);
    _ensure_tenant_sla_table();

                                                                      
    JsonObject *root = json_object_new();
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, g_qos_tenant_sla);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        PcvQosTenantSla *sla = v;
        JsonObject *to = json_object_new();
        json_object_set_int_member(to, "min_mbps", (gint64)sla->min_mbps);
        json_object_set_int_member(to, "max_mbps", (gint64)sla->max_mbps);
        json_object_set_object_member(root, (const char *)k, to);
    }

    JsonNode *rootn = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(rootn, root);
    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, rootn);
    json_generator_set_pretty(gen, TRUE);
    gsize len = 0;
    gchar *data = json_generator_to_data(gen, &len);
    g_object_unref(gen);
    json_node_free(rootn);

                                                                 
                                                        
    gchar *dir = g_path_get_dirname(path);
    if (g_mkdir_with_parents(dir, 0755) != 0) {
        int saved_errno = errno;
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                    "qos_tenants 디렉토리 생성 실패: %s (%s)", dir, g_strerror(saved_errno));
        g_free(dir);
        g_free(data);
        g_mutex_unlock(&g_qos_tenant_sla_mu);
        return FALSE;
    }
    g_free(dir);

    gboolean ok = g_file_set_contents(path, data, (gssize)len, error);
    g_free(data);
    g_mutex_unlock(&g_qos_tenant_sla_mu);
    return ok;
}

                                                            
                                                   
void
pcv_qos_tenant_sla_clear(void)
{
    g_mutex_lock(&g_qos_tenant_sla_mu);
    if (g_qos_tenant_sla) g_hash_table_remove_all(g_qos_tenant_sla);
    g_mutex_unlock(&g_qos_tenant_sla_mu);
}

   
                                                                    
                                                       
                                                      
                                                   
                                                                   
                                                                     
                                                                           
                                                     
                                                             
                                                                     
                                                                        
   
gboolean
pcv_qos_tenant_sla_set(const char *tenant, guint32 min_mbps, guint32 max_mbps,
                       gboolean *live_applied_out, gchar **live_error_out, GError **error)
{
    if (live_applied_out) *live_applied_out = FALSE;
    if (live_error_out)   *live_error_out   = NULL;

    if (!tenant || !*tenant) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "tenant must be non-empty");
        return FALSE;
    }
    if (max_mbps == 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "max_mbps must be > 0");
        return FALSE;
    }
    if (min_mbps > max_mbps) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "min_mbps (%u) > max_mbps (%u)", min_mbps, max_mbps);
        return FALSE;
    }

    g_mutex_lock(&g_qos_tenant_sla_mu);
    _ensure_tenant_sla_table();
    PcvQosTenantSla *v = g_new(PcvQosTenantSla, 1);
    v->min_mbps = min_mbps;
    v->max_mbps = max_mbps;
    g_hash_table_insert(g_qos_tenant_sla, g_strdup(tenant), v);
    g_mutex_unlock(&g_qos_tenant_sla_mu);

                                                            
                                                  
                                             
                                             
                                                    
                                                     
    GError *save_err = NULL;
    if (!pcv_qos_tenant_sla_save(
            g_tenant_sla_path_override ? g_tenant_sla_path_override : PCV_QOS_TENANT_SLA_PATH,
            &save_err)) {
        g_propagate_prefixed_error(error, save_err,
            "tenant SLA는 온메모리에 반영됐으나 durable 저장 실패(재부팅 생존 위험): ");
        return FALSE;
    }

                                                       
                                                                  
                                                            
                                                  
                         
      
                                                       
                                                
                                                    
                                               
                                                             
                                                                 
                       
    g_mutex_lock(&g_qos_tc_mu);
    _ensure_tc_tables();
    gboolean tenant_class_exists = g_hash_table_contains(g_qos_tenant_refcnt, tenant);
    g_mutex_unlock(&g_qos_tc_mu);

    if (tenant_class_exists) {
        gchar *tenant_cid = pcv_qos_tenant_classid(tenant);
        GError *tc_err = NULL;
        gboolean ok = _qos_tenant_class_replace(tenant_cid, min_mbps, max_mbps,
                                                  "tenant SLA live update", &tc_err);
        g_free(tenant_cid);
        if (ok) {
            if (live_applied_out) *live_applied_out = TRUE;
        } else {
            PCV_LOG_WARN(QOS_LOG_DOM,
                "qos.tenant.set: 라이브 갱신 실패(저장은 이미 성공 — [최종리뷰 I-1] "
                "reconcile 은 누락 클래스만 재생성해 이 실패는 복구하지 않음, 복구는 "
                "다음 qos.vm.set 재적용) tenant=%s: %s",
                tenant, tc_err ? tc_err->message : "unknown");
            if (live_error_out)
                *live_error_out = g_strdup(tc_err ? tc_err->message : "tc class replace failed");
            g_clear_error(&tc_err);
        }
    }

    return TRUE;
}

                                                              
                                                                     
void
pcv_qos_uplink_clear(void)
{
    g_mutex_lock(&g_qos_tc_mu);
    g_qos_uplink_mbps = 0;
    g_mutex_unlock(&g_qos_tc_mu);
}

   
                                                                             
                                                       
                                                         
                                  
                                                               
                                                
                                                                         
   
gboolean
pcv_qos_ensure_root(guint32 uplink_mbps, GError **error)
{
    if (uplink_mbps == 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "uplink_mbps must be > 0");
        return FALSE;
    }

    if (!_qos_ensure_ifb_dev(error))
        return FALSE;

    gchar *uplink_rate = g_strdup_printf("%uMbit", uplink_mbps);
    gboolean ok = FALSE;

                                                          
                                      
    {
        const gchar *argv[] = {
            "tc", "qdisc", "replace", "dev", PCV_QOS_IFB_DEV,
            "root", "handle", PCV_QOS_ROOT_HANDLE, "hfsc",
            "default", PCV_QOS_DEFAULT_MINOR, NULL
        };
        if (!_qos_run(argv, "hfsc root qdisc", error)) goto out;
    }

                                                           
    {
        const gchar *argv[] = {
            "tc", "class", "replace", "dev", PCV_QOS_IFB_DEV,
            "parent", PCV_QOS_ROOT_HANDLE, "classid", PCV_QOS_LINKCAP_CLASSID,
            "hfsc", "ls", "m2", uplink_rate, "ul", "m2", uplink_rate, NULL
        };
        if (!_qos_run(argv, "link-cap class 1:1", error)) goto out;
    }

                                                       
                                                      
                                            
                                                
                                                
                             
    {
        const gchar *argv[] = {
            "tc", "class", "replace", "dev", PCV_QOS_IFB_DEV,
            "parent", PCV_QOS_LINKCAP_CLASSID, "classid", PCV_QOS_DEFAULT_CLASSID,
            "hfsc", "ls", "m2", "1Mbit", "ul", "m2", uplink_rate, NULL
        };
        if (!_qos_run(argv, "default class 1:fffe", error)) goto out;
    }

    g_mutex_lock(&g_qos_tc_mu);
    g_qos_uplink_mbps = uplink_mbps;
    g_mutex_unlock(&g_qos_tc_mu);

    PCV_LOG_INFO(QOS_LOG_DOM, "QoS root ensured: %s uplink=%uMbit",
                 PCV_QOS_IFB_DEV, uplink_mbps);
    ok = TRUE;

out:
    g_free(uplink_rate);
    return ok;
}

   
                                                                 
                                                                
                                                  
                                                    
                                            
                                                   
                                                                           
                                    
                                                                        
                                                          
                                                               
   
gboolean
pcv_qos_apply_vm(const char *vm_iface, const PcvQosSla *sla, GError **error)
{
    if (!vm_iface || !*vm_iface || !pcv_validate_iface_name(vm_iface)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "invalid vm_iface: '%s'", vm_iface ? vm_iface : "(null)");
        return FALSE;
    }
    if (!sla || !sla->tenant[0] || !sla->vm[0]) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "sla.tenant/sla.vm must be non-empty");
        return FALSE;
    }
    if (sla->max_mbps == 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "sla.max_mbps must be > 0");
        return FALSE;
    }

    g_mutex_lock(&g_qos_tc_mu);
    guint32 uplink = g_qos_uplink_mbps;
    g_mutex_unlock(&g_qos_tc_mu);
    if (uplink == 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "pcv_qos_ensure_root() must be called before pcv_qos_apply_vm() "
                    "— uplink unknown (pcv_qos.h 계약 참고)");
        return FALSE;
    }

    gchar *tenant_cid = pcv_qos_tenant_classid(sla->tenant);
    gchar *vm_cid     = pcv_qos_classid(sla->tenant, sla->vm);
    guint16 vm_minor  = pcv_qos_vm_minor(sla->tenant, sla->vm);
    gchar *pref   = g_strdup_printf("%u", vm_minor);
    gchar *handle = g_strdup_printf("0x%x", vm_minor);
    gboolean ok = FALSE;

                                                            
                                                                  
                                                      
                                                               
                            
    {
        PcvQosTenantSla tsla;
        guint32 t_min = uplink, t_max = uplink;
        if (pcv_qos_tenant_sla_get(sla->tenant, &tsla)) {
            t_min = tsla.min_mbps;
            t_max = tsla.max_mbps;
        }
        if (!_qos_tenant_class_replace(tenant_cid, t_min, t_max, "tenant interior class", error))
            goto out;
    }

                                                                      
                                                         
                                         
    {
        gchar *max_rate = g_strdup_printf("%uMbit", sla->max_mbps);
        gboolean vok;
        if (sla->min_mbps > 0) {
            gchar *min_rate = g_strdup_printf("%uMbit", sla->min_mbps);
            const gchar *argv[] = {
                "tc", "class", "replace", "dev", PCV_QOS_IFB_DEV,
                "parent", tenant_cid, "classid", vm_cid,
                "hfsc", "sc", "rate", min_rate, "ul", "rate", max_rate, NULL
            };
            vok = _qos_run(argv, "vm leaf class(sc+ul)", error);
            g_free(min_rate);
        } else {
            const gchar *argv[] = {
                "tc", "class", "replace", "dev", PCV_QOS_IFB_DEV,
                "parent", tenant_cid, "classid", vm_cid,
                "hfsc", "ls", "m2", max_rate, "ul", "m2", max_rate, NULL
            };
            vok = _qos_run(argv, "vm leaf class(ls+ul)", error);
        }
        g_free(max_rate);
        if (!vok) goto out;
    }

                                                                 
                                                     
    {
        const gchar *argv[] = {
            "tc", "qdisc", "replace", "dev", PCV_QOS_IFB_DEV,
            "parent", vm_cid, "cake", "besteffort", NULL
        };
        if (!_qos_run(argv, "leaf cake", error)) goto out;
    }

                                                                   
                                                   
                                                        
                                     
    {
        const gchar *ing_argv[] = {
            "tc", "qdisc", "replace", "dev", vm_iface, "ingress", NULL
        };
        if (!_qos_run(ing_argv, "vm iface ingress qdisc", error)) goto out;

        const gchar *del_argv[] = {
            "tc", "filter", "del", "dev", vm_iface, "parent", "ffff:",
            "pref", pref, "handle", handle, "matchall", NULL
        };
        _qos_run_best_effort(del_argv);

        const gchar *add_argv[] = {
            "tc", "filter", "add", "dev", vm_iface, "parent", "ffff:",
            "protocol", "all", "pref", pref, "handle", handle, "matchall",
            "action", "mirred", "egress", "redirect", "dev", PCV_QOS_IFB_DEV, NULL
        };
        if (!_qos_run(add_argv, "vm iface ingress redirect filter", error)) goto out;
    }

                                                           
                                                 
                                                          
                                                   
                                         
    {
        const gchar *argv[] = {
            "tc", "filter", "replace", "dev", PCV_QOS_IFB_DEV,
            "parent", PCV_QOS_ROOT_HANDLE, "protocol", "all",
            "pref", pref, "handle", handle, "flower",
            "indev", vm_iface, "classid", vm_cid, NULL
        };
        if (!_qos_run(argv, "indev classify filter", error)) goto out;
    }

                                                                   
                                                      
                                               
                                                  
                                                         
                                          
    {
        g_mutex_lock(&g_qos_tc_mu);
        _ensure_tc_tables();
        _QosActiveEntry *ent = g_hash_table_lookup(g_qos_active_vms, sla->vm);
        if (!ent) {
            ent = g_new(_QosActiveEntry, 1);
            ent->tenant = g_strdup(sla->tenant);
            ent->iface  = g_strdup(vm_iface);
            g_hash_table_insert(g_qos_active_vms, g_strdup(sla->vm), ent);

            guint cur = GPOINTER_TO_UINT(
                g_hash_table_lookup(g_qos_tenant_refcnt, sla->tenant));
            g_hash_table_insert(g_qos_tenant_refcnt, g_strdup(sla->tenant),
                                GUINT_TO_POINTER(cur + 1));
        } else {
                                                             
                                                           
                                                          
                                                             
                                                              
                                                         
                           
            g_free(ent->tenant);
            ent->tenant = g_strdup(sla->tenant);
            g_free(ent->iface);
            ent->iface = g_strdup(vm_iface);
        }
        g_mutex_unlock(&g_qos_tc_mu);
    }

    PCV_LOG_INFO(QOS_LOG_DOM,
                 "QoS applied: tenant=%s vm=%s iface=%s min=%uMbit max=%uMbit classid=%s",
                 sla->tenant, sla->vm, vm_iface, sla->min_mbps, sla->max_mbps, vm_cid);
    ok = TRUE;

out:
    g_free(pref);
    g_free(handle);
    g_free(tenant_cid);
    g_free(vm_cid);
    return ok;
}

   
                                                                  
                                                         
                                                       
                                                     
                                                                       
                                                      
                                                           
                                                                  
                                                               
                                                         
   
gboolean
pcv_qos_remove_vm(const char *vm_iface, const char *tenant, const char *vm, GError **error)
{
    if (!vm_iface || !*vm_iface || !pcv_validate_iface_name(vm_iface)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "invalid vm_iface: '%s'", vm_iface ? vm_iface : "(null)");
        return FALSE;
    }
    if (!tenant || !*tenant || !vm || !*vm) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "tenant/vm must be non-empty");
        return FALSE;
    }

    gchar *vm_cid    = pcv_qos_classid(tenant, vm);
    guint16 vm_minor = pcv_qos_vm_minor(tenant, vm);
    gchar *pref      = g_strdup_printf("%u", vm_minor);
    gchar *handle    = g_strdup_printf("0x%x", vm_minor);

                                                       
                                                            
                                                   
                                               
                                                        
    {
        const gchar *argv[] = {
            "tc", "filter", "del", "dev", PCV_QOS_IFB_DEV, "parent", PCV_QOS_ROOT_HANDLE,
            "pref", pref, "handle", handle, "flower", NULL
        };
        _qos_run_best_effort(argv);
    }
    {
        const gchar *argv[] = {
            "tc", "qdisc", "del", "dev", PCV_QOS_IFB_DEV, "parent", vm_cid, NULL
        };
        _qos_run_best_effort(argv);
    }
    {
        const gchar *argv[] = {
            "tc", "class", "del", "dev", PCV_QOS_IFB_DEV, "classid", vm_cid, NULL
        };
        _qos_run_best_effort(argv);
    }
    {
        const gchar *argv[] = {"tc", "qdisc", "del", "dev", vm_iface, "ingress", NULL};
        _qos_run_best_effort(argv);
    }

    g_free(pref);
    g_free(handle);

                                                   
                                                     
                                                    
    gboolean tenant_now_empty = FALSE;
    g_mutex_lock(&g_qos_tc_mu);
    _ensure_tc_tables();
    if (g_hash_table_contains(g_qos_active_vms, vm)) {
        g_hash_table_remove(g_qos_active_vms, vm);                                               
        guint cur = GPOINTER_TO_UINT(g_hash_table_lookup(g_qos_tenant_refcnt, tenant));
        if (cur > 0) cur--;
        if (cur == 0) {
            g_hash_table_remove(g_qos_tenant_refcnt, tenant);
            tenant_now_empty = TRUE;
        } else {
            g_hash_table_insert(g_qos_tenant_refcnt, g_strdup(tenant), GUINT_TO_POINTER(cur));
        }
    }
    g_mutex_unlock(&g_qos_tc_mu);

    if (tenant_now_empty) {
        gchar *tenant_cid = pcv_qos_tenant_classid(tenant);
        const gchar *argv[] = {
            "tc", "class", "del", "dev", PCV_QOS_IFB_DEV, "classid", tenant_cid, NULL
        };
        _qos_run_best_effort(argv);
        PCV_LOG_INFO(QOS_LOG_DOM, "QoS tenant interior class removed(last VM): tenant=%s",
                     tenant);
        g_free(tenant_cid);
    }

                                                          
                                         
                                                      
                                                  
                                                       
                          
    {
        gchar *class_out = _qos_class_show();
        gboolean still_present = _qos_class_output_has_classid(class_out, vm_cid);
        g_free(class_out);

        if (still_present) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "remove_vm 사후조건 실패 — classid %s 가 %s 에 여전히 존재 "
                        "(refcount 정리는 이미 반영됨 — 커널 잔재는 D09 T3 reconcile 의 "
                        "고아 정리 대상)", vm_cid, PCV_QOS_IFB_DEV);
            PCV_LOG_WARN(QOS_LOG_DOM,
                         "QoS removed 사후조건 실패: tenant=%s vm=%s classid=%s 가 잔존",
                         tenant, vm, vm_cid);
            g_free(vm_cid);
            return FALSE;
        }
    }

    g_free(vm_cid);
    PCV_LOG_INFO(QOS_LOG_DOM, "QoS removed: tenant=%s vm=%s iface=%s", tenant, vm, vm_iface);
    return TRUE;
}

   
                          
                                                       
                                                     
                                             
                                                                      
                                                                          
  
                                                             
                                                          
                                                          
                                                 
                                                        
              
  
                                                           
                                                              
                                                        
                                                        
                                                     
                                                  
                                                  
                
  
                                                                    
   
gboolean
pcv_qos_lookup_applied(const char *vm, gchar **tenant_out, gchar **iface_out)
{
    if (tenant_out) *tenant_out = NULL;
    if (iface_out)  *iface_out  = NULL;
    if (!vm) return FALSE;

    g_mutex_lock(&g_qos_tc_mu);
    _ensure_tc_tables();
    _QosActiveEntry *ent = g_hash_table_lookup(g_qos_active_vms, vm);
    if (ent) {
        if (tenant_out) *tenant_out = g_strdup(ent->tenant);
        if (iface_out)  *iface_out  = g_strdup(ent->iface);
    }
    g_mutex_unlock(&g_qos_tc_mu);
    return ent != NULL;
}

   
                                                                  
                                                     
                                     
                                                                   
                                                             
   
gboolean
pcv_qos_iface_is_managed(const char *iface)
{
    if (!iface || !*iface) return FALSE;

    gboolean found = FALSE;
    g_mutex_lock(&g_qos_tc_mu);
    _ensure_tc_tables();
    GHashTableIter it;
    gpointer v;
    g_hash_table_iter_init(&it, g_qos_active_vms);
    while (g_hash_table_iter_next(&it, NULL, &v)) {
        const _QosActiveEntry *e = v;
        if (g_strcmp0(e->iface, iface) == 0) {
            found = TRUE;
            break;
        }
    }
    g_mutex_unlock(&g_qos_tc_mu);
    return found;
}

                                                                             
                                                      
  
                                                  
                                                                                

                                                            
static void
_qos_recon_entry_free(gpointer p)
{
    PcvQosReconEntry *e = p;
    g_free(e->classid);
    g_free(e);
}

                                                                         
static void
_qos_recon_diff_add(GPtrArray *diff, const gchar *classid, PcvQosReconItem action)
{
    PcvQosReconEntry *e = g_new(PcvQosReconEntry, 1);
    e->classid = g_strdup(classid);
    e->action  = action;
    g_ptr_array_add(diff, e);
}

   
                                                                         
                                                  
                                                        
                              
                                                          
                                                          
                                                                           
                                                  
                                                            
   
GPtrArray *
pcv_qos_reconcile_diff(GPtrArray *expected_ids, GPtrArray *actual_ids)
{
    GPtrArray *diff = g_ptr_array_new_with_free_func(_qos_recon_entry_free);

                                                               
                                               
    GHashTable *actual_set = g_hash_table_new(g_str_hash, g_str_equal);
    if (actual_ids)
        for (guint i = 0; i < actual_ids->len; i++)
            g_hash_table_add(actual_set, g_ptr_array_index(actual_ids, i));

    GHashTable *expected_set = g_hash_table_new(g_str_hash, g_str_equal);
    if (expected_ids)
        for (guint i = 0; i < expected_ids->len; i++)
            g_hash_table_add(expected_set, g_ptr_array_index(expected_ids, i));

    if (expected_ids) {
        for (guint i = 0; i < expected_ids->len; i++) {
            const gchar *id = g_ptr_array_index(expected_ids, i);
            PcvQosReconItem action = g_hash_table_contains(actual_set, id)
                ? PCV_QOS_RECON_OK : PCV_QOS_RECON_MISSING;
            _qos_recon_diff_add(diff, id, action);
        }
    }
    if (actual_ids) {
        for (guint i = 0; i < actual_ids->len; i++) {
            const gchar *id = g_ptr_array_index(actual_ids, i);
            if (!g_hash_table_contains(expected_set, id))
                _qos_recon_diff_add(diff, id, PCV_QOS_RECON_ORPHAN);
        }
    }

    g_hash_table_unref(actual_set);
    g_hash_table_unref(expected_set);
    return diff;
}

   
                                                                                      
                                                          
                                                
                                                                               
                                                                 
                                                                
   
GPtrArray *
pcv_qos_parse_class_show(const gchar *output)
{
    GPtrArray *ids = g_ptr_array_new_with_free_func(g_free);
    if (!output || !*output)
        return ids;

                                                               
                                                           
                                              
    gchar **lines = g_strsplit(output, "\n", -1);
    for (guint li = 0; lines[li]; li++) {
        gchar **raw = g_strsplit_set(lines[li], " \t", -1);
        const gchar *tok[3] = { NULL, NULL, NULL };
        guint n = 0;
        for (guint ti = 0; raw[ti] && n < 3; ti++)
            if (*raw[ti]) tok[n++] = raw[ti];

                                                                 
                                                       
                                      
        if (n == 3 && g_strcmp0(tok[0], "class") == 0 && g_str_has_prefix(tok[2], "1:")) {
            const gchar *minor_str = tok[2] + 2;
            if (*minor_str != '\0') {
                gchar *end = NULL;
                guint64 minor = g_ascii_strtoull(minor_str, &end, 16);
                if (end && *end == '\0' &&
                    minor >= PCV_QOS_VM_MIN && minor <= PCV_QOS_VM_MAX)
                    g_ptr_array_add(ids, g_strdup(tok[2]));
            }
        }
        g_strfreev(raw);
    }
    g_strfreev(lines);
    return ids;
}

                                                                  
                                                    
                                            
                                                            
static GPtrArray *
_qos_actual_ids(void)
{
    gchar *out = _qos_class_show();
    GPtrArray *ids = pcv_qos_parse_class_show(out);
    g_free(out);
    return ids;
}

                                                                
                                                   
          
static GMutex                 g_qos_recon_mu;
static PcvQosExpectedProvider g_qos_expected_provider = NULL;

                                                                         
                                                       
                                                           
void
pcv_qos_set_expected_provider(PcvQosExpectedProvider fn)
{
    g_mutex_lock(&g_qos_recon_mu);
    g_qos_expected_provider = fn;
    g_mutex_unlock(&g_qos_recon_mu);
}

   
                                                               
                                                          
                                                      
                                          
                                                            
                                                               
                                               
                                                          
                                                           
   
gboolean
pcv_qos_reconcile(GError **error)
{
    g_mutex_lock(&g_qos_recon_mu);
    PcvQosExpectedProvider provider = g_qos_expected_provider;
    g_mutex_unlock(&g_qos_recon_mu);

    if (!provider) {
                                                      
                                                  
                                                   
        PCV_LOG_WARN(QOS_LOG_DOM,
            "reconcile: expected-provider 미설정 — no-op(부팅 배선 전 안전측, "
            "T5 가 pcv_qos_set_expected_provider() 로 배선 예정)");
        return TRUE;
    }

    GPtrArray *expected_entries = provider();
    if (!expected_entries)
        expected_entries = g_ptr_array_new();                           

                                                                
                                                                    
                     
    GHashTable *by_classid = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (guint i = 0; i < expected_entries->len; i++) {
        PcvQosExpectedEntry *e = g_ptr_array_index(expected_entries, i);
        gchar *cid = pcv_qos_classid(e->tenant, e->vm);
        g_hash_table_insert(by_classid, cid, e);
    }
    GPtrArray *expected_ids = g_ptr_array_new();
    {
        GHashTableIter it;
        gpointer k, v;
        g_hash_table_iter_init(&it, by_classid);
        while (g_hash_table_iter_next(&it, &k, &v))
            g_ptr_array_add(expected_ids, k);
    }

    GPtrArray *actual_ids = _qos_actual_ids();
    GPtrArray *diff = pcv_qos_reconcile_diff(expected_ids, actual_ids);

    guint failures = 0;
    for (guint i = 0; i < diff->len; i++) {
        PcvQosReconEntry *ent = g_ptr_array_index(diff, i);
        switch (ent->action) {
        case PCV_QOS_RECON_OK:
            break;

        case PCV_QOS_RECON_ORPHAN: {
                                                     
                                                                  
                                                       
                                                  
                                                                   
                                                          
                                                               
                                                 
                                                          
                                                          
                                                    
                                                     
                                                        
                                                          
                                                              
                                                     
            guint32 minor = 0;
            if (g_str_has_prefix(ent->classid, "1:"))
                minor = (guint32)g_ascii_strtoull(ent->classid + 2, NULL, 16);
            gchar *pref   = g_strdup_printf("%u", minor);
            gchar *handle = g_strdup_printf("0x%x", minor);
            const gchar *filter_argv[] = {
                "tc", "filter", "del", "dev", PCV_QOS_IFB_DEV, "parent", PCV_QOS_ROOT_HANDLE,
                "pref", pref, "handle", handle, "flower", NULL
            };
            _qos_run_best_effort(filter_argv);
            g_free(pref);
            g_free(handle);

            const gchar *cake_argv[] = {
                "tc", "qdisc", "del", "dev", PCV_QOS_IFB_DEV,
                "parent", ent->classid, NULL
            };
            _qos_run_best_effort(cake_argv);

            const gchar *class_argv[] = {
                "tc", "class", "del", "dev", PCV_QOS_IFB_DEV,
                "classid", ent->classid, NULL
            };
            GError *sub = NULL;
            if (!_qos_run(class_argv, "reconcile: orphan class del", &sub)) {
                PCV_LOG_WARN(QOS_LOG_DOM, "reconcile: 고아 classid %s 제거 실패: %s",
                             ent->classid, sub ? sub->message : "unknown");
                g_clear_error(&sub);
                failures++;
            } else {
                PCV_LOG_INFO(QOS_LOG_DOM, "reconcile: 고아 classid %s 제거", ent->classid);
            }
            break;
        }

        case PCV_QOS_RECON_MISSING: {
            PcvQosExpectedEntry *e = g_hash_table_lookup(by_classid, ent->classid);
            if (!e) {
                                                             
                                                       
                PCV_LOG_WARN(QOS_LOG_DOM,
                    "reconcile: MISSING classid %s 에 대응하는 expected entry 없음"
                    "(내부 불일치) — skip", ent->classid);
                failures++;
                break;
            }
            GError *sub = NULL;
            if (!pcv_qos_apply_vm(e->iface, &e->sla, &sub)) {
                PCV_LOG_WARN(QOS_LOG_DOM,
                    "reconcile: 누락 VM 재적용 실패 tenant=%s vm=%s: %s",
                    e->tenant, e->vm, sub ? sub->message : "unknown");
                g_clear_error(&sub);
                failures++;
            } else {
                PCV_LOG_INFO(QOS_LOG_DOM,
                    "reconcile: 누락 VM 재적용 tenant=%s vm=%s classid=%s",
                    e->tenant, e->vm, ent->classid);
            }
            break;
        }
        }
    }

    g_ptr_array_unref(diff);
    g_ptr_array_unref(actual_ids);
    g_ptr_array_unref(expected_ids);                                      
    g_hash_table_unref(by_classid);
    g_ptr_array_unref(expected_entries);                                                  

    if (failures > 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "reconcile: %u건 정리/재적용 실패(다음 주기 재시도)", failures);
        return FALSE;
    }
    return TRUE;
}

                                                                        
                                                            
                                                     
                                                      
                                              
static gint g_qos_reconcile_inflight = 0;                        

static void
_qos_reconcile_worker(GTask *task, gpointer src, gpointer td, GCancellable *c)
{
    (void)src; (void)td; (void)c;
    GError *err = NULL;
    if (!pcv_qos_reconcile(&err)) {
        PCV_LOG_WARN(QOS_LOG_DOM, "reconcile 실패(다음 주기 재시도): %s",
                     err ? err->message : "unknown");
        g_clear_error(&err);
    }
    g_atomic_int_set(&g_qos_reconcile_inflight, 0);
    g_task_return_boolean(task, TRUE);
}

                                                            
            
                                                     
                                           
static gboolean
_qos_reconcile_tick(gpointer data)
{
    (void)data;
    if (!g_atomic_int_compare_and_exchange(&g_qos_reconcile_inflight, 0, 1))
        return G_SOURCE_CONTINUE;                                    
    GTask *t = g_task_new(NULL, NULL, NULL, NULL);
    pcv_worker_pool_push(t, _qos_reconcile_worker);
    g_object_unref(t);                                  
    return G_SOURCE_CONTINUE;
}

   
                                                           
                                                       
                                                                         
                                                    
   
guint
pcv_qos_reconcile_timer_start(void)
{
    gint interval = pcv_config_get_int("qos", "reconcile_interval_sec", 300);
    if (interval <= 0) {
        PCV_LOG_INFO(QOS_LOG_DOM, "reconcile 타이머 비활성 (reconcile_interval_sec=%d)", interval);
        return 0;
    }
    guint id = g_timeout_add_seconds((guint)interval, _qos_reconcile_tick, NULL);
    PCV_LOG_INFO(QOS_LOG_DOM, "reconcile 타이머 등록 (%d초 주기)", interval);
    return id;
}

                                                                             
                                                                             
                                                                                

   
                                                                       
                                                       
                                          
                                   
                                                                                
                                                                               
                                                              
                                                                     
   
gboolean
pcv_qos_reverse_lookup_vm(guint16 minor, gchar **tenant_out, gchar **vm_out)
{
    if (tenant_out) *tenant_out = NULL;
    if (vm_out)     *vm_out     = NULL;

    gboolean found = FALSE;
    g_mutex_lock(&g_qos_ids_mu);
    _ensure_tables();
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, g_vm_fwd);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        if ((guint16)GPOINTER_TO_UINT(v) == minor) {
            const gchar *key = k;                                                   
            const gchar *slash = strchr(key, '/');
            if (slash) {
                if (tenant_out) *tenant_out = g_strndup(key, (gsize)(slash - key));
                if (vm_out)     *vm_out     = g_strdup(slash + 1);
                found = TRUE;
            }
            break;
        }
    }
    g_mutex_unlock(&g_qos_ids_mu);
    return found;
}

                                                                               
                          
static void
_qos_class_stat_free(gpointer p)
{
    PcvQosClassStat *s = p;
    if (!s) return;
    g_free(s->classid);
    g_free(s->tenant);
    g_free(s->vm);
    g_free(s);
}

   
                                                                                     
                                                       
                                                                     
                                                             
                                                                                
                                                               
                                                           
                                     
   
GPtrArray *
pcv_qos_parse_class_stats(const gchar *output)
{
    GPtrArray *stats = g_ptr_array_new_with_free_func(_qos_class_stat_free);
    if (!output || !*output)
        return stats;

                                                                  
               
                                                                              
                                                                      
                                  
                                                               
                                                                   
                                                            
                                                                 
                                                              
                   
    gchar *cur_classid = NULL;
    gchar **lines = g_strsplit(output, "\n", -1);
    for (guint li = 0; lines[li]; li++) {
        gchar **raw = g_strsplit_set(lines[li], " \t", -1);
        const gchar *tok[16] = { NULL };
        guint n = 0;
        for (guint ti = 0; raw[ti] && n < 16; ti++)
            if (*raw[ti]) tok[n++] = raw[ti];

        if (n >= 3 && g_strcmp0(tok[0], "class") == 0) {
            g_free(cur_classid);
            cur_classid = NULL;
            if (g_str_has_prefix(tok[2], "1:")) {
                const gchar *minor_str = tok[2] + 2;
                if (*minor_str != '\0') {
                    gchar *end = NULL;
                    guint64 minor = g_ascii_strtoull(minor_str, &end, 16);
                    if (end && *end == '\0' &&
                        minor >= PCV_QOS_VM_MIN && minor <= PCV_QOS_VM_MAX)
                        cur_classid = g_strdup(tok[2]);
                }
            }
        } else if (cur_classid && n >= 2 && g_strcmp0(tok[0], "Sent") == 0) {
            guint64 bytes = g_ascii_strtoull(tok[1], NULL, 10);
            guint64 drops = 0;
            for (guint ti = 0; ti < n; ti++) {
                if (g_str_has_prefix(tok[ti], "(dropped") && ti + 1 < n) {
                    drops = g_ascii_strtoull(tok[ti + 1], NULL, 10);
                    break;
                }
            }
            PcvQosClassStat *s = g_new0(PcvQosClassStat, 1);
            s->classid = cur_classid;                 
            s->bytes = bytes;
            s->drops = drops;
            g_ptr_array_add(stats, s);
            cur_classid = NULL;                                       
                                                              
        }

        g_strfreev(raw);
    }
    g_free(cur_classid);
    g_strfreev(lines);
    return stats;
}

   
                                                                              
                                                      
                                         
                                                                            
                                                                      
   
GPtrArray *
pcv_qos_collect_stats(void)
{
    gchar *out = _qos_class_show_stats();
    GPtrArray *stats = pcv_qos_parse_class_stats(out);
    g_free(out);

    for (guint i = 0; i < stats->len; i++) {
        PcvQosClassStat *s = g_ptr_array_index(stats, i);
        if (!g_str_has_prefix(s->classid, "1:")) continue;
        guint64 minor = g_ascii_strtoull(s->classid + 2, NULL, 16);
        gchar *tenant = NULL, *vm = NULL;
        if (pcv_qos_reverse_lookup_vm((guint16)minor, &tenant, &vm)) {
            s->tenant = tenant;
            s->vm = vm;
        }
                                                         
                                                             
    }
    return stats;
}

                                                             
                                                            
                                                       
void
pcv_qos_metrics_tick(void)
{
    GPtrArray *stats = pcv_qos_collect_stats();
    for (guint i = 0; i < stats->len; i++) {
        PcvQosClassStat *s = g_ptr_array_index(stats, i);
        if (!s->tenant || !s->vm) {
            PCV_LOG_WARN(QOS_LOG_DOM,
                "metrics_tick: classid %s 역매핑 실패(추적되지 않는 고아 classid) — "
                "메트릭 skip(다음 reconcile 이 정리하면 사라짐)", s->classid);
            continue;
        }
                                                                
                                                            
        gchar lbl[256];
        g_snprintf(lbl, sizeof(lbl), "tenant=\"%s\",vm=\"%s\",class=\"%s\"",
                   s->tenant, s->vm, s->classid);
        pcv_prom_gauge_set_labels("purecvisor_qos_class_bytes_total", lbl, (gdouble)s->bytes);
        pcv_prom_gauge_set_labels("purecvisor_qos_class_drops_total", lbl, (gdouble)s->drops);
    }
    g_ptr_array_unref(stats);
}

                                                                    
                                               
                                                                    
static gint g_qos_metrics_inflight = 0;

                                                             
                                                   
                                                     
                                        
                                              
                                                                
                                                               
                                                      
                     
                                                
                                                      
                               
static void
_qos_metrics_worker(GTask *task, gpointer src, gpointer td, GCancellable *c)
{
    (void)src; (void)td; (void)c;
    pcv_qos_metrics_tick();
    g_atomic_int_set(&g_qos_metrics_inflight, 0);
    g_task_return_boolean(task, TRUE);
}

                                                    
                                                
                                             
                                                
                                              
                                                                  
                                                     
                                                     
                                                      
                                                                    
static gboolean
_qos_metrics_tick_cb(gpointer data)
{
    (void)data;
    if (!g_atomic_int_compare_and_exchange(&g_qos_metrics_inflight, 0, 1))
        return G_SOURCE_CONTINUE;                               
    GTask *t = g_task_new(NULL, NULL, NULL, NULL);
    pcv_worker_pool_push(t, _qos_metrics_worker);
    g_object_unref(t);
    return G_SOURCE_CONTINUE;
}

   
                                                          
                                                       
                                                                  
                                                                             
                                                         
   
guint
pcv_qos_metrics_timer_start(void)
{
    gint interval = pcv_config_get_int("qos", "metrics_interval_sec", 30);
    if (interval <= 0) {
        PCV_LOG_INFO(QOS_LOG_DOM, "metrics 타이머 비활성 (metrics_interval_sec=%d)", interval);
        return 0;
    }
    guint id = g_timeout_add_seconds((guint)interval, _qos_metrics_tick_cb, NULL);
    PCV_LOG_INFO(QOS_LOG_DOM, "metrics 타이머 등록 (%d초 주기)", interval);
    return id;
}
