#ifndef PURECVISOR_WS_SERVER_H
#define PURECVISOR_WS_SERVER_H

/**
 * @file ws_server.h
 * @brief WebSocket 서버 — 실시간 이벤트 스트림 + VNC 프록시
 *
 * ──────────────────────────────────────────────────────────────
 * [아키텍처 내 위치]
 *   REST 서버(rest_server.c)의 libsoup3 SoupServer 인스턴스 위에
 *   WebSocket 업그레이드 핸들러를 등록하여, HTTP 연결을 WebSocket으로
 *   승격(upgrade)시키는 서브시스템이다.
 *
 * [제공 엔드포인트]
 *   1. ws://host:80/api/v1/ws/events
 *      - 실시간 텔레메트리, VM 상태 변경, 클러스터 이벤트를 JSON으로 push
 *      - Web UI 대시보드의 실시간 모니터링에 사용
 *      - 텔레메트리(telemetry.c)에서 1초 간격으로 수집한 메트릭을
 *        pcv_ws_broadcast()로 모든 연결된 클라이언트에 동시 전송
 *
 *   2. ws://host:80/api/v1/ws/vnc
 *      - noVNC 호환 WebSocket-to-TCP VNC 프록시
 *      - 브라우저의 noVNC 클라이언트 ↔ WebSocket ↔ 로컬 VNC 포트(5900+n)
 *      - VM별 VNC 디스플레이 포트를 자동 조회하여 TCP 소켓으로 중계
 *
 * [연결 관리]
 *   - 최대 동시 연결: MAX_WS_CONNECTIONS=1000 (초과 시 거부 + 경고 로그)
 *   - 연결/해제 시 내부 GList에서 클라이언트 추적
 *   - 브로드캐스트 시 끊어진 연결은 자동 제거
 *
 * [스레드 안전성]
 *   - pcv_ws_broadcast()는 GMainLoop 컨텍스트에서 호출되어야 한다.
 *     (텔레메트리 콜백, virt_events 콜백 등은 모두 GMainLoop에서 실행)
 *   - pcv_ws_client_count()는 어디서든 호출 가능 (atomic 읽기)
 *
 * [의존 모듈]
 *   - rest_server.c   : SoupServer 인스턴스 제공 (pcv_ws_server_init에 전달)
 *   - telemetry.c     : 주기적 메트릭 → pcv_ws_broadcast() 호출
 *   - virt_events.c   : VM 상태 변경 이벤트 → pcv_ws_broadcast() 호출
 *   - handler_vnc.c   : VNC 연결 정보(포트) 조회
 *
 * [프로토콜 형식 — 이벤트 스트림]
 *   {
 *     "type": "telemetry" | "vm_event" | "cluster_event",
 *     "data": { ... }    // type별 페이로드
 *   }
 * ──────────────────────────────────────────────────────────────
 */

#include <glib.h>
#include <libsoup/soup.h>

G_BEGIN_DECLS

/**
 * @brief WebSocket 서버를 초기화하고 SoupServer에 핸들러를 등록한다.
 *
 * REST 서버 초기화(pcv_rest_server_start) 직후에 호출된다.
 * 내부적으로 soup_server_add_websocket_handler()를 사용하여
 * /api/v1/ws/events 및 /api/v1/ws/vnc 경로에 핸들러를 등록한다.
 *
 * @param soup  REST 서버가 생성한 SoupServer 인스턴스.
 *              NULL이면 WebSocket 기능이 비활성화된다.
 *              이 포인터의 소유권은 호출자(rest_server)에 있으며,
 *              ws_server는 참조만 유지한다.
 */
void pcv_ws_server_init(SoupServer *soup);

/**
 * @brief WebSocket 서버를 종료하고 모든 연결을 닫는다.
 *
 * 데몬 종료(drain) 시 호출된다. 연결된 모든 WebSocket 클라이언트에
 * close 프레임을 전송하고, 내부 클라이언트 목록을 정리한다.
 * 이후 pcv_ws_broadcast() 호출은 무시된다.
 */
void pcv_ws_server_shutdown(void);

/**
 * @brief 연결된 모든 WebSocket 클라이언트에 이벤트를 브로드캐스트한다.
 *
 * JSON 형식의 메시지를 모든 활성 연결에 동시 전송한다.
 * 전송 실패(연결 끊김)한 클라이언트는 자동으로 목록에서 제거된다.
 *
 * 호출 컨텍스트: GMainLoop 스레드에서만 호출해야 한다.
 * (텔레메트리, virt_events 콜백은 모두 GMainLoop에서 실행되므로 안전)
 *
 * @param type          이벤트 타입 문자열 (예: "telemetry", "vm_event",
 *                      "cluster_event"). JSON의 "type" 필드에 들어간다.
 * @param payload_json  이벤트 데이터의 JSON 문자열. JSON의 "data" 필드에
 *                      들어간다. 호출자가 메모리를 소유하며, 함수 반환 후
 *                      해제 가능 (내부에서 복사하지 않고 즉시 전송).
 */
void pcv_ws_broadcast(const gchar *type, const gchar *payload_json);

/**
 * @brief 현재 연결된 WebSocket 클라이언트 수를 반환한다.
 *
 * /health 엔드포인트, Prometheus 메트릭(purecvisor_ws_clients),
 * 연결 제한 검사에 사용된다.
 *
 * 스레드 안전: atomic 읽기를 사용하므로 어느 스레드에서든 호출 가능.
 *
 * @return 현재 활성 WebSocket 연결 수 (0 이상)
 */
gint pcv_ws_client_count(void);

/**
 * @brief 비동기 작업 완료 이벤트를 모든 WebSocket 클라이언트에 브로드캐스트한다.
 *
 * ADR-0012: fire-and-forget 비동기 결과 채널.
 * GTask 워커 완료 콜백에서 호출하여 작업 결과를 실시간으로 전달한다.
 *
 * 전송 형식:
 *   {"type":"job.complete","ts":1234567890,"payload":{
 *     "job_id":"job-XXXXXXXX","method":"vm.create",
 *     "status":"completed|failed","error":"..."}}
 *
 * 내부적으로 pcv_ws_broadcast("job.complete", payload_json)을 호출한다.
 * GMainLoop 스레드가 아닌 곳에서 호출해도 안전하다 (g_idle_add 우회 불필요,
 * pcv_ws_broadcast 자체가 뮤텍스로 보호됨).
 *
 * @param job_id    작업 ID ("job-XXXXXXXX")
 * @param method    RPC 메서드명 (예: "vm.create")
 * @param status    최종 상태 ("completed" 또는 "failed")
 * @param error_msg 에러 메시지 (성공 시 NULL)
 */
void pcv_ws_broadcast_job_complete(const gchar *job_id, const gchar *method,
                                    const gchar *status, const gchar *error_msg);

G_END_DECLS

#endif /* PURECVISOR_WS_SERVER_H */
