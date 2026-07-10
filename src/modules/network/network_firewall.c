/**
 * @file network_firewall.c
 * @brief nftables 기반 네트워크 방화벽 제어 모듈
 *
 * ====================================================================
 * [아키텍처 위치]
 *   network_manager.c --> network_firewall (이 파일)
 *
 *   network.create / network.mode_set RPC 처리 중 모드별 방화벽 규칙을
 *   설정하기 위해 network_manager.c 에서 호출된다.
 *   network.delete 시에는 teardown 이 호출되어 규칙을 제거한다.
 *
 * [nftables 기본 개념 (주니어 필독)]
 *   nftables는 Linux 커널의 패킷 필터링/NAT 프레임워크이다.
 *   iptables를 대체하며, 더 간결한 문법과 원자적 규칙 적용을 제공한다.
 *
 *   [계층 구조]
 *     table (테이블)
 *       +-- chain (체인) : 패킷을 가로채는 훅(hook) 포인트
 *             +-- rule (규칙) : 매칭 조건 + 액션
 *
 *   [주요 훅 포인트]
 *     postrouting : 패킷이 라우팅 결정 후 출구로 나가기 직전
 *                   -> NAT(MASQUERADE)에 사용
 *     forward     : 호스트를 경유하는(자기 것이 아닌) 패킷
 *                   -> 브릿지 간 포워딩 허용/차단에 사용
 *
 *   [MASQUERADE란?]
 *     SNAT(Source NAT)의 동적 버전. 나가는 패킷의 소스 IP를
 *     호스트의 외부 인터페이스 IP로 자동 변환한다.
 *     가정용 공유기의 NAT와 동일한 원리.
 *
 *   [conntrack (ct state)]
 *     커널의 연결 추적 시스템. established,related 상태는
 *     이미 허용된 연결의 응답 패킷을 의미한다.
 *     예: VM이 외부로 HTTP 요청 -> 외부 서버의 응답이 돌아올 때
 *     conntrack이 이를 "기존 연결의 응답"으로 인식하여 허용.
 *
 * [nftables 테이블 구조 -- PureCVisor 전용]
 *   테이블:  inet purecvisor (IPv4+IPv6 통합)
 *   체인:    postrouting (type nat, hook postrouting) -- MASQUERADE 규칙
 *            forward     (type filter, hook forward)  -- 패킷 포워딩 규칙
 *
 *   _ensure_table() 로 테이블/체인을 멱등 생성한다.
 *   nft add 명령은 이미 존재하면 무시하므로 중복 호출해도 안전.
 *
 * [모드별 방화벽 정책 상세]
 *   nat:
 *     - postrouting: ip saddr <subnet> masquerade
 *     - forward: iif <bridge> accept (브릿지에서 나가는 패킷 허용)
 *     - forward: oif <bridge> ct state established,related accept (응답 허용)
 *     - sysctl net.ipv4.ip_forward=1
 *     [B-2 수정] 이전에는 iif=br oif=br (인트라 브릿지 전용)이었으나
 *     외부 통신이 불가하여 수정됨.
 *
 *   isolated:
 *     - forward: iif <bridge> oif <bridge> accept (브릿지 내부만 허용)
 *     - forward: iif <bridge> drop (외부로 나가는 패킷 차단)
 *     - DHCP/DNS 트래픽은 호스트 INPUT에서 허용 (게이트웨이 역할)
 *     - MASQUERADE 없음
 *
 *   routed:
 *     - sysctl net.ipv4.ip_forward=1 만 설정
 *     - MASQUERADE 없음 (상위 라우터가 정적 경로 보유 전제)
 *     - 최소한의 규칙으로 성능 오버헤드 최소화
 *
 * [핵심 내부 함수]
 *   _ensure_table()   - inet purecvisor 테이블+체인 멱등 생성
 *   _cidr_to_subnet() - CIDR("10.0.0.1/24") -> 서브넷("10.0.0.0/24") 변환
 *                        NULL/형식오류 방어 포함 (Fix #2)
 *
 * [teardown 처리]
 *   network_firewall_teardown() 은 해당 브릿지 관련 nftables 규칙만
 *   선택적으로 삭제한다. 다른 브릿지의 규칙에는 영향 없음.
 *   "No such file or directory" 에러는 이미 없는 규칙이므로 무시 (멱등).
 *
 * [의존 모듈]
 *   pcv_spawn.h - nft, sysctl 명령 실행
 *
 * [주의사항]
 *   - _cidr_to_subnet 에서 '/' 없는 CIDR 입력 시 NULL dereference 방어 필수.
 *   - nft 명령은 root 권한 필요 (현재 에디션 데몬은 root로 실행).
 *   - 체인 우선순위: postrouting=srcnat, forward=filter (기본값 0).
 * ====================================================================
 */
#include <glib.h>
#include <gio/gio.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "modules/network/network_firewall.h"
#include "modules/network/network_firewall_host.h"  /* [VP-6] 호스트 방화벽 공존 */
#include "../../utils/pcv_spawn.h"
#include "../../utils/pcv_validate.h"
#include "../../utils/pcv_config.h"                  /* [VP-6] firewall_integration 가드 */

/* ── nftables 공용 테이블/체인 초기화 (멱등) ───────────────────
 *
 * [동작]
 *   1. "inet purecvisor" 테이블 생성 (inet = IPv4+IPv6 통합 패밀리)
 *   2. "postrouting" 체인 생성 (NAT 훅, MASQUERADE용)
 *      - type nat : NAT 전용 체인
 *      - hook postrouting : 라우팅 결정 후, 패킷이 나가기 직전
 *      - priority srcnat : NAT 체인의 표준 우선순위
 *   3. "forward" 체인 생성 (필터 훅, 포워딩 허용/차단용)
 *      - type filter : 일반 패킷 필터링
 *      - hook forward : 호스트를 경유하는 패킷
 *      - priority filter : 필터 체인의 표준 우선순위
 *
 * [멱등성]
 *   nft add 는 이미 존재하는 객체에 대해 에러를 반환하지 않는다.
 *   따라서 여러 브릿지가 동시에 생성되어도 안전하다.
 *
 * [주의]
 *   pcv_spawn_fire()는 fire-and-forget으로 결과를 확인하지 않는다.
 *   테이블/체인 생성 실패는 이후 규칙 추가 시 감지된다.
 * ──────────────────────────────────────────────────────────────── */
static void _ensure_table(void) {
    /* 1단계: 테이블 생성 (inet = IPv4+IPv6 통합) */
    { const gchar *a[] = {"nft","add","table","inet","purecvisor",NULL};          pcv_spawn_fire(a); }
    /* 2단계: postrouting 체인 — MASQUERADE(NAT) 규칙용 */
    { const gchar *a[] = {"nft","add","chain","inet","purecvisor","postrouting",
                           "{ type nat hook postrouting priority srcnat; }",NULL}; pcv_spawn_fire(a); }
    /* 3단계: forward 체인 — 브릿지 간 포워딩 허용/차단 규칙용 */
    { const gchar *a[] = {"nft","add","chain","inet","purecvisor","forward",
                           "{ type filter hook forward priority filter; }",NULL};  pcv_spawn_fire(a); }
}

/* ── CIDR -> 서브넷 변환 ("10.0.0.1/24" -> "10.0.0.0/24") ────────
 *
 * [왜 변환이 필요한가?]
 *   브릿지에 할당된 CIDR의 호스트 부분(예: .1)은 게이트웨이 IP이다.
 *   nftables MASQUERADE 규칙에서는 서브넷 주소(예: .0)가 필요하다.
 *   "ip saddr 10.0.0.0/24 masquerade" 로 해당 서브넷의 모든 트래픽을 NAT.
 *
 * [단순화된 구현]
 *   마지막 옥텟을 항상 0으로 대체하는 간이 방식.
 *   /16, /8 같은 큰 서브넷에서는 정확하지 않을 수 있으나,
 *   VM 네트워크는 통상 /24를 사용하므로 실용적으로 충분.
 *
 * [Fix #2] NULL 및 형식 오류 방어
 *   cidr=NULL, '.' 개수 부족, '/' 누락 시 GError를 반환한다.
 * ──────────────────────────────────────────────────────────────── */
static gchar *_cidr_to_subnet(const gchar *cidr, GError **error) {
    /* [Fix #2] NULL 및 형식 오류 방어 */
    if (!cidr) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "CIDR is NULL");
        return NULL;
    }
    /* CIDR을 '.'으로 4개 옥텟으로 분리: "10.0.0.1/24" -> ["10","0","0","1/24"] */
    gchar **parts = g_strsplit(cidr, ".", 4);
    if (!parts || g_strv_length(parts) != 4) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Invalid CIDR: %s", cidr);
        g_strfreev(parts);
        return NULL;
    }
    /* 마지막 옥텟에서 '/'를 기준으로 분리: "1/24" -> ["1","24"] */
    gchar **last = g_strsplit(parts[3], "/", 2);
    /* [Fix #2] '/' 없는 CIDR 입력 시 last[1]=NULL -> NULL dereference 방어 */
    if (!last || g_strv_length(last) < 2 || !last[1]) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Invalid CIDR format (missing /prefix): %s", cidr);
        g_strfreev(last);
        g_strfreev(parts);
        return NULL;
    }
    /* 마지막 옥텟을 0으로 대체하여 서브넷 주소 생성 */
    gchar *subnet = g_strdup_printf("%s.%s.%s.0/%s",
                                    parts[0], parts[1], parts[2], last[1]);
    g_strfreev(parts);
    g_strfreev(last);
    return subnet;
}

/* =================================================================
 * network_firewall_setup_nat  (B-2 수정 버전)
 *
 * [NAT 모드 방화벽 규칙 설정]
 *   VM이 외부 인터넷에 접근할 수 있도록 MASQUERADE + FORWARD 규칙을 설정.
 *   가정용 공유기와 동일한 원리로 동작한다.
 *
 * [생성되는 규칙 3개]
 *   1. postrouting: 브릿지 외부로 나가는 서브넷 트래픽을 MASQUERADE
 *      - oifname != "br" : 브릿지 내부 트래픽은 NAT하지 않음
 *      - ip saddr <subnet> : 해당 서브넷에서 출발하는 패킷만
 *      - masquerade : 소스 IP를 호스트의 외부 IP로 변환
 *
 *   2. forward: 브릿지에서 나가는 모든 패킷 허용
 *      - iifname "br" : 이 브릿지에서 들어오는(=VM에서 나가는) 패킷
 *      - accept : 포워딩 허용
 *
 *   3. forward: 브릿지로 들어오는 응답 패킷만 허용
 *      - oifname "br" : 이 브릿지로 나가는(=VM으로 돌아가는) 패킷
 *      - ct state established,related : 기존 연결의 응답만 허용
 *      - accept : 포워딩 허용
 *
 * [B-2 수정 이력]
 *   수정 전: iif=br oif=br (브릿지 내부만 허용) -> VM이 외부 접근 불가
 *   수정 후: iif=br (모든 외부로 허용) + oif=br + conntrack (응답만 허용)
 *
 * [sysctl ip_forward]
 *   커널의 IP 포워딩 기능을 활성화한다.
 *   이 설정이 없으면 호스트가 패킷을 다른 인터페이스로 전달하지 않아
 *   VM 트래픽이 외부로 나가지 못한다.
 * ================================================================= */
gboolean network_firewall_setup_nat(const gchar *bridge_name, const gchar *cidr,
                                    GError **error) {
    /* [보안] bridge_name 인젝션 방어: [a-zA-Z0-9_-] 이외 문자 차단 */
    if (!pcv_validate_bridge_name(bridge_name)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Invalid bridge name for firewall rule: %s",
                    bridge_name ? bridge_name : "(null)");
        return FALSE;
    }
    /* 커널 IP 포워딩 활성화 (0=비활성, 1=활성)
     * 이 설정은 시스템 전역이므로 한 번만 해도 되지만, 멱등이므로 매번 호출해도 무방 */
    { const gchar *a[] = {"sysctl","-w","net.ipv4.ip_forward=1",NULL}; pcv_spawn_fire(a); }
    _ensure_table();

    /* CIDR에서 서브넷 주소 추출 (예: "10.10.10.1/24" -> "10.10.10.0/24") */
    gchar *subnet = _cidr_to_subnet(cidr, error);
    if (!subnet) return FALSE;

    /* [규칙 1] masquerade: 브릿지 이외 출구로 나가는 서브넷 패킷에 SNAT 적용
     * oifname != "br" : 브릿지 내부(VM<->VM) 트래픽은 NAT 불필요 */
    gchar *oif_ne = g_strdup_printf("!= \"%s\"", bridge_name);
    { const gchar *a[] = {"nft","add","rule","inet","purecvisor","postrouting",
                           "oifname", oif_ne, "ip","saddr", subnet,
                           "masquerade", NULL};
      pcv_spawn_fire(a); }
    g_free(oif_ne);

    /* [규칙 2] [B-2 수정] forward iif=br -> 외부로 나가는 패킷 허용
     * VM에서 발생한 패킷이 호스트를 거쳐 외부로 나갈 수 있게 한다 */
    gchar *iif = g_strdup_printf("\"%s\"", bridge_name);
    { const gchar *a[] = {"nft","add","rule","inet","purecvisor","forward",
                           "iifname", iif, "accept", NULL};
      pcv_spawn_fire(a); }

    /* [규칙 3] [B-2 수정] forward oif=br + established,related -> 응답 패킷 복귀 허용
     * 외부에서 VM으로 들어오는 패킷 중, 기존 연결의 응답만 허용한다.
     * 이렇게 하면 VM이 먼저 시작한 연결의 응답은 허용하되,
     * 외부에서 VM으로의 새로운 연결 시작은 차단된다 (보안). */
    gchar *oif = g_strdup_printf("\"%s\"", bridge_name);
    { const gchar *a[] = {"nft","add","rule","inet","purecvisor","forward",
                           "oifname", oif,
                           "ct","state","established,related","accept", NULL};
      pcv_spawn_fire(a); }

    g_free(iif); g_free(oif); g_free(subnet);

    /* ── [VP-6] 호스트 방화벽 자동 공존 ──────────────────────────────
     * Developer note:
     *   이 지점은 create 경로와 bootstrap ensure 경로가 함께 통과하는
     *   NAT 설정 공통 말미다. config [network] firewall_integration 기본값
     *   "auto" 이면 호스트가 이미 쓰는 UFW/iptables-DROP 정책에 게스트
     *   포워딩 + 호스트 dnsmasq(DHCP 67/DNS 53) 경로를 뚫는다.
     *   pcv_host_fw_integrate 는 멱등(ufw 자체 중복 스킵 / iptables -C 가드)
     *   이므로 NAT 마커와 무관하게 재호출돼도 안전하다. 반환값은 의도적으로
     *   무시(soft)하여, 호스트 방화벽 공존 실패가 setup_nat 성공 판정(=nft NAT
     *   규칙 자체는 걸림)에 영향을 주지 않게 한다.
     *
     * Operator note:
     *   "게스트가 인터넷과 DHCP를 쓸 수 있도록 호스트 방화벽에 자동으로 구멍을
     *   내는" 개입 지점이다. libvirt 가 virbr0 을 뚫는 것과 동급의 인터페이스
     *   전체 allow 다. 원치 않으면 daemon.conf 의 [network] firewall_integration
     *   = off 로 옵트아웃할 수 있으며, 그 경우 데몬은 호스트 방화벽을 건드리지
     *   않는다(공존 실패 시 게스트 네트워크가 죽을 수 있음은 운영자 책임). */
    if (g_strcmp0(pcv_config_get_string("network", "firewall_integration", "auto"),
                  "auto") == 0)
        pcv_host_fw_integrate(bridge_name, NULL);

    return TRUE;
}

/* =================================================================
 * network_firewall_setup_isolated
 *
 * [isolated 모드 방화벽 규칙 설정]
 *   같은 브릿지에 연결된 VM끼리만 통신을 허용하고,
 *   외부(인터넷, 다른 브릿지)와의 통신을 완전히 차단한다.
 *   보안이 중요한 테스트 환경이나 격리된 개발 환경에서 사용.
 *
 * [생성되는 규칙 3개 (순서 중요!)]
 *   1. iif=br oif=br accept  -> 브릿지 내부 통신만 허용
 *   2. iif=br drop            -> 외부로 나가는 것 차단
 *   3. oif=br drop            -> 외부에서 들어오는 것 차단
 *
 *   규칙 1이 먼저 매칭되어야 브릿지 내부 트래픽이 규칙 2에서
 *   차단되지 않는다. nftables는 위에서 아래로 순서대로 평가.
 *
 * [ip_forward 활성화 이유]
 *   Linux Bridge 내부 트래픽도 forward 체인을 거치는 경우가 있다.
 *   ip_forward=0이면 forward 체인 자체가 비활성화되어
 *   drop 규칙이 적용되지 않을 수 있다 (Fix #3).
 * ================================================================= */
gboolean network_firewall_setup_isolated(const gchar *bridge_name,
                                         const gchar *cidr __attribute__((unused)),
                                         GError **error) {
    /* [보안] bridge_name 인젝션 방어: [a-zA-Z0-9_-] 이외 문자 차단 */
    if (!pcv_validate_bridge_name(bridge_name)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Invalid bridge name for firewall rule: %s",
                    bridge_name ? bridge_name : "(null)");
        return FALSE;
    }
    /* [Fix #3] forward chain 동작을 보장하기 위해 ip_forward 활성화.
     * Linux Bridge intra-bridge 통신은 L2이나, forward chain 규칙 자체가
     * ip_forward=1 상태에서만 커널에 의해 평가됨. */
    { const gchar *a[] = {"sysctl","-w","net.ipv4.ip_forward=1",NULL};
      pcv_spawn_fire(a); }
    _ensure_table();

    gchar *iif = g_strdup_printf("\"%s\"", bridge_name);
    gchar *oif = g_strdup_printf("\"%s\"", bridge_name);

    /* [규칙 1] intra-bridge (iif=br, oif=br) -> accept
     * 같은 브릿지 내 VM끼리의 통신을 허용 */
    { const gchar *a[] = {"nft","add","rule","inet","purecvisor","forward",
                           "iifname", iif, "oifname", oif, "accept", NULL};
      pcv_spawn_fire(a); }

    /* [규칙 2] iif=br -> 외부 drop
     * 이 브릿지에서 출발하여 외부로 나가려는 패킷을 차단 */
    { const gchar *a[] = {"nft","add","rule","inet","purecvisor","forward",
                           "iifname", iif, "drop", NULL};
      pcv_spawn_fire(a); }

    /* [규칙 3] 외부 -> oif=br drop
     * 외부에서 이 브릿지로 들어오려는 패킷을 차단 */
    { const gchar *a[] = {"nft","add","rule","inet","purecvisor","forward",
                           "oifname", oif, "drop", NULL};
      pcv_spawn_fire(a); }

    g_free(iif); g_free(oif);
    return TRUE;
}

/* =================================================================
 * network_firewall_setup_routed
 *
 * [routed 모드 방화벽 규칙 설정]
 *   IP 포워딩만 활성화하고 MASQUERADE는 사용하지 않는다.
 *   상위 라우터가 이 서브넷에 대한 정적 경로(static route)를
 *   이미 보유하고 있다는 전제 하에 동작한다.
 *
 *   예: 상위 라우터에 "10.10.10.0/24 via <호스트IP>" 경로가 설정됨
 *
 *   NAT 오버헤드가 없으므로 성능이 nat 모드보다 좋지만,
 *   네트워크 인프라의 추가 설정이 필요하다.
 * ================================================================= */
gboolean network_firewall_setup_routed(const gchar *bridge_name,
                                       const gchar *cidr __attribute__((unused)),
                                       GError **error) {
    /* [보안] bridge_name 인젝션 방어: [a-zA-Z0-9_-] 이외 문자 차단 */
    if (!pcv_validate_bridge_name(bridge_name)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Invalid bridge name for firewall rule: %s",
                    bridge_name ? bridge_name : "(null)");
        return FALSE;
    }
    /* IP 포워딩만 활성화 — MASQUERADE 없음 */
    { const gchar *a[] = {"sysctl","-w","net.ipv4.ip_forward=1",NULL}; pcv_spawn_fire(a); }
    _ensure_table();

    /* forward: 브릿지에서 나가는 패킷 허용 */
    gchar *iif = g_strdup_printf("\"%s\"", bridge_name);
    { const gchar *a[] = {"nft","add","rule","inet","purecvisor","forward",
                           "iifname", iif, "accept", NULL};
      pcv_spawn_fire(a); }

    /* forward: 응답 패킷만 브릿지로 허용 (conntrack 기반) */
    gchar *oif = g_strdup_printf("\"%s\"", bridge_name);
    { const gchar *a[] = {"nft","add","rule","inet","purecvisor","forward",
                           "oifname", oif,
                           "ct","state","established,related","accept", NULL};
      pcv_spawn_fire(a); }

    g_free(iif); g_free(oif);
    return TRUE;
}

/* =================================================================
 * network_firewall_teardown  (B-3 수정: 브릿지 삭제 시 규칙 정리)
 *
 * [동작 원리]
 *   특정 브릿지에 관련된 nftables 규칙만 선택적으로 삭제한다.
 *   다른 브릿지의 규칙에는 영향을 주지 않는다.
 *
 * [알고리즘]
 *   1. nft -a list chain 명령으로 체인의 모든 규칙을 조회
 *      -a 플래그: 각 규칙에 핸들(handle) 번호를 표시
 *      예: "iifname \"br-dev\" accept # handle 42"
 *
 *   2. 출력에서 bridge_name이 포함된 줄만 필터링
 *      "# handle N" 패턴에서 핸들 번호를 추출
 *
 *   3. 핸들 번호를 역순으로 삭제
 *      [역순 삭제가 필요한 이유]
 *      nftables에서 규칙 삭제 시 이후 규칙의 핸들 번호가 변경될 수 있다.
 *      큰 번호부터 삭제하면 앞의 규칙에 영향을 주지 않는다.
 *
 *   4. postrouting + forward 양쪽 체인 모두 처리
 *
 * [멱등성]
 *   체인이 없거나 규칙이 없어도 에러를 반환하지 않는다.
 *   이미 삭제된 브릿지에 대해 다시 호출해도 안전하다.
 *
 * [Fix #7] popen() -> pcv_spawn_sync() 교체
 *   셸을 경유하지 않으므로 command injection 경로가 제거되었다.
 * ================================================================= */
gboolean network_firewall_teardown(const gchar *bridge_name,
                                   GError **error) {
    /* [보안] bridge_name 인젝션 방어: [a-zA-Z0-9_-] 이외 문자 차단 */
    if (!pcv_validate_bridge_name(bridge_name)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Invalid bridge name for firewall teardown: %s",
                    bridge_name ? bridge_name : "(null)");
        return FALSE;
    }
    /* [Fix #7] popen() -> pcv_spawn_sync() 로 교체.
     * 셸을 경유하지 않으므로 인젝션 경로 제거 및 pcv_spawn_* 스타일 일관성 확보. */
    const gchar *chains[] = {"postrouting", "forward", NULL};

    /* [V15] 부분 문자열 충돌 방지: nft는 인터페이스 이름을 큰따옴표로 감싸
     * 출력한다 (iifname "br0"). bare strstr(*l, "br0") 는 "br0x" 규칙 줄까지
     * 매칭해 다른 브릿지 규칙을 삭제한다. 따옴표로 감싼 토큰("br0")을 needle
     * 로 사용해 정확히 이 브릿지의 규칙만 매칭한다. bridge_name 은 위에서
     * 이미 화이트리스트 검증되었으므로 needle 구성은 안전하다. */
    gchar *needle = g_strdup_printf("\"%s\"", bridge_name);

    /* 두 체인(postrouting, forward) 모두에서 해당 브릿지 관련 규칙을 제거 */
    for (int ci = 0; chains[ci]; ci++) {
        /* nft -a list chain: 각 규칙에 핸들 번호를 포함하여 출력 */
        const gchar *list_argv[] = {
            "nft", "-a", "list", "chain", "inet", "purecvisor", chains[ci], NULL};
        gchar *stdout_buf = NULL;
        /* 실패해도 계속 진행 (체인이 없을 수 있음 — 최초 사용 전 등) */
        pcv_spawn_sync(list_argv, &stdout_buf, NULL, NULL);
        if (!stdout_buf) continue;

        /* 삭제할 규칙의 핸들 번호를 수집 */
        GList *handles = NULL;
        gchar **lines  = g_strsplit(stdout_buf, "\n", -1);
        g_free(stdout_buf);

        for (gchar **l = lines; *l; l++) {
            /* [V15] 따옴표 토큰 매칭 — "br0" 는 "br0x" 규칙 줄과 충돌하지 않음 */
            if (!strstr(*l, needle)) continue;
            /* "# handle 42" 패턴에서 핸들 번호 추출 */
            gchar *h_ptr = strstr(*l, "# handle ");
            if (!h_ptr) continue;
            gint handle = atoi(h_ptr + 9);  /* "# handle " = 9글자 */
            if (handle > 0)
                handles = g_list_append(handles, GINT_TO_POINTER(handle));
        }
        g_strfreev(lines);

        /* 역순 삭제: 큰 핸들 번호부터 삭제하여 인덱스 밀림 방지 */
        handles = g_list_reverse(handles);
        for (GList *lp = handles; lp; lp = lp->next) {
            gchar *h_str = g_strdup_printf("%d", GPOINTER_TO_INT(lp->data));
            const gchar *del[] = {"nft","delete","rule","inet","purecvisor",
                                   chains[ci], "handle", h_str, NULL};
            pcv_spawn_fire(del);
            g_free(h_str);
        }
        g_list_free(handles);
    }
    g_free(needle);

    /* ── [VP-6] setup_nat 와 대칭: 호스트 방화벽 공존 룰 제거 ─────────
     * Operator note:
     *   브릿지 삭제 시, setup_nat 에서 자동으로 추가했던 ufw/iptables 공존
     *   룰을 되돌린다(대칭). config 가드도 동일하게 적용하여, off 로 옵트아웃한
     *   환경에서는 추가도 제거도 하지 않는다. 반환값은 soft 로 무시한다. */
    if (g_strcmp0(pcv_config_get_string("network", "firewall_integration", "auto"),
                  "auto") == 0)
        pcv_host_fw_remove(bridge_name, NULL);

    return TRUE;
}
