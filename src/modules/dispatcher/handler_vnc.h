/**
 * @file handler_vnc.h
 * @brief VNC 및 WebSocket 연결 정보 조회 디스패처 헤더
 *
 * 실행 중인 가상 머신(VM)의 Libvirt XML을 파싱하여 
 * 웹 콘솔(noVNC) 연결에 필요한 포트 정보를 반환하는 모듈의 인터페이스입니다.
 */

#ifndef PURECVISOR_DISPATCHER_HANDLER_VNC_H
#define PURECVISOR_DISPATCHER_HANDLER_VNC_H

#include <glib.h>
#include <json-glib/json-glib.h>
#include "api/uds_server.h" // UdsServer, GSocketConnection 타입 참조

/* C++ 호환성을 위한 Name Mangling 방지 매크로 */
G_BEGIN_DECLS

// 🚀 서명 업데이트 (Phase 6 규격)
void handle_vnc_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/* * RpcConnection 구조체 전방 선언 (Forward Declaration)
 * API 계층과 디스패처 계층의 순환 참조(Circular Dependency)를 방지합니다.
 */
//typedef struct _RpcConnection RpcConnection;

/**
 * @brief 클라이언트의 VNC 포트 정보 조회 요청을 비동기로 처리합니다.
 *
 * @param request 파싱된 JSON-RPC 요청 데이터 (내부에서 "vm_id" 추출)
 * @param conn 클라이언트와 연결된 RPC 커넥션 객체 (결과를 비동기로 응답할 때 사용)
 * * @note 이 함수는 GTask를 생성하여 백그라운드 스레드로 작업을 넘기고 
 * 즉시 리턴하므로 MainLoop를 블로킹하지 않습니다.
 */
//void handle_vnc_request(gpointer request, RpcConnection *conn);

G_END_DECLS

#endif /* PURECVISOR_DISPATCHER_HANDLER_VNC_H */