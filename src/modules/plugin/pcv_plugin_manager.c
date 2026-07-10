/**
 * @file pcv_plugin_manager.c
 * @brief 플러그인 시스템 — GModule .so 동적 로딩 + RPC 핸들러 레지스트리
 *
 * [파일 역할]
 *   PureCVisor의 플러그인 확장 시스템. /etc/purecvisor/plugins.d/ 디렉터리의
 *   .so 공유 라이브러리를 런타임에 동적 로딩하고, 플러그인이 등록한 RPC 메서드를
 *   디스패처에서 호출할 수 있도록 레지스트리를 관리합니다.
 *
 * [아키텍처 위치]
 *   main.c (데몬 시작)
 *     -> pcv_plugin_manager_init("/etc/purecvisor/plugins.d")
 *          -> *.so 파일 스캔 -> pcv_plugin_load() -> 플러그인 register 콜백 호출
 *   dispatcher.c (RPC 라우팅)
 *     -> pcv_plugin_has_handler(method) -> TRUE면 pcv_plugin_dispatch() 호출
 *        -> 등록된 RPC 핸들러 함수 포인터 실행
 *
 * [플러그인 ABI 규약 (v1)]
 *   .so 파일은 반드시 다음 심볼을 export 해야 합니다:
 *     pcv_plugin_get_meta()   — 필수: PcvPluginMeta* 반환 (이름, 버전, ABI 버전)
 *     pcv_plugin_register()   — 필수: PcvPluginRegistry에 RPC 핸들러 등록
 *     pcv_plugin_shutdown()   — 선택: 종료 시 정리 콜백
 *
 *   ABI 버전이 PCV_PLUGIN_ABI_VERSION과 다르면 로딩 거부 (하위 호환성 보호)
 *
 * [핵심 패턴]
 *   - GModule: GLib의 플랫폼 독립 동적 라이브러리 로더 (dlopen/LoadLibrary 추상화)
 *   - G_MODULE_BIND_LOCAL: 플러그인 심볼이 전역으로 노출되지 않도록 격리
 *   - 고정 배열: MAX_PLUGINS(16), MAX_METHODS(64) — 동적 할당 대비 단순성/성능 우선
 *
 * [스레드 안전]
 *   초기화(init)와 종료(shutdown)는 메인 스레드에서만 호출됩니다.
 *   has_handler/dispatch는 읽기 전용이므로 초기화 이후에는 안전합니다.
 *   런타임 동적 로드/언로드 시에는 뮤텍스가 필요하나 현재 미구현
 *   (디스패처가 단일 스레드에서 실행되므로 실질적 문제 없음).
 *
 * [외부 의존성] GModule (gmodule-2.0)
 */
#include "pcv_plugin_manager.h"
#include "utils/pcv_log.h"
#include "../audit/pcv_audit.h"  /* B10-C1 audit */
#include <gmodule.h>
#include <string.h>
#include <sys/stat.h>

#define PLUG_LOG_DOM "plugin_mgr"
constexpr int MAX_PLUGINS = 16;    /* 동시 로딩 가능한 최대 플러그인 수 */
constexpr int MAX_METHODS = 64;    /* 플러그인이 등록할 수 있는 최대 RPC 메서드 수 */

/* ── C23 컴파일 타임 검증 ────────────────────────────────────── */
static_assert(MAX_PLUGINS >= 1);
static_assert(MAX_METHODS >= 1);

/* ── 내부 구조체 정의 ────────────────────────────────────────── */

/**
 * LoadedPlugin — 로딩된 단일 플러그인의 런타임 정보
 *
 * GModule 핸들을 보유하고, 종료 시 shutdown_fn을 호출합니다.
 * name/version은 pcv_plugin_get_meta()에서 복사됩니다.
 */
typedef struct {
    gchar          name[64];        /* 플러그인 이름 (메타데이터에서 복사) */
    gchar          version[32];     /* 플러그인 버전 문자열 */
    GModule       *module;          /* GModule 핸들 (g_module_close 시 해제) */
    PcvPluginShutdownFunc shutdown_fn; /* 종료 콜백 (NULL 가능) */
} LoadedPlugin;

/**
 * MethodEntry — 플러그인이 등록한 단일 RPC 메서드
 *
 * method: RPC 메서드 이름 (예: "myplugin.hello")
 * handler: 실제 핸들러 함수 포인터 (시그니처: JsonObject*, rpc_id, server, connection)
 */
typedef struct {
    gchar          method[128];     /* RPC 메서드 이름 */
    PcvRpcHandler  handler;         /* 핸들러 함수 포인터 */
    gchar          owner[64];       /* B10-C1: 소유 플러그인 이름 — unload 시 정리 */
} MethodEntry;

/**
 * PcvPluginRegistry — RPC 메서드 레지스트리
 *
 * 플러그인의 register 콜백에 이 구조체의 포인터가 전달되며,
 * 플러그인은 pcv_plugin_registry_add()를 통해 자신의 메서드를 등록합니다.
 */
struct _PcvPluginRegistry {
    MethodEntry methods[MAX_METHODS]; /* 등록된 메서드 배열 */
    gint        count;                /* 현재 등록된 메서드 수 */
};

/* ── 모듈 전역 상태 ─────────────────────────────────────────── */

static struct {
    LoadedPlugin      plugins[MAX_PLUGINS]; /* 로딩된 플러그인 배열 */
    gint              plugin_count;         /* 현재 로딩된 플러그인 수 */
    PcvPluginRegistry registry;             /* RPC 메서드 레지스트리 */
    gchar            *plugin_dir;           /* 플러그인 디렉터리 경로 */
    gboolean          initialized;          /* 초기화 완료 플래그 */
    /* B10-C1: 현재 register 콜백 실행 중인 플러그인 이름 (owner 추적용) */
    gchar             current_loading_plugin[64];
} G = {0};

/**
 * pcv_plugin_registry_add:
 * @reg:         레지스트리 포인터 (플러그인의 register 콜백에 전달됨)
 * @method_name: 등록할 RPC 메서드 이름 (예: "myplugin.hello")
 * @handler:     핸들러 함수 포인터
 *
 * 플러그인이 자신의 RPC 메서드를 레지스트리에 등록합니다.
 * 플러그인의 pcv_plugin_register() 콜백 내부에서 호출됩니다.
 * MAX_METHODS(64) 초과 시 조용히 무시합니다.
 */
void pcv_plugin_registry_add(PcvPluginRegistry *reg, const gchar *method_name,
                              PcvRpcHandler handler)
{
    if (!reg || !method_name || !handler) return;
    /* B10-C1 (Phase 5): MAX_METHODS 초과 시 명시적 경고 (silent skip 제거) */
    if (reg->count >= MAX_METHODS) {
        PCV_LOG_WARN(PLUG_LOG_DOM,
            "Plugin registry full (%d) — cannot register '%s' from '%s'",
            MAX_METHODS, method_name, G.current_loading_plugin);
        return;
    }
    /* B10-C4: 중복 메서드명 검출 (첫 등록 우선) */
    for (gint j = 0; j < reg->count; j++) {
        if (g_strcmp0(reg->methods[j].method, method_name) == 0) {
            PCV_LOG_WARN(PLUG_LOG_DOM,
                "Duplicate plugin method '%s' (owner '%s') — keeping first",
                method_name, reg->methods[j].owner);
            return;
        }
    }
    g_strlcpy(reg->methods[reg->count].method, method_name, sizeof(reg->methods[0].method));
    reg->methods[reg->count].handler = handler;
    /* B10-C1: owner 기록 — unload 시 정확한 정리 */
    g_strlcpy(reg->methods[reg->count].owner, G.current_loading_plugin,
              sizeof(reg->methods[0].owner));
    reg->count++;
    PCV_LOG_INFO(PLUG_LOG_DOM, "Registered plugin method: %s (owner: %s)",
                 method_name, G.current_loading_plugin);
}

/**
 * pcv_plugin_manager_init:
 * @plugin_dir: 플러그인 .so 파일이 있는 디렉터리 (NULL이면 /etc/purecvisor/plugins.d)
 *
 * 플러그인 매니저를 초기화하고 디렉터리 내 모든 .so 파일을 로딩합니다.
 * 디렉터리가 없으면 플러그인 없이 정상 시작합니다 (에러 아님).
 */
void pcv_plugin_manager_init(const gchar *plugin_dir)
{
    G.registry.count = 0;
    G.plugin_count = 0;
    G.plugin_dir = g_strdup(plugin_dir ? plugin_dir : "/etc/purecvisor/plugins.d");
    G.initialized = TRUE;

    if (!g_module_supported()) {
        PCV_LOG_WARN(PLUG_LOG_DOM, "GModule not supported on this platform");
        return;
    }

    /* 디렉터리 스캔 */
    GError *dir_err = NULL;  /* B10-M1: capture open error for informative log */
    GDir *dir = g_dir_open(G.plugin_dir, 0, &dir_err);
    if (!dir) {
        PCV_LOG_INFO(PLUG_LOG_DOM, "Plugin dir '%s' unavailable: %s",
                     G.plugin_dir, dir_err ? dir_err->message : "not found");
        g_clear_error(&dir_err);
        return;
    }
    const gchar *name;
    gint loaded = 0;
    while ((name = g_dir_read_name(dir)) != NULL) {
        if (!g_str_has_suffix(name, ".so")) continue;
        gchar *path = g_build_filename(G.plugin_dir, name, NULL);
        GError *err = NULL;
        if (pcv_plugin_load(path, &err))
            loaded++;
        else if (err) {
            PCV_LOG_WARN(PLUG_LOG_DOM, "Failed to load %s: %s", name, err->message);
            g_error_free(err);
        }
        g_free(path);
    }
    g_dir_close(dir);

    PCV_LOG_INFO(PLUG_LOG_DOM, "Plugin manager initialized (%d plugins, %d methods)",
                 loaded, G.registry.count);
}

/**
 * pcv_plugin_manager_shutdown:
 *
 * 모든 플러그인을 역순 정리합니다:
 *   1. 각 플러그인의 shutdown 콜백 호출 (있으면)
 *   2. GModule 핸들 닫기 (g_module_close → dlclose)
 *   3. 플러그인 디렉터리 경로 해제
 */
void pcv_plugin_manager_shutdown(void)
{
    for (gint i = 0; i < G.plugin_count; i++) {
        if (G.plugins[i].shutdown_fn)
            G.plugins[i].shutdown_fn();
        if (G.plugins[i].module)
            g_module_close(G.plugins[i].module);
    }
    g_free(G.plugin_dir);
    G.initialized = FALSE;
}

/**
 * pcv_plugin_has_handler:
 * @method: 검색할 RPC 메서드 이름
 *
 * 플러그인 레지스트리에 해당 메서드의 핸들러가 등록되어 있는지 확인합니다.
 * dispatcher.c에서 else-if 체인 끝에 플러그인 핸들러를 검색할 때 사용됩니다.
 *
 * Returns: 핸들러가 있으면 TRUE
 */
gboolean pcv_plugin_has_handler(const gchar *method)
{
    if (!G.initialized || !method) return FALSE;
    for (gint i = 0; i < G.registry.count; i++)
        if (g_strcmp0(G.registry.methods[i].method, method) == 0) return TRUE;
    return FALSE;
}

/**
 * pcv_plugin_dispatch:
 * @method:     호출할 RPC 메서드 이름
 * @params:     JSON-RPC params 객체
 * @rpc_id:     JSON-RPC 요청 ID
 * @server:     UDS 서버 포인터 (응답 전송용)
 * @connection: 소켓 연결 (응답 전송용)
 *
 * 레지스트리에서 메서드 이름으로 핸들러를 찾아 실행합니다.
 * pcv_plugin_has_handler()로 존재를 확인한 후 호출해야 합니다.
 */
void pcv_plugin_dispatch(const gchar *method, JsonObject *params,
                          const gchar *rpc_id, gpointer server,
                          GSocketConnection *connection)
{
    for (gint i = 0; i < G.registry.count; i++) {
        if (g_strcmp0(G.registry.methods[i].method, method) == 0) {
            G.registry.methods[i].handler(params, rpc_id, server, connection);
            return;
        }
    }
}

/**
 * pcv_plugin_load:
 * @path:  .so 파일의 절대 경로
 * @error: GError 반환
 *
 * 단일 플러그인을 동적 로딩합니다.
 *
 * 로딩 절차:
 *   1. g_module_open()으로 .so 파일 열기 (G_MODULE_BIND_LOCAL: 심볼 격리)
 *   2. 필수 심볼 검색: pcv_plugin_get_meta, pcv_plugin_register
 *   3. ABI 버전 검증 (meta->abi_version == PCV_PLUGIN_ABI_VERSION)
 *   4. 플러그인 배열에 등록, register 콜백 호출 (RPC 메서드 등록)
 *
 * 실패 시 에러코드:
 *   1: 최대 플러그인 수 초과 (MAX_PLUGINS)
 *   2: .so 파일 열기 실패 (경로 오류, 의존성 누락 등)
 *   3: 필수 심볼(pcv_plugin_get_meta/register) 누락
 *   4: ABI 버전 불일치
 *
 * Returns: 성공 시 TRUE
 */
gboolean pcv_plugin_load(const gchar *path, GError **error)
{
    if (G.plugin_count >= MAX_PLUGINS) {
        g_set_error(error, g_quark_from_static_string("plugin"), 1, "max plugins reached");
        return FALSE;
    }

    /* Reject symlinks to prevent arbitrary .so loading via symlink attacks */
    struct stat st;
    if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        g_set_error(error, g_quark_from_static_string("plugin"), 6,
                    "not a regular file (symlink?)");
        /* B10-M1: log basename only — 전체 경로 노출 방지 */
        gchar *bn = g_path_get_basename(path);
        PCV_LOG_WARN(PLUG_LOG_DOM, "Skipping '%s': not a regular file (symlink?)", bn);
        g_free(bn);
        pcv_audit_log(NULL, "plugin.load", path, "fail", 6, 0, "local");
        return FALSE;
    }

    GModule *mod = g_module_open(path, G_MODULE_BIND_LOCAL);
    if (!mod) {
        g_set_error(error, g_quark_from_static_string("plugin"), 2, "%s", g_module_error());
        pcv_audit_log(NULL, "plugin.load", path, "fail", 2, 0, "local");
        return FALSE;
    }
    /* B10-M2: 심볼 변수 초기화 — lookup 실패 시 garbage 사용 방지 */
    PcvPluginGetMetaFunc get_meta = NULL;
    PcvPluginRegisterFunc reg_fn = NULL;
    PcvPluginShutdownFunc shut_fn = NULL;

    if (!g_module_symbol(mod, "pcv_plugin_get_meta", (gpointer*)&get_meta) ||
        !get_meta ||
        !g_module_symbol(mod, "pcv_plugin_register", (gpointer*)&reg_fn) ||
        !reg_fn) {
        g_set_error(error, g_quark_from_static_string("plugin"), 3, "missing required symbols");
        g_module_close(mod);
        pcv_audit_log(NULL, "plugin.load", path, "fail", 3, 0, "local");
        return FALSE;
    }
    /* B10-M2: optional shutdown symbol — lookup 실패 시 NULL 유지 */
    if (!g_module_symbol(mod, "pcv_plugin_shutdown", (gpointer*)&shut_fn)) {
        shut_fn = NULL;
    }

    const PcvPluginMeta *meta = get_meta();
    if (!meta || meta->abi_version != PCV_PLUGIN_ABI_VERSION) {
        g_set_error(error, g_quark_from_static_string("plugin"), 4, "ABI version mismatch");
        g_module_close(mod);
        pcv_audit_log(NULL, "plugin.load", path, "fail", 4, 0, "local");
        return FALSE;
    }

    gint idx = G.plugin_count++;
    g_strlcpy(G.plugins[idx].name, meta->name, sizeof(G.plugins[idx].name));
    g_strlcpy(G.plugins[idx].version, meta->version, sizeof(G.plugins[idx].version));
    G.plugins[idx].module = mod;
    G.plugins[idx].shutdown_fn = shut_fn;

    /* B10-C1: register 콜백 실행 중 플러그인 owner 추적 */
    g_strlcpy(G.current_loading_plugin, meta->name, sizeof(G.current_loading_plugin));
    reg_fn(&G.registry);
    G.current_loading_plugin[0] = '\0';

    /* B10-M1: log basename only */
    gchar *bn = g_path_get_basename(path);
    PCV_LOG_INFO(PLUG_LOG_DOM, "Loaded plugin: %s v%s (%s)", meta->name, meta->version, bn);
    g_free(bn);
    pcv_audit_log(NULL, "plugin.load", meta->name, "ok", 0, 0, "local");
    return TRUE;
}

/**
 * pcv_plugin_unload:
 * @name:  언로드할 플러그인 이름
 * @error: GError 반환
 *
 * 이름으로 플러그인을 찾아 shutdown 콜백 호출 후 GModule을 닫습니다.
 *
 * 주의: 현재 레지스트리에서 해당 플러그인의 메서드를 제거하지 않습니다 (향후 개선).
 * 따라서 언로드 후에도 이전에 등록된 핸들러 포인터가 남아 있을 수 있으며,
 * 해당 메서드가 호출되면 dangling pointer로 인한 크래시가 발생합니다.
 * 런타임 언로드는 현재 사용되지 않으며, 향후 개선 시 메서드 정리가 필요합니다.
 *
 * Returns: 성공 시 TRUE, 플러그인 미발견 시 FALSE
 */
gboolean pcv_plugin_unload(const gchar *name, GError **error)
{
    for (gint i = 0; i < G.plugin_count; i++) {
        if (g_strcmp0(G.plugins[i].name, name) == 0) {
            /* B10-C1 (Phase 5): 레지스트리에서 owner == name 인 메서드 제거.
             * 이전엔 핸들러 포인터가 dangling 상태로 남아 있었음 (UAF 크래시).
             * 배열 compact 방식: 끝에서부터 역순 순회하며 swap-remove. */
            gint removed = 0;
            for (gint j = G.registry.count - 1; j >= 0; j--) {
                if (g_strcmp0(G.registry.methods[j].owner, name) == 0) {
                    if (j < G.registry.count - 1) {
                        G.registry.methods[j] = G.registry.methods[G.registry.count - 1];
                    }
                    G.registry.count--;
                    removed++;
                }
            }
            if (G.plugins[i].shutdown_fn) {
                G.plugins[i].shutdown_fn();
                G.plugins[i].shutdown_fn = NULL;  /* B10-M2: prevent double-call */
            }
            if (G.plugins[i].module) g_module_close(G.plugins[i].module);
            /* plugin 배열 compact */
            if (i < G.plugin_count - 1) {
                G.plugins[i] = G.plugins[G.plugin_count - 1];
            }
            memset(&G.plugins[G.plugin_count - 1], 0, sizeof(LoadedPlugin));
            G.plugin_count--;
            PCV_LOG_INFO(PLUG_LOG_DOM,
                "Unloaded plugin: %s (removed %d methods from registry)",
                name, removed);
            pcv_audit_log(NULL, "plugin.unload", name, "ok", 0, 0, "local");
            return TRUE;
        }
    }
    g_set_error(error, g_quark_from_static_string("plugin"), 5, "plugin '%s' not found", name);
    pcv_audit_log(NULL, "plugin.unload", name, "fail", 5, 0, "local");
    return FALSE;
}

/**
 * pcv_plugin_list:
 *
 * 현재 로딩된 모든 플러그인의 이름과 버전을 JsonArray로 반환합니다.
 * REST API와 CLI에서 플러그인 상태를 조회할 때 사용됩니다.
 *
 * Returns: (transfer full): JsonArray* (각 원소: {"name":"...", "version":"..."})
 */
JsonArray *pcv_plugin_list(void)
{
    JsonArray *arr = json_array_new();
    for (gint i = 0; i < G.plugin_count; i++) {
        JsonObject *p = json_object_new();
        json_object_set_string_member(p, "name", G.plugins[i].name);
        json_object_set_string_member(p, "version", G.plugins[i].version);
        json_array_add_object_element(arr, p);
    }
    /* 등록된 메서드 목록도 포함 */
    for (gint i = 0; i < G.registry.count; i++) {
        /* 플러그인 메서드는 별도 배열로 */
    }
    return arr;
}
