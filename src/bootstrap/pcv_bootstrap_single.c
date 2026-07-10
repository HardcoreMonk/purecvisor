#include "pcv_bootstrap.h"

#include "modules/network/ovn_manager.h"
#include "modules/network/ovs_overlay.h"
#include "modules/network/network_manager.h"   /* VP-1: network_bridge_create */
#include "modules/network/network_firewall.h"  /* VP-1: network_firewall_setup_nat */
#include "modules/network/network_dhcp.h"      /* VP-1: network_dhcp_start */
#include "utils/pcv_config.h"
#include "utils/pcv_validate.h"                 /* VP-1: PCV_NETWORK_RUNDIR */

/*
 * Single Edge 런타임 부트스트랩.
 *
 * [비전공자 설명]
 * 이 서버는 한 대의 물리 서버에서 VM을 운영하는 구성이므로, 클러스터 매니저,
 * 외부 스케줄러, 사이트 federation을 시작하지 않습니다. 대신 로컬 libvirt,
 * 로컬 OVN controller, 설정된 OVS overlay처럼 단일 서버 안에서 필요한
 * 네트워크만 준비합니다.
 *
 * [주니어 참고]
 * "skipped" 로그는 실패가 아니라 제품 범위입니다. 이 파일에서 no-op으로
 * 남긴 함수에 임시 클러스터 초기화를 넣으면 Single Edge 공개 범위가 깨지고,
 * health/RBAC/UI가 클러스터 기능을 사용할 수 있다고 오해합니다.
 */
const gchar *
pcv_bootstrap_get_daemon_binary_path(void)
{
    return "/usr/local/bin/purecvisorsd";
}

void
pcv_bootstrap_init_cluster_manager(void)
{
    g_message("[init] Single Edge mode — cluster manager bootstrap skipped");
}

void
pcv_bootstrap_init_scheduler_proxy(void)
{
    g_message("[init] Single Edge mode — scheduler/proxy bootstrap skipped");
}

void
pcv_bootstrap_init_federation(void)
{
    g_message("[init] Single Edge mode — federation bootstrap skipped");
}

void
pcv_bootstrap_init_runtime_network(void)
{
    /* Operator note:
     * 이 블록은 "VM에 기본으로 물려줄 사내 공유기(pcvnat0)를 데몬이 켜질 때마다
     * 점검·복구하는 절차"다. 브릿지/NAT/DHCP 중 하나라도 실패하면 데몬은 그대로
     * 뜨지만, 브릿지를 지정하지 않고 만든 새 VM은 IP를 못 받거나(DHCP 실패)
     * 인터넷이 안 되는(NAT 실패) 상태가 된다. 그 경우 기동 로그의 "[init] default"
     * WARN을 확인하고 데몬 재시작(자동 재시도) 또는 pcvctl network 명령으로
     * 수동 복구한다. 기존에 돌던 VM의 네트워크는 건드리지 않는다. */
    /* VP-1: 관리형 기본 NAT 네트워크(pcvnat0) 멱등 보장 — soft-fail.
     *
     * network_manager의 create 워커 관례(bridge→nat→dhcp)를 따르되, 데몬 재시작마다
     * 중복 리소스가 쌓이지 않도록 각 단계를 독립 마커로 가드한다:
     *   - 브릿지: /sys/class/net/<br> 존재 검사 (ip link add는 중복 시 실패)
     *   - NAT   : /run(tmpfs)의 nat-<br>.ok 마커 — setup_nat는 fire-and-forget
     *             append라 무조건 호출 시 재시작마다 규칙이 누적된다. 마커는
     *             성공 시에만 기록하므로 첫 시도 실패 후 데몬 재시작하면 재시도된다
     *             (브릿지 존재로 가드하면 NAT 실패가 다음 호스트 재부팅까지 고착 —
     *             리뷰 지적 반영).
     *   - DHCP  : dnsmasq-<br>.conf 존재 = 이번 부팅에서 이미 기동됨.
     * /run 마커들은 호스트 재부팅 시 nft 규칙·브릿지와 함께 소멸 — 수명 일치.
     * 실패는 모두 WARN 후 계속 — 부팅 블로킹 금지(M-7). */
    if (pcv_config_get_int("network", "default_ensure", 1)) {
        const gchar *def_br   = pcv_config_get_string("network", "default_bridge", "pcvnat0");
        const gchar *def_cidr = pcv_config_get_string("network", "default_subnet", "10.78.0.1/24");
        GError *net_err = NULL;

        gchar *sys_path = g_strdup_printf("/sys/class/net/%s", def_br);
        gboolean br_exists = g_file_test(sys_path, G_FILE_TEST_IS_DIR);
        g_free(sys_path);

        if (!br_exists) {
            if (network_bridge_create(def_br, def_cidr, 1500, &net_err)) {
                g_message("[init] default NAT bridge '%s' created (%s)", def_br, def_cidr);
                br_exists = TRUE;
                /* meta 기록 — network list의 mode 표시용 (VP-5 잔여) */
                pcv_network_meta_save(def_br, "nat", def_cidr);
            } else {
                g_warning("[init] default bridge create failed: %s",
                          net_err ? net_err->message : "unknown");
                g_clear_error(&net_err);
            }
        }

        if (br_exists) {
            gchar *nat_mark = g_strdup_printf(PCV_NETWORK_RUNDIR "/nat-%s.ok", def_br);
            if (!g_file_test(nat_mark, G_FILE_TEST_EXISTS)) {
                if (network_firewall_setup_nat(def_br, def_cidr, &net_err)) {
                    if (!g_file_set_contents(nat_mark, "ok\n", -1, NULL))
                        g_warning("[init] NAT marker write failed: %s", nat_mark);
                    g_message("[init] default NAT rules applied on '%s'", def_br);
                } else {
                    g_warning("[init] default NAT setup failed (재시작 시 재시도): %s",
                              net_err ? net_err->message : "unknown");
                    g_clear_error(&net_err);
                }
            }
            g_free(nat_mark);

            /* DHCP 게이트는 conf 파일이 아니라 **프로세스 생존**으로 판단한다.
             * 데몬이 스폰한 dnsmasq는 같은 systemd cgroup이라 데몬 재시작 때
             * 함께 죽을 수 있는데(KillMode 미조정 배포), conf(tmpfs 수명) 기준
             * 게이트는 그 죽음을 못 보고 재기동을 건너뛴다 (2026-07-06 VP-6
             * E2E 실측).
             *
             * 생존 검사는 kill(pid,0)이 아니라 /proc/<pid>/comm 을 읽는다:
             * privdrop(Stage 2)이 CAP_KILL 을 떨군 뒤라 nobody 소유 dnsmasq 에
             * signal-0 도 EPERM → 살아 있어도 "죽음" 오판으로 매 재시작마다
             * dnsmasq 를 불필요하게 재기동했다 (2026-07-06 bpftrace 실측).
             * comm 비교는 pid 재사용 오탐도 막는다. 죽어 있으면
             * network_dhcp_start_ex 가 self-healing(pkill+재작성)으로 재기동. */
            gchar *dhcp_pidf = g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-%s.pid", def_br);
            gboolean dhcp_up = FALSE;
            gchar *dhcp_pid_txt = NULL;
            if (g_file_get_contents(dhcp_pidf, &dhcp_pid_txt, NULL, NULL)) {
                gint64 dhcp_pid = g_ascii_strtoll(dhcp_pid_txt, NULL, 10);
                if (dhcp_pid > 0) {
                    gchar *comm_path = g_strdup_printf("/proc/%" G_GINT64_FORMAT "/comm",
                                                       dhcp_pid);
                    gchar *comm = NULL;
                    if (g_file_get_contents(comm_path, &comm, NULL, NULL)) {
                        dhcp_up = g_str_has_prefix(comm, "dnsmasq");
                        g_free(comm);
                    }
                    g_free(comm_path);
                }
                g_free(dhcp_pid_txt);
            }
            g_free(dhcp_pidf);
            if (!dhcp_up) {
                /* dns_enabled=TRUE: 게스트는 이 dnsmasq를 DNS 포워더로 받는다.
                 * port=0(기본, DHCP 전용)이면 게스트가 이름 해석을 못해 apt 등이
                 * 전부 실패한다 (2026-07-05 E2E 실측 — VP-6). bind-interfaces +
                 * interface=<br> 라 systemd-resolved(127.0.0.53)와 포트 충돌 없음. */
                if (network_dhcp_start_ex(def_br, def_cidr, TRUE, NULL, &net_err)) {
                    g_message("[init] default network DHCP+DNS started on '%s'", def_br);
                } else {
                    g_warning("[init] default DHCP start failed: %s",
                              net_err ? net_err->message : "unknown");
                    g_clear_error(&net_err);
                }
            }
        }
    }

    if (pcv_ovn_is_available()) {
        GError *ovn_local_err = NULL;
        if (pcv_ovn_single_prepare_local(&ovn_local_err)) {
            g_message("[init] OVN local controller prepared in Single Edge");
        } else {
            g_warning("[init] OVN local controller prepare failed: %s",
                      ovn_local_err ? ovn_local_err->message : "unknown");
            g_clear_error(&ovn_local_err);
        }
    }

    const gchar *ovl_br = pcv_config_get_string("overlay", "default_bridge", "");
    if (!ovl_br || !*ovl_br)
        return;

    GError *ovl_err = NULL;
    pcv_overlay_create(ovl_br,
                       pcv_config_get_int("overlay", "default_vni", 100),
                       pcv_config_get_string("overlay", "default_cidr", ""),
                       &ovl_err);
    if (ovl_err) {
        g_warning("Overlay auto-create: %s", ovl_err->message);
        g_error_free(ovl_err);
    }

    g_message("OVS overlay '%s' auto-provisioned (VNI=%d)",
              ovl_br,
              pcv_config_get_int("overlay", "default_vni", 100));
}

void
pcv_bootstrap_shutdown_cluster_stack(void)
{
}
