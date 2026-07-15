/**
 * @file self_healing.c
 * @brief AI Ops Phase 3 — Self-Healing 정책 엔진
 *
 * [파일 역할]
 *   anomaly_detector(Z-Score 이상 탐지)와 workload_predict(워크로드 예측)에서
 *   이벤트를 수신하여, 사전 등록된 정책에 따라 자동 복구 액션을 실행하는 엔진.
 *   정책 기반 자동화이므로 AI 모델 없이도 동작하며, 복합 조건 시에만
 *   AI Agent(ai_agent.c)에 자문을 요청한다.
 *
 * [아키텍처 위치]
 *   anomaly_detector.c (Z-Score 이상 감지)
 *     → pcv_healing_on_anomaly()     [이 파일, 이벤트 수신]
 *       → _try_policy()              [안전장치 검사 + 액션 실행]
 *         → _execute_action()        [실제 액션 또는 dry run]
 *         → _queue_approval()        [위험 액션 승인 대기]
 *
 *   workload_predict.c (EMA/OLS 예측)
 *     → pcv_healing_on_prediction()  [이 파일, 예측 이벤트 수신]
 *       → _try_policy()
 *
 *   handler_monitor.c (healing.approve / healing.dismiss RPC)
 *     → pcv_healing_approve() / pcv_healing_dismiss()  [이 파일]
 *
 * [안전장치 — 5중 보호 (운영 안정성 최우선)]
 *   1. Dry Run 모드: 기본값 TRUE — 모든 액션을 로그만 남기고 실행하지 않음.
 *      daemon.conf [ai] mode=active로 명시 설정해야 실제 실행.
 *   2. Approval Gate: require_approval=TRUE인 정책은 Web UI에서 관리자가
 *      승인해야만 실행됨. (migrate 등 위험 액션에 적용 의도)
 *      AIO-8: 현재 등록된 10개 정책 전부 require_approval=FALSE라
 *      _queue_approval/pcv_healing_approve/dismiss 경로는 의도적으로 dormant.
 *      migrate 액션을 자동승격 도입 시 해당 정책의 require_approval=TRUE 전환과
 *      함께 이 게이트를 재활성해야 함.
 *   3. Cooldown: 동일 정책이 cooldown_sec 이내에 재실행되지 않음.
 *      실패 시 지수 백오프(×2, 최대 1시간)로 쿨다운이 늘어남.
 *   4. Circuit Breaker: 연속 3회 실패(CIRCUIT_BREAKER_MAX) 시 정책 자동 비활성화.
 *   5. Rate Limit: 5분(RATE_LIMIT_WINDOW) 내 최대 3개(RATE_LIMIT_MAX) 자동 액션.
 *      alert_only는 레이트 리밋에서 제외.
 *
 * [내장 정책 목록 (pcv_healing_init에서 등록, 10개)]
 *   cpu-overload:        CPU Z≥3.0 또는 예측>85% → alert_only (승인 불요, 쿨다운 600초, AF-O1(a))
 *   mem-pressure:        MEM Z≥2.5 또는 예측>90% → alert_only (승인 불요, 쿨다운 600초, AF-O1(a))
 *   thermal-alert:       온도 Z≥2.0 또는 >80도  → alert_only (쿨다운 1800초)
 *   vm-unresponsive:     (메트릭 없음, 수동 트리거) → restart (승인 불요, 쿨다운 300초)
 *   swap-storm:          swap 출력 Z≥2.5 → alert_only (쿨다운 300초)
 *   disk-saturated:      disk I/O Z≥3.0 → alert_only (쿨다운 600초)
 *   net-errors:          네트워크 에러 Z≥2.0 → alert_only (쿨다운 300초)
 *   conntrack-full:      conntrack 엔트리 Z≥2.5 → alert_only (쿨다운 600초)
 *   vm-reboot-loop:      재시작 루프(10분 내 5회+) → alert_only (쿨다운 1200초)
 *   vm-migration-failed: 마이그레이션 반복실패(30분 내 3회+) → alert_only (쿨다운 1800초)
 *
 * [스레드 안전]
 *   G.mu (GMutex): 정책 배열 + 승인 대기열 보호
 *   g_healing_hist_mu (GMutex): 이력 링버퍼 보호
 *   호출 컨텍스트: ebpf_telemetry 스레드에서 on_anomaly/on_prediction 호출,
 *   메인 스레드에서 approve/dismiss/get_pending 호출 → 뮤텍스로 보호.
 *
 * 외부 의존성: 없음 (순수 C + GLib)
 */
#include "self_healing.h"
#include "ai_agent.h"
#include "restart_breaker.h"      /* AF-1 후속: VM 단위 재시작 서킷 브레이커 */
#include "self_healing_restart.h" /* 결정 로직 추출 seam (PCV_SAFETY_CONTROL: self-healing-restart) */
#include <string.h>
#include <stdio.h>
#include <libvirt/libvirt.h>      /* AF-1: restart 실배선 — virDomain* API */
#include <libvirt/virterror.h>    /* AF-1: virGetLastError() 명시적 선언 */
#include "modules/daemons/prometheus_exporter.h"
#include "modules/daemons/ebpf_telemetry.h"
#include "modules/audit/pcv_audit.h"
#include "modules/virt/virt_conn_pool.h"  /* AF-1: 워커에서 virConnectPtr 획득 */
#include "utils/pcv_worker_pool.h"        /* AF-1: 재시작을 워커 풀로 오프로드 */
#include "utils/pcv_config.h"   /* Issue-M1: daemon.conf [ai] mode 읽기 */
#include "utils/pcv_log.h"

/* AF-1: 다형성 도메인 조회(UUID→이름). handler_vm_lifecycle.c 정의.
 * virt_events.c 는 UUID 문자열을 target 으로 전달하므로 UUID 우선 조회가 필수 —
 * virDomainLookupByName 단독으로는 UUID target 을 찾지 못한다. */
extern virDomainPtr pure_virt_get_domain(virConnectPtr conn, const gchar *identifier);

/*
 * ============================================================================
 *  [주니어 개발자 필독] 자가 치유 엔진 핵심 개념 정리
 * ============================================================================
 *
 *  1. 5계층 안전 스택 (운영 안정성 최우선)
 *     AI가 VM을 마음대로 마이그레이션/재시작하면 위험합니다.
 *     5중 보호 계층이 순서대로 적용됩니다:
 *
 *     ┌─ 계층 1: Dry Run ─────────────────────────────────────┐
 *     │ 기본값 TRUE — 모든 액션을 로그만 남기고 실행하지 않음   │
 *     │ daemon.conf [ai] mode=active로 명시해야 실제 실행       │
 *     ├─ 계층 2: Approval Gate ────────────────────────────────┤
 *     │ require_approval=TRUE인 정책은 Web UI 관리자 승인 필요  │
 *     │ migrate, restart 같은 위험 액션에 적용                   │
 *     ├─ 계층 3: Cooldown ─────────────────────────────────────┤
 *     │ 동일 정책이 cooldown_sec 이내에 재실행되지 않음          │
 *     │ 실패 시 지수 백오프(×2, 최대 1시간)로 쿨다운 증가        │
 *     ├─ 계층 4: Circuit Breaker ──────────────────────────────┤
 *     │ 연속 3회 실패 시 정책 자동 비활성화 (OPEN 상태)          │
 *     │ 같은 문제로 무한 재시도하는 것을 차단                    │
 *     ├─ 계층 5: Rate Limit ───────────────────────────────────┤
 *     │ 5분 내 최대 3개 자동 액션 (alert_only는 제외)           │
 *     │ 여러 정책이 동시에 트리거되어도 과도한 액션 방지         │
 *     └───────────────────────────────────────────────────────┘
 *
 *  2. per-policy dry-run (CE-A12)
 *     HealingPolicy.policy_dry_run 필드로 정책별로 dry-run 모드를
 *     오버라이드할 수 있습니다:
 *       -1 = 글로벌 설정(G.dry_run) 상속 (기본값)
 *        0 = 이 정책만 실제 실행 (다른 정책은 dry-run 유지)
 *        1 = 이 정책만 dry-run (다른 정책이 active여도)
 *     이를 통해 정책을 하나씩 점진적으로 활성화할 수 있습니다.
 *
 *  3. 승인 큐 타임아웃
 *     PendingAction이 APPROVAL_TIMEOUT_SEC(1시간) 동안 승인되지 않으면
 *     자동으로 resolved=TRUE 처리됩니다 (향후 구현).
 *     무기한 대기하면 대기열이 쌓여 의미 있는 알림이 묻히므로
 *     타임아웃으로 강제 정리합니다.
 * ============================================================================
 */

/** 로그 도메인 — journalctl에서 "healing" 태그로 필터 가능 */
#define HEALING_LOG_DOM       "healing"
/** 등록 가능한 최대 정책 수 */
constexpr int MAX_POLICIES        = 16;
/** 승인 대기열 최대 크기 (초과 시 가장 오래된 항목 덮어씀) */
constexpr int MAX_PENDING         = 8;
/** 레이트 리밋 윈도우 (초) — 이 시간 내 최대 RATE_LIMIT_MAX 액션 */
constexpr int RATE_LIMIT_WINDOW   = 300;  /* 5분 */
/** 레이트 리밋 윈도우 내 최대 자동 액션 수 */
constexpr int RATE_LIMIT_MAX      = 3;
/** 서킷 브레이커 임계값 — 연속 이 횟수만큼 실패하면 정책 비활성화 */
constexpr int CIRCUIT_BREAKER_MAX = 3;
/** 자가 치유 이력 링버퍼 최대 크기 */
constexpr int HEALING_HISTORY_MAX = 100;
/** 지수 백오프 쿨다운 상한 (초) — 아무리 실패해도 1시간 넘게 대기하지 않음.
 * [감사 AF-1] 실제 실행이 미배선이라 현재 backoff 경로가 없어 미사용 — restart/
 * migrate 실배선 시 실패 backoff와 함께 재사용된다. */
[[maybe_unused]] constexpr int COOLDOWN_MAX_SEC = 3600;

/* ── C23 컴파일 타임 검증 ────────────────────────────────────── */
static_assert(MAX_POLICIES >= 1);
static_assert(MAX_PENDING >= 1);
static_assert(RATE_LIMIT_MAX >= 1);

/* ── 정책 정의 구조체 ────────────────────────────────────────── */

/**
 * @struct HealingPolicy
 * @brief 단일 자가 치유 정책의 설정 + 런타임 상태
 *
 * 설정 필드 (init 시 _add_policy로 설정):
 *   name             — 정책 식별자 (예: "cpu-overload")
 *   trigger_metric   — 이상 감지 시 매칭할 Prometheus 메트릭 이름
 *   trigger_zscore   — 이상 판정 Z-Score 임계값
 *   predict_threshold — 예측값 임계값 (0이면 예측 트리거 비활성)
 *   action           — 실행할 액션 ("migrate", "restart", "alert_only" 등)
 *   cooldown_sec     — 현재 적용 중인 쿨다운 (실패 시 지수 증가)
 *   base_cooldown_sec — 원래 쿨다운 값 (성공 시 리셋 대상)
 *   require_approval — TRUE면 Web UI 승인 필요
 *
 * 런타임 필드 (정책 평가 중 자동 갱신):
 *   last_trigger_us  — 마지막 트리거 시각 (쿨다운 판정용)
 *   consecutive_failures — 서킷 브레이커용 연속 실패 카운터
 */
typedef struct {
    gchar    name[64];             /**< 정책 이름 (고유 식별자) */
    gchar    trigger_metric[128];  /**< 매칭할 Prometheus 메트릭 이름 (부분 매칭) */
    gdouble  trigger_zscore;       /**< 이상 판정 Z-Score 임계값 */
    gdouble  predict_threshold;    /**< 예측값 임계값 (0이면 예측 트리거 비활성) */
    gchar    action[32];           /**< 실행 액션 ("alert_only", "migrate", "restart") */
    gint     cooldown_sec;         /**< 현재 쿨다운 (초, 실패 시 ×2 증가) */
    gint     base_cooldown_sec;    /**< 원래 쿨다운 (성공 시 리셋) */
    gboolean require_approval;     /**< TRUE면 Web UI 관리자 승인 필요 */
    gboolean enabled;              /**< 정책 활성화 여부 */
    gint     policy_dry_run;       /**< CE-A12: 정책별 dry_run 오버라이드.
                                    *   -1=글로벌 설정 상속, 0=실행(live), 1=dry-run */
    /* 런타임 상태 — _try_policy에서 자동 관리 */
    gint64   last_trigger_us;      /**< 마지막 트리거 시각 (모노토닉, 마이크로초) */
    gint     consecutive_failures; /**< 연속 실패 횟수 (서킷 브레이커 판정용) */
} HealingPolicy;

/* ── 승인 대기 액션 ──────────────────────────────────────────── */

/**
 * @struct PendingAction
 * @brief Web UI 관리자 승인을 기다리는 자가 치유 액션
 *
 * require_approval=TRUE인 정책이 트리거되면 즉시 실행하지 않고
 * 이 구조체에 기록하여 승인 대기열에 추가한다.
 * Web UI에서 관리자가 승인(approve)하면 _execute_action() 호출,
 * 거부(dismiss)하면 resolved=TRUE로만 표시하고 실행하지 않는다.
 */
typedef struct {
    gint     id;                /**< 승인 요청 고유 ID (자동 증가) */
    gchar    policy_name[64];   /**< 트리거된 정책 이름 */
    gchar    action[32];        /**< 실행 예정 액션 (예: "migrate") */
    gchar    reason[256];       /**< 트리거 사유 (메트릭 값 + Z-Score 포함) */
    gint64   created_us;        /**< 생성 시각 (마이크로초, Unix epoch) */
    gboolean resolved;          /**< 처리 완료 여부 (승인 또는 거부) */
} PendingAction;

/* ── 액션 이력 저널 (링버퍼) ─────────────────────────────────── */

/**
 * @struct HealingHistoryEntry
 * @brief 자가 치유 액션 실행 이력 — 감사 추적 및 Web UI 표시용
 *
 * 모든 실행된 액션(성공/실패/dry_run/skipped)이 링버퍼에 기록된다.
 * pcv_healing_get_history_json()으로 JSON 배열로 변환되어
 * REST/RPC를 통해 Web UI에 표시된다.
 */
typedef struct {
    gchar   action[64];       /**< 실행된 액션 (예: "migrate", "restart", "alert_only") */
    gchar   target[128];      /**< 대상 (정책 이름 또는 VM 이름) */
    gchar   reason[256];      /**< 실행 사유 (메트릭 값 + Z-Score 등) */
    gchar   result[64];       /**< 결과 ("success", "failed", "dry_run", "skipped") */
    gint64  timestamp;        /**< 실행 시각 (Unix epoch 초) */
    gint64  duration_ms;      /**< 액션 소요 시간 (밀리초) */
} HealingHistoryEntry;

static HealingHistoryEntry g_healing_history[HEALING_HISTORY_MAX];
static gint g_healing_hist_idx   = 0;
static gint g_healing_hist_count = 0;
static GMutex g_healing_hist_mu;

/* ── 모듈 전역 상태 ──────────────────────────────────────────── */

/**
 * @brief 자가 치유 엔진 전역 상태
 *
 * 단일 정적 구조체에 모든 상태를 모아 관리한다.
 * {0}으로 zero-initialize되므로 초기 상태가 명확하다.
 *
 * 스레드 안전:
 *   G.mu로 policies[], pending[] 동시 접근 보호.
 *   Prometheus 카운터는 원자적 증가가 아니지만, 정확도보다
 *   성능을 우선하여 락 없이 갱신한다 (카운터이므로 약간의 오차 허용).
 */
static struct {
    HealingPolicy policies[MAX_POLICIES];  /**< 등록된 정책 배열 */
    gint          policy_count;            /**< 현재 등록된 정책 수 */
    PendingAction pending[MAX_PENDING];    /**< 승인 대기 액션 링버퍼 */
    gint          pending_pos;             /**< 대기열 다음 쓰기 위치 */
    gint          pending_count;           /**< 대기열 내 유효 항목 수 */
    gint          next_action_id;          /**< 다음 승인 요청 ID (자동 증가) */
    GMutex        mu;                      /**< 정책/대기열 접근 보호 뮤텍스 */
    gboolean      initialized;            /**< init() 호출 여부 */
    gboolean      dry_run;                /**< dry_run 모드 여부. TRUE면 액션 미실행, 로그만 기록.
                                            *   기본값 TRUE — daemon.conf [ai] mode=active로 해제 */
    /* 레이트 리밋 — 최근 RATE_LIMIT_MAX개 액션 시각을 기록하여 빈도 제한 */
    gint64        action_times[RATE_LIMIT_MAX]; /**< 최근 액션 시각 (모노토닉, 마이크로초) */
    gint          action_time_pos;              /**< 다음 기록 위치 (환형) */
    /* Prometheus 카운터 */
    guint64       total_triggered;  /**< 정책 트리거 누적 횟수 */
    guint64       total_executed;   /**< 실제 액션 실행 누적 횟수 */
    guint64       total_pending;    /**< 현재 승인 대기 중인 액션 수 */
} G = {0};

/* ───────────────────────────────────────────────────────────────
 * 1.0: 시간 윈도우 anomaly 누적 → AI Agent 자동 트리거
 *
 * pcv_healing_on_anomaly의 "단일 호출 내 triggered_count >= 2" 조건은
 * 정책이 모두 별도 메트릭을 트리거하는 현실에서 자연 발생이 어렵다.
 *
 * 보완: 60초 윈도우에서 distinct metric anomaly 3개 이상 누적되면
 * AI Agent compare_async 호출. 같은 메트릭 반복은 1로 카운트.
 * ─────────────────────────────────────────────────────────────── */
#define MULTI_ANOMALY_WINDOW_SEC  60
#define MULTI_ANOMALY_THRESHOLD   3
#define MULTI_ANOMALY_MAX_TRACK   16   /* distinct metric 추적 상한 */

typedef struct {
    gchar  metric[64];
    gint64 last_seen_us;
} AnomalyTrack;

static AnomalyTrack g_recent_anomalies[MULTI_ANOMALY_MAX_TRACK] = {0};
static gint64       g_last_multi_agent_us = 0;
static GMutex       g_anomaly_mu;   /* AIO-1: 위 두 static의 RMW 보호 (eBPF/이벤트/메인 3스레드 경쟁) */

/**
 * _track_distinct_anomaly:
 * 한 메트릭의 anomaly를 시간 윈도우에 기록.
 * 윈도우 내 distinct count가 임계 도달 + 5분 쿨다운 통과 시 TRUE 반환.
 */
static gboolean
_track_distinct_anomaly(const gchar *metric)
{
    if (!metric || !*metric) return FALSE;
    gint64 now = g_get_monotonic_time();
    gint64 window_us = (gint64)MULTI_ANOMALY_WINDOW_SEC * G_USEC_PER_SEC;
    gboolean trigger = FALSE;

    /* AIO-1: g_recent_anomalies[]/g_last_multi_agent_us는 eBPF/이벤트/메인
     * 3스레드에서 호출되므로 전체 RMW를 전용 뮤텍스로 가드한다. */
    g_mutex_lock(&g_anomaly_mu);

    /* 기존 항목 갱신 또는 빈 슬롯에 삽입 */
    gint distinct = 0;
    gint empty = -1;
    for (gint i = 0; i < MULTI_ANOMALY_MAX_TRACK; i++) {
        AnomalyTrack *t = &g_recent_anomalies[i];
        if (t->metric[0] == 0) { if (empty < 0) empty = i; continue; }
        if (now - t->last_seen_us > window_us) {
            t->metric[0] = 0;     /* 만료 */
            if (empty < 0) empty = i;
            continue;
        }
        if (g_strcmp0(t->metric, metric) == 0) {
            t->last_seen_us = now;
            distinct++;
            continue;
        }
        distinct++;
    }
    /* 이 metric이 처음이면 새로 등록 */
    gboolean is_new = TRUE;
    for (gint i = 0; i < MULTI_ANOMALY_MAX_TRACK; i++) {
        if (g_strcmp0(g_recent_anomalies[i].metric, metric) == 0) { is_new = FALSE; break; }
    }
    if (is_new && empty >= 0) {
        g_strlcpy(g_recent_anomalies[empty].metric, metric,
                  sizeof(g_recent_anomalies[empty].metric));
        g_recent_anomalies[empty].last_seen_us = now;
        distinct++;
    }

    /* 임계 + 쿨다운 검사 (5분) */
    if (distinct >= MULTI_ANOMALY_THRESHOLD &&
        (now - g_last_multi_agent_us) > 300 * G_USEC_PER_SEC) {
        g_last_multi_agent_us = now;
        trigger = TRUE;
    }

    g_mutex_unlock(&g_anomaly_mu);
    return trigger;
}

/* 외부에서 호출 가능: agent.compare_manual RPC 등 */
gboolean pcv_healing_should_trigger_agent_now(void)
{
    g_mutex_lock(&g_anomaly_mu);
    gboolean recent = (g_get_monotonic_time() - g_last_multi_agent_us) < 60 * G_USEC_PER_SEC;
    g_mutex_unlock(&g_anomaly_mu);
    return recent;
}

/* ── 정책 등록 ───────────────────────────────────────────────── */

/**
 * _add_policy — 자가 치유 정책을 배열에 추가한다.
 *
 * pcv_healing_init()에서 내장 정책을 등록할 때 호출된다.
 * MAX_POLICIES(16)를 초과하면 무시한다.
 *
 * @param name              정책 이름 (고유 식별자)
 * @param trigger_metric    매칭할 Prometheus 메트릭 이름 (부분 매칭)
 * @param trigger_zscore    이상 판정 Z-Score 임계값
 * @param predict_threshold 예측값 임계값 (0이면 비활성)
 * @param action            실행 액션 ("migrate", "restart", "alert_only")
 * @param cooldown_sec      쿨다운 시간 (초)
 * @param require_approval  TRUE면 Web UI 승인 필요
 */
static void
_add_policy(const gchar *name, const gchar *trigger_metric,
            gdouble trigger_zscore, gdouble predict_threshold,
            const gchar *action, gint cooldown_sec,
            gboolean require_approval)
{
    if (G.policy_count >= MAX_POLICIES) return;
    HealingPolicy *p = &G.policies[G.policy_count++];
    memset(p, 0, sizeof(*p));
    g_strlcpy(p->name, name, sizeof(p->name));
    g_strlcpy(p->trigger_metric, trigger_metric, sizeof(p->trigger_metric));
    p->trigger_zscore = trigger_zscore;
    p->predict_threshold = predict_threshold;
    g_strlcpy(p->action, action, sizeof(p->action));
    p->cooldown_sec = cooldown_sec;
    p->base_cooldown_sec = cooldown_sec;
    p->require_approval = require_approval;
    p->enabled = TRUE;
    p->policy_dry_run = -1;  /* CE-A12: 기본값 = 글로벌 설정 상속 */
}

/* ── 레이트 리밋 — 5분 내 최대 3개 자동 액션 허용 ─────────────── */

/**
 * _rate_check — 현재 레이트 리밋에 여유가 있는지 확인한다.
 *
 * 최근 RATE_LIMIT_WINDOW(5분) 내에 RATE_LIMIT_MAX(3)개 미만의
 * 액션이 실행되었으면 TRUE를 반환한다.
 * alert_only 액션은 _try_policy에서 이 검사를 건너뛰므로
 * 레이트 리밋에 영향을 주지 않는다.
 *
 * Returns: 여유가 있으면 TRUE, 리밋 도달 시 FALSE
 */
static gboolean
_rate_check(void)
{
    gint64 now = g_get_monotonic_time();
    gint64 window = RATE_LIMIT_WINDOW * G_USEC_PER_SEC;

    gint recent = 0;
    for (gint i = 0; i < RATE_LIMIT_MAX; i++) {
        if (now - G.action_times[i] < window)
            recent++;
    }
    return recent < RATE_LIMIT_MAX;
}

/**
 * _rate_record — 현재 시각을 레이트 리밋 기록에 추가한다.
 * 환형 배열에 기록하므로 가장 오래된 항목이 자동으로 덮어쓰인다.
 */
static void
_rate_record(void)
{
    G.action_times[G.action_time_pos] = g_get_monotonic_time();
    G.action_time_pos = (G.action_time_pos + 1) % RATE_LIMIT_MAX;
}

/* ── 액션 이력 기록 ──────────────────────────────────────────── */

/**
 * _record_healing_action — 액션 실행 결과를 이력 링버퍼에 기록한다.
 *
 * g_healing_hist_mu 잠금 하에 HealingHistoryEntry를 채우고
 * 인덱스를 순환 전진시킨다. 이 이력은 pcv_healing_get_history_json()으로
 * JSON으로 변환되어 Web UI에 표시된다.
 */
static void
_record_healing_action(const gchar *action, const gchar *target,
                       const gchar *reason, const gchar *result,
                       gint64 duration_ms)
{
    g_mutex_lock(&g_healing_hist_mu);

    HealingHistoryEntry *e = &g_healing_history[g_healing_hist_idx];
    g_strlcpy(e->action, action ? action : "", sizeof(e->action));
    g_strlcpy(e->target, target ? target : "", sizeof(e->target));
    g_strlcpy(e->reason, reason ? reason : "", sizeof(e->reason));
    g_strlcpy(e->result, result ? result : "", sizeof(e->result));
    e->timestamp   = g_get_real_time() / G_USEC_PER_SEC;
    e->duration_ms = duration_ms;

    g_healing_hist_idx = (g_healing_hist_idx + 1) % HEALING_HISTORY_MAX;
    if (g_healing_hist_count < HEALING_HISTORY_MAX)
        g_healing_hist_count++;

    g_mutex_unlock(&g_healing_hist_mu);
}

/* ── AF-1: restart 실배선 — 실제 VM 재시작을 워커 풀로 오프로드 ──── */

/**
 * @struct RestartCtx
 * @brief _vm_restart_worker 로 소유권 이전되는 재시작 컨텍스트.
 *
 * self_healing 이벤트 콜체인은 메인/이벤트 스레드에서 실행되므로,
 * 수십 초 블로킹할 수 있는 virDomainCreate 는 워커 스레드로 넘긴다.
 * 세 문자열은 g_strdup 복사본으로, _restart_ctx_free 가 해제한다.
 */
typedef struct {
    gchar *policy_name;  /**< 트리거 정책 이름 (감사/이력용) */
    gchar *vm;           /**< 대상 VM 식별자 (UUID 또는 이름) */
    gchar *reason;       /**< 트리거 사유 */
} RestartCtx;

static void
_restart_ctx_free(gpointer data)
{
    RestartCtx *c = data;
    if (!c) return;
    g_free(c->policy_name);
    g_free(c->vm);
    g_free(c->reason);
    g_free(c);
}

/**
 * _vm_restart_worker — 워커 스레드에서 실제 VM 재시작을 수행한다.
 *
 * 실행 순서:
 *   1. virt_conn_pool_acquire() 로 libvirt 커넥션 획득
 *   2. pure_virt_get_domain() 다형성 조회 (UUID→이름)
 *   3. running-guard: virDomainIsActive()>0 이면 skip (이미 실행 중 — 재시작 안 함)
 *   4. virDomainCreate() 로 실제 기동
 *
 * 안전판:
 *   - 메인 스레드 블로킹 금지 (워커 풀에서 실행)
 *   - running-guard 로 정상 실행 중 VM 오재시작 차단
 *   - executed 카운트/WS "executed"/actions_total 은 **실제 재시작 성공 시에만** 갱신
 *     (running-guard skip·조회 실패·create 실패는 executed 로 세지 않는다 — 정직 보고)
 *   - 결과(success/skipped/failed)를 이력 링버퍼 + 감사 로그에 기록
 *
 * running-guard + virDomainCreate 결정 로직 자체는 self_healing_restart.c 로
 * 추출됐다(PCV_SAFETY_CONTROL 마커도 그쪽으로 이동 — 효과테스트가 닿는 코드에
 * 마커가 있어야 게이트가 검증 가능). 여기서는 libvirt 어댑터(_sh_domain_create)로
 * 감싸 위임한다.
 */
/**
 * _sh_domain_create — pcv_healing_restart_decide() 의 create_fn 어댑터.
 * virDomainCreate() 시그니처(virDomainPtr 인자)를 gpointer 콜백으로 감싼다.
 */
static int
_sh_domain_create(gpointer dom)
{
    return virDomainCreate((virDomainPtr)dom);
}

static void
_vm_restart_worker(GTask *task, gpointer src, gpointer task_data, GCancellable *c)
{
    (void)src; (void)c;
    RestartCtx *ctx = task_data;
    gint64 start_us = g_get_monotonic_time();
    const gchar *result = "failed";
    /* AF-1 후속: VM 단위 브레이커 되먹임 신호.
     *  +1 = 성공(카운터 리셋/CLOSED), -1 = 재시작 실패(카운터++/OPEN 가능),
     *   0 = 미반영(커넥션 획득 실패·도메인 조회 실패는 VM 고장이 아니므로 카운트 제외 —
     *       libvirt-down 은 커넥션 브레이커 소관, 조회 실패는 별도 즉시 로그). */
    gint rb_feedback = 0;

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        PCV_LOG_WARN(HEALING_LOG_DOM,
            "[restart] VM '%s': libvirt 커넥션 획득 실패 — 재시작 중단", ctx->vm);
    } else {
        virDomainPtr dom = pure_virt_get_domain(conn, ctx->vm);
        if (!dom) {
            PCV_LOG_WARN(HEALING_LOG_DOM,
                "[restart] VM '%s' 조회 실패 (UUID/이름 불일치) — 재시작 중단", ctx->vm);
        } else {
            /* 결정 로직(running-guard + create) 위임 — self_healing_restart.c 참조. */
            result = pcv_healing_restart_decide(virDomainIsActive(dom),
                                                _sh_domain_create, dom, &rb_feedback);
            if (g_strcmp0(result, "skipped") == 0) {
                /* running-guard: 이미 실행(또는 paused=active) 중 → 재시작하지 않는다.
                 * VM 이 건강하다는 신호이므로 브레이커를 성공으로 취급해 리셋한다. */
                PCV_LOG_INFO(HEALING_LOG_DOM,
                    "[restart] VM '%s' 이미 실행 중 — running-guard skip", ctx->vm);
            } else if (g_strcmp0(result, "success") == 0) {
                PCV_LOG_WARN(HEALING_LOG_DOM,
                    "[restart] VM '%s' 재시작 성공 (policy=%s)", ctx->vm, ctx->policy_name);
            } else {
                virErrorPtr err = virGetLastError();
                PCV_LOG_WARN(HEALING_LOG_DOM,
                    "[restart] VM '%s' 재시작 실패: %s", ctx->vm,
                    (err && err->message) ? err->message : "(unknown)");
            }
        }
        if (dom) virDomainFree(dom);
        virt_conn_pool_release(conn);
    }

    /* AF-1 후속: 워커 결과를 VM 단위 브레이커에 되먹인다(G.mu 미보유 — g_rb 뮤텍스만).
     * 이 되먹임이 반복 create 실패 시 계층4(OPEN)를 개방해 무한 재시도를 끊는다. */
    if (rb_feedback > 0)      rb_record(ctx->vm, TRUE);
    else if (rb_feedback < 0) rb_record(ctx->vm, FALSE);
    else                      rb_release_probe(ctx->vm);   /* AIO-2: 0-피드백 → 프로브 토큰 회수 */

    gint64 dur_ms = (g_get_monotonic_time() - start_us) / 1000;

    /* executed 회계는 실제 재시작 성공 시에만 (규약: total_executed / actions_total / WS "executed") */
    if (g_strcmp0(result, "success") == 0) {
        g_mutex_lock(&G.mu);
        G.total_executed++;
        gchar lbl[128];
        g_snprintf(lbl, sizeof(lbl), "policy=\"%s\",action=\"restart\"", ctx->policy_name);
        pcv_prom_gauge_set_labels("purecvisor_healing_actions_total", lbl,
                                  (gdouble)G.total_executed);
        g_mutex_unlock(&G.mu);

        extern void pcv_ws_broadcast(const gchar*, const gchar*);
        extern gint pcv_ws_client_count(void);
        if (pcv_ws_client_count() > 0) {
            gchar payload[512];
            g_snprintf(payload, sizeof(payload),
                "{\"policy\":\"%s\",\"action\":\"restart\",\"target\":\"%s\","
                "\"reason\":\"%s\",\"dry_run\":false,\"executed\":true}",
                ctx->policy_name, ctx->vm, ctx->reason);
            pcv_ws_broadcast("healing", payload);
        }
    }

    /* 감사 로그 + 이력: 결과와 무관하게 기록 (정직 보고) */
    gchar detail[384];
    g_snprintf(detail, sizeof(detail), "action=restart target=%s result=%s reason=%s",
               ctx->vm, result, ctx->reason);
    pcv_audit_log("ai-ops", "healing_action", ctx->policy_name, detail, 0, 0, "local");
    _record_healing_action("restart", ctx->vm, ctx->reason, result, dur_ms);

    g_task_return_boolean(task, TRUE);
}

/**
 * _dispatch_vm_restart — 재시작 작업을 워커 풀에 넣는다 (fire-and-forget).
 * _schedule_sg_sync(virt_events.c) 와 동일한 GTask 오프로드 관례.
 */
static void
_dispatch_vm_restart(const gchar *policy_name, const gchar *vm, const gchar *reason)
{
    RestartCtx *ctx = g_new0(RestartCtx, 1);
    ctx->policy_name = g_strdup(policy_name);
    ctx->vm          = g_strdup(vm);
    ctx->reason      = g_strdup(reason);

    GTask *task = g_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(task, ctx, _restart_ctx_free);
    pcv_worker_pool_push(task, _vm_restart_worker);
    g_object_unref(task);  /* 워커 풀이 참조 유지 → 여기서 unref 안전 (dispatcher.c 관례) */
}

/* ── 액션 실행 ───────────────────────────────────────────────── */

/**
 * _execute_action — 정책에 따른 액션을 실행한다.
 *
 * @param p          실행할 정책
 * @param reason     트리거 사유 문자열 (감사 로그/WebSocket 알림에 포함)
 * @param target_vm  (nullable) restart 액션의 실제 대상 VM(UUID/이름). AF-1:
 *                   action="restart" + target_vm 비-NULL + active 모드일 때만
 *                   _dispatch_vm_restart() 로 실제 재시작을 워커에 오프로드한다.
 *                   그 외(alert_only/migrate/미구현)는 기존 경로 그대로.
 *
 * dry_run=TRUE이면 로그만 남기고 실행하지 않는다.
 * 실행 결과에 따라 적응형 쿨다운을 적용한다:
 *   성공: 쿨다운을 base_cooldown_sec으로 리셋, 연속 실패 카운터 0
 *   실패: 쿨다운을 2배로 증가 (최대 COOLDOWN_MAX_SEC=1시간),
 *         연속 실패 카운터 +1 (CIRCUIT_BREAKER_MAX 도달 시 정책 비활성화)
 *
 * 부수 효과:
 *   - 감사 로그 기록 (pcv_audit_log)
 *   - Prometheus 카운터 갱신
 *   - WebSocket 브로드캐스트 (Web UI 실시간 알림)
 *   - 레이트 리밋 기록 (_rate_record)
 *   - 이력 링버퍼 기록 (_record_healing_action)
 */
static void
_execute_action(HealingPolicy *p, const gchar *reason, const gchar *target_vm)
{
    gint64 start_us = g_get_monotonic_time();

    /* AF-1: 대상 VM 이 있는 restart 인지 — dry_run/active 양 분기에서 공용 판정 */
    gboolean restart_with_target =
        (g_strcmp0(p->action, "restart") == 0) && target_vm && *target_vm;

    /* CE-A12: 정책별 dry_run 오버라이드 — policy_dry_run >= 0 이면 정책 설정 우선 */
    gboolean effective_dry_run = (p->policy_dry_run >= 0)
        ? (gboolean)p->policy_dry_run : G.dry_run;

    if (effective_dry_run) {
        /* AF-1: dry_run 존중 — 실제 재시작 없이 "would restart <vm>" 로그만 */
        if (restart_with_target)
            PCV_LOG_INFO(HEALING_LOG_DOM,
                "[DRY RUN] Policy '%s' would restart VM '%s': %s",
                p->name, target_vm, reason);
        else
            PCV_LOG_INFO(HEALING_LOG_DOM,
                "[DRY RUN] Policy '%s' would execute '%s': %s",
                p->name, p->action, reason);

        gchar detail[384];
        g_snprintf(detail, sizeof(detail), "[DRY_RUN] action=%s reason=%s", p->action, reason);
        pcv_audit_log("ai-ops", "healing_dry_run", p->name, detail, 0, 0, "local");

        gchar metric_label[160];
        g_snprintf(metric_label, sizeof(metric_label), "policy=\"%s\"", p->name);
        pcv_prom_gauge_set_labels("purecvisor_healing_policy_triggered_total",
            metric_label, (gdouble)(++G.total_triggered));

        gint64 dur_ms = (g_get_monotonic_time() - start_us) / 1000;
        _record_healing_action(p->action, restart_with_target ? target_vm : p->name,
                               reason, "dry_run", dur_ms);
        return;
    }

    /* AF-1: active 모드 + 대상 VM 이 있는 restart → 실제 재시작을 워커로 오프로드.
     * executed 카운트/WS "executed"/이력(success·skipped·failed)/감사 로그는
     * 워커(_vm_restart_worker)가 실제 결과로 소유·기록한다. 여기서는 트리거 회계와
     * 레이트 리밋·쿨다운만 처리하고 조기 반환 (아래 기존 경로는 건드리지 않음). */
    if (restart_with_target) {
        /* AF-1 후속: VM 단위 재시작 브레이커 게이트. 반복 create 실패로 OPEN 되면
         * cooldown 동안 재시작을 건너뛴다(계층4 실제 개방). cooldown 경과 시
         * rb_allow() 가 HALF_OPEN 프로브 1회를 허용한다. */
        if (!rb_allow(target_vm)) {
            PCV_LOG_WARN(HEALING_LOG_DOM,
                "ACTION: Policy '%s' restart of VM '%s' SKIPPED — 재시작 브레이커 OPEN "
                "(반복 재시작 실패, cooldown 중): %s", p->name, target_vm, reason);

            gchar detail[384];
            g_snprintf(detail, sizeof(detail),
                "action=restart target=%s result=skipped reason=breaker-open trigger=%s",
                target_vm, reason);
            pcv_audit_log("ai-ops", "healing_action_skipped", p->name, detail, 0, 0, "local");

            gchar skip_reason[320];
            g_snprintf(skip_reason, sizeof(skip_reason),
                "breaker-open (재시작 브레이커 차단); trigger=%s", reason);
            gint64 dur_ms = (g_get_monotonic_time() - start_us) / 1000;
            _record_healing_action("restart", target_vm, skip_reason, "skipped", dur_ms);

            /* 트리거는 발생했으나 실행하지 않음 — total_triggered 만 갱신하고
             * rate_record/executed 는 갱신하지 않는다(no-op 이라 rate 예산 미소비). */
            G.total_triggered++;
            return;
        }

        PCV_LOG_WARN(HEALING_LOG_DOM,
            "ACTION: Policy '%s' dispatching restart of VM '%s': %s",
            p->name, target_vm, reason);
        _dispatch_vm_restart(p->name, target_vm, reason);

        G.total_triggered++;
        _rate_record();
        /* 디스패치 성공 → 정책 쿨다운을 원래 값으로 리셋. 워커 측 create 결과는
         * _vm_restart_worker 가 rb_record() 로 VM 단위 브레이커에 되먹인다
         * (AF-1 후속: 비동기 실패 되먹임 배선 완료). */
        p->cooldown_sec = p->base_cooldown_sec;
        p->consecutive_failures = 0;
        return;
    }

    /* [감사 AF-1] alert_only만 실제로 동작한다(경고 발생). restart/migrate 등
     * 비-alert 액션은 실제 VM 오퍼레이션을 디스패치하는 코드가 없어(아래 주석의
     * "separate RPC calls"는 미배선) 아무 복구도 하지 않는다. 이전에는 그럼에도
     * total_executed 증가·dry_run:false 성공 브로드캐스트·이력 "success"를 기록해
     * 운영자에게 거짓 복구 확신을 줬다. 실제 실행 여부를 정직하게 반영한다.
     * (restart/migrate 실배선은 A6-6[graceful/crash 구분]·AF-O1[죽은 메트릭 수복]
     * 선행 후 별도 처리 — 그 전까지는 정상 정지 VM 자동재시작 위험으로 배선 금지.) */
    gboolean action_executed = (g_strcmp0(p->action, "alert_only") == 0);

    if (action_executed) {
        PCV_LOG_WARN(HEALING_LOG_DOM,
            "ALERT: Policy '%s' triggered: %s", p->name, reason);
    } else {
        PCV_LOG_WARN(HEALING_LOG_DOM,
            "ACTION-NOT-IMPLEMENTED: Policy '%s' action '%s' is not wired to a real "
            "VM operation — no remediation performed: %s", p->name, p->action, reason);
    }

    /* Audit log — 실행 여부를 event 종류·detail에 반영 */
    gchar detail[384];
    g_snprintf(detail, sizeof(detail), "action=%s reason=%s executed=%s",
               p->action, reason, action_executed ? "true" : "false");
    pcv_audit_log("ai-ops",
                  action_executed ? "healing_action" : "healing_action_skipped",
                  p->name, detail, 0, 0, "local");

    /* Prometheus counters — trigger는 항상, executed(actions_total)는 실제 실행분만 */
    G.total_triggered++;
    if (action_executed) G.total_executed++;
    gchar lbl[128];
    g_snprintf(lbl, sizeof(lbl), "policy=\"%s\",action=\"%s\"", p->name, p->action);
    pcv_prom_gauge_set_labels("purecvisor_healing_actions_total", lbl, (gdouble)G.total_executed);

    /* WebSocket notification — 실제 실행 여부를 정확히 표기 */
    extern void pcv_ws_broadcast(const gchar*, const gchar*);
    extern gint pcv_ws_client_count(void);
    if (pcv_ws_client_count() > 0) {
        gchar payload[512];
        g_snprintf(payload, sizeof(payload),
            "{\"policy\":\"%s\",\"action\":\"%s\",\"reason\":\"%s\",\"dry_run\":false,\"executed\":%s}",
            p->name, p->action, reason, action_executed ? "true" : "false");
        pcv_ws_broadcast("healing", payload);
    }

    _rate_record();

    /* 트리거를 처리했으므로 쿨다운은 기본값으로 리셋(실제 실행 실패 경로는
     * restart/migrate 실배선 시 추가 — 그때 backoff 재도입). */
    p->cooldown_sec = p->base_cooldown_sec;
    p->consecutive_failures = 0;

    /* Record action history — 미실행 액션은 "not_implemented"로 정직 기록 */
    gint64 dur_ms = (g_get_monotonic_time() - start_us) / 1000;
    _record_healing_action(p->action, p->name, reason,
                           action_executed ? "success" : "not_implemented", dur_ms);
}

/* ── 승인 대기열에 추가 ──────────────────────────────────────── */

/**
 * _queue_approval — 위험 액션을 Web UI 승인 대기열에 추가한다.
 *
 * require_approval=TRUE인 정책이 트리거되면 즉시 실행하지 않고
 * PendingAction 링버퍼에 기록한다. Web UI에 WebSocket으로
 * "healing-request" 이벤트를 브로드캐스트하여 관리자에게 알린다.
 *
 * @param p       트리거된 정책
 * @param reason  트리거 사유 (관리자에게 표시)
 */
static void
_queue_approval(HealingPolicy *p, const gchar *reason)
{
    PendingAction *pa = &G.pending[G.pending_pos];
    /* A6-5: 고정 링버퍼(MAX_PENDING)에서 관리자가 승인/거부하기 전에 슬롯이
     * 덮어써지면(eviction) 그 액션의 id 가 사라져 approve/dismiss 가 영영
     * 찾지 못하고 total_pending-- 기회가 없어져 gauge 가 무한 증가한다.
     * 덮어쓰기 직전, 기존 슬롯이 미해결이면 eviction 을 dismiss 와 동등하게
     * 취급해 카운터를 내린다(id!=0 으로 초기 미사용 슬롯과 구분). */
    if (pa->id != 0 && !pa->resolved && G.total_pending > 0) {
        G.total_pending--;
    }
    pa->id = ++G.next_action_id;
    g_strlcpy(pa->policy_name, p->name, sizeof(pa->policy_name));
    g_strlcpy(pa->action, p->action, sizeof(pa->action));
    g_strlcpy(pa->reason, reason, sizeof(pa->reason));
    pa->created_us = g_get_real_time();
    pa->resolved = FALSE;
    G.pending_pos = (G.pending_pos + 1) % MAX_PENDING;
    if (G.pending_count < MAX_PENDING) G.pending_count++;

    G.total_pending++;
    pcv_prom_gauge_set_labels("purecvisor_healing_pending_approvals", "",
        (gdouble)G.total_pending);

    /* WebSocket push approval request */
    extern void pcv_ws_broadcast(const gchar*, const gchar*);
    extern gint pcv_ws_client_count(void);
    if (pcv_ws_client_count() > 0) {
        gchar payload[512];
        g_snprintf(payload, sizeof(payload),
            "{\"id\":%d,\"policy\":\"%s\",\"action\":\"%s\",\"reason\":\"%s\"}",
            pa->id, p->name, p->action, reason);
        pcv_ws_broadcast("healing-request", payload);
    }

    PCV_LOG_INFO(HEALING_LOG_DOM,
        "Approval required: [%d] policy='%s' action='%s' — %s",
        pa->id, p->name, p->action, reason);
}

/* ── 정책 매칭 — 5중 안전장치 검사 후 액션 실행 또는 승인 대기 ── */

/**
 * _try_policy:
 * @p:      평가할 정책
 * @metric: 트리거된 메트릭 이름
 * @value:  현재 메트릭 값
 * @zscore: 이상 탐지 Z-Score
 * @reason: 트리거 사유 문자열
 *
 * 정책의 안전장치를 순서대로 검사한 후 액션을 실행하거나 승인 대기열에 추가합니다.
 *
 * 검사 순서 (하나라도 실패하면 즉시 반환):
 *   1. enabled 확인 — 비활성 정책은 건너뜀
 *   2. 쿨다운 확인 — cooldown_sec 이내 재실행 방지
 *   3. 서킷 브레이커 — 연속 3회 실패 시 정책 비활성화 (OPEN 상태)
 *   4. 레이트 리밋 — 5분 내 최대 3개 자동 액션 (alert_only는 제외)
 *
 * 모든 검사 통과 후:
 *   - require_approval=TRUE: _queue_approval() → Web UI 승인 대기
 *   - require_approval=FALSE 또는 alert_only: _execute_action() → 즉시 실행
 */
static void
_try_policy(HealingPolicy *p, const gchar *metric, gdouble value,
            gdouble zscore, const gchar *reason, const gchar *target_vm)
{
    if (!p->enabled) return;

    /* Cooldown check */
    gint64 now = g_get_monotonic_time();
    if (now - p->last_trigger_us < p->cooldown_sec * G_USEC_PER_SEC)
        return;

    /* Circuit breaker */
    if (p->consecutive_failures >= CIRCUIT_BREAKER_MAX) {
        PCV_LOG_WARN(HEALING_LOG_DOM,
            "Circuit breaker OPEN for policy '%s' (%d consecutive failures)",
            p->name, p->consecutive_failures);
        return;
    }

    /* Rate limit */
    if (g_strcmp0(p->action, "alert_only") != 0 && !_rate_check())
        return;

    p->last_trigger_us = now;

    if (p->require_approval && g_strcmp0(p->action, "alert_only") != 0) {
        _queue_approval(p, reason);
    } else {
        _execute_action(p, reason, target_vm);
    }
}

/* ── Public API ──────────────────────────────────────────────── */

void
pcv_healing_init(void)
{
    g_mutex_init(&G.mu);
    g_mutex_init(&g_healing_hist_mu);
    g_mutex_init(&g_anomaly_mu);
    G.initialized = TRUE;

    /* Issue-M1 fix: daemon.conf [ai] mode=active 설정 반영.
     * 이전에는 항상 dry_run=TRUE로 고정되어 실제 자가치유가 실행되지 않았음.
     * 지원 값: "active" → dry_run=FALSE, 그 외 → dry_run=TRUE (safe default). */
    const gchar *mode = pcv_config_get_string("ai", "mode", "dry_run");
    G.dry_run = (g_ascii_strcasecmp(mode, "active") != 0);

    /* AF-1 후속: VM 단위 재시작 서킷 브레이커 초기화.
     * _vm_restart_worker 의 virDomainCreate 실패는 비동기라 정책 쿨다운(300s)만
     * 지나면 무한 재시도된다. 브레이커가 VM 별 연속 실패를 누적해 계층4를 실제로
     * 개방(OPEN)하여 되먹임 고리를 닫는다. threshold/cooldown 은 [ai] 로 설정. */
    rb_init();
    gint rb_threshold = pcv_config_get_int("ai", "restart_breaker_threshold",
                                           RESTART_BREAKER_THRESHOLD_DEFAULT);
    gint rb_cooldown  = pcv_config_get_int("ai", "restart_breaker_cooldown_sec",
                                           RESTART_BREAKER_COOLDOWN_SEC_DEFAULT);
    rb_configure(rb_threshold, rb_cooldown);

    /* Built-in policies (architecture doc section 6.3)
     *
     * AF-O1(a): single_edge 빌드는 마이그레이션 대상 노드가 없어 migrate가
     * 무의미하다 — AF-O1(a) 로 host cpu/mem 메트릭이 이제 실제 트리거되므로,
     * "migrate" 로 두면 실행 불가능한 액션이 승인 큐에만 쌓인다(감사 테마 #1
     * "보고성공-무동작" 신규 인스턴스). 따라서 cpu-overload/mem-pressure 는
     * thermal-alert 와 동일하게 alert_only(무승인)로 운영한다. */
    _add_policy("cpu-overload", "purecvisor_host_cpu_percent",
        3.0, 85.0, "alert_only", 600, FALSE);
    _add_policy("mem-pressure", "purecvisor_host_memory_percent",
        2.5, 90.0, "alert_only", 600, FALSE);
    _add_policy("thermal-alert", "node_hwmon_temp_celsius",
        2.0, 80.0, "alert_only", 1800, FALSE);
    /* BUG-20 fix: trigger_metric="vm-unresponsive"로 설정하여 virt_events.c의
     * pcv_healing_on_anomaly("vm-unresponsive", ...) 호출이 매칭되도록 함.
     * 이전에는 trigger_metric=""이어서 핸들러가 즉시 continue로 스킵했음. */
    _add_policy("vm-unresponsive", "vm-unresponsive",
        0, 0, "restart", 300, FALSE);
    _add_policy("swap-storm", "node_vmstat_pswpout",
        2.5, 0, "alert_only", 300, FALSE);
    _add_policy("disk-saturated", "node_disk_io_time_seconds_total",
        3.0, 0, "alert_only", 600, FALSE);
    _add_policy("net-errors", "node_network_receive_errs_total",
        2.0, 0, "alert_only", 300, FALSE);
    _add_policy("conntrack-full", "node_nf_conntrack_entries",
        2.5, 0, "alert_only", 600, FALSE);
    /* 1.0: vm-reboot-loop synthetic 정책 (ADR-0020 규칙 4 준수)
     * - virt_events.c가 윈도우(10분) 내 5회+ stop 감지 시 호출
     * - alert_only로 시작 (자동 migrate은 위험: 부팅 실패 원인 분석 우선)
     * - 운영자가 audit 로그 확인 후 수동 처치 */
    _add_policy("vm-reboot-loop", "vm-reboot-loop",
        0, 0, "alert_only", 1200, FALSE);
    /* 1.0: vm-migration-failed synthetic 정책
     * - handler_cluster.c::_migrate_thread가 윈도우(30분) 내 3회+ 실패 시 호출
     * - alert_only (자동 retry 위험: 동일 원인 반복 가능)
     * - 운영자가 SSH key/네트워크/디스크 공간 확인 후 수동 재시도 */
    _add_policy("vm-migration-failed", "vm-migration-failed",
        0, 0, "alert_only", 1800, FALSE);

    PCV_LOG_INFO(HEALING_LOG_DOM,
        "Self-healing initialized — %d policies, mode=%s",
        G.policy_count, G.dry_run ? "dry_run" : "active");
}

/**
 * Issue-M2 fix: 런타임 모드 전환.
 * healing.set_mode RPC 경로: dispatcher → _handle_healing_set_mode →
 * pcv_healing_set_mode → G.dry_run 변경.
 * 감사 로그에 모드 변경 기록 (운영자 추적용).
 */
void
pcv_healing_set_mode(gboolean dry_run)
{
    if (!G.initialized) return;
    g_mutex_lock(&G.mu);
    gboolean prev = G.dry_run;
    G.dry_run = dry_run;
    g_mutex_unlock(&G.mu);
    if (prev != dry_run) {
        PCV_LOG_WARN(HEALING_LOG_DOM,
            "Mode changed: %s → %s",
            prev ? "dry_run" : "active",
            dry_run ? "dry_run" : "active");
        pcv_audit_log("ai-ops", "healing_mode_change",
            dry_run ? "dry_run" : "active",
            prev ? "was_dry_run" : "was_active", 0, 0, "local");
    }
}

gboolean
pcv_healing_get_mode(void)
{
    return G.initialized ? G.dry_run : TRUE;
}

void
pcv_healing_shutdown(void)
{
    if (!G.initialized) return;
    G.initialized = FALSE;
    rb_shutdown();   /* AF-1 후속: 재시작 브레이커 자원 해제 */
    g_mutex_clear(&G.mu);
    g_mutex_clear(&g_healing_hist_mu);
    g_mutex_clear(&g_anomaly_mu);
}

/**
 * pcv_healing_on_anomaly:
 * @metric:    이상이 감지된 메트릭 이름
 * @value:     현재 메트릭 값
 * @zscore:    이상 탐지 Z-Score
 * @threshold: 해당 메트릭의 Z-Score 임계값
 *
 * anomaly_detector에서 이상 감지 시 호출되는 이벤트 핸들러입니다.
 * 등록된 정책 중 trigger_metric이 일치하고 zscore가 기준을 초과하는
 * 정책을 찾아 _try_policy()로 평가합니다.
 *
 * 복합 조건: 2개 이상 정책이 동시 트리거되면 AI Agent에 자문을 요청합니다.
 * → pcv_agent_compare_async()로 4개 프로바이더 병렬 질의
 */
void
pcv_healing_on_anomaly(const gchar *metric, gdouble value,
                        gdouble zscore, gdouble threshold,
                        const gchar *target_vm)
{
    if (!G.initialized) return;

    g_mutex_lock(&G.mu);

    gint triggered_count = 0;
    gchar triggered_metrics[512] = {0};

    for (gint i = 0; i < G.policy_count; i++) {
        HealingPolicy *p = &G.policies[i];
        if (!p->trigger_metric[0]) continue;
        if (strstr(metric, p->trigger_metric) == NULL) continue;
        if (zscore < p->trigger_zscore) continue;

        triggered_count++;
        gchar frag[128];
        g_snprintf(frag, sizeof(frag), "%s(Z=%.1f) ", metric, zscore);
        g_strlcat(triggered_metrics, frag, sizeof(triggered_metrics));

        gchar reason[256];
        g_snprintf(reason, sizeof(reason),
            "%s Z=%.2f (>%.1f) val=%.2f", metric, zscore, threshold, value);
        _try_policy(p, metric, value, zscore, reason, target_vm);
    }

    g_mutex_unlock(&G.mu);

    /* 1.0: 시간 윈도우 누적 검사 — 60초 내 distinct metric 3개 이상이면
     * triggered_count(단일 호출 내 정책 수)와 별개로 AI Agent 트리거 */
    gboolean window_trigger = _track_distinct_anomaly(metric);

    /* 복합 조건: (단일 호출 내 2개+ 정책) 또는 (시간 윈도우 누적 3 metric+) */
    if (triggered_count >= 2 || window_trigger) {
        JsonObject *host = pcv_ebpf_telemetry_get_host();
        gchar *host_json = NULL;
        if (host) {
            JsonNode *n = json_node_new(JSON_NODE_OBJECT);
            json_node_set_object(n, host);
            host_json = json_to_string(n, FALSE);
            json_node_free(n);
            json_object_unref(host);
        }

        gchar context[768];
        if (window_trigger) {
            g_snprintf(context, sizeof(context),
                "Multi-anomaly time-window: distinct metrics in 60s exceeded threshold (this metric=%s, Z=%.2f). "
                "Recently triggered: %s",
                metric, zscore, triggered_metrics);
            PCV_LOG_INFO(HEALING_LOG_DOM,
                "Time-window trigger: 3+ distinct anomalies in 60s — dispatching AI Agent");
        } else {
            g_snprintf(context, sizeof(context),
                "Multi-anomaly detected: %d policies triggered simultaneously: %s",
                triggered_count, triggered_metrics);
            PCV_LOG_INFO(HEALING_LOG_DOM,
                "Complex condition: %d simultaneous anomalies — dispatching AI Agent",
                triggered_count);
        }

        pcv_agent_compare_async(host_json, context);
        g_free(host_json);
    }
}

/**
 * pcv_healing_on_prediction:
 * @cpu_pred:  5분 후 CPU 예측값 (%)
 * @mem_pred:  5분 후 MEM 예측값 (%)
 * @cpu_trend: CPU 추세 기울기 (양수=상승)
 * @mem_trend: MEM 추세 기울기
 *
 * workload_predict에서 예측값이 갱신될 때 호출되는 이벤트 핸들러입니다.
 * predict_threshold가 설정된 정책에 대해, 예측값이 임계값을 초과하면
 * _try_policy()를 호출하여 사전 대응(preventive action)을 수행합니다.
 *
 * 예) cpu-overload 정책: predict_threshold=85 → 5분 후 CPU 85% 예측 시 migrate 트리거
 */
void
pcv_healing_on_prediction(gdouble cpu_pred, gdouble mem_pred,
                           gdouble cpu_trend, gdouble mem_trend)
{
    if (!G.initialized) return;

    g_mutex_lock(&G.mu);

    for (gint i = 0; i < G.policy_count; i++) {
        HealingPolicy *p = &G.policies[i];
        if (p->predict_threshold <= 0) continue;

        if (strstr(p->trigger_metric, "cpu") && cpu_pred > p->predict_threshold) {
            gchar reason[256];
            g_snprintf(reason, sizeof(reason),
                "CPU predicted %.1f%% in 5min (threshold %.0f%%, trend=%.4f)",
                cpu_pred, p->predict_threshold, cpu_trend);
            _try_policy(p, "cpu_predicted", cpu_pred, 0, reason, NULL);
        }
        if (strstr(p->trigger_metric, "memory") && mem_pred > p->predict_threshold) {
            gchar reason[256];
            g_snprintf(reason, sizeof(reason),
                "MEM predicted %.1f%% in 5min (threshold %.0f%%, trend=%.4f)",
                mem_pred, p->predict_threshold, mem_trend);
            _try_policy(p, "mem_predicted", mem_pred, 0, reason, NULL);
        }
    }

    g_mutex_unlock(&G.mu);
}

/**
 * pcv_healing_get_pending_json:
 *
 * 승인 대기 중인 액션 목록을 JSON 배열 문자열로 반환합니다.
 * Web UI의 Self-Healing 패널에서 관리자가 승인/거부할 수 있도록 표시됩니다.
 * resolved=TRUE인 항목은 건너뛰고, 미처리 항목만 포함합니다.
 *
 * Returns: (transfer full): JSON 배열 문자열 (g_free 필요)
 */
gchar *
pcv_healing_get_pending_json(void)
{
    g_mutex_lock(&G.mu);

    GString *buf = g_string_new("[");
    for (gint i = 0; i < G.pending_count; i++) {
        gint idx = (G.pending_pos - G.pending_count + i + MAX_PENDING) % MAX_PENDING;
        PendingAction *pa = &G.pending[idx];
        if (pa->resolved) continue;
        if (buf->len > 1) g_string_append_c(buf, ',');
        g_string_append_printf(buf,
            "{\"id\":%d,\"policy\":\"%s\",\"action\":\"%s\","
            "\"reason\":\"%s\",\"ts\":%ld}",
            pa->id, pa->policy_name, pa->action, pa->reason,
            (long)(pa->created_us / G_USEC_PER_SEC));
    }
    g_string_append_c(buf, ']');

    g_mutex_unlock(&G.mu);
    return g_string_free(buf, FALSE);
}

/**
 * pcv_healing_approve:
 * @action_id: 승인할 액션 ID (Web UI에서 선택)
 *
 * 대기열에서 해당 ID의 액션을 찾아 승인 처리합니다.
 * resolved=TRUE로 표시하고, 매칭되는 정책의 _execute_action()을 호출합니다.
 */
void
pcv_healing_approve(gint action_id)
{
    g_mutex_lock(&G.mu);
    for (gint i = 0; i < MAX_PENDING; i++) {
        if (G.pending[i].id == action_id && !G.pending[i].resolved) {
            G.pending[i].resolved = TRUE;
            G.total_pending--;
            pcv_prom_gauge_set_labels("purecvisor_healing_pending_approvals", "",
                (gdouble)G.total_pending);

            /* Find matching policy and execute.
             * 승인 큐에는 대상 VM 이 저장되지 않으므로 target_vm=NULL.
             * (restart 는 require_approval=FALSE 라 승인 경로를 타지 않는다.) */
            for (gint j = 0; j < G.policy_count; j++) {
                if (g_strcmp0(G.policies[j].name, G.pending[i].policy_name) == 0) {
                    _execute_action(&G.policies[j], G.pending[i].reason, NULL);
                    break;
                }
            }

            PCV_LOG_INFO(HEALING_LOG_DOM, "Action [%d] APPROVED: %s",
                action_id, G.pending[i].reason);
            break;
        }
    }
    g_mutex_unlock(&G.mu);
}

/**
 * pcv_healing_dismiss:
 * @action_id: 거부할 액션 ID
 *
 * 대기열에서 해당 ID의 액션을 찾아 거부 처리합니다.
 * resolved=TRUE로 표시만 하고 액션은 실행하지 않습니다.
 */
void
pcv_healing_dismiss(gint action_id)
{
    g_mutex_lock(&G.mu);
    for (gint i = 0; i < MAX_PENDING; i++) {
        if (G.pending[i].id == action_id && !G.pending[i].resolved) {
            G.pending[i].resolved = TRUE;
            G.total_pending--;
            pcv_prom_gauge_set_labels("purecvisor_healing_pending_approvals", "",
                (gdouble)G.total_pending);

            PCV_LOG_INFO(HEALING_LOG_DOM, "Action [%d] DISMISSED: %s",
                action_id, G.pending[i].reason);
            break;
        }
    }
    g_mutex_unlock(&G.mu);
}

/**
 * pcv_healing_get_history_json:
 *
 * 자가 치유 액션 이력을 JSON 배열 문자열로 반환합니다.
 * 최신 엔트리부터 역순으로 최대 HEALING_HISTORY_MAX개를 포함합니다.
 *
 * Returns: (transfer full): JSON 배열 문자열 (g_free 필요)
 */
gchar *
pcv_healing_get_history_json(void)
{
    g_mutex_lock(&g_healing_hist_mu);

    GString *buf = g_string_new("[");
    for (gint i = 0; i < g_healing_hist_count; i++) {
        /* iterate newest-first */
        gint idx = (g_healing_hist_idx - 1 - i + HEALING_HISTORY_MAX) % HEALING_HISTORY_MAX;
        HealingHistoryEntry *e = &g_healing_history[idx];
        if (buf->len > 1) g_string_append_c(buf, ',');
        g_string_append_printf(buf,
            "{\"action\":\"%s\",\"target\":\"%s\",\"reason\":\"%s\","
            "\"result\":\"%s\",\"timestamp\":%ld,\"duration_ms\":%ld}",
            e->action, e->target, e->reason, e->result,
            (long)e->timestamp, (long)e->duration_ms);
    }
    g_string_append_c(buf, ']');

    g_mutex_unlock(&g_healing_hist_mu);
    return g_string_free(buf, FALSE);
}
