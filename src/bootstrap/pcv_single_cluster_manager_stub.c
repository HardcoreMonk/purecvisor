/**
 * @file pcv_single_cluster_manager_stub.c
 * @brief Single Edge 클러스터 매니저 stub — Multi Edge 전용 심볼의 standalone 구현.
 *
 * [왜 stub 인가]
 *   quorum·복제·노드 drain/evacuate·클러스터 config 전파는 Multi Edge 전용이며
 *   공개 Single Edge 빌드에 포함되지 않는다. 그러나 dispatcher/health/UI 는 edition
 *   과 무관하게 동일한 cluster.* 심볼을 링크·호출한다. 이 파일은 그 심볼들을 전부
 *   정의해 링크 실패나 미정의 동작 없이 단일 노드에서 일관되게 응답한다.
 *
 * [계약 — 호출되면 무엇을 반환/거부하는가]
 *   - role/status/repl 조회: DISABLED · "standalone" · enabled=FALSE 로 고정.
 *   - quorum: 노드가 하나뿐이라 정족수 개념이 없어 항상 FALSE.
 *   - zvol fence: 경합할 다른 노드가 없어 항상 통과(TRUE).
 *   - drain/resume/evacuate/upgrade/config-push: "unsupported"/"standalone" JSON
 *     으로 거부하며 부작용을 남기지 않는다.
 *   - maintenance flag: 단일 노드에서도 의미가 있어 유일하게 프로세스 메모리에 보관.
 *
 * [주니어 참고]
 *   빈 함수·FALSE·"unsupported" 는 미완성이 아니라 제품 경계다. __attribute__((weak))
 *   심볼은 Multi Edge 빌드/테스트가 실제 구현으로 override 할 수 있게 남긴 링크
 *   seam 이며, 여기에 임시 클러스터 초기화를 넣으면 Single Edge 공개 범위가 깨진다.
 */
#include "bootstrap/pcv_single_edge_runtime.h"

#include <gio/gio.h>

/*
 * Single Edge용 클러스터 매니저 stub.
 *
 * [비전공자 설명]
 * 단독 서버에는 quorum, 복제, 노드 drain 같은 "여러 서버를 묶는 기능"이
 * 없습니다. 그래도 화면이나 health check가 클러스터 상태를 물어볼 수 있으므로
 * 이 파일은 항상 일관된 standalone/unsupported 응답을 반환합니다.
 *
 * [주니어 참고]
 * 빈 함수와 FALSE 반환은 미완성이 아니라 제품 경계입니다. 예외적으로
 * maintenance flag처럼 단일 서버에서도 의미가 있는 상태만 메모리에 보관합니다.
 * weak 함수들은 클러스터 빌드나 테스트에서 더 구체적인 구현으로 대체될 수
 * 있도록 남겨 둔 링크 경계입니다.
 */
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

/* zvol fence 는 "다른 노드가 같은 zvol 을 동시에 잡지 않았는가"를 확인하는 안전
 * 게이트다. Single Edge 에는 경합할 노드가 없어 항상 통과시킨다. weak 로 두어
 * Multi Edge 가 실제 etcd 기반 fence 로 교체할 수 있게 한다. */
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
