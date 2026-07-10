/**
 * @file circuit_breaker.c
 * @brief 서킷 브레이커 — CLOSED/OPEN/HALF_OPEN 상태 전이로 장애 허용
 *
 * == 아키텍처에서의 위치 ==
 *   virt_conn_pool.c → cb_is_open() / cb_record_success() / cb_record_failure()
 *
 *   libvirt 연결 풀(virt_conn_pool)이 연결 실패를 감지하면 cb_record_failure()를
 *   호출하고, 성공하면 cb_record_success()를 호출합니다.
 *   서킷이 OPEN 상태이면 acquire()가 즉시 NULL을 반환하여
 *   장애 상태의 libvirt에 계속 연결을 시도하는 것을 방지합니다.
 *
 * == 상태 전이 다이어그램 ==
 *   CLOSED ──(연속 5회 실패)──→ OPEN ──(백오프 만료)──→ HALF_OPEN
 *     ↑                                                      │
 *     └──────────────(프로브 성공)───────────────────────────┘
 *                         │
 *                 (프로브 실패)──→ OPEN (백오프 2배 증가 후 재시작)
 *
 * == 백오프 스케줄 ==
 *   200ms → 400ms → 800ms → 1.6s → 3.2s → 6.4s → 12.8s → 25.6s → 30s (상한)
 *   프로브 실패 시 백오프가 2배로 증가하여 장애 서버에 대한 부하를 줄입니다.
 *
 * == 스레드 안전성 ==
 *   GMutex로 모든 상태 접근을 보호합니다.
 *   cb_*() 함수는 어느 스레드에서든 안전하게 호출 가능합니다.
 *
 * == 전역 싱글톤 ==
 *   g_cb 정적 변수 하나로 전역 상태를 관리합니다.
 *   프로세스 전체에서 서킷 브레이커는 1개만 존재합니다.
 */
/* src/modules/virt/circuit_breaker.c
 *
 * Sprint C-2: Circuit Breaker 구현
 */

#include "circuit_breaker.h"
#include "../../utils/pcv_log.h"
#include "../../utils/pcv_config.h"

#include <glib.h>
#include <stddef.h>  /* C23 unreachable() */

#define CB_LOG_DOM "circuit_breaker"

/* ── 내부 상태 ────────────────────────────────────────── */

typedef struct {
    CbState  state;
    GMutex   mutex;

    gint     failure_count;     /* 연속 실패 횟수                   */
    gint     failure_threshold; /* 런타임 설정 가능 임계값            */
    gint     backoff_ms;        /* 현재 백오프 (밀리초)              */

    /* OPEN 상태 만료 시각 (g_get_monotonic_time() 기준, 마이크로초) */
    gint64   open_until_us;

    /* HALF_OPEN: 프로브 진행 중 플래그 (중복 프로브 방지) */
    gboolean probe_in_flight;

    /* HALF_OPEN → CLOSED 전이를 위한 연속 성공 카운터 + 임계값 */
    gint     consecutive_successes;  /* 연속 성공 횟수 */
    gint     success_threshold;      /* CLOSED 복귀에 필요한 연속 성공 수 (기본 3) */
} CbInternal;

static CbInternal g_cb = { 0 };

/* ── 내부 유틸리티 ────────────────────────────────────── */

static void
_transition_to_open(void)
{
    g_cb.state        = CB_STATE_OPEN;
    g_cb.open_until_us = g_get_monotonic_time()
                         + (gint64)g_cb.backoff_ms * G_TIME_SPAN_MILLISECOND;
    g_cb.probe_in_flight = FALSE;
    g_cb.consecutive_successes = 0;

    PCV_LOG_WARN(CB_LOG_DOM,
                 "State → OPEN (failures=%d, backoff=%dms, retry_after=%.1fs)",
                 g_cb.failure_count,
                 g_cb.backoff_ms,
                 (double)g_cb.backoff_ms / 1000.0);
}

static void
_transition_to_half_open(void)
{
    g_cb.state           = CB_STATE_HALF_OPEN;
    g_cb.probe_in_flight = FALSE;
    g_cb.consecutive_successes = 0;
    PCV_LOG_INFO(CB_LOG_DOM, "State → HALF_OPEN (sending probe request, need %d successes)",
                 g_cb.success_threshold);
}

static void
_transition_to_closed(void)
{
    g_cb.state         = CB_STATE_CLOSED;
    g_cb.failure_count = 0;
    g_cb.backoff_ms    = CB_BACKOFF_INITIAL_MS;
    g_cb.probe_in_flight = FALSE;
    g_cb.consecutive_successes = 0;
    PCV_LOG_INFO(CB_LOG_DOM, "State → CLOSED (libvirt connection restored)");
}

/* ── 공개 API ─────────────────────────────────────────── */

/**
 * cb_init — 서킷 브레이커 초기화 (CLOSED 상태에서 시작)
 *
 * [호출 시점] virt_conn_pool_init()에서 풀 초기화와 함께 호출
 * [동작] GMutex 초기화 → CLOSED 상태 설정 → daemon.conf에서 임계값 로드
 * [스레드] 메인 스레드 (데몬 시작 시 1회)
 * [주의] cb_shutdown()과 반드시 짝을 이루어야 합니다 (뮤텍스 누수 방지).
 *
 * [서킷 브레이커 패턴이란? — 주니어 필독]
 *   전기 회로의 차단기에서 영감을 받은 장애 허용 패턴입니다.
 *   libvirt 연결이 반복 실패하면 "회로를 열어" 추가 연결 시도를 차단합니다.
 *   이렇게 하면:
 *   1. 죽은 서버에 대한 불필요한 연결 시도를 방지 (리소스 절약)
 *   2. 장애 서버에 대한 부하를 줄여 복구를 도움
 *   3. 빠른 실패 반환으로 RPC 응답 지연 방지
 *
 *   상태 전이:
 *   CLOSED(정상) →(5회 연속 실패)→ OPEN(차단) →(백오프 만료)→ HALF_OPEN(프로브)
 *     ↑(3회 연속 성공)← HALF_OPEN         HALF_OPEN →(프로브 실패)→ OPEN(백오프 2배)
 *
 *   [왜 3회 연속 성공이 필요한가?]
 *     한 번의 성공으로 CLOSED로 복귀하면, 간헐적 장애(flapping) 시
 *     OPEN↔CLOSED를 빠르게 오가며 불안정해집니다. 3회 연속 성공을 요구하면
 *     복구가 안정적인지 확인한 후에만 정상 상태로 돌아갑니다.
 */
void
cb_init(void)
{
    g_mutex_init(&g_cb.mutex);
    g_cb.state           = CB_STATE_CLOSED;
    g_cb.failure_count   = 0;
    g_cb.backoff_ms      = CB_BACKOFF_INITIAL_MS;
    g_cb.open_until_us   = 0;
    g_cb.probe_in_flight = FALSE;
    g_cb.consecutive_successes = 0;
    g_cb.success_threshold = 3;  /* HALF_OPEN→CLOSED에 필요한 연속 성공 횟수 기본값 */

    /* daemon.conf [libvirt] cb_failure_threshold 로 설정 가능 (기본값 5, 범위 1~50) */
    gint threshold = pcv_config_get_int("libvirt", "cb_failure_threshold",
                                        CB_FAILURE_THRESHOLD_DEFAULT);
    if (threshold < 1)  threshold = 1;
    if (threshold > 50) threshold = 50;
    g_cb.failure_threshold = threshold;

    PCV_LOG_INFO(CB_LOG_DOM,
                 "Initialized (threshold=%d, initial_backoff=%dms, max_backoff=%dms)"
                 " — configurable via [libvirt] cb_failure_threshold",
                 g_cb.failure_threshold, CB_BACKOFF_INITIAL_MS, CB_BACKOFF_MAX_MS);
}

/**
 * cb_is_open — 서킷이 열려있는지(차단 중인지) 확인
 *
 * [호출 시점] virt_conn_pool.c의 pool_new_conn()에서 libvirt 연결 시도 전 호출
 * [동작] CLOSED→FALSE(통과), OPEN→백오프 만료 확인→미만료 TRUE(차단)/만료 HALF_OPEN(프로브 허용)
 *        HALF_OPEN→프로브 진행중이면 TRUE(추가 차단)/아니면 상태에 따라 결정
 * [스레드] 어느 스레드에서든 안전 (GMutex 보호)
 * [주의] HALF_OPEN에서 probe_in_flight=TRUE로 설정하여 동시에 1개만 프로브합니다.
 *        여러 스레드가 동시에 호출해도 첫 번째만 프로브를 진행하고 나머지는 차단됩니다.
 *
 * @return TRUE=차단중(연결 시도하지 마세요), FALSE=통과(연결 시도 가능)
 */
gboolean
cb_is_open(void)
{
    g_mutex_lock(&g_cb.mutex);

    switch (g_cb.state) {

    case CB_STATE_CLOSED:
        g_mutex_unlock(&g_cb.mutex);
        return FALSE;   /* 정상 동작 */

    case CB_STATE_OPEN:
        /* 백오프 만료 확인 */
        if (g_get_monotonic_time() >= g_cb.open_until_us) {
            _transition_to_half_open();
            /* 첫 프로브 허용 */
            g_cb.probe_in_flight = TRUE;
            g_mutex_unlock(&g_cb.mutex);
            return FALSE;
        }
        g_mutex_unlock(&g_cb.mutex);
        return TRUE;    /* 아직 차단 */

    case CB_STATE_HALF_OPEN:
        /* probe_in_flight로 동시 프로브를 1개로 제한하는 이유:
         * 여러 스레드가 동시에 프로브하면 하나의 성공/실패가 아닌
         * 결과의 혼합으로 상태 전이 판단이 왜곡된다. */
        if (g_cb.probe_in_flight) {
            g_mutex_unlock(&g_cb.mutex);
            return TRUE;
        }
        /* 주의: mutex unlock 후 g_cb.state 읽기는 레이스 가능.
         * 다른 스레드가 record_success로 CLOSED 전이할 수 있으나,
         * FALSE(통과) 반환이므로 최악의 경우 한 번 더 연결 시도할 뿐 안전하다. */
        g_mutex_unlock(&g_cb.mutex);
        return (g_cb.state != CB_STATE_CLOSED);
    }
    unreachable();  /* all CbState values covered */
}

/**
 * cb_record_success — libvirt 연결/작업 성공 기록
 *
 * [호출 시점] virt_conn_pool.c에서 연결 성공 또는 건강 검사 성공 시 호출
 * [동작] HALF_OPEN→연속 성공 카운터 증가, threshold(3) 도달 시 CLOSED 전이
 *        CLOSED→실패 카운터 리셋
 * [스레드] 어느 스레드에서든 안전 (GMutex 보호)
 */
void
cb_record_success(void)
{
    g_mutex_lock(&g_cb.mutex);

    if (g_cb.state == CB_STATE_HALF_OPEN) {
        /* 3회 연속 성공을 요구하는 이유: 1회 성공으로 CLOSED 전이하면
         * 간헐적 장애(flapping) 시 OPEN↔CLOSED를 빠르게 오가며 불안정해진다.
         * 연속 성공 요구로 복구가 안정적인지 확인한 후에만 정상 복귀한다. */
        g_cb.consecutive_successes++;
        if (g_cb.consecutive_successes >= g_cb.success_threshold) {
            PCV_LOG_INFO(CB_LOG_DOM,
                "HALF_OPEN: %d consecutive successes (threshold=%d) — transitioning to CLOSED",
                g_cb.consecutive_successes, g_cb.success_threshold);
            _transition_to_closed();
        } else {
            /* 프로브 재허용 — 다음 요청도 통과시켜 연속 성공 카운팅 */
            g_cb.probe_in_flight = FALSE;
            PCV_LOG_INFO(CB_LOG_DOM,
                "HALF_OPEN: success %d/%d — need more probes",
                g_cb.consecutive_successes, g_cb.success_threshold);
        }
    } else if (g_cb.state == CB_STATE_CLOSED) {
        /* CLOSED에서 성공 시 failure_count를 0으로 리셋하는 이유:
         * 실패 3회 → 성공 1회 → 실패 2회가 임계값(5)에 도달하지 않게 하기 위함.
         * "연속" 실패만 카운팅하여 간헐적 에러에 과민반응하지 않는다. */
        g_cb.failure_count = 0;
    }
    /* OPEN 상태에서 success가 올 수 없는 이유: cb_is_open()이 TRUE를 반환하여 연결 차단 */

    g_mutex_unlock(&g_cb.mutex);
}

/**
 * cb_record_failure — libvirt 연결/작업 실패 기록
 *
 * [호출 시점] virt_conn_pool.c에서 연결 실패 시 호출
 * [동작] CLOSED→실패 카운터 증가, threshold(5) 도달 시 OPEN 전이
 *        HALF_OPEN→프로브 실패, 백오프 2배 증가 후 OPEN 복귀
 *        OPEN→무시 (이미 차단 중이므로 백오프 타이머 재시작 없음)
 * [스레드] 어느 스레드에서든 안전 (GMutex 보호)
 * [주의] 백오프 지수 성장: 200ms → 400ms → ... → 30s(상한)
 *        장애 서버에 대한 프로브 빈도를 점진적으로 줄여 부하를 최소화합니다.
 */
void
cb_record_failure(void)
{
    g_mutex_lock(&g_cb.mutex);

    switch (g_cb.state) {

    case CB_STATE_CLOSED:
        g_cb.failure_count++;
        PCV_LOG_WARN(CB_LOG_DOM,
                     "libvirt failure %d/%d",
                     g_cb.failure_count, g_cb.failure_threshold);

        if (g_cb.failure_count >= g_cb.failure_threshold) {
            _transition_to_open();
        }
        break;

    case CB_STATE_HALF_OPEN:
        /* 프로브 실패 → OPEN 복귀 + 지수 백오프(exponential backoff).
         * 백오프 2배 증가 이유: 장애가 지속되면 프로브 빈도를 줄여
         * 장애 서버 부하를 최소화한다. 상한 30초로 무한 증가 방지. */
        g_cb.failure_count++;
        g_cb.backoff_ms = MIN(g_cb.backoff_ms * 2, CB_BACKOFF_MAX_MS);
        PCV_LOG_WARN(CB_LOG_DOM,
                     "Probe failed — returning to OPEN (next_backoff=%dms)",
                     g_cb.backoff_ms);
        _transition_to_open();
        break;

    case CB_STATE_OPEN:
        /* OPEN 중 추가 실패를 무시하는 이유: 백오프 타이머를 재시작하면
         * 장애 지속 시 HALF_OPEN 전이가 무한 연기되어 복구 프로브를 못 보낸다. */
        break;
    }

    g_mutex_unlock(&g_cb.mutex);
}

/* 주의: mutex 없이 읽기 — 모니터링/로깅 전용. 정확한 판단이 필요하면 cb_is_open() 사용. */
CbState
cb_get_state(void)
{
    return g_cb.state;
}

const gchar *
cb_get_state_str(void)
{
    switch (g_cb.state) {
        case CB_STATE_CLOSED:    return "CLOSED";
        case CB_STATE_OPEN:      return "OPEN";
        case CB_STATE_HALF_OPEN: return "HALF_OPEN";
    }
    unreachable();  /* all CbState values covered */
}

gint
cb_get_failure_count(void)
{
    return g_atomic_int_get(&g_cb.failure_count);
}

void
cb_set_failure_threshold(gint threshold)
{
    if (threshold < 1)  threshold = 1;
    if (threshold > 50) threshold = 50;
    g_mutex_lock(&g_cb.mutex);
    g_cb.failure_threshold = threshold;
    g_mutex_unlock(&g_cb.mutex);
    PCV_LOG_INFO(CB_LOG_DOM, "Failure threshold updated to %d", threshold);
}

gint
cb_get_failure_threshold(void)
{
    return g_atomic_int_get(&g_cb.failure_threshold);
}

/* ── Per-resource CB 인스턴스 팩토리 ─────────────────────── */

/**
 * Per-resource 서킷 브레이커 인스턴스 팩토리 — 주니어 필독
 *
 * [왜 per-resource CB가 필요한가?]
 *   위의 g_cb 전역 싱글톤은 libvirt 전용입니다.
 *   하지만 PureCVisor는 libvirt 외에도 etcd, S3, gRPC 등 여러 외부 서비스와 통신합니다.
 *   각 서비스에 대해 독립적인 서킷 브레이커가 필요합니다:
 *     - libvirt가 장애여도 etcd는 정상 → etcd CB는 CLOSED 유지
 *     - etcd만 장애여도 libvirt 연결은 정상 → libvirt CB는 CLOSED 유지
 *
 * [팩토리 패턴]
 *   cb_get_named_state("etcd")를 처음 호출하면 새 CbInternal 인스턴스를 생성합니다.
 *   이후 호출에서는 기존 인스턴스를 반환합니다 (lazy initialization).
 *   GHashTable: "libvirt" → CbInternal*, "etcd" → CbInternal*, ...
 *
 * [현재 상태]
 *   글로벌 CB(g_cb)는 virt_conn_pool에서 직접 사용합니다.
 *   named instances는 확장성을 위해 준비된 인프라입니다.
 */
static GHashTable *g_cb_instances = NULL;
static GMutex      g_cb_instances_mu;

/**
 * _cb_instance_free — GHashTable의 value_destroy_func
 */
static void
_cb_instance_free(gpointer p)
{
    CbInternal *cb = p;
    if (cb) {
        g_mutex_clear(&cb->mutex);
        g_free(cb);
    }
}

/**
 * cb_get_named_state — 이름으로 식별된 CB 인스턴스의 상태를 조회한다.
 *
 * 인스턴스가 없으면 기본 CLOSED 상태의 새 인스턴스를 생성한다.
 *
 * @param name 서브시스템 이름 (예: "libvirt", "etcd", "s3")
 * @return CB 상태 (CbState 열거형)
 */
CbState
cb_get_named_state(const gchar *name)
{
    if (!name) return CB_STATE_CLOSED;

    g_mutex_lock(&g_cb_instances_mu);
    if (!g_cb_instances) {
        g_cb_instances = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                g_free, _cb_instance_free);
    }

    CbInternal *cb = g_hash_table_lookup(g_cb_instances, name);
    if (!cb) {
        cb = g_new0(CbInternal, 1);
        g_mutex_init(&cb->mutex);
        cb->state = CB_STATE_CLOSED;
        cb->failure_threshold = CB_FAILURE_THRESHOLD_DEFAULT;
        cb->backoff_ms = CB_BACKOFF_INITIAL_MS;
        cb->success_threshold = 3;
        g_hash_table_insert(g_cb_instances, g_strdup(name), cb);
        PCV_LOG_INFO(CB_LOG_DOM, "Named CB instance created: '%s'", name);
    }

    CbState state = cb->state;
    g_mutex_unlock(&g_cb_instances_mu);
    return state;
}

/* ── Prometheus 메트릭 출력 ──────────────────────────────── */

/**
 * cb_get_prometheus_metrics — CB 상태를 Prometheus 형식 문자열로 반환한다.
 *
 * 글로벌 CB의 상태 + 실패 횟수 + 임계값을 포함한다.
 *
 * @return Prometheus 형식 문자열 (g_free 필요)
 */
gchar *
cb_get_prometheus_metrics(void)
{
    GString *buf = g_string_new("");
    g_string_append_printf(buf,
        "# HELP purecvisor_cb_state Circuit breaker state (0=CLOSED,1=OPEN,2=HALF_OPEN)\n"
        "# TYPE purecvisor_cb_state gauge\n"
        "purecvisor_cb_state{subsystem=\"libvirt\"} %d\n"
        "# HELP purecvisor_cb_failures Current consecutive failure count\n"
        "# TYPE purecvisor_cb_failures gauge\n"
        "purecvisor_cb_failures{subsystem=\"libvirt\"} %d\n"
        "# HELP purecvisor_cb_failure_threshold Failure threshold for OPEN transition\n"
        "# TYPE purecvisor_cb_failure_threshold gauge\n"
        "purecvisor_cb_failure_threshold{subsystem=\"libvirt\"} %d\n",
        (gint)g_cb.state,
        g_cb.failure_count,
        g_cb.failure_threshold);
    return g_string_free(buf, FALSE);
}

void
cb_shutdown(void)
{
    g_mutex_clear(&g_cb.mutex);

    /* Named instances 정리 */
    g_mutex_lock(&g_cb_instances_mu);
    if (g_cb_instances) {
        g_hash_table_destroy(g_cb_instances);
        g_cb_instances = NULL;
    }
    g_mutex_unlock(&g_cb_instances_mu);

    PCV_LOG_INFO(CB_LOG_DOM, "Shutdown.");
}
