/**
 * @file dispatcher.h
 * @brief JSON-RPC 2.0 디스패처 공개 인터페이스
 *
 * 아키텍처 위치:
 *   main.c, uds_server.c, rest_server.c에서 include합니다.
 *   main.c는 purecvisor_dispatcher_new()로 인스턴스를 생성하고,
 *   uds_server.c는 purecvisor_dispatcher_dispatch()로 수신된 JSON-RPC를 라우팅합니다.
 *
 * 주요 API:
 *   - purecvisor_dispatcher_new(): 디스패처 인스턴스 생성 (내부에서 vm_manager도 생성)
 *   - purecvisor_dispatcher_set_connection(): libvirt GVirConnection 설정
 *   - purecvisor_dispatcher_get_vm_manager(): 텔레메트리 연동용 vm_manager 참조 획득
 *   - purecvisor_dispatcher_dispatch(): JSON-RPC 요청 문자열을 파싱하여 핸들러에 분배
 *
 * 시그니처 변경 이력:
 *   Phase 5에서 (Dispatcher, JsonNode, OutputStream) → (Dispatcher, UdsServer,
 *   SocketConnection, RawString)으로 변경. UDS 서버가 파싱 전 원본 문자열을 전달하고,
 *   디스패처가 직접 JSON 파싱을 수행합니다.
 *
 * GObject 타입: PURECVISOR_TYPE_DISPATCHER
 *   G_DECLARE_FINAL_TYPE 매크로로 PureCVisorDispatcher 타입을 선언합니다.
 *   PURECVISOR_DISPATCHER(obj) 캐스팅 매크로를 자동 생성합니다.
 */

#ifndef PURECVISOR_DISPATCHER_H
#define PURECVISOR_DISPATCHER_H

#include <glib-object.h>
#include <json-glib/json-glib.h>
#include <libvirt-gobject/libvirt-gobject.h>
#include "uds_server.h"
#include "../modules/virt/vm_manager.h"  /* GIO P6: PureCVisorVmManager */

G_BEGIN_DECLS

#define PURECVISOR_TYPE_DISPATCHER (purecvisor_dispatcher_get_type())

G_DECLARE_FINAL_TYPE(PureCVisorDispatcher, purecvisor_dispatcher, PURECVISOR, DISPATCHER, GObject)

PureCVisorDispatcher *purecvisor_dispatcher_new(void);

void purecvisor_dispatcher_set_connection(PureCVisorDispatcher *self, GVirConnection *conn);

/**
 * purecvisor_dispatcher_get_vm_manager:
 *
 * (transfer none): 내부 vm_manager 포인터 반환.
 * GIO P6: telemetry 초기화에서 vm-metrics-updated 신호 연결에 사용.
 */
PureCVisorVmManager *purecvisor_dispatcher_get_vm_manager(PureCVisorDispatcher *self);

/* [Phase 5 Updated Signature] 
 * 기존: (Dispatcher, JsonNode, OutputStream)
 * 변경: (Dispatcher, UdsServer, SocketConnection, RawString)
 */
void purecvisor_dispatcher_dispatch(PureCVisorDispatcher *self, 
                                   UdsServer *server, 
                                   GSocketConnection *connection, 
                                   const gchar *request_json);

/**
 * dispatcher_shutdown_routes:
 * 라우트 테이블(GHashTable) 해제. 데몬 종료 시 호출.
 */
void dispatcher_shutdown_routes(void);

/**
 * PcvDispatchHook:
 * pre-dispatch 미들웨어 훅 (BE-A5).
 * @method: RPC 메서드명
 * @params: 요청 파라미터 (읽기 전용)
 * @rpc_id: 요청 ID
 * @user_data: 등록 시 전달한 사용자 데이터
 * @return: TRUE면 계속 진행, FALSE면 요청 거부 (-32000 에러 응답)
 */
typedef gboolean (*PcvDispatchHook)(const gchar *method, JsonObject *params,
                                     const gchar *rpc_id, gpointer user_data);

/**
 * pcv_dispatcher_register_pre_hook:
 * pre-dispatch 훅 등록. 핸들러 호출 전 순차 실행됩니다.
 * 플러그인/모듈에서 호출하여 커스텀 미들웨어를 추가할 수 있습니다.
 */
void pcv_dispatcher_register_pre_hook(PcvDispatchHook hook, gpointer user_data);

/* bootstrap 전용: edition-specific RPC registration bridge */
void pcv_dispatcher_handle_cluster_maintenance_enter(JsonObject *params, const gchar *rpc_id,
                                                     UdsServer *server, GSocketConnection *connection);
void pcv_dispatcher_handle_cluster_maintenance_exit(JsonObject *params, const gchar *rpc_id,
                                                    UdsServer *server, GSocketConnection *connection);
void pcv_dispatcher_handle_cluster_affinity_set(JsonObject *params, const gchar *rpc_id,
                                                UdsServer *server, GSocketConnection *connection);
void pcv_dispatcher_handle_cluster_affinity_list(JsonObject *params, const gchar *rpc_id,
                                                 UdsServer *server, GSocketConnection *connection);
void pcv_dispatcher_handle_cluster_affinity_delete(JsonObject *params, const gchar *rpc_id,
                                                   UdsServer *server, GSocketConnection *connection);
void pcv_dispatcher_handle_cluster_node_label_set(JsonObject *params, const gchar *rpc_id,
                                                  UdsServer *server, GSocketConnection *connection);
void pcv_dispatcher_handle_cluster_node_label_get(JsonObject *params, const gchar *rpc_id,
                                                  UdsServer *server, GSocketConnection *connection);
void pcv_dispatcher_handle_cluster_node_label_delete(JsonObject *params, const gchar *rpc_id,
                                                     UdsServer *server, GSocketConnection *connection);
void pcv_dispatcher_handle_cluster_node_drain(JsonObject *params, const gchar *rpc_id,
                                              UdsServer *server, GSocketConnection *connection);
void pcv_dispatcher_handle_cluster_node_resume(JsonObject *params, const gchar *rpc_id,
                                               UdsServer *server, GSocketConnection *connection);
void pcv_dispatcher_handle_cluster_upgrade_status(JsonObject *params, const gchar *rpc_id,
                                                  UdsServer *server, GSocketConnection *connection);
void pcv_dispatcher_handle_cluster_config_push(JsonObject *params, const gchar *rpc_id,
                                               UdsServer *server, GSocketConnection *connection);
void pcv_dispatcher_handle_cluster_config_get(JsonObject *params, const gchar *rpc_id,
                                              UdsServer *server, GSocketConnection *connection);
void pcv_dispatcher_handle_federation_site_join(JsonObject *params, const gchar *rpc_id,
                                                UdsServer *server, GSocketConnection *connection);
void pcv_dispatcher_handle_federation_site_list(JsonObject *params, const gchar *rpc_id,
                                                UdsServer *server, GSocketConnection *connection);
void pcv_dispatcher_handle_federation_site_remove(JsonObject *params, const gchar *rpc_id,
                                                  UdsServer *server, GSocketConnection *connection);

G_END_DECLS

#endif /* PURECVISOR_DISPATCHER_H */
