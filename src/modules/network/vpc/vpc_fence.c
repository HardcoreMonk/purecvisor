   
                    
                                                                                
  
                           
                                                                                   
                      
                                                                         
                                                                                            
                                                                                                    
                                                                               
                                                                             
                                                                                              
                           
  
                                                                  
                                                        
   
#include "vpc_fence.h"

#include "vpc_backend_ovn.h"
#include "vpc_model.h"
#include "vpc_policy_nft.h"
#include "utils/pcv_log.h"
#include "utils/pcv_spawn.h"

static gboolean
_nft_apply(const gchar *script, GError **error)
{
    const gchar *check_argv[] = { "nft", "-c", "-f", "-", NULL };
    g_autofree gchar *stderr_text = NULL;
    if (!pcv_spawn_sync_stdin(check_argv, script, -1, NULL, &stderr_text, error)) {
        if (error && !*error)
            g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_IO,
                        "nft policy validation 실패: %s", stderr_text ? stderr_text : "unknown");
        return FALSE;
    }
    const gchar *apply_argv[] = { "nft", "-f", "-", NULL };
    g_clear_pointer(&stderr_text, g_free);
    if (!pcv_spawn_sync_stdin(apply_argv, script, -1, NULL, &stderr_text, error)) {
        if (error && !*error)
            g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_IO,
                        "nft policy apply 실패: %s", stderr_text ? stderr_text : "unknown");
        return FALSE;
    }
    return TRUE;
}

gboolean
pcv_vpc_fence_nft_quarantine(PcvVpcStore *store, GError **error)
{
    g_autoptr(GPtrArray) bridges = pcv_vpc_store_list_managed_bridges(store, error);
    if (!bridges) return FALSE;
    if (bridges->len == 0) return TRUE;
    g_autofree gchar *script = pcv_vpc_policy_build_quarantine_script(bridges);
    return _nft_apply(script, error);
}

static gboolean
_vpc_is_ovn(JsonObject *vpc)
{
    return g_strcmp0(json_object_get_string_member(vpc, "backend"), "ovn") == 0;
}

                                                
static gboolean
_vpc_quarantine_applies(JsonObject *vpc, const gchar *skip_vpc_id)
{
    return _vpc_is_ovn(vpc) &&
        g_strcmp0(json_object_get_string_member(vpc, "state"), "CREATING") != 0 &&
        g_strcmp0(json_object_get_string_member(vpc, "id"), skip_vpc_id) != 0;
}

                                                                          
                                                                                                
                                                                                   
                                                                         
                               
static void
_unquarantine_first(JsonArray *vpcs, guint count, const gchar *skip_vpc_id)
{
    for (guint j = 0; j < count; j++) {
        JsonObject *earlier = json_array_get_object_element(vpcs, j);
        if (_vpc_quarantine_applies(earlier, skip_vpc_id) &&
            g_strcmp0(json_object_get_string_member(earlier, "state"), "DEGRADED") != 0)
            (void)pcv_vpc_ovn_set_quarantine(earlier, FALSE, NULL);
    }
}

                                                                           
                                                                             
                                                      
gboolean
pcv_vpc_fence_quarantine(PcvVpcStore *store, const gchar *skip_vpc_id, GError **error)
{
    g_autoptr(JsonArray) vpcs = pcv_vpc_store_list_vpcs(store, NULL, error);
    if (!vpcs) return FALSE;
    guint length = json_array_get_length(vpcs);
    for (guint i = 0; i < length; i++) {
        JsonObject *vpc = json_array_get_object_element(vpcs, i);
        if (!_vpc_quarantine_applies(vpc, skip_vpc_id) ||
            pcv_vpc_ovn_set_quarantine(vpc, TRUE, error))
            continue;
        _unquarantine_first(vpcs, i + 1, skip_vpc_id);
        return FALSE;
    }
    if (pcv_vpc_fence_nft_quarantine(store, error))
        return TRUE;
    _unquarantine_first(vpcs, length, skip_vpc_id);
    return FALSE;
}

gboolean
pcv_vpc_fence_full_policy(PcvVpcStore *store, GError **error)
{
    g_autoptr(PcvVpcPolicySnapshot) snapshot = pcv_vpc_store_policy_snapshot(store, error);
    if (!snapshot) return FALSE;
    g_autofree gchar *script = pcv_vpc_policy_build_script(snapshot);
    return _nft_apply(script, error);
}

                                                                                
                                                 
                                                                                                              
                                                                                       
                                                                               
                                                            
static gboolean
_clear_ovn(PcvVpcStore *store, const gchar *keep_vpc_id, const gchar *target_vpc_id,
           GHashTable *degraded, GError **error)
{
    g_autoptr(JsonArray) vpcs = pcv_vpc_store_list_vpcs(store, NULL, error);
    if (!vpcs) return FALSE;
    gboolean ok = TRUE;
    for (guint i = 0; i < json_array_get_length(vpcs); i++) {
        JsonObject *vpc = json_array_get_object_element(vpcs, i);
        const gchar *id = json_object_get_string_member(vpc, "id");
        if (!_vpc_is_ovn(vpc) || g_strcmp0(id, keep_vpc_id) == 0) continue;
        g_autoptr(GError) stage_error = NULL;
        g_autoptr(JsonArray) attachments = pcv_vpc_store_list_attachments(store, id, NULL, &stage_error);
        if (attachments && pcv_vpc_ovn_sync_policy(vpc, attachments, FALSE, &stage_error)) continue;
        if (degraded && g_error_matches(stage_error, PCV_VPC_ERROR, PCV_VPC_ERROR_PORT_GROUP_MISSING)) {
            g_autoptr(GError) db_error = NULL;
            const gboolean recorded = pcv_vpc_fence_ovn_failed(store, degraded, id, stage_error, &db_error);
            if (recorded && g_strcmp0(id, target_vpc_id) != 0) continue;
            if (!recorded) {
                g_clear_error(&stage_error);
                stage_error = g_steal_pointer(&db_error);
            }
        }
        if (ok) g_propagate_error(error, g_steal_pointer(&stage_error));
        ok = FALSE;
    }
    return ok;
}

gboolean
pcv_vpc_fence_clear_ovn(PcvVpcStore *store, const gchar *target_vpc_id, GError **error)
{
    g_autoptr(GHashTable) degraded = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    const gboolean ok = _clear_ovn(store, NULL, target_vpc_id, degraded, error);
    pcv_vpc_fence_ovn_failed_edges_down(degraded);
    return ok;
}

void
pcv_vpc_fence_policy_sync_failed(PcvVpcStore *store, const gchar *vpc_id, const GError *cause)
{
    if (!g_error_matches(cause, PCV_VPC_ERROR, PCV_VPC_ERROR_PORT_GROUP_MISSING)) {
        (void)pcv_vpc_store_set_resource_state(store, "vpcs", vpc_id, "DEGRADED",
            cause ? cause->message : "OVN Security Group ACL sync failed", NULL);
        return;
    }
    g_autoptr(GHashTable) degraded = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    (void)pcv_vpc_fence_ovn_failed(store, degraded, vpc_id, cause, NULL);
    pcv_vpc_fence_ovn_failed_edges_down(degraded);
}

void
pcv_vpc_fence_release(PcvVpcStore *store, const gchar *keep_vpc_id)
{
    g_autoptr(GError) nft_error = NULL;
    if (!pcv_vpc_fence_full_policy(store, &nft_error))
        PCV_LOG_WARN("VPC", "실패한 변경 뒤 nft 격리 해제 실패(reconcile 이 복구한다): %s", nft_error->message);
    g_autoptr(GError) ovn_error = NULL;
    if (!_clear_ovn(store, keep_vpc_id, NULL, NULL, &ovn_error))
        PCV_LOG_WARN("VPC", "실패한 변경 뒤 OVN 격리 해제 실패(reconcile 이 복구한다): %s", ovn_error->message);
}

static void
_link_down(const gchar *iface)
{
    const gchar *argv[] = { "ip", "link", "set", "dev", iface, "down", NULL };
    pcv_spawn_sync(argv, NULL, NULL, NULL);
}

void
pcv_vpc_fence_all_down(PcvVpcStore *store)
{
    g_autoptr(GPtrArray) bridges = pcv_vpc_store_list_managed_bridges(store, NULL);
    for (guint i = 0; bridges && i < bridges->len; i++)
        _link_down(g_ptr_array_index(bridges, i));
}

gboolean
pcv_vpc_fence_ovn_failed(PcvVpcStore *store, GHashTable *failed, const gchar *vpc_id,
                         const GError *cause, GError **error)
{
    const gchar *message = cause && cause->message ? cause->message : "OVN backend 수렴 실패";
    PCV_LOG_WARN("VPC", "OVN VPC %s 수렴 실패 — 이 VPC 만 DEGRADED 로 격리: %s", vpc_id, message);
    if (!g_hash_table_contains(failed, vpc_id))
        g_hash_table_insert(failed, g_strdup(vpc_id), g_strdup(message));
    return pcv_vpc_store_set_resource_state(store, "vpcs", vpc_id, "DEGRADED", message, error);
}

gboolean
pcv_vpc_fence_reconcile_ovn_begin(PcvVpcStore *store, JsonArray *vpcs,
                                  const gchar *transit_pool, GHashTable *failed, GError **error)
{
    for (guint i = 0; i < json_array_get_length(vpcs); i++) {
        JsonObject *vpc = json_array_get_object_element(vpcs, i);
        if (!_vpc_is_ovn(vpc)) continue;
        const gchar *id = json_object_get_string_member(vpc, "id");
        g_autoptr(GError) stage_error = NULL;
        gboolean ok = !_vpc_quarantine_applies(vpc, NULL) ||
            pcv_vpc_ovn_set_quarantine(vpc, TRUE, &stage_error);
        g_autoptr(JsonObject) binding = ok ? pcv_vpc_store_get_backend_binding(
            store, id, NULL, &stage_error) : NULL;
        ok = ok && binding && pcv_vpc_ovn_require_ready(transit_pool, &stage_error) &&
             pcv_vpc_ovn_ensure_vpc(vpc, binding, &stage_error);
        if (!ok && !pcv_vpc_fence_ovn_failed(store, failed, id, stage_error, error))
            return FALSE;
    }
    return TRUE;
}

gboolean
pcv_vpc_fence_reconcile_ovn_clear(PcvVpcStore *store, GHashTable *failed, GError **error)
{
    g_autoptr(JsonArray) vpcs = pcv_vpc_store_list_vpcs(store, NULL, error);
    if (!vpcs) return FALSE;
    for (guint i = 0; i < json_array_get_length(vpcs); i++) {
        JsonObject *vpc = json_array_get_object_element(vpcs, i);
        const gchar *id = json_object_get_string_member(vpc, "id");
        if (!_vpc_is_ovn(vpc) || g_hash_table_contains(failed, id)) continue;
        g_autoptr(JsonArray) attachments = pcv_vpc_store_list_attachments(store, id, NULL, error);
        if (!attachments) return FALSE;
        g_autoptr(GError) stage_error = NULL;
        if (!pcv_vpc_ovn_sync_policy(vpc, attachments, FALSE, &stage_error) &&
            !pcv_vpc_fence_ovn_failed(store, failed, id, stage_error, error))
            return FALSE;
    }
    return TRUE;
}

void
pcv_vpc_fence_ovn_failed_edges_down(GHashTable *failed)
{
    GHashTableIter it;
    gpointer key;
    g_hash_table_iter_init(&it, failed);
    while (g_hash_table_iter_next(&it, &key, NULL)) {
        g_autofree gchar *edge = pcv_vpc_ovn_edge_iface_name_from_id(key);
        _link_down(edge);
    }
}
