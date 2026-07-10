/**
 * @file network_firewall.h
 * @brief nftables 방화벽 공개 인터페이스 -- 모드별 규칙 설정/해제
 *
 * ====================================================================
 * [역할]
 *   network_manager.c 에서 브릿지 모드(nat/isolated/routed)에 따라
 *   적절한 setup 함수를 호출하고, 삭제 시 teardown 을 호출한다.
 *
 * [nftables 기본 개념 (주니어 참고)]
 *   nftables는 Linux 커널의 패킷 필터링 프레임워크이다.
 *   iptables의 후속 프로젝트로, 더 간결한 문법과 성능을 제공한다.
 *
 *   구조: table > chain > rule
 *     - table : 규칙 그룹 (예: inet purecvisor)
 *     - chain : 패킷 훅 포인트 (예: postrouting, forward)
 *     - rule  : 매칭 조건 + 액션 (예: ip saddr 10.0.0.0/24 masquerade)
 *
 *   "inet" 패밀리는 IPv4+IPv6 통합 처리를 의미한다.
 *
 * [호출 흐름]
 *   network.create (mode="nat")      --> network_firewall_setup_nat()
 *   network.create (mode="isolated") --> network_firewall_setup_isolated()
 *   network.create (mode="routed")   --> network_firewall_setup_routed()
 *   network.delete                   --> network_firewall_teardown()
 *   network.mode_set                 --> teardown() + 새 모드 setup()
 *
 * [공통 파라미터]
 *   bridge_name: 대상 브릿지 인터페이스명 (예: "pcvbr0")
 *   cidr:        브릿지에 할당된 CIDR (예: "10.10.10.1/24")
 *   error:       실패 시 GError 설정 (호출자가 g_error_free 해야 함)
 *
 * [주의]
 *   bridge 모드(물리 NIC 슬레이브)에서는 방화벽 함수를 호출하지 않는다.
 *   모든 함수는 root 권한이 필요하다 (nft 명령 사용).
 *
 * [모드별 요약]
 *   nat:
 *     - MASQUERADE : 내부 VM -> 외부 트래픽의 소스 IP를 호스트 IP로 변환
 *     - FORWARD    : 브릿지에서 나가는/들어오는 패킷 허용 (conntrack)
 *     - ip_forward : 커널 라우팅 활성화 (sysctl)
 *     가장 흔한 모드. 가정/소규모 환경에서 사용.
 *
 *   isolated:
 *     - intra-bridge ACCEPT : 같은 브릿지 내 VM끼리만 통신 허용
 *     - 외부 DROP           : 외부로 나가거나 들어오는 트래픽 전면 차단
 *     보안이 중요한 테스트 환경에서 사용.
 *
 *   routed:
 *     - ip_forward 만 활성화
 *     - MASQUERADE 없음
 *     상위 라우터가 서브넷 경로를 알고 있는 환경에서 사용.
 * ====================================================================
 */
#ifndef PURECVISOR_NETWORK_FIREWALL_H
#define PURECVISOR_NETWORK_FIREWALL_H

#include <glib.h>

G_BEGIN_DECLS

/* Sprint G: 3종 네트워크 모드 방화벽 설정 함수 */

/**
 * nat 모드 -- MASQUERADE + forward 규칙 설정 (기본값, B-2 수정 포함)
 *
 * [생성되는 nftables 규칙]
 *   postrouting: oifname != "br" ip saddr <subnet> masquerade
 *   forward:     iifname "br" accept
 *   forward:     oifname "br" ct state established,related accept
 *
 * [왜 oifname != "br" 인가?]
 *   브릿지 내부 통신(VM<->VM)은 NAT할 필요가 없다.
 *   브릿지 밖으로 나가는 패킷만 MASQUERADE 한다.
 */
gboolean network_firewall_setup_nat     (const gchar *bridge_name, const gchar *cidr, GError **error);

/**
 * isolated 모드 -- intra-bridge 허용, 외부 forward 전면 차단
 *
 * [생성되는 nftables 규칙]
 *   forward: iifname "br" oifname "br" accept  (브릿지 내부만 허용)
 *   forward: iifname "br" drop                  (외부로 나가는 것 차단)
 *   forward: oifname "br" drop                  (외부에서 들어오는 것 차단)
 *
 * [주의] 규칙 순서가 중요하다. intra-bridge accept가 먼저 매칭되어야
 * 이후의 drop 규칙에 걸리지 않는다.
 */
gboolean network_firewall_setup_isolated(const gchar *bridge_name, const gchar *cidr, GError **error);

/**
 * routed 모드 -- IP forward 활성화, masquerade 없음
 *
 * 상위 라우터가 이 서브넷에 대한 정적 경로를 보유하고 있다는 전제.
 * 최소한의 규칙으로 성능 오버헤드를 최소화한다.
 */
gboolean network_firewall_setup_routed  (const gchar *bridge_name, const gchar *cidr, GError **error);

/**
 * teardown -- 브릿지 삭제 시 관련 nftables 규칙 제거 (B-3 수정)
 *
 * nft -a list chain 으로 handle 번호를 조회한 후
 * bridge_name이 포함된 규칙만 선택적으로 삭제한다.
 * 다른 브릿지의 규칙에는 영향 없음.
 *
 * [멱등] 규칙이 이미 없으면 에러 없이 TRUE 반환.
 */
gboolean network_firewall_teardown      (const gchar *bridge_name, GError **error);

G_END_DECLS

#endif /* PURECVISOR_NETWORK_FIREWALL_H */
