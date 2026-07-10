/* tests/test_plugin.c
 *
 * 대상 모듈: src/modules/plugin/pcv_plugin_manager.c — GModule .so 플러그인 시스템
 *
 * 이 테스트가 검증하는 것:
 *   플러그인 시스템의 유효성 검사 로직을 데몬 링크 없이 인라인으로 복제하여
 *   경계값·보안·호환성을 검사한다. 실제 .so 로딩은 수행하지 않는다.
 *
 *   1. plugin_path/valid   — 정상 경로 허용
 *   2. plugin_path/invalid — NULL·빈문자열·'..'순회·256자 초과 거부
 *   3. plugin_name/extract — g_path_get_basename 기반 추출, 엣지케이스 포함
 *   4. symbol_name/valid   — C 식별자 규칙 준수 이름 허용
 *   5. symbol_name/invalid — 빈문자열·숫자시작·특수문자·128자 초과 거부
 *   6. abi_version/compat  — 호스트==플러그인 ABI 호환
 *   7. abi_version/incompat — 버전 불일치 거부
 *   8. abi_version/zero    — ABI v0 (미설정) 거부
 *   9. unload_safety/null_ptrs — 언로드 후 모든 포인터 NULL
 *  10. registry_add/duplicate — 중복 메서드 등록 시 첫 번째 유지
 *
 * 실행: sudo ./test_runner -p /plugin
 *
 * 외부 의존: GLib 만 사용. 실제 GModule(.so 로딩) 없음.
 *            pcv_plugin_manager.c 를 링크하지 않으므로
 *            검증 로직을 이 파일 안에 인라인으로 재구현한다.
 */

#include <glib.h>
#include <string.h>
#include "modules/plugin/pcv_plugin_api.h"  /* PCV_PLUGIN_ABI_VERSION, PcvPluginMeta */

/* ── 인라인 복제 로직 ──────────────────────────────────────────────
 *
 * 테스트 대상 로직을 데몬 바이너리에 링크하지 않고 독립적으로 검증하기 위해
 * pcv_plugin_manager.c 의 핵심 판단 로직을 이 파일에 직접 구현한다.
 * 실제 구현이 변경되면 이 함수들도 함께 갱신해야 한다.
 * ────────────────────────────────────────────────────────────────── */

/** 플러그인 디렉터리 경로 유효성 검사
 *
 * 규칙 (pcv_plugin_manager_init 의 암묵적 요구사항):
 *   - NULL 또는 빈 문자열 거부
 *   - ".." 구성요소를 포함하는 경로 순회 거부 (보안)
 *   - 256자 초과 거부 (PATH_MAX 여유 마진 포함)
 *   - 슬래시로 시작하는 절대경로 권장 (비절대 경로 허용하되 경고)
 *
 * Returns: TRUE — 유효, FALSE — 유효하지 않음
 */
static gboolean
plugin_path_is_valid(const gchar *path)
{
    if (!path || path[0] == '\0')
        return FALSE;
    if (strlen(path) > 256)
        return FALSE;
    /* ".." 구성요소 탐지: "/../", "/..", "../", ".." 단독 */
    if (strstr(path, "/../") != NULL)
        return FALSE;
    if (g_str_has_suffix(path, "/.."))
        return FALSE;
    if (g_str_has_prefix(path, "../"))
        return FALSE;
    if (strcmp(path, "..") == 0)
        return FALSE;
    return TRUE;
}

/** C 식별자 규칙 기반 심볼 이름 유효성 검사
 *
 * 규칙: [a-zA-Z_][a-zA-Z0-9_]*  + 길이 1~128
 * 참고: g_module_symbol() 은 이 규칙을 따르는 심볼만 의미 있게 조회된다.
 *
 * Returns: TRUE — 유효, FALSE — 유효하지 않음
 */
static gboolean
symbol_name_is_valid(const gchar *sym)
{
    if (!sym || sym[0] == '\0')
        return FALSE;
    gsize len = strlen(sym);
    if (len > 128)
        return FALSE;
    /* 첫 문자: 알파벳 또는 밑줄 */
    if (!g_ascii_isalpha(sym[0]) && sym[0] != '_')
        return FALSE;
    /* 나머지: 알파벳·숫자·밑줄 */
    for (gsize i = 1; i < len; i++) {
        if (!g_ascii_isalnum(sym[i]) && sym[i] != '_')
            return FALSE;
    }
    return TRUE;
}

/** ABI 버전 호환성 검사
 *
 * 규칙: 플러그인의 abi_version 이 host_abi 와 정확히 일치해야 한다.
 *       v0 은 "미설정" 으로 간주하여 항상 거부한다.
 *
 * @plugin_abi: 플러그인 meta->abi_version
 * @host_abi:   데몬이 지원하는 ABI (PCV_PLUGIN_ABI_VERSION)
 * Returns: TRUE — 호환, FALSE — 거부
 */
static gboolean
abi_version_compatible(guint plugin_abi, guint host_abi)
{
    if (plugin_abi == 0)
        return FALSE;            /* v0: 미설정 — 항상 거부 */
    return plugin_abi == host_abi;
}

/* ── 언로드 안전성 검사용 인라인 구조체 ────────────────────────── */

/** LoadedPlugin 의 테스트용 미니 버전 (실제 GModule 없이 포인터만 추적) */
typedef struct {
    gchar   name[64];
    gchar   version[32];
    void   *module;            /* 실제: GModule*, 테스트: 임의 포인터 */
    void  (*shutdown_fn)(void);
} TestLoadedPlugin;

/** 언로드 시뮬레이션: 모든 필드를 초기화(0)한다 */
static void
test_plugin_clear(TestLoadedPlugin *p)
{
    memset(p, 0, sizeof(*p));
}

/* ── 중복 등록 추적용 미니 레지스트리 ──────────────────────────── */

#define TEST_MAX_METHODS 8

typedef struct {
    gchar method[128];
} TestMethodEntry;

typedef struct {
    TestMethodEntry entries[TEST_MAX_METHODS];
    gint            count;
} TestRegistry;

/** 중복 등록 방어를 포함한 레지스트리 추가 (pcv_plugin_registry_add 로직 복제) */
static gboolean
test_registry_add(TestRegistry *reg, const gchar *method)
{
    if (!reg || !method || method[0] == '\0')
        return FALSE;
    if (reg->count >= TEST_MAX_METHODS)
        return FALSE;
    /* 중복 검사 */
    for (gint i = 0; i < reg->count; i++) {
        if (strcmp(reg->entries[i].method, method) == 0)
            return FALSE;  /* 중복: 첫 번째 유지, 추가 거부 */
    }
    g_strlcpy(reg->entries[reg->count].method, method,
              sizeof(reg->entries[0].method));
    reg->count++;
    return TRUE;
}

/* ═══════════════════════════════════════════════════════════════════
 * 1. Plugin path validation
 * ═══════════════════════════════════════════════════════════════════ */

static void test_plugin_path_valid(void)
{
    /* 표준 절대경로 */
    g_assert_true(plugin_path_is_valid("/usr/lib/purecvisor/plugins"));
    g_assert_true(plugin_path_is_valid("/etc/purecvisor/plugins.d"));
    g_assert_true(plugin_path_is_valid("/opt/pcv/ext"));
    /* 슬래시로 끝나는 경로 */
    g_assert_true(plugin_path_is_valid("/usr/lib/purecvisor/plugins/"));
    /* 짧은 루트 경로 */
    g_assert_true(plugin_path_is_valid("/p"));
}

static void test_plugin_path_invalid(void)
{
    /* NULL */
    g_assert_false(plugin_path_is_valid(NULL));
    /* 빈 문자열 */
    g_assert_false(plugin_path_is_valid(""));
    /* '..' 경로 순회 — 중간 */
    g_assert_false(plugin_path_is_valid("/usr/lib/../etc/passwd"));
    /* '..' 경로 순회 — 끝 */
    g_assert_false(plugin_path_is_valid("/usr/lib/.."));
    /* '..' 경로 순회 — 시작 */
    g_assert_false(plugin_path_is_valid("../etc/shadow"));
    /* '..' 단독 */
    g_assert_false(plugin_path_is_valid(".."));
    /* 256자 초과 */
    gchar *over = g_strnfill(257, 'x');
    over[0] = '/';  /* 절대경로 형식이어도 길이 초과 */
    g_assert_false(plugin_path_is_valid(over));
    g_free(over);
}

static void test_plugin_path_boundary_256(void)
{
    /* 정확히 256자 — 허용 */
    gchar *at256 = g_strnfill(256, 'a');
    at256[0] = '/';
    g_assert_true(plugin_path_is_valid(at256));
    g_free(at256);

    /* 257자 — 거부 */
    gchar *at257 = g_strnfill(257, 'a');
    at257[0] = '/';
    g_assert_false(plugin_path_is_valid(at257));
    g_free(at257);
}

/* ═══════════════════════════════════════════════════════════════════
 * 2. Plugin name extraction
 * ═══════════════════════════════════════════════════════════════════ */

static void test_plugin_name_extract_normal(void)
{
    gchar *name = g_path_get_basename("/usr/lib/purecvisor/plugins/libfoo.so");
    g_assert_cmpstr(name, ==, "libfoo.so");
    g_free(name);
}

static void test_plugin_name_extract_edge_cases(void)
{
    /* 디렉터리 없음 — 파일명만 그대로 반환 */
    gchar *n1 = g_path_get_basename("libbar.so");
    g_assert_cmpstr(n1, ==, "libbar.so");
    g_free(n1);

    /* 끝에 슬래시 — GLib는 빈 문자열 구성요소를 '.' 로 반환 */
    gchar *n2 = g_path_get_basename("/usr/lib/plugins/");
    g_assert_cmpstr(n2, ==, "plugins");  /* GLib 동작: 끝 슬래시 제거 후 마지막 구성요소 */
    g_free(n2);

    /* NULL 입력 — GLib g_path_get_basename(NULL)은 "." 반환 */
    gchar *n3 = g_path_get_basename(".");
    g_assert_cmpstr(n3, ==, ".");
    g_free(n3);

    /* 중첩 경로 */
    gchar *n4 = g_path_get_basename("/a/b/c/libplugin_v2.so");
    g_assert_cmpstr(n4, ==, "libplugin_v2.so");
    g_free(n4);
}

/* ═══════════════════════════════════════════════════════════════════
 * 3. Symbol name validation
 * ═══════════════════════════════════════════════════════════════════ */

static void test_symbol_name_valid(void)
{
    /* 표준 C 함수명 패턴 */
    g_assert_true(symbol_name_is_valid("pcv_plugin_get_meta"));
    g_assert_true(symbol_name_is_valid("pcv_plugin_register"));
    g_assert_true(symbol_name_is_valid("pcv_plugin_shutdown"));
    /* 밑줄 시작 (내부 심볼) */
    g_assert_true(symbol_name_is_valid("_init"));
    /* 단일 문자 */
    g_assert_true(symbol_name_is_valid("f"));
    /* 숫자 포함 (시작 아님) */
    g_assert_true(symbol_name_is_valid("handler_v2"));
    /* 정확히 128자 */
    gchar *max128 = g_strnfill(128, 'a');
    g_assert_true(symbol_name_is_valid(max128));
    g_free(max128);
}

static void test_symbol_name_invalid(void)
{
    /* NULL */
    g_assert_false(symbol_name_is_valid(NULL));
    /* 빈 문자열 */
    g_assert_false(symbol_name_is_valid(""));
    /* 숫자로 시작 */
    g_assert_false(symbol_name_is_valid("1init"));
    g_assert_false(symbol_name_is_valid("0handler"));
    /* 하이픈 포함 */
    g_assert_false(symbol_name_is_valid("my-plugin"));
    /* 점 포함 */
    g_assert_false(symbol_name_is_valid("my.func"));
    /* 공백 포함 */
    g_assert_false(symbol_name_is_valid("get meta"));
    /* 129자 초과 */
    gchar *over128 = g_strnfill(129, 'a');
    g_assert_false(symbol_name_is_valid(over128));
    g_free(over128);
}

/* ═══════════════════════════════════════════════════════════════════
 * 4 & 5. ABI version compatibility
 * ═══════════════════════════════════════════════════════════════════ */

static void test_abi_version_compatible(void)
{
    /* 호스트 v1, 플러그인 v1 → 호환 */
    g_assert_true(abi_version_compatible(1, 1));
    /* 임의 버전 N — 양쪽 동일하면 호환 */
    g_assert_true(abi_version_compatible(2, 2));
    g_assert_true(abi_version_compatible(42, 42));
}

static void test_abi_version_incompatible(void)
{
    /* 플러그인 v2, 호스트 v1 → 거부 */
    g_assert_false(abi_version_compatible(2, 1));
    /* 플러그인 v1, 호스트 v2 → 거부 (다운그레이드) */
    g_assert_false(abi_version_compatible(1, 2));
    /* 큰 버전 차이 */
    g_assert_false(abi_version_compatible(100, 1));
}

static void test_abi_version_zero_rejected(void)
{
    /* v0은 "미설정" — 어떤 호스트 버전에도 거부 */
    g_assert_false(abi_version_compatible(0, 1));
    g_assert_false(abi_version_compatible(0, 0));
    g_assert_false(abi_version_compatible(0, 2));
}

static void test_abi_version_constant(void)
{
    /* 현재 데몬 ABI 상수가 1임을 확인 */
    g_assert_cmpuint(PCV_PLUGIN_ABI_VERSION, ==, 1u);
    /* 현재 ABI 상수로 호환 검사 */
    g_assert_true(abi_version_compatible(PCV_PLUGIN_ABI_VERSION,
                                          PCV_PLUGIN_ABI_VERSION));
    g_assert_false(abi_version_compatible(PCV_PLUGIN_ABI_VERSION + 1,
                                           PCV_PLUGIN_ABI_VERSION));
}

/* ═══════════════════════════════════════════════════════════════════
 * 6. Plugin unload safety
 * ═══════════════════════════════════════════════════════════════════ */

static void
dummy_shutdown(void) { /* 테스트용 빈 shutdown 콜백 */ }

static void test_unload_clears_all_fields(void)
{
    TestLoadedPlugin p;
    memset(&p, 0xAB, sizeof(p));  /* 쓰레기 값으로 초기화 */

    /* 로딩 시뮬레이션: 유효한 값 채우기 */
    g_strlcpy(p.name, "libfoo", sizeof(p.name));
    g_strlcpy(p.version, "1.0", sizeof(p.version));
    p.module = (void*)0xDEADBEEF;  /* 가짜 GModule 포인터 */
    p.shutdown_fn = dummy_shutdown;

    /* 언로드 시뮬레이션 */
    test_plugin_clear(&p);

    /* 모든 포인터·필드가 NULL/0으로 초기화되었는지 확인 */
    g_assert_null(p.module);
    g_assert_null(p.shutdown_fn);
    g_assert_cmpstr(p.name, ==, "");
    g_assert_cmpstr(p.version, ==, "");
}

static void test_unload_idempotent_clear(void)
{
    TestLoadedPlugin p;
    p.module = (void*)0x1234;
    p.shutdown_fn = dummy_shutdown;

    test_plugin_clear(&p);
    g_assert_null(p.module);
    g_assert_null(p.shutdown_fn);

    /* 두 번 clear해도 안전해야 한다 */
    test_plugin_clear(&p);
    g_assert_null(p.module);
    g_assert_null(p.shutdown_fn);
}

/* ═══════════════════════════════════════════════════════════════════
 * 7. Registry — duplicate method guard
 * ═══════════════════════════════════════════════════════════════════ */

static void test_registry_duplicate_rejected(void)
{
    TestRegistry reg = {0};

    /* 첫 번째 등록 → 성공 */
    gboolean ok1 = test_registry_add(&reg, "myplugin.hello");
    g_assert_true(ok1);
    g_assert_cmpint(reg.count, ==, 1);

    /* 동일 메서드 재등록 → 거부 (count 불변) */
    gboolean ok2 = test_registry_add(&reg, "myplugin.hello");
    g_assert_false(ok2);
    g_assert_cmpint(reg.count, ==, 1);

    /* 다른 메서드 → 성공 */
    gboolean ok3 = test_registry_add(&reg, "myplugin.bye");
    g_assert_true(ok3);
    g_assert_cmpint(reg.count, ==, 2);

    /* 첫 번째가 보존되어 있는지 확인 */
    g_assert_cmpstr(reg.entries[0].method, ==, "myplugin.hello");
}

/* ═══════════════════════════════════════════════════════════════════
 * 등록 함수
 * ═══════════════════════════════════════════════════════════════════ */

void test_plugin_register(void)
{
    /* 1. Plugin path validation */
    g_test_add_func("/plugin/path/valid",          test_plugin_path_valid);
    g_test_add_func("/plugin/path/invalid",        test_plugin_path_invalid);
    g_test_add_func("/plugin/path/boundary_256",   test_plugin_path_boundary_256);

    /* 2. Plugin name extraction */
    g_test_add_func("/plugin/name/extract_normal", test_plugin_name_extract_normal);
    g_test_add_func("/plugin/name/extract_edges",  test_plugin_name_extract_edge_cases);

    /* 3 & 4. Symbol name validation */
    g_test_add_func("/plugin/symbol/valid",        test_symbol_name_valid);
    g_test_add_func("/plugin/symbol/invalid",      test_symbol_name_invalid);

    /* 5, 6 & 7. ABI version check */
    g_test_add_func("/plugin/abi/compatible",      test_abi_version_compatible);
    g_test_add_func("/plugin/abi/incompatible",    test_abi_version_incompatible);
    g_test_add_func("/plugin/abi/zero_rejected",   test_abi_version_zero_rejected);
    g_test_add_func("/plugin/abi/constant",        test_abi_version_constant);

    /* 8. Plugin unload safety */
    g_test_add_func("/plugin/unload/clears_all",   test_unload_clears_all_fields);
    g_test_add_func("/plugin/unload/idempotent",   test_unload_idempotent_clear);

    /* 9. Registry duplicate guard */
    g_test_add_func("/plugin/registry/duplicate",  test_registry_duplicate_rejected);
}
