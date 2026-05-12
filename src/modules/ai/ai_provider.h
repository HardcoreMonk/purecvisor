






















#ifndef PURECVISOR_AI_PROVIDER_H
#define PURECVISOR_AI_PROVIDER_H

#include <glib.h>

G_BEGIN_DECLS











typedef enum {
    PCV_AI_PROVIDER_CLAUDE  = 0,
    PCV_AI_PROVIDER_OPENAI  = 1,
    PCV_AI_PROVIDER_GEMINI  = 2,
    PCV_AI_PROVIDER_OLLAMA  = 3,
    PCV_AI_PROVIDER_COUNT   = 4
} PcvAiProvider;










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












typedef struct {
    PcvAiProvider provider;
    gchar        model[64];
    gchar        api_key[256];
    gchar        endpoint[256];
    gboolean     enabled;
} PcvAiProviderConfig;




















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
    gint         input_tokens;
    gint         output_tokens;
    gboolean     success;
    gchar        error[256];
} PcvAgentResult;













typedef struct {
    PcvAgentResult results[PCV_AI_PROVIDER_COUNT];
    gint           result_count;
    gchar          consensus_action[32];
    gdouble        consensus_confidence;
    gdouble        avg_latency_ms;
    gint64         timestamp_us;
} PcvAgentComparison;

G_END_DECLS

#endif
