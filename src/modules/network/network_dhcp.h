/**
 * @file network_dhcp.h
 * @brief dnsmasq DHCP 서버 공개 인터페이스
 *
 * ====================================================================
 * [역할]
 *   network_manager.c 에서 브릿지 생성 시 DHCP를 시작하기 위해 호출.
 *   nat/isolated 모드에서만 사용되며, routed/bridge 모드에서는 미호출.
 *
 * [DHCP 기본 개념 (주니어 참고)]
 *   DHCP(Dynamic Host Configuration Protocol)는 네트워크에 연결된 장치에
 *   IP 주소, 서브넷 마스크, 게이트웨이, DNS 서버 등을 자동 할당하는 프로토콜.
 *   VM이 브릿지에 연결되면 DHCP 요청(DISCOVER)을 보내고,
 *   dnsmasq가 범위 내 IP를 할당(OFFER->REQUEST->ACK)한다.
 *
 * [dnsmasq란?]
 *   경량 DNS 포워더 + DHCP 서버. libvirt도 내부적으로 dnsmasq를 사용한다.
 *   PureCVisor는 libvirt와 별도로 자체 dnsmasq 인스턴스를 관리한다.
 *   포트 67(DHCP)과 선택적으로 포트 53(DNS)에서 수신.
 *
 * [함수 선택 가이드]
 *   network_dhcp_start()    - 기본 DHCP 전용 (DNS 비활성, 대부분의 경우)
 *   network_dhcp_start_ex() - DNS 포워더 활성화 옵션 포함 (E5 확장)
 *
 * [파라미터 규칙]
 *   bridge_name : pcv_validate_bridge_name() 을 통과한 유효한 이름
 *   cidr        : "10.10.10.1/24" 형식 (게이트웨이 IP + prefix length)
 *                 게이트웨이(.1)는 DHCP 범위에서 제외된다.
 *   error       : 실패 시 GError 설정, 호출자가 g_error_free 필수
 *
 * [PCV_NETWORK_RUNDIR]
 *   dnsmasq 설정/PID/리스 파일의 기준 디렉토리.
 *   pcv_validate.h 에서 "/var/run/purecvisor/network" 으로 정의됨.
 *   런타임 디렉토리이므로 재부팅 시 내용이 사라진다 (tmpfs).
 *
 * [생성되는 파일] (PCV_NETWORK_RUNDIR = /var/run/purecvisor/)
 *   dnsmasq-<bridge>.conf    -- 이 모듈이 생성하는 설정 파일
 *   dnsmasq-<bridge>.pid     -- dnsmasq가 자동 생성하는 PID 파일
 *   dnsmasq-<bridge>.leases  -- dnsmasq가 자동 생성하는 DHCP 임대 DB
 *   이 파일들은 network.delete 시 network_manager.c에서 전부 삭제된다.
 * ====================================================================
 */
#ifndef PURECVISOR_NETWORK_DHCP_H
#define PURECVISOR_NETWORK_DHCP_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * network_dhcp_start -- DHCP 전용 모드로 dnsmasq 시작 (DNS 비활성)
 * @bridge_name: 대상 브릿지 이름 (예: "br-dev")
 * @cidr: 게이트웨이 IP/CIDR (예: "10.10.10.1/24")
 * @error: 실패 시 GError 설정
 *
 * 내부적으로 network_dhcp_start_ex(bridge_name, cidr, FALSE, NULL, error)를 호출.
 * 기존 호출자와의 하위 호환을 위한 래퍼 함수이다.
 *
 * @return 성공 시 TRUE
 */
gboolean network_dhcp_start(const gchar *bridge_name,
                             const gchar *cidr,
                             GError     **error);

/**
 * network_dhcp_start_ex -- DNS 포워더 옵션 포함 확장 버전 (E5 추가)
 * @bridge_name: 대상 브릿지 이름
 * @cidr: 게이트웨이 IP/CIDR
 * @dns_enabled: TRUE이면 dnsmasq DNS 포워더도 활성화 (port=53).
 *               FALSE(기본)이면 port=0으로 DNS 비활성 -- systemd-resolved 충돌 방지.
 * @upstream_dns: dns_enabled=TRUE일 때 사용할 업스트림 DNS. NULL이면 "8.8.8.8".
 * @error: 실패 시 GError 설정
 *
 * [DNS 활성화가 필요한 경우]
 *   VM에서 호스트명 해석이 필요하지만 별도 DNS 서버가 없는 경우.
 *   dnsmasq가 upstream DNS로 쿼리를 중계(forward)한다.
 *
 * [DNS 비활성이 기본인 이유]
 *   systemd-resolved가 이미 포트 53을 점유하고 있으면
 *   dnsmasq가 "address already in use" 에러로 시작에 실패한다.
 *   DHCP만 필요한 대부분의 경우 DNS는 비활성이 안전하다.
 *
 * @return 성공 시 TRUE
 */
gboolean network_dhcp_start_ex(const gchar *bridge_name,
                                const gchar *cidr,
                                gboolean     dns_enabled,
                                const gchar *upstream_dns,
                                GError     **error);

/**
 * network_dhcp_start_v6 -- IPv6 RA(Router Advertisement) + DHCPv6 활성화
 * @bridge_name: 대상 브릿지 이름
 * @ipv6_prefix: IPv6 프리픽스 (예: "fd00:1::/64") — prefix length 포함 필수
 * @error: 실패 시 GError 설정
 *
 * dnsmasq 설정에 enable-ra + DHCPv6 범위를 추가하고 재시작합니다.
 * 기존 IPv4 DHCP 설정이 있으면 append, 없으면 새로 생성합니다.
 *
 * [IPv6 주소 할당 방식]
 *   - SLAAC: RA 패킷으로 프리픽스를 광고, 게스트가 자체 주소 생성
 *   - DHCPv6: 상태 기반 주소 할당 (::100 ~ ::1ff 범위)
 *   - 두 방식 병용 (enable-ra + dhcp-range)
 *
 * @return 성공 시 TRUE
 */
gboolean network_dhcp_start_v6(const gchar *bridge_name,
                                const gchar *ipv6_prefix,
                                GError     **error);

G_END_DECLS

#endif /* PURECVISOR_NETWORK_DHCP_H */
