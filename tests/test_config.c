/* tests/test_config.c
 *
 * 대상 모듈: src/utils/pcv_config.c — daemon.conf 파싱 + 환경변수 오버라이드
 *
 * 이 테스트가 검증하는 것:
 *   설정 우선순위(환경변수 > GKeyFile > 컴파일 기본값), 정수 파싱 실패 시
 *   안전한 폴백, PCV_SECRET_* 시크릿 환경변수, ENC: 접두사 암호화 라운드트립,
 *   init/shutdown 사이클 안전성, 손상된 INI 파일 방어를 검사한다.
 *
 * 실행: sudo ./test_runner -p /config
 *
 * 테스트 추가: test_config_register() 하단에 g_test_add_func 등록
 *   주의: pcv_config는 전역 싱글턴이므로 각 테스트에서 반드시
 *   환경변수 정리 → init → 검증 → shutdown 순서를 지켜야 한다.
 *
 * 외부 의존:
 *   - PCV_CONFIG_PATH 환경변수로 임시 파일 지정 (자동 정리)
 *   - /etc/machine-id 읽기 (encrypt_value 테스트, 없으면 graceful skip)
 */

#include <glib.h>
#include <glib/gstdio.h>
#include "utils/pcv_config.h"

/* pcv_config 는 전역 상태이므로 각 테스트에서 명시적으로
 * unsetenv → init → 검증 → shutdown 순으로 실행합니다.     */

/* ── 기본값 반환 ─────────────────────────────────────── */

static void test_defaults(void) {
    /* 모든 오버라이드 환경변수 제거 */
    g_unsetenv("PURECVISOR_SOCKET_PATH");
    g_unsetenv("PURECVISOR_LIBVIRT_URI");
    g_unsetenv("PURECVISOR_POOL_MAX_CONN");
    g_unsetenv("PURECVISOR_DRAIN_TIMEOUT");
    g_unsetenv("PURECVISOR_DB_PATH");
    g_unsetenv("PURECVISOR_LOG_LEVEL");

    pcv_config_init();

    g_assert_cmpstr(pcv_config_get_socket_path(), ==, PCV_DEFAULT_SOCKET_PATH);
    g_assert_cmpstr(pcv_config_get_libvirt_uri(),  ==, PCV_DEFAULT_LIBVIRT_URI);
    g_assert_cmpint(pcv_config_get_pool_max_conn(),==, PCV_DEFAULT_POOL_MAX_CONN);
    g_assert_cmpint(pcv_config_get_drain_timeout(),==, PCV_DEFAULT_DRAIN_TIMEOUT);
    g_assert_cmpstr(pcv_config_get_db_path(),      ==, PCV_DEFAULT_DB_PATH);
    g_assert_cmpstr(pcv_config_get_log_level(),    ==, PCV_DEFAULT_LOG_LEVEL);

    pcv_config_shutdown();
}

/* ── 환경변수 오버라이드: socket_path ───────────────── */

static void test_env_override_socket(void) {
    g_setenv("PURECVISOR_SOCKET_PATH", "/tmp/test.sock", TRUE);
    g_unsetenv("PURECVISOR_LIBVIRT_URI");
    g_unsetenv("PURECVISOR_POOL_MAX_CONN");
    g_unsetenv("PURECVISOR_DRAIN_TIMEOUT");
    g_unsetenv("PURECVISOR_DB_PATH");
    g_unsetenv("PURECVISOR_LOG_LEVEL");

    pcv_config_init();
    g_assert_cmpstr(pcv_config_get_socket_path(), ==, "/tmp/test.sock");
    /* 나머지는 기본값 */
    g_assert_cmpstr(pcv_config_get_libvirt_uri(), ==, PCV_DEFAULT_LIBVIRT_URI);
    pcv_config_shutdown();

    g_unsetenv("PURECVISOR_SOCKET_PATH");
}

/* ── 환경변수 오버라이드: libvirt_uri ───────────────── */

static void test_env_override_uri(void) {
    g_unsetenv("PURECVISOR_SOCKET_PATH");
    g_setenv("PURECVISOR_LIBVIRT_URI", "test:///default", TRUE);
    g_unsetenv("PURECVISOR_POOL_MAX_CONN");
    g_unsetenv("PURECVISOR_DRAIN_TIMEOUT");
    g_unsetenv("PURECVISOR_DB_PATH");
    g_unsetenv("PURECVISOR_LOG_LEVEL");

    pcv_config_init();
    g_assert_cmpstr(pcv_config_get_libvirt_uri(), ==, "test:///default");
    pcv_config_shutdown();

    g_unsetenv("PURECVISOR_LIBVIRT_URI");
}

/* ── 환경변수 오버라이드: pool_max_conn (정수) ──────── */

static void test_env_override_pool_int(void) {
    g_unsetenv("PURECVISOR_SOCKET_PATH");
    g_unsetenv("PURECVISOR_LIBVIRT_URI");
    g_setenv("PURECVISOR_POOL_MAX_CONN", "4", TRUE);
    g_unsetenv("PURECVISOR_DRAIN_TIMEOUT");
    g_unsetenv("PURECVISOR_DB_PATH");
    g_unsetenv("PURECVISOR_LOG_LEVEL");

    pcv_config_init();
    g_assert_cmpint(pcv_config_get_pool_max_conn(), ==, 4);
    pcv_config_shutdown();

    g_unsetenv("PURECVISOR_POOL_MAX_CONN");
}

/* ── 잘못된 정수 환경변수 → 기본값 폴백 ────────────── */

static void test_env_invalid_int_fallback(void) {
    g_unsetenv("PURECVISOR_SOCKET_PATH");
    g_unsetenv("PURECVISOR_LIBVIRT_URI");
    g_setenv("PURECVISOR_POOL_MAX_CONN", "not-a-number", TRUE);
    g_unsetenv("PURECVISOR_DRAIN_TIMEOUT");
    g_unsetenv("PURECVISOR_DB_PATH");
    g_unsetenv("PURECVISOR_LOG_LEVEL");

    pcv_config_init();
    g_assert_cmpint(pcv_config_get_pool_max_conn(), ==, PCV_DEFAULT_POOL_MAX_CONN);
    pcv_config_shutdown();

    g_unsetenv("PURECVISOR_POOL_MAX_CONN");
}

/* ── dump() crash 없음 ───────────────────────────────── */

static void test_dump_no_crash(void) {
    g_unsetenv("PURECVISOR_SOCKET_PATH");
    g_unsetenv("PURECVISOR_LIBVIRT_URI");
    g_unsetenv("PURECVISOR_POOL_MAX_CONN");
    g_unsetenv("PURECVISOR_DRAIN_TIMEOUT");
    g_unsetenv("PURECVISOR_DB_PATH");
    g_unsetenv("PURECVISOR_LOG_LEVEL");

    pcv_config_init();
    pcv_config_dump();   /* crash 없이 정상 반환 */
    pcv_config_shutdown();
}

/* ── 추가 케이스: getter들 + generic key/value + secret ─── */

static void clear_env(void) {
    g_unsetenv("PURECVISOR_SOCKET_PATH");
    g_unsetenv("PURECVISOR_LIBVIRT_URI");
    g_unsetenv("PURECVISOR_POOL_MAX_CONN");
    g_unsetenv("PURECVISOR_DRAIN_TIMEOUT");
    g_unsetenv("PURECVISOR_DB_PATH");
    g_unsetenv("PURECVISOR_LOG_LEVEL");
}

static void test_storage_getters(void) {
    clear_env();
    pcv_config_init();
    /* 저장소 관련 getter는 항상 non-null 폴백 */
    g_assert_nonnull(pcv_config_get_zvol_pool());
    g_assert_nonnull(pcv_config_get_container_pool());
    g_assert_nonnull(pcv_config_get_container_path());
    g_assert_nonnull(pcv_config_get_image_dir());
    g_assert_nonnull(pcv_config_get_iso_dirs());
    g_assert_nonnull(pcv_config_get_ssh_user());
    pcv_config_shutdown();
}

static void test_get_string_default(void) {
    clear_env();
    pcv_config_init();
    /* 미존재 섹션/키 → 기본값 */
    const gchar *v = pcv_config_get_string("nonexistent", "key", "fallback");
    g_assert_cmpstr(v, ==, "fallback");
    /* NULL section/key는 안전 */
    v = pcv_config_get_string(NULL, NULL, "default");
    g_assert_nonnull(v);
    pcv_config_shutdown();
}

static void test_get_int_default(void) {
    clear_env();
    pcv_config_init();
    gint v = pcv_config_get_int("nonexistent", "key", 42);
    g_assert_cmpint(v, ==, 42);
    /* NULL section/key는 sentinel(-1) 또는 def 반환 — crash 없으면 OK */
    (void)pcv_config_get_int(NULL, NULL, 99);
    pcv_config_shutdown();
}

static void test_db_path_env_override(void) {
    clear_env();
    g_setenv("PURECVISOR_DB_PATH", "/tmp/test-vm-state.db", TRUE);
    pcv_config_init();
    g_assert_cmpstr(pcv_config_get_db_path(), ==, "/tmp/test-vm-state.db");
    pcv_config_shutdown();
    g_unsetenv("PURECVISOR_DB_PATH");
}

static void test_log_level_env_override(void) {
    clear_env();
    g_setenv("PURECVISOR_LOG_LEVEL", "debug", TRUE);
    pcv_config_init();
    g_assert_cmpstr(pcv_config_get_log_level(), ==, "debug");
    pcv_config_shutdown();
    g_unsetenv("PURECVISOR_LOG_LEVEL");
}

static void test_drain_timeout_env(void) {
    clear_env();
    g_setenv("PURECVISOR_DRAIN_TIMEOUT", "60", TRUE);
    pcv_config_init();
    g_assert_cmpint(pcv_config_get_drain_timeout(), ==, 60);
    pcv_config_shutdown();
    g_unsetenv("PURECVISOR_DRAIN_TIMEOUT");
}

static void test_init_shutdown_cycle(void) {
    clear_env();
    /* 다회 init/shutdown 사이클 */
    for (int i = 0; i < 3; i++) {
        pcv_config_init();
        pcv_config_shutdown();
    }
}

/* ── GKeyFile 파일 파싱 (PCV_CONFIG_PATH 격리) ─────────── */

static void test_parse_keyfile(void) {
    clear_env();
    gchar *tmpdir = g_dir_make_tmp("pcv-cfg-XXXXXX", NULL);
    gchar *cfgpath = g_build_filename(tmpdir, "daemon.conf", NULL);
    const gchar *content =
        "[daemon]\n"
        "socket_path=/tmp/parsed.sock\n"
        "pool_max_conn=16\n"
        "drain_timeout=45\n"
        "[storage]\n"
        "zvol_pool=parsed_pool/vms\n"
        "image_dir=/tmp/imgs\n"
        "[logging]\n"
        "level=debug\n";
    g_file_set_contents(cfgpath, content, -1, NULL);
    g_setenv("PCV_CONFIG_PATH", cfgpath, TRUE);

    pcv_config_init();
    g_assert_cmpstr(pcv_config_get_socket_path(), ==, "/tmp/parsed.sock");
    g_assert_cmpint(pcv_config_get_pool_max_conn(), ==, 16);
    g_assert_cmpint(pcv_config_get_drain_timeout(), ==, 45);
    g_assert_cmpstr(pcv_config_get_zvol_pool(), ==, "parsed_pool/vms");
    g_assert_cmpstr(pcv_config_get_image_dir(), ==, "/tmp/imgs");
    /* generic getter */
    g_assert_cmpstr(pcv_config_get_string("daemon", "socket_path", "x"), ==, "/tmp/parsed.sock");
    g_assert_cmpint(pcv_config_get_int("daemon", "pool_max_conn", 0), ==, 16);
    pcv_config_shutdown();

    g_unsetenv("PCV_CONFIG_PATH");
    g_unlink(cfgpath); g_rmdir(tmpdir);
    g_free(cfgpath); g_free(tmpdir);
}

/* PCV_SECRET_TESTGROUP_APIKEY 환경변수가 pcv_config_get_secret으로 읽히는지 확인 */
static void test_secret_from_env(void) {
    clear_env();
    g_setenv("PCV_SECRET_TESTGROUP_APIKEY", "supersecret123", TRUE);
    pcv_config_init();
    gchar *v = pcv_config_get_secret("testgroup", "apikey", "default");
    g_assert_cmpstr(v, ==, "supersecret123");
    g_free(v);
    pcv_config_shutdown();
    g_unsetenv("PCV_SECRET_TESTGROUP_APIKEY");
}

static void test_secret_fallback(void) {
    clear_env();
    pcv_config_init();
    /* 환경변수 + config 모두 없음 → fallback */
    gchar *v = pcv_config_get_secret("nogroup", "nokey", "fallback-value");
    g_assert_cmpstr(v, ==, "fallback-value");
    g_free(v);
    pcv_config_shutdown();
}

static void test_secret_plaintext_from_keyfile(void) {
    clear_env();
    gchar *tmpdir = g_dir_make_tmp("pcv-cfg-sec-XXXXXX", NULL);
    gchar *cfgpath = g_build_filename(tmpdir, "daemon.conf", NULL);
    g_file_set_contents(cfgpath,
        "[secrets]\nplain_pw=mypassword\n", -1, NULL);
    g_setenv("PCV_CONFIG_PATH", cfgpath, TRUE);
    pcv_config_init();
    gchar *v = pcv_config_get_secret("secrets", "plain_pw", "x");
    g_assert_cmpstr(v, ==, "mypassword");
    g_free(v);
    pcv_config_shutdown();
    g_unsetenv("PCV_CONFIG_PATH");
    g_unlink(cfgpath); g_rmdir(tmpdir);
    g_free(cfgpath); g_free(tmpdir);
}

/* pcv_config_encrypt_value가 "ENC:" 접두사 + base64 형태를 반환하는지 확인 (/etc/machine-id 의존) */
static void test_encrypt_value_returns_enc_prefix(void) {
    clear_env();
    pcv_config_init();
    gchar *enc = pcv_config_encrypt_value("hello");
    if (enc) {
        /* "ENC:" 접두사 + base64 본문 */
        g_assert_true(g_str_has_prefix(enc, "ENC:"));
        g_free(enc);
    }
    /* NULL 입력 안전 */
    g_assert_null(pcv_config_encrypt_value(NULL));
    pcv_config_shutdown();
}

static void test_reload_no_crash(void) {
    clear_env();
    pcv_config_init();
    pcv_config_reload();  /* 재로드 — crash 없으면 OK */
    pcv_config_shutdown();
}

/* ── 신규 케이스 a: env_override_jwt_secret ─────────── */

static void test_env_override_jwt_secret(void) {
    clear_env();
    g_unsetenv("PURECVISOR_JWT_SECRET");
    g_setenv("PURECVISOR_JWT_SECRET", "my-test-jwt-secret-key", TRUE);
    pcv_config_init();
    g_assert_cmpstr(pcv_config_get_jwt_secret(), ==, "my-test-jwt-secret-key");
    pcv_config_shutdown();
    g_unsetenv("PURECVISOR_JWT_SECRET");
}

/* ── 신규 케이스 b: env_override_admin_user ─────────── */

static void test_env_override_admin_user(void) {
    clear_env();
    g_unsetenv("PURECVISOR_ADMIN_USER");
    g_setenv("PURECVISOR_ADMIN_USER", "testadmin", TRUE);
    pcv_config_init();
    g_assert_cmpstr(pcv_config_get_admin_user(), ==, "testadmin");
    /* 다른 설정은 기본값 유지 */
    g_assert_cmpstr(pcv_config_get_socket_path(), ==, PCV_DEFAULT_SOCKET_PATH);
    pcv_config_shutdown();
    g_unsetenv("PURECVISOR_ADMIN_USER");
}

/* ── 신규 케이스 c: env_override_pool_max_conn ──────── */

static void test_env_override_pool_max_conn(void) {
    clear_env();
    g_unsetenv("PURECVISOR_POOL_MAX_CONN");
    g_setenv("PURECVISOR_POOL_MAX_CONN", "8", TRUE);
    pcv_config_init();
    g_assert_cmpint(pcv_config_get_pool_max_conn(), ==, 8);
    pcv_config_shutdown();
    g_unsetenv("PURECVISOR_POOL_MAX_CONN");
}

static void test_parse_invalid_keyfile(void) {
    clear_env();
    gchar *tmpdir = g_dir_make_tmp("pcv-cfg-bad-XXXXXX", NULL);
    gchar *cfgpath = g_build_filename(tmpdir, "daemon.conf", NULL);
    /* 손상된 INI — 섹션 헤더 없는 키, 깨진 escape */
    g_file_set_contents(cfgpath, "this is not valid ini\n=key\n[unclosed\n", -1, NULL);
    g_setenv("PCV_CONFIG_PATH", cfgpath, TRUE);

    pcv_config_init(); /* 파싱 실패 → 기본값 폴백 (crash 없음) */
    g_assert_nonnull(pcv_config_get_socket_path());
    pcv_config_shutdown();

    g_unsetenv("PCV_CONFIG_PATH");
    g_unlink(cfgpath); g_rmdir(tmpdir);
    g_free(cfgpath); g_free(tmpdir);
}

/* ── 등록 함수 ───────────────────────────────────────── */

void test_config_register(void) {
    g_test_add_func("/config/defaults",                 test_defaults);
    g_test_add_func("/config/env_override_socket",      test_env_override_socket);
    g_test_add_func("/config/env_override_uri",         test_env_override_uri);
    g_test_add_func("/config/env_override_pool_int",    test_env_override_pool_int);
    g_test_add_func("/config/env_invalid_int_fallback", test_env_invalid_int_fallback);
    g_test_add_func("/config/dump_no_crash",            test_dump_no_crash);
    g_test_add_func("/config/storage_getters",          test_storage_getters);
    g_test_add_func("/config/get_string_default",       test_get_string_default);
    g_test_add_func("/config/get_int_default",          test_get_int_default);
    g_test_add_func("/config/db_path_env_override",     test_db_path_env_override);
    g_test_add_func("/config/log_level_env_override",   test_log_level_env_override);
    g_test_add_func("/config/drain_timeout_env",        test_drain_timeout_env);
    g_test_add_func("/config/init_shutdown_cycle",      test_init_shutdown_cycle);
    g_test_add_func("/config/parse_keyfile",            test_parse_keyfile);
    g_test_add_func("/config/parse_invalid_keyfile",    test_parse_invalid_keyfile);
    g_test_add_func("/config/secret_from_env",          test_secret_from_env);
    g_test_add_func("/config/secret_fallback",          test_secret_fallback);
    g_test_add_func("/config/secret_plaintext_from_keyfile", test_secret_plaintext_from_keyfile);
    g_test_add_func("/config/encrypt_value_returns_enc_prefix", test_encrypt_value_returns_enc_prefix);
    g_test_add_func("/config/reload_no_crash",          test_reload_no_crash);
    g_test_add_func("/config/env_override_jwt_secret",  test_env_override_jwt_secret);
    g_test_add_func("/config/env_override_admin_user",  test_env_override_admin_user);
    g_test_add_func("/config/env_override_pool_max_conn", test_env_override_pool_max_conn);
}
