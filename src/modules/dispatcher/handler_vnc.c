/**
 * @file handler_vnc.c
 * @brief VNC 연결 정보 조회 RPC 핸들러 — get_vnc_info
 *
 * [아키텍처 위치]
 *   클라이언트 -> UDS/REST -> dispatcher.c ("get_vnc_info") -> handle_vnc_request()
 *                                                                -> virt_conn_pool (libvirt 연결)
 *                                                                -> virDomainGetXMLDesc (XML 파싱)
 *
 * [처리하는 RPC 메서드] (1개)
 *   get_vnc_info -> handle_vnc_request : 실행 중 VM의 VNC 포트/WebSocket 포트 조회
 *     - params: { "vm_id": "<이름 또는 UUID>" }
 *     - 응답: { "vnc_port": 5900, "websocket_port": 5700, "listen": "0.0.0.0" }
 *
 * [fire-and-forget 패턴 미사용]
 *   동기 응답입니다. libvirt XML 파싱이 즉시 완료되므로 비동기 처리가 불필요합니다.
 *
 * [주의사항]
 *   - pure_virt_get_domain()은 handler_vm_lifecycle.c에 정의된 extern 함수로,
 *     VM 이름 또는 UUID 어느 쪽으로든 도메인을 검색합니다.
 *   - VM이 실행 중이 아니면 VNC 포트가 할당되지 않으므로 에러를 반환합니다.
 *   - noVNC 웹 콘솔 연동 시 이 엔드포인트로 포트를 조회합니다.
 *
 * [에러 코드]
 *   -32602 : 필수 파라미터(vm_id) 누락
 *   -32001 : 지정한 VM이 존재하지 않음
 *   -32000 : libvirt 연결 실패 또는 XML 파싱 실패
 */
#include "handler_vnc.h"
#include "rpc_utils.h"
#include "modules/virt/virt_conn_pool.h"
#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>
#include <string.h>
#include <stdlib.h>

/*
 * handler_vm_lifecycle.c에 정의된 다형성 검색 함수를 재사용합니다.
 * UUID 또는 이름 어느 쪽으로든 VM 도메인을 검색할 수 있습니다.
 * 링크 시 handler_vm_lifecycle.o에서 심볼이 해결됩니다.
 */
extern virDomainPtr pure_virt_get_domain(virConnectPtr conn, const gchar *identifier);

/**
 * handle_vnc_request:
 * get_vnc_info RPC 진입점 — 실행 중 VM의 VNC 접속 정보를 조회합니다.
 *
 * @param params: { "vm_id": "<이름 또는 UUID>" }
 * @param rpc_id: JSON-RPC 요청 ID
 * @param server: UDS 서버 인스턴스
 * @param connection: 클라이언트 소켓 연결
 * @return: { "host": "0.0.0.0", "port": 5900 }
 *
 * [처리 흐름]
 *   1. 파라미터 검증 (vm_id 필수)
 *   2. virt_conn_pool에서 libvirt 연결 획득
 *   3. pure_virt_get_domain()으로 VM 검색 (UUID 또는 이름)
 *   4. Live XML에서 <graphics type='vnc'> 태그를 문자열 파싱
 *   5. VNC 포트와 listen 주소를 추출하여 JSON 응답 반환
 *
 * [동기 응답] XML 파싱이 즉시 완료되므로 fire-and-forget 미사용.
 *
 * [noVNC 통합] Web UI의 noVNC 콘솔이 이 엔드포인트로 VNC 포트를 조회한 뒤
 *               ws://host:8080/api/v1/ws/vnc 웹소켓 프록시를 통해 연결합니다.
 */
void handle_vnc_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    /* 파라미터 검증: vm_id는 필수 */
    const gchar *vm_id = json_object_get_string_member(params, "vm_id");
    if (!vm_id) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing parameter: vm_id");
        pure_uds_server_send_response(server, connection, err); g_free(err); return;
    }

    /* 커넥션 풀에서 libvirt 연결 획득 (서킷 브레이커 포함) */
    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "Hypervisor Connection Failed");
        pure_uds_server_send_response(server, connection, err); g_free(err); return;
    }

    /* 1단계: 다형성 검색 — UUID 또는 이름 어느 쪽으로든 VM을 찾습니다 */
    virDomainPtr dom = pure_virt_get_domain(conn, vm_id);
    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "VM Entity not found");
        pure_uds_server_send_response(server, connection, err); g_free(err); virt_conn_pool_release(conn); return;
    }

    /*
     * 2단계: 실행 중인 VM의 실시간(Live) XML 정보 추출
     *
     * 플래그 0: 메모리 상의 동적 XML을 가져옵니다.
     * VNC autoport가 활성화되면 런타임에 포트가 할당되므로
     * 반드시 Live XML에서 포트를 읽어야 합니다 (정적 XML에는 port='-1').
     */
    gchar *xml = virDomainGetXMLDesc(dom, 0);
    gint vnc_port = -1;
    gchar *host = g_strdup("127.0.0.1");  /* 기본값: localhost */

    if (xml) {
        /*
         * XML 간이 파싱: <graphics type='vnc' port='5900' listen='...'> 태그 검색
         *
         * [파싱 전략]
         *   1. "type='vnc'" 태그 위치를 찾음
         *   2. 해당 태그 내에서 port='...' 추출
         *   3. listen='...' 추출 (0.0.0.0이면 외부 접근 가능)
         *
         * [쌍따옴표 대응] libvirt 버전에 따라 작은따옴표(') 또는 큰따옴표(")를
         *                 사용할 수 있으므로 양쪽 모두 체크합니다.
         */
        gchar *vnc_tag = strstr(xml, "<graphics type='vnc'");
        if (vnc_tag) {
            gchar *port_attr = strstr(vnc_tag, "port='");
            if (!port_attr) port_attr = strstr(vnc_tag, "port=\"");  /* 큰따옴표 대응 */

            if (port_attr) {
                vnc_port = atoi(port_attr + 6);  /* "port='" 길이(6) 건너뛰고 숫자 파싱 */
            }

            /* Listen Address 추출: 0.0.0.0이면 외부에서 VNC 접속 가능 */
            gchar *listen_attr = strstr(vnc_tag, "listen='");
            if (!listen_attr) listen_attr = strstr(vnc_tag, "listen=\"");
            if (listen_attr && strncmp(listen_attr + 8, "0.0.0.0", 7) == 0) {
                g_free(host);
                host = g_strdup("0.0.0.0");
            }
        }
        free(xml);  /* libvirt API 반환값은 libc free()로 해제 (g_free 아님!) */
    }

    virDomainFree(dom);
    virt_conn_pool_release(conn);  /* 커넥션 풀에 연결 반환 */

    /* 3단계: 결과 JSON 조립 및 반환 */
    if (vnc_port != -1 && vnc_port != 0) {
        /* VNC 포트가 유효한 경우: {"host": "...", "port": 5900} */
        JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
        JsonObject *res_obj = json_object_new();
        json_object_set_string_member(res_obj, "host", host);
        json_object_set_int_member(res_obj, "port", vnc_port);
        json_node_take_object(res_node, res_obj);

        gchar *resp = pure_rpc_build_success_response(rpc_id, res_node);
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
    } else {
        /* VNC 포트를 찾지 못한 경우 — VM shutoff 또는 VNC 어댑터 없음.
         * 빈 결과를 성공으로 반환하여 UI 콘솔 에러를 방지.
         * UI는 port=-1을 보고 "VM을 시작하세요" 메시지를 표시. */
        JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
        JsonObject *res_obj = json_object_new();
        json_object_set_string_member(res_obj, "host", host);
        json_object_set_int_member(res_obj, "port", -1);
        json_object_set_string_member(res_obj, "message",
            "VNC not available — VM is not running or has no VNC graphics adapter");
        json_node_take_object(res_node, res_obj);
        gchar *resp = pure_rpc_build_success_response(rpc_id, res_node);
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
    }
    
    g_free(host);
}