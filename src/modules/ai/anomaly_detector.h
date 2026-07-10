/* ==========================================================================
 * src/modules/ai/anomaly_detector.h
 * PureCVisor — Z-Score 기반 이상 탐지 엔진 공개 API
 *
 * [파일 역할]
 *   Prometheus 메트릭을 순환 버퍼에 저장하고, 이동 평균 + 표준편차 기반
 *   Z-Score로 이상 여부를 실시간 판정하는 모듈의 공개 인터페이스.
 *
 * [아키텍처 위치]
 *   main.c         -> pcv_anomaly_init()      (감시 메트릭 등록)
 *   ai_agent.c     -> pcv_anomaly_evaluate()   (5초 타이머에서 호출)
 *   self_healing.c -> 이상 이벤트 수신 후 정책 매칭
 *   rest_server.c  -> pcv_anomaly_get_recent_json() (REST API 조회)
 *
 * [Z-Score 이상 탐지 원리]
 *   Z = |현재값 - 평균| / 표준편차
 *   Z > threshold 이면 통계적으로 비정상적인 값으로 판정.
 *   예) CPU 평균 30%, 표준편차 5%, 현재 55% -> Z = 5.0 (이상!)
 *
 * [감시 메트릭 (init 시 등록)]
 *   CPU%, MEM%, Disk I/O, Network Error, RPC 레이턴시,
 *   Swap, PSI, 온도(coretemp/k10temp), conntrack 등 10개
 *
 * [출력 경로]
 *   1. Prometheus gauge (purecvisor_anomaly_score/active/alerts_total)
 *   2. WebSocket broadcast ("anomaly" 타입)
 *   3. 감사 로그 (pcv_audit)
 *
 * [메모리 관리]
 *   pcv_anomaly_get_recent_json() 반환값: 호출자 g_free()
 * ========================================================================== */

#ifndef PURECVISOR_ANOMALY_DETECTOR_H
#define PURECVISOR_ANOMALY_DETECTOR_H

#include <glib.h>

G_BEGIN_DECLS

/** 이상 탐지 모듈 초기화 — 감시 메트릭 등록 + 뮤텍스 설정 */
void pcv_anomaly_init(void);

/** 이상 탐지 모듈 종료 — 뮤텍스 해제 */
void pcv_anomaly_shutdown(void);

/**
 * pcv_anomaly_evaluate:
 * 등록된 모든 감시 메트릭에 대해 이상 탐지를 수행한다.
 * ai_agent.c의 5초 타이머에서 주기적으로 호출된다.
 * Prometheus 렌더링 캐시(2초)를 통해 현재 값을 읽고,
 * 링 버퍼에 추가 → Z-Score 평가 → 이상 시 알림 발생.
 */
void pcv_anomaly_evaluate(void);

/**
 * pcv_anomaly_get_recent_json:
 * 최근 이상 이벤트를 JSON 배열 문자열로 반환 (최신 순 정렬).
 * REST API(/api/v1/ai/anomalies)와 Web UI에서 사용.
 * Returns: (transfer full): JSON 배열 문자열 (호출자 g_free 필요)
 */
gchar *pcv_anomaly_get_recent_json(void);

/**
 * pcv_anomaly_reset_baseline:
 * CE-A13: 모든 감시 메트릭의 롤링 통계(링 버퍼, 합계, 카운터)를 초기화한다.
 * 시스템 구성 변경 후 기존 베이스라인이 무효화되었을 때 RPC로 호출.
 * 메트릭 이름/라벨/임계값 설정은 보존된다.
 */
void pcv_anomaly_reset_baseline(void);

G_END_DECLS

#endif /* PURECVISOR_ANOMALY_DETECTOR_H */
