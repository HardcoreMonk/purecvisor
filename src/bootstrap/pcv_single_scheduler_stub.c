#include "bootstrap/pcv_single_edge_runtime.h"

#include <gio/gio.h>

/*
 * Single Edge용 클러스터 스케줄러 stub.
 *
 * [비전공자 설명]
 * 클러스터 스케줄러는 "어느 서버에 VM을 만들지" 고르는 기능입니다. 단독
 * 서버에는 선택할 다른 노드가 없으므로 VM 생성 스케줄링은 지원하지 않습니다.
 *
 * [주니어 참고]
 * create_vm은 NULL + NOT_SUPPORTED를 반환해 잘못된 API 사용을 즉시 드러냅니다.
 * 반면 label/affinity 조회·삭제 계열은 빈 값 또는 성공 no-op으로 둡니다.
 * 공통 UI가 설정 화면을 조회할 때 Single Edge 서버가 불필요하게 에러 상태로
 * 보이지 않게 하기 위한 호환 계층입니다.
 */
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
