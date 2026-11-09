   
                            
                                                                                       
  
                           
                                                              
                                                                   
                                                                 
                                             
  
                                     
                                                               
                                                               
                                                              
                                                                 
                                                             
  
                                                        
                                                               
                                                          
   
#include "modules/network/network_qos_rules.h"
#include "modules/network/network_manager.h"
#include "modules/network/pcv_qos.h"
#include "api/drain.h"
#include "../../utils/pcv_spawn.h"
#include "../../utils/pcv_log.h"
#include "../../utils/pcv_config.h"
#include "../../utils/pcv_worker_pool.h"
#include <errno.h>
#include <string.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

                                                                      
static guint g_qos_reconcile_timer_id = 0;                             
static gint  g_qos_reconcile_inflight = 0;                          

                                                                          
                                            
  
                                        
                                            
                                                           
  
                                                     
                                                           
                                                                                
                                            
                                       
  
                                        
                                     
                                                                               
                                             
                            
  
                                    
                                                 
                                                               
                                                       
  
                      
                                                     
                                                           
                                                       
  
                             
                                                 
                                 
                                                          
                                        
                                           
                                                  
                                                                             
                                                     
                                          
#define QOS_PERSIST_DEFAULT_PATH "/var/lib/purecvisor/qos_rules.json"
#define QOS_SYSFS_NET_DEFAULT    "/sys/class/net"

                                                          
                                                                                       
                                           
void pcv_network_qos_rules_test_set_paths(const gchar *rules_path, const gchar *sysfs_net_root);
static gchar *g_rules_path_override;
static gchar *g_sysfs_net_override;

#define QOS_PERSIST_PATH (g_rules_path_override ? g_rules_path_override : QOS_PERSIST_DEFAULT_PATH)

                                                            
static GMutex g_rules_mu;

                                                                       
                                                                      
                                                     
static gboolean
_iface_name_is_ephemeral(const gchar *iface)
{
    if (!g_str_has_prefix(iface ? iface : "", "vnet"))
        return FALSE;
    const gchar *p = iface + strlen("vnet");
    if (!g_ascii_isdigit(*p))
        return FALSE;
    while (g_ascii_isdigit(*p))
        p++;
    return *p == '\0' || *p == ':';
}

                                                                            
                                                                  
                                                                      
                                                              
static gboolean
_rules_write_locked(JsonObject *root)
{
    JsonGenerator *gen = json_generator_new();
    json_generator_set_pretty(gen, TRUE);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(node, root);
    json_generator_set_root(gen, node);
    gsize length = 0;
    gchar *data = json_generator_to_data(gen, &length);
    GError *error = NULL;
    const gboolean written = g_file_set_contents(QOS_PERSIST_PATH, data, (gssize)length, &error);
    if (!written) {
        PCV_LOG_WARN("QOS", "QoS 규칙 파일 저장 실패: %s", error->message);
        g_clear_error(&error);
    }
    g_free(data);
    json_node_unref(node);
    g_object_unref(gen);
    return written;
}

void
pcv_network_qos_rules_test_set_paths(const gchar *rules_path, const gchar *sysfs_net_root)
{
    g_free(g_rules_path_override);
    g_free(g_sysfs_net_override);
    g_rules_path_override = g_strdup(rules_path);
    g_sysfs_net_override = g_strdup(sysfs_net_root);
}

   
                                             
  
                                                 
  
                                                   
                                                     
                                                 
                               
                                                       
   
static void
_qos_ensure_dir(void)
{
    gchar *dir = g_path_get_dirname(QOS_PERSIST_PATH);
    if (g_mkdir_with_parents(dir, 0700) != 0) {
        PCV_LOG_WARN("QOS", "Cannot create dir %s: %s", dir, g_strerror(errno));
    }
    g_free(dir);
}

   
                                                         
  
                                                  
                                                  
  
                                                      
                                                                   
                                          
                              
  
                                                    
                                         
                                 
                               
                                                                   
   
gboolean
pcv_network_qos_rules_save(const gchar *iface, const gchar *direction,
                           gint rate_mbps, gint burst_kb)
{
    if (_iface_name_is_ephemeral(iface)) {
        PCV_LOG_INFO("QOS", "QoS 규칙 %s(%s)은 현재 세션에만 적용하고 저장하지 않습니다 "
                     "(vnet 이름은 재부팅 뒤 다른 VM 에 재사용될 수 있음 — qos.vm.set 권장)",
                     iface, direction);
        return FALSE;
    }
    _qos_ensure_dir();                                          

    g_mutex_lock(&g_rules_mu);
                     
    JsonParser *parser = json_parser_new();
    JsonObject *root = NULL;
    if (g_file_test(QOS_PERSIST_PATH, G_FILE_TEST_EXISTS)) {
        if (json_parser_load_from_file(parser, QOS_PERSIST_PATH, NULL)) {
            JsonNode *root_node = json_parser_get_root(parser);
            if (root_node && JSON_NODE_HOLDS_OBJECT(root_node))
                root = json_node_dup_object(root_node);
        }
    }
    if (!root) root = json_object_new();

                           
    gchar *key = g_strdup_printf("%s:%s", iface, direction);
    JsonObject *rule = json_object_new();
    json_object_set_string_member(rule, "interface", iface);
    json_object_set_string_member(rule, "direction", direction);
    json_object_set_int_member(rule, "rate_mbps", rate_mbps);
    json_object_set_int_member(rule, "burst_kb", burst_kb);
    json_object_set_object_member(root, key, rule);
    g_free(key);

    const gboolean written = _rules_write_locked(root);
    g_mutex_unlock(&g_rules_mu);

    json_object_unref(root);
    g_object_unref(parser);
    return written;
}

   
                                                      
  
                                             
                                        
  
                                      
                                                  
                        
                                                                         
                                                      
   
void
pcv_network_qos_rules_remove(const gchar *iface, const gchar *direction)
{
    g_mutex_lock(&g_rules_mu);
    if (!g_file_test(QOS_PERSIST_PATH, G_FILE_TEST_EXISTS)) {
        g_mutex_unlock(&g_rules_mu);
        return;
    }

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_file(parser, QOS_PERSIST_PATH, NULL)) {
        g_object_unref(parser);
        g_mutex_unlock(&g_rules_mu);
        return;
    }
    JsonNode *root_n = json_parser_get_root(parser);
    if (!root_n || json_node_get_node_type(root_n) != JSON_NODE_OBJECT) {
        g_object_unref(parser);
        g_mutex_unlock(&g_rules_mu);
        return;
    }
    JsonObject *root = json_node_dup_object(root_n);

                                         
    if (g_strcmp0(direction, "both") == 0) {
        gchar *key_eg = g_strdup_printf("%s:egress", iface);
        gchar *key_in = g_strdup_printf("%s:ingress", iface);
        json_object_remove_member(root, key_eg);
        json_object_remove_member(root, key_in);
        g_free(key_eg); g_free(key_in);
    } else {
        gchar *key = g_strdup_printf("%s:%s", iface, direction);
        json_object_remove_member(root, key);
        g_free(key);
    }

    (void)_rules_write_locked(root);                                              
    g_mutex_unlock(&g_rules_mu);

    json_object_unref(root);
    g_object_unref(parser);
}

                                                             
                                                                   
                                                                   
static void
_rules_purge_ephemeral_locked(JsonObject *root)
{
    GList *members = json_object_get_members(root);
    guint purged = 0;
    for (GList *l = members; l; l = l->next) {
        const gchar *key = l->data;
        JsonNode *node = json_object_get_member(root, key);
        const gchar *iface = node && JSON_NODE_HOLDS_OBJECT(node)
            ? json_object_get_string_member_with_default(
                json_node_get_object(node), "interface", NULL) : NULL;
        if (!_iface_name_is_ephemeral(key) && !_iface_name_is_ephemeral(iface))
            continue;
        PCV_LOG_WARN("QOS", "저장된 QoS 규칙 '%s' 폐기: vnet 이름은 재부팅 뒤 다른 VM 에 재사용될 수 "
                     "있어 복원하지 않습니다(필요하면 qos.vm.set 으로 다시 설정)", key);
        json_object_remove_member(root, key);
        purged++;
    }
    g_list_free(members);
    if (purged > 0)
        (void)_rules_write_locked(root);                                            
}

   
                                            
  
                                              
                                                  
                                                              
  
                                
                                              
                                                                 
                                            
                                                      
                           
                                                               
                                                  
   
void
pcv_qos_restore(void)
{
    g_mutex_lock(&g_rules_mu);
    if (!g_file_test(QOS_PERSIST_PATH, G_FILE_TEST_EXISTS)) {
        g_mutex_unlock(&g_rules_mu);
        return;
    }

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_file(parser, QOS_PERSIST_PATH, NULL)) {
        g_object_unref(parser);
        g_mutex_unlock(&g_rules_mu);
        return;
    }

    JsonNode *qos_root_n = json_parser_get_root(parser);
    if (!qos_root_n || json_node_get_node_type(qos_root_n) != JSON_NODE_OBJECT) {
        g_warning("[QOS] %s is not a valid JSON object — skipping restore",
                  QOS_PERSIST_PATH);
        g_object_unref(parser);
        g_mutex_unlock(&g_rules_mu);
        return;
    }
    JsonObject *root = json_node_get_object(qos_root_n);
    _rules_purge_ephemeral_locked(root);
    g_mutex_unlock(&g_rules_mu);                                       
    GList *members = json_object_get_members(root);
    gint restored = 0;

    for (GList *l = members; l; l = l->next) {
        const gchar *member_key = l->data;
        JsonNode *rule_node = json_object_get_member(root, member_key);
        if (!rule_node || !JSON_NODE_HOLDS_OBJECT(rule_node)) continue;
        JsonObject *rule = json_node_get_object(rule_node);
        const gchar *iface = json_object_get_string_member_with_default(rule, "interface", NULL);
        const gchar *dir   = json_object_get_string_member_with_default(rule, "direction", NULL);
        gint rate  = (gint)json_object_get_int_member_with_default(rule, "rate_mbps", 0);
        gint burst = (gint)json_object_get_int_member_with_default(rule, "burst_kb", 256);

                                                        
        if (!iface || rate <= 0) continue;

                                                      
                                             
                                                         
                                                               
                                                                           
                                                      
        gchar *sys_path = g_build_filename(
            g_sysfs_net_override ? g_sysfs_net_override : QOS_SYSFS_NET_DEFAULT, iface, NULL);
        gboolean iface_present = g_file_test(sys_path, G_FILE_TEST_EXISTS);
        g_free(sys_path);
        if (!iface_present) {
            PCV_LOG_INFO("QOS", "QoS restore skip %s (%s): iface 미존재 — 다음 reconcile 재시도",
                         iface, dir ? dir : "egress");
            continue;
        }

                                                                           
                                                               
                                                                      
        if (dir && g_strcmp0(dir, "egress") != 0 && pcv_qos_iface_is_managed(iface)) {
            PCV_LOG_INFO("QOS", "QoS restore skip %s (ingress): D09 관리 대상", iface);
            continue;
        }

        PCV_LOG_INFO("QOS", "Restoring QoS for %s (%s): %d Mbps, %d KB burst",
                     iface, dir ? dir : "egress", rate, burst);

                                                       
                                                        
                                                       
                           
        if (!dir || g_strcmp0(dir, "egress") == 0) {
            const gchar *qdisc_argv[] = {"tc", "qdisc", "replace", "dev", iface,
                "root", "handle", "1:", "htb", "default", "10", NULL};
            pcv_spawn_sync(qdisc_argv, NULL, NULL, NULL);

            gchar *rate_str = g_strdup_printf("%dMbit", rate);
            gchar *burst_str = g_strdup_printf("%dk", burst);
            const gchar *class_argv[] = {"tc", "class", "replace", "dev", iface,
                "parent", "1:", "classid", "1:10", "htb",
                "rate", rate_str, "burst", burst_str, NULL};
            pcv_spawn_sync(class_argv, NULL, NULL, NULL);
            g_free(rate_str); g_free(burst_str);
        } else {
                                     
                                                              
                                                                   
                                            
            const gchar *ing_del_argv[] = {"tc", "qdisc", "del", "dev", iface,
                "ingress", NULL};
            pcv_spawn_sync(ing_del_argv, NULL, NULL, NULL);

            const gchar *ing_argv[] = {"tc", "qdisc", "add", "dev", iface,
                "ingress", NULL};
            pcv_spawn_sync(ing_argv, NULL, NULL, NULL);

            gchar *rate_str = g_strdup_printf("%dmbit", rate);
            gchar *burst_str = g_strdup_printf("%dk", burst);
            const gchar *filter_argv[] = {"tc", "filter", "add", "dev", iface,
                "parent", "ffff:", "protocol", "all", "u32",
                "match", "u32", "0", "0",
                "police", "rate", rate_str, "burst", burst_str,
                "action", "drop", NULL};
            pcv_spawn_sync(filter_argv, NULL, NULL, NULL);
            g_free(rate_str); g_free(burst_str);
        }
        restored++;
    }

    g_list_free(members);
    g_object_unref(parser);

    if (restored > 0)
        PCV_LOG_INFO("QOS", "Restored %d QoS rule(s) from %s",
                     restored, QOS_PERSIST_PATH);
}

                                                                        
                                                               
                                                                    
  
                                                 
                                       
                                                                
void
pcv_qos_rehydrate_reconcile(void)
{
                                                              
                                                                   
    pcv_qos_restore();
}

                                                                               
  
                                               
                                            
                                                               
                                                             
static void
_qos_reconcile_worker(GTask *task, gpointer src, gpointer td, GCancellable *c)
{
    (void)src; (void)td; (void)c;                       
    pcv_qos_rehydrate_reconcile();
    g_atomic_int_set(&g_qos_reconcile_inflight, 0);                                
    g_task_return_boolean(task, TRUE);
}

                                                           
                                                      
  
                                              
                                                  
                                                  
                                 
                                                           
static gboolean
_qos_reconcile_tick(gpointer data)
{
    (void)data;
                                                           
                                                       
    if (!g_atomic_int_compare_and_exchange(&g_qos_reconcile_inflight, 0, 1))
        return G_SOURCE_CONTINUE;                                            
    GTask *t = pcv_drain_task_new(NULL, NULL, NULL, NULL);
    pcv_worker_pool_push(t, _qos_reconcile_worker);                             
    g_object_unref(t);                                  
    return G_SOURCE_CONTINUE;
}

   
                                                   
  
                                                  
                                    
                                                               
                                   
   
void
pcv_qos_reconcile_timer_init(void)
{
                                                    
    gint interval = pcv_config_get_int("qos", "reconcile_interval_sec", 300);
    if (interval <= 0) {
        PCV_LOG_INFO("QOS", "QoS reconcile 타이머 비활성 (reconcile_interval_sec=%d)", interval);
        return;
    }
    g_qos_reconcile_timer_id = g_timeout_add_seconds((guint)interval, _qos_reconcile_tick, NULL);
    PCV_LOG_INFO("QOS", "QoS reconcile 타이머 등록 (%d초 주기)", interval);
}

   
                                                       
  
                                                       
                                              
   
void
pcv_qos_reconcile_timer_shutdown(void)
{
    if (g_qos_reconcile_timer_id) {
        g_source_remove(g_qos_reconcile_timer_id);
        g_qos_reconcile_timer_id = 0;                    
    }
}
