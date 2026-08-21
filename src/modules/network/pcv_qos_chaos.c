                                      
                                                                                          
  
                           
                                                   
                                                    
                                        
  
          
                                                   
                                                  
                                                  
                                                  
                                                     
                                          
  
                                             
                                   
  
                                    
                                                           
                                                         
                                                                         
                                                           
                                               
                                             
  
                                                       
                                                             
                                                            
                                              
                                                                            
                                                                          
                                                                  
                                                 
                                                   
                                                   
                                             
                                              
                 
   
#include "modules/network/pcv_qos_chaos.h"
#include "modules/network/pcv_qos.h"
#include "modules/audit/pcv_audit.h"
#include "utils/pcv_log.h"
#include "utils/pcv_spawn.h"
#include <string.h>

#define QOS_CHAOS_LOG_DOM "qos_chaos"

                                                               
                                                            
                                                    
                
                                                        
                                                                 
typedef struct {
    gchar  *profile;
    gchar  *admin;
    guint   timebox_sec;
    gint64  expires_at;                                     
    guint   timer_id;                                           
} _ChaosEntry;

static GMutex      g_chaos_mu;
static GHashTable *g_chaos_active = NULL;

                                                        
  
                                                     
                                      
                                                                  
                                                          
                                           
                                             
                                     
  
                                                
                                                  
                                              
                                                      
                                                
                                        
  
                                                 
                                      
static void
_chaos_entry_free(gpointer p)
{
    _ChaosEntry *e = p;
    if (!e) return;
    g_free(e->profile);
    g_free(e->admin);
    g_free(e);
}

                                                        
                                   
static void
_ensure_chaos_table(void)
{
    if (!g_chaos_active)
        g_chaos_active = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                g_free, _chaos_entry_free);
}

                                                                             
                                
                                                                                

                                                                   
                                                          
gboolean
pcv_qos_chaos_timebox_valid(guint timebox_sec)
{
    return timebox_sec >= 1 && timebox_sec <= PCV_QOS_CHAOS_MAX_SEC;
}

                                                   
                                                   
                                      
                                                             
static gboolean
_all_ascii_digits(const gchar *s)
{
    if (!s || !*s) return FALSE;
    for (const gchar *p = s; *p; p++)
        if (!g_ascii_isdigit(*p)) return FALSE;
    return TRUE;
}

                                                      
                                                             
                                       
                                                 
static gboolean
_parse_bounded_uint(const gchar *s, guint32 lo, guint32 hi, guint32 *out)
{
    if (!_all_ascii_digits(s) || strlen(s) > 10) return FALSE;
    guint64 v = g_ascii_strtoull(s, NULL, 10);
    if (v < lo || v > hi) return FALSE;
    *out = (guint32)v;
    return TRUE;
}

                                                                 
                                                                               
                                                        
                                                
                                                                                 
gchar **
pcv_qos_chaos_profile_validate(const gchar *profile, GError **error)
{
    if (!profile || !*profile) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "profile must be non-empty");
        return NULL;
    }
                                                          
                                             
                                                 
    if (strlen(profile) > 64) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "profile too long (max 64 chars)");
        return NULL;
    }

                                              
                                              
                                             
                                       
    gchar **tok = g_strsplit(profile, " ", -1);
    guint n = g_strv_length(tok);
    for (guint i = 0; i < n; i++) {
        if (!*tok[i]) {
            g_strfreev(tok);
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "profile malformed: leading/trailing/consecutive spaces");
            return NULL;
        }
    }

    gchar **result = NULL;

                                                                   
    if (n == 2 && g_strcmp0(tok[0], "delay") == 0 && g_str_has_suffix(tok[1], "ms")) {
        gchar *num = g_strndup(tok[1], strlen(tok[1]) - 2);                     
        guint32 ms;
        if (_parse_bounded_uint(num, 1, 10000, &ms)) {
            result = g_new0(gchar *, 3);
            result[0] = g_strdup("delay");
            result[1] = g_strdup_printf("%ums", ms);
        }
        g_free(num);
                                                           
    } else if (n == 2 && g_strcmp0(tok[0], "loss") == 0 && g_str_has_suffix(tok[1], "%")) {
        gchar *num = g_strndup(tok[1], strlen(tok[1]) - 1);                    
        guint32 pct;
        if (_parse_bounded_uint(num, 1, 100, &pct)) {
            result = g_new0(gchar *, 3);
            result[0] = g_strdup("loss");
            result[1] = g_strdup_printf("%u%%", pct);
        }
        g_free(num);
                                                                                     
    } else if (n == 3 && g_strcmp0(tok[0], "reorder") == 0 &&
               g_str_has_suffix(tok[1], "%") && g_str_has_suffix(tok[2], "%")) {
        gchar *num1 = g_strndup(tok[1], strlen(tok[1]) - 1);
        gchar *num2 = g_strndup(tok[2], strlen(tok[2]) - 1);
        guint32 p, c;
        if (_parse_bounded_uint(num1, 1, 100, &p) && _parse_bounded_uint(num2, 1, 100, &c)) {
            result = g_new0(gchar *, 4);
            result[0] = g_strdup("reorder");
            result[1] = g_strdup_printf("%u%%", p);
            result[2] = g_strdup_printf("%u%%", c);
        }
        g_free(num1);
        g_free(num2);
    }

    g_strfreev(tok);

    if (!result) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "profile '%s' does not match whitelist "
                    "(delay <1-10000>ms | loss <1-100>%% | reorder <1-100>%% <1-100>%%)",
                    profile);
        return NULL;
    }
    return result;
}

                                                                        
                                                      
                                                  
                                                           
gboolean
pcv_qos_chaos_resolve_dry_run(JsonObject *params)
{
    if (!params) return TRUE;
    if (!json_object_has_member(params, "dry_run")) return TRUE;

    JsonNode *n = json_object_get_member(params, "dry_run");
    if (!n || !JSON_NODE_HOLDS_VALUE(n) ||
        json_node_get_value_type(n) != G_TYPE_BOOLEAN)
        return TRUE;                                         

    return json_node_get_boolean(n) ? TRUE : FALSE;
}

                                                                                
                                           
                                                                
                                                                                      
GPtrArray *
pcv_qos_chaos_parse_netem_parents(const gchar *tc_qdisc_show_output)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    if (!tc_qdisc_show_output || !*tc_qdisc_show_output) return out;

    gchar **lines = g_strsplit(tc_qdisc_show_output, "\n", -1);
    for (guint i = 0; lines[i]; i++) {
        gchar **raw = g_strsplit_set(lines[i], " \t", -1);
        GPtrArray *compact = g_ptr_array_new();                                   
                                                        
        for (guint j = 0; raw[j]; j++)
            if (*raw[j]) g_ptr_array_add(compact, raw[j]);

                                                                                
                                                                             
        if (compact->len >= 2 &&
            g_strcmp0(g_ptr_array_index(compact, 0), "qdisc") == 0 &&
            g_strcmp0(g_ptr_array_index(compact, 1), "netem") == 0) {
            for (guint j = 2; j + 1 < compact->len; j++) {
                if (g_strcmp0(g_ptr_array_index(compact, j), "parent") == 0) {
                    g_ptr_array_add(out, g_strdup(g_ptr_array_index(compact, j + 1)));
                    break;
                }
            }
        }
        g_ptr_array_unref(compact);
        g_strfreev(raw);
    }
    g_strfreev(lines);
    return out;
}

                                                                             
                  
                                                                                

                                                             
                                                      
                                                            
static gboolean
_chaos_restore_cake(const gchar *vm)
{
    gchar *tenant = NULL, *iface = NULL;
    if (!pcv_qos_lookup_applied(vm, &tenant, &iface)) {
        PCV_LOG_INFO(QOS_CHAOS_LOG_DOM,
            "vm=%s 더 이상 QoS 추적 대상 아님 — cake 복원 생략(VM 제거 등으로 "
            "leaf 자체가 이미 없을 가능성)", vm);
        return TRUE;
    }
    gchar *vm_cid = pcv_qos_classid(tenant, vm);
    g_free(tenant);
    g_free(iface);

    const gchar *argv[] = {
        "tc", "qdisc", "replace", "dev", PCV_QOS_IFB_DEV,
        "parent", vm_cid, "cake", "besteffort", NULL
    };
    GError *err = NULL;
    gboolean ok = pcv_spawn_sync(argv, NULL, NULL, &err);
    if (!ok) {
        PCV_LOG_WARN(QOS_CHAOS_LOG_DOM,
            "cake 복원 실패 vm=%s classid=%s: %s",
            vm, vm_cid, err && err->message ? err->message : "unknown");
        g_clear_error(&err);
    }
    g_free(vm_cid);
    return ok;
}

                                                        
                                                  
                                                           
                                               
                                                                          
                                                
                                                                  
                                
  
                                                                        
                                                        
                                             
                                                
                                                      
                                                
gboolean
_chaos_expire_cb(gpointer data)
{
    const gchar *vm = (const gchar *)data;

    gchar *admin_copy = NULL;
    gboolean had_entry = FALSE;

    g_mutex_lock(&g_chaos_mu);
    _ensure_chaos_table();
    _ChaosEntry *e = g_hash_table_lookup(g_chaos_active, vm);
    if (e) {
        admin_copy = g_strdup(e->admin);
        had_entry = TRUE;
        g_hash_table_remove(g_chaos_active, vm);                        
    }
    g_mutex_unlock(&g_chaos_mu);

    if (!had_entry) {
                                                    
        return G_SOURCE_REMOVE;
    }

    gboolean restore_ok = _chaos_restore_cake(vm);
    pcv_audit_log(admin_copy, "qos.chaos.expire", vm,
                  restore_ok ? "ok" : "restore_failed",
                  restore_ok ? 0 : -1, 0, "local");
    g_free(admin_copy);

    return G_SOURCE_REMOVE;                     
}

                                                               
                                                         
                                                         
                                                                          
                                           
gboolean
pcv_qos_chaos_start(const char *vm, const char *profile, guint timebox_sec,
                     const char *admin, gboolean dry_run, GError **error)
{
    if (!vm || !*vm) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "vm must be non-empty");
        pcv_audit_log(admin, "qos.chaos.start", vm ? vm : "", "fail", -1, 0, "local");
        return FALSE;
    }

    if (!pcv_qos_chaos_timebox_valid(timebox_sec)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "timebox_sec must be 1..%u (got %u)", PCV_QOS_CHAOS_MAX_SEC, timebox_sec);
        pcv_audit_log(admin, "qos.chaos.start", vm, "fail", -1, 0, "local");
        return FALSE;
    }

    GError *perr = NULL;
    gchar **tokens = pcv_qos_chaos_profile_validate(profile, &perr);
    if (!tokens) {
        g_propagate_error(error, perr);
        pcv_audit_log(admin, "qos.chaos.start", vm, "fail", -1, 0, "local");
        return FALSE;
    }

    gchar *tenant = NULL, *iface = NULL;
    if (!pcv_qos_lookup_applied(vm, &tenant, &iface)) {
        g_strfreev(tokens);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "vm '%s' is not QoS-tracked (chaos requires an applied VM — "
                    "not QoS-tracked)", vm);
        pcv_audit_log(admin, "qos.chaos.start", vm, "fail", -1, 0, "local");
        return FALSE;
    }
    gchar *vm_cid = pcv_qos_classid(tenant, vm);
    g_free(tenant);
    g_free(iface);

                                                      
                                                  
                       
    g_mutex_lock(&g_chaos_mu);
    _ensure_chaos_table();
    gboolean already_active = g_hash_table_contains(g_chaos_active, vm);
    g_mutex_unlock(&g_chaos_mu);

    if (already_active && !dry_run) {
        g_strfreev(tokens);
        g_free(vm_cid);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                    "chaos already active for vm '%s' — stop first", vm);
        pcv_audit_log(admin, "qos.chaos.start", vm, "fail", -1, 0, "local");
        return FALSE;
    }

    if (dry_run) {
        gchar *detail = g_strdup_printf(
            "ok dry_run=true profile=\"%s\" timebox_sec=%u classid=%s",
            profile, timebox_sec, vm_cid);
        pcv_audit_log(admin, "qos.chaos.start", vm, detail, 0, 0, "local");
        g_free(detail);
        g_strfreev(tokens);
        g_free(vm_cid);
        return TRUE;
    }

                                                                           
                                                                               
                                            
    GPtrArray *argv = g_ptr_array_new();
    g_ptr_array_add(argv, (gchar *)"tc");
    g_ptr_array_add(argv, (gchar *)"qdisc");
    g_ptr_array_add(argv, (gchar *)"replace");
    g_ptr_array_add(argv, (gchar *)"dev");
    g_ptr_array_add(argv, (gchar *)PCV_QOS_IFB_DEV);
    g_ptr_array_add(argv, (gchar *)"parent");
    g_ptr_array_add(argv, vm_cid);
    g_ptr_array_add(argv, (gchar *)"netem");
    for (guint i = 0; tokens[i]; i++)
        g_ptr_array_add(argv, tokens[i]);
    g_ptr_array_add(argv, NULL);

    GError *terr = NULL;
    gboolean ok = pcv_spawn_sync((const gchar * const *)argv->pdata, NULL, NULL, &terr);
    g_ptr_array_free(argv, TRUE);                                           

    if (!ok) {
        gchar *detail = g_strdup_printf(
            "fail dry_run=false profile=\"%s\" timebox_sec=%u classid=%s error=\"%s\"",
            profile, timebox_sec, vm_cid,
            terr && terr->message ? terr->message : "unknown");
        pcv_audit_log(admin, "qos.chaos.start", vm, detail, -1, 0, "local");
        g_free(detail);
        g_propagate_prefixed_error(error, terr, "netem inject failed: ");
        g_strfreev(tokens);
        g_free(vm_cid);
        return FALSE;
    }
    g_strfreev(tokens);

    _ChaosEntry *e = g_new0(_ChaosEntry, 1);
    e->profile = g_strdup(profile);
    e->admin = g_strdup(admin ? admin : "");
    e->timebox_sec = timebox_sec;
    e->expires_at = g_get_real_time() / G_USEC_PER_SEC + timebox_sec;

                                                         
                                                 
                                                                    
    g_mutex_lock(&g_chaos_mu);
    g_hash_table_insert(g_chaos_active, g_strdup(vm), e);
    e->timer_id = g_timeout_add_seconds_full(G_PRIORITY_DEFAULT, timebox_sec,
                                              _chaos_expire_cb, g_strdup(vm), g_free);
    g_mutex_unlock(&g_chaos_mu);

    gchar *detail = g_strdup_printf(
        "ok dry_run=false profile=\"%s\" timebox_sec=%u classid=%s",
        profile, timebox_sec, vm_cid);
    pcv_audit_log(admin, "qos.chaos.start", vm, detail, 0, 0, "local");
    g_free(detail);
    g_free(vm_cid);
    return TRUE;
}

                                                    
                                                    
                                                                        
gboolean
pcv_qos_chaos_stop(const char *vm, const char *admin, GError **error)
{
    if (!vm || !*vm) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "vm must be non-empty");
                                                            
        pcv_audit_log(admin, "qos.chaos.stop", vm ? vm : "", "fail", -1, 0, "local");
        return FALSE;
    }

    g_mutex_lock(&g_chaos_mu);
    _ensure_chaos_table();
    _ChaosEntry *e = g_hash_table_lookup(g_chaos_active, vm);
    guint timer_id = e ? e->timer_id : 0;
    g_mutex_unlock(&g_chaos_mu);

    if (!e) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "no active chaos for vm '%s'", vm);
        pcv_audit_log(admin, "qos.chaos.stop", vm, "fail", -1, 0, "local");
        return FALSE;
    }

    if (timer_id > 0) g_source_remove(timer_id);                                      

    g_mutex_lock(&g_chaos_mu);
    g_hash_table_remove(g_chaos_active, vm);                        
    g_mutex_unlock(&g_chaos_mu);

    gboolean restore_ok = _chaos_restore_cake(vm);
    pcv_audit_log(admin, "qos.chaos.stop", vm, restore_ok ? "ok" : "restore_failed",
                  restore_ok ? 0 : -1, 0, "local");

    if (!restore_ok) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "chaos stopped (tracking cleared) but cake restore failed for vm '%s'", vm);
        return FALSE;
    }
    return TRUE;
}

                                                                      
                                                     
                                              
void
pcv_qos_chaos_purge_all(void)
{
    const gchar *show_argv[] = {"tc", "qdisc", "show", "dev", PCV_QOS_IFB_DEV, NULL};
    gchar *out = NULL;
    if (!pcv_spawn_sync(show_argv, &out, NULL, NULL)) {
                                                                  
        g_free(out);
        return;
    }

    GPtrArray *parents = pcv_qos_chaos_parse_netem_parents(out);
    g_free(out);

    for (guint i = 0; i < parents->len; i++) {
        const gchar *cid = g_ptr_array_index(parents, i);
        const gchar *argv[] = {
            "tc", "qdisc", "replace", "dev", PCV_QOS_IFB_DEV,
            "parent", cid, "cake", "besteffort", NULL
        };
        GError *err = NULL;
        if (!pcv_spawn_sync(argv, NULL, NULL, &err)) {
            PCV_LOG_WARN(QOS_CHAOS_LOG_DOM,
                "부팅 purge: classid=%s cake 복원 실패: %s",
                cid, err && err->message ? err->message : "unknown");
            g_clear_error(&err);
        } else {
            PCV_LOG_INFO(QOS_CHAOS_LOG_DOM,
                "부팅 purge: classid=%s netem→cake 복원(이전 데몬 인스턴스가 "
                "타임박스 만료 전 종료된 것으로 추정)", cid);
        }
    }
    g_ptr_array_unref(parents);

                                               
                                        
    pcv_qos_chaos_clear();
}

                                                                
                                                     
                                                    
                                     
                                                 
                                         
static void
_status_entry_free(gpointer p)
{
    PcvQosChaosStatusEntry *s = p;
    if (!s) return;
    g_free(s->vm);
    g_free(s->profile);
    g_free(s->admin);
    g_free(s);
}

                                               
                                                      
                                                                            
GPtrArray *
pcv_qos_chaos_status(void)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(_status_entry_free);

    g_mutex_lock(&g_chaos_mu);
    _ensure_chaos_table();
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, g_chaos_active);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        _ChaosEntry *e = v;
        PcvQosChaosStatusEntry *s = g_new0(PcvQosChaosStatusEntry, 1);
        s->vm = g_strdup((const gchar *)k);
        s->profile = g_strdup(e->profile);
        s->admin = g_strdup(e->admin);
        s->timebox_sec = e->timebox_sec;
        s->expires_at = e->expires_at;
        g_ptr_array_add(out, s);
    }
    g_mutex_unlock(&g_chaos_mu);

    return out;
}

                                                               
                                                    
                                
void
pcv_qos_chaos_clear(void)
{
    g_mutex_lock(&g_chaos_mu);
    if (g_chaos_active) {
        GHashTableIter it;
        gpointer k, v;
        g_hash_table_iter_init(&it, g_chaos_active);
        while (g_hash_table_iter_next(&it, &k, &v)) {
            _ChaosEntry *e = v;
            if (e->timer_id > 0) g_source_remove(e->timer_id);
        }
        g_hash_table_remove_all(g_chaos_active);
    }
    g_mutex_unlock(&g_chaos_mu);
}
