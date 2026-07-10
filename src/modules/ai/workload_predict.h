/* ==========================================================================
 * src/modules/ai/workload_predict.h
 * PureCVisor — EMA + 선형 회귀 워크로드 예측 공개 API
 *
 * [파일 역할]
 *   현재 CPU/MEM 사용률의 추세를 분석하여 5분 후 값을 예측하는 모듈의
 *   공개 인터페이스. 지수 이동 평균(EMA)으로 노이즈를 제거하고,
 *   최소제곱법 선형 회귀로 기울기를 계산하여 예측값을 산출한다.
 *
 * [아키텍처 위치]
 *   main.c         -> pcv_predict_init()      (뮤텍스 설정)
 *   ai_agent.c     -> pcv_predict_evaluate()   (5초 타이머, anomaly 후 호출)
 *   self_healing.c -> pcv_healing_on_prediction() (예측값 초과 시 사전 대응)
 *   rest_server.c  -> pcv_predict_get_forecast_json() (REST API 조회)
 *
 * [예측 알고리즘]
 *   1. EMA: alpha=0.3, 최근 값 30% 반영 (노이즈 감쇠)
 *   2. 선형 회귀: 60샘플(5분)로 기울기 계산 (OLS)
 *   3. 예측: EMA + slope x 60(5분) -> [0, 100] 클램핑
 *
 * [출력]
 *   Prometheus gauge 4개 (predict_cpu_5m, predict_mem_5m, trend_cpu, trend_mem)
 *   WebSocket broadcast ("forecast" 타입)
 *
 * [메모리 관리]
 *   pcv_predict_get_forecast_json() 반환값: 호출자 g_free()
 * ========================================================================== */

#ifndef PURECVISOR_WORKLOAD_PREDICT_H
#define PURECVISOR_WORKLOAD_PREDICT_H

#include <glib.h>

G_BEGIN_DECLS

/** 워크로드 예측 모듈 초기화 — 뮤텍스 설정 */
void pcv_predict_init(void);

/** 워크로드 예측 모듈 종료 — 뮤텍스 해제 */
void pcv_predict_shutdown(void);

/**
 * pcv_predict_evaluate:
 * ebpf_telemetry에서 현재 CPU%/MEM%를 읽어 예측을 갱신한다.
 * ai_agent.c의 5초 타이머에서 pcv_anomaly_evaluate() 후 호출.
 * Prometheus 게이지 + WebSocket 브로드캐스트 업데이트.
 */
void pcv_predict_evaluate(void);

/**
 * pcv_predict_get_forecast_json:
 * 모든 추적 노드의 현재 예측 상태를 JSON 배열 문자열로 반환.
 * REST API(/api/v1/ai/forecast)와 Web UI 대시보드에서 사용.
 * Returns: (transfer full): JSON 배열 문자열 (호출자 g_free 필요)
 */
gchar *pcv_predict_get_forecast_json(void);

G_END_DECLS

#endif /* PURECVISOR_WORKLOAD_PREDICT_H */
