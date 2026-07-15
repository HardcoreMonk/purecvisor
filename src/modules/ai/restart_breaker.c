/**
 * @file restart_breaker.c
 * @brief AF-1 후속 — self-healing VM 재시작 서킷 브레이커 (VM(uuid) 단위)
 *
 * == 아키텍처에서의 위치 ==
 *   self_healing.c
 *     _execute_action() → rb_allow(uuid)   [재시작 dispatch 전 게이트]
 *     _vm_restart_worker() → rb_record(uuid, success)  [워커 결과 되먹임]
 *
 *   AF-1 restart 실배선은 virDomainCreate() 를 워커 스레드에서 비동기 수행하므로
 *   create 실패가 트리거 시점의 동기 안전장치로 되먹여지지 않는다. 이 모듈이
 *   VM 별 실패를 누적해 반복 실패 시 재시작을 차단(OPEN)함으로써 그 되먹임 고리를
 *   닫는다.
 *
 * == 상태 전이 (VM uuid 별 독립) ==
 *   CLOSED ──(연속 threshold 회 실패)──→ OPEN ──(cooldown 경과)──→ HALF_OPEN
 *     ↑                                                                │
 *     └────────────────(프로브 성공)───────────────────────────────────┘
 *                              │
 *                      (프로브 실패)──→ OPEN (cooldown 재무장)
 *
 *   성공/running-guard skip 은 실패 카운터를 리셋한다(성공 재시작 시 리셋).
 *
 * == 스레드 안전 ==
 *   g_rb.mutex 로 테이블·설정·엔트리 상태를 모두 보호한다. self_healing 의 G.mu
 *   를 잡은 채 rb_allow() 를 호출해도, 이 모듈은 오직 g_rb.mutex 만 잡으므로
 *   G.mu → g_rb.mutex 단방향 중첩만 존재한다(역방향 없음 → 데드락 불가).
 *
 * == circuit_breaker.c 와의 관계 ==
 *   상태 어휘(CbState) 만 재사용한다. 구동 로직은 독립이다. 이유는 헤더 주석 참조.
 */
#include "restart_breaker.h"
#include "utils/pcv_log.h"

#include <glib.h>

#define RB_LOG_DOM "restart_breaker"

/* ── VM 별 브레이커 엔트리 ─────────────────────────────── */
typedef struct {
    CbState  state;             /* CLOSED / OPEN / HALF_OPEN */
    gint     failure_count;     /* 연속 실패 횟수 */
    gint64   open_until_us;     /* OPEN 만료 시각 (g_get_monotonic_time 기준, us) */
    gboolean probe_in_flight;   /* HALF_OPEN: 프로브 진행 중 (중복 프로브 차단) */
} RbEntry;

/* ── 모듈 전역 상태 ────────────────────────────────────── */
static struct {
    GHashTable *table;          /* uuid(gchar*) → RbEntry* */
    GMutex      mutex;
    gint        threshold;      /* 연속 실패 임계값 */
    gint        cooldown_sec;   /* OPEN 차단 시간(초) */
    gboolean    initialized;
} g_rb = { 0 };

/* ── 내부 유틸 ─────────────────────────────────────────── */

/* 호출자는 g_rb.mutex 를 보유해야 한다. 미등록 시 CLOSED 엔트리를 새로 만든다. */
static RbEntry *
_rb_entry(const gchar *uuid)
{
    RbEntry *e = g_hash_table_lookup(g_rb.table, uuid);
    if (!e) {
        e = g_new0(RbEntry, 1);
        e->state = CB_STATE_CLOSED;
        g_hash_table_insert(g_rb.table, g_strdup(uuid), e);
    }
    return e;
}

/* 호출자는 g_rb.mutex 를 보유해야 한다. */
static void
_rb_open(RbEntry *e, const gchar *uuid)
{
    e->state           = CB_STATE_OPEN;
    e->open_until_us    = g_get_monotonic_time()
                          + (gint64)g_rb.cooldown_sec * G_USEC_PER_SEC;
    e->probe_in_flight  = FALSE;
    PCV_LOG_WARN(RB_LOG_DOM,
        "VM '%s' 재시작 브레이커 OPEN (연속 실패 %d/%d, cooldown %ds)",
        uuid, e->failure_count, g_rb.threshold, g_rb.cooldown_sec);
}

/* ── 생명주기 ──────────────────────────────────────────── */

void
rb_init(void)
{
    g_mutex_init(&g_rb.mutex);
    g_rb.table        = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              g_free, g_free);
    g_rb.threshold    = RESTART_BREAKER_THRESHOLD_DEFAULT;
    g_rb.cooldown_sec = RESTART_BREAKER_COOLDOWN_SEC_DEFAULT;
    g_rb.initialized  = TRUE;
    PCV_LOG_INFO(RB_LOG_DOM,
        "Initialized (threshold=%d, cooldown=%ds) — [ai] restart_breaker_threshold / restart_breaker_cooldown_sec 로 설정",
        g_rb.threshold, g_rb.cooldown_sec);
}

void
rb_shutdown(void)
{
    if (!g_rb.initialized) return;
    g_mutex_lock(&g_rb.mutex);
    if (g_rb.table) {
        g_hash_table_destroy(g_rb.table);
        g_rb.table = NULL;
    }
    g_rb.initialized = FALSE;
    g_mutex_unlock(&g_rb.mutex);
    g_mutex_clear(&g_rb.mutex);
}

void
rb_configure(gint threshold, gint cooldown_sec)
{
    if (threshold < 1)  threshold = 1;
    if (threshold > 50) threshold = 50;
    if (cooldown_sec < 0) cooldown_sec = 0;
    g_mutex_lock(&g_rb.mutex);
    g_rb.threshold    = threshold;
    g_rb.cooldown_sec = cooldown_sec;
    g_mutex_unlock(&g_rb.mutex);
    PCV_LOG_INFO(RB_LOG_DOM, "Configured (threshold=%d, cooldown=%ds)",
                 threshold, cooldown_sec);
}

gint
rb_get_threshold(void)
{
    g_mutex_lock(&g_rb.mutex);
    gint t = g_rb.threshold;
    g_mutex_unlock(&g_rb.mutex);
    return t;
}

gint
rb_get_cooldown_sec(void)
{
    g_mutex_lock(&g_rb.mutex);
    gint c = g_rb.cooldown_sec;
    g_mutex_unlock(&g_rb.mutex);
    return c;
}

/* ── 게이트/피드백 ─────────────────────────────────────── */

/* PCV_SAFETY_CONTROL: restart-breaker — 임계 초과 실패 시 재시작 차단(FALSE 반환) (AIO-2) */
gboolean
rb_allow(const gchar *uuid)
{
    if (!uuid || !*uuid) return TRUE;   /* 브레이커 미적용 — 기존 동작 보존 */
    if (!g_rb.initialized) return TRUE;

    g_mutex_lock(&g_rb.mutex);
    RbEntry *e = _rb_entry(uuid);
    gboolean allow;

    switch (e->state) {
    case CB_STATE_CLOSED:
        allow = TRUE;
        break;

    case CB_STATE_OPEN:
        if (g_get_monotonic_time() >= e->open_until_us) {
            /* cooldown 경과 → HALF_OPEN 전이, 프로브 1회 허용 */
            e->state           = CB_STATE_HALF_OPEN;
            e->probe_in_flight = TRUE;
            PCV_LOG_INFO(RB_LOG_DOM,
                "VM '%s' 재시작 브레이커 HALF_OPEN — 프로브 1회 허용", uuid);
            allow = TRUE;
        } else {
            allow = FALSE;   /* 아직 차단 중 */
        }
        break;

    case CB_STATE_HALF_OPEN:
        if (e->probe_in_flight) {
            allow = FALSE;   /* 프로브 진행 중 — 중복 프로브 차단 */
        } else {
            e->probe_in_flight = TRUE;
            allow = TRUE;
        }
        break;

    default:
        allow = TRUE;
        break;
    }

    g_mutex_unlock(&g_rb.mutex);
    return allow;
}

void
rb_record(const gchar *uuid, gboolean success)
{
    if (!uuid || !*uuid) return;
    if (!g_rb.initialized) return;

    g_mutex_lock(&g_rb.mutex);
    RbEntry *e = _rb_entry(uuid);

    if (success) {
        /* 성공/running-guard skip → 카운터 리셋, HALF_OPEN 이면 CLOSED 복귀 */
        gboolean was_open = (e->state != CB_STATE_CLOSED);
        e->state           = CB_STATE_CLOSED;
        e->failure_count   = 0;
        e->probe_in_flight = FALSE;
        e->open_until_us   = 0;
        if (was_open) {
            PCV_LOG_INFO(RB_LOG_DOM,
                "VM '%s' 재시작 성공 — 브레이커 CLOSED 복귀", uuid);
        }
    } else {
        switch (e->state) {
        case CB_STATE_HALF_OPEN:
            /* 프로브 실패 → 즉시 재-OPEN (cooldown 재무장) */
            e->failure_count++;
            _rb_open(e, uuid);
            break;
        case CB_STATE_CLOSED:
            e->failure_count++;
            if (e->failure_count >= g_rb.threshold)
                _rb_open(e, uuid);
            else
                PCV_LOG_WARN(RB_LOG_DOM,
                    "VM '%s' 재시작 실패 %d/%d", uuid,
                    e->failure_count, g_rb.threshold);
            break;
        case CB_STATE_OPEN:
        default:
            /* OPEN 중 실패 되먹임은 무시 (dispatch 되지 않았어야 함) */
            break;
        }
    }

    g_mutex_unlock(&g_rb.mutex);
}

/* AIO-2: 프로브 토큰 회수 — 워커가 결과 없이 중단된 경우(conn 획득/도메인 조회 실패
 * 등 rb_feedback=0). 실패 카운트 없이 HALF_OPEN→OPEN 으로 되돌려 토큰을 회수하고
 * cooldown 을 재무장한다. probe_in_flight 를 리셋해 다음 프로브를 가능케 한다.
 * HALF_OPEN·프로브 진행 중이 아니면 no-op(토큰 미소비 상태에서 호출돼도 안전). */
void
rb_release_probe(const gchar *uuid)
{
    if (!uuid || !*uuid) return;
    if (!g_rb.initialized) return;

    g_mutex_lock(&g_rb.mutex);
    RbEntry *e = g_hash_table_lookup(g_rb.table, uuid);
    if (e && e->state == CB_STATE_HALF_OPEN && e->probe_in_flight) {
        e->state          = CB_STATE_OPEN;
        e->open_until_us   = g_get_monotonic_time()
                             + (gint64)g_rb.cooldown_sec * G_USEC_PER_SEC;
        e->probe_in_flight = FALSE;
        PCV_LOG_INFO(RB_LOG_DOM,
            "VM '%s' 프로브 무결과 중단 — OPEN 유지, cooldown 재무장(토큰 회수)", uuid);
    }
    g_mutex_unlock(&g_rb.mutex);
}

/* ── 진단/테스트 조회 ──────────────────────────────────── */

CbState
rb_state(const gchar *uuid)
{
    if (!uuid || !*uuid || !g_rb.initialized) return CB_STATE_CLOSED;
    g_mutex_lock(&g_rb.mutex);
    RbEntry *e = g_hash_table_lookup(g_rb.table, uuid);
    CbState s = e ? e->state : CB_STATE_CLOSED;
    g_mutex_unlock(&g_rb.mutex);
    return s;
}

gint
rb_failure_count(const gchar *uuid)
{
    if (!uuid || !*uuid || !g_rb.initialized) return 0;
    g_mutex_lock(&g_rb.mutex);
    RbEntry *e = g_hash_table_lookup(g_rb.table, uuid);
    gint c = e ? e->failure_count : 0;
    g_mutex_unlock(&g_rb.mutex);
    return c;
}
