/**
 * @file test_container_owner_scope.c
 * @brief 컨테이너 operator owner-scope 소유자 저장소 효과 테스트 (B1 — IDOR 시정)
 *
 * 대상: src/modules/lxc/lxc_driver.c
 *         pcv_lxc_stamp_owner()  — create 성공 경로가 소유자 subject를 기록
 *         pcv_lxc_read_owner()   — dispatcher owner-scope 게이트가 소유자를 조회
 *
 * ============================================================================
 *  이 테스트가 검증하는 것 (owner-scope 결정 계약)
 * ============================================================================
 *  dispatcher.c의 게이트 _container_owner_matches_caller()는
 *      owner = pcv_lxc_read_owner(name);
 *      allowed = (owner && g_strcmp0(owner, caller_sub) == 0);
 *  로 operator의 접근을 판정한다(admin은 그 위 role 분기에서 우회). 즉 보안-임계
 *  substrate는 purecvisor.owner 파일 저장소이고, 이 테스트는 그 저장소를 통해
 *  결정 계약을 실증한다:
 *    - 소유자 일치         → 허용
 *    - 소유자 불일치       → 거부
 *    - 소유자 파일 부재    → 거부(operator) — 구 컨테이너/UDS admin 생성분 fail-secure
 *    - owner_sub 부재 스탬프 → 파일 미생성(→ read NULL → 부재와 동일 거부)
 *
 *  반사실(counterfactual): B1 이전에는 컨테이너에 소유자 개념이 없어
 *  container.start/stop/clone이 role만 통과하면 남의 컨테이너도 조작 가능했다
 *  (operator 교차테넌트, A01). owner 저장소가 없거나 스탬프가 빠지면
 *  read_owner가 NULL을 반환하고 owner_match_decision(...)이 항상 거부가 되어
 *  아래 allow 케이스가 red가 된다.
 *
 * 실행: ./test_runner -p /container_owner_scope
 * 외부 의존: 없음 (실 UDS/libvirt/lxc 불필요 — 파일 저장소 + 순수 결정 로직).
 *            PCV_CONFIG_PATH 임시 conf로 [container] lxc_path를 temp dir로 격리.
 * ============================================================================
 */
#include <glib.h>
#include <glib/gstdio.h>
#include "../src/modules/lxc/lxc_owner.h"
#include "../src/utils/pcv_config.h"

/*
 * operator owner-scope 판정 재현: dispatcher.c _container_owner_matches_caller의
 * 핵심(소유자 존재 && caller와 일치)을 실 저장소(pcv_lxc_read_owner) 위에서 재현한다.
 * trivial invariant지만 read_owner 저장소를 실제로 관통하므로, 스탬프/조회가
 * 깨지면(예: 파일명 오타, 경로 규칙 drift) 곧바로 red가 된다.
 */
static gboolean owner_match_decision(const gchar *name, const gchar *caller_sub) {
    gchar *owner = pcv_lxc_read_owner(name);
    gboolean allowed = (owner && g_strcmp0(owner, caller_sub) == 0);
    g_free(owner);
    return allowed;
}

/* temp lxc_path 격리 fixture — conf 작성 + config init, 컨테이너 dir 생성 */
typedef struct {
    gchar *root;     /* temp lxc_path */
    gchar *conf;     /* temp daemon.conf */
} OwnerFixture;

static void owner_fixture_setup(OwnerFixture *fx) {
    fx->root = g_dir_make_tmp("pcv-owner-scope-XXXXXX", NULL);
    g_assert_nonnull(fx->root);

    gchar *conf_body = g_strdup_printf("[container]\nlxc_path=%s\n", fx->root);
    fx->conf = g_build_filename(fx->root, "daemon.conf", NULL);
    g_assert_true(g_file_set_contents(fx->conf, conf_body, -1, NULL));
    g_free(conf_body);

    g_setenv("PCV_CONFIG_PATH", fx->conf, TRUE);
    pcv_config_init();
    g_assert_cmpstr(pcv_config_get_container_path(), ==, fx->root);
}

static void owner_fixture_teardown(OwnerFixture *fx) {
    pcv_config_shutdown();
    g_unsetenv("PCV_CONFIG_PATH");
    /* temp 트리 정리는 best-effort — 실패해도 테스트 판정에 영향 없음 */
    if (fx->conf) g_remove(fx->conf);
    g_free(fx->conf);
    g_free(fx->root);
    fx->conf = NULL;
    fx->root = NULL;
}

/* <root>/<name> 컨테이너 디렉터리 생성(create 이후 lxc가 만들어 둔 상태 모사) */
static void make_container_dir(OwnerFixture *fx, const gchar *name) {
    gchar *dir = g_build_filename(fx->root, name, NULL);
    g_assert_cmpint(g_mkdir_with_parents(dir, 0700), ==, 0);
    g_free(dir);
}

/* 소유자 일치 → 허용 / 불일치 → 거부 (round-trip + 결정) */
static void test_owner_match_allows_mismatch_denies(void) {
    OwnerFixture fx = {0};
    owner_fixture_setup(&fx);

    make_container_dir(&fx, "c1");
    g_assert_true(pcv_lxc_stamp_owner("c1", "alice"));

    gchar *back = pcv_lxc_read_owner("c1");
    g_assert_cmpstr(back, ==, "alice");   /* 저장소 round-trip */
    g_free(back);

    g_assert_true(owner_match_decision("c1", "alice"));   /* 일치 → 허용 */
    g_assert_false(owner_match_decision("c1", "bob"));     /* 불일치 → 거부 */

    owner_fixture_teardown(&fx);
}

/* 소유자 파일 부재 → operator 거부 (구 컨테이너/admin 생성분 fail-secure) */
static void test_absent_owner_denies(void) {
    OwnerFixture fx = {0};
    owner_fixture_setup(&fx);

    make_container_dir(&fx, "legacy");   /* dir는 있으나 purecvisor.owner 미기록 */
    g_assert_null(pcv_lxc_read_owner("legacy"));
    g_assert_false(owner_match_decision("legacy", "alice"));
    g_assert_false(owner_match_decision("legacy", ""));

    owner_fixture_teardown(&fx);
}

/* owner_sub 부재/빈값 스탬프 → 파일 미생성 → 부재와 동일 거부 */
static void test_empty_owner_sub_not_stamped(void) {
    OwnerFixture fx = {0};
    owner_fixture_setup(&fx);

    make_container_dir(&fx, "c2");
    g_assert_false(pcv_lxc_stamp_owner("c2", NULL));
    g_assert_false(pcv_lxc_stamp_owner("c2", ""));
    g_assert_null(pcv_lxc_read_owner("c2"));
    g_assert_false(owner_match_decision("c2", "alice"));

    owner_fixture_teardown(&fx);
}

/* 컨테이너별 소유자 격리 — c_a는 alice, c_b는 bob (교차 접근 거부) */
static void test_per_container_isolation(void) {
    OwnerFixture fx = {0};
    owner_fixture_setup(&fx);

    make_container_dir(&fx, "c_a");
    make_container_dir(&fx, "c_b");
    g_assert_true(pcv_lxc_stamp_owner("c_a", "alice"));
    g_assert_true(pcv_lxc_stamp_owner("c_b", "bob"));

    g_assert_true(owner_match_decision("c_a", "alice"));
    g_assert_false(owner_match_decision("c_a", "bob"));   /* bob는 alice 컨테이너 조작 불가 */
    g_assert_true(owner_match_decision("c_b", "bob"));
    g_assert_false(owner_match_decision("c_b", "alice"));

    owner_fixture_teardown(&fx);
}

/* 존재하지 않는 컨테이너 dir로 스탬프 → 파일 쓰기 실패(FALSE), read NULL */
static void test_stamp_missing_dir_fails(void) {
    OwnerFixture fx = {0};
    owner_fixture_setup(&fx);

    /* make_container_dir 미호출 — <root>/nope/ 부재 */
    g_assert_false(pcv_lxc_stamp_owner("nope", "alice"));
    g_assert_null(pcv_lxc_read_owner("nope"));

    owner_fixture_teardown(&fx);
}

void test_container_owner_scope_register(void) {
    g_test_add_func("/container_owner_scope/owner_match_allows_mismatch_denies",
                    test_owner_match_allows_mismatch_denies);
    g_test_add_func("/container_owner_scope/absent_owner_denies",
                    test_absent_owner_denies);
    g_test_add_func("/container_owner_scope/empty_owner_sub_not_stamped",
                    test_empty_owner_sub_not_stamped);
    g_test_add_func("/container_owner_scope/per_container_isolation",
                    test_per_container_isolation);
    g_test_add_func("/container_owner_scope/stamp_missing_dir_fails",
                    test_stamp_missing_dir_fails);
}
