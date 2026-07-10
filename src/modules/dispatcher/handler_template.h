/**
 * @file handler_template.h
 * @brief VM 템플릿 관리 RPC 핸들러 공개 인터페이스
 *
 * ============================================================================
 * [RPC 메서드 매핑] (4개)
 * ============================================================================
 *   template.list   -> handle_template_list   (동기 — 전체 템플릿 목록)
 *   template.get    -> handle_template_get    (동기 — 단일 템플릿 상세)
 *   template.create -> handle_template_create (동기 — 생성/갱신 UPSERT)
 *   template.delete -> handle_template_delete (동기 — 삭제)
 *
 * ============================================================================
 * [핸들러 시그니처] (모든 디스패처 핸들러 공통)
 * ============================================================================
 *   void handler(JsonObject *params, const gchar *rpc_id,
 *                UdsServer *server, GSocketConnection *connection)
 *
 * ============================================================================
 * [모든 핸들러 동기 응답 — fire-and-forget 미사용]
 * ============================================================================
 *   템플릿 데이터는 /etc/purecvisor/templates/ 디렉터리 내 JSON 파일로 관리되며,
 *   파일 I/O가 즉시 완료되므로 비동기 처리가 불필요합니다.
 *
 * ============================================================================
 * [호출 경로]
 * ============================================================================
 *   dispatcher.c -> handle_template_*() -> vm_template.c (JSON 파일 CRUD)
 *
 * ============================================================================
 * [기본 제공 프리셋 템플릿] (vm_template.c에서 초기화)
 * ============================================================================
 *   ubuntu-small  : 1 vCPU / 1GB RAM / 10GB 디스크
 *   ubuntu-medium : 2 vCPU / 4GB RAM / 20GB 디스크
 *   ubuntu-large  : 4 vCPU / 8GB RAM / 40GB 디스크
 *   — 모두 cloud-init 자동 프로비저닝 지원
 *
 * ============================================================================
 * [CLI / REST 사용 예시]
 * ============================================================================
 *   CLI:  pcvctl template list
 *         pcvctl template create web --vcpu 2 --memory_mb 2048 --disk_gb 20 --os_variant ubuntu24.04
 *         pcvctl template delete web
 *   REST: GET  /api/v1/templates           (template.list)
 *         GET  /api/v1/templates/{name}    (template.get)
 *         POST /api/v1/templates           (template.create)
 *         DELETE /api/v1/templates/{name}  (template.delete)
 *
 * ============================================================================
 * [주의사항]
 * ============================================================================
 *   - UdsServer 전방 선언(typedef struct _UdsServer UdsServer)으로
 *     헤더 순환 참조를 방지합니다. 실제 정의는 api/uds_server.h에 있습니다.
 *   - G_BEGIN_DECLS / G_END_DECLS: C++ 호환성 매크로 (extern "C" {} 래핑)
 */

#ifndef PURECVISOR_HANDLER_TEMPLATE_H
#define PURECVISOR_HANDLER_TEMPLATE_H

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* UdsServer 전방 선언 — 헤더 순환 참조 방지 (실제 정의는 api/uds_server.h) */
typedef struct _UdsServer UdsServer;

/**
 * @brief 전체 템플릿 목록 조회 (template.list)
 * params: {} (불필요)
 * 반환: [{"name","vcpu","memory_mb","disk_gb","os_variant",...}, ...]
 */
void handle_template_list  (JsonObject *params, const gchar *rpc_id,
                            UdsServer *server, GSocketConnection *conn);

/**
 * @brief 단일 템플릿 상세 조회 (template.get)
 * params: {"name": str}
 * 반환: {"name","vcpu","memory_mb","disk_gb","os_variant",...}
 * 에러: -32602 (이름 무효 / 미존재)
 */
void handle_template_get   (JsonObject *params, const gchar *rpc_id,
                            UdsServer *server, GSocketConnection *conn);

/**
 * @brief 템플릿 생성 또는 갱신 — UPSERT (template.create)
 * params: {"name","vcpu","memory_mb","disk_gb","os_variant", "iso_path"?, "network_bridge"?,
 *          "cloud_init_user_data"?, "description"?}
 * 범위: vcpu 1~128, memory_mb 128~1048576, disk_gb 1~65536
 * 반환: {"status":"created","name":"..."}
 */
void handle_template_create(JsonObject *params, const gchar *rpc_id,
                            UdsServer *server, GSocketConnection *conn);

/**
 * @brief 템플릿 삭제 (template.delete)
 * params: {"name": str}
 * 반환: {"status":"deleted","name":"..."}
 * 에러: -32602 (이름 무효), -32000 (미존재 / 파일 삭제 실패)
 */
void handle_template_delete(JsonObject *params, const gchar *rpc_id,
                            UdsServer *server, GSocketConnection *conn);

G_END_DECLS

#endif /* PURECVISOR_HANDLER_TEMPLATE_H */
