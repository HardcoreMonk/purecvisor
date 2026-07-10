/* ==========================================================================
 * src/modules/ai/ai_provider.h
 * PureCVisor — AI 프로바이더 열거형 및 공유 자료구조
 *
 * [파일 역할]
 *   AI Agent 모듈 전체에서 공유하는 프로바이더 열거형, 설정 구조체,
 *   질의 결과 구조체, 비교(합의) 결과 구조체를 정의합니다.
 *   ai_agent.c, anomaly_detector.c, workload_predict.c, self_healing.c에서
 *   공통으로 include합니다.
 *
 * [주요 자료구조]
 *   PcvAiProvider       — 프로바이더 열거형 (Claude/OpenAI/Gemini/Ollama)
 *   PcvAiProviderConfig — 프로바이더별 설정 (모델명, API 키, 엔드포인트, 활성화)
 *   PcvAgentResult      — 단일 프로바이더의 질의 응답 (액션, 신뢰도, 레이턴시)
 *   PcvAgentComparison  — 전체 비교 결과 (프로바이더별 응답 + 합의 액션)
 *
 * [프로바이더 특성]
 *   Claude  — Anthropic Messages API, x-api-key 인증, system 최상위 필드
 *   OpenAI  — Chat Completions API, Bearer 토큰, response_format JSON 모드
 *   Gemini  — generateContent API, URL 파라미터 키, system 미지원(프롬프트 합침)
 *   Ollama  — 로컬 /api/chat, 인증 불필요, stream:false 필수
 * ========================================================================== */

#ifndef PURECVISOR_AI_PROVIDER_H
#define PURECVISOR_AI_PROVIDER_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * PcvAiProvider:
 * AI 프로바이더 열거형. 배열 인덱스로도 사용됩니다.
 *
 * CLAUDE(0):  Anthropic Claude (클라우드, 대형 모델)
 * OPENAI(1):  OpenAI GPT (클라우드, 대형 모델)
 * GEMINI(2):  Google Gemini (클라우드, 대형 모델)
 * OLLAMA(3):  Ollama 로컬 LLM (자체 호스팅, 소형 모델)
 * COUNT(4):   프로바이더 총 수 (배열 크기 상수)
 */
typedef enum {
    PCV_AI_PROVIDER_CLAUDE  = 0,
    PCV_AI_PROVIDER_OPENAI  = 1,
    PCV_AI_PROVIDER_GEMINI  = 2,
    PCV_AI_PROVIDER_OLLAMA  = 3,
    PCV_AI_PROVIDER_COUNT   = 4
} PcvAiProvider;

/**
 * pcv_ai_provider_name:
 * @p: 프로바이더 열거형
 *
 * 프로바이더 열거형을 사람이 읽을 수 있는 문자열로 변환.
 * 로그 출력, Prometheus 라벨, JSON 응답에서 사용.
 *
 * Returns: (transfer none): 정적 문자열 ("Claude"/"OpenAI"/"Gemini"/"Ollama")
 */
static inline const gchar *
pcv_ai_provider_name(PcvAiProvider p)
{
    switch (p) {
    case PCV_AI_PROVIDER_CLAUDE:  return "Claude";
    case PCV_AI_PROVIDER_OPENAI:  return "OpenAI";
    case PCV_AI_PROVIDER_GEMINI:  return "Gemini";
    case PCV_AI_PROVIDER_OLLAMA:  return "Ollama";
    default:                      return "Unknown";
    }
}

/**
 * PcvAiProviderConfig:
 * 단일 프로바이더의 런타임 설정.
 * ai_agent.c의 G.providers[] 배열에 프로바이더별 1개씩 저장됩니다.
 *
 * @provider: 프로바이더 종류 (배열 인덱스와 동일)
 * @model:    사용할 모델명 (예: "claude-sonnet-4-20250514", "gpt-4o")
 * @api_key:  API 인증 키 (빈 문자열이면 비활성 상태)
 * @endpoint: API 엔드포인트 URL (프로바이더별 기본값 존재)
 * @enabled:  활성 여부 (API 키 설정 시 자동 TRUE)
 */
typedef struct {
    PcvAiProvider provider;
    gchar        model[64];
    gchar        api_key[256];
    gchar        endpoint[256];
    gboolean     enabled;
} PcvAiProviderConfig;

/**
 * PcvAgentResult:
 * 단일 프로바이더의 질의 응답 결과.
 * _query_thread()에서 HTTP POST 후 응답을 파싱하여 채웁니다.
 *
 * @provider:    응답한 프로바이더
 * @model:       사용된 모델명
 * @action:      추천 액션 ("migrate"/"restart"/"scale_cpu"/"scale_mem"/"alert_only")
 * @target_vm:   대상 VM 이름
 * @from_node:   마이그레이션 출발 노드
 * @to_node:     마이그레이션 도착 노드
 * @reason:      추천 사유 (한국어)
 * @alternative: 대안 액션 설명
 * @confidence:  신뢰도 (0.0 ~ 1.0, 높을수록 확신)
 * @urgency:     긴급도 ("low"/"medium"/"high"/"critical")
 * @latency_ms:  API 응답 시간 (밀리초)
 * @success:     유효한 액션이 파싱되었으면 TRUE
 * @error:       에러 발생 시 메시지
 */
typedef struct {
    PcvAiProvider provider;
    gchar        model[64];
    gchar        action[32];
    gchar        target_vm[64];
    gchar        from_node[32];
    gchar        to_node[32];
    gchar        reason[512];
    gchar        alternative[256];
    gdouble      confidence;
    gchar        urgency[16];
    gdouble      latency_ms;
    gint         input_tokens;     /* 입력 토큰 수 (프로바이더가 반환 시) */
    gint         output_tokens;    /* 출력 토큰 수 (프로바이더가 반환 시) */
    gboolean     success;
    gchar        error[256];
} PcvAgentResult;

/**
 * PcvAgentComparison:
 * 단일 비교 사이클의 전체 결과 — 프로바이더별 응답 + 합의.
 * ai_agent.c의 G.history[] 링버퍼에 저장됩니다.
 *
 * @results:              프로바이더별 응답 배열 (최대 4개)
 * @result_count:         실제 응답 수
 * @consensus_action:     합의된 최종 액션 (가중 다수결 결과)
 * @consensus_confidence: 합의 액션의 평균 신뢰도
 * @avg_latency_ms:       전체 프로바이더 평균 응답 시간
 * @timestamp_us:         비교 완료 시각 (마이크로초, Unix epoch)
 */
typedef struct {
    PcvAgentResult results[PCV_AI_PROVIDER_COUNT];
    gint           result_count;
    gchar          consensus_action[32];
    gdouble        consensus_confidence;
    gdouble        avg_latency_ms;
    gint64         timestamp_us;
} PcvAgentComparison;

G_END_DECLS

#endif /* PURECVISOR_AI_PROVIDER_H */
