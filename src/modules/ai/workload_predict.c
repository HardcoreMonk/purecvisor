/**
 * @file workload_predict.c
 * @brief AI Ops Phase 2 — EMA + 선형 회귀 워크로드 예측
 *
 * [파일 역할]
 *   현재 CPU/MEM 사용률의 추세를 분석하여 5분 후 값을 예측하는 모듈.
 *   지수 이동 평균(EMA)으로 노이즈를 제거하고, 최소제곱법 선형 회귀로
 *   추세(기울기)를 계산한 뒤, 두 값을 합산하여 예측값을 산출합니다.
 *
 * [아키텍처 위치]
 *   ai_agent.c (5초 타이머)
 *     -> pcv_predict_evaluate() — 현재 메트릭 읽기 + 예측 갱신
 *   self_healing.c
 *     -> pcv_healing_on_prediction() — 예측값이 임계값 초과 시 사전 대응
 *
 * [예측 알고리즘 — 2단계 결합]
 *   1. EMA(지수 이동 평균):
 *      ema_new = alpha * current + (1 - alpha) * ema_old
 *      alpha=0.3 → 최근 값에 30% 가중, 과거에 70% 가중 (노이즈 감쇠)
 *
 *   2. 선형 회귀(최소제곱법):
 *      60개 샘플로 기울기(slope) 계산 → 샘플당 변화량
 *      slope = (N*Σxy - Σx*Σy) / (N*Σx² - (Σx)²)
 *
 *   3. 예측: EMA + slope × horizon(60샘플 = 5분)
 *      → [0, 100] 범위로 클램핑
 *
 * [스레드 안전]
 *   GMutex(G.mu)로 노드별 예측 상태 접근을 직렬화합니다.
 *
 * [외부 의존성] 없음 (순수 C + GLib + math.h)
 */
#include "workload_predict.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include "modules/daemons/prometheus_exporter.h"
#include "modules/daemons/ebpf_telemetry.h"
#include "modules/ai/self_healing.h"   /* BUG-20: prediction → healing 파이프라인 */
#include "utils/pcv_log.h"

/*
 * ============================================================================
 *  [주니어 개발자 필독] 워크로드 예측 핵심 개념
 * ============================================================================
 *
 *  EMA (지수 이동 평균):
 *    ema_new = alpha × 현재값 + (1-alpha) × ema_old
 *    alpha=0.3이면 최근 값에 30%, 과거 누적에 70% 가중합니다.
 *    순간 스파이크(노이즈)를 부드럽게 깎아내는 효과가 있습니다.
 *
 *  OLS (최소제곱법 선형 회귀):
 *    60개 샘플에 직선 y = slope×x + b를 피팅합니다.
 *    slope > 0이면 상승 추세, slope < 0이면 하락 추세.
 *    5분간의 추세를 연장하여 "5분 후 CPU/MEM가 몇 %일 것인가"를 예측합니다.
 *
 *  예측 공식: predicted = EMA + slope × horizon(60샘플)
 *    → [0, 100] 범위로 클램핑 (퍼센트이므로)
 *
 *  사용처: self_healing.c에서 예측값이 임계값(85%/90%)을 초과하면
 *  사전 대응(마이그레이션 등)을 트리거합니다.
 * ============================================================================
 */

#define PREDICT_LOG_DOM  "predict"
constexpr int    PREDICT_WINDOW  = 60;   /* 회귀 분석용 샘플 수: 60 × 5초 = 5분 윈도우 */
constexpr int    PREDICT_HORIZON = 60;   /* 예측 수평선: 60 샘플 앞 = 5분 후 예측 */
constexpr double EMA_ALPHA       = 0.3;  /* EMA 평활 계수: 0.3 = 최근 값 30% 반영 */
constexpr int    MAX_NODES       = 8;    /* 최대 추적 노드 수 (local + 클러스터 피어) */

/* ── 노드별 예측 상태 구조체 ─────────────────────────────────── */

/**
 * NodeForecast — 단일 노드의 CPU/MEM 예측 상태
 *
 * cpu_ring/mem_ring: 최근 60개 샘플을 저장하는 순환 버퍼
 * cpu_ema/mem_ema: 지수 이동 평균 (노이즈 제거된 현재 수준)
 * cpu_trend/mem_trend: 선형 회귀 기울기 (샘플당 변화량, 양수=증가 추세)
 * cpu_predicted_5m/mem_predicted_5m: 5분 후 예측값 (0~100%)
 */
typedef struct {
    gchar    ip[32];              /* 노드 IP (클러스터 식별용, "local"=자기 자신) */
    gchar    name[32];            /* 노드 이름 */
    /* CPU 예측 */
    gdouble  cpu_ring[PREDICT_WINDOW]; /* CPU% 순환 버퍼 */
    gdouble  cpu_ema;             /* CPU EMA (지수 이동 평균) */
    gdouble  cpu_trend;           /* CPU 기울기 (샘플당 변화량, 양수=상승) */
    gdouble  cpu_predicted_5m;    /* CPU 5분 후 예측값 (0~100%) */
    /* MEM 예측 */
    gdouble  mem_ring[PREDICT_WINDOW]; /* MEM% 순환 버퍼 */
    gdouble  mem_ema;             /* MEM EMA */
    gdouble  mem_trend;           /* MEM 기울기 */
    gdouble  mem_predicted_5m;    /* MEM 5분 후 예측값 (0~100%) */
    /* 상태 관리 */
    gint     pos;                 /* 순환 버퍼 다음 쓰기 위치 */
    gint     count;               /* 현재 저장된 샘플 수 */
    gboolean ema_primed;          /* EMA 초기값 설정 완료 여부 (첫 샘플로 초기화) */
} NodeForecast;

/* ── 모듈 전역 상태 ─────────────────────────────────────────── */

static struct {
    NodeForecast nodes[MAX_NODES]; /* 노드별 예측 상태 배열 */
    gint         node_count;       /* 현재 추적 중인 노드 수 */
    GMutex       mu;               /* 예측 상태 보호 뮤텍스 */
    gboolean     initialized;      /* 초기화 완료 플래그 */
} G = {0};

/* ── 선형 회귀 기울기 계산 (최소제곱법) ──────────────────────── */

/**
 * _trend_slope:
 * @ring:  순환 버퍼 배열
 * @pos:   현재 쓰기 위치 (가장 최신 데이터의 다음 위치)
 * @count: 저장된 샘플 수
 *
 * 최소제곱법(OLS)으로 선형 회귀 기울기를 계산합니다.
 * x축: 시간 인덱스 (0, 1, 2, ..., n-1)
 * y축: 메트릭 값 (CPU% 또는 MEM%)
 *
 * 기울기 공식: slope = (N*Σxy - Σx*Σy) / (N*Σx² - (Σx)²)
 * 양수: 상승 추세, 음수: 하락 추세, 0에 가까우면: 안정
 *
 * 최소 10 샘플이 필요합니다 (이하에서는 0.0 반환).
 *
 * Returns: 샘플당 변화량 (기울기)
 */
static gdouble
_trend_slope(const gdouble *ring, gint pos, gint count)
{
    if (count < 10) return 0.0;

    gint n = count < PREDICT_WINDOW ? count : PREDICT_WINDOW;
    gdouble sum_x = 0, sum_y = 0, sum_xy = 0, sum_xx = 0;

    for (gint i = 0; i < n; i++) {
        gint idx = (pos - n + i + PREDICT_WINDOW) % PREDICT_WINDOW;
        gdouble x = (gdouble)i;
        gdouble y = ring[idx];
        sum_x += x;
        sum_y += y;
        sum_xy += x * y;
        sum_xx += x * x;
    }

    gdouble denom = (gdouble)n * sum_xx - sum_x * sum_x;
    if (fabs(denom) < 1e-9) return 0.0;

    return ((gdouble)n * sum_xy - sum_x * sum_y) / denom;
}

/* ── 노드 검색 또는 생성 ─────────────────────────────────────── */

/**
 * _get_node:
 * @ip:   노드 IP ("local"이면 자기 자신)
 * @name: 노드 이름 (표시용)
 *
 * IP로 기존 노드를 찾거나, 없으면 새 NodeForecast를 생성합니다.
 * MAX_NODES(8) 초과 시 NULL을 반환합니다.
 *
 * Returns: NodeForecast 포인터, 노드 한도 초과 시 NULL
 */
static NodeForecast *
_get_node(const gchar *ip, const gchar *name)
{
    for (gint i = 0; i < G.node_count; i++) {
        if (g_strcmp0(G.nodes[i].ip, ip) == 0)
            return &G.nodes[i];
    }
    if (G.node_count >= MAX_NODES) return NULL;

    NodeForecast *n = &G.nodes[G.node_count++];
    memset(n, 0, sizeof(*n));
    g_strlcpy(n->ip, ip, sizeof(n->ip));
    if (name) g_strlcpy(n->name, name, sizeof(n->name));
    return n;
}

/* ── 샘플 추가 + 예측 갱신 ───────────────────────────────────── */

/**
 * _push_sample:
 * @n:   대상 노드 예측 상태
 * @cpu: 현재 CPU 사용률 (%)
 * @mem: 현재 MEM 사용률 (%)
 *
 * 새 샘플을 링 버퍼에 추가하고, EMA + 추세 + 5분 예측을 갱신합니다.
 *
 * 갱신 순서:
 *   1. 링 버퍼에 값 저장 (pos 순환)
 *   2. EMA 갱신 (첫 샘플이면 현재값으로 초기화)
 *   3. 선형 회귀 기울기(trend) 재계산
 *   4. 예측: ema + trend × 60(5분) → [0, 100] 클램핑
 */
static void
_push_sample(NodeForecast *n, gdouble cpu, gdouble mem)
{
    /* Ring buffer */
    n->cpu_ring[n->pos] = cpu;
    n->mem_ring[n->pos] = mem;
    n->pos = (n->pos + 1) % PREDICT_WINDOW;
    if (n->count < PREDICT_WINDOW) n->count++;

    /* EMA */
    if (!n->ema_primed) {
        n->cpu_ema = cpu;
        n->mem_ema = mem;
        n->ema_primed = TRUE;
    } else {
        n->cpu_ema = EMA_ALPHA * cpu + (1.0 - EMA_ALPHA) * n->cpu_ema;
        n->mem_ema = EMA_ALPHA * mem + (1.0 - EMA_ALPHA) * n->mem_ema;
    }

    /* Trend (linear regression slope) */
    n->cpu_trend = _trend_slope(n->cpu_ring, n->pos, n->count);
    n->mem_trend = _trend_slope(n->mem_ring, n->pos, n->count);

    /* Predict: EMA + trend × horizon, clamped to [0, 100] */
    n->cpu_predicted_5m = n->cpu_ema + n->cpu_trend * PREDICT_HORIZON;
    if (n->cpu_predicted_5m < 0) n->cpu_predicted_5m = 0;
    if (n->cpu_predicted_5m > 100) n->cpu_predicted_5m = 100;

    n->mem_predicted_5m = n->mem_ema + n->mem_trend * PREDICT_HORIZON;
    if (n->mem_predicted_5m < 0) n->mem_predicted_5m = 0;
    if (n->mem_predicted_5m > 100) n->mem_predicted_5m = 100;
}

/* ── 공개 API ────────────────────────────────────────────────── */

/** pcv_predict_init: 워크로드 예측 모듈 초기화 — 뮤텍스 설정 */
void
pcv_predict_init(void)
{
    g_mutex_init(&G.mu);
    G.initialized = TRUE;
    PCV_LOG_INFO(PREDICT_LOG_DOM, "Workload predictor initialized");
}

/** pcv_predict_shutdown: 워크로드 예측 모듈 종료 — 뮤텍스 해제 */
void
pcv_predict_shutdown(void)
{
    if (!G.initialized) return;
    G.initialized = FALSE;
    g_mutex_clear(&G.mu);
}

/**
 * pcv_predict_evaluate:
 *
 * ebpf_telemetry에서 현재 호스트 메트릭을 읽어 예측을 갱신합니다.
 * ai_agent.c의 5초 타이머에서 주기적으로 호출됩니다.
 *
 * 동작:
 *   1. pcv_ebpf_telemetry_get_host()로 CPU%/MEM% 획득
 *   2. _push_sample()로 링 버퍼 갱신 + EMA/추세/예측 재계산
 *   3. Prometheus 게이지 4개 갱신 (predict_cpu_5m, predict_mem_5m, trend_cpu, trend_mem)
 *   4. WebSocket 클라이언트가 있으면 "forecast" 타입으로 브로드캐스트
 */
void
pcv_predict_evaluate(void)
{
    if (!G.initialized) return;

    /* Read current host metrics directly from ebpf_telemetry cache */
    JsonObject *host = pcv_ebpf_telemetry_get_host();
    if (!host) return;

    gdouble cpu = json_object_get_double_member(host, "cpu_percent");
    gdouble mem = json_object_get_double_member(host, "mem_percent");
    /* B9-M3: transfer-full — pcv_ebpf_telemetry_get_host() returns a new
     * JsonObject ref; caller must unref (confirmed: ebpf_telemetry.c:1726) */
    json_object_unref(host);

    g_mutex_lock(&G.mu);

    if (cpu >= 0 && mem >= 0) {
        NodeForecast *n = _get_node("local", "local");
        if (n) {
            _push_sample(n, cpu, mem);

            /* Prometheus metrics */
            pcv_prom_gauge_set_labels("purecvisor_predict_cpu_5m", "", n->cpu_predicted_5m);
            pcv_prom_gauge_set_labels("purecvisor_predict_mem_5m", "", n->mem_predicted_5m);
            pcv_prom_gauge_set_labels("purecvisor_predict_trend_cpu", "", n->cpu_trend);
            pcv_prom_gauge_set_labels("purecvisor_predict_trend_mem", "", n->mem_trend);

            /* WebSocket broadcast */
            extern void pcv_ws_broadcast(const gchar *type, const gchar *payload);
            extern gint pcv_ws_client_count(void);
            if (pcv_ws_client_count() > 0) {
                gchar payload[256];
                g_snprintf(payload, sizeof(payload),
                    "{\"cpu\":%.1f,\"mem\":%.1f,"
                    "\"cpu_pred\":%.1f,\"mem_pred\":%.1f,"
                    "\"cpu_trend\":%.4f,\"mem_trend\":%.4f}",
                    cpu, mem, n->cpu_predicted_5m, n->mem_predicted_5m,
                    n->cpu_trend, n->mem_trend);
                pcv_ws_broadcast("forecast", payload);
            }

            /* BUG-20 fix: 예측값 + 추세를 self_healing으로 전파.
             * cpu-overload/mem-pressure 정책이 predict_threshold 초과 시
             * 사전 migrate 액션 트리거 (승인 게이트 경유). */
            gdouble cp = n->cpu_predicted_5m;
            gdouble mp = n->mem_predicted_5m;
            gdouble ct = n->cpu_trend;
            gdouble mt = n->mem_trend;
            g_mutex_unlock(&G.mu);
            pcv_healing_on_prediction(cp, mp, ct, mt);
            return;  /* unlock 이미 수행, 함수 종료 */
        }
    }

    g_mutex_unlock(&G.mu);
}

/**
 * pcv_predict_get_forecast_json:
 *
 * 모든 추적 노드의 현재 예측 상태를 JSON 배열 문자열로 반환합니다.
 * REST API와 Web UI 대시보드에서 사용됩니다.
 *
 * JSON 구조: [{"node":"local", "cpu_ema":N, "mem_ema":N,
 *               "cpu_pred":N, "mem_pred":N, "cpu_trend":N, "mem_trend":N, "samples":N}]
 *
 * Returns: (transfer full): JSON 배열 문자열 (g_free 필요)
 */
gchar *
pcv_predict_get_forecast_json(void)
{
    g_mutex_lock(&G.mu);

    GString *buf = g_string_new("[");
    for (gint i = 0; i < G.node_count; i++) {
        NodeForecast *n = &G.nodes[i];
        if (buf->len > 1) g_string_append_c(buf, ',');
        g_string_append_printf(buf,
            "{\"node\":\"%s\",\"cpu_ema\":%.1f,\"mem_ema\":%.1f,"
            "\"cpu_pred\":%.1f,\"mem_pred\":%.1f,"
            "\"cpu_trend\":%.4f,\"mem_trend\":%.4f,"
            "\"samples\":%d}",
            n->name, n->cpu_ema, n->mem_ema,
            n->cpu_predicted_5m, n->mem_predicted_5m,
            n->cpu_trend, n->mem_trend, n->count);
    }
    g_string_append_c(buf, ']');

    g_mutex_unlock(&G.mu);
    return g_string_free(buf, FALSE);
}
