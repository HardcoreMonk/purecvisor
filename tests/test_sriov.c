/* tests/test_sriov.c
 *
 * 대상 모듈: src/modules/network/sriov_manager.c — SR-IOV VF 관리
 *
 * 이 테스트가 검증하는 것:
 *   SR-IOV 미지원 환경에서 status 구조 필드 존재,
 *   list 빈 배열 반환, disable 멱등 동작, 미존재 PF의 VF PCI 주소
 *   NULL 반환을 검사한다.
 *
 * 실행: sudo ./test_runner -p /sriov
 *
 * 외부 의존: SR-IOV NIC + sysfs (미지원 시에도 PASS — graceful degradation)
 */

#include <glib.h>
#include <json-glib/json-glib.h>

/* sriov_manager.h 함수 직접 참조 */
extern JsonObject *pcv_sriov_status(void);
extern JsonArray *pcv_sriov_list(const gchar *pf);
extern gboolean pcv_sriov_disable(const gchar *pf, GError **error);
extern gchar *pcv_sriov_vf_pci_addr(const gchar *pf, gint vf_index);
extern gboolean pcv_sriov_set(const gchar *pf, gint vf_index,
                              const gchar *mac, gint vlan,
                              gint spoofchk, GError **error);
extern gboolean pcv_sriov_detach_vm(const gchar *vm_name,
                                    const gchar *pci_addr, GError **error);

/* ── graceful degradation: SR-IOV 미지원 시 ── */

static void test_sriov_status_structure(void) {
    JsonObject *obj = pcv_sriov_status();
    g_assert_nonnull(obj);
    g_assert_true(json_object_has_member(obj, "available"));
    g_assert_true(json_object_has_member(obj, "physical_functions"));
    JsonArray *pfs = json_object_get_array_member(obj, "physical_functions");
    g_assert_nonnull(pfs);
    json_object_unref(obj);
}

static void test_sriov_list_empty(void) {
    JsonArray *arr = pcv_sriov_list(NULL);
    g_assert_nonnull(arr);
    /* SR-IOV 미지원이면 빈 배열 */
    json_array_unref(arr);
}

static void test_sriov_list_nonexist_pf(void) {
    JsonArray *arr = pcv_sriov_list("nonexist99");
    g_assert_nonnull(arr);
    g_assert_cmpuint(json_array_get_length(arr), ==, 0);
    json_array_unref(arr);
}

/* ── 멱등 동작 ── */

static void test_sriov_disable_idempotent(void) {
    /* 존재하지 않는 PF — VF 없으므로 항상 성공 */
    gboolean ok = pcv_sriov_disable("nonexist99", NULL);
    /* sysfs 경로가 없으면 명령 자체는 성공 (;true 패턴) */
    g_assert_true(ok);
}

/* ── VF PCI 주소 조회 — 존재하지 않는 PF ── */

static void test_sriov_vf_pci_null(void) {
    gchar *pci = pcv_sriov_vf_pci_addr("nonexist99", 0);
    g_assert_null(pci);
}

/* ── 입력 검증(화이트리스트) 거부 경로 — 커맨드 인젝션 방어 ──
 *
 * 아래 3개는 셸/argv 로 흘러가기 전에 형식 검증 단계에서 거부되어야 한다.
 * 즉 FALSE 반환 + GError 설정(검증 실패 경로)이며, 외부 명령은 실행되지 않는다.
 */

static void test_sriov_disable_reject_injection(void) {
    GError *err = NULL;
    /* 셸 메타문자/공백 포함 PF 이름은 pcv_validate_iface_name 에서 거부 */
    gboolean ok = pcv_sriov_disable("x; touch /tmp/pwn", &err);
    g_assert_false(ok);
    g_assert_nonnull(err);   /* 검증 실패 경로임을 확인 (명령 미실행) */
    g_clear_error(&err);
}

static void test_sriov_set_reject_bad_mac(void) {
    GError *err = NULL;
    /* MAC 필드에 주입된 'vlan 4095' 토큰은 pcv_validate_mac 에서 거부 */
    gboolean ok = pcv_sriov_set("eth0", 0, "52:54:00 vlan 4095", -1, -1, &err);
    g_assert_false(ok);
    g_assert_nonnull(err);
    g_clear_error(&err);
}

static void test_sriov_detach_reject_bad_vm(void) {
    GError *err = NULL;
    /* vm_name 에 주입된 '; reboot' 는 pcv_validate_vm_name 에서 거부 (pci 는 유효) */
    gboolean ok = pcv_sriov_detach_vm("vm; reboot", "0000:01:00.0", &err);
    g_assert_false(ok);
    g_assert_nonnull(err);
    g_clear_error(&err);
}

/* ── 등록 ── */

void test_sriov_register(void) {
    g_test_add_func("/sriov/status/structure",       test_sriov_status_structure);
    g_test_add_func("/sriov/list/empty",             test_sriov_list_empty);
    g_test_add_func("/sriov/list/nonexist_pf",       test_sriov_list_nonexist_pf);
    g_test_add_func("/sriov/disable/idempotent",     test_sriov_disable_idempotent);
    g_test_add_func("/sriov/vf_pci/null",            test_sriov_vf_pci_null);
    g_test_add_func("/sriov/disable/reject_injection", test_sriov_disable_reject_injection);
    g_test_add_func("/sriov/set/reject_bad_mac",       test_sriov_set_reject_bad_mac);
    g_test_add_func("/sriov/detach/reject_bad_vm",     test_sriov_detach_reject_bad_vm);
}
