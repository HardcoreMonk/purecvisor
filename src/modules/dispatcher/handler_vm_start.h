/**
 * @file handler_vm_start.h
 * @brief VM 시작(vm.start) RPC 핸들러 공개 인터페이스
 *
 * [RPC 메서드 매핑]
 *   vm.start -> handle_vm_start_request()
 *
 * [핸들러 시그니처] (모든 디스패처 핸들러 공통)
 *   void handler(JsonObject *params, const gchar *rpc_id,
 *                UdsServer *server, GSocketConnection *connection)
 *
 * [fire-and-forget 패턴]
 *   이 핸들러는 비동기 패턴을 사용합니다.
 *   응답을 먼저 전송한 뒤 GTask로 libvirt 기동 작업을 수행하므로,
 *   콜백에서 send_response를 호출하면 안 됩니다 (소켓이 이미 닫힘).
 *
 * [호출 경로]
 *   dispatcher.c -> handle_vm_start_request() -> GTask -> libvirt + CPU 핀닝
 */
#ifndef PURECVISOR_DISPATCHER_HANDLER_VM_START_H
#define PURECVISOR_DISPATCHER_HANDLER_VM_START_H

#include <glib.h>
#include <json-glib/json-glib.h>
#include "api/uds_server.h"

G_BEGIN_DECLS

void handle_vm_start_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

G_END_DECLS

#endif