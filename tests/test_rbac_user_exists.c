/* tests/test_rbac_user_exists.c
 *
 * pcv_rbac_user_exists()의 3-상태(ABSENT/PRESENT/UNKNOWN) DB 존재 조회 계약을
 * 검증한다 (SEC-2 기반 — daemon.conf bootstrap fallback이 비밀번호 회전 후에도
 * 계속 유효해지는 백도어를 막으려면, fallback 결정 이전에 실제 RBAC DB에
 * 사용자가 존재하는지부터 확인해야 한다).
 *
 * pcv_rbac.c는 test_runner에 실제로 링크되어 있으므로(Makefile
 * TEST_COMMON_SRCS), 아래 테스트는 재현이 아니라 실제 컴파일된
 * pcv_rbac_user_exists()/pcv_rbac_user_create()를 직접 호출한다.
 *
 * 각 테스트는 독립된 임시 DB로 pcv_rbac_init()/pcv_rbac_shutdown() 전체
 * 라이프사이클을 거친다 (WAL 모드라 -wal/-shm 파일도 함께 정리, 관례는
 * tests/test_vm_state.c 참고).
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <sqlite3.h>

#include "../src/modules/auth/pcv_rbac.h"

static gchar *g_tmpdir = NULL;
static gchar *g_dbpath = NULL;

static void
setup(void)
{
    g_tmpdir = g_dir_make_tmp("pcv-rbac-user-exists-XXXXXX", NULL);
    g_dbpath = g_build_filename(g_tmpdir, "rbac.db", NULL);
    pcv_rbac_init(g_dbpath);
}

static void
teardown(void)
{
    pcv_rbac_shutdown();
    if (g_dbpath) {
        gchar *wal = g_strconcat(g_dbpath, "-wal", NULL);
        gchar *shm = g_strconcat(g_dbpath, "-shm", NULL);
        g_unlink(g_dbpath); g_unlink(wal); g_unlink(shm);
        g_free(wal); g_free(shm); g_free(g_dbpath); g_dbpath = NULL;
    }
    if (g_tmpdir) { g_rmdir(g_tmpdir); g_free(g_tmpdir); g_tmpdir = NULL; }
}

static void
test_user_exists_absent_for_missing_user(void)
{
    setup();
    g_assert_cmpint(pcv_rbac_user_exists("nobody_here"), ==, PCV_USER_ABSENT);
    teardown();
}

static void
test_user_exists_present_after_create(void)
{
    setup();
    GError *err = NULL;
    gboolean ok = pcv_rbac_user_create("alice", "pw", PCV_ROLE_ADMIN, NULL, &err);
    g_assert_true(ok);
    g_assert_no_error(err);
    g_assert_cmpint(pcv_rbac_user_exists("alice"), ==, PCV_USER_PRESENT);
    teardown();
}

static void
test_user_exists_absent_for_empty_username(void)
{
    setup();
    g_assert_cmpint(pcv_rbac_user_exists(""), ==, PCV_USER_ABSENT);
    teardown();
}

/* SEC-2 회귀: DB 조회 실패(fail-secure) 분기가 UNKNOWN을 계속 반환하는지 확인.
 * g_rbac_db(setup()이 연 첫 연결)는 그대로 둔 채, 같은 파일을 가리키는
 * 두 번째 sqlite3 연결로 users 테이블을 지운다 — 그러면
 * pcv_rbac_user_exists() 내부 sqlite3_prepare_v2()가 "no such table: users"로
 * 실패해 UNKNOWN을 반환할 수밖에 없다(g_rbac_db 핸들·뮤텍스 자체는 계속
 * 유효하므로 "init 전 호출" 서브케이스와 달리 안전하게 재현 가능).
 * 이 분기가 실수로 ABSENT로 바뀌면 비밀번호 회전 후에도 daemon.conf
 * bootstrap fallback이 되살아나는 SEC-2 백도어가 재발한다. */
static void
test_user_exists_unknown_on_db_error(void)
{
    setup();

    sqlite3 *raw_db = NULL;
    g_assert_cmpint(sqlite3_open(g_dbpath, &raw_db), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_exec(raw_db, "DROP TABLE users;", NULL, NULL, NULL),
                    ==, SQLITE_OK);
    sqlite3_close(raw_db);

    g_assert_cmpint(pcv_rbac_user_exists("anyone"), ==, PCV_USER_UNKNOWN);

    teardown();
}

void
test_rbac_user_exists_register(void)
{
    g_test_add_func("/rbac/user_exists/absent_for_missing_user",
                    test_user_exists_absent_for_missing_user);
    g_test_add_func("/rbac/user_exists/present_after_create",
                    test_user_exists_present_after_create);
    g_test_add_func("/rbac/user_exists/absent_for_empty_username",
                    test_user_exists_absent_for_empty_username);
    g_test_add_func("/rbac/user_exists/unknown_on_db_error",
                    test_user_exists_unknown_on_db_error);
}
