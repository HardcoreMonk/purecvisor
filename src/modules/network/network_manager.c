   
                          
                                             
  
                           
                                                   
                                                    
                                        
  
                                                 
                                                 
                                                      
                                                     
                                                   
                                            
  
                                                                       
            
                                                                     
                                                                    
                                                                       
  
                                              
                                           
  
                     
                                                      
                                                                
                                                                    
                                                            
                                                                           
                                                            
                                          
                                                         
                                                         
                                                  
  
          
                                                            
                                            
                                           
                                                      
                                           
                                                       
                           
  
              
                                                            
                                               
                                                 
                                                  
  
              
                                               
                                       
                                                 
  
             
                                                             
                                                            
                                                                 
  
          
                                            
                                          
                                                  
                                                   
                                         
  
         
                                            
                                                                  
                                                           
                                               
                                                 
  
         
                                                          
                                                 
                                                         
                                                    
                                                        
                                                          
                                               
        
                                                                      
                     
                                                      
                                                     
                                                             
                                                     
         
                                                           
                                                  
        
                                                  
                                                                 
                                                         
        
                                                                     
                                                              
                                                 
                                                                     
                                
                                                                       
   
#include <stdio.h>
#include <glib.h>
#include <glib/gstdio.h>                             
#include <gio/gio.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <unistd.h>

#include "modules/network/network_manager.h"
#include "../../utils/pcv_validate.h"
#include "modules/dispatcher/rpc_utils.h"
#include "modules/network/network_firewall.h"
#include "modules/network/network_dhcp.h"
#include "../../utils/pcv_spawn.h"
#include "../../utils/pcv_log.h"
#include "../../utils/pcv_config.h"                                             
#include "../../utils/pcv_worker_pool.h"                                
#include "vm_iface.h"
#include "modules/network/pcv_qos.h"                                                   
#include "modules/network/vpc/vpc_manager.h"                                   
#include "modules/network/pcv_shared_bridge.h"                                        
#include "bpf/pcv_shared_bridge.h"                                                   
#include <json-glib/json-glib.h>

#define NET_LOG_DOM "network"

                                                                      
static guint g_qos_reconcile_timer_id = 0;                             
static gint  g_qos_reconcile_inflight = 0;                          

                                                       
                                                         
                         
static gboolean
_reject_vpc_managed_bridge(const gchar *bridge_name,
                           const gchar *rpc_id,
                           UdsServer *server,
                           GSocketConnection *connection)
{
    if (!pcv_vpc_bridge_is_managed(bridge_name)) return FALSE;
    gchar *response = pure_rpc_build_error_response(
        rpc_id, PURE_RPC_ERR_CONFLICT,
        "VPC-managed bridge is read-only; use the vpc.* API");
    pure_uds_server_send_response(server, connection, response);
    g_free(response);
    return TRUE;
}

                                                                          
                                            
  
                                        
                                            
                                                           
  
                                                     
                                                           
                                                                                
                                            
                                       
  
                                        
                                     
                                                                               
                                             
                            
  
                                    
                                                 
                                                               
                                                       
  
                      
                                                     
                                                           
                                                       
  
                             
                                                 
                                 
                                                          
                                        
                                           
                                                  
                                                                             
                                                     
                                          
#define QOS_PERSIST_PATH "/var/lib/purecvisor/qos_rules.json"

   
                                             
  
                                                 
  
                                                   
                                                     
                                                 
                               
                                                       
   
static void
_qos_ensure_dir(void)
{
    gchar *dir = g_path_get_dirname(QOS_PERSIST_PATH);
    if (g_mkdir_with_parents(dir, 0700) != 0) {
        PCV_LOG_WARN("QOS", "Cannot create dir %s: %s", dir, g_strerror(errno));
    }
    g_free(dir);
}

   
                                                
  
                                                  
                                                  
  
                                                      
                                             
                                          
                              
  
                                         
                                         
                                 
                               
   
static void
_qos_persist_save(const gchar *iface, const gchar *direction,
                  gint rate_mbps, gint burst_kb)
{
    _qos_ensure_dir();                                          

                     
    JsonParser *parser = json_parser_new();
    JsonObject *root = NULL;
    if (g_file_test(QOS_PERSIST_PATH, G_FILE_TEST_EXISTS)) {
        if (json_parser_load_from_file(parser, QOS_PERSIST_PATH, NULL)) {
            root = json_node_dup_object(json_parser_get_root(parser));
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

                    
    JsonGenerator *gen = json_generator_new();
    json_generator_set_pretty(gen, TRUE);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(node, root);
    json_generator_set_root(gen, node);
    json_generator_to_file(gen, QOS_PERSIST_PATH, NULL);

    json_node_unref(node);
    json_object_unref(root);
    g_object_unref(gen);
    g_object_unref(parser);
}

   
                                             
  
                                             
                                        
  
                                      
                                                  
                        
                                                                         
                                                      
   
static void
_qos_persist_remove(const gchar *iface, const gchar *direction)
{
    if (!g_file_test(QOS_PERSIST_PATH, G_FILE_TEST_EXISTS)) return;

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_file(parser, QOS_PERSIST_PATH, NULL)) {
        g_object_unref(parser);
        return;
    }
    JsonNode *root_n = json_parser_get_root(parser);
    if (!root_n || json_node_get_node_type(root_n) != JSON_NODE_OBJECT) {
        g_object_unref(parser);
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

    JsonGenerator *gen = json_generator_new();
    json_generator_set_pretty(gen, TRUE);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(node, root);
    json_generator_set_root(gen, node);
    json_generator_to_file(gen, QOS_PERSIST_PATH, NULL);

    json_node_unref(node);
    json_object_unref(root);
    g_object_unref(gen);
    g_object_unref(parser);
}

   
                                            
  
                                              
                                                 
                                     
  
                                
                                              
                                                                 
                                            
                                                      
                           
                                                               
                                                  
   
void
pcv_qos_restore(void)
{
    if (!g_file_test(QOS_PERSIST_PATH, G_FILE_TEST_EXISTS)) return;

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_file(parser, QOS_PERSIST_PATH, NULL)) {
        g_object_unref(parser);
        return;
    }

    JsonNode *qos_root_n = json_parser_get_root(parser);
    if (!qos_root_n || json_node_get_node_type(qos_root_n) != JSON_NODE_OBJECT) {
        g_warning("[QOS] %s is not a valid JSON object — skipping restore",
                  QOS_PERSIST_PATH);
        g_object_unref(parser);
        return;
    }
    JsonObject *root = json_node_get_object(qos_root_n);
    GList *members = json_object_get_members(root);
    gint restored = 0;

    for (GList *l = members; l; l = l->next) {
        const gchar *member_key = l->data;
        JsonObject *rule = json_object_get_object_member(root, member_key);
        const gchar *iface = json_object_get_string_member(rule, "interface");
        const gchar *dir   = json_object_get_string_member(rule, "direction");
        gint rate  = (gint)json_object_get_int_member(rule, "rate_mbps");
        gint burst = (gint)json_object_get_int_member(rule, "burst_kb");

                                                        
        if (!iface || rate <= 0) continue;

                                                      
                                             
                                                         
                                                               
                                                                           
                                                      
        gchar *sys_path = g_strdup_printf("/sys/class/net/%s", iface);
        gboolean iface_present = g_file_test(sys_path, G_FILE_TEST_EXISTS);
        g_free(sys_path);
        if (!iface_present) {
            PCV_LOG_INFO("QOS", "QoS restore skip %s (%s): iface 미존재 — 다음 reconcile 재시도",
                         iface, dir ? dir : "egress");
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
    GTask *t = g_task_new(NULL, NULL, NULL, NULL);
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

   
                                                 
  
                                                   
                                                   
  
                                                          
                                                       
                                                   
                           
   
typedef struct {
    gchar    *bridge_name;
    gchar    *cidr;
    gchar    *mode;
    gchar    *physical_if;
    gchar    *uplink_mode;                                    
    gchar    *rpc_id;
    gchar    *dhcp_warning;                                       
    gboolean  dns_enabled;                                         
    gchar    *upstream_dns;                                            
    gchar    *ipv6_prefix;                                               
    gint      mtu;                                             
    UdsServer *server;
    GSocketConnection *connection;
} NetworkCtx;

                                                              
  
                   
                                                 
                                              
                                        
  
          
                                                               
                                              
                                                      
  
                         
                                    
                             
   
static GHashTable *g_net_inflight = NULL;                                      
static GMutex      g_net_inflight_mu;                                    
                                                                  
static GMutex      g_physical_bridge_mu;

#define PCV_PHYSICAL_NETWORK_STATE_DIR "/var/lib/purecvisor/networks"
#define PCV_PHYSICAL_NETWORK_STATE_VERSION 2

typedef struct {
    gchar *bridge_name;
    gchar *physical_if;
    gchar *uplink_mode;
    gchar *phase;
    gchar *physical_mac;
    gchar *portal_bridge_if;
    gchar *portal_if;
    gchar *bpf_sha256;
    gint mtu;
    gboolean physical_was_up;
    gboolean promisc_was_on;
    guint32 generation;
} PcvPhysicalBridgeState;

static void _physical_state_clear(PcvPhysicalBridgeState *state);
static gboolean _physical_state_load_at(const gchar *state_dir,
                                        const gchar *bridge_name,
                                        PcvPhysicalBridgeState *state,
                                        GError **error);
static gboolean _physical_state_remove_at(const gchar *state_dir,
                                          const gchar *bridge_name,
                                          GError **error);
static gboolean _physical_detach_restore_at(const gchar *bridge_name,
                                            const gchar *physical_if,
                                            gboolean was_up,
                                            const gchar *sysfs_net_root,
                                            GError **error);

                                                          
                                                
                                                             
static void _net_inflight_init_once(void) {
    static gsize initialized = 0;
    if (g_once_init_enter(&initialized)) {                          
        g_net_inflight = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        g_once_init_leave(&initialized, 1);
    }
}

   
                                              
                                              
  
                                           
                                            
  
                                                  
                                                          
                                    
   
static void free_network_ctx(gpointer data) {
    if (!data) return;
    NetworkCtx *ctx = (NetworkCtx *)data;
    g_free(ctx->bridge_name);
    g_free(ctx->cidr);
    g_free(ctx->mode);
    g_free(ctx->physical_if);
    g_free(ctx->uplink_mode);
    g_free(ctx->rpc_id);
    g_free(ctx->dhcp_warning);
    g_free(ctx->upstream_dns);            
    g_free(ctx->ipv6_prefix);                       
    if (ctx->server) g_object_unref(ctx->server);
    if (ctx->connection) g_object_unref(ctx->connection);
    g_free(ctx);
}

                                                                    
                          
  
                                
                                                                     
                                                                   
                                                               
                                              
                                                
  
                                     
                                                
                                                              
                                                                       
   
                                                             
                                         
                                                                                
                               
                    
  
                                             
                                                         
                                                 
              
  
                                               
                                        
                                           
                         
         
                                                     
                                                       
                                                                    
                                                         
                                           
  
                                            
   
static void _bridge_create_rollback(const gchar *bridge_name, const gchar *cidr)
{
    if (cidr && *cidr) {
        const gchar *unaddr[] = {"ip", "addr", "del", cidr, "dev", bridge_name, NULL};
        GError *cleanup = NULL;
        if (!pcv_spawn_sync(unaddr, NULL, NULL, &cleanup))
            PCV_LOG_WARN(NET_LOG_DOM, "bridge create address rollback failed for %s: %s",
                         bridge_name, cleanup ? cleanup->message : "unknown");
        g_clear_error(&cleanup);
    }

    const gchar *del[] = {"ip", "link", "delete", bridge_name, "type", "bridge", NULL};
    GError *cleanup = NULL;
    if (!pcv_spawn_sync(del, NULL, NULL, &cleanup))
        PCV_LOG_WARN(NET_LOG_DOM, "bridge create rollback failed for %s: %s",
                     bridge_name, cleanup ? cleanup->message : "unknown");
    g_clear_error(&cleanup);
}

gboolean network_bridge_create(const gchar *bridge_name, const gchar *cidr, gint mtu, GError **error) {

                
    {
        const gchar *argv[] = {"ip","link","add","name",bridge_name,"type","bridge","stp_state","0",NULL};
        gchar *std_err_local = NULL;
        if (!pcv_spawn_sync(argv, NULL, &std_err_local, error)) {
            if (error && !*error)
                g_set_error(error,G_IO_ERROR,G_IO_ERROR_FAILED,"Bridge creation failed: %s",
                            std_err_local ? std_err_local : "unknown");
            g_free(std_err_local); return FALSE;
        }
        g_free(std_err_local);
    }

                                               
    if (cidr && strlen(cidr) > 0) {
        const gchar *addr_argv[] = {"ip","addr","add",cidr,"dev",bridge_name,NULL};
        gchar *std_err_local = NULL;
        if (!pcv_spawn_sync(addr_argv, NULL, &std_err_local, error)) {
            if (error && !*error)
                g_set_error(error,G_IO_ERROR,G_IO_ERROR_FAILED,"IP assignment failed: %s",
                            std_err_local ? std_err_local : "unknown");
            g_free(std_err_local);
                                                            
                                                       
            _bridge_create_rollback(bridge_name, NULL);
            return FALSE;
        }
        g_free(std_err_local);
    }

                                 
                                                                         
                                                      
    {
        gint eff_mtu = (mtu > 0) ? mtu : 1500;
        gchar mtu_str[16];
        g_snprintf(mtu_str, sizeof(mtu_str), "%d", eff_mtu);
        const gchar *mtu_argv[] = {"ip","link","set","dev",bridge_name,"mtu",mtu_str,NULL};
        gchar *mtu_err = NULL;
        if (!pcv_spawn_sync(mtu_argv, NULL, &mtu_err, error)) {
            if (error && !*error)
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "MTU set failed: %s", mtu_err ? mtu_err : "unknown");
            g_free(mtu_err);
                                                              
                                    
            _bridge_create_rollback(bridge_name, cidr);
            return FALSE;
        }
        g_free(mtu_err);
    }

                
    {
        const gchar *up_argv[] = {"ip","link","set","dev",bridge_name,"up",NULL};
        gchar *up_err = NULL;
        if (!pcv_spawn_sync(up_argv, NULL, &up_err, error)) {
            if (error && !*error)
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Bridge UP failed: %s", up_err ? up_err : "unknown");
            g_free(up_err);
                                                             
                                    
            _bridge_create_rollback(bridge_name, cidr);
            return FALSE;
        }
        g_free(up_err);
    }

    return TRUE;
}

   
                                                             
  
                                                       
                                               
  
                                                                       
                                                                                     
                                                                    
                                                               
  
                                                                        
                                                         
                                                          
   
gint
pcv_bridge_mtu_read(const gchar *bridge, const gchar *sysfs_net_root)
{
    if (!bridge || !*bridge || strchr(bridge, '/') || strstr(bridge, ".."))
        return 0;
    const gchar *root = sysfs_net_root ? sysfs_net_root : "/sys/class/net";
    gchar *path = g_build_filename(root, bridge, "mtu", NULL);
    gchar *contents = NULL;
    gint mtu = 0;
    if (g_file_get_contents(path, &contents, NULL, NULL)) {
        gchar *end = NULL;
        gint64 v = g_ascii_strtoll(contents, &end, 10);
        if (end != contents && v >= 68 && v <= 9216) {
            mtu = (gint)v;
        } else {
            g_warning("[N8] bridge '%s' MTU 파싱 실패 또는 대역(68–9216) 밖: '%s' — <mtu> 생략",
                      bridge, contents);
        }
    } else {
        g_warning("[N8] bridge '%s' MTU sysfs 읽기 실패(%s) — <mtu> 생략", bridge, path);
    }
    g_free(contents);
    g_free(path);
    return mtu;
}

                                                            
static GPtrArray *_route_fields(const gchar *line)
{
    GPtrArray *fields = g_ptr_array_new_with_free_func(g_free);
    gchar **raw = g_strsplit_set(line ? line : "", " \t\r", -1);
    for (guint i = 0; raw[i]; i++) {
        if (*raw[i]) g_ptr_array_add(fields, g_strdup(raw[i]));
    }
    g_strfreev(raw);
    return fields;
}

                                                                      
                                                         
static gboolean _route_file_has_default(const gchar *path,
                                        const gchar *iface,
                                        gboolean ipv6,
                                        gboolean *has_default,
                                        GError **error)
{
    gchar *contents = NULL;
    if (!g_file_get_contents(path, &contents, NULL, error)) return FALSE;

    *has_default = FALSE;
    gchar **lines = g_strsplit(contents, "\n", -1);
    for (guint i = 0; lines[i]; i++) {
        GPtrArray *f = _route_fields(lines[i]);
        if (!ipv6 && f->len >= 2) {
            const gchar *row_iface = g_ptr_array_index(f, 0);
            const gchar *dest = g_ptr_array_index(f, 1);
            if (g_strcmp0(row_iface, iface) == 0 && g_strcmp0(dest, "00000000") == 0)
                *has_default = TRUE;
        } else if (ipv6 && f->len >= 3) {
            const gchar *dest = g_ptr_array_index(f, 0);
            const gchar *prefix = g_ptr_array_index(f, 1);
            const gchar *row_iface = g_ptr_array_index(f, f->len - 1);
            if (g_strcmp0(row_iface, iface) == 0
                && g_strcmp0(dest, "00000000000000000000000000000000") == 0
                && g_strcmp0(prefix, "00") == 0)
                *has_default = TRUE;
        }
        g_ptr_array_unref(f);
        if (*has_default) break;
    }
    g_strfreev(lines);
    g_free(contents);
    return TRUE;
}

static gboolean _iface_master_read(const gchar *sysfs_net_root,
                                   const gchar *iface,
                                   gchar **master_out,
                                   GError **error)
{
    *master_out = NULL;
    gchar *path = g_build_filename(sysfs_net_root, iface, "master", NULL);
    GStatBuf st = {0};
    if (g_lstat(path, &st) != 0) {
        gint saved_errno = errno;
        g_free(path);
        if (saved_errno == ENOENT) return TRUE;
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved_errno),
                    "cannot inspect interface '%s' master: %s",
                    iface, g_strerror(saved_errno));
        return FALSE;
    }
    gchar *link = g_file_read_link(path, error);
    if (!link) {
        g_free(path);
        return FALSE;
    }
    *master_out = g_path_get_basename(link);
    g_free(link);
    g_free(path);
    return TRUE;
}

static gboolean
_iface_preflight_dedicated(const gchar *physical_if,
                           const gchar *expected_master,
                           const gchar *sysfs_net_root,
                           const gchar *proc_root,
                           gboolean *was_up,
                           GError **error)
{
    const gchar *sysroot = sysfs_net_root ? sysfs_net_root : "/sys/class/net";
    const gchar *procroot = proc_root ? proc_root : "/proc";
    gboolean real_host_view = !sysfs_net_root && !proc_root;
    if (was_up) *was_up = FALSE;

    if (!pcv_validate_iface_name(physical_if) || g_strcmp0(physical_if, "lo") == 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "physical_if must be a non-loopback interface");
        return FALSE;
    }

    gchar *iface_path = g_build_filename(sysroot, physical_if, NULL);
    gchar *type_path = g_build_filename(iface_path, "type", NULL);
    gchar *flags_path = g_build_filename(iface_path, "flags", NULL);
    gchar *device_path = g_build_filename(iface_path, "device", NULL);
    gchar *wireless_path = g_build_filename(iface_path, "wireless", NULL);
    gchar *bridge_path = g_build_filename(iface_path, "bridge", NULL);
    gchar *bonding_path = g_build_filename(iface_path, "bonding", NULL);
    gchar *vlan_path = g_build_filename(procroot, "net", "vlan", physical_if, NULL);
    gboolean ok = FALSE;
    gchar *contents = NULL;

    if (!g_file_test(iface_path, G_FILE_TEST_IS_DIR)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "physical interface '%s' does not exist", physical_if);
        goto out;
    }
    if (!g_file_get_contents(type_path, &contents, NULL, error)) goto out;
    g_strstrip(contents);
    if (g_strcmp0(contents, "1") != 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "interface '%s' is not an Ethernet uplink", physical_if);
        goto out;
    }
    g_clear_pointer(&contents, g_free);
    if (!g_file_test(device_path, G_FILE_TEST_EXISTS)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "interface '%s' is not a physical device", physical_if);
        goto out;
    }
    if (g_file_test(wireless_path, G_FILE_TEST_EXISTS)
        || g_file_test(bridge_path, G_FILE_TEST_EXISTS)
        || g_file_test(bonding_path, G_FILE_TEST_EXISTS)
        || g_file_test(vlan_path, G_FILE_TEST_EXISTS)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "interface '%s' is wireless, bonded, VLAN, or already a bridge",
                    physical_if);
        goto out;
    }
    gchar *master = NULL;
    if (!_iface_master_read(sysroot, physical_if, &master, error)) goto out;
    if (master && g_strcmp0(master, expected_master) != 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "interface '%s' already belongs to master '%s'", physical_if, master);
        g_free(master);
        goto out;
    }
    g_free(master);

    if (!g_file_get_contents(flags_path, &contents, NULL, error)) goto out;
    gchar *end = NULL;
    guint64 flags = g_ascii_strtoull(contents, &end, 0);
    if (end == contents) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "cannot determine administrative state of '%s'", physical_if);
        goto out;
    }
    if (was_up) *was_up = (flags & IFF_UP) != 0;
    g_clear_pointer(&contents, g_free);

    gchar *route4 = g_build_filename(procroot, "net", "route", NULL);
    gchar *route6 = g_build_filename(procroot, "net", "ipv6_route", NULL);
    gboolean has_default = FALSE;
    if (!_route_file_has_default(route4, physical_if, FALSE, &has_default, error)) {
        g_free(route4); g_free(route6); goto out;
    }
    if (has_default) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "interface '%s' owns the IPv4 default route and may be the management NIC",
                    physical_if);
        g_free(route4); g_free(route6); goto out;
    }
    if (!_route_file_has_default(route6, physical_if, TRUE, &has_default, error)) {
        g_free(route4); g_free(route6); goto out;
    }
    g_free(route4); g_free(route6);
    if (has_default) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "interface '%s' owns the IPv6 default route and may be the management NIC",
                    physical_if);
        goto out;
    }

                                                                         
                                                          
    if (real_host_view) {
        struct ifaddrs *ifaddr = NULL;
        if (getifaddrs(&ifaddr) != 0) {
            pcv_network_iface_address_facts_dedicated(
                physical_if, FALSE, FALSE, FALSE, error);
            goto out;
        }
        gboolean has_ipv4 = FALSE;
        gboolean has_non_link_local_ipv6 = FALSE;
        for (struct ifaddrs *it = ifaddr; it; it = it->ifa_next) {
            if (!it->ifa_addr || g_strcmp0(it->ifa_name, physical_if) != 0) continue;
            int family = it->ifa_addr->sa_family;
            if (family == AF_INET) {
                has_ipv4 = TRUE;
            }
            if (family == AF_INET6) {
                const struct sockaddr_in6 *sa6 = (const struct sockaddr_in6 *)it->ifa_addr;
                if (!IN6_IS_ADDR_LINKLOCAL(&sa6->sin6_addr))
                    has_non_link_local_ipv6 = TRUE;
            }
        }
        freeifaddrs(ifaddr);
        if (!pcv_network_iface_address_facts_dedicated(
                physical_if, TRUE, has_ipv4, has_non_link_local_ipv6, error))
            goto out;
    }

    ok = TRUE;
out:
    g_free(contents);
    g_free(iface_path);
    g_free(type_path);
    g_free(flags_path);
    g_free(device_path);
    g_free(wireless_path);
    g_free(bridge_path);
    g_free(bonding_path);
    g_free(vlan_path);
    return ok;
}

gboolean
pcv_network_iface_address_facts_dedicated(const gchar *physical_if,
                                          gboolean inventory_known,
                                          gboolean has_ipv4,
                                          gboolean has_non_link_local_ipv6,
                                          GError **error)
{
    if (!inventory_known) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "cannot inspect addresses on interface '%s'", physical_if);
        return FALSE;
    }
    if (has_ipv4) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "interface '%s' has a host IPv4 address; dedicated uplink required",
                    physical_if);
        return FALSE;
    }
    if (has_non_link_local_ipv6) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "interface '%s' has a non-link-local host IPv6 address",
                    physical_if);
        return FALSE;
    }
    return TRUE;
}

gboolean
pcv_network_iface_preflight_dedicated(const gchar *physical_if,
                                      const gchar *sysfs_net_root,
                                      const gchar *proc_root,
                                      gboolean *was_up,
                                      GError **error)
{
    return _iface_preflight_dedicated(physical_if, NULL, sysfs_net_root, proc_root,
                                      was_up, error);
}

gboolean
pcv_network_iface_address_facts_shared(const gchar *physical_if,
                                       gboolean inventory_known,
                                       gboolean has_ipv4,
                                       gboolean has_non_link_local_ipv6,
                                       GError **error)
{
    if (!inventory_known) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "cannot inspect addresses on shared interface '%s'", physical_if);
        return FALSE;
    }
    if (!has_ipv4 && !has_non_link_local_ipv6) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "shared interface '%s' has no usable host address; use dedicated uplink",
                    physical_if);
        return FALSE;
    }
    return TRUE;
}

gboolean
pcv_network_iface_preflight_shared(const gchar *physical_if,
                                   const gchar *sysfs_net_root,
                                   const gchar *proc_root,
                                   PcvSharedIfaceFacts *facts,
                                   GError **error)
{
    const gchar *sysroot = sysfs_net_root ? sysfs_net_root : "/sys/class/net";
    const gchar *procroot = proc_root ? proc_root : "/proc";
    gboolean production_view = !sysfs_net_root && !proc_root;
    PcvSharedIfaceFacts found = {0};
    if (!facts || !pcv_validate_iface_name(physical_if)
        || g_strcmp0(physical_if, "lo") == 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "shared physical_if must be a non-loopback interface");
        return FALSE;
    }

    gchar *base = g_build_filename(sysroot, physical_if, NULL);
    gchar *type = g_build_filename(base, "type", NULL);
    gchar *flags = g_build_filename(base, "flags", NULL);
    gchar *mtu = g_build_filename(base, "mtu", NULL);
    gchar *address = g_build_filename(base, "address", NULL);
    gchar *device = g_build_filename(base, "device", NULL);
    gchar *wireless = g_build_filename(base, "wireless", NULL);
    gchar *bridge = g_build_filename(base, "bridge", NULL);
    gchar *bonding = g_build_filename(base, "bonding", NULL);
    gchar *vlan = g_build_filename(procroot, "net", "vlan", physical_if, NULL);
    gchar *contents = NULL;
    gboolean ok = FALSE;

    if (!g_file_test(base, G_FILE_TEST_IS_DIR)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "shared physical interface '%s' does not exist", physical_if);
        goto out;
    }
    if (!g_file_get_contents(type, &contents, NULL, error)) goto out;
    g_strstrip(contents);
    if (g_strcmp0(contents, "1") != 0 || !g_file_test(device, G_FILE_TEST_EXISTS)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "shared interface '%s' must be a physical Ethernet device", physical_if);
        goto out;
    }
    g_clear_pointer(&contents, g_free);
    if (g_file_test(wireless, G_FILE_TEST_EXISTS)
        || g_file_test(bridge, G_FILE_TEST_EXISTS)
        || g_file_test(bonding, G_FILE_TEST_EXISTS)
        || g_file_test(vlan, G_FILE_TEST_EXISTS)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "shared interface '%s' cannot be wireless, bonded, VLAN, or a bridge",
                    physical_if);
        goto out;
    }
    gchar *master = NULL;
    if (!_iface_master_read(sysroot, physical_if, &master, error)) goto out;
    if (master) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "shared interface '%s' already belongs to master '%s'",
                    physical_if, master);
        g_free(master);
        goto out;
    }

    if (!g_file_get_contents(flags, &contents, NULL, error)) goto out;
    gchar *end = NULL;
    guint64 parsed_flags = g_ascii_strtoull(contents, &end, 0);
    if (end == contents || !(parsed_flags & IFF_UP)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "shared interface '%s' must already be administratively up", physical_if);
        goto out;
    }
    found.was_up = TRUE;
    found.promisc_was_on = (parsed_flags & IFF_PROMISC) != 0;
    g_clear_pointer(&contents, g_free);

    if (!g_file_get_contents(mtu, &contents, NULL, error)) goto out;
    end = NULL;
    gint64 parsed_mtu = g_ascii_strtoll(contents, &end, 10);
    if (end == contents || parsed_mtu < 68 || parsed_mtu > 9216) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "shared interface '%s' has an invalid MTU", physical_if);
        goto out;
    }
    found.mtu = (gint)parsed_mtu;
    g_clear_pointer(&contents, g_free);
    if (!g_file_get_contents(address, &contents, NULL, error)) goto out;
    g_strstrip(contents);
    if (!pcv_validate_mac(contents)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "shared interface '%s' has an invalid MAC address", physical_if);
        goto out;
    }
    g_strlcpy(found.mac, contents, sizeof(found.mac));

    if (production_view) {
        struct ifaddrs *ifaddr = NULL;
        if (getifaddrs(&ifaddr) != 0) {
            pcv_network_iface_address_facts_shared(
                physical_if, FALSE, FALSE, FALSE, error);
            goto out;
        }
        gboolean has_ipv4 = FALSE;
        gboolean has_global_v6 = FALSE;
        for (struct ifaddrs *it = ifaddr; it; it = it->ifa_next) {
            if (!it->ifa_addr || g_strcmp0(it->ifa_name, physical_if) != 0) continue;
            if (it->ifa_addr->sa_family == AF_INET) has_ipv4 = TRUE;
            if (it->ifa_addr->sa_family == AF_INET6) {
                const struct sockaddr_in6 *sa6 = (const struct sockaddr_in6 *)it->ifa_addr;
                if (!IN6_IS_ADDR_LINKLOCAL(&sa6->sin6_addr)) has_global_v6 = TRUE;
            }
        }
        freeifaddrs(ifaddr);
        if (!pcv_network_iface_address_facts_shared(
                physical_if, TRUE, has_ipv4, has_global_v6, error))
            goto out;
    }
    *facts = found;
    ok = TRUE;
out:
    g_free(contents);
    g_free(base); g_free(type); g_free(flags); g_free(mtu); g_free(address);
    g_free(device); g_free(wireless); g_free(bridge); g_free(bonding); g_free(vlan);
    return ok;
}

   
                                                       
                          
                                      
                    
  
                                               
                                            
                                  
  
                                                                      
                                                
  
                                                 
                                                                           
  
                    
   
gboolean network_bridge_bind_physical(const gchar *bridge_name, const gchar *physical_if, GError **error) {
    {
        const gchar *argv[] = {"ip","link","set",physical_if,"master",bridge_name,NULL};
        gchar *std_err_local = NULL;
        if (!pcv_spawn_sync(argv, NULL, &std_err_local, error)) {
            if (error && !*error)
                g_set_error(error,G_IO_ERROR,G_IO_ERROR_FAILED,
                            "Failed to bind physical NIC '%s': %s",
                            physical_if, std_err_local ? std_err_local : "unknown");
            g_free(std_err_local); return FALSE;
        }
        g_free(std_err_local);
    }
    {
        const gchar *a[] = {"ip","link","set","dev",physical_if,"up",NULL};
        gchar *std_err_local = NULL;
        if (!pcv_spawn_sync(a, NULL, &std_err_local, error)) {
            if (error && !*error)
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Failed to bring physical NIC '%s' up: %s",
                            physical_if, std_err_local ? std_err_local : "unknown");
            g_free(std_err_local);
            const gchar *undo[] = {"ip", "link", "set", physical_if, "nomaster", NULL};
            pcv_spawn_sync(undo, NULL, NULL, NULL);
            return FALSE;
        }
        g_free(std_err_local);
    }

    return TRUE;
}

gboolean
pcv_network_bridge_has_host_uplink_at(const gchar *bridge_name,
                                      const gchar *sysfs_net_root,
                                      gchar **uplink_out,
                                      GError **error)
{
    const gchar *sysroot = sysfs_net_root ? sysfs_net_root : "/sys/class/net";
    if (uplink_out) *uplink_out = NULL;
    if (!pcv_validate_bridge_name(bridge_name)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "invalid bridge name for uplink inspection");
        return FALSE;
    }

    gchar *bridge_path = g_build_filename(sysroot, bridge_name, NULL);
    if (!g_file_test(bridge_path, G_FILE_TEST_IS_DIR)) {
        g_free(bridge_path);
        return TRUE;                                    
    }
    gchar *brif_path = g_build_filename(bridge_path, "brif", NULL);
    GError *dir_error = NULL;
    GDir *dir = g_dir_open(brif_path, 0, &dir_error);
    g_free(bridge_path);
    if (!dir) {
        if (error) {
            g_propagate_prefixed_error(error, dir_error,
                                       "cannot inspect bridge '%s' uplinks: ", bridge_name);
        } else {
            g_clear_error(&dir_error);
        }
        g_free(brif_path);
        return FALSE;
    }

    const gchar *entry = NULL;
    while ((entry = g_dir_read_name(dir)) != NULL) {
        if (g_str_has_prefix(entry, "vnet")
            || g_str_has_prefix(entry, "tap")
            || g_str_has_prefix(entry, "veth")
            || g_str_has_prefix(entry, "psb"))
            continue;
        if (uplink_out) *uplink_out = g_strdup(entry);
        break;
    }
    g_dir_close(dir);
    g_free(brif_path);
    return TRUE;
}

   
                                            
  
                                               
                                                             
                                              
                      
  
                                                             
                                                  
  
                  
                                                              
                                    
                                          
                                                
                                       
  
          
                                               
                                     
   
gboolean
pcv_network_bridge_delete_at(const gchar *bridge_name,
                             const gchar *sysfs_net_root,
                             GError **error)
{
    const gchar *sysroot = sysfs_net_root ? sysfs_net_root : "/sys/class/net";
    gchar *host_uplink = NULL;
    if (!pcv_network_bridge_has_host_uplink_at(
            bridge_name, sysfs_net_root, &host_uplink, error))
        return FALSE;
    if (host_uplink) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "bridge '%s' still has unmanaged host uplink '%s'; refusing generic delete",
                    bridge_name, host_uplink);
        g_free(host_uplink);
        return FALSE;
    }

    gchar *pid_path  = g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-%s.pid",    bridge_name);
    gchar *conf_path = g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-%s.conf",   bridge_name);
    gchar *lease_path= g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-%s.leases", bridge_name);
    gchar *meta_path = g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-%s.meta",   bridge_name);
                                                         
                                               
    GError *dhcp_stop_error = NULL;
    if (!network_dhcp_stop(bridge_name, &dhcp_stop_error)) {
        PCV_LOG_WARN("network", "network.delete DHCP stop soft-fail for %s: %s",
                     bridge_name,
                     dhcp_stop_error ? dhcp_stop_error->message : "unknown");
        g_clear_error(&dhcp_stop_error);
    }
    remove(conf_path); remove(pid_path); remove(lease_path); remove(meta_path);
    g_free(pid_path); g_free(conf_path); g_free(lease_path); g_free(meta_path);

                                                 
                                                         
                        
    {
        gchar *brif_path = g_build_filename(sysroot, bridge_name, "brif", NULL);
        GDir  *dir = g_dir_open(brif_path, 0, NULL);
        if (dir) {
            const gchar *slave;
                                                            
            while ((slave = g_dir_read_name(dir)) != NULL) {
                const gchar *nm[] = {"ip","link","set",slave,"nomaster",NULL};
                pcv_spawn_fire(nm);
            }
            g_dir_close(dir);
        }
        g_free(brif_path);
    }

    const gchar *del_argv[] = {"ip","link","delete",bridge_name,"type","bridge",NULL};
    gchar *std_err = NULL;
    pcv_spawn_sync(del_argv, NULL, &std_err, error);

                             
                                                             
                                                                     
    if (error && *error) {
        const gchar *msg = (*error)->message ? (*error)->message : "";
                                                    
                                                          
        if (strstr(msg, "Cannot find device") || strstr(msg, "does not exist")) {
                                            
            g_error_free(*error);
            *error = NULL;
        } else {
            g_free(std_err);
            return FALSE;
        }
    }

    g_free(std_err);
    return TRUE;
}

gboolean network_bridge_delete(const gchar *bridge_name, GError **error)
{
    return pcv_network_bridge_delete_at(bridge_name, NULL, error);
}

                                                          
  
                                              
                                                   
                               
  
                                    
                                                     
  
                          
                                                  
                                                                
                                                                      
                                                                
                                                        
static void _network_meta_save(const gchar *bridge_name, const gchar *mode, const gchar *cidr) {
    gchar *meta_path = g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-%s.meta", bridge_name);
    gchar *content   = g_strdup_printf(
        "{\"mode\":\"%s\",\"cidr\":\"%s\"}",
        mode  ? mode  : "nat",
        cidr  ? cidr  : "");
    if (g_file_set_contents(meta_path, content, -1, NULL)) {
                                                                 
                                                  
        if (chmod(meta_path, 0600) != 0) {
            PCV_LOG_WARN("network", "chmod 0600 failed on %s: %s",
                         meta_path, g_strerror(errno));
        }
    }
    g_free(meta_path);
    g_free(content);
}

   
                                            
  
                                               
                                                            
                                           
                                                                           
  
               
                                       
                                     
                                          
                                              
                                             
                                                    
                                               
                                                           
                                                              
                           
  
                
                                                           
                                                      
   
                                                                     
                                                          
                                                 
                                                     
                                                
void pcv_network_meta_save(const gchar *bridge_name, const gchar *mode, const gchar *cidr) {
    _network_meta_save(bridge_name, mode, cidr);
}

static gchar *_physical_state_path(const gchar *state_dir, const gchar *bridge_name)
{
    gchar *filename = g_strdup_printf("%s.json", bridge_name);
    gchar *path = g_build_filename(state_dir, filename, NULL);
    g_free(filename);
    return path;
}

                                                                        
                                                
                                                                        
                                                
gboolean
pcv_network_live_l3_mutation_allowed_at(const gchar *bridge_name,
                                        const gchar *state_dir,
                                        const gchar *sysfs_net_root,
                                        gchar **reason_out)
{
    g_return_val_if_fail(reason_out != NULL, FALSE);
    const gchar *desired_dir = state_dir ? state_dir : PCV_PHYSICAL_NETWORK_STATE_DIR;
    const gchar *sysroot = sysfs_net_root ? sysfs_net_root : "/sys/class/net";
    *reason_out = NULL;
    if (!pcv_validate_bridge_name(bridge_name)) {
        *reason_out = g_strdup("invalid bridge name for live L3 mutation");
        return FALSE;
    }

    gchar *state_path = _physical_state_path(desired_dir, bridge_name);
    GStatBuf state_stat = {0};
    if (g_lstat(state_path, &state_stat) == 0) {
        *reason_out = g_strdup(
            "physical bridge live L3 mutation is disabled; delete and recreate safely");
        g_free(state_path);
        return FALSE;
    }
    gint state_errno = errno;
    g_free(state_path);
    if (state_errno != ENOENT) {
        *reason_out = g_strdup_printf(
            "cannot inspect physical bridge desired state: %s",
            g_strerror(state_errno));
        return FALSE;
    }

    gchar *bridge_kind = g_build_filename(sysroot, bridge_name, "bridge", NULL);
    GStatBuf bridge_stat = {0};
    if (g_lstat(bridge_kind, &bridge_stat) != 0) {
        gint saved_errno = errno;
        *reason_out = saved_errno == ENOENT
            ? g_strdup("network bridge does not exist")
            : g_strdup_printf("cannot inspect network bridge: %s",
                              g_strerror(saved_errno));
        g_free(bridge_kind);
        return FALSE;
    }
    if (!S_ISDIR(bridge_stat.st_mode)) {
        *reason_out = g_strdup("target is not a Linux bridge");
        g_free(bridge_kind);
        return FALSE;
    }
    g_free(bridge_kind);

    gchar *host_uplink = NULL;
    GError *uplink_error = NULL;
    if (!pcv_network_bridge_has_host_uplink_at(
            bridge_name, sysfs_net_root, &host_uplink, &uplink_error)) {
        *reason_out = g_strdup(uplink_error ? uplink_error->message
                                            : "cannot inspect bridge uplinks");
        g_clear_error(&uplink_error);
        return FALSE;
    }
    if (host_uplink) {
        *reason_out = g_strdup_printf(
            "bridge has host uplink '%s'; live L3 mutation is disabled",
            host_uplink);
        g_free(host_uplink);
        return FALSE;
    }
    return TRUE;
}

static void _physical_state_clear(PcvPhysicalBridgeState *state)
{
    if (!state) return;
    g_clear_pointer(&state->bridge_name, g_free);
    g_clear_pointer(&state->physical_if, g_free);
    g_clear_pointer(&state->uplink_mode, g_free);
    g_clear_pointer(&state->phase, g_free);
    g_clear_pointer(&state->physical_mac, g_free);
    g_clear_pointer(&state->portal_bridge_if, g_free);
    g_clear_pointer(&state->portal_if, g_free);
    g_clear_pointer(&state->bpf_sha256, g_free);
    state->mtu = 0;
    state->physical_was_up = FALSE;
    state->promisc_was_on = FALSE;
    state->generation = 0;
}

static gboolean
_physical_state_save_full_at(const gchar *state_dir,
                             const PcvPhysicalBridgeState *state,
                             GError **error)
{
    if (!state_dir || !*state_dir || !state
        || !pcv_validate_bridge_name(state->bridge_name)
        || !pcv_validate_iface_name(state->physical_if)
        || (g_strcmp0(state->uplink_mode, "dedicated") != 0
            && g_strcmp0(state->uplink_mode, "shared") != 0)
        || !state->phase || !*state->phase) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "invalid physical bridge desired-state parameters");
        return FALSE;
    }
    if (g_mkdir_with_parents(state_dir, 0700) != 0 || chmod(state_dir, 0700) != 0) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                    "cannot prepare physical network state directory '%s': %s",
                    state_dir, g_strerror(errno));
        return FALSE;
    }

    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "schema_version");
    json_builder_add_int_value(builder, PCV_PHYSICAL_NETWORK_STATE_VERSION);
    json_builder_set_member_name(builder, "kind");
    json_builder_add_string_value(builder, "physical-bridge");
    json_builder_set_member_name(builder, "phase");
    json_builder_add_string_value(builder, state->phase);
    json_builder_set_member_name(builder, "bridge");
    json_builder_add_string_value(builder, state->bridge_name);
    json_builder_set_member_name(builder, "mode");
    json_builder_add_string_value(builder, "bridge");
    json_builder_set_member_name(builder, "uplink_mode");
    json_builder_add_string_value(builder, state->uplink_mode);
    json_builder_set_member_name(builder, "physical_if");
    json_builder_add_string_value(builder, state->physical_if);
    json_builder_set_member_name(builder, "physical_mac");
    json_builder_add_string_value(builder, state->physical_mac ? state->physical_mac : "");
    json_builder_set_member_name(builder, "mtu");
    json_builder_add_int_value(builder, state->mtu > 0 ? state->mtu : 1500);
    json_builder_set_member_name(builder, "physical_was_up");
    json_builder_add_boolean_value(builder, state->physical_was_up);
    json_builder_set_member_name(builder, "promisc_was_on");
    json_builder_add_boolean_value(builder, state->promisc_was_on);
    json_builder_set_member_name(builder, "dataplane");
    json_builder_add_string_value(builder,
        g_strcmp0(state->uplink_mode, "shared") == 0 ? "tc-bpf-portal" : "linux-bridge-port");
    json_builder_set_member_name(builder, "bpf_revision");
    json_builder_add_int_value(builder,
        g_strcmp0(state->uplink_mode, "shared") == 0 ? PCV_SHARED_BPF_REVISION : 0);
    json_builder_set_member_name(builder, "bpf_sha256");
    json_builder_add_string_value(builder, state->bpf_sha256 ? state->bpf_sha256 : "");
    json_builder_set_member_name(builder, "generation");
    json_builder_add_int_value(builder, state->generation);
    json_builder_set_member_name(builder, "portal_bridge_if");
    json_builder_add_string_value(builder,
                                  state->portal_bridge_if ? state->portal_bridge_if : "");
    json_builder_set_member_name(builder, "portal_if");
    json_builder_add_string_value(builder, state->portal_if ? state->portal_if : "");
    json_builder_set_member_name(builder, "tc_priority");
    json_builder_add_int_value(builder, PCV_SHARED_TC_PRIORITY);
    json_builder_set_member_name(builder, "tc_handle_phys_ingress");
    json_builder_add_int_value(builder, PCV_SHARED_TC_HANDLE_PHYS_INGRESS);
    json_builder_set_member_name(builder, "tc_handle_phys_egress");
    json_builder_add_int_value(builder, PCV_SHARED_TC_HANDLE_PHYS_EGRESS);
    json_builder_set_member_name(builder, "tc_handle_portal");
    json_builder_add_int_value(builder, PCV_SHARED_TC_HANDLE_PORTAL);
    json_builder_end_object(builder);
    JsonNode *root = json_builder_get_root(builder);
    JsonGenerator *generator = json_generator_new();
    json_generator_set_root(generator, root);
    gsize data_len = 0;
    gchar *data = json_generator_to_data(generator, &data_len);

    gchar *target = _physical_state_path(state_dir, state->bridge_name);
    gchar *tmp_template = g_build_filename(state_dir, ".physical-bridge-XXXXXX", NULL);
    gint fd = g_mkstemp_full(tmp_template, O_RDWR | O_CLOEXEC, 0600);
    gboolean ok = FALSE;
    if (fd < 0) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                    "cannot create desired-state temporary file: %s", g_strerror(errno));
        goto out;
    }
    gsize written = 0;
    while (written < data_len) {
        ssize_t n = write(fd, data + written, data_len - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                        "cannot write physical bridge desired state: %s", g_strerror(errno));
            goto close_out;
        }
        written += (gsize)n;
    }
    if (fsync(fd) != 0) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                    "cannot sync physical bridge desired state: %s", g_strerror(errno));
        goto close_out;
    }
    if (close(fd) != 0) {
        fd = -1;
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                    "cannot close physical bridge desired state: %s", g_strerror(errno));
        goto out;
    }
    fd = -1;
    if (g_rename(tmp_template, target) != 0) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                    "cannot commit physical bridge desired state: %s", g_strerror(errno));
        goto out;
    }
    gint dir_fd = open(state_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir_fd < 0 || fsync(dir_fd) != 0) {
        gint saved_errno = errno;
        if (dir_fd >= 0) close(dir_fd);
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved_errno),
                    "cannot sync physical network state directory: %s",
                    g_strerror(saved_errno));
        goto out;
    }
    close(dir_fd);
    ok = TRUE;
    goto out;

close_out:
    close(fd);
    fd = -1;
out:
    if (!ok) g_unlink(tmp_template);
    g_free(target);
    g_free(tmp_template);
    g_free(data);
    g_object_unref(generator);
    json_node_free(root);
    g_object_unref(builder);
    return ok;
}

gboolean
pcv_network_physical_state_save_at(const gchar *state_dir,
                                   const gchar *bridge_name,
                                   const gchar *physical_if,
                                   gint mtu,
                                   gboolean physical_was_up,
                                   GError **error)
{
    PcvPhysicalBridgeState state = {
        .bridge_name = (gchar *)bridge_name,
        .physical_if = (gchar *)physical_if,
        .uplink_mode = "dedicated",
        .phase = "active",
        .mtu = mtu,
        .physical_was_up = physical_was_up,
    };
    return _physical_state_save_full_at(state_dir, &state, error);
}

static gboolean
_physical_state_load_at(const gchar *state_dir,
                        const gchar *bridge_name,
                        PcvPhysicalBridgeState *state,
                        GError **error)
{
    memset(state, 0, sizeof(*state));
    if (!pcv_validate_bridge_name(bridge_name)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "invalid bridge name in desired state");
        return FALSE;
    }
    gchar *path = _physical_state_path(state_dir, bridge_name);
    JsonParser *parser = json_parser_new();
    gboolean ok = FALSE;
    if (!json_parser_load_from_file(parser, path, error)) goto out;
    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "physical bridge desired state is not a JSON object");
        goto out;
    }
    JsonObject *obj = json_node_get_object(root);
    gboolean legacy_v1 = json_object_has_member(obj, "version")
                         && !json_object_has_member(obj, "schema_version");
    if ((!legacy_v1 && !json_object_has_member(obj, "schema_version"))
        || !json_object_has_member(obj, "bridge")
        || !json_object_has_member(obj, "mode")
        || !json_object_has_member(obj, "physical_if")
        || !json_object_has_member(obj, "mtu")
        || !json_object_has_member(obj, "physical_was_up")) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "physical bridge desired state is incomplete");
        goto out;
    }
    gint version = (gint)json_object_get_int_member(
        obj, legacy_v1 ? "version" : "schema_version");
    const gchar *stored_bridge = json_object_get_string_member(obj, "bridge");
    const gchar *mode = json_object_get_string_member(obj, "mode");
    const gchar *physical_if = json_object_get_string_member(obj, "physical_if");
    gint mtu = (gint)json_object_get_int_member(obj, "mtu");
    if ((legacy_v1 ? version != 1 : version != PCV_PHYSICAL_NETWORK_STATE_VERSION)
        || g_strcmp0(stored_bridge, bridge_name) != 0
        || g_strcmp0(mode, "bridge") != 0
        || !pcv_validate_iface_name(physical_if)
        || mtu < 68 || mtu > 9216) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "physical bridge desired state failed schema validation");
        goto out;
    }
    state->bridge_name = g_strdup(stored_bridge);
    state->physical_if = g_strdup(physical_if);
    state->uplink_mode = g_strdup(legacy_v1 ? "dedicated"
        : json_object_get_string_member_with_default(obj, "uplink_mode", ""));
    state->phase = g_strdup(legacy_v1 ? "active"
        : json_object_get_string_member_with_default(obj, "phase", ""));
    state->mtu = mtu;
    state->physical_was_up = json_object_get_boolean_member(obj, "physical_was_up");
    state->promisc_was_on = !legacy_v1
        && json_object_get_boolean_member_with_default(obj, "promisc_was_on", FALSE);
    state->physical_mac = g_strdup(legacy_v1 ? ""
        : json_object_get_string_member_with_default(obj, "physical_mac", ""));
    state->portal_bridge_if = g_strdup(legacy_v1 ? ""
        : json_object_get_string_member_with_default(obj, "portal_bridge_if", ""));
    state->portal_if = g_strdup(legacy_v1 ? ""
        : json_object_get_string_member_with_default(obj, "portal_if", ""));
    state->bpf_sha256 = g_strdup(legacy_v1 ? ""
        : json_object_get_string_member_with_default(obj, "bpf_sha256", ""));
    state->generation = legacy_v1 ? 0
        : (guint32)json_object_get_int_member_with_default(obj, "generation", 0);

    gboolean shared = g_strcmp0(state->uplink_mode, "shared") == 0;
    gboolean dedicated = g_strcmp0(state->uplink_mode, "dedicated") == 0;
    if ((!shared && !dedicated)
        || (g_strcmp0(state->phase, "preparing") != 0
            && g_strcmp0(state->phase, "active") != 0)
        || (shared
            && (!pcv_validate_mac(state->physical_mac)
                || !pcv_validate_iface_name(state->portal_bridge_if)
                || !pcv_validate_iface_name(state->portal_if)
                || strlen(state->bpf_sha256) != 64
                || state->generation == 0))) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "physical bridge desired state failed uplink schema validation");
        goto out;
    }
    ok = TRUE;
out:
    if (!ok) _physical_state_clear(state);
    g_object_unref(parser);
    g_free(path);
    return ok;
}

static gboolean
_physical_state_remove_at(const gchar *state_dir,
                          const gchar *bridge_name,
                          GError **error)
{
    gchar *path = _physical_state_path(state_dir, bridge_name);
    if (g_unlink(path) != 0) {
        gint saved_errno = errno;
        if (saved_errno == ENOENT) {
            g_free(path);
            return TRUE;
        }
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved_errno),
                    "cannot remove physical bridge desired state '%s': %s",
                    path, g_strerror(saved_errno));
        g_free(path);
        return FALSE;
    }
    g_free(path);

    gint dir_fd = open(state_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir_fd < 0 || fsync(dir_fd) != 0) {
        gint saved_errno = errno;
        if (dir_fd >= 0) close(dir_fd);
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved_errno),
                    "cannot sync physical network state deletion: %s",
                    g_strerror(saved_errno));
        return FALSE;
    }
    close(dir_fd);
    return TRUE;
}

static gboolean _set_iface_admin_state(const gchar *iface, gboolean up, GError **error)
{
    const gchar *argv[] = {"ip", "link", "set", "dev", iface, up ? "up" : "down", NULL};
    gchar *std_err = NULL;
    gboolean ok = pcv_spawn_sync(argv, NULL, &std_err, error);
    if (!ok && error && !*error)
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "cannot restore interface '%s' %s: %s", iface, up ? "up" : "down",
                    std_err ? std_err : "unknown");
    g_free(std_err);
    return ok;
}

                                                       
                                                       
static gboolean _physical_force_detach_restore(const gchar *physical_if,
                                               gboolean was_up,
                                               GError **error)
{
    const gchar *detach[] = {"ip", "link", "set", physical_if, "nomaster", NULL};
    gchar *std_err = NULL;
    GError *detach_error = NULL;
    gboolean detached = pcv_spawn_sync(detach, NULL, &std_err, &detach_error);
    if (!detached) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "cannot rollback interface '%s' master: %s", physical_if,
                    detach_error ? detach_error->message : (std_err ? std_err : "unknown"));
    }
    g_clear_error(&detach_error);
    g_free(std_err);
    GError *admin_error = NULL;
    gboolean restored = _set_iface_admin_state(physical_if, was_up, &admin_error);
    if (!restored && error && !*error) g_propagate_error(error, admin_error);
    else g_clear_error(&admin_error);
    return detached && restored;
}

static gboolean
_shared_spawn(const gchar *const argv[], const gchar *description, GError **error)
{
    gchar *std_err = NULL;
    gboolean ok = pcv_spawn_sync(argv, NULL, &std_err, error);
    if (!ok && error && !*error)
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s: %s",
                    description, std_err ? std_err : "unknown");
    g_free(std_err);
    return ok;
}

                                                                   
                                                         
static gchar *
_shared_host_l3_snapshot(const gchar *physical_if, GError **error)
{
    const gchar *commands[][10] = {
        {"ip", "-brief", "address", "show", "dev", physical_if, NULL},
        {"ip", "-o", "route", "show", "table", "all", "dev", physical_if, NULL},
        {"ip", "-o", "-6", "route", "show", "table", "all", "dev", physical_if, NULL},
    };
    GString *snapshot = g_string_new(NULL);
    for (guint i = 0; i < G_N_ELEMENTS(commands); i++) {
        gchar *stdout_text = NULL;
        gchar *stderr_text = NULL;
        if (!pcv_spawn_sync(commands[i], &stdout_text, &stderr_text, error)) {
            if (error && !*error)
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "cannot snapshot host L3 on '%s': %s", physical_if,
                            stderr_text ? stderr_text : "unknown");
            g_free(stdout_text); g_free(stderr_text);
            g_string_free(snapshot, TRUE);
            return NULL;
        }
        g_string_append(snapshot, stdout_text ? stdout_text : "");
        g_string_append_c(snapshot, '\n');
        g_free(stdout_text); g_free(stderr_text);
    }
    gchar *resolv = NULL;
    if (!g_file_get_contents("/etc/resolv.conf", &resolv, NULL, error)) {
        g_string_free(snapshot, TRUE);
        return NULL;
    }
    g_string_append(snapshot, resolv);
    g_free(resolv);
    return g_string_free(snapshot, FALSE);
}

static gboolean
_shared_mac_parse(const gchar *text, guint8 mac[6])
{
    guint octets[6] = {0};
    if (!pcv_validate_mac(text)
        || sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x",
                  &octets[0], &octets[1], &octets[2],
                  &octets[3], &octets[4], &octets[5]) != 6)
        return FALSE;
    for (guint i = 0; i < 6; i++) mac[i] = (guint8)octets[i];
    return TRUE;
}

static gboolean
_shared_internal_link_unnumber(const gchar *interface_name, GError **error)
{
                                                                 
                                                                
                                                 
                                             
    const gchar *disable_autoconf[] = {
        "ip", "link", "set", "dev", interface_name, "addrgenmode", "none", NULL
    };
    const gchar *flush_link_local[] = {
        "ip", "-6", "address", "flush", "dev", interface_name, "scope", "link", NULL
    };
    return _shared_spawn(disable_autoconf,
                         "cannot disable shared internal IPv6 address generation", error)
        && _shared_spawn(flush_link_local,
                         "cannot clear shared internal link-local address", error);
}

static gboolean
_shared_portal_create(const PcvPhysicalBridgeState *state, GError **error)
{
    gboolean pair_created = FALSE;
    gchar mtu_text[16];
    g_snprintf(mtu_text, sizeof(mtu_text), "%d", state->mtu);
    const gchar *commands[][12] = {
        {"ip", "link", "add", "name", state->portal_bridge_if, "type", "veth",
         "peer", "name", state->portal_if, NULL},
        {"ip", "link", "set", "dev", state->portal_bridge_if, "mtu", mtu_text, NULL},
        {"ip", "link", "set", "dev", state->portal_if, "mtu", mtu_text, NULL},
        {"ip", "link", "set", state->portal_bridge_if, "master", state->bridge_name, NULL},
    };
    for (guint i = 0; i < G_N_ELEMENTS(commands); i++) {
        if (!_shared_spawn(commands[i], "cannot create shared bridge portal", error))
            goto rollback;
        if (i == 0) pair_created = TRUE;
    }
    if (!_shared_internal_link_unnumber(state->portal_bridge_if, error)
        || !_shared_internal_link_unnumber(state->portal_if, error))
        goto rollback;
    const gchar *bridge_end_up[] = {
        "ip", "link", "set", "dev", state->portal_bridge_if, "up", NULL
    };
    const gchar *portal_up[] = {
        "ip", "link", "set", "dev", state->portal_if, "up", NULL
    };
    if (_shared_spawn(bridge_end_up, "cannot enable shared bridge portal", error)
        && _shared_spawn(portal_up, "cannot enable shared bridge portal", error))
        return TRUE;
rollback:
                                                       
                                                       
    if (pair_created) {
        const gchar *drop[] = {
            "ip", "link", "delete", state->portal_bridge_if, NULL
        };
        _shared_spawn(drop, "cannot roll back shared bridge portal", NULL);
    }
    return FALSE;
}

static gboolean
_shared_portal_delete(const gchar *portal_bridge_if, GError **error)
{
    if (if_nametoindex(portal_bridge_if) == 0) return TRUE;
    const gchar *argv[] = {"ip", "link", "delete", portal_bridge_if, NULL};
    return _shared_spawn(argv, "cannot delete shared bridge portal", error);
}

static gboolean
_shared_promisc_set(const gchar *physical_if, gboolean on, GError **error)
{
    const gchar *argv[] = {
        "ip", "link", "set", "dev", physical_if, "promisc", on ? "on" : "off", NULL
    };
    return _shared_spawn(argv, "cannot update shared uplink promiscuous mode", error);
}

static gboolean
_shared_iface_unclaimed(const gchar *state_dir,
                        const gchar *physical_if,
                        const gchar *bridge_name,
                        GError **error)
{
    if (!g_file_test(state_dir, G_FILE_TEST_IS_DIR)) return TRUE;
    GDir *dir = g_dir_open(state_dir, 0, error);
    if (!dir) return FALSE;
    const gchar *entry = NULL;
    gboolean ok = TRUE;
    while ((entry = g_dir_read_name(dir)) != NULL) {
        if (!g_str_has_suffix(entry, ".json")) continue;
        gchar *other_bridge = g_strndup(entry, strlen(entry) - strlen(".json"));
        if (g_strcmp0(other_bridge, bridge_name) != 0) {
            PcvPhysicalBridgeState other = {0};
            GError *local = NULL;
            if (!_physical_state_load_at(state_dir, other_bridge, &other, &local)) {
                g_propagate_prefixed_error(error, local,
                                           "cannot prove shared uplink ownership: ");
                ok = FALSE;
            } else if (g_strcmp0(other.uplink_mode, "shared") == 0
                       && g_strcmp0(other.physical_if, physical_if) == 0) {
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                            "physical interface '%s' is already shared by bridge '%s'",
                            physical_if, other.bridge_name);
                ok = FALSE;
            }
            _physical_state_clear(&other);
        }
        g_free(other_bridge);
        if (!ok) break;
    }
    g_dir_close(dir);
    return ok;
}

gboolean
pcv_network_shared_bridge_create_at(const gchar *bridge_name,
                                    const gchar *physical_if,
                                    gint mtu,
                                    const gchar *sysfs_net_root,
                                    const gchar *proc_root,
                                    const gchar *state_dir,
                                    GError **error)
{
    const gchar *desired_dir = state_dir ? state_dir : PCV_PHYSICAL_NETWORK_STATE_DIR;
    gboolean production_view = !sysfs_net_root && !proc_root && !state_dir;
    PcvSharedIfaceFacts facts = {0};
    PcvPhysicalBridgeState state = {0};
    gboolean state_saved = FALSE, bridge_created = FALSE, portal_created = FALSE;
    gboolean promisc_changed = FALSE, bpf_attached = FALSE, ok = FALSE;
    gchar *before = NULL;
    g_mutex_lock(&g_physical_bridge_mu);

    gchar *state_path = _physical_state_path(desired_dir, bridge_name);
    if (g_file_test(state_path, G_FILE_TEST_EXISTS)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                    "physical bridge '%s' already has desired state", bridge_name);
        g_free(state_path);
        goto out;
    }
    g_free(state_path);
    if (!pcv_network_iface_preflight_shared(
            physical_if, sysfs_net_root, proc_root, &facts, error)
        || !_shared_iface_unclaimed(desired_dir, physical_if, bridge_name, error))
        goto out;
    if (!pcv_shared_bridge_bpf_is_prepared()) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                    "shared bridge BPF dataplane is not prepared");
        goto out;
    }
    gint effective_mtu = mtu > 0 ? mtu : facts.mtu;
    if (effective_mtu != facts.mtu) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "shared bridge MTU %d must match physical interface MTU %d",
                    effective_mtu, facts.mtu);
        goto out;
    }
    if (production_view && !(before = _shared_host_l3_snapshot(physical_if, error))) goto out;

    gchar portal_bridge[16] = {0}, portal[16] = {0};
    pcv_shared_bridge_portal_names(bridge_name, portal_bridge, portal);
    guint32 generation = (guint32)g_get_real_time();
    if (generation == 0) generation = 1;
    state.bridge_name = (gchar *)bridge_name;
    state.physical_if = (gchar *)physical_if;
    state.uplink_mode = "shared";
    state.phase = "preparing";
    state.physical_mac = facts.mac;
    state.portal_bridge_if = portal_bridge;
    state.portal_if = portal;
    state.bpf_sha256 = (gchar *)pcv_shared_bridge_bpf_sha256();
    state.mtu = effective_mtu;
    state.physical_was_up = facts.was_up;
    state.promisc_was_on = facts.promisc_was_on;
    state.generation = generation;
    if (!_physical_state_save_full_at(desired_dir, &state, error)) goto out;
    state_saved = TRUE;
    if (!network_bridge_create(bridge_name, NULL, effective_mtu, error)) goto rollback;
    bridge_created = TRUE;
    if (!_shared_internal_link_unnumber(bridge_name, error)) goto rollback;
    if (!_shared_portal_create(&state, error)) goto rollback;
    portal_created = TRUE;
    if (!facts.promisc_was_on) {
        if (!_shared_promisc_set(physical_if, TRUE, error)) goto rollback;
        promisc_changed = TRUE;
    }
    guint8 mac[6] = {0};
    if (!_shared_mac_parse(facts.mac, mac)
        || !pcv_shared_bridge_attach(
            physical_if, portal, mac, (guint32)effective_mtu, generation, error))
        goto rollback;
    bpf_attached = TRUE;
    if (production_view) {
        PcvSharedIfaceFacts after_facts = {0};
        gchar *after = NULL;
        if (!pcv_network_iface_preflight_shared(
                physical_if, NULL, NULL, &after_facts, error)
            || !(after = _shared_host_l3_snapshot(physical_if, error))) {
            g_free(after);
            goto rollback;
        }
        gboolean preserved = g_strcmp0(before, after) == 0
            && g_strcmp0(facts.mac, after_facts.mac) == 0
            && facts.mtu == after_facts.mtu;
        g_free(after);
        if (!preserved) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "shared bridge changed host address, route, DNS, MAC, or MTU; rolled back");
            goto rollback;
        }
    }
    state.phase = "active";
    if (!_physical_state_save_full_at(desired_dir, &state, error)) goto rollback;
    ok = TRUE;
    goto out;

rollback:
    if (bpf_attached) {
        GError *cleanup = NULL;
        pcv_shared_bridge_detach(physical_if, portal, &cleanup);
        g_clear_error(&cleanup);
    }
    if (promisc_changed) _shared_promisc_set(physical_if, FALSE, NULL);
    if (portal_created) _shared_portal_delete(portal_bridge, NULL);
    if (bridge_created) network_bridge_delete(bridge_name, NULL);
    if (state_saved) _physical_state_remove_at(desired_dir, bridge_name, NULL);
out:
    g_free(before);
    g_mutex_unlock(&g_physical_bridge_mu);
    return ok;
}

gboolean
pcv_network_physical_bridge_create_at(const gchar *bridge_name,
                                      const gchar *physical_if,
                                      gint mtu,
                                      const gchar *sysfs_net_root,
                                      const gchar *proc_root,
                                      const gchar *state_dir,
                                      GError **error)
{
    const gchar *desired_dir = state_dir ? state_dir : PCV_PHYSICAL_NETWORK_STATE_DIR;
    gboolean was_up = FALSE;
    gboolean bridge_created = FALSE;
    gboolean bind_started = FALSE;
    gboolean ok = FALSE;
    g_mutex_lock(&g_physical_bridge_mu);

    gchar *state_path = _physical_state_path(desired_dir, bridge_name);
    GStatBuf state_stat = {0};
    if (g_lstat(state_path, &state_stat) == 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                    "physical bridge '%s' already has desired state; reconcile or delete it first",
                    bridge_name);
        g_free(state_path);
        goto out;
    }
    gint state_errno = errno;
    g_free(state_path);
    if (state_errno != ENOENT) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(state_errno),
                    "cannot inspect existing physical bridge desired state: %s",
                    g_strerror(state_errno));
        goto out;
    }

    if (!pcv_network_iface_preflight_dedicated(physical_if, sysfs_net_root, proc_root,
                                               &was_up, error))
        goto out;
    if (!network_bridge_create(bridge_name, NULL, mtu, error)) goto out;
    bridge_created = TRUE;
    bind_started = TRUE;
    if (!network_bridge_bind_physical(bridge_name, physical_if, error)) goto rollback;
    if (!pcv_network_physical_state_save_at(desired_dir, bridge_name, physical_if,
                                            mtu, was_up, error))
        goto rollback;
    ok = TRUE;
    goto out;

rollback:
    if (bind_started) {
        GError *cleanup = NULL;
        if (!_physical_force_detach_restore(physical_if, was_up, &cleanup))
            PCV_LOG_WARN(NET_LOG_DOM, "physical NIC rollback failed for %s: %s",
                         physical_if, cleanup ? cleanup->message : "unknown");
        g_clear_error(&cleanup);
    }
    if (bridge_created) {
        GError *cleanup = NULL;
        if (!network_bridge_delete(bridge_name, &cleanup))
            PCV_LOG_WARN(NET_LOG_DOM, "bridge rollback failed for %s: %s",
                         bridge_name, cleanup ? cleanup->message : "unknown");
        g_clear_error(&cleanup);
    }
out:
    g_mutex_unlock(&g_physical_bridge_mu);
    return ok;
}

static gboolean
_physical_detach_restore_at(const gchar *bridge_name,
                            const gchar *physical_if,
                            gboolean was_up,
                            const gchar *sysfs_net_root,
                            GError **error)
{
    const gchar *sysroot = sysfs_net_root ? sysfs_net_root : "/sys/class/net";
    gchar *iface_path = g_build_filename(sysroot, physical_if, NULL);
    if (!g_file_test(iface_path, G_FILE_TEST_IS_DIR)) {
        g_free(iface_path);
        return TRUE;                                  
    }
    g_free(iface_path);
    gchar *master = NULL;
    if (!_iface_master_read(sysroot, physical_if, &master, error)) return FALSE;
    if (master && g_strcmp0(master, bridge_name) != 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "interface '%s' moved to unexpected master '%s'; refusing to detach",
                    physical_if, master);
        g_free(master);
        return FALSE;
    }
    if (master) {
        const gchar *argv[] = {"ip", "link", "set", physical_if, "nomaster", NULL};
        gchar *std_err = NULL;
        if (!pcv_spawn_sync(argv, NULL, &std_err, error)) {
            if (error && !*error)
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "cannot detach interface '%s': %s", physical_if,
                            std_err ? std_err : "unknown");
            g_free(std_err);
            g_free(master);
            return FALSE;
        }
        g_free(std_err);
    }
    g_free(master);
    return _set_iface_admin_state(physical_if, was_up, error);
}

static gboolean
_shared_bridge_idle(const PcvPhysicalBridgeState *state,
                    const gchar *sysfs_net_root,
                    GError **error)
{
    const gchar *sysroot = sysfs_net_root ? sysfs_net_root : "/sys/class/net";
    gchar *bridge_path = g_build_filename(sysroot, state->bridge_name, NULL);
    if (!g_file_test(bridge_path, G_FILE_TEST_IS_DIR)) {
        g_free(bridge_path);
        return TRUE;
    }
    gchar *brif = g_build_filename(bridge_path, "brif", NULL);
    g_free(bridge_path);
    GDir *dir = g_dir_open(brif, 0, error);
    g_free(brif);
    if (!dir) return FALSE;
    const gchar *entry = NULL;
    gboolean idle = TRUE;
    while ((entry = g_dir_read_name(dir)) != NULL) {
        if (g_strcmp0(entry, state->portal_bridge_if) == 0) continue;
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "shared bridge '%s' still has guest port '%s'; detach guests first",
                    state->bridge_name, entry);
        idle = FALSE;
        break;
    }
    g_dir_close(dir);
    return idle;
}

gboolean
pcv_network_physical_bridge_delete_at(const gchar *bridge_name,
                                      const gchar *sysfs_net_root,
                                      const gchar *state_dir,
                                      GError **error)
{
    const gchar *desired_dir = state_dir ? state_dir : PCV_PHYSICAL_NETWORK_STATE_DIR;
    if (!pcv_validate_bridge_name(bridge_name)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "invalid physical bridge name");
        return FALSE;
    }

    gboolean ok = FALSE;
    PcvPhysicalBridgeState state = {0};
    g_mutex_lock(&g_physical_bridge_mu);
    if (!_physical_state_load_at(desired_dir, bridge_name, &state, error)) goto out;
    if (g_strcmp0(state.uplink_mode, "shared") == 0) {
        if (!_shared_bridge_idle(&state, sysfs_net_root, error)) goto out;
        if (!pcv_shared_bridge_detach(state.physical_if, state.portal_if, error)) goto out;
        if (!state.promisc_was_on && if_nametoindex(state.physical_if) != 0
            && !_shared_promisc_set(state.physical_if, FALSE, error))
            goto out;
        if (!_shared_portal_delete(state.portal_bridge_if, error)) goto out;
    } else if (!_physical_detach_restore_at(
                   state.bridge_name, state.physical_if,
                   state.physical_was_up, sysfs_net_root, error)) {
        goto out;
    }
    if (!network_bridge_delete(bridge_name, error)) goto out;
    if (!_physical_state_remove_at(desired_dir, bridge_name, error)) goto out;
    ok = TRUE;
out:
    _physical_state_clear(&state);
    g_mutex_unlock(&g_physical_bridge_mu);
    return ok;
}

gboolean
pcv_network_bridge_uplink_mode(const gchar *bridge_name,
                               gchar **uplink_mode_out,
                               GError **error)
{
    g_return_val_if_fail(uplink_mode_out != NULL, FALSE);
    *uplink_mode_out = NULL;
    if (!pcv_validate_bridge_name(bridge_name)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "invalid bridge name for uplink mode lookup");
        return FALSE;
    }
    gchar *path = _physical_state_path(PCV_PHYSICAL_NETWORK_STATE_DIR, bridge_name);
    gboolean exists = g_file_test(path, G_FILE_TEST_EXISTS);
    g_free(path);
    if (!exists) return TRUE;
    PcvPhysicalBridgeState state = {0};
    if (!_physical_state_load_at(
            PCV_PHYSICAL_NETWORK_STATE_DIR, bridge_name, &state, error))
        return FALSE;
    *uplink_mode_out = g_strdup(state.uplink_mode);
    _physical_state_clear(&state);
    return TRUE;
}

static gboolean
_shared_bridge_reconcile(PcvPhysicalBridgeState *state,
                         const gchar *desired_dir,
                         const gchar *sysfs_net_root,
                         const gchar *proc_root,
                         gboolean production_view,
                         GError **error)
{
    const gchar *sysroot = sysfs_net_root ? sysfs_net_root : "/sys/class/net";
    PcvSharedIfaceFacts facts = {0};
    if (!pcv_shared_bridge_bpf_is_prepared()) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                    "shared bridge BPF dataplane is not prepared");
        return FALSE;
    }
    if (!pcv_network_iface_preflight_shared(
            state->physical_if, sysfs_net_root, proc_root, &facts, error))
        return FALSE;
    if (g_strcmp0(facts.mac, state->physical_mac) != 0 || facts.mtu != state->mtu) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "shared uplink '%s' MAC or MTU changed; refusing automatic rewire",
                    state->physical_if);
        return FALSE;
    }
    gchar *before = production_view ? _shared_host_l3_snapshot(state->physical_if, error) : NULL;
    if (production_view && !before) return FALSE;

    gchar *bridge_path = g_build_filename(sysroot, state->bridge_name, NULL);
    gchar *bridge_kind = g_build_filename(bridge_path, "bridge", NULL);
    gboolean bridge_exists = g_file_test(bridge_path, G_FILE_TEST_IS_DIR);
    if (bridge_exists && !g_file_test(bridge_kind, G_FILE_TEST_IS_DIR)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "shared bridge name exists but is not a Linux bridge");
        g_free(before); g_free(bridge_kind); g_free(bridge_path);
        return FALSE;
    }
    if (!bridge_exists && !network_bridge_create(
            state->bridge_name, NULL, state->mtu, error)) {
        g_free(before); g_free(bridge_kind); g_free(bridge_path);
        return FALSE;
    }
    g_free(bridge_kind); g_free(bridge_path);
    if (!_shared_internal_link_unnumber(state->bridge_name, error)) {
        g_free(before);
        return FALSE;
    }

    gchar *bridge_end_path = g_build_filename(sysroot, state->portal_bridge_if, NULL);
    gchar *portal_path = g_build_filename(sysroot, state->portal_if, NULL);
    gboolean bridge_end_exists = g_file_test(bridge_end_path, G_FILE_TEST_IS_DIR);
    gboolean portal_exists = g_file_test(portal_path, G_FILE_TEST_IS_DIR);
    g_free(bridge_end_path); g_free(portal_path);
    if (bridge_end_exists != portal_exists) {
        const gchar *orphan = bridge_end_exists ? state->portal_bridge_if : state->portal_if;
        const gchar *drop[] = {"ip", "link", "delete", orphan, NULL};
        if (!_shared_spawn(drop, "cannot remove orphan shared portal", error)) {
            g_free(before);
            return FALSE;
        }
        bridge_end_exists = portal_exists = FALSE;
    }
    if (!bridge_end_exists) {
        if (!_shared_portal_create(state, error)) {
            g_free(before);
            return FALSE;
        }
    } else {
        gchar *master = NULL;
        if (!_iface_master_read(sysroot, state->portal_bridge_if, &master, error)) {
            g_free(before);
            return FALSE;
        }
        if (master && g_strcmp0(master, state->bridge_name) != 0) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                        "shared portal belongs to unexpected master '%s'", master);
            g_free(master); g_free(before);
            return FALSE;
        }
        if (!master) {
            const gchar *set_master[] = {
                "ip", "link", "set", state->portal_bridge_if,
                "master", state->bridge_name, NULL
            };
            if (!_shared_spawn(set_master, "cannot restore shared portal master", error)) {
                g_free(before);
                return FALSE;
            }
        }
        g_free(master);
        if (!_shared_internal_link_unnumber(state->portal_bridge_if, error)
            || !_shared_internal_link_unnumber(state->portal_if, error)) {
            g_free(before);
            return FALSE;
        }
        gchar mtu_text[16];
        g_snprintf(mtu_text, sizeof(mtu_text), "%d", state->mtu);
        const gchar *commands[][9] = {
            {"ip", "link", "set", "dev", state->portal_bridge_if, "mtu", mtu_text, NULL},
            {"ip", "link", "set", "dev", state->portal_if, "mtu", mtu_text, NULL},
            {"ip", "link", "set", "dev", state->portal_bridge_if, "up", NULL},
            {"ip", "link", "set", "dev", state->portal_if, "up", NULL},
        };
        for (guint i = 0; i < G_N_ELEMENTS(commands); i++) {
            if (!_shared_spawn(commands[i], "cannot restore shared portal", error)) {
                g_free(before);
                return FALSE;
            }
        }
    }
    if (!facts.promisc_was_on && !_shared_promisc_set(state->physical_if, TRUE, error)) {
        g_free(before);
        return FALSE;
    }
    guint8 mac[6] = {0};
    if (!_shared_mac_parse(facts.mac, mac)
        || !pcv_shared_bridge_attach(state->physical_if, state->portal_if, mac,
                                     (guint32)state->mtu, state->generation, error)) {
        g_free(before);
        return FALSE;
    }
    if (production_view) {
        gchar *after = _shared_host_l3_snapshot(state->physical_if, error);
        gboolean preserved = after && g_strcmp0(before, after) == 0;
        g_free(after); g_free(before);
        if (!preserved) {
            pcv_shared_bridge_detach(state->physical_if, state->portal_if, NULL);
                                                             
                                                                       
            if (!error || !*error) {
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "shared reconcile changed host L3 state; dataplane detached");
            }
            return FALSE;
        }
    }
    g_free(state->phase);
    state->phase = g_strdup("active");
    g_free(state->bpf_sha256);
    state->bpf_sha256 = g_strdup(pcv_shared_bridge_bpf_sha256());
    if (!_physical_state_save_full_at(desired_dir, state, error)) return FALSE;
    if (production_view) _network_meta_save(state->bridge_name, "bridge", NULL);
    return TRUE;
}

gboolean
pcv_network_reconcile_physical_bridges_at(const gchar *state_dir,
                                          const gchar *sysfs_net_root,
                                          const gchar *proc_root,
                                          GError **error)
{
    const gchar *desired_dir = state_dir ? state_dir : PCV_PHYSICAL_NETWORK_STATE_DIR;
    const gchar *sysroot = sysfs_net_root ? sysfs_net_root : "/sys/class/net";
    gboolean production_view = !state_dir && !sysfs_net_root && !proc_root;
    if (!g_file_test(desired_dir, G_FILE_TEST_IS_DIR)) return TRUE;
    GError *dir_error = NULL;
    GDir *dir = g_dir_open(desired_dir, 0, &dir_error);
    if (!dir) {
        g_propagate_error(error, dir_error);
        return FALSE;
    }
    GString *failures = g_string_new(NULL);
    const gchar *entry = NULL;
    g_mutex_lock(&g_physical_bridge_mu);
    while ((entry = g_dir_read_name(dir)) != NULL) {
        if (!g_str_has_suffix(entry, ".json")) continue;
        gchar *bridge_name = g_strndup(entry, strlen(entry) - strlen(".json"));
        PcvPhysicalBridgeState state = {0};
        GError *local = NULL;
        if (!_physical_state_load_at(desired_dir, bridge_name, &state, &local)) {
            g_string_append_printf(failures, "%s: %s; ", bridge_name,
                                   local ? local->message : "invalid state");
            g_clear_error(&local);
            g_free(bridge_name);
            continue;
        }

        if (g_strcmp0(state.uplink_mode, "shared") == 0) {
            if (!_shared_bridge_reconcile(&state, desired_dir,
                                          sysfs_net_root, proc_root,
                                          production_view, &local)) {
                g_string_append_printf(failures, "%s: %s; ", state.bridge_name,
                                       local ? local->message : "shared reconcile failed");
                g_clear_error(&local);
            }
            _physical_state_clear(&state);
            g_free(bridge_name);
            continue;
        }

        gchar *bridge_path = g_build_filename(sysroot, state.bridge_name, NULL);
        gchar *bridge_kind = g_build_filename(bridge_path, "bridge", NULL);
        gboolean bridge_exists = g_file_test(bridge_path, G_FILE_TEST_IS_DIR);
        gboolean bridge_is_linux = g_file_test(bridge_kind, G_FILE_TEST_IS_DIR);
        gchar *master = NULL;
        if (!_iface_master_read(sysroot, state.physical_if, &master, &local)) {
            g_string_append_printf(failures, "%s: %s; ", state.bridge_name,
                                   local ? local->message : "cannot inspect master");
            g_clear_error(&local);
            g_free(bridge_path);
            g_free(bridge_kind);
            _physical_state_clear(&state);
            g_free(bridge_name);
            continue;
        }
        gboolean converged = bridge_exists && bridge_is_linux
                             && g_strcmp0(master, state.bridge_name) == 0;
        if (bridge_exists && !bridge_is_linux) {
            g_set_error(&local, G_IO_ERROR, G_IO_ERROR_BUSY,
                        "name exists but is not a Linux bridge");
        } else if (master && g_strcmp0(master, state.bridge_name) != 0) {
            g_set_error(&local, G_IO_ERROR, G_IO_ERROR_BUSY,
                        "uplink belongs to unexpected master '%s'", master);
        } else if (converged) {
            gboolean ignored_was_up = FALSE;
            if (_iface_preflight_dedicated(state.physical_if, state.bridge_name,
                                           sysfs_net_root, proc_root,
                                           &ignored_was_up, &local)) {
                if (production_view) _network_meta_save(state.bridge_name, "bridge", NULL);
            }
        } else {
            gboolean was_up_now = FALSE;
            if (!pcv_network_iface_preflight_dedicated(state.physical_if,
                                                       sysfs_net_root, proc_root,
                                                       &was_up_now, &local)) {
                                                                                      
            } else {
                gboolean created = FALSE;
                if (!bridge_exists) {
                    created = network_bridge_create(state.bridge_name, NULL, state.mtu, &local);
                }
                if ((bridge_exists || created)
                    && network_bridge_bind_physical(state.bridge_name, state.physical_if, &local)) {
                    if (production_view) _network_meta_save(state.bridge_name, "bridge", NULL);
                } else if (created) {
                    GError *cleanup = NULL;
                    network_bridge_delete(state.bridge_name, &cleanup);
                    g_clear_error(&cleanup);
                }
            }
        }
        if (local) {
            g_string_append_printf(failures, "%s: %s; ", state.bridge_name, local->message);
            g_clear_error(&local);
        }
        g_free(master);
        g_free(bridge_path);
        g_free(bridge_kind);
        _physical_state_clear(&state);
        g_free(bridge_name);
    }
    g_mutex_unlock(&g_physical_bridge_mu);
    g_dir_close(dir);
    if (failures->len) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "physical bridge reconcile failed: %s", failures->str);
        g_string_free(failures, TRUE);
        return FALSE;
    }
    g_string_free(failures, TRUE);
    return TRUE;
}

gboolean pcv_network_reconcile_physical_bridges(GError **error)
{
    return pcv_network_reconcile_physical_bridges_at(NULL, NULL, NULL, error);
}

                                                              
                                                           
                                 
  
                                                
                                                     
                                            
                            
  
                                                    
                                                      
                                                       
                                                        
                                                
static gchar *_network_meta_get_mode(const gchar *bridge_name) {
    gchar *meta_path = g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-%s.meta", bridge_name);
    gchar *content   = NULL;
    gchar *mode_out  = NULL;

    if (g_file_get_contents(meta_path, &content, NULL, NULL) && content) {
                                           
        const gchar *hit = strstr(content, "\"mode\":\"");
        if (hit) {
            hit += 8;                     
            const gchar *end = strchr(hit, '"');
            if (end) {
                mode_out = g_strndup(hit, end - hit);
            }
        }
        g_free(content);
    }
    g_free(meta_path);

                                                        
    if (!mode_out) {
                                         
        if (g_str_has_prefix(bridge_name, "lxcbr"))
            mode_out = g_strdup("nat");
                                       
        else if (g_str_has_prefix(bridge_name, "virbr"))
            mode_out = g_strdup("nat");
                                     
        else if (g_strcmp0(bridge_name, "docker0") == 0)
            mode_out = g_strdup("nat");
                                 
        else if (g_str_has_prefix(bridge_name, "br-"))
            mode_out = g_strdup("bridge");
                                                      
        else if (g_str_has_prefix(bridge_name, "pcvbr"))
            mode_out = g_strdup("bridge");
                                      
        else if (g_str_has_prefix(bridge_name, "pcvoverlay"))
            mode_out = g_strdup("bridge");
        else {
                                          
            gchar *lxc_conf = NULL;
            if (g_file_get_contents("/etc/lxc/default.conf", &lxc_conf, NULL, NULL)) {
                gchar *needle = g_strdup_printf("lxc.net.0.link = %s", bridge_name);
                if (strstr(lxc_conf, needle))
                    mode_out = g_strdup("nat");
                g_free(needle);
                g_free(lxc_conf);
            }
                                           
                                                           
                                                             
                                                      
                                             
            if (!mode_out) {
                gchar *brif = g_strdup_printf("/sys/class/net/%s/brif", bridge_name);
                GDir *d = g_dir_open(brif, 0, NULL);
                if (d) {
                    const gchar *ifn;
                    while ((ifn = g_dir_read_name(d))) {
                                                             
                        if (!g_str_has_prefix(ifn, "vnet") &&
                            !g_str_has_prefix(ifn, "tap") &&
                            !g_str_has_prefix(ifn, "veth")) {
                            mode_out = g_strdup("bridge");
                            break;
                        }
                    }
                    g_dir_close(d);
                }
                g_free(brif);
            }
            if (!mode_out)
                mode_out = g_strdup("unknown");
        }
    }

    return mode_out;
}

                                                                    
                              
  
              
              
                                                                  
                                 
  
              
                                                       
                                 
  
            
                                        
                                      
  
            
                                               
                                  
                                    
  
                      
                                                
                                              
                                
                                                
                                                                       
   
                                              
                                             
                        
                                 
                         
  
                                             
                                                             
                                                
                                            
  
                                                       
                                                    
                                                                      
  
                             
                                      
                                       
                                            
                                             
                               
                   
  
                                                   
                                                           
   
static void network_create_worker(GTask *task, gpointer source_obj, gpointer task_data, GCancellable *cancellable) {
    NetworkCtx *ctx = (NetworkCtx *)task_data;
    GError *error = NULL;
    gboolean bridge_created = FALSE;

                                                                
                              
    const gchar *cidr = (g_strcmp0(ctx->mode, "bridge") == 0) ? NULL : ctx->cidr;

    if (g_strcmp0(ctx->mode, "bridge") == 0) {
        gboolean created = g_strcmp0(ctx->uplink_mode, "shared") == 0
            ? pcv_network_shared_bridge_create_at(
                ctx->bridge_name, ctx->physical_if, ctx->mtu,
                NULL, NULL, NULL, &error)
            : pcv_network_physical_bridge_create_at(
                ctx->bridge_name, ctx->physical_if, ctx->mtu,
                NULL, NULL, NULL, &error);
        if (!created)
            goto fail;
                                                                    
                                      
        goto post_mode_setup;
    }

                                                           
    if (!network_bridge_create(ctx->bridge_name, cidr, ctx->mtu, &error)) {
        goto fail;
    }
    bridge_created = TRUE;

                                                          
                                                                
    if (g_strcmp0(ctx->mode, "nat") == 0 || ctx->mode == NULL) {
                                                                    
        if (!network_firewall_setup_nat(ctx->bridge_name, ctx->cidr, &error)) {
            goto fail;
        }
        GError *dhcp_err = NULL;
        if (!network_dhcp_start_ex(ctx->bridge_name, ctx->cidr,
                                ctx->dns_enabled, ctx->upstream_dns, &dhcp_err)) {
                                                
                                                   
                                       
            ctx->dhcp_warning = g_strdup(dhcp_err ? dhcp_err->message : "dnsmasq start failed");
            g_error_free(dhcp_err);
        }
    }
    else if (g_strcmp0(ctx->mode, "isolated") == 0) {
                                                 
        if (!network_firewall_setup_isolated(ctx->bridge_name, ctx->cidr, &error)) {
            goto fail;
        }
        GError *dhcp_err = NULL;
        if (!network_dhcp_start_ex(ctx->bridge_name, ctx->cidr,
                                ctx->dns_enabled, ctx->upstream_dns, &dhcp_err)) {
            ctx->dhcp_warning = g_strdup(dhcp_err ? dhcp_err->message : "dnsmasq start failed");
            g_error_free(dhcp_err);
        }
    }
    else if (g_strcmp0(ctx->mode, "routed") == 0) {
                                                          
        if (!network_firewall_setup_routed(ctx->bridge_name, ctx->cidr, &error)) {
            goto fail;
        }
    }
post_mode_setup:

                                                        
                                                    
                                                         
                                                                      
                                                  
                                                                
                                                       
    if (ctx->ipv6_prefix && ctx->ipv6_prefix[0]
        && !pcv_validate_ipv6_prefix(ctx->ipv6_prefix)) {
        gchar *warn = g_strdup("IPv6 setup skipped: invalid ipv6_prefix");
        if (ctx->dhcp_warning) {
            gchar *merged = g_strdup_printf("%s; %s", ctx->dhcp_warning, warn);
            g_free(ctx->dhcp_warning);
            ctx->dhcp_warning = merged;
            g_free(warn);
        } else {
            ctx->dhcp_warning = warn;
        }
    }
    else if (ctx->ipv6_prefix && ctx->ipv6_prefix[0]) {
                                                
                                                      
                                                     
                                                                        
        {
                                                                      
            const gchar *slash = g_strrstr(ctx->ipv6_prefix, "/");
            gchar *v6_gw_cidr = NULL;
            gchar *prefix_base = slash
                ? g_strndup(ctx->ipv6_prefix, (gsize)(slash - ctx->ipv6_prefix))
                : g_strdup(ctx->ipv6_prefix);
            if (g_str_has_suffix(prefix_base, "::"))                                     
                v6_gw_cidr = g_strdup_printf("%s1%s", prefix_base, slash ? slash : "/64");
            else if (g_str_has_suffix(prefix_base, ":"))                                  
                v6_gw_cidr = g_strdup_printf("%s:1%s", prefix_base, slash ? slash : "/64");
            else                                                                    
                v6_gw_cidr = g_strdup_printf("%s::1%s", prefix_base, slash ? slash : "/64");
            const gchar *v6_argv[] = {"ip", "-6", "addr", "add", v6_gw_cidr,
                "dev", ctx->bridge_name, NULL};
            pcv_spawn_fire(v6_argv);
            g_free(prefix_base);
            g_free(v6_gw_cidr);
        }
        GError *v6_err = NULL;
        if (!network_dhcp_start_v6(ctx->bridge_name, ctx->ipv6_prefix, &v6_err)) {
            gchar *warn = g_strdup_printf("IPv6 DHCP soft-fail: %s",
                v6_err ? v6_err->message : "unknown");
            if (ctx->dhcp_warning) {
                gchar *merged = g_strdup_printf("%s; %s", ctx->dhcp_warning, warn);
                g_free(ctx->dhcp_warning);
                ctx->dhcp_warning = merged;
                g_free(warn);
            } else {
                ctx->dhcp_warning = warn;
            }
            if (v6_err) g_error_free(v6_err);
        }
    }

                                    
    _network_meta_save(ctx->bridge_name,
                       ctx->mode ? ctx->mode : "nat",
                       ctx->cidr);
    g_task_return_boolean(task, TRUE);
    return;

fail:
                                                  
                                                                 
    if (bridge_created) {
        if (g_strcmp0(ctx->mode, "bridge") != 0)
            network_firewall_teardown(ctx->bridge_name, NULL);
        GError *cleanup = NULL;
        if (!network_bridge_delete(ctx->bridge_name, &cleanup)) {
            PCV_LOG_WARN(NET_LOG_DOM, "bridge rollback failed for %s: %s",
                         ctx->bridge_name, cleanup ? cleanup->message : "unknown");
        }
        g_clear_error(&cleanup);
    }
    if (!error)
        g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "network creation failed");
    g_task_return_error(task, error);
}

   
                                              
                  
                        
                                              
                         
  
                                              
                                   
                                                    
  
         
                                                  
                                                                         
   
static void network_delete_worker(GTask *task, gpointer source_obj, gpointer task_data, GCancellable *cancellable) {
    NetworkCtx *ctx = (NetworkCtx *)task_data;
    GError *error = NULL;
    gchar *state_path = _physical_state_path(PCV_PHYSICAL_NETWORK_STATE_DIR, ctx->bridge_name);
    gboolean has_physical_state = g_file_test(state_path, G_FILE_TEST_EXISTS);
    g_free(state_path);

    if (has_physical_state) {
        if (!pcv_network_physical_bridge_delete_at(ctx->bridge_name, NULL, NULL, &error)) {
            g_task_return_error(task, error);
            return;
        }
        g_task_return_boolean(task, TRUE);
        return;
    }

                                                                           
                                                                
                                       
    gchar *host_uplink = NULL;
    if (!pcv_network_bridge_has_host_uplink_at(
            ctx->bridge_name, NULL, &host_uplink, &error)) {
        g_task_return_error(task, error);
        return;
    }
    if (host_uplink) {
        g_set_error(&error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "bridge '%s' still has unmanaged host uplink '%s'; refusing delete",
                    ctx->bridge_name, host_uplink);
        g_free(host_uplink);
        g_task_return_error(task, error);
        return;
    }

                                                     
    network_firewall_teardown(ctx->bridge_name, NULL);
    if (!network_bridge_delete(ctx->bridge_name, &error)) {
        g_task_return_error(task, error);
        return;
    }
    g_task_return_boolean(task, TRUE);
}

   
                                                         
                        
                                  
                                 
  
                                              
                                                  
                            
  
                                                      
                                                 
  
                                                      
  
       
                                                      
                                                                      
                                                           
  
                                                         
                                               
   
static void network_action_callback(GObject *source_obj, GAsyncResult *res, gpointer user_data) {
    GTask *task = G_TASK(res);
    NetworkCtx *ctx = (NetworkCtx *)user_data;
    GError *error = NULL;
    gboolean is_delete = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(task), "is_delete"));

                                                          
                                                            
    if (g_net_inflight) {
        g_mutex_lock(&g_net_inflight_mu);
        g_hash_table_remove(g_net_inflight, ctx->bridge_name);
        g_mutex_unlock(&g_net_inflight_mu);
    }

    if (!g_task_propagate_boolean(task, &error)) {
        gchar *err_resp = pure_rpc_build_error_response(ctx->rpc_id, PURE_RPC_ERR_ZFS_OPERATION, error->message);
        pure_uds_server_send_response(ctx->server, ctx->connection, err_resp);
        g_free(err_resp);
        g_error_free(error);
    } else {
        JsonObject *result_obj = json_object_new();
        json_object_set_string_member(result_obj, "bridge", ctx->bridge_name);
        
                                                            
        if (is_delete) {
            json_object_set_string_member(result_obj, "status", "deleted");
        } else {
            json_object_set_string_member(result_obj, "status", "created");
            if (g_strcmp0(ctx->mode, "bridge") == 0) {
                json_object_set_string_member(result_obj, "physical_if", ctx->physical_if);
                json_object_set_string_member(result_obj, "uplink_mode", ctx->uplink_mode);
                json_object_set_string_member(result_obj, "dataplane",
                    g_strcmp0(ctx->uplink_mode, "shared") == 0
                        ? "tc-bpf-portal" : "linux-bridge-port");
                json_object_set_boolean_member(result_obj, "host_ip_assigned", FALSE);
                json_object_set_boolean_member(result_obj, "host_l3_preserved",
                    g_strcmp0(ctx->uplink_mode, "shared") == 0);
                json_object_set_boolean_member(result_obj, "physical_enslaved",
                    g_strcmp0(ctx->uplink_mode, "dedicated") == 0);
                json_object_set_string_member(result_obj, "upstream_addressing", "external");
                json_object_set_boolean_member(result_obj, "persistent", TRUE);
            }
                                                     
            if (ctx->dhcp_warning)
                json_object_set_string_member(result_obj, "dhcp_warning", ctx->dhcp_warning);
        }

        JsonNode *result_node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(result_node, result_obj);
        
        gchar *succ_resp = pure_rpc_build_success_response(ctx->rpc_id, result_node);
        pure_uds_server_send_response(ctx->server, ctx->connection, succ_resp);
        g_free(succ_resp);
    }
}

                                                                    
                             
  
                                                                
                                       
                                                                                
                                                                       

   
                                                         
                                                                                  
                          
                     
                           
  
                                                 
                                                  
                                            
                     
  
                                                 
                                               
                                                        
  
                      
                                                    
                                          
                                               
                                       
  
                                      
                                 
   
void handle_network_create_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    if (!params || !json_object_has_member(params, "bridge_name")) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing parameter: bridge_name");
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
        return;
    }

                                                
    const gchar *br_name  = json_object_get_string_member(params, "bridge_name");
    const gchar *mode_raw = json_object_has_member(params, "mode")
                            ? json_object_get_string_member(params, "mode") : NULL;
    const gchar *cidr_raw = json_object_has_member(params, "cidr")
                            ? json_object_get_string_member(params, "cidr") : NULL;
    const gchar *phys_raw = json_object_has_member(params, "physical_if")
                            ? json_object_get_string_member(params, "physical_if") : NULL;
    const gchar *uplink_raw = json_object_has_member(params, "uplink_mode")
                              ? json_object_get_string_member(params, "uplink_mode") : NULL;
    const gchar *ack_raw = json_object_has_member(params, "safety_ack")
                           ? json_object_get_string_member(params, "safety_ack") : NULL;

    GError *validate_err = NULL;
    if (!pcv_validate_network_create_params(br_name, mode_raw, cidr_raw, phys_raw,
                                            uplink_raw, ack_raw, &validate_err)) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            validate_err ? validate_err->message : "Invalid parameters");
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
        if (validate_err) g_error_free(validate_err);
        return;
    }
    if (_reject_vpc_managed_bridge(br_name, rpc_id, server, connection)) return;

    if (g_strcmp0(mode_raw, "bridge") == 0
        && json_object_has_member(params, "ipv6_prefix")
        && *json_object_get_string_member(params, "ipv6_prefix")) {
        gchar *err_resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "bridge mode does not accept ipv6_prefix; the host bridge remains unnumbered");
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
        return;
    }

    NetworkCtx *ctx = g_new0(NetworkCtx, 1);
    ctx->bridge_name  = g_strdup(br_name);
    if (cidr_raw)  ctx->cidr         = g_strdup(cidr_raw);
    if (mode_raw)  ctx->mode         = g_strdup(mode_raw);
    if (phys_raw)  ctx->physical_if  = g_strdup(phys_raw);
    if (g_strcmp0(mode_raw, "bridge") == 0)
        ctx->uplink_mode = g_strdup((uplink_raw && *uplink_raw) ? uplink_raw : "dedicated");
                                                                        
                                                                     
    ctx->mtu = json_object_has_member(params, "mtu")
               ? (gint)json_object_get_int_member(params, "mtu") : 0;
    if (ctx->mtu != 0 && (ctx->mtu < 68 || ctx->mtu > 9216)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid mtu — must be between 68 and 9216 (or 0 for default 1500)");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        free_network_ctx(ctx);
        return;
    }
                         
    ctx->dns_enabled  = json_object_has_member(params, "dns_enabled")
                        && json_object_get_boolean_member(params, "dns_enabled");
    if (json_object_has_member(params, "upstream_dns"))
        ctx->upstream_dns = g_strdup(json_object_get_string_member(params, "upstream_dns"));
                                 
    if (json_object_has_member(params, "ipv6_prefix"))
        ctx->ipv6_prefix = g_strdup(json_object_get_string_member(params, "ipv6_prefix"));

                                                              
                                                         
                                                                   
    if (ctx->ipv6_prefix && ctx->ipv6_prefix[0]
        && !pcv_validate_ipv6_prefix(ctx->ipv6_prefix)) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid ipv6_prefix — must be <ipv6-literal>/<0..128>, no spaces/newlines");
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
        free_network_ctx(ctx);
        return;
    }

    ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = g_object_ref(server);
    ctx->connection = g_object_ref(connection);

                                                 
                                                     
                             
    _net_inflight_init_once();
    g_mutex_lock(&g_net_inflight_mu);                                    
    if (g_hash_table_contains(g_net_inflight, br_name)) {
        g_mutex_unlock(&g_net_inflight_mu);
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            "Network creation already in progress for this bridge");
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
        free_network_ctx(ctx);
        return;
    }
    g_hash_table_insert(g_net_inflight, g_strdup(br_name), GINT_TO_POINTER(TRUE));
    g_mutex_unlock(&g_net_inflight_mu);

    GTask *task = g_task_new(NULL, NULL, network_action_callback, ctx);

                                
    g_task_set_task_data(task, ctx, free_network_ctx);

                                          
    g_object_set_data(G_OBJECT(task), "is_delete", GINT_TO_POINTER(FALSE));
    
    g_task_run_in_thread(task, network_create_worker);
    g_object_unref(task);
}

   
                                                         
                         
                          
                     
                           
  
                                                  
                                                          
                                
                                                   
  
                                               
                                    
   
void handle_network_delete_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    if (!params || !json_object_has_member(params, "bridge_name")) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing parameter: bridge_name");
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
        return;
    }

    const gchar *br = json_object_get_string_member(params, "bridge_name");

                                                                       
                                                 
    if (!pcv_validate_bridge_name(br)) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid bridge name");
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
        return;
    }
    if (_reject_vpc_managed_bridge(br, rpc_id, server, connection)) return;

                                                   
                                                           
                                                          
                                                                  
    {
        gboolean force = json_object_has_member(params, "force")
                       ? json_object_get_boolean_member(params, "force") : FALSE;
        if (!force) {
            gchar *brif_dir = g_strdup_printf("/sys/class/net/%s/brif", br);
            GDir *d = g_dir_open(brif_dir, 0, NULL);
            g_free(brif_dir);
            if (d) {
                const gchar *entry;
                gboolean has_vnet = FALSE;
                while ((entry = g_dir_read_name(d))) {
                    if (g_str_has_prefix(entry, "vnet")) { has_vnet = TRUE; break; }
                }
                g_dir_close(d);
                if (has_vnet) {
                    gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
                        "Bridge has active VM interfaces — stop VMs or pass force=true");
                    pure_uds_server_send_response(server, connection, err_resp);
                    g_free(err_resp);
                    return;
                }
            }
        }
    }

                                                                          
                                                          
    _net_inflight_init_once();
    g_mutex_lock(&g_net_inflight_mu);
    if (g_hash_table_contains(g_net_inflight, br)) {
        g_mutex_unlock(&g_net_inflight_mu);
        gchar *err_resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_CONFLICT,
            "Network mutation already in progress for this bridge");
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
        return;
    }
    g_hash_table_insert(g_net_inflight, g_strdup(br), GINT_TO_POINTER(TRUE));
    g_mutex_unlock(&g_net_inflight_mu);

    NetworkCtx *ctx = g_new0(NetworkCtx, 1);
    ctx->bridge_name = g_strdup(br);
    ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = g_object_ref(server);
    ctx->connection = g_object_ref(connection);

    GTask *task = g_task_new(NULL, NULL, network_action_callback, ctx);

                                
    g_task_set_task_data(task, ctx, free_network_ctx);

                                          
    g_object_set_data(G_OBJECT(task), "is_delete", GINT_TO_POINTER(TRUE));
    
    g_task_run_in_thread(task, network_delete_worker);
    g_object_unref(task);
}
                                                                  
                       
  
                                                    
                                                              
                                                                     

#include "../../utils/pcv_log.h"
#include <net/if.h>


                                                           

   
                                                  
                       
  
                                                     
                
  
                                                    
                                                         
                      
  
                                                         
   
static JsonArray *_read_bridge_slaves(const gchar *bridge_name)
{
    JsonArray *arr  = json_array_new();
    gchar     *path = g_strdup_printf("/sys/class/net/%s/brif", bridge_name);
    GDir      *dir  = g_dir_open(path, 0, NULL);

    if (dir) {
        const gchar *entry;
        while ((entry = g_dir_read_name(dir)) != NULL)
            json_array_add_string_element(arr, entry);
        g_dir_close(dir);
    }

    g_free(path);
    return arr;
}

   
                                        
                       
  
                                                         
  
                                                               
                              
  
                                  
  
                                     
   
static const gchar *_bridge_carrier(const gchar *bridge_name)
{
    gchar *path = g_strdup_printf("/sys/class/net/%s/carrier", bridge_name);
    gchar *content = NULL;
    gboolean ok = g_file_get_contents(path, &content, NULL, NULL);
    g_free(path);

    const gchar *state = "unknown";
    if (ok) {
        g_strstrip(content);
        state = (content[0] == '1') ? "up" : "down";
        g_free(content);
    }
    return state;
}

   
                                                       
  
                                                  
                                                
                                                 
  
               
                                                                     
                                                  
                                     
   
static gboolean
_pid_file_process_alive(const gchar *pid_path)
{
    gchar *pid_str = NULL;
    gboolean alive = FALSE;

    if (!pid_path || !g_file_get_contents(pid_path, &pid_str, NULL, NULL)) {
        return FALSE;
    }

    g_strstrip(pid_str);
    if (pid_str[0] != '\0') {
        gchar *proc_path = g_strdup_printf("/proc/%s/status", pid_str);
        alive = g_file_test(proc_path, G_FILE_TEST_EXISTS);
        g_free(proc_path);
    }

    g_free(pid_str);
    return alive;
}

   
                                                              
  
                                                          
                                                
                            
  
                                                              
                                                            
                                                 
              
   
static gboolean
_libvirt_dhcp_active_for_bridge(const gchar *bridge_name)
{
    GDir *dir = NULL;
    const gchar *entry = NULL;
    gboolean active = FALSE;

    if (!bridge_name || bridge_name[0] == '\0') {
        return FALSE;
    }

    dir = g_dir_open("/var/lib/libvirt/dnsmasq", 0, NULL);
    if (!dir) {
        return FALSE;
    }

    while ((entry = g_dir_read_name(dir)) != NULL) {
        if (!g_str_has_suffix(entry, ".conf")) {
            continue;
        }

        gchar *conf_path = g_build_filename("/var/lib/libvirt/dnsmasq", entry, NULL);
        gchar *content = NULL;
        if (!g_file_get_contents(conf_path, &content, NULL, NULL) || !content) {
            g_free(conf_path);
            continue;
        }

                                                             
                                                                
        gchar *iface_line = g_strdup_printf("interface=%s\n", bridge_name);
        gboolean matches_bridge = strstr(content, iface_line) != NULL;
        gboolean has_dhcp_range = strstr(content, "\ndhcp-range=") != NULL ||
                                  g_str_has_prefix(content, "dhcp-range=");
        g_free(iface_line);
        g_free(content);

                                                          
                                               
        if (matches_bridge && has_dhcp_range) {
            gchar *network_name = g_strndup(entry, strlen(entry) - strlen(".conf"));
            gchar *pid_file = g_strdup_printf("%s.pid", network_name);
            gchar *pid_path = g_build_filename("/var/lib/libvirt/dnsmasq", pid_file, NULL);
            active = _pid_file_process_alive(pid_path);
            g_free(pid_path);
            g_free(pid_file);
            g_free(network_name);
        }

        g_free(conf_path);
        if (active) {
            break;
        }
    }

    g_dir_close(dir);
    return active;
}

   
                                                       
  
                                                   
                                    
  
                                                 
                                                 
                          
   
static gboolean
_dhcp_socket_active_for_bridge(const gchar *bridge_name)
{
    const gchar *argv[] = {"ss", "-lun", NULL};
    gchar *stdout_buf = NULL;
    gboolean active = FALSE;

    if (!bridge_name || bridge_name[0] == '\0') {
        return FALSE;
    }

    if (!pcv_spawn_sync(argv, &stdout_buf, NULL, NULL) || !stdout_buf) {
        g_free(stdout_buf);
        return FALSE;
    }

    gchar *needle = g_strdup_printf("%%%s:67", bridge_name);
    active = strstr(stdout_buf, needle) != NULL;
    g_free(needle);
    g_free(stdout_buf);
    return active;
}

   
                                              
  
                                                     
                                              
  
         
                                      
                                           
                        
  
                                                  
                                        
   
static gboolean
_network_dhcp_active(const gchar *bridge_name)
{
    gchar *pid_path = g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-%s.pid", bridge_name);
    gboolean active = _pid_file_process_alive(pid_path);
    g_free(pid_path);

    if (active) {
        return TRUE;
    }

    if (_libvirt_dhcp_active_for_bridge(bridge_name)) {
        return TRUE;
    }

    return _dhcp_socket_active_for_bridge(bridge_name);
}

   
                                        
                       
  
                                                 
  
                                                           
                                           
  
                                                           
   
static gchar *_get_bridge_ip(const gchar *bridge_name)
{
    const gchar *argv[] = {"ip", "-4", "addr", "show",
                            "dev", bridge_name, NULL};
    gchar *stdout_buf = NULL;
    pcv_spawn_sync(argv, &stdout_buf, NULL, NULL);
    if (!stdout_buf) return g_strdup("");

                                          
    const gchar *hit = strstr(stdout_buf, "inet ");
    gchar *result = g_strdup("");
    if (hit) {
        hit += 5;
        const gchar *end = strchr(hit, ' ');
        if (!end) end = strchr(hit, '\n');
        if (end) {
            g_free(result);
            result = g_strndup(hit, end - hit);
        }
    }
    g_free(stdout_buf);
    return result;
}

   
                                                     
                            
                          
                     
                           
  
                                               
                                                
                                                            
                   
  
                                   
  
               
                                                                   
  
          
                                                     
                  
                                      
                                              
                                                                 
                                                               
                                    
                                        
   
void handle_network_list_request(JsonObject *params __attribute__((unused)),
                                  const gchar *rpc_id,
                                  UdsServer *server,
                                  GSocketConnection *connection)
{
    const gchar *argv[] = {"ip", "-o", "link", "show",
                            "type", "bridge", NULL};
    gchar *stdout_buf = NULL;
    GError *err = NULL;

    if (!pcv_spawn_sync(argv, &stdout_buf, NULL, &err)) {
        const gchar *msg = err ? err->message : "ip link failed";
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, msg);
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        if (err) g_error_free(err);
        return;
    }

    JsonArray *bridges = json_array_new();

                                                                 
    gchar **lines = g_strsplit(stdout_buf ? g_strstrip(stdout_buf) : "", "\n", -1);
    for (gchar **l = lines; *l; l++) {
        if (!**l) continue;

                                         
        const gchar *colon = strchr(*l, ':');
        if (!colon) continue;
        colon++;                               
        while (*colon == ' ') colon++;
        const gchar *name_end = strchr(colon, ':');
        if (!name_end) continue;
        gchar *br_name = g_strndup(colon, name_end - colon);
        g_strstrip(br_name);

                                               
        gchar *at = strchr(br_name, '@');
        if (at) *at = '\0';

                        
        gboolean is_up = (strstr(*l, "UP") != NULL);

                   
        gchar     *ip_cidr = _get_bridge_ip(br_name);
        JsonArray *slaves  = _read_bridge_slaves(br_name);

                                    
        gchar *br_mode = _network_meta_get_mode(br_name);

                            
                                                                 
                                                 
        gboolean dhcp_on = _network_dhcp_active(br_name);

                                                                             
        gchar *phys_uplink = g_strdup("-");
        {
            gchar *brif_path = g_strdup_printf("/sys/class/net/%s/brif", br_name);
            GDir  *brif_dir  = g_dir_open(brif_path, 0, NULL);
            if (brif_dir) {
                const gchar *ifname;
                while ((ifname = g_dir_read_name(brif_dir)) != NULL) {
                                                        
                    if (!g_str_has_prefix(ifname, "vnet") &&
                        !g_str_has_prefix(ifname, "tap")  &&
                        !g_str_has_prefix(ifname, "veth")) {
                        g_free(phys_uplink);
                        phys_uplink = g_strdup(ifname);
                        break;
                    }
                }
                g_dir_close(brif_dir);
            }
            g_free(brif_path);
        }
        PcvPhysicalBridgeState physical_state = {0};
        gchar *desired_path = _physical_state_path(
            PCV_PHYSICAL_NETWORK_STATE_DIR, br_name);
        gboolean managed_physical = g_file_test(desired_path, G_FILE_TEST_EXISTS)
            && _physical_state_load_at(PCV_PHYSICAL_NETWORK_STATE_DIR, br_name,
                                       &physical_state, NULL);
        g_free(desired_path);
        if (managed_physical) {
            g_free(phys_uplink);
            phys_uplink = g_strdup(physical_state.physical_if);
        }

                                                                    
                                                       
                                
                                                 
                                             
                 
                                         
                                                           
                                               
                                               
        gchar *subnet = g_strdup("-");
        if (ip_cidr && ip_cidr[0] && g_strcmp0(ip_cidr, "") != 0) {
            gchar **cidr_parts = g_strsplit(ip_cidr, "/", 2);
            if (cidr_parts[0] && cidr_parts[1]) {
                gchar **octets = g_strsplit(cidr_parts[0], ".", 4);
                int prefix = atoi(cidr_parts[1]);
                if (octets[0]&&octets[1]&&octets[2]&&octets[3]) {
                                                          
                    unsigned int addr =
                        ((unsigned)atoi(octets[0]) << 24) |
                        ((unsigned)atoi(octets[1]) << 16) |
                        ((unsigned)atoi(octets[2]) <<  8) |
                         (unsigned)atoi(octets[3]);
                                                              
                    unsigned int mask = (prefix == 0) ? 0 : (~0u << (32 - prefix));
                                                           
                    unsigned int net  = addr & mask;
                    g_free(subnet);
                    subnet = g_strdup_printf("%u.%u.%u.%u/%d",
                        (net>>24)&0xFF, (net>>16)&0xFF,
                        (net>>8)&0xFF,  net&0xFF, prefix);
                }
                g_strfreev(octets);
            }
            g_strfreev(cidr_parts);
        }

        JsonObject *obj = json_object_new();
        gboolean managed_by_vpc = pcv_vpc_bridge_is_managed(br_name);
        json_object_set_string_member (obj, "name",       br_name);
        json_object_set_string_member (obj, "state",      is_up ? "up" : "down");
        json_object_set_string_member (obj, "ip_cidr",    ip_cidr);
        json_object_set_string_member (obj, "mode",       br_mode);
        json_object_set_boolean_member(obj, "dhcp",       dhcp_on);
        json_object_set_string_member (obj, "phys",       phys_uplink);
        json_object_set_string_member (obj, "subnet",     subnet);
        json_object_set_string_member (obj, "managed_by", managed_by_vpc ? "vpc" : "network");
        json_object_set_boolean_member(obj, "read_only",  managed_by_vpc);
        json_object_set_string_member(obj, "uplink_mode",
            managed_physical ? physical_state.uplink_mode : "none");
        json_object_set_string_member(obj, "dataplane",
            managed_physical && g_strcmp0(physical_state.uplink_mode, "shared") == 0
                ? "tc-bpf-portal"
                : managed_physical ? "linux-bridge-port" : "none");
        json_object_set_boolean_member(obj, "host_l3_preserved",
            managed_physical && g_strcmp0(physical_state.uplink_mode, "shared") == 0);
        json_object_set_boolean_member(obj, "physical_enslaved",
            managed_physical && g_strcmp0(physical_state.uplink_mode, "dedicated") == 0);
        _physical_state_clear(&physical_state);
        g_free(phys_uplink);
        g_free(subnet);
        g_free(br_mode);

        JsonNode *slaves_node = json_node_new(JSON_NODE_ARRAY);
        json_node_take_array(slaves_node, slaves);
        json_object_set_member(obj, "slaves", slaves_node);

        JsonNode *node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(node, obj);
        json_array_add_element(bridges, node);

        g_free(br_name);
        g_free(ip_cidr);
    }

    g_strfreev(lines);
    g_free(stdout_buf);

    JsonNode *result = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(result, bridges);
    gchar *resp = pure_rpc_build_success_response(rpc_id, result);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

   
                                                     
                         
                          
                     
                           
  
                                                
                                                    
                               
  
                    
                                                                         
  
                                             
                                  
   
void handle_network_info_request(JsonObject *params,
                                  const gchar *rpc_id,
                                  UdsServer *server,
                                  GSocketConnection *connection)
{
    if (!params || !json_object_has_member(params, "bridge_name")) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                          "Missing parameter: bridge_name");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

    const gchar *br_name = json_object_get_string_member(params,
                               "bridge_name");

                  
    gchar *sys_path = g_strdup_printf("/sys/class/net/%s", br_name);
    gboolean exists = g_file_test(sys_path, G_FILE_TEST_IS_DIR);
    g_free(sys_path);

    if (!exists) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
                          "Bridge not found");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

    gchar     *ip_cidr  = _get_bridge_ip(br_name);
    JsonArray *slaves   = _read_bridge_slaves(br_name);
    const gchar *carrier = _bridge_carrier(br_name);

                  
                                                   
                                                                
    gboolean has_dhcp = _network_dhcp_active(br_name);

                 
    guint slave_count = json_array_get_length(slaves);

                             
    gchar *info_mode = _network_meta_get_mode(br_name);
    PcvPhysicalBridgeState physical_state = {0};
    gchar *desired_path = _physical_state_path(PCV_PHYSICAL_NETWORK_STATE_DIR, br_name);
    gboolean managed_physical = g_file_test(desired_path, G_FILE_TEST_EXISTS)
        && _physical_state_load_at(PCV_PHYSICAL_NETWORK_STATE_DIR, br_name,
                                   &physical_state, NULL);
    g_free(desired_path);

    JsonObject *info = json_object_new();
    gboolean managed_by_vpc = pcv_vpc_bridge_is_managed(br_name);
    json_object_set_string_member(info, "name",        br_name);
    json_object_set_string_member(info, "state",       carrier);
    json_object_set_string_member(info, "ip_cidr",     ip_cidr);
    json_object_set_string_member(info, "mode",        info_mode);
    json_object_set_boolean_member(info, "dhcp_active", has_dhcp);
    json_object_set_int_member(info, "slave_count", (gint64)slave_count);
    json_object_set_string_member(info, "managed_by", managed_by_vpc ? "vpc" : "network");
    json_object_set_boolean_member(info, "read_only", managed_by_vpc);
    json_object_set_string_member(info, "physical_if",
        managed_physical ? physical_state.physical_if : "-");
    json_object_set_string_member(info, "uplink_mode",
        managed_physical ? physical_state.uplink_mode : "none");
    json_object_set_string_member(info, "dataplane",
        managed_physical && g_strcmp0(physical_state.uplink_mode, "shared") == 0
            ? "tc-bpf-portal"
            : managed_physical ? "linux-bridge-port" : "none");
    json_object_set_boolean_member(info, "host_l3_preserved",
        managed_physical && g_strcmp0(physical_state.uplink_mode, "shared") == 0);
    json_object_set_boolean_member(info, "physical_enslaved",
        managed_physical && g_strcmp0(physical_state.uplink_mode, "dedicated") == 0);
    _physical_state_clear(&physical_state);
    g_free(info_mode);

    JsonNode *slaves_node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(slaves_node, slaves);
    json_object_set_member(info, "slaves", slaves_node);

    JsonNode *result = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(result, info);
    gchar *resp = pure_rpc_build_success_response(rpc_id, result);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
    g_free(ip_cidr);

    PCV_LOG_INFO(NET_LOG_DOM, "network.info: %s → %s slaves=%u",
                 br_name, carrier, slave_count);
}

                                                                  
                                            
  
                                                     
                                                        
                                                        
           
  
                               
                                                       
                                      
  
                                    
                                      
                                                  
                                                                     
void handle_network_mode_set_request(JsonObject *params, const gchar *rpc_id,
                                     UdsServer *server,
                                     GSocketConnection *connection) {


    if (!params
        || !json_object_has_member(params, "name")
        || !json_object_has_member(params, "mode")) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing parameters: name, mode");
        pure_uds_server_send_response(server, connection, e);
        g_free(e);
        return;
    }

    const gchar *br   = json_object_get_string_member(params, "name");
    const gchar *mode = json_object_get_string_member(params, "mode");
    const gchar *cidr = json_object_has_member(params, "cidr")
                        ? json_object_get_string_member(params, "cidr") : NULL;

                                                           
                                                               
                                                           
    if (!pcv_validate_bridge_name(br)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid bridge name");
        pure_uds_server_send_response(server, connection, e);
        g_free(e);
        return;
    }
    if (_reject_vpc_managed_bridge(br, rpc_id, server, connection)) return;

                                                                        
                                                                  
                                                   
    gchar *mutation_reason = NULL;
    g_mutex_lock(&g_physical_bridge_mu);
    gboolean live_l3_allowed = pcv_network_live_l3_mutation_allowed_at(
        br, NULL, NULL, &mutation_reason);
    g_mutex_unlock(&g_physical_bridge_mu);
    if (!live_l3_allowed) {
        gchar *e = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_CONFLICT, mutation_reason);
        pure_uds_server_send_response(server, connection, e);
        g_free(e);
        g_free(mutation_reason);
        return;
    }

                  
    if (g_strcmp0(mode, "nat")      != 0 &&
        g_strcmp0(mode, "isolated") != 0 &&
        g_strcmp0(mode, "routed")   != 0) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid mode: must be nat | isolated | routed");
        pure_uds_server_send_response(server, connection, e);
        g_free(e);
        return;
    }

                                          
    if (!cidr || strlen(cidr) == 0) {
                                                     
                        
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing parameter: cidr (required for mode change)");
        pure_uds_server_send_response(server, connection, e);
        g_free(e);
        return;
    }

                                                                          
                                                     
                           
    if (!pcv_validate_private_cidr(cidr)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid cidr — must be a private CIDR (RFC1918/RFC6598/fc00::/7)");
        pure_uds_server_send_response(server, connection, e);
        g_free(e);
        return;
    }

    GError *error = NULL;

                                                          
                                              
    if (g_strcmp0(mode, "routed") == 0
        && !network_dhcp_stop(br, &error)) {
        const gchar *msg = error ? error->message : "dnsmasq stop failed";
        gchar *e = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_ZFS_OPERATION, msg);
        pure_uds_server_send_response(server, connection, e);
        g_free(e);
        g_clear_error(&error);
        return;
    }

                           
    network_firewall_teardown(br, NULL);

                       
    gboolean ok = FALSE;
    if      (g_strcmp0(mode, "nat")      == 0) ok = network_firewall_setup_nat     (br, cidr, &error);
    else if (g_strcmp0(mode, "isolated") == 0) ok = network_firewall_setup_isolated(br, cidr, &error);
    else if (g_strcmp0(mode, "routed")   == 0)
        ok = network_firewall_setup_routed(br, cidr, &error);

    if (!ok) {
        const gchar *msg = error ? error->message : "firewall setup failed";
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, msg);
        pure_uds_server_send_response(server, connection, e);
        g_free(e);
        if (error) g_error_free(error);
        return;
    }

                                                        
                                                          
                                        
                                                          
                                                        
    if (ok && (g_strcmp0(mode, "nat") == 0 || g_strcmp0(mode, "isolated") == 0)) {
        GError *dhcp_err = NULL;
        if (!network_dhcp_start(br, cidr, &dhcp_err)) {
                                                      
            PCV_LOG_INFO(NET_LOG_DOM,
                "network.mode_set: DHCP restart soft-fail for %s: %s",
                br, dhcp_err ? dhcp_err->message : "unknown");
            if (dhcp_err) g_error_free(dhcp_err);
        }
    }

                                     
    _network_meta_save(br, mode, cidr);

                  
    JsonObject *res_obj = json_object_new();
    json_object_set_string_member(res_obj, "bridge", br);
    json_object_set_string_member(res_obj, "mode",   mode);
    JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(res_node, res_obj);
    gchar *resp = pure_rpc_build_success_response(rpc_id, res_node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);

    PCV_LOG_INFO(NET_LOG_DOM, "network.mode_set: %s → %s", br, mode);
}

                                                                             
                                              
                           
                                                    
  
                                                
                                           
                                                        
  
                                    
                                                                                
void handle_network_bind_phys_request(JsonObject *params, const gchar *rpc_id,
                                      UdsServer *server, GSocketConnection *connection)
{
    const gchar *br    = json_object_has_member(params, "bridge")
                         ? json_object_get_string_member(params, "bridge") : NULL;
    const gchar *iface = json_object_has_member(params, "iface")
                         ? json_object_get_string_member(params, "iface")  : NULL;

    if (!br || !strlen(br) || !iface || !strlen(iface)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Missing params: 'bridge' and 'iface' are required");
        pure_uds_server_send_response(server, connection, e);
        g_free(e);
        return;
    }

                                                                     
    if (!pcv_validate_bridge_name(br) || !pcv_validate_iface_name(iface)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Invalid bridge or iface name");
        pure_uds_server_send_response(server, connection, e);
        g_free(e);
        return;
    }
    if (_reject_vpc_managed_bridge(br, rpc_id, server, connection)) return;

                                                                               
                                                          
    gchar *e = pure_rpc_build_error_response(
        rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
        "network.bind_phys is disabled; use network.create mode=bridge with "
        "physical_if, uplink_mode='dedicated|shared', and the matching safety_ack");
    pure_uds_server_send_response(server, connection, e);
    g_free(e);
}

                                                                             
                                                
                             
                                                         
  
                                                    
                                                   
                         
                                                                          
  
                                        
                                                              
                                             
                                                                                
void handle_network_dhcp_toggle_request(JsonObject *params, const gchar *rpc_id,
                                        UdsServer *server, GSocketConnection *connection)
{
    const gchar *br = json_object_has_member(params, "bridge")
                      ? json_object_get_string_member(params, "bridge") : NULL;
    if (!br || !strlen(br)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Missing param: 'bridge' is required");
        pure_uds_server_send_response(server, connection, e);
        g_free(e);
        return;
    }

                                                            
                                                  
    if (!pcv_validate_bridge_name(br)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Invalid bridge name");
        pure_uds_server_send_response(server, connection, e);
        g_free(e);
        return;
    }
    if (_reject_vpc_managed_bridge(br, rpc_id, server, connection)) return;

    gboolean enable = TRUE;
    if (json_object_has_member(params, "enable"))
        enable = json_object_get_boolean_member(params, "enable");

                                                                     
                                                           
    if (enable) {
        gchar *mutation_reason = NULL;
        g_mutex_lock(&g_physical_bridge_mu);
        gboolean live_l3_allowed = pcv_network_live_l3_mutation_allowed_at(
            br, NULL, NULL, &mutation_reason);
        g_mutex_unlock(&g_physical_bridge_mu);
        if (!live_l3_allowed) {
            gchar *e = pure_rpc_build_error_response(
                rpc_id, PURE_RPC_ERR_CONFLICT, mutation_reason);
            pure_uds_server_send_response(server, connection, e);
            g_free(e);
            g_free(mutation_reason);
            return;
        }
    }

    if (enable) {
                                         
        gchar *meta_path = g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-%s.meta", br);
        gchar *cidr = NULL;
        GError *meta_err = NULL;
        gchar  *meta_data = NULL;

        if (g_file_get_contents(meta_path, &meta_data, NULL, &meta_err)) {
            JsonParser *p = json_parser_new();
            if (json_parser_load_from_data(p, meta_data, -1, NULL)) {
                JsonObject *mo = json_node_get_object(json_parser_get_root(p));
                if (json_object_has_member(mo, "cidr"))
                    cidr = g_strdup(json_object_get_string_member(mo, "cidr"));
            }
            g_object_unref(p);
            g_free(meta_data);
        }
        if (meta_err) g_error_free(meta_err);
        g_free(meta_path);

        if (!cidr || !strlen(cidr)) {
            g_free(cidr);
            gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
                           "Cannot read CIDR from bridge meta — set mode first");
            pure_uds_server_send_response(server, connection, e);
            g_free(e);
            return;
        }

        GError *dhcp_err = NULL;
        if (!network_dhcp_start(br, cidr, &dhcp_err)) {
            const gchar *msg = dhcp_err ? dhcp_err->message : "dnsmasq start failed";
            gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, msg);
            pure_uds_server_send_response(server, connection, e);
            g_free(e);
            if (dhcp_err) g_error_free(dhcp_err);
            g_free(cidr);
            return;
        }
        g_free(cidr);
        PCV_LOG_INFO(NET_LOG_DOM, "network.dhcp_toggle: DHCP started on %s", br);
    } else {
                                                          
        GError *dhcp_err = NULL;
        if (!network_dhcp_stop(br, &dhcp_err)) {
            const gchar *msg = dhcp_err ? dhcp_err->message : "dnsmasq stop failed";
            gchar *e = pure_rpc_build_error_response(
                rpc_id, PURE_RPC_ERR_ZFS_OPERATION, msg);
            pure_uds_server_send_response(server, connection, e);
            g_free(e);
            g_clear_error(&dhcp_err);
            return;
        }
        PCV_LOG_INFO(NET_LOG_DOM, "network.dhcp_toggle: DHCP stopped on %s", br);
    }

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "bridge", br);
    json_object_set_boolean_member(res, "dhcp",  enable);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

                                                                    
                                     
  
                        
                                                
                                           
                                              
  
                            
                                                  
                                                       
                                                                   
                                          
                                     
  
           
                                           
                                                
                                                                       

   
                                                                 
                               
                          
                     
                           
  
                                                   
                                                
  
                                                    
                                                  
   
void handle_network_ovs_create_request(JsonObject *params, const gchar *rpc_id,
                                       UdsServer *server, GSocketConnection *connection)
{
    const gchar *br = json_object_has_member(params, "bridge")
        ? json_object_get_string_member(params, "bridge") : NULL;
    if (!br || br[0] == '\0') {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing parameter: bridge");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); return;
    }

                                                           
    if (!pcv_validate_bridge_name(br)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid bridge name");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); return;
    }

    const gchar *argv[] = {"ovs-vsctl", "--may-exist", "add-br", br, NULL};
    gchar *std_err = NULL;
    GError *error = NULL;

    if (!pcv_spawn_sync(argv, NULL, &std_err, &error)) {
        gchar *msg = g_strdup_printf("OVS bridge create failed: %s",
            error ? error->message : (std_err ? std_err : "unknown"));
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, msg);
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp); g_free(msg);
        if (error) g_error_free(error);
        g_free(std_err); return;
    }
    g_free(std_err);

                 
    const gchar *up_argv[] = {"ip", "link", "set", br, "up", NULL};
    pcv_spawn_sync(up_argv, NULL, NULL, NULL);

    g_message("[OVS] Bridge '%s' created.", br);

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "success");
    json_object_set_string_member(res, "bridge", br);
    json_object_set_string_member(res, "type", "ovs");
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

   
                                                                 
                                   
                          
                     
                           
  
                                                  
  
                                              
                                              
   
void handle_network_ovs_delete_request(JsonObject *params, const gchar *rpc_id,
                                       UdsServer *server, GSocketConnection *connection)
{
    const gchar *br = json_object_has_member(params, "bridge")
        ? json_object_get_string_member(params, "bridge") : NULL;
    if (!br || br[0] == '\0') {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing parameter: bridge");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); return;
    }

                                                           
    if (!pcv_validate_bridge_name(br)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid bridge name");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); return;
    }

    const gchar *argv[] = {"ovs-vsctl", "--if-exists", "del-br", br, NULL};
    gchar *std_err = NULL;
    GError *error = NULL;

    if (!pcv_spawn_sync(argv, NULL, &std_err, &error)) {
        gchar *msg = g_strdup_printf("OVS bridge delete failed: %s",
            error ? error->message : (std_err ? std_err : "unknown"));
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, msg);
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp); g_free(msg);
        if (error) g_error_free(error);
        g_free(std_err); return;
    }
    g_free(std_err);

    g_message("[OVS] Bridge '%s' deleted.", br);

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "success");
    json_object_set_string_member(res, "bridge", br);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

                                                                  
  
             
                                                       
                                                                 
  
                                                             
                                          
  
                    
                                                 
                                                                                  
  
                                              
                                                           
                                          
                                                       
   
void handle_network_ovs_vxlan_add_request(JsonObject *params, const gchar *rpc_id,
                                          UdsServer *server, GSocketConnection *connection)
{
    const gchar *br       = json_object_has_member(params, "bridge")
        ? json_object_get_string_member(params, "bridge") : NULL;
    const gchar *port     = json_object_has_member(params, "port_name")
        ? json_object_get_string_member(params, "port_name") : NULL;
    const gchar *remote   = json_object_has_member(params, "remote_ip")
        ? json_object_get_string_member(params, "remote_ip") : NULL;
    gint64 vni = json_object_has_member(params, "vni")
        ? json_object_get_int_member(params, "vni") : 100;

    if (!br || !port || !remote) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Required: bridge, port_name, remote_ip");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); return;
    }

                                                                    
                                                                  
    if (!pcv_validate_bridge_name(br) || !pcv_validate_ip_literal(remote)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid bridge name or remote_ip");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); return;
    }

    gchar *vni_str = g_strdup_printf("%ld", (long)vni);
    const gchar *argv[] = {
        "ovs-vsctl", "--may-exist", "add-port", br, port,
        "--", "set", "interface", port,
        "type=vxlan",
        NULL, NULL, NULL, NULL                  
    };
                                                          
                                                           
                                                                 
    gchar *opt_key    = g_strdup_printf("options:key=%s", vni_str);
    gchar *opt_remote = g_strdup_printf("options:remote_ip=%s", remote);
    argv[10] = opt_key;                                    
    argv[11] = opt_remote;                                      
    argv[12] = NULL;                                

    gchar *std_err = NULL;
    GError *error = NULL;

    if (!pcv_spawn_sync(argv, NULL, &std_err, &error)) {
        gchar *msg = g_strdup_printf("OVS VXLAN add failed: %s",
            error ? error->message : (std_err ? std_err : "unknown"));
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, msg);
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp); g_free(msg);
        if (error) g_error_free(error);
        g_free(std_err); g_free(vni_str);
        g_free(opt_key); g_free(opt_remote); return;
    }
    g_free(std_err);

    g_message("[OVS] VXLAN port '%s' added to '%s' (remote=%s, vni=%ld)",
              port, br, remote, (long)vni);

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "success");
    json_object_set_string_member(res, "bridge", br);
    json_object_set_string_member(res, "port", port);
    json_object_set_string_member(res, "remote_ip", remote);
    json_object_set_int_member(res, "vni", vni);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
    g_free(vni_str); g_free(opt_key); g_free(opt_remote);
}

   
                                                                       
                               
                          
                     
                           
  
                                                  
  
                                                        
   
void handle_network_ovs_vxlan_del_request(JsonObject *params, const gchar *rpc_id,
                                          UdsServer *server, GSocketConnection *connection)
{
    const gchar *br   = json_object_has_member(params, "bridge")
        ? json_object_get_string_member(params, "bridge") : NULL;
    const gchar *port = json_object_has_member(params, "port_name")
        ? json_object_get_string_member(params, "port_name") : NULL;

    if (!br || !port) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Required: bridge, port_name");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); return;
    }

                                                           
    if (!pcv_validate_bridge_name(br)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid bridge name");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); return;
    }

    const gchar *argv[] = {"ovs-vsctl", "--if-exists", "del-port", br, port, NULL};
    gchar *std_err = NULL;
    GError *error = NULL;

    if (!pcv_spawn_sync(argv, NULL, &std_err, &error)) {
        gchar *msg = g_strdup_printf("OVS VXLAN del failed: %s",
            error ? error->message : (std_err ? std_err : "unknown"));
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, msg);
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp); g_free(msg);
        if (error) g_error_free(error);
        g_free(std_err); return;
    }
    g_free(std_err);

    g_message("[OVS] VXLAN port '%s' removed from '%s'", port, br);

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "success");
    json_object_set_string_member(res, "bridge", br);
    json_object_set_string_member(res, "port", port);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

                                                                         
                                   
  
                                                     
                                                 
  
                                                    
                            
                                                                 
                                                                            
static gchar *
_find_vm_vnet(const gchar *vm_name)
{
    GPtrArray *ifaces = pcv_vm_iface_list(vm_name);
    gchar *vnet = (ifaces->len > 0)
        ? g_strdup(g_ptr_array_index(ifaces, 0)) : NULL;
    g_ptr_array_unref(ifaces);
    return vnet;
}

                                                                
                                                        
                                
                                                           
                                                     
static void
_qos_deprecate_warn_once(void)
{
    static gsize warned = 0;
    if (g_once_init_enter(&warned)) {
        PCV_LOG_WARN("NET",
            "network.qos.* deprecated — use qos.vm.set (D09 계층 QoS)");
        g_once_init_leave(&warned, 1);
    }
}

                                                                         
                                                        
  
                                                          
                                                            
                                                  
                                                  
  
         
                                                          
                                          
                                         
                                                                         
  
       
                                                       
                                            
                                              
                                                           
                                   
  
                                  
                                                           
                                                   
                                                                  
                                                 
                                                             
                                                       
                                                                            
void handle_network_qos_set(JsonObject *params, const gchar *rpc_id,
                             UdsServer *server, GSocketConnection *connection)
{
    _qos_deprecate_warn_once();

    const gchar *iface = json_object_has_member(params, "interface")
        ? json_object_get_string_member(params, "interface") : NULL;
    gint rate_mbps = json_object_has_member(params, "rate_mbps")
        ? (gint)json_object_get_int_member(params, "rate_mbps") : 0;
    gint burst_kb = json_object_has_member(params, "burst_kb")
        ? (gint)json_object_get_int_member(params, "burst_kb") : 256;
    const gchar *direction = json_object_has_member(params, "direction")
        ? json_object_get_string_member(params, "direction") : "egress";

                                                           
                                                    
                                            
    gchar *auto_iface = NULL;
    if ((!iface || !iface[0]) && json_object_has_member(params, "vm_name")) {
        const gchar *vm_name = json_object_get_string_member(params, "vm_name");
        if (vm_name && *vm_name) {
                                                                     
            if (!pcv_validate_vm_name(vm_name)) {
                gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                    "Invalid vm_name");
                pure_uds_server_send_response(server, connection, err);
                g_free(err); g_free(auto_iface); return;
            }
            auto_iface = _find_vm_vnet(vm_name);
            if (auto_iface) {
                iface = auto_iface;
                PCV_LOG_INFO("NET", "Resolved vm_name '%s' → interface '%s'",
                             vm_name, iface);
            }
        }
    }

    if (!iface || !iface[0]) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing required parameter: interface (or vm_name)");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); g_free(auto_iface); return;
    }
                                                                 
    if (!pcv_validate_iface_name(iface)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid interface name");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); g_free(auto_iface); return;
    }
    if (rate_mbps <= 0) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "rate_mbps must be > 0");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); g_free(auto_iface); return;
    }
    if (burst_kb <= 0) burst_kb = 256;

                                             
    if (g_strcmp0(direction, "egress") != 0 && g_strcmp0(direction, "ingress") != 0) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "direction must be 'egress' or 'ingress'");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); g_free(auto_iface); return;
    }

    if (g_strcmp0(direction, "ingress") == 0) {
                                                          
                                                           
                                                        
                                                         
                                                                
                                                             
                                                             
                                              
                                            
        if (pcv_qos_iface_is_managed(iface)) {
            gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
                "iface is under D09 hierarchical QoS — use qos.vm.set; "
                "ingress police would destroy the SLA redirect");
            pure_uds_server_send_response(server, connection, err);
            g_free(err); g_free(auto_iface); return;
        }

                                                                   
                                                  
                                                      
        {
            const gchar *del_argv[] = {"tc", "qdisc", "del", "dev", iface,
                "ingress", NULL};
            gchar *std_err = NULL;
            GError *error = NULL;
                                                 
            pcv_spawn_sync(del_argv, NULL, &std_err, &error);
            g_free(std_err);
            if (error) g_error_free(error);
        }
        {
            const gchar *argv[] = {"tc", "qdisc", "add", "dev", iface,
                "handle", "ffff:", "ingress", NULL};
            gchar *std_err = NULL;
            GError *error = NULL;
            if (!pcv_spawn_sync(argv, NULL, &std_err, &error)) {
                gchar *msg = g_strdup_printf("tc ingress qdisc failed: %s",
                    error ? error->message : (std_err ? std_err : "unknown"));
                gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, msg);
                pure_uds_server_send_response(server, connection, err_resp);
                g_free(err_resp); g_free(msg);
                if (error) g_error_free(error);
                g_free(std_err); g_free(auto_iface); return;                      
            }
            g_free(std_err);
        }

                                                   
                                     
                                                
                                                          
        {
            gchar *rate = g_strdup_printf("%dmbit", rate_mbps);
            gchar *burst = g_strdup_printf("%dk", burst_kb);
            const gchar *argv[] = {"tc", "filter", "add", "dev", iface,
                "parent", "ffff:", "protocol", "all",
                "u32", "match", "u32", "0", "0",
                "police", "rate", rate, "burst", burst, "drop", NULL};
            gchar *std_err = NULL;
            GError *error = NULL;
            gboolean ok = pcv_spawn_sync(argv, NULL, &std_err, &error);
            g_free(rate); g_free(burst);
            if (!ok) {
                gchar *msg = g_strdup_printf("tc ingress filter failed: %s",
                    error ? error->message : (std_err ? std_err : "unknown"));
                gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, msg);
                pure_uds_server_send_response(server, connection, err_resp);
                g_free(err_resp); g_free(msg);
                if (error) g_error_free(error);
                g_free(std_err); g_free(auto_iface); return;                      
            }
            g_free(std_err);
        }

        g_message("[QoS] Set ingress policing %dMbit burst=%dk on %s",
                  rate_mbps, burst_kb, iface);
    } else {
                                                               
                                                             
                                                    
                                                              
                                                          
        {
            const gchar *argv[] = {"tc", "qdisc", "replace", "dev", iface,
                "root", "handle", "1:", "htb", "default", "10", NULL};
            gchar *std_err = NULL;
            GError *error = NULL;
            if (!pcv_spawn_sync(argv, NULL, &std_err, &error)) {
                gchar *msg = g_strdup_printf("tc qdisc failed: %s",
                    error ? error->message : (std_err ? std_err : "unknown"));
                gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, msg);
                pure_uds_server_send_response(server, connection, err_resp);
                g_free(err_resp); g_free(msg);
                if (error) g_error_free(error);
                g_free(std_err); g_free(auto_iface); return;                      
            }
            g_free(std_err);
        }

                                               
                                                               
                                                          
                                                       
        {
            gchar *rate = g_strdup_printf("%dMbit", rate_mbps);
            gchar *burst = g_strdup_printf("%dk", burst_kb);
            const gchar *argv[] = {"tc", "class", "replace", "dev", iface,
                "parent", "1:", "classid", "1:10", "htb",
                "rate", rate, "burst", burst, NULL};
            gchar *std_err = NULL;
            GError *error = NULL;
            gboolean ok = pcv_spawn_sync(argv, NULL, &std_err, &error);
            g_free(rate); g_free(burst);
            if (!ok) {
                gchar *msg = g_strdup_printf("tc class failed: %s",
                    error ? error->message : (std_err ? std_err : "unknown"));
                gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, msg);
                pure_uds_server_send_response(server, connection, err_resp);
                g_free(err_resp); g_free(msg);
                if (error) g_error_free(error);
                g_free(std_err); g_free(auto_iface); return;                      
            }
            g_free(std_err);
        }

        g_message("[QoS] Set egress %dMbit burst=%dk on %s", rate_mbps, burst_kb, iface);
    }

                                                    
    _qos_persist_save(iface, direction, rate_mbps, burst_kb);

    JsonObject *qos_res = json_object_new();
    json_object_set_string_member(qos_res, "status", "ok");
    json_object_set_string_member(qos_res, "interface", iface);
    json_object_set_string_member(qos_res, "direction", direction);
    json_object_set_int_member(qos_res, "rate_mbps", rate_mbps);
    json_object_set_int_member(qos_res, "burst_kb", burst_kb);
    JsonNode *qos_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(qos_node, qos_res);
    gchar *qos_resp = pure_rpc_build_success_response(rpc_id, qos_node);
    pure_uds_server_send_response(server, connection, qos_resp);
    g_free(qos_resp);
    g_free(auto_iface);                               
}

                                                                         
                                  
  
                                                  
                                    
  
         
                                 
                                                                            
void handle_network_qos_get(JsonObject *params, const gchar *rpc_id,
                             UdsServer *server, GSocketConnection *connection)
{
    const gchar *iface = json_object_has_member(params, "interface")
        ? json_object_get_string_member(params, "interface") : NULL;

    if (!iface || !iface[0]) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing required parameter: interface");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); return;
    }

    const gchar *argv[] = {"tc", "-s", "class", "show", "dev", iface, NULL};
    gchar *stdout_buf = NULL;
    gchar *std_err = NULL;
    GError *error = NULL;

    if (!pcv_spawn_sync(argv, &stdout_buf, &std_err, &error)) {
        gchar *msg = g_strdup_printf("tc query failed: %s",
            error ? error->message : (std_err ? std_err : "unknown"));
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, msg);
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp); g_free(msg);
        if (error) g_error_free(error);
        g_free(stdout_buf); g_free(std_err); return;
    }
    g_free(std_err);

    JsonObject *qget_res = json_object_new();
    json_object_set_string_member(qget_res, "interface", iface);
    json_object_set_string_member(qget_res, "tc_output", stdout_buf ? stdout_buf : "");
    json_object_set_boolean_member(qget_res, "egress_active",
        stdout_buf && strstr(stdout_buf, "htb") != NULL);
    g_free(stdout_buf);

                              
    {
        const gchar *ing_argv[] = {"tc", "qdisc", "show", "dev", iface, "ingress", NULL};
        gchar *ing_stdout = NULL;
        gchar *ing_stderr = NULL;
        GError *ing_error = NULL;
        gboolean ing_ok = pcv_spawn_sync(ing_argv, &ing_stdout, &ing_stderr, &ing_error);
        json_object_set_boolean_member(qget_res, "ingress_active",
            ing_ok && ing_stdout && strstr(ing_stdout, "ingress") != NULL);
        g_free(ing_stdout); g_free(ing_stderr);
        if (ing_error) g_error_free(ing_error);
    }

    JsonNode *qget_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(qget_node, qget_res);
    gchar *qget_resp = pure_rpc_build_success_response(rpc_id, qget_node);
    pure_uds_server_send_response(server, connection, qget_resp);
    g_free(qget_resp);
}

                                                                         
                                            
  
                                                  
                                                   
  
         
                                 
                                                            
                                                                            
void handle_network_qos_remove(JsonObject *params, const gchar *rpc_id,
                                UdsServer *server, GSocketConnection *connection)
{
    const gchar *iface = json_object_has_member(params, "interface")
        ? json_object_get_string_member(params, "interface") : NULL;
    const gchar *direction = json_object_has_member(params, "direction")
        ? json_object_get_string_member(params, "direction") : "egress";

    if (!iface || !iface[0]) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing required parameter: interface");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); return;
    }

    gboolean remove_egress  = (g_strcmp0(direction, "egress") == 0 || g_strcmp0(direction, "all") == 0);
    gboolean remove_ingress = (g_strcmp0(direction, "ingress") == 0 || g_strcmp0(direction, "all") == 0);

                                
    if (remove_egress) {
        const gchar *argv[] = {"tc", "qdisc", "del", "dev", iface, "root", NULL};
        gchar *std_err = NULL;
        GError *error = NULL;

        if (!pcv_spawn_sync(argv, NULL, &std_err, &error)) {
                                       
            const gchar *emsg = error ? error->message : (std_err ? std_err : "");
            if (strstr(emsg, "No such file") || strstr(emsg, "Cannot delete")) {
                if (error) g_error_free(error);
                g_free(std_err);
            } else {
                gchar *msg = g_strdup_printf("tc qdisc del failed: %s", emsg);
                gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, msg);
                pure_uds_server_send_response(server, connection, err_resp);
                g_free(err_resp); g_free(msg);
                if (error) g_error_free(error);
                g_free(std_err); return;
            }
        } else {
            g_free(std_err);
        }
    }

                          
    if (remove_ingress) {
        const gchar *argv[] = {"tc", "qdisc", "del", "dev", iface, "ingress", NULL};
        gchar *std_err = NULL;
        GError *error = NULL;

        if (!pcv_spawn_sync(argv, NULL, &std_err, &error)) {
                                               
            const gchar *emsg = error ? error->message : (std_err ? std_err : "");
            if (!strstr(emsg, "No such file") && !strstr(emsg, "Cannot delete")
                && !strstr(emsg, "Invalid argument")) {
                gchar *msg = g_strdup_printf("tc ingress del failed: %s", emsg);
                gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, msg);
                pure_uds_server_send_response(server, connection, err_resp);
                g_free(err_resp); g_free(msg);
                if (error) g_error_free(error);
                g_free(std_err); return;
            }
            if (error) g_error_free(error);
            g_free(std_err);
        } else {
            g_free(std_err);
        }
    }

    g_message("[QoS] Removed tc qdisc (%s) on %s", direction, iface);

                                
    _qos_persist_remove(iface,
        g_strcmp0(direction, "all") == 0 ? "both" : direction);

    JsonObject *qrm_res = json_object_new();
    json_object_set_string_member(qrm_res, "status", "removed");
    json_object_set_string_member(qrm_res, "interface", iface);
    JsonNode *qrm_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(qrm_node, qrm_res);
    gchar *qrm_resp = pure_rpc_build_success_response(rpc_id, qrm_node);
    pure_uds_server_send_response(server, connection, qrm_resp);
    g_free(qrm_resp);
}

                                                                         
                                                
  
                                                            
                                                             
                                                                            

   
                       
                                 
                                         
                             
  
                                                 
                                                    
                       
  
                     
                                                           
   
gboolean
pcv_bridge_vlan_add(const gchar *bridge, const gchar *iface, gint vlan_id)
{
    if (!bridge || !iface || vlan_id < 1 || vlan_id > 4094) return FALSE;

                                   
    gchar *filter_path = g_strdup_printf("/sys/class/net/%s/bridge/vlan_filtering",
                                          bridge);
    GError *werr = NULL;
    if (!g_file_set_contents(filter_path, "1", -1, &werr)) {
        PCV_LOG_WARN("NET", "Failed to enable VLAN filtering on %s: %s",
                     bridge, werr ? werr->message : "unknown");
        if (werr) g_error_free(werr);
        g_free(filter_path);
        return FALSE;
    }
    g_free(filter_path);

                                                   
    gchar *vid = g_strdup_printf("%d", vlan_id);
    const gchar *argv[] = {"bridge", "vlan", "add", "dev", iface,
                           "vid", vid, NULL};
    gchar *std_err = NULL;
    GError *error = NULL;
    gboolean ok = pcv_spawn_sync(argv, NULL, &std_err, &error);
    g_free(vid);

    if (ok) {
        PCV_LOG_INFO("NET", "Added VLAN %d to %s on bridge %s",
                     vlan_id, iface, bridge);
    } else {
        PCV_LOG_WARN("NET", "Failed to add VLAN %d to %s: %s",
                     vlan_id, iface,
                     error ? error->message : (std_err ? std_err : "unknown"));
    }
    if (error) g_error_free(error);
    g_free(std_err);
    return ok;
}

   
                          
                   
                     
                        
  
                                                    
                 
  
                                        
                               
   
gboolean
pcv_bridge_vlan_remove(const gchar *bridge, const gchar *iface, gint vlan_id)
{
    if (!bridge || !iface || vlan_id < 1 || vlan_id > 4094) return FALSE;

    gchar *vid = g_strdup_printf("%d", vlan_id);
    const gchar *argv[] = {"bridge", "vlan", "del", "dev", iface,
                           "vid", vid, NULL};
    gchar *std_err = NULL;
    GError *error = NULL;
    gboolean ok = pcv_spawn_sync(argv, NULL, &std_err, &error);
    g_free(vid);

    if (ok) {
        PCV_LOG_INFO("NET", "Removed VLAN %d from %s on bridge %s",
                     vlan_id, iface, bridge);
    } else {
                                      
        const gchar *emsg = error ? error->message : (std_err ? std_err : "");
        if (strstr(emsg, "No such") || strstr(emsg, "does not exist")) {
            ok = TRUE;
        } else {
            PCV_LOG_WARN("NET", "Failed to remove VLAN %d from %s: %s",
                         vlan_id, iface, emsg);
        }
    }
    if (error) g_error_free(error);
    g_free(std_err);
    return ok;
}
