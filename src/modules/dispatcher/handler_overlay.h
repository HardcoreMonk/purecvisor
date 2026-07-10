/**
 * @file handler_overlay.h
 * @brief 네트워크 인프라 RPC 핸들러 공개 인터페이스 (OVS + iSCSI + OVN)
 *
 * ──────────────────────────────────────────────────────────────
 * [아키텍처 내 위치]
 *   dispatcher.c → 이 파일의 핸들러 → ovs_overlay.c / iscsi_manager.c / ovn_manager.c
 *   가상 네트워크 오버레이(OVS VXLAN), 공유 스토리지(iSCSI), SDN(OVN)을 관리.
 *
 * [RPC 메서드 매핑] (29개)
 *
 *   --- OVS 오버레이 (6개) ---
 *   overlay.create      -> handle_overlay_create      : OVS 브릿지 + VXLAN 터널 생성
 *   overlay.delete      -> handle_overlay_delete      : OVS 브릿지 삭제
 *   overlay.list        -> handle_overlay_list        : 오버레이 브릿지 목록 조회
 *   overlay.info        -> handle_overlay_info        : 특정 브릿지 상세 정보
 *   overlay.add_peer    -> handle_overlay_add_peer    : VXLAN 피어 노드 추가
 *   overlay.remove_peer -> handle_overlay_remove_peer : VXLAN 피어 노드 제거
 *
 *   --- iSCSI (5개) ---
 *   iscsi.target.create -> handle_iscsi_target_create : iSCSI 타겟 생성 (tgtadm)
 *   iscsi.target.delete -> handle_iscsi_target_delete : iSCSI 타겟 삭제
 *   iscsi.target.list   -> handle_iscsi_target_list   : 타겟 목록 조회
 *   iscsi.connect       -> handle_iscsi_connect       : 이니시에이터 연결 (iscsiadm)
 *   iscsi.disconnect    -> handle_iscsi_disconnect    : 이니시에이터 해제
 *
 *   --- OVN SDN (18개) ---
 *   ovn.switch.create   -> handle_ovn_switch_create   : 논리 스위치 생성 (ovn-nbctl ls-add)
 *   ovn.switch.delete   -> handle_ovn_switch_delete   : 논리 스위치 삭제
 *   ovn.switch.list     -> handle_ovn_switch_list     : 논리 스위치 목록
 *   ovn.switch.detail   -> handle_ovn_switch_detail   : 논리 스위치 상세 (포트/ACL 포함)
 *   ovn.port.add        -> handle_ovn_port_add        : 논리 포트 추가 (lsp-add)
 *   ovn.port.remove     -> handle_ovn_port_remove     : 논리 포트 제거
 *   ovn.acl.add         -> handle_ovn_acl_add         : ACL 규칙 추가 (방화벽)
 *   ovn.acl.list        -> handle_ovn_acl_list        : ACL 규칙 목록
 *   ovn.router.create   -> handle_ovn_router_create   : 논리 라우터 생성 (lr-add)
 *   ovn.router.delete   -> handle_ovn_router_delete   : 논리 라우터 삭제
 *   ovn.router.list     -> handle_ovn_router_list     : 논리 라우터 목록
 *   ovn.router.detail   -> handle_ovn_router_detail   : 논리 라우터 상세 (포트/NAT 포함)
 *   ovn.router.add_port -> handle_ovn_router_add_port : 라우터-스위치 포트 연결
 *   ovn.dhcp.enable     -> handle_ovn_dhcp_enable     : 분산 DHCP 활성화
 *   ovn.nat.add         -> handle_ovn_nat_add         : NAT 규칙 추가 (SNAT/DNAT)
 *   ovn.nat.list        -> handle_ovn_nat_list        : NAT 규칙 목록
 *   ovn.tenant.create   -> handle_ovn_tenant_create   : 멀티테넌트 환경 일괄 생성
 *   ovn.status          -> handle_ovn_status          : OVN 클러스터 상태 조회
 *
 * [모든 핸들러 동기 응답 — fire-and-forget 미사용]
 *   모든 핸들러가 외부 명령(ovs-vsctl, tgtadm, ovn-nbctl) 결과를
 *   기다린 후 동기적으로 응답한다.
 *
 * [핸들러 시그니처] (모든 디스패처 핸들러 공통)
 *   void handler(JsonObject *params, const gchar *rpc_id,
 *                UdsServer *server, GSocketConnection *connection)
 * ──────────────────────────────────────────────────────────────
 */
#ifndef PURECVISOR_HANDLER_OVERLAY_H
#define PURECVISOR_HANDLER_OVERLAY_H

#include <json-glib/json-glib.h>
#include <gio/gio.h>
#include "api/uds_server.h"

G_BEGIN_DECLS

/* ================================================================
 * OVS 오버레이 RPC 핸들러 (6개)
 * - ovs-vsctl 명령을 통해 OVS 브릿지 및 VXLAN 터널을 관리
 * - pcvoverlay0 브릿지가 기본 오버레이 (VNI=100, MTU=1450)
 * ================================================================ */

/** params: {"bridge":"pcvoverlay0", "vni":100, "local_ip":"x.x.x.x"} */
void handle_overlay_create(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/** params: {"bridge":"pcvoverlay0"} — 멱등: 존재하지 않으면 성공 반환 */
void handle_overlay_delete(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/** params: {} — 모든 OVS 브릿지 목록 반환 */
void handle_overlay_list(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/** params: {"bridge":"pcvoverlay0"} — 포트/VXLAN 터널 상세 정보 */
void handle_overlay_info(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/** params: {"bridge":"pcvoverlay0", "remote_ip":"x.x.x.x", "vni":100} */
void handle_overlay_add_peer(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/** params: {"bridge":"pcvoverlay0", "remote_ip":"x.x.x.x"} */
void handle_overlay_remove_peer(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/* ================================================================
 * iSCSI RPC 핸들러 (5개)
 * - tgtadm/iscsiadm 명령을 통해 iSCSI 타겟/이니시에이터를 관리
 * - 3노드 클러스터 간 공유 스토리지 제공
 * ================================================================ */

/** params: {"target_name":"iqn.2024.xxx", "zvol":"/dev/zvol/pcvpool/vms/xxx"} */
void handle_iscsi_target_create(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/** params: {"target_name":"iqn.2024.xxx"} — 멱등 삭제 */
void handle_iscsi_target_delete(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/** params: {} — 모든 iSCSI 타겟 목록 */
void handle_iscsi_target_list(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/** params: {"target_ip":"x.x.x.x", "target_name":"iqn.2024.xxx"} */
void handle_iscsi_connect(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/** params: {"target_ip":"x.x.x.x", "target_name":"iqn.2024.xxx"} */
void handle_iscsi_disconnect(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/* ================================================================
 * OVN SDN RPC 핸들러 (18개)
 * - ovn-nbctl 명령을 통해 OVN Northbound DB를 조작
 * - Geneve 터널 기반 소프트웨어 정의 네트워킹
 * - graceful degradation: OVN 미설치 시 빈 배열/멱등 삭제 반환
 * ================================================================ */

/** params: {"name":"ls-web"} — 논리 스위치 생성 */
void handle_ovn_switch_create(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);
/** params: {"name":"ls-web"} — 논리 스위치 삭제 (멱등) */
void handle_ovn_switch_delete(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);
/** params: {} — 전체 논리 스위치 목록 */
void handle_ovn_switch_list(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);
/** params: {"switch":"ls-web"} — 논리 스위치 상세 (포트, ACL 포함) */
void handle_ovn_switch_detail(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);
/** params: {"switch":"ls-web", "port":"lsp-vm1", "mac":"xx:xx:xx:xx:xx:xx", "ip":"10.0.0.2"} */
void handle_ovn_port_add(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);
/** params: {"port":"lsp-vm1"} — 논리 포트 제거 */
void handle_ovn_port_remove(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);
/** params: {"switch":"ls-web", "direction":"from-lport", "priority":1000, "match":"...", "action":"allow"} */
void handle_ovn_acl_add(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);
/** params: {"switch":"ls-web"} — 해당 스위치의 ACL 규칙 목록 */
void handle_ovn_acl_list(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);
/** params: {"name":"lr-main"} — 논리 라우터 생성 */
void handle_ovn_router_create(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);
/** params: {"name":"lr-main"} — 논리 라우터 삭제 (멱등) */
void handle_ovn_router_delete(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);
/** params: {} — 전체 논리 라우터 목록 */
void handle_ovn_router_list(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);
/** params: {"router":"lr-main"} — 논리 라우터 상세 (포트, NAT 포함) */
void handle_ovn_router_detail(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);
/** params: {"router":"lr-main", "switch":"ls-web", "router_port":"lrp-web", "mac":"...", "network":"10.0.0.1/24"} */
void handle_ovn_router_add_port(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);
/** params: {"switch":"ls-web", "cidr":"10.0.0.0/24", "server_mac":"...", "server_ip":"10.0.0.1"} — OVN 분산 DHCP */
void handle_ovn_dhcp_enable(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);
/** params: {"router":"lr-main", "type":"snat|dnat_and_snat", "external_ip":"...", "logical_ip":"..."} */
void handle_ovn_nat_add(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);
/** params: {"router":"lr-main"} — NAT 규칙 목록 */
void handle_ovn_nat_list(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);
/** params: {"tenant":"prod", "cidr":"10.1.0.0/24"} — 멀티테넌트 환경 일괄 생성 (스위치+ACL+DHCP) */
void handle_ovn_tenant_create(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);
/** params: {} — OVN 클러스터 전체 상태 (NB/SB 연결, encap 설정 등) */
void handle_ovn_status(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);

G_END_DECLS

#endif /* PURECVISOR_HANDLER_OVERLAY_H */
