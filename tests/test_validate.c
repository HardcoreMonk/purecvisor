/* tests/test_validate.c
 *
 * 대상 모듈: src/utils/pcv_validate.c — 모든 사용자 입력의 1차 방어선
 *
 * 이 테스트가 검증하는 것:
 *   pcv_validate_* 함수군의 정상/경계/거부 동작을 확인한다.
 *   Command Injection, 경로 순회 등 보안 위협이 검증 단계에서 차단되는지 검사.
 *
 * 실행 방법:
 *   make test                          # 전체 테스트 (이 파일 포함)
 *   sudo ./test_runner -p /validate    # 이 모듈만 실행
 *
 * 테스트 추가 방법:
 *   1. static void test_xxx(void) 함수 작성 (g_assert_* 사용)
 *   2. test_validate_register() 하단에 g_test_add_func("/validate/xxx", test_xxx) 추가
 *   3. make test 로 빌드+실행 확인
 *
 * 외부 의존: 없음 (순수 문자열 검증, 모킹 불필요)
 *
 * 커버리지:
 *   - vm_name    : 정상/경계값/금지문자/길이
 *   - snap_name  : 정상/경계값/NULL
 *   - bridge_name: 정상/초과 길이
 *   - iso_path   : 절대경로/상대경로/경로순회
 *   - memory_mb  : 최소/최대/범위 외
 *   - vcpu       : 정상/0/초과
 *   - disk_gb    : 정상/경계
 *   - create_params: 통합 검증 + GError 메시지
 *   - container_image: 형식 검증
 *   - exec_cmd   : NULL/빈문자열/정상
 */

#include <glib.h>
#include "purecvisor/pcv_validate.h"

/* ── vm_name ─────────────────────────────────────────── */

static void test_vm_name_valid(void) {
    g_assert_true(pcv_validate_vm_name("myvm"));
    g_assert_true(pcv_validate_vm_name("vm-01"));
    g_assert_true(pcv_validate_vm_name("VM_test"));
    g_assert_true(pcv_validate_vm_name("a"));           /* 최소 1자 */
}

static void test_vm_name_invalid(void) {
    g_assert_false(pcv_validate_vm_name(NULL));
    g_assert_false(pcv_validate_vm_name(""));           /* 빈 문자열 */
    g_assert_false(pcv_validate_vm_name("vm name"));    /* 공백 */
    g_assert_false(pcv_validate_vm_name("vm/name"));    /* 슬래시 */
    g_assert_false(pcv_validate_vm_name("vm;name"));    /* 세미콜론 */
    g_assert_false(pcv_validate_vm_name("../etc"));     /* 경로 순회 */
}

static void test_vm_name_boundary(void) {
    /* 정확히 PCV_MAX_VM_NAME 자 → OK */
    gchar *maxname = g_strnfill(PCV_MAX_VM_NAME, 'a');
    g_assert_true(pcv_validate_vm_name(maxname));
    g_free(maxname);

    /* PCV_MAX_VM_NAME + 1 자 → FAIL */
    gchar *overmax = g_strnfill(PCV_MAX_VM_NAME + 1, 'a');
    g_assert_false(pcv_validate_vm_name(overmax));
    g_free(overmax);
}

/* ── snap_name ───────────────────────────────────────── */

static void test_snap_name_valid(void) {
    g_assert_true(pcv_validate_snap_name("snap-2025"));
    g_assert_true(pcv_validate_snap_name("SNAP_01"));
}

static void test_snap_name_invalid(void) {
    g_assert_false(pcv_validate_snap_name(NULL));
    g_assert_false(pcv_validate_snap_name("snap name"));  /* 공백 */
}

/* ── bridge_name ─────────────────────────────────────── */

static void test_bridge_name_valid(void) {
    g_assert_true(pcv_validate_bridge_name("virbr0"));
    g_assert_true(pcv_validate_bridge_name("br-eth0"));
}

static void test_bridge_name_invalid(void) {
    g_assert_false(pcv_validate_bridge_name(NULL));
    /* PCV_MAX_BRIDGE_NAME + 1 자 */
    gchar *over = g_strnfill(PCV_MAX_BRIDGE_NAME + 1, 'x');
    g_assert_false(pcv_validate_bridge_name(over));
    g_free(over);
}

/* ── backup.replicate 원격 대상 ─────────────────────── */

static void test_remote_host_valid(void) {
    g_assert_true(pcv_validate_remote_host("192.0.2.20"));
    g_assert_true(pcv_validate_remote_host("pcv-node-1"));
    g_assert_true(pcv_validate_remote_host("node-1.example.internal"));
}

static void test_remote_host_invalid(void) {
    g_assert_false(pcv_validate_remote_host(NULL));
    g_assert_false(pcv_validate_remote_host(""));
    g_assert_false(pcv_validate_remote_host("host name"));
    g_assert_false(pcv_validate_remote_host("host;id"));
    g_assert_false(pcv_validate_remote_host("host$(id)"));
    g_assert_false(pcv_validate_remote_host("-leadingdash"));
    g_assert_false(pcv_validate_remote_host(".leadingdot"));
    g_assert_false(pcv_validate_remote_host("trailingdot."));
}

static void test_ssh_user_valid(void) {
    g_assert_true(pcv_validate_ssh_user("pcvdev"));
    g_assert_true(pcv_validate_ssh_user("pcv-dev_1"));
    g_assert_true(pcv_validate_ssh_user("user.name"));
}

static void test_ssh_user_invalid(void) {
    g_assert_false(pcv_validate_ssh_user(NULL));
    g_assert_false(pcv_validate_ssh_user(""));
    g_assert_false(pcv_validate_ssh_user("-oProxyCommand"));
    g_assert_false(pcv_validate_ssh_user("user@host"));
    g_assert_false(pcv_validate_ssh_user("../root"));
    g_assert_false(pcv_validate_ssh_user("user;id"));
}

/* ── iso_path ────────────────────────────────────────── */

static void test_iso_path_valid(void) {
    g_assert_true(pcv_validate_iso_path("/var/lib/images/ubuntu.iso"));
    g_assert_true(pcv_validate_iso_path("/tmp/test.iso"));
}

static void test_iso_path_invalid(void) {
    g_assert_false(pcv_validate_iso_path(NULL));
    g_assert_false(pcv_validate_iso_path("relative/path.iso")); /* 상대경로 */
    g_assert_false(pcv_validate_iso_path("/var/../../etc/passwd")); /* 경로 순회 */
    g_assert_false(pcv_validate_iso_path(""));
}

/* ── base_image (CMP-3 확장) ─────────────────────────── */

static void test_base_image_valid(void) {
    g_assert_true(pcv_validate_base_image_path("/var/lib/images/jammy.qcow2"));
    g_assert_true(pcv_validate_base_image_path("/pcvpool/images/base.img"));
    g_assert_true(pcv_validate_base_image_path("/tmp/cloud.raw"));
    g_assert_true(pcv_validate_base_image_path("/tmp/cloud.QCOW2")); /* 대소문자 무시 */
}

static void test_base_image_invalid(void) {
    g_assert_false(pcv_validate_base_image_path(NULL));
    g_assert_false(pcv_validate_base_image_path(""));
    g_assert_false(pcv_validate_base_image_path("/etc/shadow"));          /* 확장자 위반 = 임의파일 흡입 */
    g_assert_false(pcv_validate_base_image_path("relative/base.qcow2"));  /* 상대경로 */
    g_assert_false(pcv_validate_base_image_path("/pcvpool/../../etc/shadow.img")); /* 경로 순회 */
    g_assert_false(pcv_validate_base_image_path("/var/lib/images/base.iso")); /* iso는 디스크이미지 아님 */
}

/* ── 숫자 범위 ───────────────────────────────────────── */

static void test_memory_mb(void) {
    g_assert_true(pcv_validate_memory_mb(PCV_MIN_MEMORY_MB));       /* 최소 */
    g_assert_true(pcv_validate_memory_mb(4096));                    /* 일반 */
    g_assert_true(pcv_validate_memory_mb(PCV_MAX_MEMORY_MB));       /* 최대 */
    g_assert_false(pcv_validate_memory_mb(PCV_MIN_MEMORY_MB - 1));  /* 미만 */
    g_assert_false(pcv_validate_memory_mb(PCV_MAX_MEMORY_MB + 1));  /* 초과 */
    g_assert_false(pcv_validate_memory_mb(0));
    g_assert_false(pcv_validate_memory_mb(-1));
}

static void test_vcpu(void) {
    g_assert_true(pcv_validate_vcpu(PCV_MIN_VCPU));
    g_assert_true(pcv_validate_vcpu(4));
    g_assert_true(pcv_validate_vcpu(PCV_MAX_VCPU));
    g_assert_false(pcv_validate_vcpu(0));
    g_assert_false(pcv_validate_vcpu(PCV_MAX_VCPU + 1));
    g_assert_false(pcv_validate_vcpu(-1));
}

static void test_disk_gb(void) {
    g_assert_true(pcv_validate_disk_gb(PCV_MIN_DISK_GB));
    g_assert_true(pcv_validate_disk_gb(100));
    g_assert_true(pcv_validate_disk_gb(PCV_MAX_DISK_GB));
    g_assert_false(pcv_validate_disk_gb(0));
    g_assert_false(pcv_validate_disk_gb(PCV_MAX_DISK_GB + 1));
}

/* ── 통합 create_params ──────────────────────────────── */

static void test_create_params_valid(void) {
    GError *err = NULL;
    gboolean ok = pcv_validate_vm_create_params(
        "myvm", 2, 2048, 20, NULL, NULL, &err);
    g_assert_true(ok);
    g_assert_null(err);
}

static void test_create_params_bad_name(void) {
    GError *err = NULL;
    gboolean ok = pcv_validate_vm_create_params(
        "bad name!", 2, 2048, 20, NULL, NULL, &err);
    g_assert_false(ok);
    g_assert_nonnull(err);
    g_error_free(err);
}

static void test_create_params_bad_vcpu(void) {
    GError *err = NULL;
    gboolean ok = pcv_validate_vm_create_params(
        "myvm", 0, 2048, 20, NULL, NULL, &err);
    g_assert_false(ok);
    g_assert_nonnull(err);
    g_error_free(err);
}

static void test_create_params_bad_mem(void) {
    GError *err = NULL;
    gboolean ok = pcv_validate_vm_create_params(
        "myvm", 2, 64, 20, NULL, NULL, &err);  /* 64 < MIN_MEMORY_MB */
    g_assert_false(ok);
    g_assert_nonnull(err);
    g_error_free(err);
}

/* ── container_image ─────────────────────────────────── */

static void test_container_image_valid(void) {
    g_assert_true(pcv_validate_container_image("ubuntu:22.04"));
    g_assert_true(pcv_validate_container_image("alpine:3.18"));
    g_assert_true(pcv_validate_container_image("debian:bookworm"));
}

static void test_container_image_invalid(void) {
    g_assert_false(pcv_validate_container_image(NULL));
    g_assert_false(pcv_validate_container_image("ubuntu"));      /* 콜론 없음 */
    g_assert_false(pcv_validate_container_image(":22.04"));      /* distro 없음 */
    g_assert_false(pcv_validate_container_image("Ubuntu:22.04")); /* 대문자 */
}

/* ── exec_cmd ────────────────────────────────────────── */

static void test_exec_cmd_valid(void) {
    g_assert_true(pcv_validate_exec_cmd("ls -la"));
    g_assert_true(pcv_validate_exec_cmd("/usr/bin/python3 script.py"));
}

static void test_exec_cmd_invalid(void) {
    g_assert_false(pcv_validate_exec_cmd(NULL));
    g_assert_false(pcv_validate_exec_cmd(""));
}

/* ── iface_name (VLAN subif 허용) ────────────────────── */

static void test_iface_name_valid(void) {
    g_assert_true(pcv_validate_iface_name("eth0"));
    g_assert_true(pcv_validate_iface_name("eth0.100"));   /* VLAN subif */
    g_assert_true(pcv_validate_iface_name("br-lan"));
    g_assert_true(pcv_validate_iface_name("en_p0"));
    g_assert_true(pcv_validate_iface_name("a"));          /* 최소 1자 */
    /* 정확히 PCV_MAX_IFACE_NAME(15)자 → OK */
    gchar *maxname = g_strnfill(PCV_MAX_IFACE_NAME, 'a');
    g_assert_true(pcv_validate_iface_name(maxname));
    g_free(maxname);
}

static void test_iface_name_invalid(void) {
    g_assert_false(pcv_validate_iface_name(NULL));
    g_assert_false(pcv_validate_iface_name(""));           /* 빈 문자열 */
    g_assert_false(pcv_validate_iface_name("-x"));         /* 선행 '-' (option injection) */
    g_assert_false(pcv_validate_iface_name("eth0;rm"));    /* 셸 메타문자 */
    g_assert_false(pcv_validate_iface_name("eth 0"));      /* 공백 */
    /* 16자 → 길이 초과 (IFNAMSIZ-1=15) */
    gchar *over = g_strnfill(PCV_MAX_IFACE_NAME + 1, 'a');
    g_assert_false(pcv_validate_iface_name(over));
    g_free(over);
}

/* ── mac ─────────────────────────────────────────────── */

static void test_mac_valid(void) {
    g_assert_true(pcv_validate_mac("52:54:00:00:00:01"));
    g_assert_true(pcv_validate_mac("AA:BB:CC:dd:ee:ff"));  /* 대소문자 hex 혼용 */
}

static void test_mac_invalid(void) {
    g_assert_false(pcv_validate_mac(NULL));
    g_assert_false(pcv_validate_mac(""));
    g_assert_false(pcv_validate_mac("52:54:00:00:00"));            /* 그룹 부족 (14자) */
    g_assert_false(pcv_validate_mac("gg:54:00:00:00:01"));         /* 비16진수 */
    g_assert_false(pcv_validate_mac("52:54:00:00:00:01 vlan 4095"));/* 추가 토큰 */
    g_assert_false(pcv_validate_mac("52540000001"));               /* 콜론 없음 */
    g_assert_false(pcv_validate_mac("52-54-00-00-00-01"));         /* 잘못된 구분자 */
}

/* ── ip_literal (IPv4/IPv6, CIDR 불허) ───────────────── */

static void test_ip_literal_valid(void) {
    g_assert_true(pcv_validate_ip_literal("10.0.0.1"));
    g_assert_true(pcv_validate_ip_literal("fd00::1"));
    g_assert_true(pcv_validate_ip_literal("255.255.255.255"));
    g_assert_true(pcv_validate_ip_literal("::1"));
}

static void test_ip_literal_invalid(void) {
    g_assert_false(pcv_validate_ip_literal(NULL));
    g_assert_false(pcv_validate_ip_literal(""));
    g_assert_false(pcv_validate_ip_literal("10.0.0.1/24"));  /* CIDR 접미사 불허 */
    g_assert_false(pcv_validate_ip_literal("10.0.0.256"));   /* 옥텟 범위 초과 */
    g_assert_false(pcv_validate_ip_literal("not-an-ip"));
}

/* ── ipv6_prefix (dnsmasq injection 방어) ────────────── */

static void test_ipv6_prefix_valid(void) {
    g_assert_true(pcv_validate_ipv6_prefix("fd00:1::/64"));
    g_assert_true(pcv_validate_ipv6_prefix("::/0"));         /* 경계: len 0 */
    g_assert_true(pcv_validate_ipv6_prefix("fd00::1/128"));  /* 경계: len 128 */
}

static void test_ipv6_prefix_invalid(void) {
    g_assert_false(pcv_validate_ipv6_prefix(NULL));
    g_assert_false(pcv_validate_ipv6_prefix(""));
    /* dnsmasq config injection — 개행으로 지시어 추가 시도 */
    g_assert_false(pcv_validate_ipv6_prefix("fd00::/64\ndhcp-script=/tmp/x"));
    g_assert_false(pcv_validate_ipv6_prefix("fd00::/129"));  /* len 초과 */
    g_assert_false(pcv_validate_ipv6_prefix("fd00::"));      /* '/' 없음 */
    g_assert_false(pcv_validate_ipv6_prefix("fd00::/64 foo"));/* 공백 + 추가 토큰 */
    g_assert_false(pcv_validate_ipv6_prefix("gg::/64"));     /* 비16진수 주소부 */
}

/* ── l4_proto ────────────────────────────────────────── */

static void test_l4_proto_valid(void) {
    g_assert_true(pcv_validate_l4_proto("tcp"));
    g_assert_true(pcv_validate_l4_proto("udp"));
    g_assert_true(pcv_validate_l4_proto("icmp"));
}

static void test_l4_proto_invalid(void) {
    g_assert_false(pcv_validate_l4_proto(NULL));
    g_assert_false(pcv_validate_l4_proto(""));
    g_assert_false(pcv_validate_l4_proto("TCP"));       /* 대소문자 구분 */
    g_assert_false(pcv_validate_l4_proto("tcp; drop")); /* 인젝션 시도 */
    g_assert_false(pcv_validate_l4_proto("sctp"));      /* 허용 목록 외 */
}

/* ── 등록 함수 ───────────────────────────────────────── */

void test_validate_register(void) {
    /* vm_name */
    g_test_add_func("/validate/vm_name/valid",    test_vm_name_valid);
    g_test_add_func("/validate/vm_name/invalid",  test_vm_name_invalid);
    g_test_add_func("/validate/vm_name/boundary", test_vm_name_boundary);
    /* snap_name */
    g_test_add_func("/validate/snap_name/valid",   test_snap_name_valid);
    g_test_add_func("/validate/snap_name/invalid", test_snap_name_invalid);
    /* bridge */
    g_test_add_func("/validate/bridge/valid",   test_bridge_name_valid);
    g_test_add_func("/validate/bridge/invalid", test_bridge_name_invalid);
    /* backup.replicate remote identity */
    g_test_add_func("/validate/remote_host/valid", test_remote_host_valid);
    g_test_add_func("/validate/remote_host/invalid", test_remote_host_invalid);
    g_test_add_func("/validate/ssh_user/valid", test_ssh_user_valid);
    g_test_add_func("/validate/ssh_user/invalid", test_ssh_user_invalid);
    /* iso */
    g_test_add_func("/validate/iso_path/valid",   test_iso_path_valid);
    g_test_add_func("/validate/iso_path/invalid", test_iso_path_invalid);
    g_test_add_func("/validate/base_image/valid",   test_base_image_valid);
    g_test_add_func("/validate/base_image/invalid", test_base_image_invalid);
    /* 숫자 범위 */
    g_test_add_func("/validate/memory_mb", test_memory_mb);
    g_test_add_func("/validate/vcpu",      test_vcpu);
    g_test_add_func("/validate/disk_gb",   test_disk_gb);
    /* create_params */
    g_test_add_func("/validate/create_params/valid",    test_create_params_valid);
    g_test_add_func("/validate/create_params/bad_name", test_create_params_bad_name);
    g_test_add_func("/validate/create_params/bad_vcpu", test_create_params_bad_vcpu);
    g_test_add_func("/validate/create_params/bad_mem",  test_create_params_bad_mem);
    /* container */
    g_test_add_func("/validate/container_image/valid",   test_container_image_valid);
    g_test_add_func("/validate/container_image/invalid", test_container_image_invalid);
    /* exec_cmd */
    g_test_add_func("/validate/exec_cmd/valid",   test_exec_cmd_valid);
    g_test_add_func("/validate/exec_cmd/invalid", test_exec_cmd_invalid);
    /* iface_name (VLAN subif) */
    g_test_add_func("/validate/iface_name/valid",   test_iface_name_valid);
    g_test_add_func("/validate/iface_name/invalid", test_iface_name_invalid);
    /* mac */
    g_test_add_func("/validate/mac/valid",   test_mac_valid);
    g_test_add_func("/validate/mac/invalid", test_mac_invalid);
    /* ip_literal */
    g_test_add_func("/validate/ip_literal/valid",   test_ip_literal_valid);
    g_test_add_func("/validate/ip_literal/invalid", test_ip_literal_invalid);
    /* ipv6_prefix (dnsmasq injection 방어) */
    g_test_add_func("/validate/ipv6_prefix/valid",   test_ipv6_prefix_valid);
    g_test_add_func("/validate/ipv6_prefix/invalid", test_ipv6_prefix_invalid);
    /* l4_proto */
    g_test_add_func("/validate/l4_proto/valid",   test_l4_proto_valid);
    g_test_add_func("/validate/l4_proto/invalid", test_l4_proto_invalid);
}
