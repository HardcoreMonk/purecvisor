/**
 * @file alert_engine.h
 * @brief WhaTap 스타일 알림 엔진 — 임계값 평가 + Webhook 발송
 *
 * ============================================================================
 *  파일 역할
 * ============================================================================
 *  호스트 메트릭(CPU/메모리/디스크 사용률)을 주기적으로 수집하여 사전 설정된
 *  임계값과 비교하고, 조건이 일정 시간(eval_period) 이상 **지속**될 때만
 *  경고(WARN) 또는 위험(CRIT) 알림을 발생시킨다.
 *  발생한 알림은 링버퍼 히스토리에 보존되며, 선택적으로 Webhook(Slack,
 *  Telegram, Generic JSON)으로 외부 시스템에 전송된다.
 *
 * ============================================================================
 *  아키텍처 위치
 * ============================================================================
 *  src/modules/daemons/ 계층에 위치하며, 텔레메트리(ebpf_telemetry)에서
 *  메트릭을 읽고, daemon(현재 에디션 데몬) 프로세스 내 별도 GThread로 동작한다.
 *
 *    purecvisorsd / purecvisormd (GMainLoop)
 *       ├── telemetry.c         ← 메트릭 수집 (1초 간격)
 *       ├── ebpf_telemetry.c    ← eBPF 호스트 메트릭 제공
 *       └── alert_engine.c      ← **이 모듈** (5초 간격 평가)
 *              ↓
 *         Webhook POST (Slack / Telegram / Generic)
 *
 * ============================================================================
 *  제공 API
 * ============================================================================
 *  - pcv_alert_engine_init()        : 엔진 초기화 + 스레드 시작
 *  - pcv_alert_engine_shutdown()    : 스레드 종료 + 리소스 정리
 *  - pcv_alert_engine_get_history() : 최근 알림 히스토리 조회 (최대 100건)
 *  - pcv_alert_engine_get_config()  : 현재 설정값 JSON 조회
 *  - pcv_alert_engine_set_config()  : 런타임 설정 변경 (임계값/Webhook 등)
 *
 * ============================================================================
 *  스레드 안전성
 * ============================================================================
 *  - init/shutdown은 메인 스레드에서 1회만 호출할 것.
 *  - get_history, get_config, set_config은 내부 GMutex로 보호되므로
 *    어느 스레드(REST 핸들러, RPC 디스패처 등)에서든 안전하게 호출 가능.
 *
 * ============================================================================
 *  주의사항
 * ============================================================================
 *  - Webhook POST는 alert-engine 스레드에서 **동기적**으로 수행된다.
 *    Webhook 서버가 느리면 해당 주기의 평가가 지연될 수 있으나,
 *    메인 이벤트 루프에는 영향을 주지 않는다.
 *  - eval_period가 0이면 순간 스파이크에도 즉시 알림이 발생하므로
 *    최소 10~30초를 권장한다.
 *  - 히스토리 링버퍼는 최대 100건이며, 초과 시 가장 오래된 기록부터 덮어쓴다.
 *
 * ============================================================================
 *  daemon.conf [alert] 섹션 설정 키
 * ============================================================================
 *   enabled          = true|false   알림 엔진 활성화 여부 (기본값: false)
 *   cpu_warn         = 80           CPU 경고 임계값 (%, 기본값: 80)
 *   cpu_crit         = 95           CPU 위험 임계값 (%, 기본값: 95)
 *   mem_warn         = 85           메모리 경고 임계값 (%, 기본값: 85)
 *   mem_crit         = 95           메모리 위험 임계값 (%, 기본값: 95)
 *   disk_warn        = 80           디스크 경고 임계값 (%, 기본값: 80)
 *   disk_crit        = 90           디스크 위험 임계값 (%, 기본값: 90)
 *   eval_period      = 30           조건 지속 시간 (초, 기본값: 30)
 *                                   → WhaTap의 "지속 조건" 개념 차용:
 *                                     순간 스파이크가 아닌 eval_period 동안
 *                                     연속으로 임계값을 초과해야 알림 발생
 *   webhook_url      = https://...  알림 전송 대상 URL (빈 문자열이면 전송 안 함)
 *   webhook_format   = slack        페이로드 포맷: slack | telegram | generic
 *   telegram_chat_id = 123456       Telegram 봇 API용 chat_id
 *
 * ============================================================================
 *  WhaTap 서버 모니터링에서 차용한 핵심 개념
 * ============================================================================
 *  1) eval_period 지속 조건
 *     - 메트릭이 임계값을 넘는 "시점"이 아니라, 임계값을 넘는 "상태가
 *       eval_period초 동안 연속 유지"될 때 알림을 발생시킨다.
 *     - 이를 통해 일시적 부하 스파이크에 의한 허위 알림(false positive)을
 *       효과적으로 억제한다.
 *
 *  2) 다단계 심각도 (Multi-level Severity)
 *     - WARN(경고)과 CRIT(위험) 두 단계로 분리하여, 운영자가 상황의
 *       심각도에 따라 차등 대응할 수 있도록 한다.
 *     - CRIT 상태에서는 WARN 알림이 중복 발생하지 않도록 상호 배제 처리된다.
 *
 *  3) 에피소드 기반 알림 (Episode-based Firing)
 *     - 한 번 알림이 발생하면 동일 에피소드 내에서는 재발생하지 않는다
 *       (warn_fired/crit_fired 플래그).
 *     - 조건이 해소(임계값 아래로 복귀)된 후 다시 초과하면 새 에피소드로
 *       간주하여 알림을 다시 발생시킨다.
 *
 * ============================================================================
 *  사용 예시 (daemon.conf 설정 + RPC 조회)
 * ============================================================================
 *
 *  [daemon.conf 설정 예시]
 *    [alert]
 *    enabled=true
 *    cpu_warn=80
 *    cpu_crit=95
 *    mem_warn=85
 *    mem_crit=95
 *    disk_warn=80
 *    disk_crit=90
 *    eval_period=30
 *    webhook_url=https://hooks.slack.com/services/T00/B00/xxx
 *    webhook_format=slack
 *
 *  [RPC 히스토리 조회]
 *    echo '{"jsonrpc":"2.0","method":"alert.history","params":{},"id":"1"}'
 *      | nc -U /var/run/purecvisor/daemon.sock
 *
 *  [REST API 설정 변경]
 *    curl -X PUT -H "Authorization: Bearer $TOKEN" \
 *      -d '{"cpu_warn":90,"eval_period":60}' \
 *      http://localhost:80/api/v1/alerts/config
 */
#ifndef PURECVISOR_ALERT_ENGINE_H
#define PURECVISOR_ALERT_ENGINE_H

#include <glib.h>
#include <json-glib/json-glib.h>

#include "alert_silence.h"  /* 음소거 스토어 추출 (AIO-3) — pcv_alert_*silence* 선언 */

G_BEGIN_DECLS

/**
 * @brief 알림 엔진을 초기화하고 평가 스레드를 시작한다.
 *
 * daemon.conf [alert] 섹션에서 설정을 읽어 임계값, Webhook URL 등을 세팅한 뒤,
 * enabled=true이면 "alert-engine" GThread를 생성하여 주기적 평가를 시작한다.
 *
 * 호출 컨텍스트: 메인 스레드에서 데몬 시작 시 1회 호출.
 * 스레드 안전성: 메인 스레드 단독 호출 전제. 중복 호출 금지.
 */
void        pcv_alert_engine_init(void);

/**
 * @brief 알림 엔진 스레드를 정지시키고 리소스를 정리한다.
 *
 * running 플래그를 FALSE로 설정하여 스레드 루프를 탈출시키고,
 * g_thread_join()으로 종료를 대기한 뒤 GMutex를 해제한다.
 *
 * 호출 컨텍스트: 메인 스레드에서 데몬 종료(drain) 시 1회 호출.
 * 스레드 안전성: 메인 스레드 단독 호출 전제. init 이전 호출 시 안전하게 무시.
 */
void        pcv_alert_engine_shutdown(void);

/**
 * @brief 최근 알림 히스토리를 JsonArray로 반환한다.
 *
 * 링버퍼에 저장된 알림 기록을 오래된 순서부터 최신 순서로 정렬하여 반환.
 * 각 요소는 JsonObject로 metric, severity, value, timestamp, message 키를 포함.
 *
 * @return JsonArray* — 호출자(caller)가 소유권을 가짐. 사용 후 json_array_unref() 필요.
 *
 * 호출 컨텍스트: REST 핸들러, RPC 디스패처 등 임의 스레드.
 * 스레드 안전성: 내부 GMutex로 보호됨. 안전.
 */
JsonArray  *pcv_alert_engine_get_history(void);

/**
 * @brief 현재 알림 엔진 설정을 JsonObject로 반환한다.
 *
 * enabled, cpu_warn, cpu_crit, mem_warn, mem_crit, disk_warn, disk_crit,
 * eval_period, webhook_url, webhook_format, telegram_chat_id, alert_count
 * 키를 포함하는 JsonObject를 생성하여 반환한다.
 *
 * @return JsonObject* — 호출자(caller)가 소유권을 가짐. 사용 후 json_object_unref() 필요.
 *
 * 호출 컨텍스트: REST 핸들러, RPC 디스패처 등 임의 스레드.
 * 스레드 안전성: 내부 GMutex로 보호됨. 안전.
 */
JsonObject *pcv_alert_engine_get_config(void);

/**
 * @brief 런타임에 알림 엔진 설정을 변경한다.
 *
 * 전달된 JsonObject에 포함된 키만 선택적으로 업데이트한다 (부분 업데이트 지원).
 * 설정 변경 시 모든 메트릭의 발화 상태(warn_since/crit_since/fired)가 초기화되어
 * 새 설정 기준으로 평가를 처음부터 다시 시작한다.
 *
 * enabled=true로 변경하면서 스레드가 실행 중이 아닌 경우, 자동으로 스레드를 시작한다.
 *
 * @param cfg JsonObject* — 변경할 설정 키-값. 호출자가 소유권을 유지함 (내부에서 unref 안 함).
 *                          NULL이면 FALSE를 반환.
 * @return gboolean — 성공 시 TRUE, cfg가 NULL이면 FALSE.
 *
 * 호출 컨텍스트: REST 핸들러, RPC 디스패처 등 임의 스레드.
 * 스레드 안전성: 내부 GMutex로 보호됨. 안전.
 */
gboolean    pcv_alert_engine_set_config(JsonObject *cfg);

/**
 * @brief 보안 이벤트를 알림 히스토리에 기록한다.
 *
 * HIDS/HIPS 보안 이벤트 제출 경로에서 WARN/CRIT 이벤트를 일반 알림 히스토리와
 * 같은 링버퍼에 보존한다. Webhook 전송은 기존 메트릭 임계값 알림 경로에만 둔다.
 */
void        pcv_alert_record_security_event(const gchar *event_id,
                                            const gchar *severity,
                                            const gchar *summary);

/**
 * @brief 임계값 평가 경로 밖에서 운영자 알림을 직접 발화한다.
 *
 * CPU/메모리/디스크 지속조건(_eval_metric) 밖의 이벤트(예: ZFS 풀 SUSPENDED)를
 * 기존 알림/webhook 경로로 통보한다. 동작:
 *   - @source 가 음소거(pcv_alert_add_silence) 대상이면 발화하지 않음.
 *   - 히스토리 링버퍼에 기록(RPC alert.history 노출).
 *   - webhook_url 설정 시 slack/telegram/generic 포맷으로 비동기 전송
 *     (is_crit && webhook_crit_url 설정 시 CRIT 전용 URL 사용).
 *
 * dedup(스팸 억제)은 호출자 책임 — 이 함수는 호출 시마다 1회 발화한다.
 *
 * @param source   알림 소스/메트릭 라벨(예: "zpool"). 음소거 키로도 사용.
 * @param is_crit  TRUE=CRIT, FALSE=WARN
 * @param value    수치 게이지 값(없으면 0)
 * @param message  사람이 읽을 전문(히스토리 + webhook 메시지에 그대로 사용)
 *
 * 호출 컨텍스트: 임의 스레드(내부 G.mu 락). init 이전 호출은 안전하게 무시.
 */
void        pcv_alert_fire_event(const gchar *source, gboolean is_crit,
                                 gdouble value, const gchar *message);

/**
 * @brief Webhook DLQ(Dead Letter Queue) 목록을 반환한다.
 *
 * 재시도 실패한 Webhook 페이로드가 저장된 DLQ 내용을 조회한다.
 * 각 항목은 {"url":"...", "payload":"...", "index":N} 형태.
 *
 * @return JsonArray* — 호출자가 소유권을 가짐.
 */
JsonArray  *pcv_alert_engine_dlq_list(void);

/**
 * @brief DLQ에 저장된 항목을 일괄 재전송한다.
 *
 * 성공한 항목은 DLQ에서 제거된다.
 *
 * @return JsonObject* {retried, succeeded, failed} — 호출자가 소유권을 가짐.
 */
JsonObject *pcv_alert_engine_dlq_retry(void);

/* ── BE-A14: 알림 ACK (확인) ──────────────────────────────── */

/**
 * pcv_alert_acknowledge:
 * @alert_id: 확인할 알림 ID
 *
 * 알림을 ACK 처리합니다. ACK된 CRIT 알림은 에스컬레이션 대상에서 제외.
 * Returns: 해당 ID 알림을 찾아 ACK했으면 TRUE
 */
gboolean    pcv_alert_acknowledge(gint64 alert_id);

/* ── [백엔드 4차] 알림 음소거 ──────────────────────────────── */
/* pcv_alert_add_silence/is_silenced/get_silences/silence_reset 선언은
 * alert_silence.h 로 이동 (AIO-3, 위 #include 로 노출). */

/* ── R-2: SLA/가동률 추적 ─────────────────────────────────── */

/**
 * pcv_alert_get_sla:
 * @vm_name: 조회할 VM 이름
 *
 * VM의 SLA(가동률) 정보를 반환합니다.
 * uptime_percent, uptime_seconds, downtime_seconds 키를 포함.
 *
 * Returns: (transfer full): JsonObject — 호출자가 json_object_unref() 해야 함.
 */
JsonObject *pcv_alert_get_sla(const gchar *vm_name);

/* ── R-4: Per-VM 웹훅 라우팅 ──────────────────────────────── */

/**
 * pcv_alert_set_vm_webhook:
 * @vm_name:     대상 VM 이름
 * @webhook_url: 해당 VM 전용 웹훅 URL (NULL/빈문자열이면 삭제)
 *
 * 특정 VM에 대한 알림을 전용 웹훅으로 라우팅합니다.
 */
void pcv_alert_set_vm_webhook(const gchar *vm_name, const gchar *webhook_url);

/* ── R-2: SLA/가동률 추적 ─────────────────────────────────── */

/**
 * pcv_alert_get_sla:
 * @vm_name: 조회할 VM 이름
 *
 * VM의 SLA(가동률) 정보를 반환합니다.
 * uptime_percent, uptime_seconds, downtime_seconds 키를 포함.
 *
 * Returns: (transfer full): JsonObject -- 호출자가 json_object_unref() 해야 함.
 */
JsonObject *pcv_alert_get_sla(const gchar *vm_name);

/* ── R-4: Per-VM 웹훅 라우팅 ──────────────────────────────── */

/**
 * pcv_alert_set_vm_webhook:
 * @vm_name:     대상 VM 이름
 * @webhook_url: 해당 VM 전용 웹훅 URL (NULL/빈문자열이면 삭제)
 *
 * 특정 VM에 대한 알림을 전용 웹훅으로 라우팅합니다.
 */
void pcv_alert_set_vm_webhook(const gchar *vm_name, const gchar *webhook_url);

G_END_DECLS

#endif /* PURECVISOR_ALERT_ENGINE_H */
