/* tests/test_network.c
 *
 * 대상 모듈: src/modules/network/ — 네트워크 브릿지/모드 이름 검증
 *
 * 이 테스트가 검증하는 것:
 *   브릿지 이름의 유효/무효/경계값과 네트워크 모드 문자열(nat/isolated/routed/bridge)이
 *   pcv_validate_bridge_name 규칙을 만족하는지 검사한다.
 *   command injection, 경로 순회 문자열 거부를 포함.
 *
 * 실행: sudo ./test_runner -p /network
 *
 * 외부 의존: 없음 (순수 문자열 검증, 실제 브릿지 생성 없음)
 */

#include <glib.h>
#include <string.h>
#include "purecvisor/pcv_validate.h"
#include "modules/network/network_firewall_host.h"

/* ── bridge_name ─────────────────────────────────────── */

static void test_bridge_name_valid(void) {
    g_assert_true(pcv_validate_bridge_name("pcvbr0"));
    g_assert_true(pcv_validate_bridge_name("br-lan"));
    g_assert_true(pcv_validate_bridge_name("virbr0"));
    g_assert_true(pcv_validate_bridge_name("a"));
}

static void test_bridge_name_invalid(void) {
    g_assert_false(pcv_validate_bridge_name(NULL));
    g_assert_false(pcv_validate_bridge_name(""));
    g_assert_false(pcv_validate_bridge_name("br name"));       /* space */
    g_assert_false(pcv_validate_bridge_name("br;inject"));     /* semicolon */
    g_assert_false(pcv_validate_bridge_name("../etc"));        /* path traversal */
}

static void test_bridge_name_boundary(void) {
    /* PCV_MAX_BRIDGE_NAME = 16 */
    gchar buf[32];
    memset(buf, 'a', PCV_MAX_BRIDGE_NAME);
    buf[PCV_MAX_BRIDGE_NAME] = '\0';
    g_assert_true(pcv_validate_bridge_name(buf));

    /* 1 over → rejected */
    buf[PCV_MAX_BRIDGE_NAME] = 'x';
    buf[PCV_MAX_BRIDGE_NAME + 1] = '\0';
    g_assert_false(pcv_validate_bridge_name(buf));
}

/* ── network mode 문자열 검증 ──────────────────────── */

static void test_network_mode_strings(void) {
    /* 유효 모드 식별자가 bridge 이름 규칙과 호환되는지 */
    g_assert_true(pcv_validate_bridge_name("nat"));
    g_assert_true(pcv_validate_bridge_name("isolated"));
    g_assert_true(pcv_validate_bridge_name("routed"));
    g_assert_true(pcv_validate_bridge_name("bridge"));
}

/* ── [V4] ipv6_prefix → dnsmasq config injection 방어 ──────────────
 *
 * handle_network_create_request / network_dhcp_start_v6 / v6 gw-build 세 곳의
 * 방어 가드가 모두 pcv_validate_ipv6_prefix 에 의존한다. 해당 모듈들은
 * test_runner 링크 대상이 아니므로(Makefile), 가드가 신뢰하는 검증기 자체가
 * config-injection 문자열을 거부하는지를 최저 도달 지점에서 검증한다. */
static void test_ipv6_prefix_valid(void) {
    g_assert_true(pcv_validate_ipv6_prefix("fd00::/64"));
    g_assert_true(pcv_validate_ipv6_prefix("fd00:1::/64"));
    g_assert_true(pcv_validate_ipv6_prefix("2001:db8::/48"));
}

static void test_ipv6_prefix_injection(void) {
    /* 개행을 통한 dhcp-script 지시어 삽입 → root RCE 시도 (V4 핵심 벡터).
     * 마지막 '/' 뒤가 "64"라 prefix_len 검사만으로는 통과하지만, 개행/문자셋
     * 화이트리스트가 이를 차단해야 한다. */
    g_assert_false(pcv_validate_ipv6_prefix("fd00::/64\ndhcp-script=/etc/x/64"));
    g_assert_false(pcv_validate_ipv6_prefix("fd00::/64\ndhcp-script=/x"));
    g_assert_false(pcv_validate_ipv6_prefix("fd00::/64 dhcp-script=/x"));  /* space */
    g_assert_false(pcv_validate_ipv6_prefix("fd00::/64;evil"));            /* charset */
    g_assert_false(pcv_validate_ipv6_prefix("gg00::/64"));                 /* non-hex */
    g_assert_false(pcv_validate_ipv6_prefix("fd00::"));                    /* no /len */
    g_assert_false(pcv_validate_ipv6_prefix(""));
    g_assert_false(pcv_validate_ipv6_prefix(NULL));
}

/* ── [V8] network.mode_set cidr → nft injection 방어 ───────────────
 * mode_set 핸들러가 network.create 와 동일하게 pcv_validate_private_cidr 로
 * cidr 을 검증하는지 (검증기 계약) 확인한다. */
static void test_mode_set_cidr_validation(void) {
    g_assert_true(pcv_validate_private_cidr("10.10.10.1/24"));
    g_assert_true(pcv_validate_private_cidr("192.0.2.10/24"));
    g_assert_false(pcv_validate_private_cidr("0.0.0.0/0"));                /* wildcard */
    g_assert_false(pcv_validate_private_cidr("8.8.8.8/24"));              /* public */
    g_assert_false(pcv_validate_private_cidr("10.0.0.0/24; nft flush ruleset"));
    g_assert_false(pcv_validate_private_cidr(NULL));
}

/* ── [V10] mode_set/dhcp_toggle bridge → pkill -F path traversal 방어 ─
 * 두 핸들러가 경로 구성 전에 pcv_validate_bridge_name 으로 br 을 검증한다.
 * pid 경로("dnsmasq-<br>.pid") traversal 벡터가 거부되는지 확인. */
static void test_bridge_name_pkill_traversal(void) {
    g_assert_false(pcv_validate_bridge_name("../../etc/passwd"));
    g_assert_false(pcv_validate_bridge_name("br/../x"));
    g_assert_false(pcv_validate_bridge_name("br0/../../root"));
    g_assert_false(pcv_validate_bridge_name("br0\nx"));                    /* newline */
    g_assert_true(pcv_validate_bridge_name("pcvbr0"));
}

/* ── [V11] delete / bind_phys / ovs / qos 검증 갭 (defense-in-depth) ──
 * argv spawn 이전에 이름/IP 를 검증하는 데 쓰이는 검증기 계약 확인. */
static void test_v11_iface_ip_vm_validation(void) {
    /* iface (bind_phys iface, qos interface) */
    g_assert_true(pcv_validate_iface_name("eth0.100"));   /* VLAN subif */
    g_assert_true(pcv_validate_iface_name("vnet0"));
    g_assert_false(pcv_validate_iface_name("-eth0"));      /* option injection */
    g_assert_false(pcv_validate_iface_name("eth0;rm"));
    g_assert_false(pcv_validate_iface_name(NULL));

    /* remote_ip (ovs.vxlan.add) — CIDR 접미사/인젝션 불허 */
    g_assert_true(pcv_validate_ip_literal("192.0.2.10"));
    g_assert_true(pcv_validate_ip_literal("fd00::1"));
    g_assert_false(pcv_validate_ip_literal("10.0.0.0/24"));  /* CIDR not allowed */
    g_assert_false(pcv_validate_ip_literal("1.2.3.4; evil"));

    /* vm_name (qos.set vm_name → virsh domiflist argv) */
    g_assert_true(pcv_validate_vm_name("web-01"));
    g_assert_false(pcv_validate_vm_name("vm; rm -rf /"));
    g_assert_false(pcv_validate_vm_name("../vm"));
}

/* ── [V15] firewall teardown 따옴표 토큰 매칭 ──────────────────────
 * network_firewall_teardown 의 정적 매칭 로직은 test_runner 링크 대상이
 * 아니므로 직접 호출 불가. 대신 그 fix 가 사용하는 needle 구성("<br>")과
 * 매칭 계약을 동일하게 재현해, "br0" 가 "br0x" 규칙 줄과 충돌하지 않음을 고정.
 * teardown 진입부의 bridge_name 게이트는 test_bridge_name_* 가 커버한다. */
static void test_firewall_teardown_token_match(void) {
    const gchar *bridge = "br0";
    gchar *needle = g_strdup_printf("\"%s\"", bridge);   /* fix 와 동일: "br0" */

    /* nft -a list chain 출력 형태 (각 규칙에 handle 포함) */
    const gchar *line_self  = "iifname \"br0\" accept # handle 5";
    const gchar *line_other = "iifname \"br0x\" accept # handle 9";

    g_assert_nonnull(strstr(line_self, needle));    /* 자기 규칙은 매칭 */
    g_assert_null(strstr(line_other, needle));      /* br0x 규칙은 매칭 안 함 (fix) */

    /* 대조: 과거 bare-substring 로직은 br0x 줄까지 잘못 매칭했었다 */
    g_assert_nonnull(strstr(line_other, bridge));

    g_free(needle);
}

/* ── [VP-6] 호스트 방화벽 공존 plan 순수 함수 검증 ────────────────
 *
 * pcv_host_fw_plan 은 상태 enum 을 직접 주입받는 순수 함수다(파일/스폰 없음).
 * detect/integrate 는 실제 호스트 방화벽에 의존하므로 유닛 대상이 아니고,
 * "어떤 명령을 생성하는가"라는 계약만 여기서 고정한다. 이 계약이 깨지면
 * 게스트 포워딩/DHCP/DNS 경로를 뚫는 룰이 잘못 생성되어 게스트 인터넷이
 * 불통이 될 수 있다 (2026-07-05 E2E 실측 회귀). */

/* 배열 안에 needle 부분문자열을 포함한 요소가 있는지 */
static gboolean _plan_contains(GPtrArray *cmds, const gchar *needle) {
    for (guint i = 0; i < cmds->len; i++)
        if (strstr((const gchar *)g_ptr_array_index(cmds, i), needle))
            return TRUE;
    return FALSE;
}

static void test_host_fw_plan_ufw_add(void) {
    GPtrArray *cmds = pcv_host_fw_plan(PCV_HOST_FW_UFW, "br0", FALSE);
    /* 인터페이스 전체 allow 3종 (libvirt 동급) */
    g_assert_cmpuint(cmds->len, ==, 3);
    g_assert_true(_plan_contains(cmds, "ufw route allow in on br0"));
    g_assert_true(_plan_contains(cmds, "ufw route allow out on br0"));
    g_assert_true(_plan_contains(cmds, "ufw allow in on br0"));
    /* add 경로에는 delete 토큰이 없어야 한다 */
    g_assert_false(_plan_contains(cmds, "delete"));
    g_ptr_array_unref(cmds);
}

static void test_host_fw_plan_ufw_remove(void) {
    GPtrArray *cmds = pcv_host_fw_plan(PCV_HOST_FW_UFW, "br0", TRUE);
    g_assert_cmpuint(cmds->len, ==, 3);
    /* add 3종과 --force delete 로 대칭 */
    g_assert_true(_plan_contains(cmds, "ufw --force delete route allow in on br0"));
    g_assert_true(_plan_contains(cmds, "ufw --force delete route allow out on br0"));
    g_assert_true(_plan_contains(cmds, "ufw --force delete allow in on br0"));
    g_ptr_array_unref(cmds);
}

static void test_host_fw_plan_iptables_add(void) {
    GPtrArray *cmds = pcv_host_fw_plan(PCV_HOST_FW_IPTABLES_DROP, "br0", FALSE);
    /* FORWARD 2 + INPUT(67/53udp/53tcp) 3 = 5종 */
    g_assert_cmpuint(cmds->len, ==, 5);
    g_assert_true(_plan_contains(cmds, "iptables -I FORWARD -i br0 -j ACCEPT"));
    /* 응답 복귀: conntrack RELATED,ESTABLISHED */
    g_assert_true(_plan_contains(cmds, "conntrack"));
    g_assert_true(_plan_contains(cmds, "--ctstate RELATED,ESTABLISHED"));
    /* 호스트 dnsmasq 로의 DHCP/DNS */
    g_assert_true(_plan_contains(cmds, "INPUT -i br0 -p udp --dport 67"));
    g_assert_true(_plan_contains(cmds, "INPUT -i br0 -p udp --dport 53"));
    g_assert_true(_plan_contains(cmds, "INPUT -i br0 -p tcp --dport 53"));
    /* -C 가드는 plan 이 아니라 executor 책임 — plan 에 -C 는 없어야 한다 */
    g_assert_false(_plan_contains(cmds, "-C "));
    g_ptr_array_unref(cmds);
}

static void test_host_fw_plan_iptables_remove(void) {
    GPtrArray *cmds = pcv_host_fw_plan(PCV_HOST_FW_IPTABLES_DROP, "br0", TRUE);
    g_assert_cmpuint(cmds->len, ==, 5);
    /* 추가는 -I, 제거는 -D 대칭 */
    g_assert_true(_plan_contains(cmds, "iptables -D FORWARD -i br0 -j ACCEPT"));
    g_assert_false(_plan_contains(cmds, "-I "));
    g_ptr_array_unref(cmds);
}

static void test_host_fw_plan_open_empty(void) {
    /* 개입 불요 상태는 add/remove 모두 빈 목록 */
    GPtrArray *add = pcv_host_fw_plan(PCV_HOST_FW_OPEN, "br0", FALSE);
    GPtrArray *rem = pcv_host_fw_plan(PCV_HOST_FW_OPEN, "br0", TRUE);
    g_assert_cmpuint(add->len, ==, 0);
    g_assert_cmpuint(rem->len, ==, 0);
    g_ptr_array_unref(add);
    g_ptr_array_unref(rem);
}

static void test_host_fw_plan_firewalld_empty(void) {
    /* firewalld 는 실개입 비범위 → 빈 목록 (감지·경고는 integrate 책임) */
    GPtrArray *cmds = pcv_host_fw_plan(PCV_HOST_FW_FIREWALLD, "br0", FALSE);
    g_assert_cmpuint(cmds->len, ==, 0);
    g_ptr_array_unref(cmds);
}

/* ── 등록 ──────────────────────────────────────────── */

void test_network_register(void) {
    g_test_add_func("/network/bridge_name/valid",    test_bridge_name_valid);
    g_test_add_func("/network/bridge_name/invalid",  test_bridge_name_invalid);
    g_test_add_func("/network/bridge_name/boundary", test_bridge_name_boundary);
    g_test_add_func("/network/mode_strings",         test_network_mode_strings);
    /* 취약점 수정 회귀 테스트 (W4) */
    g_test_add_func("/network/v4/ipv6_prefix/valid",      test_ipv6_prefix_valid);
    g_test_add_func("/network/v4/ipv6_prefix/injection",  test_ipv6_prefix_injection);
    g_test_add_func("/network/v8/mode_set_cidr",          test_mode_set_cidr_validation);
    g_test_add_func("/network/v10/bridge_pkill_traversal",test_bridge_name_pkill_traversal);
    g_test_add_func("/network/v11/iface_ip_vm",           test_v11_iface_ip_vm_validation);
    g_test_add_func("/network/v15/teardown_token_match",  test_firewall_teardown_token_match);
    /* [VP-6] 호스트 방화벽 공존 plan 순수 함수 */
    g_test_add_func("/network/host_fw_plan_ufw_add",        test_host_fw_plan_ufw_add);
    g_test_add_func("/network/host_fw_plan_ufw_remove",     test_host_fw_plan_ufw_remove);
    g_test_add_func("/network/host_fw_plan_iptables_add",   test_host_fw_plan_iptables_add);
    g_test_add_func("/network/host_fw_plan_iptables_remove",test_host_fw_plan_iptables_remove);
    g_test_add_func("/network/host_fw_plan_open_empty",     test_host_fw_plan_open_empty);
    g_test_add_func("/network/host_fw_plan_firewalld_empty",test_host_fw_plan_firewalld_empty);
}
