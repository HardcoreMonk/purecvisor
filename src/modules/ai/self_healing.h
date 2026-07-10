/* ==========================================================================
 * src/modules/ai/self_healing.h
 * PureCVisor — Self-Healing 정책 엔진 공개 API
 *
 * [파일 역할]
 *   이상 탐지(anomaly_detector)와 워크로드 예측(workload_predict)의 이벤트를
 *   수신하여, 사전 정의된 정책에 따라 자동 복구 액션을 실행하는 모듈의
 *   공개 인터페이스. 안전장치(Approval Gate, Cooldown, Circuit Breaker,
 *   Rate Limit, Dry Run)를 갖춘 정책 기반 자가 치유 시스템.
 *
 * [아키텍처 위치]
 *   main.c              -> pcv_healing_init() / shutdown()
 *   anomaly_detector.c  -> pcv_healing_on_anomaly()    (이상 감지 시)
 *   workload_predict.c  -> pcv_healing_on_prediction() (예측 초과 시)
 *   rest_server.c       -> pcv_healing_get_pending_json() (대기 목록 조회)
 *   rest_server.c       -> pcv_healing_approve/dismiss() (Web UI 승인/거부)
 *
 * [내장 정책 (8개)]
 *   cpu-overload:     CPU Z>3.0 또는 예측>85% → migrate (승인 필요)
 *   mem-pressure:     MEM Z>2.5 또는 예측>90% → migrate (승인 필요)
 *   thermal-alert:    온도 Z>2.0 또는 >80도  → alert_only
 *   vm-unresponsive:  (트리거 없음)          → restart (자동)
 *   swap-storm:       Swap Z>2.5             → alert_only
 *   disk-saturated:   Disk I/O Z>3.0         → alert_only
 *   net-errors:       Net Error Z>2.0        → alert_only
 *   conntrack-full:   conntrack Z>2.5        → alert_only
 *
 * [안전장치]
 *   Approval Gate:   위험 액션(migrate/restart)은 Web UI 승인 대기
 *   Cooldown:        동일 정책 재실행 방지 (정책별 초 단위)
 *   Circuit Breaker: 연속 3회 실패 시 정책 비활성화
 *   Rate Limit:      5분 내 최대 3개 자동 액션
 *   Dry Run:         기본 모드, 액션 미실행 + 로그만 기록
 *
 * [복합 조건]
 *   2개+ 정책 동시 트리거 시 AI Agent에 자문 요청
 *   (pcv_agent_compare_async → 4개 프로바이더 병렬 질의)
 *
 * [메모리 관리]
 *   pcv_healing_get_pending_json() 반환값: 호출자 g_free()
 * ========================================================================== */

#ifndef PURECVISOR_SELF_HEALING_H
#define PURECVISOR_SELF_HEALING_H

#include <glib.h>

G_BEGIN_DECLS

/** Self-Healing 모듈 초기화 — 내장 정책 등록, 기본 dry_run 모드 */
void pcv_healing_init(void);

/** Self-Healing 모듈 종료 — 뮤텍스 해제 */
void pcv_healing_shutdown(void);

/**
 * pcv_healing_on_anomaly:
 * @metric:    이상이 감지된 메트릭 이름
 * @value:     현재 메트릭 값
 * @zscore:    이상 탐지 Z-Score
 * @threshold: 해당 메트릭의 Z-Score 임계값
 * @target_vm: (nullable) 액션 대상 VM 식별자(UUID 또는 이름). restart 액션의
 *             실제 재시작 대상. VM 컨텍스트가 없는 호출부는 NULL 을 전달한다.
 *
 * anomaly_detector에서 이상 감지 시 호출. 등록된 정책 중
 * trigger_metric이 일치하고 zscore가 기준을 초과하는 정책을 찾아 평가.
 * 복합 조건(2개+ 동시 트리거) 시 AI Agent 자문 요청.
 *
 * AF-1: target_vm 이 비-NULL 이고 매칭 정책 action="restart" 이면 실제 VM 재시작을
 * 워커 풀로 오프로드한다(dry_run/running-guard 안전판 적용).
 */
void pcv_healing_on_anomaly(const gchar *metric, gdouble value,
                             gdouble zscore, gdouble threshold,
                             const gchar *target_vm);

/**
 * pcv_healing_on_prediction:
 * @cpu_pred:  5분 후 CPU 예측값 (%)
 * @mem_pred:  5분 후 MEM 예측값 (%)
 * @cpu_trend: CPU 추세 기울기 (양수=상승)
 * @mem_trend: MEM 추세 기울기
 *
 * workload_predict에서 예측값 갱신 시 호출.
 * predict_threshold 설정된 정책에 대해 예측값 초과 시 사전 대응.
 */
void pcv_healing_on_prediction(gdouble cpu_pred, gdouble mem_pred,
                                gdouble cpu_trend, gdouble mem_trend);

/**
 * pcv_healing_get_pending_json:
 * 승인 대기 중인 액션 목록을 JSON 배열로 반환.
 * Web UI Self-Healing 패널에서 관리자가 승인/거부.
 * Returns: (transfer full): JSON 배열 문자열 (호출자 g_free 필요)
 */
gchar *pcv_healing_get_pending_json(void);

/**
 * pcv_healing_approve:
 * @action_id: 승인할 액션 ID (Web UI에서 선택)
 * 대기열에서 해당 ID의 액션을 찾아 승인 후 실행.
 */
void pcv_healing_approve(gint action_id);

/**
 * pcv_healing_dismiss:
 * @action_id: 거부할 액션 ID
 * 대기열에서 해당 ID의 액션을 찾아 거부 (실행하지 않음).
 */
void pcv_healing_dismiss(gint action_id);

/**
 * pcv_healing_get_history_json:
 * 자가 치유 액션 이력을 JSON 배열로 반환 (최신순, 최대 100개).
 * Returns: (transfer full): JSON 배열 문자열 (호출자 g_free 필요)
 */
gchar *pcv_healing_get_history_json(void);

/**
 * pcv_healing_set_mode:
 * @dry_run: TRUE면 로그만, FALSE면 실제 액션 실행
 *
 * Issue-M2 fix: 런타임에 self-healing 실행 모드를 전환.
 * Web UI healing.set_mode RPC 또는 pcv_healing_init() 내부에서 호출.
 * 기존엔 init에서만 TRUE 고정 + daemon.conf [ai] mode=active가 무시되는 문제 해결.
 */
void pcv_healing_set_mode(gboolean dry_run);

/**
 * pcv_healing_get_mode:
 * 현재 self-healing 모드 조회 (TRUE=dry_run, FALSE=active).
 */
gboolean pcv_healing_get_mode(void);

G_END_DECLS

#endif /* PURECVISOR_SELF_HEALING_H */
