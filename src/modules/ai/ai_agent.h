/* ==========================================================================
 * src/modules/ai/ai_agent.h
 * PureCVisor — 멀티 프로바이더 AI Agent 공개 API
 *
 * [파일 역할]
 *   PureCVisor AI Ops의 핵심 엔진 인터페이스. 4개 AI 프로바이더(Claude, OpenAI,
 *   Gemini, Ollama)에 클러스터 메트릭과 이상 탐지 컨텍스트를 병렬로 전송하고,
 *   가중 다수결 합의 알고리즘으로 최적의 자가 치유 액션을 도출한다.
 *
 * [아키텍처 위치]
 *   main.c         -> pcv_agent_init() / pcv_agent_shutdown()
 *   daemon.c       -> pcv_agent_configure() (daemon.conf [ai] 섹션 파싱 후)
 *   self_healing.c -> pcv_agent_compare_async() (복합 이상 조건 감지 시)
 *   dispatcher.c   -> pcv_agent_get/set_config() (agent.config.get/set RPC)
 *                  -> pcv_agent_get_last_comparison_json() (agent.history RPC)
 *
 * [합의 알고리즘]
 *   각 프로바이더의 추천 액션에 confidence x weight를 곱해 점수를 합산.
 *   2개+ 프로바이더 동의 또는 단일 0.8+ 점수 시 채택, 미달 시 alert_only.
 *   가중치: Claude=1.0, OpenAI=1.0, Gemini=0.9, Ollama=0.7
 *
 * [안전장치]
 *   - 레이트 리밋: 5분 내 최대 1회 질의
 *   - HTTP 타임아웃: 10초 초과 시 프로바이더 응답 무시
 *   - API 키 마스킹: 조회 시 앞뒤 3자만 표시 ("sk-...xxx")
 *   - 비활성 기본값: daemon.conf에서 API 키 미설정 시 프로바이더 비활성
 *
 * [메모리 관리]
 *   - pcv_agent_get_last_comparison_json(): 호출자 g_free()
 *   - pcv_agent_get_config(): 호출자 json_object_unref()
 *
 * [주의사항]
 *   - pcv_agent_init()은 pcv_config_init() 이후 호출
 *   - compare_async는 fire-and-forget (응답을 기다리지 않음)
 *   - 빌드 의존성: libsoup-3.0, json-glib-1.0
 * ========================================================================== */

#ifndef PURECVISOR_AI_AGENT_H
#define PURECVISOR_AI_AGENT_H

#include <glib.h>
#include <json-glib/json-glib.h>
#include "ai_provider.h"

G_BEGIN_DECLS

/**
 * pcv_agent_init:
 * AI Agent 모듈 초기화. 4개 프로바이더 기본 엔드포인트/모델 설정.
 * API 키가 없으므로 모두 비활성(enabled=FALSE) 상태로 시작한다.
 * main.c에서 pcv_config_init() 이후 1회 호출.
 */
void pcv_agent_init(void);

/**
 * pcv_agent_shutdown:
 * AI Agent 모듈 종료. 이력 링버퍼와 뮤텍스 해제.
 * 진행 중인 GTask는 GLib이 자동 정리한다.
 */
void pcv_agent_shutdown(void);

/**
 * pcv_agent_configure:
 * @provider: 설정할 프로바이더 (PCV_AI_PROVIDER_CLAUDE/OPENAI/GEMINI/OLLAMA)
 * @model:    모델명 (NULL이면 기존 유지, 예: "claude-sonnet-4-20250514")
 * @api_key:  API 키 (NULL이면 기존 유지, 설정 시 자동 활성화)
 * @endpoint: API 엔드포인트 URL (NULL이면 기존 유지)
 *
 * 프로바이더 설정 변경. daemon.conf 파싱 후 또는 REST API로 런타임 변경 시 호출.
 * API 키가 설정되면 해당 프로바이더가 자동으로 enabled=TRUE 전환.
 */
void pcv_agent_configure(PcvAiProvider provider, const gchar *model,
                          const gchar *api_key, const gchar *endpoint);

/**
 * pcv_agent_compare_async:
 * @metrics_json:    현재 호스트 메트릭 JSON (ebpf_telemetry에서 수집)
 * @anomaly_context: 이상 탐지 컨텍스트 (어떤 메트릭이 왜 이상인지 설명)
 *
 * 활성화된 모든 프로바이더에 병렬 질의 발송 (fire-and-forget).
 * 5분 레이트 리밋 적용, 활성 프로바이더 0개면 무시.
 * 결과는 내부 콜백에서 비동기 수집 → 합의 도출 → 이력 저장.
 */
void pcv_agent_compare_async(const gchar *metrics_json,
                              const gchar *anomaly_context);

/**
 * pcv_agent_get_last_comparison_json:
 * 가장 최근 비교 결과를 JSON 문자열로 반환.
 * Web UI AI Agent 패널 + agent.history RPC에서 사용.
 * 이력 없으면 {"status":"no_data"} 반환.
 * Returns: (transfer full): JSON 문자열 (호출자 g_free 필요)
 */
gchar *pcv_agent_get_last_comparison_json(void);

/**
 * pcv_agent_get_config:
 * 현재 AI Agent 설정을 JsonObject로 반환.
 * API 키는 보안을 위해 마스킹 (앞뒤 3자 + "...").
 * Returns: (transfer full): JsonObject* (호출자 json_object_unref 필요)
 */
JsonObject *pcv_agent_get_config(void);

/**
 * pcv_agent_set_config:
 * @params: 설정 변경 JSON (providers 배열 포함)
 * REST/RPC를 통해 프로바이더 설정을 런타임 변경.
 * 마스킹된 키("..."포함)는 무시, 빈 문자열이면 프로바이더 비활성화.
 * Returns: 설정 변경 성공 시 TRUE
 */
gboolean    pcv_agent_set_config(JsonObject *params);

G_END_DECLS

#endif /* PURECVISOR_AI_AGENT_H */
