/**
 * @file ovn_manager.h
 * @brief OVN SDN 컨트롤 플레인 공개 인터페이스
 *
 * ====================================================================
 * [역할]
 *   handler_overlay.c 에서 ovn.* RPC 16개 처리 시 호출.
 *   main.c 에서 부팅 시 init 호출.
 *   vm_manager.c 에서 VM 생성/삭제 시 port setup/cleanup 호출.
 *
 * [OVN 기본 개념 (주니어 참고)]
 *   OVN(Open Virtual Network)은 OVS 위에 구축된 SDN(Software Defined
 *   Networking) 컨트롤 플레인이다.
 *
 *   [OVS vs OVN]
 *     OVS : 데이터 플레인 (패킷 스위칭)
 *     OVN : 컨트롤 플레인 (논리적 네트워크 정의, ACL, 분산 DHCP, NAT)
 *
 *   [핵심 구성 요소]
 *     Northbound DB (NB) : 관리자가 정의하는 논리적 네트워크 (스위치/라우터/ACL)
 *     Southbound DB (SB) : 물리적 바인딩 (어떤 포트가 어떤 hypervisor에 있는지)
 *     ovn-northd          : NB -> SB 변환 데몬
 *     ovn-controller       : 각 hypervisor에서 실행, SB를 로컬 OVS flow로 변환
 *
 *   [Geneve 터널]
 *     OVN은 VXLAN 대신 Geneve 터널을 기본 사용한다.
 *     Geneve는 가변 길이 메타데이터를 지원하여 OVN의 풍부한 기능에 적합.
 *
 *   [분산 DHCP]
 *     OVN의 DHCP는 중앙 dnsmasq가 아니라 각 노드의 ovn-controller가
 *     로컬에서 DHCP 응답을 생성한다. 별도 데몬 프로세스 불필요.
 *
 *   [논리 라우터]
 *     OVN 논리 라우터는 분산 라우터로, 각 hypervisor에서 로컬로 동작.
 *     중앙 집중식 라우터의 병목 없이 스케일 아웃 가능.
 *
 * [함수 분류]
 *   Lifecycle     : init, shutdown, is_available
 *   Phase 1 스위치: switch_create/delete/list, port_add/remove
 *   Phase 2 ACL   : acl_add/delete/list, dhcp_enable/list
 *   Phase 3 라우터: router_create/delete/list, router_add/remove_port
 *   NAT           : nat_add/delete/list
 *   프로비저닝    : setup_encap, auto_provision (클러스터 빌드 전용)
 *   멀티테넌트    : tenant_create/delete
 *   VM 포트       : vm_port_setup/cleanup
 *   상세 조회     : switch_detail, router_detail (REST API용)
 *   상태          : status
 *
 * [에디션 경계]
 *   Single Edge: 로컬 OVN 코어 기능 (switch/router/ACL/NAT/DHCP/tenant/vm_port)
 *   Cluster build : 위 공용 코어 + encap/auto_provision 자동화
 *
 * [Graceful Degradation]
 *   pcv_ovn_is_available()==FALSE 이면 list 는 빈 배열,
 *   변경 작업은 에러를 반환한다. ovn-nbctl 미설치 환경에서도
 *   데몬이 정상 기동된다.
 *
 * [반환값 규칙]
 *   gboolean: 실패 시 GError 설정 (호출자 g_error_free)
 *   JsonArray/JsonObject: 호출자가 소유권 보유 (json_*_unref)
 *   gchar** (iface_id_out): 호출자가 g_free
 * ====================================================================
 */
#ifndef PURECVISOR_OVN_MANAGER_H
#define PURECVISOR_OVN_MANAGER_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/*
 * OVN Manager -- Open Virtual Network SDN 컨트롤 플레인
 *
 * Phase 1: 논리 스위치 + Geneve 터널 자동화
 * Phase 2: ACL (보안 그룹) + 분산 DHCP + 멀티테넌트
 * Phase 3: 분산 L3 라우터 + 외부 게이트웨이
 *
 * 내부적으로 ovn-nbctl 명령을 실행합니다.
 */

/* ---- Lifecycle ---- */
void pcv_ovn_init(void);         /* ovn-nbctl 존재 여부 확인. 데몬 시작 시 호출. */
void pcv_ovn_shutdown(void);     /* g_ovn_available 플래그 리셋. 데몬 종료 시 호출. */
gboolean pcv_ovn_is_available(void);  /* OVN 가용 여부 반환 */

/* ---- Phase 1: 논리 스위치 ---- */
gboolean    pcv_ovn_switch_create(const gchar *name, const gchar *subnet, GError **error);
gboolean    pcv_ovn_switch_delete(const gchar *name, GError **error);
JsonArray  *pcv_ovn_switch_list(void);
gboolean    pcv_ovn_port_add(const gchar *sw, const gchar *port, const gchar *mac, const gchar *ip, GError **error);
gboolean    pcv_ovn_port_remove(const gchar *sw, const gchar *port, GError **error);

/* ---- Phase 2: ACL + DHCP ----
 *
 * [ACL (Access Control List)]
 *   OVN의 보안 그룹 역할. 방향(to-lport/from-lport), 우선순위(0~32767),
 *   매칭 조건, 액션(allow/drop/reject)으로 구성.
 *
 * [분산 DHCP]
 *   ovn-nbctl dhcp-options-create 로 서브넷의 DHCP 옵션을 설정.
 *   각 hypervisor의 ovn-controller가 로컬에서 DHCP 응답을 생성한다. */
gboolean pcv_ovn_acl_add(const gchar *sw, const gchar *direction, gint priority,
                           const gchar *match, const gchar *action, GError **error);
gboolean pcv_ovn_acl_delete(const gchar *sw, const gchar *direction, gint priority,
                              const gchar *match, GError **error);
JsonArray *pcv_ovn_acl_list(const gchar *sw);
gboolean pcv_ovn_dhcp_enable(const gchar *subnet, const gchar *gw, GError **error);

/* ---- Phase 3: 논리 라우터 ----
 *
 * [논리 라우터]
 *   OVN 논리 라우터는 서로 다른 논리 스위치 간의 L3 라우팅을 제공.
 *   router_add_port()로 스위치와 라우터를 연결하면
 *   해당 서브넷 간 트래픽이 라우팅된다.
 *
 * [NAT]
 *   snat  : 내부 -> 외부 소스 주소 변환 (인터넷 접근용)
 *   dnat  : 외부 -> 내부 목적지 주소 변환 (포트 포워딩용)
 *   dnat_and_snat : 양방향 1:1 NAT (플로팅 IP) */
gboolean    pcv_ovn_router_create(const gchar *name, GError **error);
gboolean    pcv_ovn_router_delete(const gchar *name, GError **error);
gboolean    pcv_ovn_router_add_port(const gchar *router, const gchar *sw,
                                     const gchar *mac, const gchar *cidr, GError **error);
gboolean    pcv_ovn_router_remove_port(const gchar *router, const gchar *port, GError **error);
JsonArray  *pcv_ovn_router_list(void);

/* ---- NAT ---- */
gboolean pcv_ovn_nat_add(const gchar *router, const gchar *type,
                          const gchar *external_ip, const gchar *logical_ip, GError **error);
gboolean pcv_ovn_nat_delete(const gchar *router, const gchar *type,
                             const gchar *external_ip, const gchar *logical_ip, GError **error);
JsonArray *pcv_ovn_nat_list(const gchar *router);

/* ---- DHCP list ---- */
JsonArray *pcv_ovn_dhcp_list(void);

/* ---- Encap + Auto-provision ----
 *
 * [Encap 설정] (클러스터 빌드 전용)
 *   OVS에 OVN 터널 캡슐화 타입(geneve/vxlan)과 터널 IP를 설정.
 *   daemon.conf [ovn] 섹션에서 읽어 auto_provision()에서 자동 호출.
 *
 * [Auto-provision] (클러스터 빌드 전용)
 *   데몬 시작 시 daemon.conf [ovn] 기반으로 기본 논리 스위치,
 *   라우터, DHCP 옵션을 자동 생성. 각 단계 실패는 비치명적. */
gboolean pcv_ovn_setup_encap(const gchar *encap_type, const gchar *encap_ip,
                              const gchar *remote, GError **error);
gboolean pcv_ovn_auto_provision(void);
gboolean pcv_ovn_single_prepare_local(GError **error);

/* ---- Multi-tenant ----
 *
 * [멀티테넌트 격리]
 *   tenant_create()는 테넌트 전용 논리 스위치 + ACL + DHCP를 일괄 생성.
 *   ACL이 자체 스위치 포트만 허용하므로 테넌트 간 트래픽은 차단된다. */
gboolean pcv_ovn_tenant_create(const gchar *tenant, const gchar *subnet, GError **error);
gboolean pcv_ovn_tenant_delete(const gchar *tenant, GError **error);

/* ---- VM port lifecycle ----
 *
 * [VM 포트 자동 관리]
 *   vm.create 시: vm_port_setup() -> "vm-<name>" OVN 포트 생성 + port-security
 *   vm.delete 시: vm_port_cleanup() -> "vm-<name>" OVN 포트 제거
 *   iface_id_out에 반환된 ID는 OVS 인테그레이션 브릿지에서 참조. */
gboolean pcv_ovn_vm_port_setup(const gchar *sw, const gchar *vm_name,
                                const gchar *mac, const gchar *ip,
                                gchar **iface_id_out, GError **error);
gboolean pcv_ovn_vm_port_cleanup(const gchar *vm_name, GError **error);

/* ---- Detail -- REST API용 상세 조회 ---- */
JsonObject *pcv_ovn_switch_detail(const gchar *name);   /* 포트 목록 + ACL 목록 */
JsonObject *pcv_ovn_router_detail(const gchar *name);   /* 포트(MAC/networks) + NAT 목록 */

/* ---- Status ---- */
JsonObject *pcv_ovn_status(void);   /* available, version, switch_count, router_count */

G_END_DECLS

#endif /* PURECVISOR_OVN_MANAGER_H */
