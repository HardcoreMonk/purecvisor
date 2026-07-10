#include "bootstrap/pcv_single_edge_runtime.h"

#include <gio/gio.h>

/*
 * Single Edge용 federation stub.
 *
 * [비전공자 설명]
 * federation은 여러 사이트나 여러 노드를 묶어 운영하는 기능입니다. 이 단독
 * 버전에서는 사이트/노드 join과 leave를 허용하지 않고, 조회 API는 빈 목록이나
 * "지원하지 않음" 상태를 반환합니다.
 *
 * [주니어 참고]
 * init/shutdown이 비어 있는 것은 정상입니다. join/leave처럼 상태를 바꾸는
 * 요청은 G_IO_ERROR_NOT_SUPPORTED로 실패시켜 호출자가 사용자에게 명확히
 * 표시하게 하고, list는 빈 배열을 돌려 UI가 안전하게 렌더링되도록 합니다.
 */
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
