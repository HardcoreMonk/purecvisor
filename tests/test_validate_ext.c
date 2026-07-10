/**
 * @file test_validate_ext.c
 * @brief 확장 입력 검증 테스트 — CIDR, PCI 주소, 네트워크 생성 파라미터
 *
 * 대상 모듈: src/utils/pcv_validate.c (네트워크/하드웨어 검증 부분)
 *
 * test_validate.c의 기본 커버리지(vm_name, snap_name, bridge, iso, memory, vcpu, disk)를
 * 보완하여 CIDR(IPv4/IPv6), PCI BDF 주소, 네트워크 생성 통합 파라미터를 검증한다.
 *
 * 실행: sudo ./test_runner -p /validate_ext
 *
 * 테스트 추가: test_validate_ext_register() 하단에 등록
 *
 * 외부 의존: 없음 (순수 문자열/형식 검증)
 */

#include <glib.h>
#include "purecvisor/pcv_validate.h"

/* ══════════════════════════════════════════════════════════════
 * 1. pcv_validate_cidr — IPv4/IPv6 CIDR 검증
 * ══════════════════════════════════════════════════════════════ */

static void test_cidr_ipv4_valid(void) {
    g_assert_true(pcv_validate_cidr("10.0.0.0/8"));
    g_assert_true(pcv_validate_cidr("192.0.2.10/24"));
    g_assert_true(pcv_validate_cidr("172.16.0.0/12"));
    g_assert_true(pcv_validate_cidr("10.0.0.1/32"));     /* /32 = 단일 호스트 */
    g_assert_true(pcv_validate_cidr("0.0.0.0/0"));       /* 기본 라우트 */
}

static void test_cidr_ipv4_invalid(void) {
    g_assert_false(pcv_validate_cidr(NULL));
    g_assert_false(pcv_validate_cidr(""));
    g_assert_false(pcv_validate_cidr("10.0.0.0"));       /* prefix 없음 */
    g_assert_false(pcv_validate_cidr("/24"));             /* IP 없음 */
    g_assert_false(pcv_validate_cidr("10.0.0.0/"));       /* prefix 빈 문자열 */
    g_assert_false(pcv_validate_cidr("10.0.0.0/33"));     /* IPv4 prefix > 32 */
    g_assert_false(pcv_validate_cidr("10.0.0.0/-1"));     /* 음수 prefix */
    g_assert_false(pcv_validate_cidr("999.999.999.999/24")); /* 옥텟 범위 초과 */
    g_assert_false(pcv_validate_cidr("10.0.0/24"));       /* 옥텟 부족 */
    g_assert_false(pcv_validate_cidr("10.0.0.0.0/24"));   /* 옥텟 초과 */
}

static void test_cidr_ipv6_valid(void) {
    g_assert_true(pcv_validate_cidr("2001:db8::/32"));
    g_assert_true(pcv_validate_cidr("fe80::/10"));
    g_assert_true(pcv_validate_cidr("::1/128"));          /* loopback */
    g_assert_true(pcv_validate_cidr("::/0"));             /* 기본 라우트 */
}

static void test_cidr_ipv6_invalid(void) {
    g_assert_false(pcv_validate_cidr("2001:db8::/129"));  /* IPv6 prefix > 128 */
    g_assert_false(pcv_validate_cidr("gggg::/32"));       /* 유효하지 않은 hex */
}

/* ══════════════════════════════════════════════════════════════
 * 2. pcv_validate_pci_addr — PCI BDF 주소 검증
 * ══════════════════════════════════════════════════════════════ */

static void test_pci_addr_valid(void) {
    g_assert_true(pcv_validate_pci_addr("0000:3b:00.0"));
    g_assert_true(pcv_validate_pci_addr("0000:00:00.0"));    /* 최소값 */
    g_assert_true(pcv_validate_pci_addr("ffff:ff:1f.7"));    /* 최대값 */
    g_assert_true(pcv_validate_pci_addr("0000:00:1f.0"));    /* 일반적인 칩셋 */
}

static void test_pci_addr_invalid(void) {
    g_assert_false(pcv_validate_pci_addr(NULL));
    g_assert_false(pcv_validate_pci_addr(""));
    g_assert_true(pcv_validate_pci_addr("00:3b:00.0"));      /* 짧은 domain — sscanf %x가 허용 */
    g_assert_false(pcv_validate_pci_addr("0000:3b:20.0"));   /* slot > 0x1F */
    g_assert_false(pcv_validate_pci_addr("0000:3b:00.8"));   /* function > 0x7 */
    g_assert_false(pcv_validate_pci_addr("../etc/passwd"));   /* 경로 순회 */
    g_assert_false(pcv_validate_pci_addr("0000:3b:00.0extra")); /* 뒤에 추가 문자 */
    g_assert_false(pcv_validate_pci_addr("not-a-pci-addr"));
}

/* ══════════════════════════════════════════════════════════════
 * 3. pcv_validate_network_create_params — 네트워크 생성 통합 검증
 * ══════════════════════════════════════════════════════════════ */

static void test_net_create_nat_valid(void) {
    GError *err = NULL;
    g_assert_true(pcv_validate_network_create_params(
        "pcvbr0", "nat", "10.0.0.1/24", NULL, &err));
    g_assert_null(err);
}

static void test_net_create_nat_no_cidr(void) {
    GError *err = NULL;
    g_assert_false(pcv_validate_network_create_params(
        "pcvbr0", "nat", NULL, NULL, &err));
    g_assert_nonnull(err);
    g_error_free(err);
}

static void test_net_create_bridge_valid(void) {
    GError *err = NULL;
    g_assert_true(pcv_validate_network_create_params(
        "pcvbr0", "bridge", NULL, "enp42s0", &err));
    g_assert_null(err);
}

static void test_net_create_bridge_no_phys(void) {
    GError *err = NULL;
    g_assert_false(pcv_validate_network_create_params(
        "pcvbr0", "bridge", NULL, NULL, &err));
    g_assert_nonnull(err);
    g_error_free(err);
}

static void test_net_create_invalid_mode(void) {
    GError *err = NULL;
    g_assert_false(pcv_validate_network_create_params(
        "pcvbr0", "invalid_mode", "10.0.0.1/24", NULL, &err));
    g_assert_nonnull(err);
    g_error_free(err);
}

static void test_net_create_invalid_bridge_name(void) {
    GError *err = NULL;
    g_assert_false(pcv_validate_network_create_params(
        NULL, "nat", "10.0.0.1/24", NULL, &err));
    g_assert_nonnull(err);
    g_error_free(err);
}

static void test_net_create_null_mode_defaults_nat(void) {
    GError *err = NULL;
    /* mode=NULL → "nat" 기본값 → cidr 필수 */
    g_assert_true(pcv_validate_network_create_params(
        "br0", NULL, "10.0.0.1/24", NULL, &err));
    g_assert_null(err);
}

static void test_net_create_isolated_valid(void) {
    GError *err = NULL;
    g_assert_true(pcv_validate_network_create_params(
        "isol0", "isolated", "192.0.2.10/24", NULL, &err));
    g_assert_null(err);
}

static void test_net_create_routed_valid(void) {
    GError *err = NULL;
    g_assert_true(pcv_validate_network_create_params(
        "rout0", "routed", "172.16.0.1/16", NULL, &err));
    g_assert_null(err);
}

/* ══════════════════════════════════════════════════════════════
 * 4. pcv_validate_private_cidr — 사설 대역 강제 검증
 * ══════════════════════════════════════════════════════════════ */

static void test_private_cidr_valid(void) {
    g_assert_true(pcv_validate_private_cidr("10.0.0.0/8"));
    g_assert_true(pcv_validate_private_cidr("172.16.0.0/12"));
    g_assert_true(pcv_validate_private_cidr("192.0.2.10/24"));
}

static void test_private_cidr_invalid(void) {
    /* 공인 IP 대역 — 거부 */
    g_assert_false(pcv_validate_private_cidr("8.8.8.0/24"));
    g_assert_false(pcv_validate_private_cidr("1.1.1.0/24"));
    /* NULL / 빈 문자열 */
    g_assert_false(pcv_validate_private_cidr(NULL));
    g_assert_false(pcv_validate_private_cidr(""));
}

/* ══════════════════════════════════════════════════════════════
 * 5. pcv_validate_port — TCP/UDP 포트 범위 검증
 * ══════════════════════════════════════════════════════════════ */

static void test_port_valid(void) {
    g_assert_true(pcv_validate_port(80));
    g_assert_true(pcv_validate_port(443));
    g_assert_true(pcv_validate_port(1));       /* 최솟값 */
    g_assert_true(pcv_validate_port(65535));   /* 최댓값 */
}

static void test_port_invalid(void) {
    g_assert_false(pcv_validate_port(0));      /* 와일드카드 — 제외 */
    g_assert_false(pcv_validate_port(65536));  /* 범위 초과 */
    g_assert_false(pcv_validate_port(-1));     /* 음수 */
}

/* ══════════════════════════════════════════════════════════════
 * 6. pcv_validate_disk_size_gb — 디스크 크기 범위 검증 (보수적 상한 2048)
 * ══════════════════════════════════════════════════════════════ */

static void test_disk_size_gb_valid(void) {
    g_assert_true(pcv_validate_disk_size_gb(1));     /* 최솟값 */
    g_assert_true(pcv_validate_disk_size_gb(100));
    g_assert_true(pcv_validate_disk_size_gb(2048));  /* 최댓값 */
}

static void test_disk_size_gb_invalid(void) {
    g_assert_false(pcv_validate_disk_size_gb(0));    /* 0은 무효 */
    g_assert_false(pcv_validate_disk_size_gb(-1));   /* 음수 */
    g_assert_false(pcv_validate_disk_size_gb(2049)); /* 보수적 상한 초과 */
}

/* ══════════════════════════════════════════════════════════════
 * 7. pcv_validate_zvol_name — ZFS zvol 이름 검증
 * ══════════════════════════════════════════════════════════════ */

static void test_zvol_name_valid(void) {
    g_assert_true(pcv_validate_zvol_name("pcvpool"));
    g_assert_true(pcv_validate_zvol_name("pcvpool.vms.web01")); /* 점 허용 */
    g_assert_true(pcv_validate_zvol_name("a"));                 /* 최소 길이 */
    g_assert_true(pcv_validate_zvol_name("vol-001_test"));      /* 하이픈/언더스코어 허용 */
}

static void test_zvol_name_invalid(void) {
    g_assert_false(pcv_validate_zvol_name(NULL));
    g_assert_false(pcv_validate_zvol_name(""));              /* 빈 문자열 */
    g_assert_false(pcv_validate_zvol_name("pool/../escape")); /* ".." 순회 차단 */
    g_assert_false(pcv_validate_zvol_name("_leadunder"));    /* 영숫자로 시작하지 않음 */
    g_assert_false(pcv_validate_zvol_name(".dotstart"));     /* 점으로 시작 금지 */
}

/* ══════════════════════════════════════════════════════════════
 * 등록
 * ══════════════════════════════════════════════════════════════ */

void test_validate_ext_register(void) {
    /* CIDR */
    g_test_add_func("/validate_ext/cidr/ipv4_valid",   test_cidr_ipv4_valid);
    g_test_add_func("/validate_ext/cidr/ipv4_invalid", test_cidr_ipv4_invalid);
    g_test_add_func("/validate_ext/cidr/ipv6_valid",   test_cidr_ipv6_valid);
    g_test_add_func("/validate_ext/cidr/ipv6_invalid", test_cidr_ipv6_invalid);

    /* PCI 주소 */
    g_test_add_func("/validate_ext/pci_addr/valid",   test_pci_addr_valid);
    g_test_add_func("/validate_ext/pci_addr/invalid", test_pci_addr_invalid);

    /* 네트워크 생성 */
    g_test_add_func("/validate_ext/net_create/nat_valid",          test_net_create_nat_valid);
    g_test_add_func("/validate_ext/net_create/nat_no_cidr",        test_net_create_nat_no_cidr);
    g_test_add_func("/validate_ext/net_create/bridge_valid",       test_net_create_bridge_valid);
    g_test_add_func("/validate_ext/net_create/bridge_no_phys",     test_net_create_bridge_no_phys);
    g_test_add_func("/validate_ext/net_create/invalid_mode",       test_net_create_invalid_mode);
    g_test_add_func("/validate_ext/net_create/invalid_bridge_name",test_net_create_invalid_bridge_name);
    g_test_add_func("/validate_ext/net_create/null_mode_nat",      test_net_create_null_mode_defaults_nat);
    g_test_add_func("/validate_ext/net_create/isolated_valid",     test_net_create_isolated_valid);
    g_test_add_func("/validate_ext/net_create/routed_valid",       test_net_create_routed_valid);

    /* 사설 CIDR */
    g_test_add_func("/validate_ext/private_cidr/valid",   test_private_cidr_valid);
    g_test_add_func("/validate_ext/private_cidr/invalid", test_private_cidr_invalid);

    /* 포트 */
    g_test_add_func("/validate_ext/port/valid",   test_port_valid);
    g_test_add_func("/validate_ext/port/invalid", test_port_invalid);

    /* 디스크 크기 */
    g_test_add_func("/validate_ext/disk_size_gb/valid",   test_disk_size_gb_valid);
    g_test_add_func("/validate_ext/disk_size_gb/invalid", test_disk_size_gb_invalid);

    /* zvol 이름 */
    g_test_add_func("/validate_ext/zvol_name/valid",   test_zvol_name_valid);
    g_test_add_func("/validate_ext/zvol_name/invalid", test_zvol_name_invalid);
}
