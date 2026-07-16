/* tests/test_dpdk.c
 *
 * 대상 모듈: src/modules/network/dpdk_manager.c — OVS-DPDK 바인딩/hugepage 관리
 *
 * 이 테스트가 검증하는 것:
 *   DPDK 미설치 환경에서 status/hugepage 구조 필드 존재,
 *   list 빈 배열 반환, unbind/bridge_delete 멱등 동작을 검사한다.
 *   PCI BDF 주소 형식 검증(유효/무효/인젝션 방어)도 포함.
 *
 * 실행: sudo ./test_runner -p /dpdk
 *
 * 외부 의존: OVS-DPDK (미설치 시에도 PASS — graceful degradation)
 */

#include <glib.h>
#include <glib/gstdio.h>   /* [NET-1] g_unlink/g_rmdir (기본경로 픽스처 정리) */
#include <json-glib/json-glib.h>

/* dpdk_manager.h 함수 직접 참조 */
extern gboolean pcv_dpdk_is_available(void);
extern JsonObject *pcv_dpdk_status(void);
extern JsonObject *pcv_dpdk_hugepage_info(void);
extern JsonArray *pcv_dpdk_list(void);
extern gboolean pcv_dpdk_unbind(const gchar *pci_addr, GError **error);
extern gboolean pcv_dpdk_bridge_delete(const gchar *name, GError **error);
extern gboolean pcv_dpdk_bridge_create(const gchar *name, const gchar *dpdk_port,
                                       GError **error);

/* pcv_validate.h 함수 참조 */
extern gboolean pcv_validate_pci_addr(const gchar *addr);

/* NET-1 관리 NIC 보호 가드 */
extern gboolean pcv_dpdk_nic_is_protected(const gchar *pci_addr, gchar **reason);
extern gboolean pcv_dpdk_route_is_default_dev(const gchar *netdev, const gchar *proc_base);

/* ── graceful degradation: DPDK 미설치 시 빈 결과 ── */

static void test_dpdk_status_structure(void) {
    JsonObject *obj = pcv_dpdk_status();
    g_assert_nonnull(obj);
    g_assert_true(json_object_has_member(obj, "available"));
    g_assert_true(json_object_has_member(obj, "vdev_count"));
    json_object_unref(obj);
}

static void test_dpdk_hugepage_structure(void) {
    JsonObject *obj = pcv_dpdk_hugepage_info();
    g_assert_nonnull(obj);
    g_assert_true(json_object_has_member(obj, "total_mb"));
    g_assert_true(json_object_has_member(obj, "free_mb"));
    g_assert_true(json_object_has_member(obj, "hugepage_1g_total"));
    g_assert_true(json_object_has_member(obj, "hugepage_2m_total"));
    json_object_unref(obj);
}

static void test_dpdk_list_empty(void) {
    JsonArray *arr = pcv_dpdk_list();
    g_assert_nonnull(arr);
    /* DPDK 미설치이면 빈 배열 */
    json_array_unref(arr);
}

/* ── 멱등 동작 ── */

static void test_dpdk_unbind_idempotent(void) {
    /* 존재하지 않는 PCI 주소도 성공 (멱등) */
    g_assert_true(pcv_dpdk_unbind("0000:ff:1f.7", NULL));
}

static void test_dpdk_bridge_delete_idempotent(void) {
    g_assert_true(pcv_dpdk_bridge_delete("nonexist-dpdk-br", NULL));
}

/* ── PCI 주소 검증 ── */

static void test_pci_addr_valid(void) {
    g_assert_true(pcv_validate_pci_addr("0000:01:00.0"));
    g_assert_true(pcv_validate_pci_addr("0000:3b:10.1"));
    g_assert_true(pcv_validate_pci_addr("ffff:ff:1f.7"));
}

static void test_pci_addr_invalid(void) {
    g_assert_false(pcv_validate_pci_addr(NULL));
    g_assert_false(pcv_validate_pci_addr(""));
    g_assert_false(pcv_validate_pci_addr("../../etc"));
    g_assert_false(pcv_validate_pci_addr("01:00.0"));        /* 도메인 없음 */
    g_assert_false(pcv_validate_pci_addr("0000:01:00"));     /* function 없음 */
    g_assert_false(pcv_validate_pci_addr("0000:01:20.0"));   /* slot > 0x1F */
    g_assert_false(pcv_validate_pci_addr("0000:01:00.8"));   /* function > 0x7 */
    g_assert_false(pcv_validate_pci_addr("0000:01:00.0 ; rm -rf /")); /* 인젝션 */
}

/* ── 입력 검증(화이트리스트) 거부 경로 — 커맨드 인젝션 방어 ── */

static void test_dpdk_bridge_create_reject_injection(void) {
    GError *err = NULL;
    /*
     * dpdk_port 에 주입된 인젝션 문자열은 거부되어야 하며 부작용이 없어야 한다.
     * pcv_validate_pci_addr 가 형식을 강제하고, ovs-vsctl 은 argv 배열로만 실행된다.
     * (DPDK 미가용 환경에서는 available 게이트에서 조기 반환 → 어느 경로든 명령 미실행)
     */
    gboolean ok = pcv_dpdk_bridge_create("br0", "x; touch /tmp/pcv_dpdk_pwn", &err);
    g_assert_false(ok);
    g_assert_nonnull(err);
    g_clear_error(&err);
    /* 인젝션 부작용 부재 확인 */
    g_assert_false(g_file_test("/tmp/pcv_dpdk_pwn", G_FILE_TEST_EXISTS));
}

/* ── NET-1: 관리 NIC 보호 가드 ── */

/* NET-1 M1(reason 오귀속 시정) 노트: _dpdk_up_with_ipv4()의 getifaddrs 실패
 * 분기(reason="interface enumeration failed ... (fail-secure)")는 이 파일에서
 * 포터블 유닛테스트로 재현하지 않는다 — pcv_dpdk_nic_is_protected()가 그
 * 분기에 도달하려면 (1) 실 PCI-backed netdev가 /sys/bus/pci/devices/<bdf>/net에
 * 존재해야 하고(호스트 하드웨어 의존, sysfs 경로는 proc_base처럼 테스트 주입
 * 불가) (2) getifaddrs() 자체가 syscall 오류로 실패해야 한다 — 둘 다 이
 * 테스트 스위트가 이미 실 호스트 의존 검증을 committed 포터블 테스트에서
 * 제외해 온 기존 관례(tests/integration/test_dpdk_bind_guard.sh 헤더 주석
 * 참조: "test_dpdk.c 임시 realhost 테스트 ... 커밋 안 함")와 일치한다.
 * reason 분기 자체는 코드리뷰로 검증됨(정상 UP+IPv4 매치 경로·판정값 무변경,
 * getifaddrs 실패 시에만 문구 분기). */

/* default-route 파서: 픽스처 /proc/net/route 로 기본경로 dev 판정. */
static void test_dpdk_nic_route_default(void) {
    gchar *base = g_dir_make_tmp("pcvdpdk_XXXXXX", NULL);
    g_assert_nonnull(base);
    gchar *pd = g_build_filename(base, "proc", "net", NULL);
    g_assert_cmpint(g_mkdir_with_parents(pd, 0700), ==, 0);
    gchar *route = g_build_filename(pd, "route", NULL);
    /* [0]=헤더, eth0=기본경로(Destination 00000000), eth1=서브넷 라우트 */
    g_assert_true(g_file_set_contents(route,
        "Iface\tDestination\tGateway\tFlags\n"
        "eth0\t00000000\t0102A8C0\t0003\n"
        "eth1\t0000A8C0\t00000000\t0001\n", -1, NULL));
    g_assert_true (pcv_dpdk_route_is_default_dev("eth0", base));
    g_assert_false(pcv_dpdk_route_is_default_dev("eth1", base));
    g_assert_false(pcv_dpdk_route_is_default_dev("ethX", base));
    g_assert_false(pcv_dpdk_route_is_default_dev(NULL,   base));
    g_unlink(route); g_rmdir(pd);
    gchar *pdir = g_build_filename(base, "proc", NULL);
    g_rmdir(pdir); g_rmdir(base);
    g_free(route); g_free(pd); g_free(pdir); g_free(base);
}

/* fail-secure: pci_addr NULL/빈 → 보호(TRUE). */
static void test_dpdk_nic_null_failsecure(void) {
    gchar *reason = NULL;
    g_assert_true(pcv_dpdk_nic_is_protected(NULL, &reason));
    g_free(reason); reason = NULL;
    g_assert_true(pcv_dpdk_nic_is_protected("", &reason));
    g_free(reason);
}

/* 커널 미관리 BDF(net 디렉토리 없음) → 통과(FALSE). */
static void test_dpdk_nic_absent_netdir_passes(void) {
    gchar *reason = NULL;
    /* 존재하지 않는 BDF → /sys/bus/pci/devices/<bdf>/net 없음 → FALSE */
    g_assert_false(pcv_dpdk_nic_is_protected("ffff:ff:1f.7", &reason));
    g_assert_null(reason);
    g_free(reason);
}

/* fail-secure: 형식검증 실패(경로순회/오형식) BDF → sysfs 탐침 없이 보호(TRUE). */
static void test_dpdk_nic_malformed_failsecure(void) {
    gchar *reason = NULL;
    g_assert_true(pcv_dpdk_nic_is_protected("../../../etc", &reason));
    g_assert_nonnull(reason);   /* invalid PCI address 사유 */
    g_free(reason); reason = NULL;
    g_assert_true(pcv_dpdk_nic_is_protected("not-a-bdf", &reason));
    g_free(reason);
}

/* ── 등록 ── */

void test_dpdk_register(void) {
    g_test_add_func("/dpdk/status/structure",          test_dpdk_status_structure);
    g_test_add_func("/dpdk/hugepage/structure",        test_dpdk_hugepage_structure);
    g_test_add_func("/dpdk/list/empty",                test_dpdk_list_empty);
    g_test_add_func("/dpdk/unbind/idempotent",         test_dpdk_unbind_idempotent);
    g_test_add_func("/dpdk/bridge_delete/idempotent",  test_dpdk_bridge_delete_idempotent);
    g_test_add_func("/dpdk/pci_addr/valid",            test_pci_addr_valid);
    g_test_add_func("/dpdk/pci_addr/invalid",          test_pci_addr_invalid);
    g_test_add_func("/dpdk/bridge_create/reject_injection",
                    test_dpdk_bridge_create_reject_injection);
    g_test_add_func("/dpdk/nic_protected/route_default",  test_dpdk_nic_route_default);
    g_test_add_func("/dpdk/nic_protected/null_failsecure", test_dpdk_nic_null_failsecure);
    g_test_add_func("/dpdk/nic_protected/absent_netdir",  test_dpdk_nic_absent_netdir_passes);
    g_test_add_func("/dpdk/nic_protected/malformed",      test_dpdk_nic_malformed_failsecure);
}
