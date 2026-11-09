   
                           
                                                                                                 
  
                           
                                                                                               
                                                                                
                                                                                 
                                                                                
                                                                        
                                                                      
                                              
                                                                       
                                                                                      
  
          
                                                                 
                                                                
                                                    
                                                                   
                                                      
                                                                  
                                                                      
  
                 
                                                                                      
                                                                             
                                                                          
  
                                                                       
            
                                                                                                            
                                                                                                                    
  
                                                                                              
                                                                                          
                                                                                                      
                                           
  
                                                                      
                                                                
  
                          
                                                                                  
                                                            
                                                                                         
                                                                                 
  
                                     
                                                                                                        
                                                                                                      
                                                                                                        
                                                                         
                                                       
  
                                                                                                  
                                                                                            
  
                                                                                                           
                                                                                                                   
                                                                       
   
#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <string.h>

#include "modules/network/network_firewall.h"
#include "modules/network/network_firewall_nft.h"
#include "modules/network/network_firewall_host.h"
#include "../../utils/pcv_spawn.h"
#include "../../utils/pcv_validate.h"
#include "../../utils/pcv_config.h"
#include "../../utils/pcv_log.h"

#define GFW_LOG_DOM   "network_fw"
#define GFW_LIST_FILE "guest-fw.json"

                                                                             
                                                                                
static GMutex     g_guest_fw_mu;
static GPtrArray *g_guest_fw_nets;                                               
static gchar     *g_gfw_rundir_override;                                                     
static gchar     *g_gfw_sysnet_override;

                              
static const gchar *
_rundir(void)
{
    return g_gfw_rundir_override ? g_gfw_rundir_override : PCV_NETWORK_RUNDIR;
}

                                     
static const gchar *
_sysnet(void)
{
    return g_gfw_sysnet_override ? g_gfw_sysnet_override : "/sys/class/net";
}

                                                                                                                
static gboolean
_host_integration_enabled(void)
{
    return g_strcmp0(pcv_config_get_string("network", "firewall_integration", "auto"), "auto") == 0;
}

                                                                              
                                                                                
                                                                             
                                                                             
static gboolean
_entry_check(const gchar *bridge, const gchar *mode, const gchar *cidr, GError **error)
{
    if (!pcv_validate_bridge_name(bridge)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Invalid bridge name for firewall rule: %s", bridge ? bridge : "(null)");
        return FALSE;
    }
    if (g_strcmp0(mode, "nat") == 0) {
        g_autofree gchar *subnet = pcv_validate_private_cidr(cidr) ? pcv_guest_fw_cidr_to_subnet(cidr, NULL) : NULL;
        if (!subnet) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Invalid CIDR for nat firewall rule (private IPv4 CIDR required): %s", cidr ? cidr : "(null)");
            return FALSE;
        }
    } else if (g_strcmp0(mode, "isolated") == 0 || g_strcmp0(mode, "routed") == 0) {
        if (cidr && *cidr && !pcv_validate_private_cidr(cidr)) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Invalid CIDR for %s firewall rule (private CIDR required): %s", mode, cidr);
            return FALSE;
        }
    } else {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Unknown guest firewall mode: %s", mode ? mode : "(null)");
        return FALSE;
    }
    g_autofree gchar *p = g_build_filename(_sysnet(), bridge, NULL);
    if (!g_file_test(p, G_FILE_TEST_IS_DIR)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Bridge %s not found (%s)", bridge, p);
        return FALSE;
    }
    return TRUE;
}

                                                                                
                                           
static const gchar *
_json_str(JsonObject *o, const gchar *name)
{
    JsonNode *n = json_object_get_member(o, name);
    if (!n || JSON_NODE_TYPE(n) != JSON_NODE_VALUE || json_node_get_value_type(n) != G_TYPE_STRING)
        return NULL;
    return json_node_get_string(n);
}

                                                                                            
static PcvGuestFwNet *
_entry_from_json(JsonObject *o, const gchar *bridge_override)
{
    const gchar *bridge = bridge_override ? bridge_override : _json_str(o, "bridge");
    const gchar *mode = _json_str(o, "mode");
    const gchar *cidr = _json_str(o, "cidr");
    return _entry_check(bridge, mode, cidr, NULL) ? pcv_guest_fw_net_new(bridge, mode, cidr) : NULL;
}

                                                                
static gboolean
_list_has(GPtrArray *nets, const gchar *bridge)
{
    for (guint i = 0; i < nets->len; i++)
        if (g_strcmp0(((const PcvGuestFwNet *)g_ptr_array_index(nets, i))->bridge, bridge) == 0)
            return TRUE;
    return FALSE;
}

                                                                                     
                                                                    
static GPtrArray *
_list_read_file(void)
{
    g_autofree gchar *path = g_build_filename(_rundir(), GFW_LIST_FILE, NULL);
    g_autofree gchar *raw = NULL;
    if (!g_file_get_contents(path, &raw, NULL, NULL))
        return NULL;
    g_autoptr(JsonParser) parser = json_parser_new();
                                                                     
    if (!json_parser_load_from_data(parser, raw, -1, NULL)) {
        PCV_LOG_WARN(GFW_LOG_DOM, "%s 파싱 실패 — dnsmasq meta에서 다시 채운다", path);
        return NULL;
    }
    JsonNode *root = json_parser_get_root(parser);
    JsonObject *obj = root && JSON_NODE_HOLDS_OBJECT(root) ? json_node_get_object(root) : NULL;
    JsonNode *ver = obj ? json_object_get_member(obj, "version") : NULL;
    JsonNode *arr_node = obj ? json_object_get_member(obj, "nets") : NULL;
    if (!ver || JSON_NODE_TYPE(ver) != JSON_NODE_VALUE || json_node_get_value_type(ver) != G_TYPE_INT64
        || json_node_get_int(ver) != 1 || !arr_node || !JSON_NODE_HOLDS_ARRAY(arr_node)) {
        PCV_LOG_WARN(GFW_LOG_DOM, "%s 형식 오류(version 1·nets 배열 아님) — dnsmasq meta에서 다시 채운다", path);
        return NULL;
    }
    JsonArray *arr = json_node_get_array(arr_node);
    GPtrArray *nets = g_ptr_array_new_with_free_func(pcv_guest_fw_net_free);
    for (guint i = 0; i < json_array_get_length(arr); i++) {
        JsonNode *n = json_array_get_element(arr, i);
        PcvGuestFwNet *net = JSON_NODE_HOLDS_OBJECT(n) ? _entry_from_json(json_node_get_object(n), NULL) : NULL;
        if (net && !_list_has(nets, net->bridge)) {
            g_ptr_array_add(nets, net);
        } else {
            PCV_LOG_WARN(GFW_LOG_DOM, "%s 항목 %u 무시(검증 실패·브릿지 없음·중복)", path, i);
            pcv_guest_fw_net_free(net);
        }
    }
    return nets;
}

                                                                                       
                                                                                
static GPtrArray *
_list_seed_from_meta(void)
{
    GPtrArray *nets = g_ptr_array_new_with_free_func(pcv_guest_fw_net_free);
    GDir *dir = g_dir_open(_rundir(), 0, NULL);
    const gchar *name;
    while (dir && (name = g_dir_read_name(dir))) {
        if (!g_str_has_prefix(name, "dnsmasq-") || !g_str_has_suffix(name, ".meta"))
            continue;
                                                                     
        g_autofree gchar *bridge = g_strndup(name + 8, strlen(name) - 8 - 5);
        g_autofree gchar *path = g_build_filename(_rundir(), name, NULL);
        g_autofree gchar *raw = NULL;
        if (!g_file_get_contents(path, &raw, NULL, NULL))
            continue;
        g_autoptr(JsonParser) parser = json_parser_new();
                                                                                
        if (!json_parser_load_from_data(parser, raw, -1, NULL))
            continue;
        JsonNode *root = json_parser_get_root(parser);
        PcvGuestFwNet *net = root && JSON_NODE_HOLDS_OBJECT(root)
                             ? _entry_from_json(json_node_get_object(root), bridge) : NULL;
        if (net)
            g_ptr_array_add(nets, net);
    }
    if (dir)
        g_dir_close(dir);
    return nets;
}

                                                        
static gboolean
_list_write_file(GPtrArray *nets, GError **error)
{
    g_autoptr(JsonBuilder) b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "version");
    json_builder_add_int_value(b, 1);
    json_builder_set_member_name(b, "nets");
    json_builder_begin_array(b);
    for (guint i = 0; i < nets->len; i++) {
        const PcvGuestFwNet *n = g_ptr_array_index(nets, i);
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "bridge"); json_builder_add_string_value(b, n->bridge);
        json_builder_set_member_name(b, "mode");   json_builder_add_string_value(b, n->mode);
        json_builder_set_member_name(b, "cidr");   json_builder_add_string_value(b, n->cidr ? n->cidr : "");
        json_builder_end_object(b);
    }
    json_builder_end_array(b);
    json_builder_end_object(b);
    g_autoptr(JsonNode) root = json_builder_get_root(b);
    g_autoptr(JsonGenerator) gen = json_generator_new();
    json_generator_set_root(gen, root);
    g_autofree gchar *data = json_generator_to_data(gen, NULL);
    if (g_mkdir_with_parents(_rundir(), 0700) != 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "cannot create %s", _rundir());
        return FALSE;
    }
    g_autofree gchar *path = g_build_filename(_rundir(), GFW_LIST_FILE, NULL);
    return g_file_set_contents_full(path, data, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600, error);
}

                                                      
static void
_list_ensure_loaded_locked(void)
{
    if (g_guest_fw_nets)
        return;
    GPtrArray *nets = _list_read_file();
    g_guest_fw_nets = nets ? nets : _list_seed_from_meta();
}

                                                           
static GPtrArray *
_list_copy_without(const gchar *bridge)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(pcv_guest_fw_net_free);
    for (guint i = 0; g_guest_fw_nets && i < g_guest_fw_nets->len; i++) {
        const PcvGuestFwNet *n = g_ptr_array_index(g_guest_fw_nets, i);
        if (!bridge || g_strcmp0(n->bridge, bridge) != 0)
            g_ptr_array_add(out, pcv_guest_fw_net_new(n->bridge, n->mode, n->cidr));
    }
    return out;
}

                                                                                                                         
                                                                             
                                                                                    
                                                                              
static gboolean
_apply_locked(GPtrArray *nets, GError **error)
{
    PcvGuestFwSnapshot *snap = pcv_guest_fw_snapshot_new();
    for (guint i = 0; i < nets->len; i++) {
        const PcvGuestFwNet *n = g_ptr_array_index(nets, i);
        pcv_guest_fw_snapshot_add(snap, n->bridge, n->mode, n->cidr);
    }
    g_autofree gchar *script = pcv_guest_fw_build_script(snap);
    pcv_guest_fw_snapshot_free(snap);
    const gchar *argv[] = {"nft", "-f", "-", NULL};                                  
    g_autofree gchar *std_err = NULL;                                                               
    if (!pcv_spawn_sync_stdin(argv, script, -1, NULL, &std_err, error)) {
        g_prefix_error(error, "guest firewall apply: ");
        return FALSE;
    }
    return TRUE;
}

                                                                     
                                                                         
                                                             
static gboolean
_commit_locked(GPtrArray *next, GError **error)
{
    if (!_list_write_file(next, error))
        return FALSE;
    if (!_apply_locked(next, error)) {
        GError *restore_err = NULL;
        g_autoptr(GPtrArray) prev = _list_copy_without(NULL);
        if (!_list_write_file(prev, &restore_err)) {
            PCV_LOG_WARN(GFW_LOG_DOM, "guest-fw.json 원복 실패: %s", restore_err ? restore_err->message : "unknown");
            g_clear_error(&restore_err);
        }
        return FALSE;
    }
    g_clear_pointer(&g_guest_fw_nets, g_ptr_array_unref);
    g_guest_fw_nets = g_ptr_array_ref(next);
    return TRUE;
}

                                                           
                                                               
static gboolean
_upsert(const gchar *bridge, const gchar *mode, const gchar *cidr, GError **error)
{
    if (!_entry_check(bridge, mode, cidr, error))
        return FALSE;
                                                                        
    { const gchar *a[] = {"sysctl", "-w", "net.ipv4.ip_forward=1", NULL}; pcv_spawn_fire(a); }

    g_mutex_lock(&g_guest_fw_mu);
    _list_ensure_loaded_locked();
    g_autoptr(GPtrArray) next = _list_copy_without(bridge);
    g_ptr_array_add(next, pcv_guest_fw_net_new(bridge, mode, cidr));
    gboolean ok = _commit_locked(next, error);
    if (ok && _host_integration_enabled())
        pcv_host_fw_integrate(bridge, mode, NULL);                                                
    g_mutex_unlock(&g_guest_fw_mu);
    return ok;
}

                                                           
gboolean
network_firewall_setup_nat(const gchar *bridge_name, const gchar *cidr, GError **error)
{
    return _upsert(bridge_name, "nat", cidr, error);
}

                                                       
gboolean
network_firewall_setup_isolated(const gchar *bridge_name, const gchar *cidr, GError **error)
{
    return _upsert(bridge_name, "isolated", cidr, error);
}

                                                        
gboolean
network_firewall_setup_routed(const gchar *bridge_name, const gchar *cidr, GError **error)
{
    return _upsert(bridge_name, "routed", cidr, error);
}

                                                                      
                                                                        
                                                            
gboolean
network_firewall_teardown(const gchar *bridge_name, GError **error)
{
    if (!pcv_validate_bridge_name(bridge_name)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Invalid bridge name for firewall teardown: %s", bridge_name ? bridge_name : "(null)");
        return FALSE;
    }
    g_mutex_lock(&g_guest_fw_mu);
    _list_ensure_loaded_locked();
    g_autoptr(GPtrArray) next = _list_copy_without(bridge_name);
    GError *apply_err = NULL;
    if (!_commit_locked(next, &apply_err)) {
        PCV_LOG_WARN(GFW_LOG_DOM, "guest firewall teardown 적용 실패 %s (목록에서는 뺌): %s",
                     bridge_name, apply_err ? apply_err->message : "unknown");
        g_clear_error(&apply_err);
        GError *write_err = NULL;
        if (!_list_write_file(next, &write_err)) {
            PCV_LOG_WARN(GFW_LOG_DOM, "guest-fw.json 기록 실패: %s", write_err ? write_err->message : "unknown");
            g_clear_error(&write_err);
        }
        g_clear_pointer(&g_guest_fw_nets, g_ptr_array_unref);
        g_guest_fw_nets = g_ptr_array_ref(next);
    }
    if (_host_integration_enabled())
        pcv_host_fw_remove(bridge_name, NULL);                                         
    g_mutex_unlock(&g_guest_fw_mu);
    return TRUE;
}

                                                                             
                                                              
gboolean
network_firewall_reconcile(GError **error)
{
    g_mutex_lock(&g_guest_fw_mu);
    _list_ensure_loaded_locked();
    g_autoptr(GPtrArray) next = _list_copy_without(NULL);
    if (next->len > 0) {
        const gchar *a[] = {"sysctl", "-w", "net.ipv4.ip_forward=1", NULL};
        pcv_spawn_fire(a);
    }
    gboolean ok = _commit_locked(next, error);
    if (ok && _host_integration_enabled()) {
        for (guint i = 0; i < next->len; i++) {
            const PcvGuestFwNet *n = g_ptr_array_index(next, i);
            pcv_host_fw_integrate(n->bridge, n->mode, NULL);                                     
        }
    }
    guint count = next->len;
    g_mutex_unlock(&g_guest_fw_mu);
    if (ok)
        PCV_LOG_INFO(GFW_LOG_DOM, "guest firewall reconciled (nets=%u)", count);
    else
        PCV_LOG_AUDIT(GFW_LOG_DOM, "network.guest_fw_reconcile", "inet purecvisor", "nets=%u result=fail", count);
    return ok;
}

                                                                                        
                                                                      
                                                               
void
network_firewall_set_paths_for_test(const gchar *rundir, const gchar *sys_class_net)
{
    g_mutex_lock(&g_guest_fw_mu);
    g_free(g_gfw_rundir_override);
    g_gfw_rundir_override = g_strdup(rundir);
    g_free(g_gfw_sysnet_override);
    g_gfw_sysnet_override = g_strdup(sys_class_net);
    g_clear_pointer(&g_guest_fw_nets, g_ptr_array_unref);
    g_mutex_unlock(&g_guest_fw_mu);
}
