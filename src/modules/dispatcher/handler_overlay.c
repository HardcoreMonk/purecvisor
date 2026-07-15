/**
 * @file handler_overlay.c
 * @brief 네트워크 인프라 RPC 핸들러 — OVS 오버레이 + iSCSI + OVN SDN (27개 메서드)
 *
 * [아키텍처 위치]
 *   클라이언트 -> UDS/REST -> dispatcher.c ("overlay.*","iscsi.*","ovn.*")
 *                              -> handle_overlay_*() / handle_iscsi_*() / handle_ovn_*()
 *                                  -> ovs_overlay.c (ovs-vsctl CLI 래퍼)
 *                                  -> iscsi_manager.c (tgtadm/iscsiadm CLI 래퍼)
 *                                  -> ovn_sdn.c (ovn-nbctl CLI 래퍼)
 *
 * 이 파일은 네트워크 인프라 관련 RPC 핸들러를 3개 섹션으로 구분합니다:
 *
 * [섹션 1] OVS 오버레이 (6개 RPC)
 *   overlay.create      -> handle_overlay_create      : OVS 브릿지 + VXLAN 터널 생성
 *   overlay.delete      -> handle_overlay_delete      : OVS 브릿지 삭제
 *   overlay.list        -> handle_overlay_list        : 전체 오버레이 목록
 *   overlay.info        -> handle_overlay_info        : 특정 오버레이 상세 (포트, VNI, 피어)
 *   overlay.add_peer    -> handle_overlay_add_peer    : VXLAN 피어 추가 (원격 노드 IP)
 *   overlay.remove_peer -> handle_overlay_remove_peer : VXLAN 피어 제거
 *
 * [섹션 2] iSCSI (5개 RPC)
 *   iscsi.target.create -> handle_iscsi_target_create : tgtadm으로 iSCSI 타겟 생성 (ZFS zvol 기반)
 *   iscsi.target.delete -> handle_iscsi_target_delete : iSCSI 타겟 삭제
 *   iscsi.target.list   -> handle_iscsi_target_list   : 전체 타겟 목록
 *   iscsi.connect       -> handle_iscsi_connect       : 이니시에이터 -> 타겟 연결 (iscsiadm)
 *   iscsi.disconnect    -> handle_iscsi_disconnect    : 이니시에이터 연결 해제
 *
 * [섹션 3] OVN SDN (16개 RPC)
 *   ovn.switch.create   -> handle_ovn_switch_create   : 논리 스위치 생성 (ovn-nbctl ls-add)
 *   ovn.switch.delete   -> handle_ovn_switch_delete   : 논리 스위치 삭제
 *   ovn.switch.list     -> handle_ovn_switch_list     : 논리 스위치 목록
 *   ovn.port.add        -> handle_ovn_port_add        : 논리 포트 추가 (ovn-nbctl lsp-add)
 *   ovn.port.remove     -> handle_ovn_port_remove     : 논리 포트 제거
 *   ovn.acl.add         -> handle_ovn_acl_add         : ACL 규칙 추가 (ovn-nbctl acl-add)
 *   ovn.acl.list        -> handle_ovn_acl_list        : ACL 규칙 목록
 *   ovn.router.create   -> handle_ovn_router_create   : 논리 라우터 생성 (ovn-nbctl lr-add)
 *   ovn.router.delete   -> handle_ovn_router_delete   : 논리 라우터 삭제
 *   ovn.router.list     -> handle_ovn_router_list     : 논리 라우터 목록
 *   ovn.router.add_port -> handle_ovn_router_add_port : 라우터<->스위치 포트 연결
 *   ovn.dhcp.enable     -> handle_ovn_dhcp_enable     : OVN 분산 DHCP 설정
 *   ovn.nat.add         -> handle_ovn_nat_add         : NAT 규칙 추가 (lr-nat-add)
 *   ovn.nat.list        -> handle_ovn_nat_list        : NAT 규칙 목록 (lr-nat-list)
 *   ovn.tenant.create   -> handle_ovn_tenant_create   : 멀티테넌트 격리 네트워크 일괄 생성
 *   ovn.status          -> handle_ovn_status           : OVN 컨트롤러 상태 조회
 *
 * [fire-and-forget 패턴 미사용]
 *   모든 핸들러가 동기 응답입니다.
 *   ovs-vsctl, tgtadm, ovn-nbctl 명령이 즉시 완료되므로 비동기 처리가 불필요합니다.
 *
 * [주의사항]
 *   - OVN 명령(ovn-nbctl)이 미설치된 환경에서는 graceful degradation으로
 *     빈 배열이나 멱등 성공을 반환합니다 (test_ovn.c에서 검증).
 *   - ovn.tenant.create는 스위치 + ACL + DHCP를 한 번에 프로비저닝하는 복합 RPC입니다.
 *   - iSCSI 타겟 ID는 자동 증가하며, tgtadm --lld iscsi로 관리합니다.
 *
 * [에러 코드]
 *   -32602 : 필수 파라미터 누락 (bridge_name, vni, target_ip 등)
 *   -32000 : 외부 명령(ovs-vsctl, tgtadm, ovn-nbctl) 실행 실패
 */
#include "handler_overlay.h"
#include "modules/network/ovs_overlay.h"
#include "modules/storage/iscsi_manager.h"
#include "modules/dispatcher/rpc_utils.h"
#include "utils/pcv_config.h"
#include "utils/pcv_validate.h"

/* ══════════════════════════════════════════════════════════════
 * OVS 오버레이 핸들러 (overlay.create/delete/list/info/add_peer/remove_peer)
 *
 * OVS (Open vSwitch) 브릿지 + VXLAN 터널을 관리합니다.
 * 3노드 클러스터에서 L2 오버레이 네트워크를 구성하여
 * VM 이 물리 호스트에 관계없이 동일 서브넷에서 통신할 수 있게 합니다.
 *
 * 기본값:
 *   - name: "pcvoverlay0" (미지정 시)
 *   - vni:  100 (VXLAN Network Identifier, 미지정 시)
 * ══════════════════════════════════════════════════════════════ */

/**
 * handle_overlay_create:
 * @params: {"name"?, "vni"?, "cidr"?} — 모두 선택 (기본값 있음)
 *
 * OVS 브릿지 생성 + IP 할당. cidr 지정 시 브릿지에 IP 부여.
 * 실패 시 -32000 에러 (ovs-vsctl 명령 실패 등).
 */
void handle_overlay_create(JsonObject *params, const gchar *rpc_id,
                            UdsServer *server, GSocketConnection *connection)
{
    /* 선택 파라미터: 미지정 시 기본값 사용 */
    const gchar *name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : "pcvoverlay0";
    gint vni = json_object_has_member(params, "vni")
        ? (gint)json_object_get_int_member(params, "vni") : 100;  /* VNI: VXLAN 네트워크 식별자 */
    const gchar *cidr = json_object_has_member(params, "cidr")
        ? json_object_get_string_member(params, "cidr") : NULL;  /* 예: "10.100.0.1/24" */

    /* V12: 브릿지 이름/CIDR 검증 (심층 방어 — ovs_overlay 는 argv 실행이지만
     * 잘못된 입력을 진입 지점에서 거부한다). */
    if (!pcv_validate_bridge_name(name)) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid: name");
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
        return;
    }
    if (cidr && !pcv_validate_cidr(cidr)) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid: cidr");
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
        return;
    }

    GError *err = NULL;
    if (!pcv_overlay_create(name, vni, cidr, &err)) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            err ? err->message : "overlay create failed");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp); if (err) g_error_free(err);
        return;
    }

    /* 성공 응답: 생성된 오버레이 정보 반환 */
    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "created");
    json_object_set_string_member(res, "name", name);
    json_object_set_int_member(res, "vni", vni);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);  /* res 소유권 → node 이전 */
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

/**
 * handle_overlay_delete:
 * @params: {"name"?} — 미지정 시 "pcvoverlay0"
 *
 * OVS 브릿지 삭제 (멱등 — 없어도 성공 반환).
 * pcv_overlay_delete 가 에러를 반환해도 무시 (err 해제만 수행).
 */
void handle_overlay_delete(JsonObject *params, const gchar *rpc_id,
                            UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : "pcvoverlay0";

    GError *err = NULL;
    pcv_overlay_delete(name, &err);
    if (err) g_error_free(err);

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "deleted");
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

/**
 * handle_overlay_list:
 * @params: 사용하지 않음
 *
 * ovs-vsctl list-br 결과를 JSON 배열로 반환.
 * pcv_overlay_list() 가 JsonArray 를 직접 반환 → 소유권 node 에 이전.
 */
void handle_overlay_list(JsonObject *params, const gchar *rpc_id,
                          UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    JsonArray *arr = pcv_overlay_list();
    JsonNode *node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(node, arr);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

/**
 * handle_overlay_info:
 * @params: {"name"?} — 미지정 시 "pcvoverlay0"
 *
 * 특정 오버레이 브릿지의 상세 정보 반환 (포트 목록, VNI, VXLAN 피어 등).
 */
void handle_overlay_info(JsonObject *params, const gchar *rpc_id,
                          UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : "pcvoverlay0";

    JsonObject *info = pcv_overlay_info(name);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, info);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

/**
 * handle_overlay_add_peer:
 * @params: {"name"?, "peer_ip"} — peer_ip 필수 (원격 노드의 VXLAN 터널 IP)
 *
 * VXLAN 터널 포트를 추가하여 원격 노드와 L2 메시를 구성.
 * 예: peer_ip="192.0.2.20" → ovs-vsctl add-port pcvoverlay0 vxlan-192.0.2.20
 */
void handle_overlay_add_peer(JsonObject *params, const gchar *rpc_id,
                              UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : "pcvoverlay0";
    const gchar *peer_ip = json_object_has_member(params, "peer_ip")
        ? json_object_get_string_member(params, "peer_ip") : NULL;

    /* peer_ip 는 필수 — NULL 또는 빈 문자열이면 -32602 에러 */
    if (!peer_ip || !*peer_ip) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing: peer_ip");
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
        return;
    }

    /* V12: 오버레이 이름/피어 IP 검증 (심층 방어) */
    if (!pcv_validate_bridge_name(name) || !pcv_validate_ip_literal(peer_ip)) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid: name or peer_ip");
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
        return;
    }

    GError *err = NULL;
    if (!pcv_overlay_add_peer(name, peer_ip, &err)) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            err ? err->message : "add_peer failed");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp); if (err) g_error_free(err);
        return;
    }

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "added");
    json_object_set_string_member(res, "peer_ip", peer_ip);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

/**
 * handle_overlay_remove_peer:
 * @params: {"name"?, "peer_ip"} — peer_ip 필수
 *
 * VXLAN 터널 포트 제거 (멱등 — 에러 무시).
 */
void handle_overlay_remove_peer(JsonObject *params, const gchar *rpc_id,
                                 UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : "pcvoverlay0";
    const gchar *peer_ip = json_object_has_member(params, "peer_ip")
        ? json_object_get_string_member(params, "peer_ip") : NULL;

    if (!peer_ip) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing: peer_ip");
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
        return;
    }

    pcv_overlay_remove_peer(name, peer_ip, NULL);

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "removed");
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

/* ══════════════════════════════════════════════════════════════
 * iSCSI 핸들러 (iscsi.target.create/delete/list, iscsi.connect/disconnect)
 *
 * ZFS zvol 을 iSCSI 타겟으로 익스포트하여
 * 원격 노드에서 블록 디바이스로 마운트할 수 있게 합니다.
 *
 * 동작 흐름:
 *   target.create → tgtadm --lld iscsi --op new --mode target
 *   connect       → iscsiadm -m discovery + login
 *   disconnect    → iscsiadm -m node --logout
 *
 * 용도: 라이브 마이그레이션 시 공유 스토리지 제공
 * ══════════════════════════════════════════════════════════════ */

/**
 * handle_iscsi_target_create:
 * @params: {"vm_name", "zvol_path"} — 둘 다 필수
 *
 * vm_name 기반으로 IQN 자동 생성, zvol_path 를 백킹 스토어로 등록.
 * 예: zvol_path="/dev/zvol/pcvpool/vms/web-prod"
 */
void handle_iscsi_target_create(JsonObject *params, const gchar *rpc_id,
                                 UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm_name = json_object_has_member(params, "vm_name")
        ? json_object_get_string_member(params, "vm_name") : NULL;
    const gchar *zvol = json_object_has_member(params, "zvol_path")
        ? json_object_get_string_member(params, "zvol_path") : NULL;

    if (!vm_name || !zvol) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing: vm_name or zvol_path");
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
        return;
    }

    GError *err = NULL;
    if (!pcv_iscsi_target_create(vm_name, zvol, &err)) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            err ? err->message : "target create failed");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp); if (err) g_error_free(err);
        return;
    }

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "created");
    json_object_set_string_member(res, "vm_name", vm_name);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

/**
 * handle_iscsi_target_delete:
 * @params: {"vm_name"} — 필수
 *
 * iSCSI 타겟 삭제 (멱등 — 에러 무시).
 */
void handle_iscsi_target_delete(JsonObject *params, const gchar *rpc_id,
                                 UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm_name = json_object_has_member(params, "vm_name")
        ? json_object_get_string_member(params, "vm_name") : NULL;

    if (!vm_name) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing: vm_name");
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
        return;
    }

    pcv_iscsi_target_delete(vm_name, NULL);

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "deleted");
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

/**
 * handle_iscsi_target_list:
 * @params: 사용하지 않음
 *
 * tgtadm --lld iscsi --op show 결과를 JSON 배열로 반환.
 */
void handle_iscsi_target_list(JsonObject *params, const gchar *rpc_id,
                               UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    JsonArray *arr = pcv_iscsi_target_list();
    JsonNode *node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(node, arr);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

/**
 * handle_iscsi_connect:
 * @params: {"target_ip", "vm_name"} — 둘 다 필수
 *
 * 이니시에이터 측에서 원격 iSCSI 타겟에 로그인합니다.
 * 성공 시 device_path 반환 (예: "/dev/sdb").
 *
 * [동작 흐름]
 *   1. iscsiadm -m discovery -t sendtargets -p <target_ip> → 타겟 발견
 *   2. iscsiadm -m node --login → 타겟에 로그인
 *   3. 커널이 SCSI 디바이스 노드 생성 → /dev/sdX 경로 반환
 *
 * [보안] target_ip는 IP 주소 형식이어야 합니다.
 *   pcv_iscsi_initiator_connect()가 iscsiadm에 직접 전달합니다.
 *
 * [참고] dev_path가 NULL이면 디바이스 노드가 아직 나타나지 않은 상태로
 *   "pending"을 반환합니다. 클라이언트가 잠시 후 재시도해야 합니다.
 */
void handle_iscsi_connect(JsonObject *params, const gchar *rpc_id,
                           UdsServer *server, GSocketConnection *connection)
{
    const gchar *target_ip = json_object_has_member(params, "target_ip")
        ? json_object_get_string_member(params, "target_ip") : NULL;
    const gchar *vm_name = json_object_has_member(params, "vm_name")
        ? json_object_get_string_member(params, "vm_name") : NULL;

    if (!target_ip || !vm_name) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing: target_ip or vm_name");
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
        return;
    }

    gchar *dev_path = NULL;
    GError *err = NULL;
    if (!pcv_iscsi_initiator_connect(target_ip, vm_name, &dev_path, &err)) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            err ? err->message : "connect failed");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp); if (err) g_error_free(err);
        return;
    }

    /* dev_path 가 NULL 이면 디바이스 노드가 아직 나타나지 않은 상태 ("pending") */
    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "connected");
    json_object_set_string_member(res, "device_path", dev_path ? dev_path : "pending");
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp); g_free(dev_path);
}

/**
 * handle_iscsi_disconnect:
 * @params: {"target_ip", "vm_name"} — 둘 다 필수
 *
 * iSCSI 세션 로그아웃 (멱등 — 에러 무시).
 */
void handle_iscsi_disconnect(JsonObject *params, const gchar *rpc_id,
                              UdsServer *server, GSocketConnection *connection)
{
    const gchar *target_ip = json_object_has_member(params, "target_ip")
        ? json_object_get_string_member(params, "target_ip") : NULL;
    const gchar *vm_name = json_object_has_member(params, "vm_name")
        ? json_object_get_string_member(params, "vm_name") : NULL;

    if (!target_ip || !vm_name) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing: target_ip or vm_name");
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
        return;
    }

    pcv_iscsi_initiator_disconnect(target_ip, vm_name, NULL);

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "disconnected");
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

/* ══════════════════════════════════════════════════════════════
 * OVN SDN 핸들러 (ovn.switch/router/port/acl/dhcp/nat/tenant/detail)
 *
 * OVN (Open Virtual Network) 은 OVS 위에 구축된 SDN 컨트롤 플레인입니다.
 * 모든 명령은 ovn-nbctl (Northbound DB) CLI 를 통해 실행됩니다.
 *
 * OVN 주요 개념:
 *   - Logical Switch (ls) : 가상 L2 스위치 (VM 포트 연결)
 *   - Logical Router (lr) : 가상 L3 라우터 (서브넷 간 라우팅)
 *   - Logical Switch Port (lsp) : 가상 NIC (MAC/IP 바인딩)
 *   - ACL : 포트 단위 방화벽 규칙 (direction + priority + match + action)
 *   - NAT : SNAT/DNAT 규칙 (라우터에 연결)
 *   - Tenant : 멀티테넌트 격리 (테넌트별 스위치+라우터 자동 생성)
 *
 * 참고: OVN 핸들러들은 짧은 변수명(p,id,s,c,r,n,e,a)을 사용합니다.
 *       이는 한 줄 패턴의 가독성을 위한 의도적 선택입니다.
 * ══════════════════════════════════════════════════════════════ */
#include "modules/network/ovn_manager.h"

/**
 * handle_ovn_switch_create:
 * @p: {"name", "subnet"?} — name 필수, subnet 선택 (예: "10.0.1.0/24")
 *
 * ovn-nbctl ls-add <name> 실행.
 * subnet 지정 시 OVN DHCP 옵션도 함께 설정될 수 있음.
 *
 * [OVN 핸들러 공통 패턴] (이하 모든 OVN 핸들러에 적용)
 *   1. JSON 파라미터 추출 (has_member 체크 + get_string_member)
 *   2. 필수 파라미터 NULL 체크 → 실패 시 -32602 에러 즉시 반환
 *   3. pcv_ovn_*() 코어 함수 호출 (ovn_manager.c → ovn-nbctl CLI 래퍼)
 *   4. GError 발생 시 -32000 에러 반환, 정상 시 성공 JSON 응답
 *
 * [짧은 변수명 규칙] OVN 핸들러는 한 줄 패턴의 가독성을 위해 짧은 변수명을 사용합니다:
 *   p=params, id=rpc_id, s=server, c=connection, r=response, n=node, e=error, a=array
 *
 * [graceful degradation] ovn-nbctl이 미설치된 환경에서는
 *   pcv_ovn_*()가 빈 배열 또는 멱등 성공을 반환합니다.
 */
void handle_ovn_switch_create(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c) {
    const gchar *name = json_object_has_member(p,"name") ? json_object_get_string_member(p,"name") : NULL;
    const gchar *subnet = json_object_has_member(p,"subnet") ? json_object_get_string_member(p,"subnet") : NULL;
    if (!name) { gchar *r=pure_rpc_build_error_response(id,PURE_RPC_ERR_INVALID_PARAMS,"Missing: name"); pure_uds_server_send_response(s,c,r); g_free(r); return; }
    GError *e=NULL; pcv_ovn_switch_create(name,subnet,&e);
    if (e) { gchar *r=pure_rpc_build_error_response(id,PURE_RPC_ERR_ZFS_OPERATION,e->message); pure_uds_server_send_response(s,c,r); g_free(r); g_error_free(e); return; }
    JsonObject *res=json_object_new(); json_object_set_string_member(res,"status","created");
    JsonNode *n=json_node_new(JSON_NODE_OBJECT); json_node_take_object(n,res);
    gchar *r=pure_rpc_build_success_response(id,n); pure_uds_server_send_response(s,c,r); g_free(r);
}

/**
 * ovn.switch.delete — ovn-nbctl ls-del
 * [멱등성] 대상 스위치가 존재하지 않아도 에러 없이 성공 반환합니다.
 * 재시도 안전(retry-safe) — 네트워크 장애 후 재요청해도 동일 결과.
 */
void handle_ovn_switch_delete(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c) {
    const gchar *name = json_object_has_member(p,"name") ? json_object_get_string_member(p,"name") : NULL;
    if (name) pcv_ovn_switch_delete(name,NULL);  /* 에러 무시 (멱등) */
    JsonObject *res=json_object_new(); json_object_set_string_member(res,"status","deleted");
    JsonNode *n=json_node_new(JSON_NODE_OBJECT); json_node_take_object(n,res);
    gchar *r=pure_rpc_build_success_response(id,n); pure_uds_server_send_response(s,c,r); g_free(r);
}

/** ovn.switch.list — ovn-nbctl ls-list 결과를 JSON 배열로 반환 */
void handle_ovn_switch_list(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c) {
    (void)p; JsonArray *a=pcv_ovn_switch_list(); JsonNode *n=json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(n,a); gchar *r=pure_rpc_build_success_response(id,n);
    pure_uds_server_send_response(s,c,r); g_free(r);
}

/**
 * handle_ovn_port_add:
 * @p: {"switch", "port", "mac"?, "ip"?} — switch, port 필수
 *
 * ovn-nbctl lsp-add <switch> <port> 실행.
 * mac/ip 지정 시 lsp-set-addresses 로 정적 바인딩 설정.
 */
void handle_ovn_port_add(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c) {
    const gchar *sw=json_object_has_member(p,"switch")?json_object_get_string_member(p,"switch"):NULL;
    const gchar *port=json_object_has_member(p,"port")?json_object_get_string_member(p,"port"):NULL;
    const gchar *mac=json_object_has_member(p,"mac")?json_object_get_string_member(p,"mac"):NULL;
    const gchar *ip=json_object_has_member(p,"ip")?json_object_get_string_member(p,"ip"):NULL;
    if (!sw||!port) { gchar *r=pure_rpc_build_error_response(id,PURE_RPC_ERR_INVALID_PARAMS,"Missing: switch,port"); pure_uds_server_send_response(s,c,r); g_free(r); return; }
    GError *e=NULL; pcv_ovn_port_add(sw,port,mac,ip,&e);
    if (e) { gchar *r=pure_rpc_build_error_response(id,PURE_RPC_ERR_ZFS_OPERATION,e->message); pure_uds_server_send_response(s,c,r); g_free(r); g_error_free(e); return; }
    JsonObject *res=json_object_new(); json_object_set_string_member(res,"status","added");
    JsonNode *n=json_node_new(JSON_NODE_OBJECT); json_node_take_object(n,res);
    gchar *r=pure_rpc_build_success_response(id,n); pure_uds_server_send_response(s,c,r); g_free(r);
}

/** ovn.port.remove — ovn-nbctl lsp-del (멱등: 에러 무시) */
void handle_ovn_port_remove(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c) {
    const gchar *sw=json_object_has_member(p,"switch")?json_object_get_string_member(p,"switch"):"";
    const gchar *port=json_object_has_member(p,"port")?json_object_get_string_member(p,"port"):NULL;
    if (port) pcv_ovn_port_remove(sw,port,NULL);
    JsonObject *res=json_object_new(); json_object_set_string_member(res,"status","removed");
    JsonNode *n=json_node_new(JSON_NODE_OBJECT); json_node_take_object(n,res);
    gchar *r=pure_rpc_build_success_response(id,n); pure_uds_server_send_response(s,c,r); g_free(r);
}

/**
 * handle_ovn_acl_add:
 * @p: {"switch", "direction"?, "priority"?, "match", "action"?}
 *   - switch  : 대상 논리 스위치 (필수)
 *   - direction: "to-lport" (기본) 또는 "from-lport"
 *   - priority : 0~32767 (기본 1000, 높을수록 우선)
 *   - match   : OVN match 표현식 (필수, 예: "ip4.src==10.0.0.0/24")
 *   - action  : "allow" (기본), "drop", "reject"
 *
 * ovn-nbctl acl-add <switch> <dir> <pri> <match> <action> 실행.
 *
 * [보안 주의사항]
 *   match 표현식은 ovn-nbctl에 직접 전달됩니다.
 *   pcv_ovn_acl_add() 내부에서 ovn-nbctl의 인자로 전달되므로
 *   셸 인젝션은 발생하지 않지만, 잘못된 match 표현식은 OVN에서 거부됩니다.
 */
void handle_ovn_acl_add(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c) {
    const gchar *sw=json_object_has_member(p,"switch")?json_object_get_string_member(p,"switch"):NULL;
    const gchar *dir=json_object_has_member(p,"direction")?json_object_get_string_member(p,"direction"):"to-lport";
    gint pri=json_object_has_member(p,"priority")?(gint)json_object_get_int_member(p,"priority"):1000;
    const gchar *match=json_object_has_member(p,"match")?json_object_get_string_member(p,"match"):NULL;
    const gchar *action=json_object_has_member(p,"action")?json_object_get_string_member(p,"action"):"allow";
    if (!sw||!match) { gchar *r=pure_rpc_build_error_response(id,PURE_RPC_ERR_INVALID_PARAMS,"Missing: switch,match"); pure_uds_server_send_response(s,c,r); g_free(r); return; }
    GError *e=NULL; pcv_ovn_acl_add(sw,dir,pri,match,action,&e);
    if (e) { gchar *r=pure_rpc_build_error_response(id,PURE_RPC_ERR_ZFS_OPERATION,e->message); pure_uds_server_send_response(s,c,r); g_free(r); g_error_free(e); return; }
    JsonObject *res=json_object_new(); json_object_set_string_member(res,"status","added");
    JsonNode *n=json_node_new(JSON_NODE_OBJECT); json_node_take_object(n,res);
    gchar *r=pure_rpc_build_success_response(id,n); pure_uds_server_send_response(s,c,r); g_free(r);
}

/** ovn.acl.list — switch 의 ACL 규칙 목록 (switch=NULL 이면 전체) */
void handle_ovn_acl_list(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c) {
    const gchar *sw=json_object_has_member(p,"switch")?json_object_get_string_member(p,"switch"):NULL;
    JsonArray *a=pcv_ovn_acl_list(sw); JsonNode *n=json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(n,a); gchar *r=pure_rpc_build_success_response(id,n);
    pure_uds_server_send_response(s,c,r); g_free(r);
}

/**
 * handle_ovn_router_create:
 * @p: {"name"} — 필수
 *
 * ovn-nbctl lr-add <name> 실행. 논리 라우터 생성.
 * 라우터에 포트를 추가하려면 ovn.router.add_port 사용.
 */
void handle_ovn_router_create(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c) {
    const gchar *name=json_object_has_member(p,"name")?json_object_get_string_member(p,"name"):NULL;
    if (!name) { gchar *r=pure_rpc_build_error_response(id,PURE_RPC_ERR_INVALID_PARAMS,"Missing: name"); pure_uds_server_send_response(s,c,r); g_free(r); return; }
    GError *e=NULL; pcv_ovn_router_create(name,&e);
    if (e) { gchar *r=pure_rpc_build_error_response(id,PURE_RPC_ERR_ZFS_OPERATION,e->message); pure_uds_server_send_response(s,c,r); g_free(r); g_error_free(e); return; }
    JsonObject *res=json_object_new(); json_object_set_string_member(res,"status","created");
    JsonNode *n=json_node_new(JSON_NODE_OBJECT); json_node_take_object(n,res);
    gchar *r=pure_rpc_build_success_response(id,n); pure_uds_server_send_response(s,c,r); g_free(r);
}

/** ovn.status — OVN 컨트롤러/Northbound DB 상태 조회 */
void handle_ovn_status(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c) {
    (void)p; JsonObject *st=pcv_ovn_status(); JsonNode *n=json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n,st); gchar *r=pure_rpc_build_success_response(id,n);
    pure_uds_server_send_response(s,c,r); g_free(r);
}

/** ovn.router.delete — ovn-nbctl lr-del (에러 시 -32000) */
void handle_ovn_router_delete(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c) {
    const gchar *name=json_object_has_member(p,"name")?json_object_get_string_member(p,"name"):NULL;
    if (!name) { gchar *r=pure_rpc_build_error_response(id,PURE_RPC_ERR_INVALID_PARAMS,"Missing: name"); pure_uds_server_send_response(s,c,r); g_free(r); return; }
    GError *e=NULL; pcv_ovn_router_delete(name,&e);
    if (e) { gchar *r=pure_rpc_build_error_response(id,PURE_RPC_ERR_ZFS_OPERATION,e->message); pure_uds_server_send_response(s,c,r); g_free(r); g_error_free(e); return; }
    JsonObject *res=json_object_new(); json_object_set_string_member(res,"status","deleted");
    json_object_set_string_member(res,"name",name);
    JsonNode *n=json_node_new(JSON_NODE_OBJECT); json_node_take_object(n,res);
    gchar *r=pure_rpc_build_success_response(id,n); pure_uds_server_send_response(s,c,r); g_free(r);
}

/** ovn.router.list — ovn-nbctl lr-list 결과를 JSON 배열로 반환 */
void handle_ovn_router_list(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c) {
    (void)p; JsonArray *a=pcv_ovn_router_list(); JsonNode *n=json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(n,a); gchar *r=pure_rpc_build_success_response(id,n);
    pure_uds_server_send_response(s,c,r); g_free(r);
}

/**
 * handle_ovn_router_add_port:
 * @p: {"router", "switch", "mac", "cidr"} — 모두 필수
 *
 * 라우터 ↔ 스위치 연결:
 *   1. ovn-nbctl lrp-add <router> <port_name> <mac> <cidr>
 *   2. ovn-nbctl lsp-add <switch> <peer_port>
 *   3. ovn-nbctl lsp-set-type <peer_port> router
 *
 * 예: router="r1", switch="ls1", mac="02:ac:10:ff:00:01", cidr="10.0.1.1/24"
 */
void handle_ovn_router_add_port(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c) {
    const gchar *router=json_object_has_member(p,"router")?json_object_get_string_member(p,"router"):NULL;
    const gchar *sw=json_object_has_member(p,"switch")?json_object_get_string_member(p,"switch"):NULL;
    const gchar *mac=json_object_has_member(p,"mac")?json_object_get_string_member(p,"mac"):NULL;
    const gchar *cidr=json_object_has_member(p,"cidr")?json_object_get_string_member(p,"cidr"):NULL;
    if (!router||!sw||!mac||!cidr) { gchar *r=pure_rpc_build_error_response(id,PURE_RPC_ERR_INVALID_PARAMS,"Missing: router,switch,mac,cidr"); pure_uds_server_send_response(s,c,r); g_free(r); return; }
    GError *e=NULL; pcv_ovn_router_add_port(router,sw,mac,cidr,&e);
    if (e) { gchar *r=pure_rpc_build_error_response(id,PURE_RPC_ERR_ZFS_OPERATION,e->message); pure_uds_server_send_response(s,c,r); g_free(r); g_error_free(e); return; }
    JsonObject *res=json_object_new(); json_object_set_string_member(res,"status","connected");
    json_object_set_string_member(res,"router",router); json_object_set_string_member(res,"switch",sw);
    JsonNode *n=json_node_new(JSON_NODE_OBJECT); json_node_take_object(n,res);
    gchar *r=pure_rpc_build_success_response(id,n); pure_uds_server_send_response(s,c,r); g_free(r);
}

/**
 * handle_ovn_dhcp_enable:
 * @p: {"subnet", "gateway"} — 둘 다 필수
 *
 * OVN 분산 DHCP 설정: 지정 서브넷에 DHCP 옵션 생성.
 * ovn-nbctl dhcp-options-create + set 으로 게이트웨이/DNS 등 설정.
 * 예: subnet="10.0.1.0/24", gateway="10.0.1.1"
 */
void handle_ovn_dhcp_enable(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c) {
    const gchar *subnet=json_object_has_member(p,"subnet")?json_object_get_string_member(p,"subnet"):NULL;
    const gchar *gw=json_object_has_member(p,"gateway")?json_object_get_string_member(p,"gateway"):NULL;
    if (!subnet||!gw) { gchar *r=pure_rpc_build_error_response(id,PURE_RPC_ERR_INVALID_PARAMS,"Missing: subnet,gateway"); pure_uds_server_send_response(s,c,r); g_free(r); return; }
    GError *e=NULL; pcv_ovn_dhcp_enable(subnet,gw,&e);
    if (e) { gchar *r=pure_rpc_build_error_response(id,PURE_RPC_ERR_ZFS_OPERATION,e->message); pure_uds_server_send_response(s,c,r); g_free(r); g_error_free(e); return; }
    JsonObject *res=json_object_new(); json_object_set_string_member(res,"status","enabled");
    json_object_set_string_member(res,"subnet",subnet);
    JsonNode *n=json_node_new(JSON_NODE_OBJECT); json_node_take_object(n,res);
    gchar *r=pure_rpc_build_success_response(id,n); pure_uds_server_send_response(s,c,r); g_free(r);
}

/**
 * handle_ovn_nat_add:
 * @p: {"router", "type", "external_ip", "logical_ip"} — 모두 필수
 *
 * OVN NAT 규칙 추가:
 *   type: "snat" (소스 NAT — 내부→외부 트래픽의 소스 IP 변환)
 *         "dnat_and_snat" (1:1 NAT — 양방향 IP 매핑)
 *   ovn-nbctl lr-nat-add <router> <type> <external_ip> <logical_ip>
 *
 * [보안] external_ip, logical_ip는 IP 주소 형식이어야 합니다.
 *   잘못된 형식은 ovn-nbctl이 거부합니다.
 */
void handle_ovn_nat_add(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c) {
    const gchar *router=json_object_has_member(p,"router")?json_object_get_string_member(p,"router"):NULL;
    const gchar *type=json_object_has_member(p,"type")?json_object_get_string_member(p,"type"):NULL;
    const gchar *ext_ip=json_object_has_member(p,"external_ip")?json_object_get_string_member(p,"external_ip"):NULL;
    const gchar *log_ip=json_object_has_member(p,"logical_ip")?json_object_get_string_member(p,"logical_ip"):NULL;
    if (!router||!type||!ext_ip||!log_ip) { gchar *r=pure_rpc_build_error_response(id,PURE_RPC_ERR_INVALID_PARAMS,"Missing: router,type,external_ip,logical_ip"); pure_uds_server_send_response(s,c,r); g_free(r); return; }
    GError *e=NULL; pcv_ovn_nat_add(router,type,ext_ip,log_ip,&e);
    if (e) { gchar *r=pure_rpc_build_error_response(id,PURE_RPC_ERR_ZFS_OPERATION,e->message); pure_uds_server_send_response(s,c,r); g_free(r); g_error_free(e); return; }
    JsonObject *res=json_object_new(); json_object_set_string_member(res,"status","added");
    json_object_set_string_member(res,"router",router); json_object_set_string_member(res,"type",type);
    JsonNode *n=json_node_new(JSON_NODE_OBJECT); json_node_take_object(n,res);
    gchar *r=pure_rpc_build_success_response(id,n); pure_uds_server_send_response(s,c,r); g_free(r);
}

/** ovn.nat.list — 라우터의 NAT 규칙 목록 반환 (router 필수) */
void handle_ovn_nat_list(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c) {
    const gchar *router=json_object_has_member(p,"router")?json_object_get_string_member(p,"router"):NULL;
    if (!router) { gchar *r=pure_rpc_build_error_response(id,PURE_RPC_ERR_INVALID_PARAMS,"Missing: router"); pure_uds_server_send_response(s,c,r); g_free(r); return; }
    JsonArray *a=pcv_ovn_nat_list(router); JsonNode *n=json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(n,a); gchar *r=pure_rpc_build_success_response(id,n);
    pure_uds_server_send_response(s,c,r); g_free(r);
}

/**
 * handle_ovn_tenant_create:
 * @p: {"tenant", "subnet"} — 둘 다 필수
 *
 * [멀티테넌트 원클릭 프로비저닝]
 * 하나의 RPC 호출로 테넌트 전용 네트워크 인프라를 일괄 생성합니다:
 *   1. 논리 스위치 "tenant-<name>-ls" 자동 생성 (테넌트 격리)
 *   2. 서브넷 DHCP 설정 (테넌트 전용 IP 대역)
 *   3. (향후) 전용 라우터 + ACL 격리 추가 예정
 *
 * 이 복합 RPC는 pcv_ovn_tenant_create() 내부에서 여러 ovn-nbctl 명령을
 * 순차적으로 실행합니다. 중간 단계 실패 시 GError로 보고됩니다.
 *
 * [테넌트 이름 규칙] 스위치 이름 = "tenant-<name>-ls"
 *   이 네이밍 규칙으로 테넌트별 리소스를 식별합니다.
 *
 * 반환: {"status":"created", "tenant":"...", "switch":"tenant-<name>-ls"}
 */
void handle_ovn_tenant_create(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c) {
    const gchar *tenant=json_object_has_member(p,"tenant")?json_object_get_string_member(p,"tenant"):NULL;
    const gchar *subnet=json_object_has_member(p,"subnet")?json_object_get_string_member(p,"subnet"):NULL;
    if (!tenant||!subnet) { gchar *r=pure_rpc_build_error_response(id,PURE_RPC_ERR_INVALID_PARAMS,"Missing: tenant,subnet"); pure_uds_server_send_response(s,c,r); g_free(r); return; }
    GError *e=NULL; pcv_ovn_tenant_create(tenant,subnet,&e);
    if (e) { gchar *r=pure_rpc_build_error_response(id,PURE_RPC_ERR_ZFS_OPERATION,e->message); pure_uds_server_send_response(s,c,r); g_free(r); g_error_free(e); return; }
    JsonObject *res=json_object_new(); json_object_set_string_member(res,"status","created");
    json_object_set_string_member(res,"tenant",tenant);
    /* 테넌트 전용 스위치 이름 규칙: "tenant-<name>-ls" */
    gchar *sw_name=g_strdup_printf("tenant-%s-ls",tenant);
    json_object_set_string_member(res,"switch",sw_name); g_free(sw_name);
    JsonNode *n=json_node_new(JSON_NODE_OBJECT); json_node_take_object(n,res);
    gchar *r=pure_rpc_build_success_response(id,n); pure_uds_server_send_response(s,c,r); g_free(r);
}

/** ovn.switch.detail — 스위치 상세 (포트 목록, ACL, DHCP 옵션 등) */
void handle_ovn_switch_detail(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c) {
    const gchar *name=json_object_has_member(p,"name")?json_object_get_string_member(p,"name"):NULL;
    if (!name) { gchar *r=pure_rpc_build_error_response(id,PURE_RPC_ERR_INVALID_PARAMS,"Missing: name"); pure_uds_server_send_response(s,c,r); g_free(r); return; }
    JsonObject *detail=pcv_ovn_switch_detail(name);
    JsonNode *n=json_node_new(JSON_NODE_OBJECT); json_node_take_object(n,detail);
    gchar *r=pure_rpc_build_success_response(id,n); pure_uds_server_send_response(s,c,r); g_free(r);
}

/** ovn.router.detail — 라우터 상세 (포트, NAT 규칙, 라우팅 테이블 등) */
void handle_ovn_router_detail(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c) {
    const gchar *name=json_object_has_member(p,"name")?json_object_get_string_member(p,"name"):NULL;
    if (!name) { gchar *r=pure_rpc_build_error_response(id,PURE_RPC_ERR_INVALID_PARAMS,"Missing: name"); pure_uds_server_send_response(s,c,r); g_free(r); return; }
    JsonObject *detail=pcv_ovn_router_detail(name);
    JsonNode *n=json_node_new(JSON_NODE_OBJECT); json_node_take_object(n,detail);
    gchar *r=pure_rpc_build_success_response(id,n); pure_uds_server_send_response(s,c,r); g_free(r);
}
