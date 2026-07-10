/* tests/test_ovn.c
 *
 * 대상 모듈: src/modules/network/ovn_manager.c — OVN 가상 네트워크 SDN
 *
 * 이 테스트가 검증하는 것:
 *   OVN이 설치되지 않은 환경에서 graceful degradation을 검사한다.
 *   switch/router/nat/dhcp/acl list가 빈 배열을 반환하는지,
 *   존재하지 않는 리소스 삭제가 멱등(TRUE 반환)인지,
 *   status 객체에 "available" 필드가 있는지 확인.
 *
 * 실행: sudo ./test_runner -p /ovn
 *
 * 외부 의존: OVN (미설치 시에도 PASS — 빈 배열/멱등 삭제 검증)
 */

#include <glib.h>
#include <json-glib/json-glib.h>
#include "modules/network/ovs_overlay.h"

/* ovn_manager.h의 함수 선언 직접 참조 */
extern gboolean pcv_ovn_is_available(void);
extern JsonArray *pcv_ovn_switch_list(void);
extern JsonArray *pcv_ovn_router_list(void);
extern JsonArray *pcv_ovn_nat_list(const gchar *router);
extern JsonArray *pcv_ovn_dhcp_list(void);
extern JsonArray *pcv_ovn_acl_list(const gchar *sw);
extern JsonObject *pcv_ovn_status(void);
extern gboolean pcv_ovn_switch_delete(const gchar *name, GError **error);
extern gboolean pcv_ovn_router_delete(const gchar *name, GError **error);
/* V6 fix: OVN 식별자 화이트리스트 검증 (argv 재파싱 인젝션 방어)의 공개 래퍼 */
extern gboolean pcv_ovn_valid_id(const gchar *s);

/* ── graceful degradation: OVN 미설치 시 빈 배열 반환 ── */

static void test_ovn_switch_list_empty(void) {
    JsonArray *arr = pcv_ovn_switch_list();
    g_assert_nonnull(arr);
    g_assert_cmpuint(json_array_get_length(arr), ==, 0);
    json_array_unref(arr);
}

static void test_ovn_router_list_empty(void) {
    JsonArray *arr = pcv_ovn_router_list();
    g_assert_nonnull(arr);
    g_assert_cmpuint(json_array_get_length(arr), ==, 0);
    json_array_unref(arr);
}

static void test_ovn_nat_list_empty(void) {
    JsonArray *arr = pcv_ovn_nat_list("nonexist");
    g_assert_nonnull(arr);
    g_assert_cmpuint(json_array_get_length(arr), ==, 0);
    json_array_unref(arr);
}

static void test_ovn_dhcp_list_empty(void) {
    JsonArray *arr = pcv_ovn_dhcp_list();
    g_assert_nonnull(arr);
    g_assert_cmpuint(json_array_get_length(arr), ==, 0);
    json_array_unref(arr);
}

static void test_ovn_acl_list_empty(void) {
    JsonArray *arr = pcv_ovn_acl_list("nonexist");
    g_assert_nonnull(arr);
    g_assert_cmpuint(json_array_get_length(arr), ==, 0);
    json_array_unref(arr);
}

/* ── 멱등 삭제: 없는 리소스 삭제해도 TRUE 반환 ── */

static void test_ovn_switch_delete_idempotent(void) {
    g_assert_true(pcv_ovn_switch_delete("nonexist-sw", NULL));
}

static void test_ovn_router_delete_idempotent(void) {
    g_assert_true(pcv_ovn_router_delete("nonexist-lr", NULL));
}

/* ── V6: 식별자 검증이 인젝션 문자열을 거부하는지 확인 ──
 *
 * ovn_manager._run 이 g_shell_parse_argv 로 명령 문자열을 재파싱하던 시절
 * 공백/따옴표/'--'/';' 이 추가 argv 로 분할되어 ovn-nbctl 하위명령 체이닝
 * (테넌트 격리 우회)이 가능했다. 이제 각 값은 단일 argv 원소로 전달되고,
 * _valid_ovn_id 화이트리스트가 심층 방어로 이런 문자열을 거부한다. */
static void test_ovn_valid_id_rejects_injection(void) {
    /* 정상 식별자는 통과 */
    g_assert_true(pcv_ovn_valid_id("pcv-ls0"));
    g_assert_true(pcv_ovn_valid_id("tenant-alpha-ls"));
    g_assert_true(pcv_ovn_valid_id("10.0.0.1"));      /* ':' '.' 허용 */

    /* 인젝션/옵션 벡터는 거부 */
    g_assert_false(pcv_ovn_valid_id("ls add"));           /* 공백 → argv 분할 */
    g_assert_false(pcv_ovn_valid_id("sw --may-exist"));   /* 공백 + 옵션 */
    g_assert_false(pcv_ovn_valid_id("--priv"));           /* 선행 '-' 옵션 인젝션 */
    g_assert_false(pcv_ovn_valid_id("--"));               /* 선행 '-' */
    g_assert_false(pcv_ovn_valid_id("sw;ls-del x"));      /* ';' */
    g_assert_false(pcv_ovn_valid_id("sw\"quote"));        /* 따옴표 */
    g_assert_false(pcv_ovn_valid_id(""));                 /* 빈 문자열 */
    g_assert_false(pcv_ovn_valid_id(NULL));               /* NULL */
}

/* ── status 객체 구조 확인 ── */

static void test_ovn_status_structure(void) {
    JsonObject *obj = pcv_ovn_status();
    g_assert_nonnull(obj);
    g_assert_true(json_object_has_member(obj, "available"));
    json_object_unref(obj);
}

#if !PCV_CLUSTER_ENABLED
static void test_overlay_list_empty_single(void) {
    JsonArray *arr = pcv_overlay_list();
    g_assert_nonnull(arr);
    g_assert_cmpuint(json_array_get_length(arr), ==, 0);
    json_array_unref(arr);
}

static void test_overlay_info_reports_disabled_before_init(void) {
    JsonObject *obj = pcv_overlay_info("pcvoverlay0");
    g_assert_nonnull(obj);
    g_assert_true(json_object_has_member(obj, "error"));
    g_assert_cmpstr(json_object_get_string_member(obj, "error"), ==, "overlay disabled");
    json_object_unref(obj);
}
#endif

/* ── 등록 ── */

void test_ovn_register(void) {
    g_test_add_func("/ovn/switch_list/empty",      test_ovn_switch_list_empty);
    g_test_add_func("/ovn/router_list/empty",      test_ovn_router_list_empty);
    g_test_add_func("/ovn/nat_list/empty",         test_ovn_nat_list_empty);
    g_test_add_func("/ovn/dhcp_list/empty",        test_ovn_dhcp_list_empty);
    g_test_add_func("/ovn/acl_list/empty",         test_ovn_acl_list_empty);
    g_test_add_func("/ovn/switch_delete/idempotent", test_ovn_switch_delete_idempotent);
    g_test_add_func("/ovn/router_delete/idempotent", test_ovn_router_delete_idempotent);
    g_test_add_func("/ovn/valid_id/rejects_injection", test_ovn_valid_id_rejects_injection);
    g_test_add_func("/ovn/status/structure",       test_ovn_status_structure);
#if !PCV_CLUSTER_ENABLED
    g_test_add_func("/overlay/list/empty_single",  test_overlay_list_empty_single);
    g_test_add_func("/overlay/info/disabled_before_init",
                    test_overlay_info_reports_disabled_before_init);
#endif
}
