
#ifndef PURECVISOR_AI_AGENT_H
#define PURECVISOR_AI_AGENT_H

#include <glib.h>
#include <json-glib/json-glib.h>
#include "ai_provider.h"

G_BEGIN_DECLS

void pcv_agent_init(void);

void pcv_agent_shutdown(void);

void pcv_agent_configure(PcvAiProvider provider, const gchar *model,
                          const gchar *api_key, const gchar *endpoint);

void pcv_agent_compare_async(const gchar *metrics_json,
                              const gchar *anomaly_context);

gchar *pcv_agent_get_last_comparison_json(void);

JsonObject *pcv_agent_get_config(void);

gboolean    pcv_agent_set_config(JsonObject *params);

G_END_DECLS

#endif
