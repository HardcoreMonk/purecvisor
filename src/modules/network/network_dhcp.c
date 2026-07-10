/**
 * @file network_dhcp.c
 * @brief dnsmasq 기반 가상 네트워크 DHCP 서버 자동 프로비저닝 모듈
 *
 * ====================================================================
 * [아키텍처 위치]
 *   network_manager.c --> network_dhcp (이 파일)
 *
 *   network.create (mode=nat/isolated) 처리 중 DHCP 서버를 시작하기 위해
 *   network_manager.c 에서 호출된다.
 *   routed/bridge 모드에서는 호출되지 않는다.
 *
 * [핵심 동작 흐름]
 *   1. CIDR 파싱: "10.10.10.1/24" -> 베이스 "10.10.10", prefix=24
 *   2. DHCP 범위 계산: prefix 기반 동적 산출 (Fix #4)
 *      .1 은 게이트웨이(브릿지 IP) 예약, .2 ~ .254 범위에서 할당
 *      /28 이하 소규모 서브넷도 올바르게 처리
 *   3. dnsmasq 설정 파일 생성 (PCV_NETWORK_RUNDIR/dnsmasq-<br>.conf)
 *   4. 기존 dnsmasq 프로세스 종료 (pkill -F, 에러 무시)
 *   5. 새 dnsmasq 백그라운드 실행 (자체 daemonize)
 *
 * [DNS 포워더 (E5 확장)]
 *   dns_enabled=TRUE 시 dnsmasq가 DNS 포워더로도 동작:
 *     - server=<upstream_dns> 지시자 추가 (기본: 8.8.8.8)
 *     - port=53 에서 DNS 쿼리 수신
 *   dns_enabled=FALSE (기본) 시:
 *     - port=0 으로 DNS 비활성화
 *     - no-resolv 로 /etc/resolv.conf 읽기 금지
 *     - systemd-resolved 와의 포트 53 충돌 방지
 *
 * [생성되는 파일] (PCV_NETWORK_RUNDIR = /var/run/purecvisor/)
 *   dnsmasq-<bridge>.conf    - dnsmasq 설정 (이 모듈이 생성)
 *   dnsmasq-<bridge>.pid     - dnsmasq PID (dnsmasq 자동 생성)
 *   dnsmasq-<bridge>.leases  - DHCP 임대 DB (dnsmasq 자동 생성)
 *   network.delete 시 network_manager.c 에서 전부 삭제.
 *
 * [하위 호환]
 *   network_dhcp_start() 는 dns_enabled=FALSE 로 _ex 버전을 호출하는
 *   래퍼 함수다. 기존 호출자를 수정하지 않아도 된다.
 *
 * [의존 모듈]
 *   pcv_validate.h - CIDR 형식 검증
 *   pcv_spawn.h    - dnsmasq/pkill 프로세스 실행
 *
 * [주의사항]
 *   - bind-interfaces 필수: libvirt dnsmasq(virbr0)가 0.0.0.0:67을
 *     점유한 상태에서 bind-interfaces 없으면 "Permission denied" 발생.
 *   - dnsmasq --conf-file= 에서 등호 형식 필수: 공백 분리 시
 *     일부 버전에서 "junk found in command line" 오류 발생.
 *   - /31, /32 서브넷은 DHCP 부적합이나 max_host=1 로 클램프.
 * ====================================================================
 */
#include <glib.h>
#include <gio/gio.h>
#include <string.h>
#include <arpa/inet.h>

#include "modules/network/network_dhcp.h"
#include "../../utils/pcv_validate.h"
#include "../../utils/pcv_spawn.h"

/* [E5] 래퍼: 기존 호출자 하위 호환 유지 */
gboolean network_dhcp_start(const gchar *bridge_name, const gchar *cidr, GError **error) {
    return network_dhcp_start_ex(bridge_name, cidr, FALSE, NULL, error);
}

/* [E5] 확장 버전: dns_enabled / upstream_dns 파라미터 지원 */
gboolean network_dhcp_start_ex(const gchar *bridge_name,
                                const gchar *cidr,
                                gboolean     dns_enabled,
                                const gchar *upstream_dns,
                                GError     **error) {
    // 1. CIDR 파싱 (예: "10.10.10.1/24" -> 베이스 IP "10.10.10" 추출)
    gchar **parts = g_strsplit(cidr, ".", 4);
    if (!parts || g_strv_length(parts) != 4) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Invalid CIDR for DHCP");
        g_strfreev(parts);
        return FALSE;
    }
    gchar *base_ip = g_strdup_printf("%s.%s.%s", parts[0], parts[1], parts[2]);
    g_strfreev(parts);

    // 2. [Fix #4] prefix 기반 동적 DHCP 범위 계산
    //    하드코딩(.100~.200) 제거 → /28 이하 소규모 서브넷에서도 올바른 범위 산출
    const gchar *slash = g_strrstr(cidr, "/");
    int prefix    = slash ? atoi(slash + 1) : 24;
    // 사용 가능한 호스트 수: 2^(32-prefix) - 2 (네트워크/브로드캐스트 제외)
    // /31, /32는 DHCP 부적합이나 최소 1로 클램프
    int max_host  = (prefix <= 30) ? ((1 << (32 - prefix)) - 2) : 1;
    int dhcp_s    = 2;   // .1은 게이트웨이(브릿지 IP) 예약
    int dhcp_e    = dhcp_s + max_host - 1;
    if (dhcp_e > 254) dhcp_e = 254;  // 브로드캐스트(.255) 보호
    if (dhcp_e < dhcp_s) dhcp_e = dhcp_s;
    gchar *dhcp_start = g_strdup_printf("%s.%d", base_ip, dhcp_s);
    gchar *dhcp_end   = g_strdup_printf("%s.%d", base_ip, dhcp_e);

    // 3. dnsmasq 전용 설정 파일 및 PID/Lease 파일 경로 설정
    gchar *conf_path  = g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-%s.conf",   bridge_name);
    gchar *pid_path   = g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-%s.pid",    bridge_name);
    gchar *lease_path = g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-%s.leases", bridge_name);

    // 4. 환경설정 파일 내용 동적 생성
    //    port=0           : DNS(53) 비활성 (dns_enabled=FALSE 시) — systemd-resolved 충돌 방지
    //    no-resolv        : /etc/resolv.conf 읽기 금지 (순수 DHCP 전용)
    //    bind-interfaces  : 특정 인터페이스에만 바인딩 (다른 dnsmasq와 포트 67 충돌 방지)
    //                       libvirt dnsmasq(virbr0)가 0.0.0.0:67 를 점유한 상태에서
    //                       bind-interfaces 없으면 "Permission denied" 발생
    //    interface=X      : bind-interfaces 와 함께 사용해야 함
    //    dhcp-range=..    : DHCP 할당 범위
    //    [E5] dns_enabled : server=<upstream> 지시자를 추가하여 DNS 포워더 활성화
    /* Validate upstream_dns to prevent config injection (newlines, semicolons, etc.) */
    const gchar *safe_dns = "8.8.8.8";
    if (dns_enabled && upstream_dns) {
        struct in_addr addr;
        if (inet_pton(AF_INET, upstream_dns, &addr) == 1) {
            safe_dns = upstream_dns;
        } else {
            g_warning("[DHCP] Invalid upstream_dns '%s' — falling back to 8.8.8.8", upstream_dns);
        }
    }
    gchar *dns_section = dns_enabled
        ? g_strdup_printf("server=%s\n", safe_dns)
        : g_strdup("port=0\nno-resolv\n");

    gchar *conf_content = g_strdup_printf(
        "%s"
        "bind-interfaces\n"
        "interface=%s\n"
        "dhcp-range=%s,%s,12h\n"
        "dhcp-leasefile=%s\n"
        "pid-file=%s\n",
        dns_section, bridge_name, dhcp_start, dhcp_end, lease_path, pid_path
    );
    g_free(dns_section);

    // 5. 설정 파일 디스크에 쓰기
    GError *write_err = NULL;
    if (!g_file_set_contents(conf_path, conf_content, -1, &write_err)) {
        g_propagate_error(error, write_err);
        goto cleanup;
    }

    // 6. 기존 데몬이 살아있다면 충돌을 막기 위해 종료
    // B4-W7 (Phase 4): pcv_spawn_fire → pcv_spawn_sync 변경으로 race 회피.
    // pkill은 실패해도 무시(비존재 PID → exit=1)하되, 완료는 보장한다.
    {
        const gchar *kill_argv[] = {"pkill", "-F", pid_path, NULL};
        pcv_spawn_sync(kill_argv, NULL, NULL, NULL);  /* best-effort, 결과 무시 */
    }

    // 7. dnsmasq 백그라운드 실행
    //    --conf-file=<path> : 등호 형식 필수 (공백 분리 시 일부 버전에서
    //                          "junk found in command line" 오류 발생)
    //    dnsmasq 는 자체 daemonize(double-fork) 하므로 부모 프로세스가
    //    exit(0) 으로 즉시 종료됨 → pcv_spawn_sync 가 성공 반환
    {
        gchar *conf_arg = g_strdup_printf("--conf-file=%s", conf_path);
        const gchar *dns_argv[] = {"dnsmasq", conf_arg, NULL};
        gchar *std_err = NULL;
        gboolean ok = pcv_spawn_sync(dns_argv, NULL, &std_err, error);
        g_free(conf_arg);
        if (!ok) {
            if (error && !*error)
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "dnsmasq failed: %s", std_err ? std_err : "unknown");
            g_free(std_err);
            goto cleanup;
        }
        g_free(std_err);
    }

cleanup:
    g_free(base_ip); g_free(dhcp_start); g_free(dhcp_end);
    g_free(conf_path); g_free(pid_path); g_free(lease_path); g_free(conf_content);

    /* [V18] error==NULL 로 호출될 수 있으므로 널 역참조 방어 (v6 변형과 동일). */
    return (error == NULL || *error == NULL);
}

/* ══════════════════════════════════════════════════════════════════════
 * IPv6 RA(Router Advertisement) + DHCPv6 지원
 *
 * [개요]
 *   IPv4 DHCP에 추가로 IPv6 주소 자동 설정을 지원한다.
 *   dnsmasq의 enable-ra + dhcp-range=<IPv6 범위> 를 사용하여
 *   SLAAC(Stateless Address Autoconfiguration) + DHCPv6(Stateful) 병용.
 *
 * [동작 방식]
 *   - enable-ra: RA(Router Advertisement) 패킷 전송으로 게스트가 프리픽스 학습
 *   - dhcp-range=<start>,<end>,<prefix>,<lease>: 상태 기반 DHCPv6 범위
 *   - dhcp-option=option6:dns-server,[<gw>]: IPv6 DNS 서버 광고
 *
 * [파라미터]
 *   bridge_name  : dnsmasq가 바인딩할 브릿지 인터페이스
 *   ipv6_prefix  : fd00:1::/64 형태 (ULA 권장, /64 필수)
 *
 * [하위 호환]
 *   이 함수는 독립적으로 호출 — 기존 IPv4 전용 경로 변경 없음.
 *   dnsmasq 설정 파일에 IPv6 지시자를 추가(append)한다.
 *
 * [전제조건]
 *   - 브릿지에 이미 IPv6 주소가 할당되어 있어야 RA가 동작
 *   - network_bridge_create()에서 IPv6 CIDR도 함께 할당
 * ══════════════════════════════════════════════════════════════════════ */
gboolean network_dhcp_start_v6(const gchar *bridge_name,
                                const gchar *ipv6_prefix,
                                GError     **error)
{
    if (!bridge_name || !ipv6_prefix) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "bridge_name and ipv6_prefix are required");
        return FALSE;
    }

    /* [V4] dnsmasq 설정 인젝션 방어 (defense-in-depth).
     * prefix_base 는 아래에서 dnsmasq .conf 에 그대로 기록되므로, 개행/공백/
     * 잘못된 문자셋을 통한 지시어 삽입(예: "fd00::/64\ndhcp-script=/x")을
     * 파일을 쓰기 전에 화이트리스트 검증으로 차단한다. 상위 핸들러에서 이미
     * 검증하지만, 이 공개 함수를 직접 호출하는 미래 경로도 안전해야 한다. */
    if (!pcv_validate_ipv6_prefix(ipv6_prefix)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Invalid ipv6_prefix (rejected by whitelist validator): %s",
                    ipv6_prefix);
        return FALSE;
    }

    /* ipv6_prefix 형식: "fd00:1::/64" → 프리픽스 "fd00:1::" + prefix_len "64" */
    const gchar *slash = g_strrstr(ipv6_prefix, "/");
    if (!slash) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "IPv6 prefix must include prefix length (e.g., fd00:1::/64)");
        return FALSE;
    }
    gint prefix_len = atoi(slash + 1);
    if (prefix_len < 48 || prefix_len > 128) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "IPv6 prefix length must be 48-128, got %d", prefix_len);
        return FALSE;
    }

    /* 프리픽스 부분 추출 ("::" 앞까지) */
    gchar *prefix_base = g_strndup(ipv6_prefix, (gsize)(slash - ipv6_prefix));

    /* IPv6 DHCP 범위: prefix::100 ~ prefix::1ff */
    gchar *v6_start = NULL;
    gchar *v6_end   = NULL;
    gchar *v6_gw    = NULL;

    /* "::"로 끝나는 경우 그대로 연결, 아니면 :: 추가 */
    if (g_str_has_suffix(prefix_base, "::")) {
        v6_start = g_strdup_printf("%s100", prefix_base);
        v6_end   = g_strdup_printf("%s1ff", prefix_base);
        v6_gw    = g_strdup_printf("%s1", prefix_base);
    } else if (g_str_has_suffix(prefix_base, ":")) {
        v6_start = g_strdup_printf("%s:100", prefix_base);
        v6_end   = g_strdup_printf("%s:1ff", prefix_base);
        v6_gw    = g_strdup_printf("%s:1", prefix_base);
    } else {
        v6_start = g_strdup_printf("%s::100", prefix_base);
        v6_end   = g_strdup_printf("%s::1ff", prefix_base);
        v6_gw    = g_strdup_printf("%s::1", prefix_base);
    }

    /* 기존 dnsmasq conf에 IPv6 지시자 추가 (append) */
    gchar *conf_path = g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-%s.conf", bridge_name);
    gchar *pid_path  = g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-%s.pid",  bridge_name);

    gchar *v6_config = g_strdup_printf(
        "\n# IPv6 RA + DHCPv6 (auto-generated)\n"
        "enable-ra\n"
        "dhcp-range=%s,%s,%d,12h\n"
        "dhcp-option=option6:dns-server,[%s]\n",
        v6_start, v6_end, prefix_len, v6_gw
    );

    /* 기존 설정이 있으면 append, 없으면 생성 */
    gchar *existing = NULL;
    gsize existing_len = 0;
    if (g_file_get_contents(conf_path, &existing, &existing_len, NULL)) {
        gchar *merged = g_strdup_printf("%s%s", existing, v6_config);
        GError *write_err = NULL;
        if (!g_file_set_contents(conf_path, merged, -1, &write_err)) {
            g_propagate_error(error, write_err);
            g_free(merged); g_free(existing);
            goto cleanup_v6;
        }
        g_free(merged); g_free(existing);
    } else {
        /* 기존 설정 없음 — IPv6 전용 설정 파일 생성 */
        gchar *lease_path = g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-%s.leases", bridge_name);
        gchar *full = g_strdup_printf(
            "port=0\nno-resolv\n"
            "bind-interfaces\n"
            "interface=%s\n"
            "dhcp-leasefile=%s\n"
            "pid-file=%s\n"
            "%s",
            bridge_name, lease_path, pid_path, v6_config
        );
        GError *write_err = NULL;
        if (!g_file_set_contents(conf_path, full, -1, &write_err)) {
            g_propagate_error(error, write_err);
            g_free(full); g_free(lease_path);
            goto cleanup_v6;
        }
        g_free(full); g_free(lease_path);
    }

    /* dnsmasq 재시작 (기존 프로세스 종료 → 재기동) — B4-M1: race 회피 */
    {
        const gchar *kill_argv[] = {"pkill", "-F", pid_path, NULL};
        pcv_spawn_sync(kill_argv, NULL, NULL, NULL);  /* best-effort */
    }
    {
        gchar *conf_arg = g_strdup_printf("--conf-file=%s", conf_path);
        const gchar *dns_argv[] = {"dnsmasq", conf_arg, NULL};
        gchar *std_err = NULL;
        gboolean ok = pcv_spawn_sync(dns_argv, NULL, &std_err, error);
        g_free(conf_arg);
        if (!ok) {
            if (error && !*error)
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "dnsmasq IPv6 restart failed: %s", std_err ? std_err : "unknown");
            g_free(std_err);
            goto cleanup_v6;
        }
        g_free(std_err);
    }

    g_message("[DHCP] IPv6 RA+DHCPv6 enabled on %s: %s-%s/%d",
              bridge_name, v6_start, v6_end, prefix_len);

cleanup_v6:
    g_free(prefix_base);
    g_free(v6_start); g_free(v6_end); g_free(v6_gw);
    g_free(v6_config);
    g_free(conf_path); g_free(pid_path);

    return (error == NULL || *error == NULL);
}