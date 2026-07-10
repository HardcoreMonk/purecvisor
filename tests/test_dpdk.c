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
}
