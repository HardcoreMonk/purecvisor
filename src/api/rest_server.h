/**
 * @file rest_server.h
 * @brief HTTP REST API 서버 공개 인터페이스
 *
 * 아키텍처 위치:
 *   main.c에서 include합니다.
 *   main.c: pcv_rest_server_new(dispatcher, port) → pcv_rest_server_start() → pcv_rest_server_stop()
 *
 * 사용 패턴 (main.c에서):
 *   PcvRestServer *rest = pcv_rest_server_new(dispatcher, 0);  // 0이면 daemon.conf에서 포트 읽음
 *   GError *err = NULL;
 *   if (!pcv_rest_server_start(rest, &err)) { ... 에러 처리 ... }
 *   // ... GMainLoop 실행 ...
 *   pcv_rest_server_stop(rest);   // cleanup 블록에서 호출
 *   g_object_unref(rest);
 *
 * 기본 포트: 8080 (daemon.conf의 [daemon] rest_port 키로 변경 가능)
 * 인증: JWT HS256 Bearer 토큰 (POST /api/v1/auth/token으로 발급)
 *
 * REST 엔드포인트 목록 (인증 불필요):
 *   GET  /api/v1/health              헬스체크 (서비스 상태, 버전)
 *   GET  /api/v1/metrics             Prometheus text format 메트릭
 *   GET  /api/v1/internal/vms        클러스터 프록시 전용 VM 목록
 *   GET  /api/v1/internal/telemetry  스케줄러 전용 호스트 메트릭
 *   POST /api/v1/auth/token          JWT 로그인 토큰 발급
 *
 * REST 엔드포인트 목록 (JWT 필요):
 *   GET/POST/DELETE /api/v1/vms              VM CRUD
 *   POST /api/v1/vms/{name}/start|stop       VM 제어
 *   GET  /api/v1/vms/{name}/metrics          VM 메트릭
 *   GET/POST/DELETE /api/v1/vms/{name}/snapshot  스냅샷 관리
 *   PUT  /api/v1/vms/{name}/vcpu|memory      핫플러그
 *   GET/POST/DELETE /api/v1/vms/{name}/nics  NIC 핫플러그
 *   POST/DELETE /api/v1/vms/{name}/iso       ISO 마운트/이젝트
 *   GET/POST/DELETE /api/v1/containers       컨테이너 관리
 *   GET/POST/DELETE /api/v1/networks         네트워크 관리
 *   GET/POST/DELETE /api/v1/storage/zvols    스토리지 관리
 *   GET/POST /api/v1/monitor/metrics|fleet   모니터링
 *
 * GObject 타입: PCV_TYPE_REST_SERVER
 */

#ifndef PCV_REST_SERVER_H
#define PCV_REST_SERVER_H

#include <glib-object.h>
#include "dispatcher.h"

G_BEGIN_DECLS

#define PCV_TYPE_REST_SERVER (pcv_rest_server_get_type())

G_DECLARE_FINAL_TYPE(PcvRestServer, pcv_rest_server,
                     PCV, REST_SERVER, GObject)

/**
 * pcv_rest_server_new:
 * @dispatcher: 기존 PureCVisorDispatcher 인스턴스 (transfer none)
 * @port:        수신 포트 (0 이면 pcv_config에서 읽음)
 *
 * Returns: 새 PcvRestServer 인스턴스 (transfer full)
 */
PcvRestServer *pcv_rest_server_new(PureCVisorDispatcher *dispatcher,
                                   guint16               port);

/**
 * pcv_rest_server_start:
 * 서버를 시작합니다. GMainLoop 이벤트 루프에 통합됩니다.
 */
gboolean pcv_rest_server_start(PcvRestServer *self, GError **error);

/**
 * pcv_rest_server_stop:
 * 서버를 정지합니다. main.c Cleanup 단계에서 호출.
 */
void pcv_rest_server_stop(PcvRestServer *self);

G_END_DECLS

#endif /* PCV_REST_SERVER_H */
