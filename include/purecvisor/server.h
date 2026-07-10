/**
 * @file server.h
 * @brief UDS(Unix Domain Socket) 서버 공개 API
 *
 * ====================================================================
 *  파일 역할
 * ====================================================================
 *  현재 에디션 데몬의 UDS 서버 인터페이스를 외부에 노출하는 헤더이다.
 *  Opaque Pointer 패턴으로 UdsServer 내부 구현을 완전히 은닉하며,
 *  생성(new), 시작(start), 정지(stop) 3개 API만 공개한다.
 *
 * ====================================================================
 *  아키텍처 위치
 * ====================================================================
 *  API 계층(src/api/)의 진입점. 데몬 메인 함수에서 uds_server_new()로
 *  서버를 생성하고, uds_server_start()로 소켓 리슨을 시작한다.
 *  클라이언트(pcvctl/pcvtui) 연결이 들어오면 JSON-RPC 요청을 수신하여
 *  dispatcher.c로 라우팅한다.
 *
 *    main() -> uds_server_new("/var/run/purecvisor/daemon.sock")
 *           -> uds_server_start()
 *           -> GMainLoop 진입
 *           -> 클라이언트 연결 수신 -> dispatcher -> 핸들러
 *
 * ====================================================================
 *  핵심 패턴
 * ====================================================================
 *  - Opaque Pointer: UdsServer 구조체의 멤버는 .c 파일에서만 정의.
 *    헤더를 포함하는 외부 코드는 포인터로만 조작하며 내부에 접근 불가.
 *  - Socket Activation 지원: systemd fd=3 계승 방식도 내부적으로
 *    uds_server.c에서 처리하나, 이 헤더 API에는 영향 없음.
 *
 * ====================================================================
 *  주의사항
 * ====================================================================
 *  - uds_server_start() 실패 시 GError에 원인이 설정된다.
 *    호출자가 g_error_free()로 해제해야 한다.
 *  - uds_server_stop() 호출 후 서버 객체를 재사용하지 말 것.
 *  - stdbool.h 포함 필수 (bool 타입 사용).
 */

#ifndef PURECVISOR_SERVER_H
#define PURECVISOR_SERVER_H

#include <stdbool.h> // [수정] 필수 추가!
#include <glib.h>
#include <gio/gio.h>

/* Opaque Pointer */
typedef struct UdsServer UdsServer;

/* Server API */
UdsServer* uds_server_new(const char *socket_path);
bool uds_server_start(UdsServer *self, GError **error);
void uds_server_stop(UdsServer *self);

#endif
