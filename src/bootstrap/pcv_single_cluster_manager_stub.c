#include "bootstrap/pcv_single_edge_runtime.h"

#include <gio/gio.h>















static gboolean g_single_maintenance = FALSE;

static JsonObject *
single_edge_result_object(const gchar *status, const gchar *message)
{
    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "edition", "single_edge");
    if (status)
        json_object_set_string_member(obj, "status", status);
    if (message)
        json_object_set_string_member(obj, "message", message);
    return obj;
}

PcvClusterRole
pcv_cluster_get_role(void)
{
    return PCV_CLUSTER_DISABLED;
}

const gchar *
pcv_cluster_get_role_str(void)
{
    return "standalone";
}

JsonObject *
pcv_cluster_get_status(void)
{
    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "edition", "single_edge");
    json_object_set_boolean_member(obj, "enabled", FALSE);
    json_object_set_string_member(obj, "role", "standalone");
    json_object_set_boolean_member(obj, "quorum", FALSE);
    json_object_set_boolean_member(obj, "maintenance", g_single_maintenance);
    return obj;
}

JsonObject *
pcv_cluster_get_repl_status(void)
{
    JsonObject *obj = single_edge_result_object("disabled",
                                                "Replication is unavailable in Single Edge");
    json_object_set_boolean_member(obj, "enabled", FALSE);
    return obj;
}

gboolean
pcv_cluster_check_quorum(void)
{
    return FALSE;
}

void
pcv_cluster_trigger_replication(void)
{
}

void
pcv_cluster_trigger_failover_test(void)
{
}

__attribute__((weak)) gboolean
pcv_cluster_check_zvol_fence(void)
{
    return TRUE;
}

JsonObject *
pcv_cluster_enter_maintenance(void)
{
    g_single_maintenance = TRUE;
    JsonObject *obj = single_edge_result_object("ok",
                                                "Single Edge maintenance mode enabled");
    json_object_set_boolean_member(obj, "maintenance", TRUE);
    return obj;
}

JsonObject *
pcv_cluster_exit_maintenance(void)
{
    g_single_maintenance = FALSE;
    JsonObject *obj = single_edge_result_object("ok",
                                                "Single Edge maintenance mode disabled");
    json_object_set_boolean_member(obj, "maintenance", FALSE);
    return obj;
}

gboolean
pcv_cluster_is_maintenance(void)
{
    return g_single_maintenance;
}

__attribute__((weak)) PcvEtcdClient *
pcv_cluster_get_etcd(void)
{
    return NULL;
}

__attribute__((weak)) void
pcv_cluster_sync_vm_xml(const gchar *vm_name)
{
    (void)vm_name;
}

__attribute__((weak)) void
pcv_cluster_remove_vm_xml(const gchar *vm_name)
{
    (void)vm_name;
}

JsonObject *
pcv_cluster_drain_node(const gchar *node_name)
{
    JsonObject *obj = single_edge_result_object("unsupported",
                                                "Node drain is unavailable in Single Edge");
    if (node_name)
        json_object_set_string_member(obj, "node", node_name);
    return obj;
}

JsonObject *
pcv_cluster_resume_node(const gchar *node_name)
{
    JsonObject *obj = single_edge_result_object("unsupported",
                                                "Node resume is unavailable in Single Edge");
    if (node_name)
        json_object_set_string_member(obj, "node", node_name);
    return obj;
}

JsonObject *
pcv_cluster_upgrade_status(void)
{
    return single_edge_result_object("unsupported",
                                     "Cluster upgrade status is unavailable in Single Edge");
}

JsonObject *
pcv_cluster_evacuate_node(const gchar *node_name)
{
    JsonObject *obj = single_edge_result_object("unsupported",
                                                "Node evacuation is unavailable in Single Edge");
    if (node_name)
        json_object_set_string_member(obj, "node", node_name);
    return obj;
}

JsonObject *
pcv_cluster_config_push(const gchar *section, const gchar *key, const gchar *value)
{
    JsonObject *obj = single_edge_result_object("standalone",
                                                "Cluster-wide config propagation is disabled in Single Edge");
    if (section)
        json_object_set_string_member(obj, "section", section);
    if (key)
        json_object_set_string_member(obj, "key", key);
    if (value)
        json_object_set_string_member(obj, "value", value);
    return obj;
}

__attribute__((weak)) void
pcv_cluster_notify_config_reload(void)
{
}

JsonObject *
pcv_cluster_config_get(const gchar *section, const gchar *key)
{
    JsonObject *obj = single_edge_result_object("standalone",
                                                "Cluster-wide config propagation is disabled in Single Edge");
    if (section)
        json_object_set_string_member(obj, "section", section);
    if (key)
        json_object_set_string_member(obj, "key", key);
    return obj;
}
