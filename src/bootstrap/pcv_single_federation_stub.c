#include "bootstrap/pcv_single_edge_runtime.h"

#include <gio/gio.h>














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

void
pcv_federation_init(void)
{
}

void
pcv_federation_shutdown(void)
{
}

gboolean
pcv_federation_site_join(const gchar *site_id, const gchar *control_url, GError **error)
{
    (void)site_id;
    (void)control_url;
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                "Federation is unavailable in Single Edge");
    return FALSE;
}

gboolean
pcv_federation_site_leave(const gchar *site_id, GError **error)
{
    (void)site_id;
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                "Federation is unavailable in Single Edge");
    return FALSE;
}

JsonArray *
pcv_federation_site_list(void)
{
    return json_array_new();
}

JsonObject *
pcv_federation_site_status(const gchar *site_id)
{
    JsonObject *obj = single_edge_result_object("unsupported",
                                                "Federation is unavailable in Single Edge");
    if (site_id)
        json_object_set_string_member(obj, "site_id", site_id);
    return obj;
}

gboolean
pcv_federation_node_join(const gchar *node_name, const gchar *ip, GError **error)
{
    (void)node_name;
    (void)ip;
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                "Federation is unavailable in Single Edge");
    return FALSE;
}

gboolean
pcv_federation_node_leave(const gchar *node_name, GError **error)
{
    (void)node_name;
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                "Federation is unavailable in Single Edge");
    return FALSE;
}

JsonArray *
pcv_federation_node_list(void)
{
    return json_array_new();
}
