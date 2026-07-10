/**
 * @file cancellable_map.c
 * @brief GCancellable 맵 — 진행 중 비동기 작업 추적 + 취소 지원
 *
 * == 아키텍처에서의 위치 ==
 *   GTask 워커 스레드 생성 시 → cmap_register(vm_name, cancellable)
 *   VM 삭제/drain 시          → cmap_cancel(vm_name) 또는 cmap_cancel_all()
 *   GTask 완료 콜백에서       → cmap_remove(vm_name)
 *
 * == 목적 ==
 *   GTask 기반 비동기 작업(VM create/start 등)에 전달된 GCancellable을
 *   VM 이름으로 조회하여 강제 취소할 수 있게 합니다.
 *
 *   사용 사례:
 *   - VM 삭제 요청 시 진행 중인 create/start GTask 강제 취소
 *   - 데몬 graceful drain 시 모든 장기 작업 일괄 취소 (cmap_cancel_all)
 *
 * == 내부 구조 ==
 *   GHashTable (gchar* → GCancellable*):
 *   - 키: VM 이름 (g_strdup 복사본, destroy 시 g_free)
 *   - 값: GCancellable* (ref 증가하여 저장, destroy 시 g_object_unref)
 *   - GMutex로 모든 접근을 직렬화 → 스레드 안전
 *
 * == 메모리 관리 ==
 *   - cmap_register(): 키 복사(g_strdup) + 값 ref(g_object_ref) → 맵이 소유
 *   - cmap_remove(): g_hash_table_remove → 키 g_free + 값 g_object_unref 자동
 *   - cmap_shutdown(): 모든 항목 취소 후 해시테이블 destroy
 *
 * == 주의사항 ==
 *   GTask 완료 콜백에서 반드시 cmap_remove()를 호출하여 맵 누수를 방지하세요.
 */
/* src/modules/virt/cancellable_map.c
 *
 * Sprint C-2 / GIO P4: GCancellable 해시맵 구현
 */

#include "cancellable_map.h"
#include "../../utils/pcv_log.h"

#include <glib.h>

#define CMAP_LOG_DOM "cancellable_map"

/* ── 내부 상태 ────────────────────────────────────────── */

typedef struct {
    GHashTable *map;    /* gchar* → GCancellable* (값은 floating ref 없음) */
    GMutex      mutex;
    gboolean    initialized;
} CancellableMap;

static CancellableMap g_cmap = { 0 };

/* ── 공개 API ─────────────────────────────────────────── */

/**
 * cmap_init — 취소 맵 초기화 (해시테이블 + 뮤텍스 생성)
 *
 * [호출 시점] main.c 데몬 초기화 시 1회 호출
 * [동작] GMutex 초기화 → GHashTable(vm_name → GCancellable*) 생성
 * [스레드] 메인 스레드 (데몬 시작 시)
 */
void
cmap_init(void)
{
    g_mutex_init(&g_cmap.mutex);
    /* 키: g_strdup 복사본 (g_free로 해제), 값: GCancellable* (g_object_unref로 해제) */
    g_cmap.map = g_hash_table_new_full(
        g_str_hash, g_str_equal,
        g_free,
        (GDestroyNotify)g_object_unref);
    g_cmap.initialized = TRUE;
    PCV_LOG_INFO(CMAP_LOG_DOM, "Initialized.");
}

void
cmap_shutdown(void)
{
    if (!g_cmap.initialized) return;

    g_mutex_lock(&g_cmap.mutex);

    /* 남은 항목 전부 취소 후 맵 해제 */
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, g_cmap.map);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        GCancellable *c = (GCancellable *)value;
        if (!g_cancellable_is_cancelled(c))
            g_cancellable_cancel(c);
    }
    guint sz = g_hash_table_size(g_cmap.map);
    g_hash_table_destroy(g_cmap.map);
    g_cmap.map = NULL;

    g_mutex_unlock(&g_cmap.mutex);
    g_mutex_clear(&g_cmap.mutex);
    g_cmap.initialized = FALSE;

    PCV_LOG_INFO(CMAP_LOG_DOM, "Shutdown (cancelled %u pending).", sz);
}

/**
 * cmap_register — VM 이름으로 GCancellable을 등록 (비동기 작업 추적)
 *
 * [호출 시점] GTask 워커 스레드 생성 시 (vm.create, vm.start 등)
 * [동작] vm_name → GCancellable 매핑을 해시테이블에 저장
 *        동일 키가 있으면 기존 값이 g_object_unref로 자동 해제됨
 * [스레드] 어느 스레드에서든 안전 (GMutex 보호)
 * [주의] GCancellable은 g_object_ref로 참조 증가되어 저장됩니다.
 *        작업 완료 후 반드시 cmap_remove()를 호출하여 맵 누수를 방지하세요.
 */
void
cmap_register(const gchar *vm_name, GCancellable *cancellable)
{
    g_return_if_fail(vm_name && cancellable);
    if (!g_cmap.initialized) return;

    g_mutex_lock(&g_cmap.mutex);
    /* g_hash_table_insert: 동일 키 존재 시 기존 값 자동 destroy (g_object_unref) */
    g_hash_table_insert(g_cmap.map,
                        g_strdup(vm_name),
                        g_object_ref(cancellable));
    PCV_LOG_DEBUG(CMAP_LOG_DOM, "Registered cancellable for '%s'.", vm_name);
    g_mutex_unlock(&g_cmap.mutex);
}

/**
 * cmap_cancel — 특정 VM의 진행 중 비동기 작업을 취소
 *
 * [호출 시점] vm.delete 요청 시 진행 중인 create/start GTask 강제 취소
 * [동작] vm_name으로 GCancellable 조회 → g_cancellable_cancel() 호출
 *        GTask 워커가 g_cancellable_is_cancelled()를 확인하여 조기 반환
 * [스레드] 어느 스레드에서든 안전 (GMutex 보호)
 * [주의] cancel 후에도 맵에서 제거되지 않습니다. cmap_remove()는 별도 호출 필요.
 */
void
cmap_cancel(const gchar *vm_name)
{
    g_return_if_fail(vm_name);
    if (!g_cmap.initialized) return;

    g_mutex_lock(&g_cmap.mutex);
    GCancellable *c = g_hash_table_lookup(g_cmap.map, vm_name);
    if (c) {
        if (!g_cancellable_is_cancelled(c)) {
            g_cancellable_cancel(c);
            PCV_LOG_INFO(CMAP_LOG_DOM, "Cancelled operation for '%s'.", vm_name);
        }
    } else {
        PCV_LOG_DEBUG(CMAP_LOG_DOM, "cmap_cancel: no entry for '%s'.", vm_name);
    }
    g_mutex_unlock(&g_cmap.mutex);
}

void
cmap_remove(const gchar *vm_name)
{
    g_return_if_fail(vm_name);
    if (!g_cmap.initialized) return;

    g_mutex_lock(&g_cmap.mutex);
    gboolean removed = g_hash_table_remove(g_cmap.map, vm_name);
    g_mutex_unlock(&g_cmap.mutex);

    if (removed)
        PCV_LOG_DEBUG(CMAP_LOG_DOM, "Removed cancellable for '%s'.", vm_name);
}

/**
 * cmap_cancel_all — 모든 진행 중 비동기 작업을 일괄 취소
 *
 * [호출 시점] 데몬 graceful drain 시 (drain.c에서 호출)
 * [동작] 해시테이블 전체를 순회하며 모든 GCancellable을 cancel
 * [스레드] 메인 스레드 (drain 시그널 핸들러에서)
 * [주의] 이미 취소된 GCancellable은 중복 cancel해도 안전합니다 (no-op).
 */
void
cmap_cancel_all(void)
{
    if (!g_cmap.initialized) return;

    g_mutex_lock(&g_cmap.mutex);
    GHashTableIter iter;
    gpointer key, value;
    guint count = 0;
    g_hash_table_iter_init(&iter, g_cmap.map);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        GCancellable *c = (GCancellable *)value;
        if (!g_cancellable_is_cancelled(c)) {
            g_cancellable_cancel(c);
            count++;
        }
    }
    g_mutex_unlock(&g_cmap.mutex);

    if (count > 0)
        PCV_LOG_INFO(CMAP_LOG_DOM, "cmap_cancel_all: cancelled %u operations.", count);
}

guint
cmap_size(void)
{
    if (!g_cmap.initialized) return 0;
    g_mutex_lock(&g_cmap.mutex);
    guint sz = g_hash_table_size(g_cmap.map);
    g_mutex_unlock(&g_cmap.mutex);
    return sz;
}
