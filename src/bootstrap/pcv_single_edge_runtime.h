#ifndef PCV_SINGLE_EDGE_RUNTIME_H
#define PCV_SINGLE_EDGE_RUNTIME_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/*
 * Single Edge 호환 런타임 API.
 *
 * [비전공자 설명]
 * 제품 안에는 "클러스터", "스케줄러", "federation"이라는 큰 기능 이름이
 * 남아 있지만, 이 단독 버전에서는 실제 여러 서버 제어를 하지 않습니다.
 * UI나 공통 핸들러가 같은 질문을 해도 서버가 죽지 않고 "지원하지 않음" 또는
 * 빈 목록을 돌려주도록 이 헤더가 함수 모양을 제공합니다.
 *
 * [주니어 참고]
 * 이 헤더는 클러스터 기능을 켜는 스위치가 아닙니다. 링크 단계에서 필요한
 * 심볼을 Single 전용 stub으로 제공하는 계약입니다. 실제 분산 락, quorum,
 * 노드 배치 같은 동작은 이 파일의 선언만으로 구현되지 않으며, ADR 적용
 * 상태를 확인하지 않고 stub을 실제 동작처럼 바꾸면 안 됩니다.
 */
typedef struct _PcvEtcdClient PcvEtcdClient;

typedef enum {
    PCV_CLUSTER_DISABLED = 0
} PcvClusterRole;

PcvClusterRole pcv_cluster_get_role(void);
const gchar *pcv_cluster_get_role_str(void);
JsonObject *pcv_cluster_get_status(void);
JsonObject *pcv_cluster_get_repl_status(void);
gboolean pcv_cluster_check_quorum(void);
void pcv_cluster_trigger_replication(void);
void pcv_cluster_trigger_failover_test(void);
gboolean pcv_cluster_check_zvol_fence(void);
JsonObject *pcv_cluster_enter_maintenance(void);
JsonObject *pcv_cluster_exit_maintenance(void);
gboolean pcv_cluster_is_maintenance(void);
PcvEtcdClient *pcv_cluster_get_etcd(void);
void pcv_cluster_sync_vm_xml(const gchar *vm_name);
void pcv_cluster_remove_vm_xml(const gchar *vm_name);
JsonObject *pcv_cluster_drain_node(const gchar *node_name);
JsonObject *pcv_cluster_resume_node(const gchar *node_name);
JsonObject *pcv_cluster_upgrade_status(void);
JsonObject *pcv_cluster_evacuate_node(const gchar *node_name);
JsonObject *pcv_cluster_config_push(const gchar *section,
                                    const gchar *key,
                                    const gchar *value);
void pcv_cluster_notify_config_reload(void);
JsonObject *pcv_cluster_config_get(const gchar *section, const gchar *key);

gboolean pcv_etcd_acquire_inflight_lock(PcvEtcdClient *c,
                                        const gchar *pool,
                                        const gchar *node_name,
                                        const gchar *op,
                                        gint ttl_sec,
                                        GError **error);
gboolean pcv_etcd_release_inflight_lock(PcvEtcdClient *c,
                                        const gchar *pool,
                                        GError **error);
gint pcv_etcd_compute_inflight_ttl(const gchar *op, gint size_gb);

void pcv_federation_init(void);
void pcv_federation_shutdown(void);
gboolean pcv_federation_site_join(const gchar *site_id,
                                  const gchar *control_url,
                                  GError **error);
gboolean pcv_federation_site_leave(const gchar *site_id, GError **error);
JsonArray *pcv_federation_site_list(void);
JsonObject *pcv_federation_site_status(const gchar *site_id);
gboolean pcv_federation_node_join(const gchar *node_name,
                                  const gchar *ip,
                                  GError **error);
gboolean pcv_federation_node_leave(const gchar *node_name, GError **error);
JsonArray *pcv_federation_node_list(void);

void pcv_scheduler_init(const gchar *peers_csv, gint rest_port);
void pcv_scheduler_shutdown(void);
JsonObject *pcv_scheduler_create_vm(const gchar *name,
                                    gint vcpu,
                                    gint ram_mb,
                                    gint disk_gb,
                                    const gchar *bridge,
                                    const gchar *anti_affinity_group,
                                    JsonObject *node_selector,
                                    GError **error);
void pcv_scheduler_affinity_set(const gchar *group,
                                const gchar **vms,
                                gboolean anti);
void pcv_scheduler_affinity_delete(const gchar *group);
JsonArray *pcv_scheduler_affinity_list(void);
gboolean pcv_scheduler_node_label_set(const gchar *node, JsonObject *labels);
JsonObject *pcv_scheduler_node_label_get(const gchar *node);
gboolean pcv_scheduler_node_label_delete(const gchar *node, const gchar *key);

G_END_DECLS

#endif /* PCV_SINGLE_EDGE_RUNTIME_H */
