#include "bootstrap/pcv_single_edge_runtime.h"

#include <gio/gio.h>

void
pcv_scheduler_init(const gchar *peers_csv, gint rest_port)
{
    (void)peers_csv;
    (void)rest_port;
}

void
pcv_scheduler_shutdown(void)
{
}

JsonObject *
pcv_scheduler_create_vm(const gchar *name, gint vcpu, gint ram_mb, gint disk_gb,
                        const gchar *bridge, const gchar *anti_affinity_group,
                        JsonObject *node_selector, GError **error)
{
    (void)name;
    (void)vcpu;
    (void)ram_mb;
    (void)disk_gb;
    (void)bridge;
    (void)anti_affinity_group;
    (void)node_selector;
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                "Cluster scheduler is unavailable in Single Edge");
    return NULL;
}

void
pcv_scheduler_affinity_set(const gchar *group, const gchar **vms, gboolean anti)
{
    (void)group;
    (void)vms;
    (void)anti;
}

void
pcv_scheduler_affinity_delete(const gchar *group)
{
    (void)group;
}

JsonArray *
pcv_scheduler_affinity_list(void)
{
    return json_array_new();
}

gboolean
pcv_scheduler_node_label_set(const gchar *node, JsonObject *labels)
{
    (void)node;
    (void)labels;
    return TRUE;
}

JsonObject *
pcv_scheduler_node_label_get(const gchar *node)
{
    JsonObject *obj = json_object_new();
    if (node)
        json_object_set_string_member(obj, "node", node);
    return obj;
}

gboolean
pcv_scheduler_node_label_delete(const gchar *node, const gchar *key)
{
    (void)node;
    (void)key;
    return TRUE;
}
