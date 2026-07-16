/**
 * @file alert_engine.c
 * @brief WhaTap 스타일 알림 엔진 — 임계값 평가 + Webhook 발송 (구현부)
 *
 * ============================================================================
 *  파일 역할
 * ============================================================================
 *  호스트 CPU/메모리/디스크 사용률을 5초 간격으로 수집하여 설정된 임계값과
 *  비교하고, eval_period(기본 30초) 동안 연속 초과 시 알림을 발생시킨다.
 *  알림은 링버퍼(최근 100건)에 저장되며, Webhook URL이 설정되어 있으면
 *  Slack/Telegram/Generic JSON 포맷으로 HTTP POST 전송한다.
 *
 * ============================================================================
 *  아키텍처 위치
 * ============================================================================
 *  현재 에디션 데몬 프로세스 내 "alert-engine" GThread로 동작.
 *  ebpf_telemetry 모듈에서 CPU/메모리 메트릭을 읽고, 디스크는 statvfs로
 *  직접 측정한다. REST/RPC 핸들러에서 히스토리 조회 및 설정 변경 API를 호출.
 *
 *    ebpf_telemetry.c ──(cpu%, mem%)──→ alert_engine.c
 *    statvfs("/")     ──(disk%)──────→ alert_engine.c
 *                                         │
 *                                    ┌────┴─────┐
 *                                    │ 임계값    │
 *                                    │ 평가 루프 │
 *                                    └────┬─────┘
 *                              ┌──────────┼──────────┐
 *                              ▼          ▼          ▼
 *                         링버퍼 저장  PCV_LOG_WARN  Webhook POST
 *
 * ============================================================================
 *  스레드 안전성
 * ============================================================================
 *  - 전역 상태(G)는 GMutex(G.mu)로 보호. 히스토리 읽기/쓰기, 설정 변경 시
 *    반드시 락을 잡는다.
 *  - 단, _eval_metric()과 _fire_alert()는 alert-engine 스레드에서만 호출되므로
 *    MetricWatch 필드 자체는 단일 스레드 접근. 히스토리 기록 시에만 락 사용.
 *
 * ============================================================================
 *  핵심 알고리즘: 임계값 평가 (WhaTap 지속 조건 모델)
 * ============================================================================
 *  _eval_metric() 함수가 각 메트릭에 대해 다음 로직을 수행:
 *
 *  [CRIT 평가]
 *    1. current_pct >= crit_thresh ?
 *       - YES → crit_since가 0이면 현재 시각을 기록 (조건 시작 시점)
 *              → (현재 시각 - crit_since) >= eval_period이고 아직 미발화이면 알림 발생
 *       - NO  → crit_since=0, crit_fired=FALSE로 초기화 (에피소드 종료)
 *
 *  [WARN 평가] — CRIT 미해당일 때만 (상호 배제)
 *    1. warn_thresh <= current_pct < crit_thresh ?
 *       - YES → warn_since가 0이면 현재 시각을 기록
 *              → (현재 시각 - warn_since) >= eval_period이고 아직 미발화이면 알림 발생
 *       - NO  → warn_since=0, warn_fired=FALSE로 초기화
 *
 *  이 구조 덕분에:
 *  - 순간 스파이크는 since 시각이 설정되지만 eval_period 미충족으로 무시됨
 *  - 한 에피소드에서 알림은 1회만 발생 (fired 플래그)
 *  - 값이 임계값 아래로 내려가면 since/fired 모두 리셋 → 재초과 시 새 에피소드
 *
 * ============================================================================
 *  Webhook 페이로드 포맷별 차이
 * ============================================================================
 *  1) slack (webhook_format="slack")
 *     → Slack Incoming Webhook API 호환
 *     → 포맷: {"text":"PureCVisor Alert: [WARN] CPU 85.2% on hostname at 2026-03-24 10:00:00"}
 *     → Slack이 "text" 키를 읽어 채널에 메시지로 표시
 *
 *  2) telegram (webhook_format="telegram")
 *     → Telegram Bot API sendMessage 호환
 *     → 포맷: {"chat_id":"123456","text":"PureCVisor Alert: [CRIT] Memory 96.3% on ..."}
 *     → telegram_chat_id 설정값이 chat_id로 삽입됨
 *     → webhook_url은 https://api.telegram.org/bot<TOKEN>/sendMessage 형태여야 함
 *
 *  3) generic (webhook_format="generic", 기본값)
 *     → 범용 JSON 포맷, 커스텀 수신 서버/PagerDuty/자체 시스템 연동용
 *     → 포맷: {"severity":"WARN","metric":"CPU","value":85.2,"host":"hostname","timestamp":"2026-03-24 10:00:00"}
 *     → 필드가 구조화되어 있어 수신 측에서 파싱/자동화에 용이
 *
 * ============================================================================
 *  링버퍼 구조 (히스토리)
 * ============================================================================
 *  - history[ALERT_HISTORY_MAX] 고정 크기 배열을 환형 버퍼로 사용
 *  - hist_idx: 다음 기록할 위치 (0에서 시작, MAX 도달 시 0으로 순환)
 *  - hist_count: 저장된 총 기록 수 (MAX 초과 불가)
 *  - 읽기 시 start 위치 계산:
 *    - count < MAX → 0부터 count개 순서대로
 *    - count == MAX → hist_idx(가장 오래된 것)부터 MAX개 순서대로
 */
#include "alert_engine.h"
#include "alert_silence.h"
#include "alert_dlq.h"
#include "ebpf_telemetry.h"
#include "utils/pcv_config.h"
#include "utils/pcv_log.h"
#include "utils/pcv_spawn.h"
#include "utils/pcv_ssrf.h"
#if PCV_CLUSTER_ENABLED
#include "../cluster/cluster_manager.h"
#endif
#include <libsoup/soup.h>
#include <string.h>
#include <time.h>
#include <sys/statvfs.h>

/* ── 상수 정의 ──────────────────────────────────────────────── */

/** 로그 출력 시 사용할 도메인 태그. journalctl 필터링에 활용. */
/*
 * ============================================================================
 *  [주니어 개발자 필독] 알림 엔진 핵심 개념 정리
 * ============================================================================
 *
 *  1. WhaTap 지속 조건 모델 알고리즘 (_eval_metric)
 *     단순 임계값 초과만으로 알림을 발생시키면 순간 스파이크에도 알림이
 *     남발됩니다. WhaTap의 "지속 조건" 모델은:
 *       - 초과 시작 시각(xxx_since)을 기록
 *       - eval_period(30초) 동안 연속 초과해야만 알림 발생
 *       - 한 에피소드에서 1회만 발화 (fired 플래그)
 *       - 값이 정상으로 돌아오면 리셋 → 재초과 시 새 에피소드 시작
 *     이 구조로 노이즈 알림을 90% 이상 줄일 수 있습니다.
 *
 *  2. 비동기 Webhook GTask (_webhook_post_async)
 *     Webhook 서버가 느리면 알림 평가 루프가 블로킹됩니다.
 *     GTask로 비동기 실행하여 평가 스레드의 5초 주기를 보장합니다.
 *     실패 시 지수 백오프 재시도(1s, 2s, 4s) 후 DLQ에 저장합니다.
 *
 *  3. ACK/에스컬레이션 흐름
 *     알림 발생 → history 링버퍼에 기록 (acknowledged=FALSE)
 *     → Web UI에서 관리자가 ACK → acknowledged=TRUE
 *     → 미ACK 알림이 설정 시간 경과 시 CRIT URL로 에스컬레이션
 *
 *  4. SLA 추적
 *     알림 기록의 fired_at + acknowledged 타임스탬프를 비교하여
 *     MTTA(평균 응답 시간)를 산출할 수 있습니다.
 *
 *  5. per-VM 알림 라우팅
 *     향후 VM별로 다른 Webhook URL이나 임계값을 설정할 수 있도록
 *     CompositeRule 구조가 확장 가능하게 설계되어 있습니다.
 *
 *  6. DLQ (Dead Letter Queue) 동작
 *     Webhook 전송이 모든 재시도(3회)에서 실패하면 pcv_alert_dlq_store()로
 *     g_webhook_dlq GPtrArray에 저장됩니다. 최대 1000건까지 보관하며,
 *     초과 시 드롭됩니다. DLQ 재전송은 alert.dlq.retry RPC로 제공됩니다.
 * ============================================================================
 */

#define ALERT_LOG_DOM      "alert_engine"

/** 알림 평가 주기 (초). 5초마다 메트릭을 읽고 임계값을 평가한다. */
constexpr int ALERT_CHECK_SEC = 5;

/** 히스토리 링버퍼의 최대 크기. 1000건을 초과하면 가장 오래된 기록부터 덮어쓴다. */
constexpr int ALERT_HISTORY_MAX = 1000;

/** 알림 중복 제거 윈도우 기본값 (초). 동일 메트릭:레벨 알림을 이 시간 내 재발화 억제. */
constexpr int ALERT_DEDUP_WINDOW_SEC = 300;

/** 복합 알림 규칙 최대 개수 */
constexpr int MAX_COMPOSITE_RULES = 8;

/* ── 타입 정의 ──────────────────────────────────────────────── */

/**
 * @enum AlertLevel
 * @brief 알림 심각도 단계 (WhaTap 다단계 심각도 모델)
 *
 * WARN과 CRIT 두 단계로 분리하여 운영자가 상황에 따라 차등 대응할 수 있게 한다.
 * 예: WARN → Slack 알림만, CRIT → 긴급 전화/PagerDuty 연동
 */
typedef enum {
    ALERT_NONE = 0,    /**< 정상 상태 — 알림 없음 */
    ALERT_WARN,        /**< 경고 — 주의 필요 (예: CPU 80% 이상 지속) */
    ALERT_CRIT         /**< 위험 — 즉각 대응 필요 (예: CPU 95% 이상 지속) */
} AlertLevel;

/**
 * @struct AlertRecord
 * @brief 링버퍼에 저장되는 단일 알림 기록
 *
 * 알림이 발생할 때마다 하나의 AlertRecord가 링버퍼에 기록된다.
 * REST API(get_history)를 통해 JSON으로 직렬화되어 외부에 노출된다.
 */
typedef struct {
    gchar      metric[16];     /**< 메트릭 이름. "CPU", "Memory", "Disk" 중 하나 */
    AlertLevel level;          /**< 알림 심각도. ALERT_WARN 또는 ALERT_CRIT */
    gdouble    value;          /**< 알림 발생 시점의 실측값 (%, 0.0~100.0) */
    gint64     fired_at;       /**< 알림 발생 시각 (Unix epoch 초, time(NULL) 기반) */
    gchar      message[256];   /**< 사람이 읽을 수 있는 알림 메시지 전문
                                *   예: "[WARN] CPU 85.2% on pcv-prod-node-1 at 2026-03-24 10:00:00" */
    gint64     alert_id;       /**< BE-A14: 알림 고유 ID (순차 증가) */
    gboolean   acknowledged;   /**< BE-A14: ACK 여부 (TRUE=확인됨) */
    gboolean   escalated;      /**< BE-A15: 에스컬레이션 전송 여부 */
} AlertRecord;

/**
 * @struct MetricWatch
 * @brief 개별 메트릭(CPU/메모리/디스크)의 임계값 + 지속 추적 상태
 *
 * WhaTap의 "지속 조건(sustained condition)" 개념을 구현한 핵심 구조체.
 * 각 메트릭마다 하나씩 존재하며, 임계값 초과가 시작된 시점(xxx_since)과
 * 해당 에피소드에서 이미 알림을 발생시켰는지(xxx_fired)를 추적한다.
 *
 * [상태 전이 예시 — CPU, eval_period=30초]
 *   t=0:  CPU 50% → warn_since=0, crit_since=0 (정상)
 *   t=5:  CPU 82% → warn_since=5 (경고 조건 시작)
 *   t=10: CPU 85% → warn_since=5 유지 (5-5=0 < 30, 아직 미발화)
 *   t=35: CPU 83% → warn_since=5 유지 (35-5=30 >= 30, 알림 발화! warn_fired=TRUE)
 *   t=40: CPU 83% → warn_fired=TRUE이므로 추가 알림 없음
 *   t=45: CPU 70% → warn_since=0, warn_fired=FALSE (에피소드 종료, 리셋)
 */
typedef struct {
    gdouble    warn_thresh;    /**< 경고 임계값 (%, 예: 80.0). daemon.conf에서 정수로 설정. */
    gdouble    crit_thresh;    /**< 위험 임계값 (%, 예: 95.0). warn_thresh보다 커야 의미 있음. */
    gint64     warn_since;     /**< 경고 조건 시작 시각 (모노토닉 초).
                                *   0이면 현재 경고 조건이 아님(정상 또는 CRIT).
                                *   임계값 초과 첫 감지 시점의 _mono_now() 값이 기록됨. */
    gint64     crit_since;     /**< 위험 조건 시작 시각 (모노토닉 초). 의미는 warn_since와 동일. */
    gboolean   warn_fired;     /**< 현재 에피소드에서 경고 알림을 이미 발생시켰는지 여부.
                                *   TRUE이면 조건이 해소될 때까지 추가 알림을 억제한다. */
    gboolean   crit_fired;     /**< 현재 에피소드에서 위험 알림을 이미 발생시켰는지 여부. */
    gint64     last_warn_fired_at;  /**< 마지막 WARN 발화 시각 (모노토닉 초). dedup 윈도우 판정용. */
    gint64     last_crit_fired_at;  /**< 마지막 CRIT 발화 시각 (모노토닉 초). dedup 윈도우 판정용. */
} MetricWatch;

/**
 * @enum CompositeOp
 * @brief 복합 알림 규칙의 논리 연산자 (AND 또는 OR)
 */
typedef enum {
    COMPOSITE_OP_AND = 0,  /**< 두 조건 모두 충족 시 알림 */
    COMPOSITE_OP_OR  = 1   /**< 하나라도 충족 시 알림 */
} CompositeOp;

/**
 * @struct CompositeRule
 * @brief 두 메트릭을 AND/OR로 조합하는 복합 알림 규칙
 *
 * 예: "CPU >= 80 AND Memory >= 70" → 동시 초과 시만 알림 발생.
 * eval_period 지속 조건과 dedup_window 중복 제거가 동일하게 적용된다.
 */
typedef struct {
    gboolean    active;         /**< 규칙 활성화 여부 */
    CompositeOp op;             /**< AND 또는 OR */
    gchar       metric_a[16];   /**< 첫 번째 메트릭 ("CPU", "Memory", "Disk") */
    gdouble     thresh_a;       /**< 첫 번째 메트릭 임계값 */
    gchar       metric_b[16];   /**< 두 번째 메트릭 */
    gdouble     thresh_b;       /**< 두 번째 메트릭 임계값 */
    AlertLevel  level;          /**< 알림 심각도 (WARN 또는 CRIT) */
    gint64      since;          /**< 조건 시작 시각 (모노토닉 초) */
    gboolean    fired;          /**< 현재 에피소드에서 발화 여부 */
    gint64      last_fired_at;  /**< 마지막 발화 시각 (dedup용) */
} CompositeRule;

/**
 * @brief 알림 엔진 전역 상태 (모듈 내부 전용)
 *
 * static 전역 구조체로 모듈 외부에서 직접 접근 불가.
 * {0}으로 제로 초기화되어, init 호출 전에는 모든 필드가 0/NULL/FALSE.
 *
 * 스레드 안전성:
 *   - history[], hist_count, hist_idx → G.mu 락 하에서만 읽기/쓰기
 *   - cpu, mem, disk (MetricWatch) → alert-engine 스레드에서만 접근
 *   - webhook_url, webhook_format 등 설정 → set_config 시 G.mu 락 하에서 변경,
 *     alert-engine 스레드에서 읽기 (락 없이 읽지만, 문자열 복사가 원자적이지 않으므로
 *     이론적 경합 가능. 실무적으로 set_config 호출 빈도가 극히 낮아 무시.)
 */
static struct {
    GThread       *thread;         /**< "alert-engine" 평가 스레드 핸들.
                                    *   NULL이면 스레드 미시작 상태. */
    gboolean       running;        /**< 스레드 루프 제어 플래그.
                                    *   FALSE로 설정하면 스레드가 루프를 탈출하고 종료. */
    gboolean       enabled;        /**< 알림 엔진 활성화 여부.
                                    *   daemon.conf [alert] enabled=true 또는
                                    *   set_config으로 런타임 변경 가능. */
    gboolean       initialized;    /**< init()이 한 번이라도 호출되었는지 여부.
                                    *   shutdown()에서 이중 정리 방지용. */

    /* ── 설정값 (daemon.conf [alert] 섹션에서 로드) ── */
    gchar          webhook_url[512];   /**< Webhook POST 대상 URL. 빈 문자열이면 전송 안 함.
                                        *   예: "https://hooks.slack.com/services/T00/B00/xxx" */
    gchar          webhook_secret[128]; /**< Webhook HMAC-SHA256 서명 키. 비어있으면 서명 안 함. */
    gchar          webhook_crit_url[512]; /**< CRIT 전용 Webhook URL. 비어있으면 webhook_url 사용. */
    gchar          webhook_format[16]; /**< Webhook 페이로드 포맷. "slack" | "telegram" | "generic".
                                        *   기본값: "generic" */
    gchar          telegram_chat_id[64]; /**< Telegram 봇 API용 chat_id.
                                          *   webhook_format="telegram"일 때만 사용. */
    gint           eval_period_sec;    /**< 조건 지속 판정 시간 (초, 기본값: 30).
                                        *   메트릭이 임계값을 이 시간 동안 연속 초과해야 알림 발생.
                                        *   WhaTap의 "지속 조건" 개념. */
    gint           dedup_window_sec;    /**< 알림 중복 제거 윈도우 (초, 기본값: 300).
                                         *   동일 메트릭:레벨 조합이 이 시간 내 재발화 억제.
                                         *   에피소드 종료 후 재진입 시에도 윈도우 내이면 무시. */

    /* ── 메트릭별 감시 상태 ── */
    MetricWatch    cpu;    /**< CPU 사용률 감시. warn/crit 임계값 + 지속 추적. */
    MetricWatch    mem;    /**< 메모리 사용률 감시. */
    MetricWatch    disk;   /**< 디스크 사용률 감시. statvfs("/")로 측정. */
    MetricWatch    data_pool; /**< 데이터 풀 사용률 감시. statvfs(image_dir)로 측정. */

    /* ── 복합 알림 규칙 (AND/OR) ── */
    CompositeRule  composite_rules[MAX_COMPOSITE_RULES]; /**< 복합 규칙 배열 */
    gint           n_composite_rules;  /**< 활성 복합 규칙 수 (0 ~ MAX_COMPOSITE_RULES) */

    /* ── 히스토리 링버퍼 ── */
    AlertRecord    history[ALERT_HISTORY_MAX];  /**< 알림 기록 고정 크기 배열 (환형 버퍼).
                                                 *   최대 100건. 초과 시 가장 오래된 것부터 덮어씀. */
    gint           hist_count;   /**< 현재 저장된 알림 기록 수 (0 ~ ALERT_HISTORY_MAX).
                                  *   MAX에 도달하면 더 이상 증가하지 않음. */
    gint           hist_idx;     /**< 다음 기록을 저장할 배열 인덱스 (0 ~ MAX-1, 순환).
                                  *   기록 후 (idx+1) % MAX로 전진. */
    GMutex         mu;           /**< 히스토리/설정 동시 접근 보호용 뮤텍스.
                                  *   get_history, get_config, set_config, _record_alert에서 사용. */
} G = {0};

/* ── 내부 헬퍼 함수 ─────────────────────────────────────────── */

/**
 * @brief 모노토닉 시계의 현재 시각을 초 단위로 반환한다.
 *
 * g_get_monotonic_time()은 마이크로초 단위이므로 G_USEC_PER_SEC(1,000,000)으로
 * 나누어 초 단위로 변환한다. 모노토닉 시계는 시스템 시간 변경(NTP 보정 등)에
 * 영향받지 않으므로 경과 시간 측정에 적합하다.
 *
 * @return gint64 — 모노토닉 시각 (초 단위). 절대값 자체는 의미 없고 차이만 유의미.
 *
 * 호출 컨텍스트: alert-engine 스레드에서 _eval_metric() 내부 호출.
 */
static gint64
_mono_now(void)
{
    return g_get_monotonic_time() / G_USEC_PER_SEC;
}

/**
 * @brief 알림 기록을 히스토리 링버퍼에 저장한다.
 *
 * G.mu 락을 잡고 G.history[G.hist_idx] 슬롯에 기록을 덮어쓴 뒤,
 * hist_idx를 순환 전진시킨다. hist_count는 MAX까지만 증가한다.
 *
 * @param metric  메트릭 이름 문자열 ("CPU", "Memory", "Disk")
 * @param level   알림 심각도 (ALERT_WARN 또는 ALERT_CRIT)
 * @param value   알림 발생 시점의 실측 퍼센트 값
 * @param msg     사람이 읽을 수 있는 알림 메시지 전문
 *
 * 호출 컨텍스트: alert-engine 스레드에서 _fire_alert() 내부 호출.
 * 스레드 안전성: 내부에서 G.mu 락을 잡으므로, get_history()와 동시 호출 안전.
 */
/* BE-A14: 알림 ID 순차 카운터 (원자적 증가) */
static volatile gint g_next_alert_id = 1;

static void
_record_alert(const gchar *metric, AlertLevel level, gdouble value, const gchar *msg)
{
    g_mutex_lock(&G.mu);
    AlertRecord *r = &G.history[G.hist_idx];
    g_strlcpy(r->metric, metric, sizeof(r->metric));
    r->level = level;
    r->value = value;
    r->fired_at = (gint64)time(NULL);
    g_strlcpy(r->message, msg, sizeof(r->message));
    r->alert_id = (gint64)g_atomic_int_add(&g_next_alert_id, 1);
    r->acknowledged = FALSE;
    r->escalated = FALSE;
    G.hist_idx = (G.hist_idx + 1) % ALERT_HISTORY_MAX;
    if (G.hist_count < ALERT_HISTORY_MAX) G.hist_count++;
    g_mutex_unlock(&G.mu);
}

void
pcv_alert_record_security_event(const gchar *event_id,
                                const gchar *severity,
                                const gchar *summary)
{
    if (!G.initialized) {
        return;
    }

    AlertLevel level = g_strcmp0(severity, "crit") == 0
        ? ALERT_CRIT
        : ALERT_WARN;
    gchar msg[256];
    g_snprintf(msg, sizeof msg, "[%s] Security event %s: %s",
               severity ? severity : "warn",
               event_id ? event_id : "",
               summary ? summary : "");
    _record_alert("Security", level, 0.0, msg);
}

/**
 * @brief Webhook URL로 JSON 페이로드를 HTTP POST 전송한다.
 *
 * webhook_url이 빈 문자열이면 아무 작업 없이 반환한다.
 * libsoup3의 SoupSession을 매 호출마다 새로 생성하여 사용한다.
 * (빈도가 낮으므로 세션 풀링은 불필요.)
 *
 * 동기적 호출(soup_session_send_and_read)이므로, Webhook 서버가 응답할 때까지
 * alert-engine 스레드가 블록된다. 메인 이벤트 루프에는 영향 없음.
 * 전송 실패(msg 생성 실패, 네트워크 오류 등)는 조용히 무시된다.
 *
 * @param payload  전송할 JSON 문자열 (NUL 종료). 소유권 이전 없음.
 *
 * 호출 컨텍스트: alert-engine 스레드에서 _fire_alert() 내부 호출.
 */
/* ── Webhook DLQ (Dead Letter Queue) ─────────────────────────── */
/* AIO-4: DLQ 스토어(g_webhook_dlq/g_dlq_mu, WEBHOOK_DLQ_MAX,
 * store/list/retry)는 src/modules/daemons/alert_dlq.{c,h} 로 추출됐다 —
 * 값매칭 제거의 실-코드 회귀 테스트를 위한 링크가능 TU 분리(alert_silence 선례).
 * HTTP 전송(_webhook_post)은 이 파일에 잔존하고 init 에서 seam 으로 등록된다. */

constexpr int WEBHOOK_MAX_RETRIES = 3;

/* ── C23 컴파일 타임 검증 ─────────────────────────────────── */
static_assert(ALERT_HISTORY_MAX >= 100, "History buffer too small");
static_assert(MAX_COMPOSITE_RULES <= 16, "Composite rules exceed limit");
static_assert(WEBHOOK_MAX_RETRIES >= 1, "Must retry at least once");

/**
 * _webhook_post — 단일 Webhook POST 시도
 *
 * @param url      대상 URL (NULL이면 G.webhook_url 사용)
 * @param payload  JSON 페이로드
 * @return TRUE 성공, FALSE 실패
 */
static gboolean
_webhook_post(const gchar *url, const gchar *payload)
{
    const gchar *target_url = url ? url : G.webhook_url;
    if (!target_url || !target_url[0]) return FALSE;

    /* URL 스킴 검증 — http:// 또는 https:// 만 허용 (SSRF 방지) */
    if (!g_str_has_prefix(target_url, "http://") &&
        !g_str_has_prefix(target_url, "https://")) {
        PCV_LOG_WARN(ALERT_LOG_DOM,
                     "Webhook URL rejected (invalid scheme): %.100s", target_url);
        return FALSE;
    }

    /* SSRF 방지 (A10/V4, Wave B Item 5-a) — 대상 host를 실주소로 resolve하여
     * 루프백/링크로컬(클라우드 메타데이터 포함) 차단. substring denylist는 인코딩
     * 우회(십진/16진/DNS 별칭)에 취약해 resolve 기반 검증으로 교체했다. */
    GError *ssrf_err = NULL;
    if (!pcv_url_target_allowed(target_url, &ssrf_err)) {
        PCV_LOG_WARN(ALERT_LOG_DOM,
                     "Webhook URL rejected (SSRF guard): %.100s — %s",
                     target_url, ssrf_err ? ssrf_err->message : "blocked");
        g_clear_error(&ssrf_err);
        return FALSE;
    }

    SoupSession *sess = soup_session_new();
    /* 타임아웃 10초 — 응답 없는 서버에 무한 블로킹 방지 */
    g_object_set(sess, "timeout", 10, NULL);
    SoupMessage *msg = soup_message_new("POST", target_url);
    if (!msg) { g_object_unref(sess); return FALSE; }
    /* SSRF(A10/V4): 리다이렉트 추종 금지 — 링크로컬/루프백 denylist 우회 차단 */
    soup_message_set_flags(msg, SOUP_MESSAGE_NO_REDIRECT);

    GBytes *body = g_bytes_new(payload, strlen(payload));
    soup_message_set_request_body_from_bytes(msg, "application/json", body);

    /* Webhook HMAC-SHA256 서명 (webhook_secret 설정 시) */
    if (G.webhook_secret[0]) {
        GHmac *hmac = g_hmac_new(G_CHECKSUM_SHA256, (const guchar *)G.webhook_secret, strlen(G.webhook_secret));
        g_hmac_update(hmac, (const guchar *)payload, strlen(payload));
        gchar *sig = g_strdup_printf("sha256=%s", g_hmac_get_string(hmac));
        SoupMessageHeaders *hdrs = soup_message_get_request_headers(msg);
        soup_message_headers_replace(hdrs, "X-PureCVisor-Signature", sig);
        g_free(sig);
        g_hmac_unref(hmac);
    }

    /* 동기 전송 */
    GBytes *resp = soup_session_send_and_read(sess, msg, NULL, NULL);
    gboolean ok = (resp != nullptr && soup_message_get_status(msg) >= 200
                   && soup_message_get_status(msg) < 300);

    if (resp) g_bytes_unref(resp);
    g_bytes_unref(body);
    g_object_unref(msg);
    g_object_unref(sess);
    return ok;
}

/**
 * _webhook_post_with_retry — 지수 백오프 재시도 + DLQ 저장
 *
 * @param url          대상 URL (NULL이면 G.webhook_url 사용)
 * @param payload      JSON 페이로드
 * @param max_retries  최대 재시도 횟수 (기본 3)
 * @return TRUE 성공, FALSE 모든 재시도 실패 (DLQ 저장됨)
 */
static gboolean
_webhook_post_with_retry(const gchar *url, const gchar *payload, gint max_retries)
{
    const gchar *target_url = url ? url : G.webhook_url;

    for (gint attempt = 0; attempt <= max_retries; attempt++) {
        if (attempt > 0) {
            guint delay_ms = 1000 * (1 << (attempt - 1)); /* 1s, 2s, 4s */
            g_usleep((guint64)delay_ms * 1000);
        }
        if (_webhook_post(target_url, payload)) return TRUE;
        PCV_LOG_WARN(ALERT_LOG_DOM, "Webhook retry %d/%d failed for %.100s",
                     attempt + 1, max_retries, target_url);
    }
    /* 모든 재시도 실패 — DLQ에 저장 (alert_dlq TU) */
    pcv_alert_dlq_store(target_url, payload);
    return FALSE;
}

/* ── 비동기 Webhook 전송 (GTask) ─────────────────────────────── */

typedef struct {
    gchar *url;
    gchar *payload;
} WebhookAsyncCtx;

static void _webhook_async_ctx_free(gpointer p) {
    WebhookAsyncCtx *ctx = p;
    g_free(ctx->url);
    g_free(ctx->payload);
    g_free(ctx);
}

static void
_webhook_async_worker(GTask *task, gpointer src, gpointer data, GCancellable *c)
{
    (void)src; (void)c; (void)task;
    WebhookAsyncCtx *ctx = data;
    _webhook_post_with_retry(ctx->url, ctx->payload, WEBHOOK_MAX_RETRIES);
}

/**
 * _webhook_post_async — 웹훅 전송을 GTask로 비��기 실행
 *
 * 알림 평가 스레드가 웹훅 응답 대기로 블로킹되는 것을 방지한다.
 */
static void
_webhook_post_async(const gchar *url, const gchar *payload)
{
    WebhookAsyncCtx *ctx = g_new0(WebhookAsyncCtx, 1);
    ctx->url = url ? g_strdup(url) : NULL;
    ctx->payload = g_strdup(payload);

    GTask *task = g_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(task, ctx, _webhook_async_ctx_free);
    g_task_run_in_thread(task, _webhook_async_worker);
    g_object_unref(task);
}

/**
 * @brief 알림을 실제로 발생시킨다 — 로그 기록 + 히스토리 저장 + Webhook 전송.
 *
 * 호스트명과 현재 시각을 포함한 사람이 읽을 수 있는 메시지를 구성하고:
 *   1. PCV_LOG_WARN으로 journalctl에 기록
 *   2. _record_alert()로 링버퍼에 저장
 *   3. webhook_format에 따라 JSON 페이로드를 구성하여 _webhook_post()로 전송
 *
 * Webhook 페이로드 포맷별 차이:
 *   - slack:    {"text":"PureCVisor Alert: [WARN] CPU 85.2% on host at time"}
 *               → Slack Incoming Webhook이 "text" 키를 메시지로 표시
 *   - telegram: {"chat_id":"123","text":"PureCVisor Alert: ..."}
 *               → Telegram Bot API /sendMessage 호환, chat_id 필수
 *   - generic:  {"severity":"WARN","metric":"CPU","value":85.2,"host":"...","timestamp":"..."}
 *               → 구조화 JSON, 커스텀 시스템/PagerDuty 연동에 적합
 *
 * @param metric  메트릭 이름 ("CPU", "Memory", "Disk")
 * @param level   알림 심각도 (ALERT_WARN 또는 ALERT_CRIT)
 * @param value   현재 메트릭 퍼센트 값 (0.0~100.0)
 *
 * 호출 컨텍스트: alert-engine 스레드에서 _eval_metric() 내부 호출.
 */

/* ── R-4: Per-VM 웹훅 라우팅 ────────────────────────────────── */

static GHashTable *g_vm_webhook_map = nullptr;  /* vm_name -> webhook_url */
static GMutex g_vm_webhook_mu;

/**
 * pcv_alert_set_vm_webhook — 특정 VM에 전용 웹훅 URL을 설정한다.
 *
 * @param vm_name     대상 VM 이름
 * @param webhook_url 전용 웹훅 URL (NULL/빈문자열이면 해당 VM 라우팅 삭제)
 */
void
pcv_alert_set_vm_webhook(const gchar *vm_name, const gchar *webhook_url)
{
    if (!vm_name) return;
    g_mutex_lock(&g_vm_webhook_mu);
    if (!g_vm_webhook_map)
        g_vm_webhook_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    if (webhook_url && *webhook_url)
        g_hash_table_insert(g_vm_webhook_map, g_strdup(vm_name), g_strdup(webhook_url));
    else
        g_hash_table_remove(g_vm_webhook_map, vm_name);
    g_mutex_unlock(&g_vm_webhook_mu);
}

/**
 * _get_vm_webhook — metric 이름으로 Per-VM 웹훅 URL을 조회한다.
 *
 * @param metric 메트릭 식별자 (예: VM 이름 또는 "CPU" 등)
 * @return const gchar* — 매칭된 웹훅 URL 또는 NULL (기본 웹훅 사용)
 */
static const gchar *
_get_vm_webhook(const gchar *metric)
{
    if (!g_vm_webhook_map || !metric) return NULL;
    g_mutex_lock(&g_vm_webhook_mu);
    const gchar *url = g_hash_table_lookup(g_vm_webhook_map, metric);
    g_mutex_unlock(&g_vm_webhook_mu);
    return url;
}

static void
_fire_alert(const gchar *metric, AlertLevel level, gdouble value)
{
    /* AF-O2: 음소거 확인 — pcv_alert_add_silence(alert.silence RPC) 로 등록된
     * 메트릭이면 알림을 발화하지 않는다. 이전에는 pcv_alert_is_silenced 가 정의만
     * 되고 이 발화 경로에서 호출되지 않아 음소거가 완전 무동작이었다(레코드·웹훅
     * 모두 발생 = 감사 테마 "통제가 보고만 성공, 실제 무동작"). */
    /* PCV_SAFETY_CONTROL: alert-silence — 음소거 등록된 메트릭의 알림 발화를 억제 (AIO-3) */
    if (pcv_alert_is_silenced(metric)) {
        PCV_LOG_INFO(ALERT_LOG_DOM, "Alert suppressed (silenced): metric=%s", metric);
        return;
    }

    const gchar *sev = (level == ALERT_CRIT) ? "CRIT" : "WARN";
    gchar hostname[64] = "unknown";
    gethostname(hostname, sizeof(hostname));

    /* 타임스탬프 문자열 생성 (로컬 시간대) */
    gchar ts[32];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);

    /* 사람이 읽을 수 있는 메시지 구성 */
    gchar msg[256];
    g_snprintf(msg, sizeof(msg), "[%s] %s %.1f%% on %s at %s",
               sev, metric, value, hostname, ts);

    PCV_LOG_WARN(ALERT_LOG_DOM, "%s", msg);
    _record_alert(metric, level, value, msg);

    /* JSON 문자열 이스케이프 — hostname 등에 포함될 수 있는 특수문자 처리 */
    GString *escaped_msg = g_string_new("");
    for (const char *p = msg; *p; p++) {
        if (*p == '"')       g_string_append(escaped_msg, "\\\"");
        else if (*p == '\\') g_string_append(escaped_msg, "\\\\");
        else if (*p == '\n') g_string_append(escaped_msg, "\\n");
        else                 g_string_append_c(escaped_msg, *p);
    }

    GString *escaped_host = g_string_new("");
    for (const char *p = hostname; *p; p++) {
        if (*p == '"')       g_string_append(escaped_host, "\\\"");
        else if (*p == '\\') g_string_append(escaped_host, "\\\\");
        else                 g_string_append_c(escaped_host, *p);
    }

    /* webhook_format에 따라 JSON 페이로드를 구성하여 전송 */
    gchar payload[1024];
    if (g_strcmp0(G.webhook_format, "slack") == 0) {
        /* Slack: {"text":"..."} — Incoming Webhook 표준 포맷 */
        g_snprintf(payload, sizeof(payload),
            "{\"text\":\"PureCVisor Alert: %s\"}", escaped_msg->str);
    } else if (g_strcmp0(G.webhook_format, "telegram") == 0) {
        /* Telegram: {"chat_id":"...","text":"..."} — Bot API sendMessage 포맷 */
        g_snprintf(payload, sizeof(payload),
            "{\"chat_id\":\"%s\",\"text\":\"PureCVisor Alert: %s\"}",
            G.telegram_chat_id, escaped_msg->str);
    } else {
        /* Generic: 구조화 JSON — severity/metric/value/host/timestamp 개별 필드 */
        g_snprintf(payload, sizeof(payload),
            "{\"severity\":\"%s\",\"metric\":\"%s\",\"value\":%.1f,"
            "\"host\":\"%s\",\"timestamp\":\"%s\"}",
            sev, metric, value, escaped_host->str, ts);
    }
    g_string_free(escaped_msg, TRUE);
    g_string_free(escaped_host, TRUE);

    /* 비동기 웹훅 전송 — 알림 평가 스레드 블로킹 방지 */
    if (G.webhook_url[0]) {
        /* R-4: Per-VM 웹훅 라우팅 — metric이 VM이름이면 해당 전용 웹훅 우선 */
        const gchar *vm_wh = _get_vm_webhook(metric);
        /* CRIT 전용 URL이 있으면 CRIT은 해당 URL로, 아니면 기본 URL */
        const gchar *url = vm_wh ? vm_wh
                         : (level == ALERT_CRIT && G.webhook_crit_url[0])
                           ? G.webhook_crit_url : NULL;
        _webhook_post_async(url, payload);
    }
}

/**
 * @brief 단일 메트릭의 임계값 평가를 수행한다.
 *
 * WhaTap 스타일 "지속 조건" 알고리즘의 핵심 함수.
 * 현재 메트릭 값(current_pct)을 warn/crit 임계값과 비교하고,
 * eval_period 동안 연속 초과하면 알림을 발생시킨다.
 *
 * 알고리즘 흐름:
 *
 *   ┌─ current_pct >= crit_thresh? ──YES──→ crit_since 기록(첫 회만)
 *   │                                        → eval_period 경과 && !fired? → 알림!
 *   │                                NO
 *   │                                ↓
 *   │                          crit_since=0, crit_fired=FALSE (리셋)
 *   │
 *   ├─ warn_thresh <= current_pct < crit_thresh? ──YES──→ warn_since 기록
 *   │                                                      → eval_period 경과? → 알림!
 *   │                                NO
 *   │                                ↓
 *   │                          warn_since=0, warn_fired=FALSE (리셋)
 *   └─────────────────────────────────
 *
 * 상호 배제: CRIT 조건일 때는 WARN 평가를 건너뛴다.
 * (current_pct >= crit_thresh이면 두 번째 if의 < crit_thresh 조건에 의해 자동 배제)
 *
 * @param w            평가 대상 MetricWatch 구조체 포인터 (CPU/메모리/디스크 중 하나)
 * @param name         메트릭 이름 문자열 ("CPU", "Memory", "Disk"). 알림 메시지에 포함.
 * @param current_pct  현재 메트릭 퍼센트 값 (0.0 ~ 100.0)
 *
 * 호출 컨텍스트: alert-engine 스레드의 _alert_thread() 루프에서 호출.
 *               단일 스레드 접근이므로 MetricWatch 필드에 대한 락 불필요.
 */
static void
_eval_metric(MetricWatch *w, const gchar *name, gdouble current_pct)
{
    gint64 now = _mono_now();

    /* ── CRIT(위험) 평가 ── */
    if (current_pct >= w->crit_thresh) {
        /* 조건 최초 진입: 시작 시각 기록 */
        if (w->crit_since == 0) w->crit_since = now;
        /* eval_period 경과 && 이번 에피소드에서 미발화 → 알림 발생 */
        if (!w->crit_fired && (now - w->crit_since) >= G.eval_period_sec) {
            /* dedup 윈도우: 최근 발화로부터 충분한 시간이 지났을 때만 실제 발화 */
            if ((now - w->last_crit_fired_at) >= G.dedup_window_sec) {
                _fire_alert(name, ALERT_CRIT, current_pct);
                w->last_crit_fired_at = now;
            }
            w->crit_fired = TRUE;
        }
    } else {
        /* 임계값 아래로 복귀 → 에피소드 종료, 상태 리셋 */
        w->crit_since = 0;
        w->crit_fired = FALSE;
    }

    /* ── WARN(경고) 평가 — CRIT이 아닐 때만 (상호 배제) ── */
    if (current_pct >= w->warn_thresh && current_pct < w->crit_thresh) {
        if (w->warn_since == 0) w->warn_since = now;
        if (!w->warn_fired && (now - w->warn_since) >= G.eval_period_sec) {
            /* dedup 윈도우: 최근 발화로부터 충분한 시간이 지났을 때만 실제 발화 */
            if ((now - w->last_warn_fired_at) >= G.dedup_window_sec) {
                _fire_alert(name, ALERT_WARN, current_pct);
                w->last_warn_fired_at = now;
            }
            w->warn_fired = TRUE;
        }
    } else {
        /* 정상 범위이거나 CRIT 범위 → WARN 에피소드 종료 */
        w->warn_since = 0;
        w->warn_fired = FALSE;
    }
}

/**
 * @brief 메트릭 이름 문자열을 실측값으로 매핑한다.
 *
 * @param name  "CPU", "Memory", "Disk" 중 하나
 * @param cpu   현재 CPU 사용률 (%)
 * @param mem   현재 메모리 사용률 (%)
 * @param disk  현재 디스크 사용률 (%)
 * @return gdouble — 해당 메트릭의 현재 값. 미인식 이름이면 0.0.
 */
static gdouble
_get_metric_value(const gchar *name, gdouble cpu, gdouble mem, gdouble disk)
{
    if (g_strcmp0(name, "CPU") == 0)    return cpu;
    if (g_strcmp0(name, "Memory") == 0) return mem;
    if (g_strcmp0(name, "Disk") == 0)   return disk;
    return 0.0;
}

/**
 * @brief 복합 알림 규칙들을 평가한다 (AND/OR 조합).
 *
 * 개별 메트릭 평가(_eval_metric) 이후 호출되어, 두 메트릭의 조합 조건을
 * eval_period 지속 판정과 dedup_window 중복 제거를 적용하여 평가한다.
 *
 * @param cpu_pct   현재 CPU 사용률 (%)
 * @param mem_pct   현재 메모리 사용률 (%)
 * @param disk_pct  현재 디스크 사용률 (%)
 */
static void
_eval_composite_rules(gdouble cpu_pct, gdouble mem_pct, gdouble disk_pct)
{
    gint64 now = _mono_now();

    for (gint i = 0; i < G.n_composite_rules; i++) {
        CompositeRule *r = &G.composite_rules[i];
        if (!r->active) continue;

        gdouble val_a = _get_metric_value(r->metric_a, cpu_pct, mem_pct, disk_pct);
        gdouble val_b = _get_metric_value(r->metric_b, cpu_pct, mem_pct, disk_pct);

        gboolean cond_a = (val_a >= r->thresh_a);
        gboolean cond_b = (val_b >= r->thresh_b);
        gboolean triggered = (r->op == COMPOSITE_OP_AND)
                              ? (cond_a && cond_b) : (cond_a || cond_b);

        if (triggered) {
            if (r->since == 0) r->since = now;
            if (!r->fired
                && (now - r->since) >= G.eval_period_sec
                && (now - r->last_fired_at) >= G.dedup_window_sec) {
                const gchar *op_str = (r->op == COMPOSITE_OP_AND) ? "AND" : "OR";
                gchar *desc = g_strdup_printf("Composite: %s>=%.0f %s %s>=%.0f",
                    r->metric_a, r->thresh_a, op_str,
                    r->metric_b, r->thresh_b);
                gdouble report_val = (val_a > val_b) ? val_a : val_b;
                _fire_alert(desc, r->level, report_val);
                g_free(desc);
                r->fired = TRUE;
                r->last_fired_at = now;
            }
        } else {
            r->since = 0;
            r->fired = FALSE;
        }
    }
}

/**
 * @brief 루트(/) 파일시스템의 디스크 사용률을 백분율로 반환한다.
 *
 * statvfs(2) 시스템 콜로 "/" 마운트포인트의 블록 통계를 읽어
 * (전체 - 여유) / 전체 * 100 으로 사용률을 계산한다.
 *
 * @return gdouble — 디스크 사용률 (%, 0.0~100.0). statvfs 실패 시 0.0 반환.
 *
 * 호출 컨텍스트: alert-engine 스레드에서 _alert_thread() 루프 내 호출.
 *
 * 참고: CPU/메모리는 ebpf_telemetry에서 가져오지만, 디스크 사용률은
 *       eBPF로 수집하지 않으므로 여기서 직접 statvfs로 측정한다.
 */
static gdouble
_get_disk_percent_path(const gchar *path)
{
    struct statvfs vfs;
    if (statvfs(path, &vfs) != 0) return 0.0;
    guint64 total = (guint64)vfs.f_blocks * vfs.f_frsize;
    guint64 free_b = (guint64)vfs.f_bfree * vfs.f_frsize;
    if (total == 0) return 0.0;
    return 100.0 * (1.0 - (gdouble)free_b / (gdouble)total);
}

static gdouble
_get_disk_percent(void)
{
    return _get_disk_percent_path("/");
}

/**
 * _get_data_pool_disk_percent — 데이터 풀 디스크 사용률 조회
 *
 * daemon.conf [storage] image_dir 또는 기본 /pcvpool을 모니터링.
 * 마운트 포인트가 존재하지 않으면 0.0 반환 (모니터링 건너뜀).
 */
static gdouble
_get_data_pool_disk_percent(void)
{
    const gchar *pool_path = pcv_config_get_string("storage", "image_dir", "/pcvpool");
    if (!pool_path || !*pool_path) pool_path = "/pcvpool";
    /* 경로 존재 여부 확인 */
    struct statvfs vfs;
    if (statvfs(pool_path, &vfs) != 0) return 0.0;
    return _get_disk_percent_path(pool_path);
}

/* ── SLA 추적 (VM 가동률 기록) ──────────────────────────────── */

#define SLA_CHECK_INTERVAL  60  /* 60초마다 VM 상태 체크 */
static GHashTable *g_vm_uptime   = nullptr;   /* vm_name -> total_up_seconds (gint64*) */
static GHashTable *g_vm_downtime = nullptr;   /* vm_name -> total_down_seconds (gint64*) */
static GMutex g_sla_mu;

/**
 * _sla_check_vms — 전체 VM 상태를 조회하여 가동/비가동 시간을 누적
 *
 * virsh list --all로 전체 VM을 열거하고, 각 VM의 상태(running 여부)에 따라
 * uptime 또는 downtime 카운터를 SLA_CHECK_INTERVAL만큼 증가시킨다.
 *
 * 호출 컨텍스트: alert-engine 스레드에서 60초마다 호출.
 */
static void
_sla_check_vms(void)
{
    g_mutex_lock(&g_sla_mu);
    if (!g_vm_uptime) {
        g_vm_uptime   = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
        g_vm_downtime = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    }
    g_mutex_unlock(&g_sla_mu);

    const gchar *argv[] = {"virsh", "list", "--all", "--name", NULL};
    gchar *out = nullptr;
    if (!pcv_spawn_sync(argv, &out, NULL, NULL) || !out) { g_free(out); return; }

    gchar **vms = g_strsplit(g_strstrip(out), "\n", -1);
    for (gchar **v = vms; *v; v++) {
        if (!**v) continue;
        const gchar *vm = *v;
        const gchar *state_argv[] = {"virsh", "domstate", vm, NULL};
        gchar *state = nullptr;
        gboolean state_ok = pcv_spawn_sync(state_argv, &state, nullptr, nullptr);
        if (!state_ok) {
            PCV_LOG_WARN("ALERT", "SLA: virsh domstate failed for '%s' — skipping this interval", vm);
            g_free(state);
            continue;
        }

        g_mutex_lock(&g_sla_mu);
        gint64 *up   = g_hash_table_lookup(g_vm_uptime, vm);
        gint64 *down = g_hash_table_lookup(g_vm_downtime, vm);
        if (!up)   { up   = g_new0(gint64, 1); g_hash_table_insert(g_vm_uptime,   g_strdup(vm), up); }
        if (!down) { down = g_new0(gint64, 1); g_hash_table_insert(g_vm_downtime, g_strdup(vm), down); }

        if (state && strstr(state, "running"))
            *up += SLA_CHECK_INTERVAL;
        else
            *down += SLA_CHECK_INTERVAL;
        g_mutex_unlock(&g_sla_mu);
        g_free(state);
    }
    g_strfreev(vms);
    g_free(out);
}

/**
 * pcv_alert_get_sla — VM의 SLA(가동률) 정보를 조회한다.
 *
 * @param vm_name  조회할 VM 이름
 * @return JsonObject* {uptime_percent, uptime_seconds, downtime_seconds}
 *         호출자가 json_object_unref()로 해제.
 */
JsonObject *
pcv_alert_get_sla(const gchar *vm_name)
{
    JsonObject *obj = json_object_new();
    g_mutex_lock(&g_sla_mu);
    if (g_vm_uptime && vm_name) {
        gint64 *up   = g_hash_table_lookup(g_vm_uptime, vm_name);
        gint64 *down = g_hash_table_lookup(g_vm_downtime, vm_name);
        gint64 u = up ? *up : 0, d = down ? *down : 0;
        gint64 total = u + d;
        gdouble pct = total > 0 ? (100.0 * (gdouble)u / (gdouble)total) : 100.0;
        json_object_set_double_member(obj, "uptime_percent", pct);
        json_object_set_int_member(obj, "uptime_seconds", u);
        json_object_set_int_member(obj, "downtime_seconds", d);
    }
    g_mutex_unlock(&g_sla_mu);
    return obj;
}

/* ── 알림 평가 스레드 ───────────────────────────────────────── */

/**
 * @brief 알림 엔진의 메인 평가 스레드 함수.
 *
 * G.running이 TRUE인 동안 ALERT_CHECK_SEC(5초) 간격으로 반복:
 *   1. pcv_ebpf_telemetry_get_host()에서 CPU/메모리 퍼센트 읽기
 *   2. _get_disk_percent()로 디스크 퍼센트 읽기
 *   3. 각 메트릭에 대해 _eval_metric() 호출 (임계값 평가 + 알림 발생)
 *   4. g_usleep()으로 다음 주기까지 대기
 *
 * 스레드 종료: shutdown()에서 G.running=FALSE 설정 → 루프 탈출 → 반환.
 *
 * @param data  사용하지 않음 (GThread 콜백 시그니처 충족용, NULL 전달됨)
 * @return gpointer — 항상 NULL 반환 (GThread 반환값, 사용하지 않음)
 *
 * 호출 컨텍스트: GThread에 의해 별도 스레드에서 실행됨.
 *               이 함수 내에서만 MetricWatch 필드를 읽고 쓴다 (단일 스레드 보장).
 */
static gpointer
_alert_thread(gpointer data)
{
    (void)data;
    PCV_LOG_INFO(ALERT_LOG_DOM, "Alert engine started (eval=%ds, webhook=%s, format=%s)",
                 G.eval_period_sec,
                 G.webhook_url[0] ? G.webhook_url : "(none)",
                 G.webhook_format);

    while (G.running) {
        /*
         * eBPF 텔레메트리에서 호스트 메트릭 조회
         *
         * pcv_ebpf_telemetry_get_host()는 새 JsonObject를 생성하여 반환하므로
         * 사용 후 반드시 json_object_unref()로 해제해야 한다.
         *
         * cpu_percent: ebpf_telemetry.c에서 /proc/stat delta로 계산한 전체 CPU 사용률
         * mem_percent: 100 * (1 - MemAvailable/MemTotal)로 계산한 메모리 사용률
         * disk: statvfs("/")로 직접 측정 (eBPF에서는 디스크 사용률을 제공하지 않음)
         */
        /* 유지보수 모드 시 알림 평가 건너뜀 */
#if PCV_CLUSTER_ENABLED
        if (pcv_cluster_is_maintenance()) {
            g_usleep((guint64)ALERT_CHECK_SEC * G_USEC_PER_SEC);
            continue;
        }
#endif

        JsonObject *host = pcv_ebpf_telemetry_get_host();
        if (host) {
            gdouble cpu = json_object_get_double_member(host, "cpu_percent");
            gdouble mem = json_object_get_double_member(host, "mem_percent");
            gdouble disk = _get_disk_percent();

            _eval_metric(&G.cpu,  "CPU",    cpu);
            _eval_metric(&G.mem,  "Memory", mem);
            _eval_metric(&G.disk, "Disk",   disk);

            /* 데이터 풀 디스크 모니터링 (/pcvpool 등) */
            gdouble data_pool_pct = _get_data_pool_disk_percent();
            if (data_pool_pct > 0.0)
                _eval_metric(&G.data_pool, "DataPool", data_pool_pct);

            /* 복합 규칙 평가 (AND/OR 조합) */
            _eval_composite_rules(cpu, mem, disk);

            json_object_unref(host);
        }

        /* ── BE-A15: 에스컬레이션 — 미확인 CRIT 알림 10분 후 재전송 ── */
#define ESCALATION_INTERVAL_SEC  600  /* 10분 */
        {
            gint64 esc_now = (gint64)time(NULL);
            g_mutex_lock(&G.mu);
            for (gint i = 0; i < G.hist_count && i < ALERT_HISTORY_MAX; i++) {
                AlertRecord *r = &G.history[i];
                if (r->level == ALERT_CRIT && !r->acknowledged &&
                    r->fired_at > 0 &&
                    (esc_now - r->fired_at) >= ESCALATION_INTERVAL_SEC &&
                    !r->escalated) {
                    r->escalated = TRUE;
                    gchar esc_msg[512];
                    g_snprintf(esc_msg, sizeof(esc_msg),
                               "[ESCALATION] Unacknowledged CRIT (id=%" G_GINT64_FORMAT "): %s",
                               r->alert_id, r->message);
                    g_mutex_unlock(&G.mu);
                    /* A6-3: 정상 경로(674~689)와 동일하게 webhook_format 별 JSON
                     * 으로 래핑한다. 이전에는 평문 esc_msg 를 그대로 전송 →
                     * _webhook_post 가 강제하는 Content-Type application/json 과
                     * 불일치 → Slack/Telegram/generic 수신 서버가 파싱 실패로
                     * 거부(에스컬레이션 무효화). 이스케이프는 정상 경로와 동일. */
                    GString *esc_esc = g_string_new("");
                    for (const char *p = esc_msg; *p; p++) {
                        if (*p == '"')       g_string_append(esc_esc, "\\\"");
                        else if (*p == '\\') g_string_append(esc_esc, "\\\\");
                        else if (*p == '\n') g_string_append(esc_esc, "\\n");
                        else                 g_string_append_c(esc_esc, *p);
                    }
                    gchar esc_payload[768];
                    if (g_strcmp0(G.webhook_format, "slack") == 0) {
                        g_snprintf(esc_payload, sizeof(esc_payload),
                            "{\"text\":\"%s\"}", esc_esc->str);
                    } else if (g_strcmp0(G.webhook_format, "telegram") == 0) {
                        g_snprintf(esc_payload, sizeof(esc_payload),
                            "{\"chat_id\":\"%s\",\"text\":\"%s\"}",
                            G.telegram_chat_id, esc_esc->str);
                    } else {
                        g_snprintf(esc_payload, sizeof(esc_payload),
                            "{\"severity\":\"CRIT\",\"event\":\"escalation\","
                            "\"text\":\"%s\"}", esc_esc->str);
                    }
                    g_string_free(esc_esc, TRUE);
                    /* 에스컬레이션은 항상 CRIT — CRIT 전용 URL 우선(정상 경로 동일) */
                    const gchar *esc_url = G.webhook_crit_url[0] ? G.webhook_crit_url : NULL;
                    _webhook_post_async(esc_url, esc_payload);
                    PCV_LOG_WARN(ALERT_LOG_DOM, "%s", esc_msg);
                    g_mutex_lock(&G.mu);
                }
            }
            g_mutex_unlock(&G.mu);
        }

        /* ── SLA 추적: 60초마다 VM 상태 체크 ── */
        {
            static gint sla_counter = 0;
            if (++sla_counter >= (SLA_CHECK_INTERVAL / ALERT_CHECK_SEC)) {
                sla_counter = 0;
                _sla_check_vms();
            }
        }

        /* 다음 평가 주기까지 대기 (5초) */
        g_usleep(ALERT_CHECK_SEC * G_USEC_PER_SEC);
    }

    PCV_LOG_INFO(ALERT_LOG_DOM, "Alert engine stopped");
    return NULL;
}

/* ── 공개 API ───────────────────────────────────────────────── */

/**
 * @brief 알림 엔진을 초기화하고, 설정에 따라 평가 스레드를 시작한다.
 *
 * 동작 순서:
 *   1. GMutex 초기화
 *   2. daemon.conf [alert] 섹션에서 모든 설정값 로드
 *   3. enabled=false이면 로그 출력 후 조기 반환 (스레드 미시작)
 *   4. enabled=true이면 MetricWatch 임계값 설정 + "alert-engine" 스레드 시작
 *
 * daemon.conf [alert] 섹션 설정 키와 기본값:
 *   enabled          = false   → 알림 엔진 활성화 여부
 *   cpu_warn         = 80      → CPU 경고 임계값 (%)
 *   cpu_crit         = 95      → CPU 위험 임계값 (%)
 *   mem_warn         = 85      → 메모리 경고 임계값 (%)
 *   mem_crit         = 95      → 메모리 위험 임계값 (%)
 *   disk_warn        = 80      → 디스크 경고 임계값 (%)
 *   disk_crit        = 90      → 디스크 위험 임계값 (%)
 *   eval_period      = 30      → 지속 판정 시간 (초)
 *   webhook_url      = ""      → Webhook POST URL
 *   webhook_format   = generic → 페이로드 포맷
 *   telegram_chat_id = ""      → Telegram chat_id
 *
 * 호출 컨텍스트: 메인 스레드에서 데몬 시작 시 1회 호출.
 * 스레드 안전성: 메인 스레드 단독 호출 전제.
 */
void
pcv_alert_engine_init(void)
{
    g_mutex_init(&G.mu);

    /* AIO-4: DLQ 재시도의 HTTP 전송 seam 등록 — enabled 여부와 무관하게 항상
     * 실 _webhook_post 를 배선한다(비활성 시 DLQ 는 비어 있어 no-op). */
    pcv_alert_dlq_set_post_fn(_webhook_post);

    /* daemon.conf [alert] 섹션에서 활성화 여부 읽기 */
    const gchar *enabled_str = pcv_config_get_string("alert", "enabled", "false");
    G.enabled = (g_strcmp0(enabled_str, "true") == 0 || g_strcmp0(enabled_str, "1") == 0);
    if (!G.enabled) {
        PCV_LOG_INFO(ALERT_LOG_DOM, "Alert engine disabled (set [alert] enabled=true to activate)");
        G.initialized = TRUE;
        return;
    }

    /* 임계값 로드 — 정수를 gdouble로 변환 (퍼센트 비교용) */
    G.cpu.warn_thresh  = (gdouble)pcv_config_get_int("alert", "cpu_warn",  80);
    G.cpu.crit_thresh  = (gdouble)pcv_config_get_int("alert", "cpu_crit",  95);
    G.mem.warn_thresh  = (gdouble)pcv_config_get_int("alert", "mem_warn",  85);
    G.mem.crit_thresh  = (gdouble)pcv_config_get_int("alert", "mem_crit",  95);
    G.disk.warn_thresh = (gdouble)pcv_config_get_int("alert", "disk_warn", 80);
    G.disk.crit_thresh = (gdouble)pcv_config_get_int("alert", "disk_crit", 90);
    /* 데이터 풀 임계값 — disk와 동일 기본값, 별도 설정 가능 */
    G.data_pool.warn_thresh = (gdouble)pcv_config_get_int("alert", "data_pool_warn", 80);
    G.data_pool.crit_thresh = (gdouble)pcv_config_get_int("alert", "data_pool_crit", 90);
    G.eval_period_sec  = pcv_config_get_int("alert", "eval_period", 30);
    G.dedup_window_sec = pcv_config_get_int("alert", "dedup_window", ALERT_DEDUP_WINDOW_SEC);

    /* Webhook 설정 로드 */
    const gchar *url = pcv_config_get_string("alert", "webhook_url", "");
    g_strlcpy(G.webhook_url, url, sizeof(G.webhook_url));

    const gchar *fmt = pcv_config_get_string("alert", "webhook_format", "generic");
    g_strlcpy(G.webhook_format, fmt, sizeof(G.webhook_format));

    const gchar *chat_id = pcv_config_get_string("alert", "telegram_chat_id", "");
    g_strlcpy(G.telegram_chat_id, chat_id, sizeof(G.telegram_chat_id));

    /* A6-4: HMAC 서명키(webhook_secret) + CRIT 전용 URL(webhook_crit_url) 로드.
     * 이전에는 init 이 이 둘을 안 읽어 G.webhook_secret[0]=='\0' → _webhook_post
     * 의 HMAC 서명 분기(481행)가 항상 거짓 → 서명 미적용. secret 은
     * s3_secret_key/chap_password 와 동일하게 get_secret(env 우선 + ENC: 복호화)
     * 으로 로드한다. (런타임 set_config 는 반영하나 daemon.conf 재기록이 없어
     * 재시작 시 유실됐다 — 부팅 로드가 정본 경로.) */
    gchar *secret = pcv_config_get_secret("alert", "webhook_secret", "");
    g_strlcpy(G.webhook_secret, secret, sizeof(G.webhook_secret));
    g_free(secret);

    const gchar *crit_url = pcv_config_get_string("alert", "webhook_crit_url", "");
    g_strlcpy(G.webhook_crit_url, crit_url, sizeof(G.webhook_crit_url));

    /* 평가 스레드 시작 */
    G.running = TRUE;
    G.initialized = TRUE;
    G.thread = g_thread_new("alert-engine", _alert_thread, NULL);
}

/**
 * @brief 알림 엔진 스레드를 정지시키고 리소스를 정리한다.
 *
 * 동작 순서:
 *   1. initialized 미설정이면 조기 반환 (init 미호출 또는 이미 shutdown됨)
 *   2. G.running=FALSE 설정 → 스레드 루프 탈출 유도
 *   3. g_thread_join()으로 스레드 종료 대기 (최대 ALERT_CHECK_SEC초 후 종료)
 *   4. GMutex 해제
 *   5. initialized=FALSE로 설정 (이중 shutdown 방지)
 *
 * 호출 컨텍스트: 메인 스레드에서 데몬 종료(drain) 시 1회 호출.
 * 스레드 안전성: 메인 스레드 단독 호출 전제.
 */
void
pcv_alert_engine_shutdown(void)
{
    if (!G.initialized) return;
    G.running = FALSE;
    if (G.thread) {
        g_thread_join(G.thread);
        G.thread = nullptr;
    }
    g_mutex_clear(&G.mu);
    G.initialized = FALSE;
}

/**
 * @brief 최근 알림 히스토리를 JsonArray로 반환한다.
 *
 * 링버퍼에서 오래된 순서 → 최신 순서로 알림 기록을 읽어 JsonArray를 구성.
 *
 * 링버퍼 읽기 알고리즘:
 *   - hist_count < MAX → 배열 인덱스 0부터 count개를 순서대로 읽음
 *   - hist_count == MAX → hist_idx(가장 오래된 것이 있는 위치)부터
 *                          MAX개를 순환하며 읽음
 *   이렇게 하면 항상 시간 순서(오래된 것 → 최신)로 정렬된 결과를 얻는다.
 *
 * 반환되는 각 JsonObject의 키:
 *   - "metric":    문자열, "CPU" | "Memory" | "Disk"
 *   - "severity":  문자열, "warn" | "crit"
 *   - "value":     실수, 알림 발생 시점의 퍼센트 값
 *   - "timestamp": 정수, Unix epoch 초
 *   - "message":   문자열, 사람이 읽을 수 있는 알림 메시지 전문
 *
 * @return JsonArray* — 호출자(caller)가 소유권을 가짐. 사용 후 json_array_unref() 필요.
 *                      알림이 없으면 빈 배열 반환.
 *
 * 호출 컨텍스트: REST 핸들러, RPC 디스패처 등 임의 스레드.
 * 스레드 안전성: 내부 G.mu 락으로 보호됨. 안전.
 */
/* ══════════════════════════════════════════════════════════════
 * BE-A14: Alert ACK (확인) API
 * ══════════════════════════════════════════════════════════════ */

/**
 * pcv_alert_acknowledge:
 * @alert_id: 확인할 알림 ID
 *
 * 알림을 확인(ACK) 처리합니다. ACK된 알림은 에스컬레이션 대상에서 제외됩니다.
 *
 * Returns: 해당 ID를 찾아 ACK 처리했으면 TRUE, 미발견 시 FALSE
 */
gboolean
pcv_alert_acknowledge(gint64 alert_id)
{
    g_mutex_lock(&G.mu);
    for (gint i = 0; i < G.hist_count && i < ALERT_HISTORY_MAX; i++) {
        if (G.history[i].alert_id == alert_id) {
            G.history[i].acknowledged = TRUE;
            g_mutex_unlock(&G.mu);
            PCV_LOG_INFO(ALERT_LOG_DOM,
                         "Alert %" G_GINT64_FORMAT " acknowledged", alert_id);
            return TRUE;
        }
    }
    g_mutex_unlock(&G.mu);
    return FALSE;
}

JsonArray *
pcv_alert_engine_get_history(void)
{
    JsonArray *arr = json_array_new();
    g_mutex_lock(&G.mu);

    /* 링버퍼에서 오래된 순서부터 읽기 위한 시작 인덱스 계산 */
    gint start = (G.hist_count < ALERT_HISTORY_MAX) ? 0 : G.hist_idx;
    for (gint i = 0; i < G.hist_count; i++) {
        gint idx = (start + i) % ALERT_HISTORY_MAX;
        AlertRecord *r = &G.history[idx];
        JsonObject *obj = json_object_new();
        json_object_set_string_member(obj, "metric",  r->metric);
        json_object_set_string_member(obj, "severity", r->level == ALERT_CRIT ? "crit" : "warn");
        json_object_set_double_member(obj, "value",    r->value);
        json_object_set_int_member   (obj, "timestamp",r->fired_at);
        json_object_set_string_member(obj, "message",  r->message);
        json_object_set_int_member   (obj, "alert_id", r->alert_id);           /* BE-A14 */
        json_object_set_boolean_member(obj, "acknowledged", r->acknowledged);  /* BE-A14 */
        json_object_set_boolean_member(obj, "escalated", r->escalated);        /* BE-A15 */
        json_array_add_object_element(arr, obj);  /* obj 소유권이 arr로 이전됨 */
    }
    g_mutex_unlock(&G.mu);
    return arr;
}

/**
 * @brief 현재 알림 엔진 설정을 JsonObject로 반환한다.
 *
 * 반환되는 JsonObject의 키:
 *   - "enabled":         gboolean, 엔진 활성화 여부
 *   - "cpu_warn":        gint64,   CPU 경고 임계값 (%)
 *   - "cpu_crit":        gint64,   CPU 위험 임계값 (%)
 *   - "mem_warn":        gint64,   메모리 경고 임계값 (%)
 *   - "mem_crit":        gint64,   메모리 위험 임계값 (%)
 *   - "disk_warn":       gint64,   디스크 경고 임계값 (%)
 *   - "disk_crit":       gint64,   디스크 위험 임계값 (%)
 *   - "eval_period":     gint64,   지속 판정 시간 (초)
 *   - "webhook_url":     문자열,   Webhook URL
 *   - "webhook_format":  문자열,   페이로드 포맷
 *   - "telegram_chat_id":문자열,   Telegram chat_id
 *   - "alert_count":     gint64,   현재 히스토리에 저장된 알림 수
 *
 * @return JsonObject* — 호출자(caller)가 소유권을 가짐. 사용 후 json_object_unref() 필요.
 *
 * 호출 컨텍스트: REST 핸들러, RPC 디스패처 등 임의 스레드.
 * 스레드 안전성: 내부 G.mu 락으로 보호됨. 안전.
 */
JsonObject *
pcv_alert_engine_get_config(void)
{
    JsonObject *obj = json_object_new();
    g_mutex_lock(&G.mu);
    json_object_set_boolean_member(obj, "enabled",        G.enabled);
    json_object_set_int_member   (obj, "cpu_warn",        (gint64)G.cpu.warn_thresh);
    json_object_set_int_member   (obj, "cpu_crit",        (gint64)G.cpu.crit_thresh);
    json_object_set_int_member   (obj, "mem_warn",        (gint64)G.mem.warn_thresh);
    json_object_set_int_member   (obj, "mem_crit",        (gint64)G.mem.crit_thresh);
    json_object_set_int_member   (obj, "disk_warn",       (gint64)G.disk.warn_thresh);
    json_object_set_int_member   (obj, "disk_crit",       (gint64)G.disk.crit_thresh);
    json_object_set_int_member   (obj, "eval_period",     G.eval_period_sec);
    json_object_set_int_member   (obj, "dedup_window",    G.dedup_window_sec);
    json_object_set_string_member(obj, "webhook_url",     G.webhook_url);
    json_object_set_string_member(obj, "webhook_format",  G.webhook_format);
    json_object_set_string_member(obj, "telegram_chat_id",G.telegram_chat_id);
    json_object_set_int_member   (obj, "alert_count",     G.hist_count);

    /* 복합 규칙 배열 직렬화 */
    JsonArray *cr_arr = json_array_new();
    for (gint i = 0; i < G.n_composite_rules; i++) {
        const CompositeRule *r = &G.composite_rules[i];
        JsonObject *cr = json_object_new();
        json_object_set_boolean_member(cr, "active",   r->active);
        json_object_set_string_member (cr, "metric_a", r->metric_a);
        json_object_set_double_member (cr, "thresh_a", r->thresh_a);
        json_object_set_string_member (cr, "op",
                                       r->op == COMPOSITE_OP_AND ? "AND" : "OR");
        json_object_set_string_member (cr, "metric_b", r->metric_b);
        json_object_set_double_member (cr, "thresh_b", r->thresh_b);
        json_object_set_string_member (cr, "level",
                                       r->level == ALERT_CRIT ? "CRIT" : "WARN");
        json_array_add_object_element(cr_arr, cr);
    }
    json_object_set_array_member(obj, "composite_rules", cr_arr);

    g_mutex_unlock(&G.mu);
    return obj;
}

/**
 * @brief 런타임에 알림 엔진 설정을 부분 업데이트한다.
 *
 * 전달된 JsonObject에 포함된 키만 선택적으로 변경한다 (부분 업데이트 패턴).
 * 예: {"cpu_warn": 90} → cpu_warn만 90으로 변경, 나머지는 기존 값 유지.
 *
 * 설정 변경 후 동작:
 *   1. 모든 MetricWatch의 since/fired 상태를 초기화 (에피소드 리셋)
 *      → 새 임계값 기준으로 eval_period를 처음부터 다시 측정
 *   2. enabled=true로 변경되었는데 스레드가 미실행 상태이면 자동 시작
 *      → 이미 실행 중이면 스레드를 재시작하지 않고 설정만 변경
 *
 * @param cfg  JsonObject* — 변경할 키-값 쌍. NULL이면 FALSE 반환.
 *             호출자가 소유권을 유지함 (이 함수에서 unref 하지 않음).
 *             지원 키: enabled, cpu_warn, cpu_crit, mem_warn, mem_crit,
 *                      disk_warn, disk_crit, eval_period, webhook_url,
 *                      webhook_format, telegram_chat_id
 * @return gboolean — 성공 시 TRUE, cfg==NULL이면 FALSE.
 *
 * 호출 컨텍스트: REST 핸들러, RPC 디스패처 등 임의 스레드.
 * 스레드 안전성: 내부 G.mu 락으로 보호됨. 안전.
 */
gboolean
pcv_alert_engine_set_config(JsonObject *cfg)
{
    if (!cfg) return FALSE;

    g_mutex_lock(&G.mu);

    /* 전달된 키만 선택적으로 업데이트 (부분 업데이트 패턴) */
    if (json_object_has_member(cfg, "enabled"))
        G.enabled = json_object_get_boolean_member(cfg, "enabled");
    if (json_object_has_member(cfg, "cpu_warn"))
        G.cpu.warn_thresh = (gdouble)json_object_get_int_member(cfg, "cpu_warn");
    if (json_object_has_member(cfg, "cpu_crit"))
        G.cpu.crit_thresh = (gdouble)json_object_get_int_member(cfg, "cpu_crit");
    if (json_object_has_member(cfg, "mem_warn"))
        G.mem.warn_thresh = (gdouble)json_object_get_int_member(cfg, "mem_warn");
    if (json_object_has_member(cfg, "mem_crit"))
        G.mem.crit_thresh = (gdouble)json_object_get_int_member(cfg, "mem_crit");
    if (json_object_has_member(cfg, "disk_warn"))
        G.disk.warn_thresh = (gdouble)json_object_get_int_member(cfg, "disk_warn");
    if (json_object_has_member(cfg, "disk_crit"))
        G.disk.crit_thresh = (gdouble)json_object_get_int_member(cfg, "disk_crit");
    if (json_object_has_member(cfg, "eval_period"))
        G.eval_period_sec = (gint)json_object_get_int_member(cfg, "eval_period");
    if (json_object_has_member(cfg, "dedup_window"))
        G.dedup_window_sec = (gint)json_object_get_int_member(cfg, "dedup_window");
    if (json_object_has_member(cfg, "webhook_url"))
        g_strlcpy(G.webhook_url,
                   json_object_get_string_member(cfg, "webhook_url"),
                   sizeof(G.webhook_url));
    if (json_object_has_member(cfg, "webhook_format")) {
        const gchar *fmt = json_object_get_string_member(cfg, "webhook_format");
        if (g_strcmp0(fmt, "slack") == 0 || g_strcmp0(fmt, "telegram") == 0 ||
            g_strcmp0(fmt, "generic") == 0) {
            g_strlcpy(G.webhook_format, fmt, sizeof(G.webhook_format));
        } else {
            PCV_LOG_WARN("alert_engine",
                         "Invalid webhook_format '%s' — keeping current '%s'",
                         fmt, G.webhook_format);
        }
    }
    if (json_object_has_member(cfg, "telegram_chat_id"))
        g_strlcpy(G.telegram_chat_id,
                   json_object_get_string_member(cfg, "telegram_chat_id"),
                   sizeof(G.telegram_chat_id));
    if (json_object_has_member(cfg, "webhook_secret"))
        g_strlcpy(G.webhook_secret,
                   json_object_get_string_member(cfg, "webhook_secret"),
                   sizeof(G.webhook_secret));
    if (json_object_has_member(cfg, "webhook_crit_url"))
        g_strlcpy(G.webhook_crit_url,
                   json_object_get_string_member(cfg, "webhook_crit_url"),
                   sizeof(G.webhook_crit_url));

    /* 복합 규칙 배열 파싱 */
    if (json_object_has_member(cfg, "composite_rules")) {
        JsonArray *arr = json_object_get_array_member(cfg, "composite_rules");
        guint len = json_array_get_length(arr);
        if (len > MAX_COMPOSITE_RULES) len = MAX_COMPOSITE_RULES;
        G.n_composite_rules = (gint)len;
        memset(G.composite_rules, 0, sizeof(G.composite_rules));
        for (guint i = 0; i < len; i++) {
            JsonObject *elem = json_array_get_object_element(arr, i);
            CompositeRule *r = &G.composite_rules[i];
            r->active = TRUE;
            if (json_object_has_member(elem, "active"))
                r->active = json_object_get_boolean_member(elem, "active");
            if (json_object_has_member(elem, "metric_a"))
                g_strlcpy(r->metric_a,
                           json_object_get_string_member(elem, "metric_a"),
                           sizeof(r->metric_a));
            if (json_object_has_member(elem, "thresh_a"))
                r->thresh_a = json_object_get_double_member(elem, "thresh_a");
            if (json_object_has_member(elem, "op")) {
                const gchar *op = json_object_get_string_member(elem, "op");
                r->op = (g_strcmp0(op, "OR") == 0)
                         ? COMPOSITE_OP_OR : COMPOSITE_OP_AND;
            }
            if (json_object_has_member(elem, "metric_b"))
                g_strlcpy(r->metric_b,
                           json_object_get_string_member(elem, "metric_b"),
                           sizeof(r->metric_b));
            if (json_object_has_member(elem, "thresh_b"))
                r->thresh_b = json_object_get_double_member(elem, "thresh_b");
            if (json_object_has_member(elem, "level")) {
                const gchar *lv = json_object_get_string_member(elem, "level");
                r->level = (g_strcmp0(lv, "CRIT") == 0)
                            ? ALERT_CRIT : ALERT_WARN;
            }
            r->since = 0;
            r->fired = FALSE;
            r->last_fired_at = 0;
        }
        PCV_LOG_INFO(ALERT_LOG_DOM, "Composite rules updated: %d rules", G.n_composite_rules);
    }

    /*
     * 설정 변경 시 모든 메트릭의 발화 상태를 초기화한다.
     * 이유: 임계값이 바뀌면 기존 since 시각은 무의미하므로, 새 임계값 기준으로
     * eval_period를 처음부터 다시 측정해야 한다.
     * 예: cpu_warn을 80→90으로 올리면, 기존에 80% 초과로 축적된 시간을 리셋.
     */
    G.cpu.warn_since = G.cpu.crit_since = 0;
    G.cpu.warn_fired = G.cpu.crit_fired = FALSE;
    G.cpu.last_warn_fired_at = G.cpu.last_crit_fired_at = 0;
    G.mem.warn_since = G.mem.crit_since = 0;
    G.mem.warn_fired = G.mem.crit_fired = FALSE;
    G.mem.last_warn_fired_at = G.mem.last_crit_fired_at = 0;
    G.disk.warn_since = G.disk.crit_since = 0;
    G.disk.warn_fired = G.disk.crit_fired = FALSE;
    G.disk.last_warn_fired_at = G.disk.last_crit_fired_at = 0;

    /* enabled=true로 변경되었는데 스레드가 아직 미실행이면 자동 시작 */
    if (G.enabled && !G.running) {
        G.running = TRUE;
        G.thread = g_thread_new("alert-engine", _alert_thread, NULL);
    }

    g_mutex_unlock(&G.mu);

    PCV_LOG_INFO(ALERT_LOG_DOM, "Alert config updated: enabled=%d cpu=%d/%d mem=%d/%d disk=%d/%d eval=%ds dedup=%ds webhook=%s",
                 G.enabled,
                 (int)G.cpu.warn_thresh, (int)G.cpu.crit_thresh,
                 (int)G.mem.warn_thresh, (int)G.mem.crit_thresh,
                 (int)G.disk.warn_thresh, (int)G.disk.crit_thresh,
                 G.eval_period_sec, G.dedup_window_sec, G.webhook_url);
    return TRUE;
}

/* ── Webhook DLQ 공개 API (thin wrappers) ─────────────────────
 * 코어 구현은 alert_dlq.{c,h} 로 추출됐다(AIO-4). dispatcher.c 가 부르는 공개
 * 심볼 pcv_alert_engine_dlq_list/_retry 를 보존하기 위해 얇은 위임만 남긴다. */

/**
 * pcv_alert_engine_dlq_list — DLQ 목록 반환 (→ pcv_alert_dlq_list)
 * Returns: (transfer full): JsonArray* — 호출자가 json_array_unref() 필요
 */
JsonArray *
pcv_alert_engine_dlq_list(void)
{
    return pcv_alert_dlq_list();
}

/**
 * pcv_alert_engine_dlq_retry — DLQ 항목 재전송 시도 (→ pcv_alert_dlq_retry)
 *
 * AIO-4: 코어 구현은 락을 스냅샷·제거 순간만 보유하고 HTTP 동안엔 보유하지
 * 않으며, 성공 항목을 값매칭으로 제거한다.
 * Returns: (transfer full): JsonObject* {retried, succeeded, failed}
 */
JsonObject *
pcv_alert_engine_dlq_retry(void)
{
    return pcv_alert_dlq_retry();
}

/* ══════════════════════════════════════════════════════════════
 * [백엔드 4차] 알림 음소거 (Silence) — src/modules/daemons/alert_silence.{c,h} 로 추출 (AIO-3).
 *   AlertSilence 스토어 + pcv_alert_add_silence/is_silenced/get_silences 정의는
 *   실-코드 효과 테스트(tests/test_alert_silence.c)를 위해 별도 TU 로 이동했다.
 *   발화측 호출부(_fire_alert)와 PCV_SAFETY_CONTROL 마커는 이 파일에 잔존.
 * ══════════════════════════════════════════════════════════════ */
