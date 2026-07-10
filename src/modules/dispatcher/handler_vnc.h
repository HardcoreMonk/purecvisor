/**
 * @file handler_vnc.h
 * @brief VNC 연결 정보 조회 RPC 핸들러 공개 인터페이스
 *
 * ──────────────────────────────────────────────────────────────
 * [아키텍처 내 위치]
 *   dispatcher.c → handle_vnc_request() → virt_conn_pool → libvirt XML 파싱
 *
 * [RPC 메서드 매핑] (1개)
 *   get_vnc_info -> handle_vnc_request (동기 — VNC/WebSocket 포트 정보 반환)
 *
 * [핸들러 시그니처] (모든 디스패처 핸들러 공통)
 *   void handler(JsonObject *params, const gchar *rpc_id,
 *                UdsServer *server, GSocketConnection *connection)
 *
 * [동작 원리]
 *   1. params에서 "name" (VM 이름)을 추출한다
 *   2. virt_conn_pool에서 libvirt 연결을 획득한다
 *   3. virDomainGetXMLDesc()로 VM의 XML 정의를 가져온다
 *   4. XML에서 <graphics type='vnc'> 엘리먼트를 파싱한다
 *   5. VNC 포트, WebSocket 포트, listen 주소를 JSON으로 반환한다
 *
 * [응답 형식]
 *   성공: {"port": 5900, "websocket": 5700, "listen": "0.0.0.0"}
 *   실패: VM이 실행 중이 아니거나 VNC가 미설정이면 에러 응답
 *
 * [Web UI 연동]
 *   noVNC 클라이언트가 이 정보로 ws://host:80/api/v1/ws/vnc에 접속하여
 *   WebSocket-to-TCP 프록시(ws_server.c)를 통해 VNC 세션을 수립한다.
 * ──────────────────────────────────────────────────────────────
 */

#ifndef PURECVISOR_DISPATCHER_HANDLER_VNC_H
#define PURECVISOR_DISPATCHER_HANDLER_VNC_H

#include <glib.h>
#include <json-glib/json-glib.h>
#include "api/uds_server.h"

G_BEGIN_DECLS

/**
 * @brief 실행 중인 VM의 VNC 연결 정보를 조회하여 반환한다 (동기).
 *
 * @param params     {"name": "<vm_name>"} — 조회할 VM 이름 (필수)
 * @param rpc_id     JSON-RPC 요청 ID
 * @param server     UDS 서버 인스턴스 (응답 전송에 사용)
 * @param connection 클라이언트 소켓 연결 (응답 전송 후 닫힘)
 */
void handle_vnc_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

G_END_DECLS

#endif /* PURECVISOR_DISPATCHER_HANDLER_VNC_H */
