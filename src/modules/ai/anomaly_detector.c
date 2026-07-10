/**
 * @file anomaly_detector.c
 * @brief AI Ops Phase 1 — Z-Score 기반 이상 탐지 엔진
 *
 * [파일 역할]
 *   Prometheus 레지스트리에서 수집된 메트릭 값을 순환 버퍼(링 버퍼)에 저장하고,
 *   이동 평균 + 표준 편차 기반 Z-Score로 이상 여부를 실시간 판정하는 모듈.
 *   Z-Score가 임계값을 초과하면 이상(anomaly)으로 판정하여 알림을 발생시킨다.
 *
 * [아키텍처 위치]
 *   main.c (데몬 시작)
 *     -> pcv_anomaly_init() — 감시 메트릭 등록
 *   ai_agent.c (5초 타이머)
 *     -> pcv_anomaly_evaluate() — 메트릭 값 읽기 + Z-Score 평가
 *     -> self_healing.c로 이상 이벤트 전파
 *
 * [동작 원리 — Z-Score 이상 탐지]
 *   Z-Score = |현재값 - 평균| / 표준편차
 *   Z > threshold 이면 "통계적으로 비정상적인 값"으로 판정
 *   예) CPU 평균 30%, 표준편차 5%, 현재 55% → Z = |55-30|/5 = 5.0 (이상!)
 *
 * [순환 버퍼]
 *   60 샘플 고정 크기 링 버퍼 (5초 간격 × 60 = 5분 윈도우)
 *   최소 10 샘플(50초) 이상 모여야 Z-Score 계산 시작 (통계적 유의성)
 *   합계(sum)와 제곱합(sum_sq)을 유지하여 O(1) 평균/분산 계산
 *
 * [출력 경로]
 *   1. Prometheus 메트릭 (purecvisor_anomaly_score, _active, _alerts_total)
 *   2. WebSocket broadcast ("anomaly" 타입 → Web UI 실시간 알림)
 *   3. 감사 로그 (pcv_audit → SQLite + 파일)
 *
 * [쿨다운 메커니즘]
 *   동일 메트릭에 대해 30초 이내 재알림 방지 (last_alert_us 타임스탬프)
 *   → 알림 폭주(alert storm) 방지
 *
 * [스레드 안전]
 *   GMutex(G.mu)로 전역 상태 접근을 직렬화합니다.
 *
 * [외부 의존성] 없음 (순수 C + GLib + math.h)
 */
#include "anomaly_detector.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include "modules/daemons/prometheus_exporter.h"
#include "modules/audit/pcv_audit.h"
#include "modules/ai/self_healing.h"      /* BUG-20: AI Ops 파이프라인 연결 */
#include "utils/pcv_log.h"

/*
 * ============================================================================
 *  [주니어 개발자 필독] Z-Score 이상 탐지 핵심 개념
 * ============================================================================
 *
 *  Z-Score란?
 *    "현재 값이 과거 패턴에서 얼마나 벗어났는가"를 수치화한 것입니다.
 *    Z = |현재값 - 평균| / 표준편차
 *    Z=0: 평균과 동일, Z=2: 평균에서 표준편차 2배 벗어남, Z=3: 매우 이상
 *
 *    예) 지난 5분간 CPU 평균 30%, 표준편차 5%
 *        현재 CPU 55% → Z = |55-30| / 5 = 5.0 (매우 이상!)
 *        현재 CPU 32% → Z = |32-30| / 5 = 0.4 (정상)
 *
 *  O(1) 증분 통계:
 *    평균과 표준편차를 매번 전체 배열 순회 없이 계산합니다.
 *    sum(합계)과 sum_sq(제곱합)을 유지하며:
 *      새 값 추가 시 sum += value, sum_sq += value*value
 *      오래된 값 제거 시 sum -= old, sum_sq -= old*old
 *    평균 = sum/N, 분산 = sum_sq/N - (sum/N)^2
 *
 *  쿨다운:
 *    Z-Score가 높은 상태가 지속되면 매 5초마다 알림이 반복됩니다.
 *    30초 쿨다운으로 동일 메트릭에 대한 재알림을 억제합니다.
 * ============================================================================
 */

#define ANOMALY_LOG_DOM   "anomaly"
constexpr int ANOMALY_WINDOW    = 60;      /* 링 버퍼 크기: 60 샘플 = 5초 간격 × 60 = 5분 윈도우 */
constexpr int ANOMALY_COOLDOWN  = 30;      /* 동일 메트릭 재알림 방지 쿨다운 (초) */
constexpr int MAX_WATCHED       = 64;      /* 동시 감시 가능한 최대 메트릭 수 */
constexpr int MAX_RECENT_EVENTS = 20;      /* Web UI 표시용 최근 이벤트 링 버퍼 크기 */

/* ── C23 컴파일 타임 검증 ─────────────────────────────────── */
static_assert(ANOMALY_WINDOW >= 10, "Window too small for meaningful Z-Score");
static_assert(MAX_WATCHED >= 1);

/* ── 메트릭별 링 버퍼 구조체 ─────────────────────────────────── */

/**
 * AnomalyMetric — 하나의 감시 대상 메트릭의 통계 상태
 *
 * ring[]: 순환 버퍼에 최근 60개 샘플 값을 저장
 * sum/sum_sq: O(1) 평균/분산 계산을 위해 합계와 제곱합을 증분 유지
 *   - 새 값 추가 시: sum += value, sum_sq += value*value
 *   - 오래된 값 제거 시: sum -= old, sum_sq -= old*old
 *   → 매번 전체 배열을 순회하지 않아도 되므로 성능 우수
 */
typedef struct {
    gchar    name[128];         /* Prometheus 메트릭 이름 (예: "node_disk_io_time_seconds_total") */
    gchar    labels[128];       /* Prometheus 라벨 필터 (빈 문자열이면 라벨 무관) */
    gdouble  ring[ANOMALY_WINDOW]; /* 순환 샘플 버퍼 (최근 60개 값) */
    gint     pos;               /* 다음 쓰기 위치 (0 ~ ANOMALY_WINDOW-1) */
    gint     count;             /* 현재 저장된 샘플 수 (최대 ANOMALY_WINDOW) */
    gdouble  sum;               /* 버퍼 내 값의 합계 (증분 갱신) */
    gdouble  sum_sq;            /* 버퍼 내 값의 제곱합 (증분 갱신) */
    gint64   last_alert_us;     /* 마지막 알림 발생 시각 (마이크로초, 쿨다운 판정용) */
    gdouble  threshold;         /* Z-Score 임계값 — 이 값을 초과하면 이상 판정 */
    gdouble  last_zscore;       /* 가장 최근 계산된 Z-Score (Prometheus 게이지에 노출) */
} AnomalyMetric;

/* ── 최근 알림 이벤트 구조체 (Web UI 표시용) ─────────────────── */

/**
 * AnomalyEvent — 이상 발생 시 기록되는 이벤트
 *
 * Web UI의 이상 탐지 패널에서 최근 이벤트 목록을 표시할 때 사용됩니다.
 * MAX_RECENT_EVENTS(20)개까지 링 버퍼로 유지됩니다.
 */
typedef struct {
    gchar    metric[128];       /* 이상이 발생한 메트릭 이름 */
    gchar    labels[128];       /* 해당 메트릭의 라벨 */
    gdouble  value;             /* 이상 발생 시점의 실제 메트릭 값 */
    gdouble  zscore;            /* 이상 발생 시점의 Z-Score */
    gdouble  mean;              /* 이상 발생 시점의 평균값 */
    gint64   timestamp_us;      /* 이상 발생 시각 (마이크로초, Unix epoch) */
} AnomalyEvent;

/* ── 모듈 전역 상태 ─────────────────────────────────────────── */

static struct {
    AnomalyMetric  watched[MAX_WATCHED];     /* 감시 대상 메트릭 배열 */
    gint           watch_count;              /* 현재 등록된 감시 메트릭 수 */
    AnomalyEvent   recent[MAX_RECENT_EVENTS]; /* 최근 이벤트 링 버퍼 */
    gint           recent_pos;               /* 이벤트 링 버퍼 다음 쓰기 위치 */
    gint           recent_count;             /* 현재 저장된 이벤트 수 */
    GMutex         mu;                       /* 전역 상태 보호 뮤텍스 */
    gboolean       initialized;              /* 초기화 완료 플래그 */
    guint64        total_alerts;             /* 누적 알림 횟수 (Prometheus 카운터) */
    gint           active_anomalies;         /* 현재 활성 이상 수 (임계값 80% 이상 포함) */
} G = {0};

/* ── 통계 함수 — 평균, 표준편차, Z-Score 계산 ──────────────── */

/**
 * _mean:
 * @m: 대상 메트릭
 *
 * 링 버퍼에 저장된 샘플의 산술 평균을 반환합니다.
 * sum을 증분 유지하므로 O(1) 시간에 계산됩니다.
 *
 * Returns: 평균값, 샘플 없으면 0.0
 */
static gdouble
_mean(const AnomalyMetric *m)
{
    if (m->count == 0) return 0.0;
    return m->sum / (gdouble)m->count;
}

/**
 * _stddev:
 * @m: 대상 메트릭
 *
 * 모표준편차를 반환합니다. 분산 = E[X^2] - (E[X])^2 공식을 사용합니다.
 * sum_sq(제곱합)을 증분 유지하므로 O(1)에 계산됩니다.
 *
 * 최소 2개 샘플이 있어야 의미 있는 값을 반환합니다.
 *
 * Returns: 표준편차, 샘플 부족 시 0.0
 */
static gdouble
_stddev(const AnomalyMetric *m)
{
    if (m->count < 2) return 0.0;
    gdouble n = (gdouble)m->count;
    gdouble variance = (m->sum_sq / n) - (m->sum / n) * (m->sum / n);
    return variance > 0.0 ? sqrt(variance) : 0.0;
}

/**
 * _zscore:
 * @m:     대상 메트릭 (평균/표준편차 계산용)
 * @value: 현재 관측값
 *
 * Z-Score를 계산합니다: Z = |value - mean| / stddev
 * Z-Score가 높을수록 현재 값이 과거 패턴에서 벗어난 정도가 큽니다.
 *
 * Returns: Z-Score 값, 분산이 없으면(모든 값 동일) 0.0
 */
static gdouble
_zscore(const AnomalyMetric *m, gdouble value)
{
    gdouble sd = _stddev(m);
    if (sd < 1e-9) return 0.0;  /* 분산이 거의 0 — 모든 값이 동일한 상태 */
    return fabs(value - _mean(m)) / sd;
}

/**
 * _push:
 * @m:     대상 메트릭
 * @value: 새 관측값
 *
 * 새 샘플을 링 버퍼에 추가하고 이상 여부를 판정합니다.
 *
 * 동작 순서:
 *   1. 버퍼가 가득 차면 가장 오래된 값을 제거 (sum/sum_sq에서 차감)
 *   2. 새 값을 현재 위치에 저장하고 sum/sum_sq에 가산
 *   3. 최소 10 샘플이 모이면 Z-Score 계산
 *   4. Z > threshold 이고 쿨다운 경과 시 TRUE(이상 감지) 반환
 *
 * Returns: 이상 감지 시 TRUE, 정상 또는 쿨다운 중이면 FALSE
 */
static gboolean
_push(AnomalyMetric *m, gdouble value)
{
    /* 버퍼가 가득 차면 가장 오래된 값을 합계/제곱합에서 제거 */
    if (m->count >= ANOMALY_WINDOW) {
        gdouble old = m->ring[m->pos];
        m->sum -= old;
        m->sum_sq -= old * old;
    } else {
        m->count++;
    }

    /* 새 값을 현재 위치에 저장하고 합계/제곱합에 추가 */
    m->ring[m->pos] = value;
    m->sum += value;
    m->sum_sq += value * value;
    m->pos = (m->pos + 1) % ANOMALY_WINDOW;

    /* 최소 10 샘플(약 50초)이 모여야 통계적으로 의미 있는 Z-Score 계산 가능 */
    if (m->count < 10) {
        m->last_zscore = 0.0;
        return FALSE;
    }

    gdouble z = _zscore(m, value);
    m->last_zscore = z;

    if (z > m->threshold) {
        gint64 now = g_get_monotonic_time();
        /* 쿨다운: 동일 메트릭에 대해 30초 이내 재알림 방지 (알림 폭주 방어) */
        if (now - m->last_alert_us < ANOMALY_COOLDOWN * G_USEC_PER_SEC)
            return FALSE;
        m->last_alert_us = now;
        return TRUE;
    }
    return FALSE;
}

/* ── 감시 대상 등록 ─────────────────────────────────────────── */

/**
 * _add_watch:
 * @name:      Prometheus 메트릭 이름
 * @labels:    라벨 필터 (빈 문자열이면 라벨 무관)
 * @threshold: Z-Score 임계값 (이 값을 초과하면 이상 판정)
 *
 * 새 메트릭을 감시 목록에 추가합니다. MAX_WATCHED(64) 초과 시 무시됩니다.
 */
static void
_add_watch(const gchar *name, const gchar *labels, gdouble threshold)
{
    if (G.watch_count >= MAX_WATCHED) return;
    AnomalyMetric *m = &G.watched[G.watch_count++];
    memset(m, 0, sizeof(*m));
    g_strlcpy(m->name, name, sizeof(m->name));
    if (labels) g_strlcpy(m->labels, labels, sizeof(m->labels));
    m->threshold = threshold;
}

/* ── 알림 발생 — Prometheus + WebSocket + 감사 로그 출력 ────── */

/**
 * _emit_alert:
 * @m:     이상이 감지된 메트릭
 * @value: 이상 발생 시점의 실제 값
 *
 * 이상 감지 시 3가지 경로로 알림을 출력합니다:
 *   1. Prometheus gauge (purecvisor_anomaly_score) — Grafana 대시보드에서 시각화
 *   2. WebSocket broadcast — Web UI 실시간 알림 패널에 푸시
 *   3. 감사 로그 (pcv_audit) — SQLite + 파일에 영구 기록
 *
 * 최근 이벤트 링 버퍼(G.recent[])에도 저장하여 REST API 조회를 지원합니다.
 */
static void
_emit_alert(AnomalyMetric *m, gdouble value)
{
    gdouble z = m->last_zscore;
    gdouble mean = _mean(m);

    G.total_alerts++;

    /* Store in recent ring */
    AnomalyEvent *ev = &G.recent[G.recent_pos];
    g_strlcpy(ev->metric, m->name, sizeof(ev->metric));
    g_strlcpy(ev->labels, m->labels, sizeof(ev->labels));
    ev->value = value;
    ev->zscore = z;
    ev->mean = mean;
    ev->timestamp_us = g_get_real_time();
    G.recent_pos = (G.recent_pos + 1) % MAX_RECENT_EVENTS;
    if (G.recent_count < MAX_RECENT_EVENTS) G.recent_count++;

    /* Prometheus metric */
    gchar lbl[256];
    g_snprintf(lbl, sizeof(lbl), "metric=\"%s\"", m->name);
    pcv_prom_gauge_set_labels("purecvisor_anomaly_score", lbl, z);

    /* WebSocket broadcast */
    {
        extern void pcv_ws_broadcast(const gchar *type, const gchar *payload);
        extern gint pcv_ws_client_count(void);
        if (pcv_ws_client_count() > 0) {
            gchar payload[512];
            g_snprintf(payload, sizeof(payload),
                "{\"metric\":\"%s\",\"labels\":\"%s\",\"value\":%.2f,"
                "\"zscore\":%.2f,\"mean\":%.2f,\"threshold\":%.1f}",
                m->name, m->labels, value, z, mean, m->threshold);
            pcv_ws_broadcast("anomaly", payload);
        }
    }

    /* Audit log */
    {
        gchar detail[256];
        g_snprintf(detail, sizeof(detail),
            "Z=%.2f (threshold=%.1f) value=%.2f mean=%.2f",
            z, m->threshold, value, mean);
        pcv_audit_log("ai-ops", "anomaly_detected", m->name, detail, 0, 0, "local");
    }

    PCV_LOG_WARN(ANOMALY_LOG_DOM,
        "ANOMALY: %s Z=%.2f (>%.1f) val=%.2f mean=%.2f",
        m->name, z, m->threshold, value, mean);

    /* BUG-20 fix: AI Ops 파이프라인 연결 — self_healing 정책 엔진으로 전파.
     * pcv_healing_on_anomaly()가 trigger_metric 매칭 정책 평가 + 2개+ 복합 조건 시
     * AI Agent 합의 요청(pcv_agent_compare_async)까지 이어진다. */
    pcv_healing_on_anomaly(m->name, value, z, m->threshold);
}

/* ── 공개 API ────────────────────────────────────────────────── */

/**
 * pcv_anomaly_init:
 *
 * 이상 탐지 모듈을 초기화하고 감시 대상 메트릭을 등록합니다.
 * 각 메트릭별 Z-Score 임계값은 경험적으로 설정되었습니다:
 *   - CPU/MEM: 2.5 (중간 민감도 — 일반적 변동은 무시)
 *   - Disk I/O: 3.0 (낮은 민감도 — I/O 스파이크가 잦으므로)
 *   - Network Error: 2.0 (높은 민감도 — 에러는 항상 주의)
 *   - 온도: 2.0 (높은 민감도 — 과열은 하드웨어 손상 위험)
 */
void
pcv_anomaly_init(void)
{
    g_mutex_init(&G.mu);
    G.initialized = TRUE;

    /* Register watched metrics — matching architecture doc section 4.3 */
    _add_watch("purecvisor_host_cpu_percent", "", 2.5);
    _add_watch("purecvisor_host_memory_percent", "", 2.5);
    _add_watch("node_disk_io_time_seconds_total", "", 3.0);
    _add_watch("node_network_receive_errs_total", "", 2.0);
    _add_watch("purecvisor_rpc_duration_ms", "method=\"vm.list\"", 3.0);
    _add_watch("node_vmstat_pswpout", "", 2.5);
    _add_watch("node_pressure_cpu_some_seconds_total", "", 2.0);
    _add_watch("node_hwmon_temp_celsius", "chip=\"coretemp\",sensor=\"temp1\"", 2.0);
    _add_watch("node_hwmon_temp_celsius", "chip=\"k10temp\",sensor=\"temp1\"", 2.0);
    _add_watch("node_nf_conntrack_entries", "", 2.5);

    PCV_LOG_INFO(ANOMALY_LOG_DOM,
        "Anomaly detector initialized — %d metrics watched", G.watch_count);
}

/** pcv_anomaly_shutdown: 이상 탐지 모듈 종료 — 뮤텍스 해제 */
void
pcv_anomaly_shutdown(void)
{
    if (!G.initialized) return;
    G.initialized = FALSE;
    g_mutex_clear(&G.mu);
}

/**
 * pcv_anomaly_evaluate:
 *
 * 등록된 모든 감시 메트릭에 대해 이상 탐지를 수행합니다.
 * ai_agent.c의 5초 타이머에서 주기적으로 호출됩니다.
 *
 * 동작 흐름:
 *   1. Prometheus 레지스트리를 텍스트로 렌더링 (2초 캐시)
 *   2. 각 메트릭 이름+라벨로 현재 값을 검색
 *   3. _push()로 링 버퍼에 추가 + Z-Score 평가
 *   4. 이상 감지 시 _emit_alert()로 알림 발생
 *   5. 임계값 80% 이상이면 "near-threshold"로 활성 이상 카운트에 포함
 *   6. 전역 Prometheus 메트릭 업데이트
 */
void
pcv_anomaly_evaluate(void)
{
    if (!G.initialized) return;

    g_mutex_lock(&G.mu);

    gint active = 0;

    for (gint i = 0; i < G.watch_count; i++) {
        AnomalyMetric *m = &G.watched[i];

        /* Read current value from prometheus registry via render —
         * more efficient: directly query the registry.
         * For now, use the gauge_set_labels lookup pattern. */
        extern gchar *pcv_prom_render(void);
        static gchar *last_render = NULL;
        static gint64 last_render_time = 0;

        /* Cache render output for this evaluation cycle (all metrics same cycle) */
        gint64 now = g_get_monotonic_time();
        if (now - last_render_time > 2 * G_USEC_PER_SEC || !last_render) {
            /* B9-M4: only advance timestamp when render succeeds;
             * a NULL return must not suppress the next render attempt */
            gchar *new_render = pcv_prom_render();
            if (new_render) {
                g_free(last_render);
                last_render = new_render;
                last_render_time = now;
            }
        }

        if (!last_render) continue;

        /* Find metric value in rendered text */
        gdouble value = NAN;
        gchar search_key[256];
        if (m->labels[0]) {
            g_snprintf(search_key, sizeof(search_key), "%s{%s}", m->name, m->labels);
        } else {
            g_snprintf(search_key, sizeof(search_key), "%s ", m->name);
        }

        const gchar *found = strstr(last_render, search_key);
        if (found) {
            const gchar *space = strrchr(search_key, ' ');
            if (!space) space = strchr(found + strlen(m->name), ' ');
            else space = found + strlen(search_key) - 1;
            if (space) {
                /* Find the value after the key */
                const gchar *val_start = found + strlen(search_key);
                if (m->labels[0]) {
                    val_start = strchr(found, '}');
                    if (val_start) val_start++; /* skip '}' */
                    while (val_start && *val_start == ' ') val_start++;
                }
                if (val_start) value = g_ascii_strtod(val_start, NULL);
            }
        }

        if (isnan(value)) continue;

        gboolean anomaly = _push(m, value);

        /* Update per-metric prometheus gauge */
        gchar lbl[256];
        g_snprintf(lbl, sizeof(lbl), "metric=\"%s\"", m->name);
        pcv_prom_gauge_set_labels("purecvisor_anomaly_score", lbl, m->last_zscore);

        if (anomaly) {
            _emit_alert(m, value);
            active++;
        } else if (m->last_zscore > m->threshold * 0.8) {
            active++; /* near-threshold counts as active */
        }
    }

    G.active_anomalies = active;

    /* Global prometheus metrics */
    pcv_prom_gauge_set_labels("purecvisor_anomaly_active", "", (gdouble)active);
    pcv_prom_gauge_set_labels("purecvisor_anomaly_alerts_total", "", (gdouble)G.total_alerts);

    g_mutex_unlock(&G.mu);
}

/**
 * pcv_anomaly_reset_baseline:
 *
 * CE-A13: 모든 감시 메트릭의 롤링 통계를 초기화합니다.
 * 시스템 구성 변경(VM 이전, 하드웨어 교체 등) 후 기존 베이스라인이
 * 더 이상 유효하지 않을 때 RPC를 통해 호출합니다.
 *
 * 초기화 대상:
 *   - 각 메트릭의 링 버퍼(ring[]), 합계(sum/sum_sq), 카운터(count/pos)
 *   - 마지막 알림 시각 (last_alert_us) — 쿨다운 리셋
 *   - 최근 Z-Score (last_zscore)
 *   - 최근 이벤트 링 버퍼 (recent[])
 *   - 활성 이상 수, 전체 알림 카운터
 */
void
pcv_anomaly_reset_baseline(void)
{
    if (!G.initialized) return;

    g_mutex_lock(&G.mu);

    /* 각 감시 메트릭의 통계 상태를 초기화 (이름/라벨/임계값은 보존) */
    for (gint i = 0; i < G.watch_count; i++) {
        AnomalyMetric *m = &G.watched[i];
        memset(m->ring, 0, sizeof(m->ring));
        m->pos = 0;
        m->count = 0;
        m->sum = 0.0;
        m->sum_sq = 0.0;
        m->last_alert_us = 0;
        m->last_zscore = 0.0;
    }

    /* 최근 이벤트 링 버퍼 초기화 */
    memset(G.recent, 0, sizeof(G.recent));
    G.recent_pos = 0;
    G.recent_count = 0;

    /* 카운터 리셋 */
    G.total_alerts = 0;
    G.active_anomalies = 0;

    g_mutex_unlock(&G.mu);

    g_message("[ANOMALY] Baseline statistics reset — all %d metrics cleared",
              G.watch_count);
}

/**
 * pcv_anomaly_get_recent_json:
 *
 * 최근 이상 이벤트를 JSON 배열 문자열로 반환합니다 (최신 순 정렬).
 * REST API(/api/v1/ai/anomalies)와 WebSocket에서 사용됩니다.
 *
 * JSON 구조: [{"metric":"...", "labels":"...", "value":N, "zscore":N, "mean":N, "ts":epoch}, ...]
 *
 * Returns: (transfer full): JSON 배열 문자열 (g_free 필요)
 */
gchar *
pcv_anomaly_get_recent_json(void)
{
    g_mutex_lock(&G.mu);

    GString *buf = g_string_new("[");
    gint start = (G.recent_count >= MAX_RECENT_EVENTS)
        ? G.recent_pos : 0;
    gint count = G.recent_count;

    for (gint i = 0; i < count; i++) {
        gint idx = (start + count - 1 - i) % MAX_RECENT_EVENTS; /* newest first */
        AnomalyEvent *ev = &G.recent[idx];
        if (ev->timestamp_us == 0) continue;
        if (buf->len > 1) g_string_append_c(buf, ',');
        g_string_append_printf(buf,
            "{\"metric\":\"%s\",\"labels\":\"%s\",\"value\":%.2f,"
            "\"zscore\":%.2f,\"mean\":%.2f,\"ts\":%ld}",
            ev->metric, ev->labels, ev->value,
            ev->zscore, ev->mean, (long)(ev->timestamp_us / G_USEC_PER_SEC));
    }
    g_string_append_c(buf, ']');

    g_mutex_unlock(&G.mu);
    return g_string_free(buf, FALSE);
}
