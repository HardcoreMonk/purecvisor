
#ifndef PURECVISOR_LOG_H
#define PURECVISOR_LOG_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
    PCV_LOG_LEVEL_DEBUG = 0,
    PCV_LOG_LEVEL_INFO  = 1,
    PCV_LOG_LEVEL_WARN  = 2,
    PCV_LOG_LEVEL_ERROR = 3,
    PCV_LOG_LEVEL_NONE  = 99
} PcvLogLevel;

void pcv_log_init(void);

void pcv_log_shutdown(void);

void pcv_log_req_id_set(const gchar *req_id);

const gchar *pcv_log_req_id_get(void);

void _pcv_log(GLogLevelFlags level,
              const gchar   *domain,
              const gchar   *fmt,
              ...) G_GNUC_PRINTF(3, 4);

void _pcv_log_audit(const gchar *domain,
                    const gchar *operation,
                    const gchar *target,
                    const gchar *fmt,
                    ...) G_GNUC_PRINTF(4, 5);

#define PCV_LOG_DEBUG(dom, ...)  _pcv_log(G_LOG_LEVEL_DEBUG,    (dom), ##__VA_ARGS__)
#define PCV_LOG_INFO(dom, ...)   _pcv_log(G_LOG_LEVEL_MESSAGE,  (dom), ##__VA_ARGS__)
#define PCV_LOG_WARN(dom, ...)   _pcv_log(G_LOG_LEVEL_WARNING,  (dom), ##__VA_ARGS__)
#define PCV_LOG_ERROR(dom, ...)  _pcv_log(G_LOG_LEVEL_CRITICAL, (dom), ##__VA_ARGS__)

#define PCV_LOG_AUDIT(dom, op, tgt, ...) \
    _pcv_log_audit((dom), (op), (tgt), ##__VA_ARGS__)

void pcv_log_load_module_levels(void);

void pcv_log_set_module_level(const gchar *domain, PcvLogLevel level);

PcvLogLevel pcv_log_get_module_level(const gchar *domain);

void pcv_log_set_global_level(PcvLogLevel level);

PcvLogLevel pcv_log_get_global_level(void);

gchar *pcv_generate_request_id(void);

typedef struct {
    gchar   trace_id[33];
    gchar   span_id[17];
    gchar   parent_id[17];
    guint8  flags;
} PcvTraceContext;

PcvTraceContext *pcv_trace_context_new(void);

PcvTraceContext *pcv_trace_context_parse(const gchar *traceparent);

gchar *pcv_trace_context_format(const PcvTraceContext *ctx);

void pcv_trace_context_free(PcvTraceContext *ctx);

G_END_DECLS

#endif
