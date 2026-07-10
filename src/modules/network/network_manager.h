/**
 * @file network_manager.h
 * @brief 네트워크 매니저 공개 인터페이스 -- Linux 브릿지 CRUD + OVS 드라이버
 *
 * ====================================================================
 * [역할]
 *   PureCVisor 네트워크 서브시스템의 최상위 진입점 헤더.
 *   dispatcher.c 에서 network.* RPC 10개를 이 헤더의 핸들러로 라우팅한다.
 *   rest_server.c 의 /api/v1/networks 엔드포인트도 동일 핸들러를 호출.
 *
 * [핸들러 시그니처 규칙]
 *   모든 핸들러는 동일한 4-파라미터 시그니처를 따른다:
 *     (JsonObject *params, const gchar *rpc_id,
 *      UdsServer *server, GSocketConnection *connection)
 *
 *   fire-and-forget 패턴:
 *     network.create/delete 는 GTask 비동기 워커를 사용한다.
 *     콜백(network_action_callback)에서 최종 응답을 전송한다.
 *     나머지 핸들러(list/info/mode_set 등)는 동기 응답이다.
 *
 * [내부 유틸리티]
 *   network_bridge_create / delete 는 `ip` 명령 래퍼로,
 *   핸들러 내부 + 테스트에서 직접 호출 가능하다.
 *   이 함수들은 JSON-RPC 레이어를 모르므로 단위 테스트에 적합.
 *
 * [모듈 의존 관계 (하위 호출)]
 *   network_manager.c
 *     |-- network_firewall.c  -- nftables 방화벽 규칙 (모드별 설정/해제)
 *     |-- network_dhcp.c      -- dnsmasq DHCP 서버 시작/중단
 *     |-- pcv_validate.h      -- bridge_name, CIDR, 모드 입력 검증
 *     |-- pcv_spawn.h         -- 외부 프로세스 실행 (ip, ovs-vsctl 등)
 *     +-- rpc_utils.h         -- JSON-RPC 응답 빌더
 *
 * [OVS 핸들러]
 *   Phase T-1 에서 추가된 OVS 전용 핸들러 4개:
 *   ovs.create, ovs.delete, ovs.vxlan.add, ovs.vxlan.del
 *   내부적으로 ovs-vsctl 명령을 사용한다.
 *   이 핸들러들은 Linux Bridge 가 아닌 Open vSwitch 브릿지를 관리하며,
 *   VXLAN 터널을 통한 3노드 오버레이 네트워크에 사용된다.
 *
 * [브릿지 모드 (4가지)]
 *   nat      -- 기본값. nftables MASQUERADE + DHCP. VM이 인터넷 접근 가능.
 *   isolated -- 브릿지 내부 통신만 허용. 외부 완전 차단. DHCP 제공.
 *   routed   -- ip_forward만 활성화. NAT 없음. 정적 라우팅 환경용.
 *   bridge   -- 물리 NIC을 슬레이브로 연결. IP/DHCP 없음. L2 브릿징.
 *
 * [PCV_NETWORK_RUNDIR]
 *   dnsmasq 관련 파일 경로의 기준 디렉토리.
 *   network_dhcp.h 에서 "/var/run/purecvisor" 로 정의.
 *   .conf, .pid, .leases, .meta 파일이 이 디렉토리에 생성/삭제된다.
 * ====================================================================
 */
#ifndef PURECVISOR_NETWORK_MANAGER_H
#define PURECVISOR_NETWORK_MANAGER_H

#include <glib.h>
#include <json-glib/json-glib.h>
#include "api/uds_server.h"

G_BEGIN_DECLS

/* ----------------------------------------------------------------
 * JSON-RPC 디스패처 진입점 -- dispatcher.c else-if 체인에서 호출
 *
 * [주니어 참고 -- 호출 흐름]
 *   1. 클라이언트가 pcvctl/REST/TUI로 "network.create" 요청 전송
 *   2. dispatcher.c 가 메서드명 매칭 -> handle_network_create_request() 호출
 *   3. 핸들러가 파라미터 검증 -> GTask 비동기 실행 (또는 동기 응답)
 *   4. 최종 JSON-RPC 응답이 소켓으로 전송됨
 * ---------------------------------------------------------------- */

/* network.create -- 브릿지 생성 (비동기, 모드별 방화벽/DHCP 자동 설정) */
void handle_network_create_request  (JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/* network.delete -- 브릿지 삭제 (비동기, 멱등, dnsmasq 정리 포함) */
void handle_network_delete_request  (JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/* Sprint F: 네트워크 목록 + 상세 조회 (동기 응답) */
/* network.list -- 시스템 전체 브릿지 열거 (Linux Bridge + OVS + LXC + Docker) */
void handle_network_list_request    (JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/* network.info -- 단일 브릿지 상세 정보 (모드, CIDR, 슬레이브, DHCP 상태 등) */
void handle_network_info_request    (JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/* Sprint G: 네트워크 모드 변경 (nat | isolated | routed)
 * 기존 nftables 규칙을 teardown 후 새 모드 규칙을 적용한다. */
void handle_network_mode_set_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/* ----------------------------------------------------------------
 * 내부 Bridge 제어 유틸리티
 *
 * JSON-RPC 레이어와 무관한 순수 브릿지 제어 함수.
 * 핸들러 내부에서 호출되며, 단위 테스트(test_network.c)에서도 직접 사용.
 * ---------------------------------------------------------------- */

/**
 * network_bridge_create -- Linux Bridge 생성 + IP 할당 + MTU 설정 + 활성화
 * @bridge_name: 생성할 브릿지 이름 (예: "pcvbr0")
 * @cidr: 게이트웨이 IP/CIDR (예: "10.10.10.1/24" 또는 "fd00::1/64"), bridge 모드일 경우 NULL
 * @mtu: MTU 값 (0이면 기본 1500 적용)
 * @error: 실패 시 GError 설정 (호출자가 g_error_free)
 * @return 성공 시 TRUE
 */
gboolean network_bridge_create(const gchar *bridge_name, const gchar *cidr, gint mtu, GError **error);

/**
 * pcv_network_meta_save -- 브릿지 meta(mode/cidr) 영속화 export (VP-5 잔여)
 * network create 밖의 생성 경로(bootstrap 기본 네트워크)용.
 */
void pcv_network_meta_save(const gchar *bridge_name, const gchar *mode, const gchar *cidr);

/**
 * network_bridge_delete -- Linux Bridge 삭제 (멱등)
 * @bridge_name: 삭제할 브릿지 이름
 * @error: 실패 시 GError 설정 (호출자가 g_error_free)
 *
 * dnsmasq 종료, 슬레이브 NIC 해제, 파일 정리를 모두 처리한다.
 * 이미 삭제된 브릿지에 대해서도 TRUE 반환 (멱등).
 * @return 성공 시 TRUE
 */
gboolean network_bridge_delete(const gchar *bridge_name, GError **error);

/* ----------------------------------------------------------------
 * [P0-Fix#4] 런타임 네트워크 제어 핸들러
 * ---------------------------------------------------------------- */

/* network.bind_phys -- 물리 NIC을 브릿지의 업링크(슬레이브)로 연결
 * params: { "bridge": "pcvbr0", "iface": "eno1" }
 * 물리 네트워크에 직접 참여하는 bridge 모드에서 사용.
 * [주의] 관리 NIC을 바인딩하면 SSH 접속이 끊어질 수 있다. */
void handle_network_bind_phys_request  (JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/* network.dhcp_toggle -- DHCP(dnsmasq) 시작/중단 토글
 * params: { "bridge": "pcvbr0", "enable": true|false }
 * enable=true:  메타 파일에서 CIDR 읽어 dnsmasq 기동
 * enable=false: dnsmasq PID 파일로 프로세스 종료 */
void handle_network_dhcp_toggle_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/* ----------------------------------------------------------------
 * Phase T-1: OVS(Open vSwitch) 드라이버
 *
 * [OVS란?]
 *   소프트웨어 정의 가상 스위치. Linux Bridge보다 고급 기능 제공:
 *   VXLAN/GRE/Geneve 터널, OpenFlow, QoS, 미러링, sFlow 등.
 *   PureCVisor에서는 3노드 VXLAN 오버레이(pcvoverlay0)에 사용.
 *
 * [VXLAN이란?]
 *   L2 프레임을 UDP(포트 4789)로 캡슐화하는 터널링 프로토콜.
 *   VNI(VXLAN Network Identifier)로 최대 16M개의 가상 네트워크 분리.
 *   3노드 풀 메시 터널로 VM 마이그레이션 후 IP 연속성 보장.
 *
 * [멱등 명령어]
 *   --may-exist (add-br) : 이미 존재하면 에러 없이 성공
 *   --if-exists (del-br/del-port) : 없으면 에러 없이 성공
 * ---------------------------------------------------------------- */

/* network.ovs.create -- OVS 브릿지 생성 (ovs-vsctl --may-exist add-br) */
void handle_network_ovs_create_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/* network.ovs.delete -- OVS 브릿지 삭제 (ovs-vsctl --if-exists del-br, 멱등) */
void handle_network_ovs_delete_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/* network.ovs.vxlan.add -- OVS 브릿지에 VXLAN 터널 포트 추가
 * params: { "bridge", "port_name", "remote_ip", "vni" } */
void handle_network_ovs_vxlan_add_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/* network.ovs.vxlan.del -- OVS 브릿지에서 VXLAN 터널 포트 삭제 (멱등)
 * params: { "bridge", "port_name" } */
void handle_network_ovs_vxlan_del_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/* ----------------------------------------------------------------
 * QoS (Traffic Control) 핸들러 — tc HTB 기반 대역폭 제한
 *
 * [tc란?]
 *   Linux Traffic Control. 네트워크 인터페이스에 qdisc(queueing discipline)를
 *   설정하여 대역폭, 지연, 패킷 손실 등을 제어한다.
 *   HTB(Hierarchical Token Bucket)는 계층적 대역폭 할당에 적합.
 *
 * [사용 시나리오]
 *   VM vnet 인터페이스에 대역폭 제한을 걸어 QoS 정책 적용.
 *   브릿지 인터페이스에 적용하면 전체 트래픽 제한 가능.
 * ---------------------------------------------------------------- */

/* network.qos.set -- HTB qdisc + class 설정 (멱등, replace 사용)
 * params: { "interface": "vnet0", "rate_mbps": 100, "burst_kb": 256 } */
void handle_network_qos_set(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/* network.qos.get -- tc 통계 조회
 * params: { "interface": "vnet0" } */
void handle_network_qos_get(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/* network.qos.remove -- tc qdisc 제거 (멱등)
 * params: { "interface": "vnet0" } */
void handle_network_qos_remove(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/* BE-5: QoS 규칙 영속화 — 데몬 시작 시 tc 규칙 복원 */
void pcv_qos_restore(void);

/* BE-A19: Bridge VLAN Filtering */
gboolean pcv_bridge_vlan_add(const gchar *bridge, const gchar *iface, gint vlan_id);
gboolean pcv_bridge_vlan_remove(const gchar *bridge, const gchar *iface, gint vlan_id);

G_END_DECLS

#endif /* PURECVISOR_NETWORK_MANAGER_H */
