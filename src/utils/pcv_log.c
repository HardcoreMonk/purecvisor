


































































#include "pcv_log.h"
#include "pcv_config.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <unistd.h>




#define AUDIT_LOG_DIR   "/var/log/purecvisor"


#define AUDIT_LOG_PATH  "/var/log/purecvisor/audit.log"


#define LOG_DOM "pcv_log"











typedef struct {
    gboolean    use_journal;
    FILE       *audit_fp;
    GMutex      audit_mutex;
    gboolean    initialized;
    PcvLogLevel global_level;
    GHashTable *module_levels;






    GMutex      module_levels_mu;
} PcvLogState;


static PcvLogState g_pcv_log_state = { 0 };


















static GPrivate g_req_id_key = G_PRIVATE_INIT(g_free);


















static gchar *
_iso8601_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    struct tm tm_utc;
    gmtime_r(&ts.tv_sec, &tm_utc);


    return g_strdup_printf(
        "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ",
        tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
        tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec,
        ts.tv_nsec / 1000000L);
}















static const gchar *
_level_str(GLogLevelFlags level)
{
    if (level & G_LOG_LEVEL_ERROR)    return "ERROR";
    if (level & G_LOG_LEVEL_CRITICAL) return "CRIT";
    if (level & G_LOG_LEVEL_WARNING)  return "WARN";
    if (level & G_LOG_LEVEL_MESSAGE)  return "INFO";
    if (level & G_LOG_LEVEL_INFO)     return "INFO";
    if (level & G_LOG_LEVEL_DEBUG)    return "DEBUG";
    return "UNKNOWN";
}


















static const gchar *
_journal_prefix(GLogLevelFlags level)
{
    if (level & G_LOG_LEVEL_ERROR)    return "<3>";
    if (level & G_LOG_LEVEL_CRITICAL) return "<3>";
    if (level & G_LOG_LEVEL_WARNING)  return "<4>";
    if (level & G_LOG_LEVEL_MESSAGE)  return "<6>";
    if (level & G_LOG_LEVEL_INFO)     return "<6>";
    if (level & G_LOG_LEVEL_DEBUG)    return "<7>";
    return "<6>";
}



















static gchar *
_json_escape(const gchar *s)
{
    if (!s) return g_strdup("");
    GString *out = g_string_sized_new(strlen(s) + 16);
    for (const gchar *p = s; *p; p++) {
        switch (*p) {
            case '"':  g_string_append(out, "\\\""); break;
            case '\\': g_string_append(out, "\\\\"); break;
            case '\n': g_string_append(out, "\\n");  break;
            case '\r': g_string_append(out, "\\r");  break;
            case '\t': g_string_append(out, "\\t");  break;
            default:
                if ((guchar)*p < 0x20)

                    g_string_append_printf(out, "\\u%04x", (guchar)*p);
                else
                    g_string_append_c(out, *p);
                break;
        }
    }
    return g_string_free(out, FALSE);
}









static PcvLogLevel
_parse_log_level_str(const gchar *s)
{
    if (!s) return PCV_LOG_LEVEL_INFO;
    if (g_ascii_strcasecmp(s, "debug") == 0) return PCV_LOG_LEVEL_DEBUG;
    if (g_ascii_strcasecmp(s, "info")  == 0) return PCV_LOG_LEVEL_INFO;
    if (g_ascii_strcasecmp(s, "warn")  == 0) return PCV_LOG_LEVEL_WARN;
    if (g_ascii_strcasecmp(s, "warning") == 0) return PCV_LOG_LEVEL_WARN;
    if (g_ascii_strcasecmp(s, "error") == 0) return PCV_LOG_LEVEL_ERROR;
    if (g_ascii_strcasecmp(s, "crit")  == 0) return PCV_LOG_LEVEL_ERROR;
    if (g_ascii_strcasecmp(s, "none")  == 0) return PCV_LOG_LEVEL_NONE;
    return PCV_LOG_LEVEL_INFO;
}




static const gchar *
_level_name(PcvLogLevel lvl)
{
    switch (lvl) {
        case PCV_LOG_LEVEL_DEBUG: return "DEBUG";
        case PCV_LOG_LEVEL_INFO:  return "INFO";
        case PCV_LOG_LEVEL_WARN:  return "WARN";
        case PCV_LOG_LEVEL_ERROR: return "ERROR";
        case PCV_LOG_LEVEL_NONE:  return "NONE";
        default:                  return "INFO";
    }
}








static PcvLogLevel
_glevel_to_pcvlevel(GLogLevelFlags level)
{
    if (level & G_LOG_LEVEL_DEBUG)    return PCV_LOG_LEVEL_DEBUG;
    if (level & G_LOG_LEVEL_MESSAGE)  return PCV_LOG_LEVEL_INFO;
    if (level & G_LOG_LEVEL_INFO)     return PCV_LOG_LEVEL_INFO;
    if (level & G_LOG_LEVEL_WARNING)  return PCV_LOG_LEVEL_WARN;
    if (level & G_LOG_LEVEL_CRITICAL) return PCV_LOG_LEVEL_ERROR;
    if (level & G_LOG_LEVEL_ERROR)    return PCV_LOG_LEVEL_ERROR;
    return PCV_LOG_LEVEL_INFO;
}











static gboolean
_should_log(const gchar *domain, GLogLevelFlags msg_level)
{
    PcvLogLevel pcv_level = _glevel_to_pcvlevel(msg_level);


    if (msg_level & G_LOG_LEVEL_ERROR)
        return TRUE;


    if (g_pcv_log_state.module_levels && domain) {
        g_mutex_lock(&g_pcv_log_state.module_levels_mu);
        gpointer val = g_hash_table_lookup(g_pcv_log_state.module_levels, domain);
        g_mutex_unlock(&g_pcv_log_state.module_levels_mu);
        if (val) {
            PcvLogLevel module_level = (PcvLogLevel)GPOINTER_TO_INT(val);
            return pcv_level >= module_level;
        }
    }


    return pcv_level >= g_pcv_log_state.global_level;
}























static gboolean
_is_libvirt_internal_noise(const gchar *domain, const gchar *message)
{

    if (domain != NULL) return FALSE;
    if (!message)       return FALSE;





    static gint show = -1;
    if (G_UNLIKELY(show < 0)) {
        const gchar *e = g_getenv("PURECVISOR_LIBVIRT_NOISE");
        show = (e && e[0] == '1') ? 1 : 0;
    }
    if (show) return FALSE;


    static const gchar * const NOISE_PREFIXES[] = {

        "Add handle ",      "Update handle ",      "Remove handle ",
        "Add timeout ",     "Update timeout ",     "Remove timeout ",

        "Dispatch handler ","Dispatch timeout ",

        "Close GVirConnection",
        NULL
    };
    for (int i = 0; NOISE_PREFIXES[i]; i++) {
        if (g_str_has_prefix(message, NOISE_PREFIXES[i]))
            return TRUE;
    }
    return FALSE;
}



















static void
_pcv_log_handler(const gchar    *log_domain,
                 GLogLevelFlags  log_level,
                 const gchar    *message,
                 gpointer        user_data __attribute__((unused)))
{

    if (_is_libvirt_internal_noise(log_domain, message))
        return;


    if (!_should_log(log_domain, log_level))
        return;

    gchar *ts      = _iso8601_now();
    const gchar *lvl = _level_str(log_level);
    const gchar *req = pcv_log_req_id_get();
    const gchar *dom = log_domain ? log_domain : "purecvisor";


    gchar *msg_esc = _json_escape(message);
    gchar *dom_esc = _json_escape(dom);
    gchar *req_esc = _json_escape(req);


    gchar *line = g_strdup_printf(
        "{\"t\":\"%s\",\"lvl\":\"%s\",\"dom\":\"%s\",\"req\":\"%s\",\"msg\":\"%s\"}",
        ts, lvl, dom_esc, req_esc, msg_esc);

    if (g_pcv_log_state.use_journal) {

        fprintf(stderr, "%s%s\n", _journal_prefix(log_level), line);
    } else {

        fprintf(stderr, "%s\n", line);
    }
    fflush(stderr);

    g_free(line);
    g_free(msg_esc);
    g_free(dom_esc);
    g_free(req_esc);
    g_free(ts);





    if (log_level & G_LOG_LEVEL_ERROR)
        g_abort();
}























void
pcv_log_init(void)
{





    const gchar *js = g_getenv("JOURNAL_STREAM");
    g_pcv_log_state.use_journal = (js && js[0] != '\0');


    if (g_mkdir_with_parents(AUDIT_LOG_DIR, 0750) == 0) {
        g_pcv_log_state.audit_fp = fopen(AUDIT_LOG_PATH, "a");
        if (!g_pcv_log_state.audit_fp)
            fprintf(stderr, "[pcv_log] Cannot open audit log %s: %s\n",
                    AUDIT_LOG_PATH, g_strerror(errno));
    }
    g_mutex_init(&g_pcv_log_state.audit_mutex);


    g_pcv_log_state.global_level = PCV_LOG_LEVEL_INFO;
    g_pcv_log_state.module_levels = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, NULL);
    g_mutex_init(&g_pcv_log_state.module_levels_mu);


    g_log_set_default_handler(_pcv_log_handler, NULL);








    g_pcv_log_state.initialized = TRUE;


    _pcv_log(G_LOG_LEVEL_MESSAGE, "pcv_log",
             "Logging initialized (journal=%s, audit=%s)",
             g_pcv_log_state.use_journal ? "yes" : "no",
             g_pcv_log_state.audit_fp   ? AUDIT_LOG_PATH : "stderr-fallback");
}







void
pcv_log_shutdown(void)
{
    if (g_pcv_log_state.audit_fp) {
        fflush(g_pcv_log_state.audit_fp);
        fclose(g_pcv_log_state.audit_fp);
        g_pcv_log_state.audit_fp = NULL;
    }
    if (g_pcv_log_state.initialized) {
        g_mutex_clear(&g_pcv_log_state.audit_mutex);
        if (g_pcv_log_state.module_levels) {
            g_hash_table_destroy(g_pcv_log_state.module_levels);
            g_pcv_log_state.module_levels = NULL;
        }
        g_mutex_clear(&g_pcv_log_state.module_levels_mu);
        g_pcv_log_state.initialized = FALSE;
    }
}


















void
pcv_log_req_id_set(const gchar *req_id)
{
    g_private_replace(&g_req_id_key,
                      req_id ? g_strdup(req_id) : NULL);
}










const gchar *
pcv_log_req_id_get(void)
{
    const gchar *id = g_private_get(&g_req_id_key);
    return id ? id : "-";
}
















void
_pcv_log(GLogLevelFlags level,
         const gchar   *domain,
         const gchar   *fmt,
         ...)
{
    va_list args;
    va_start(args, fmt);
    gchar *msg = g_strdup_vprintf(fmt, args);
    va_end(args);


    g_log(domain ? domain : G_LOG_DOMAIN, level, "%s", msg);
    g_free(msg);
}

























void
_pcv_log_audit(const gchar *domain,
               const gchar *operation,
               const gchar *target,
               const gchar *fmt,
               ...)
{
    va_list args;
    va_start(args, fmt);
    gchar *msg = g_strdup_vprintf(fmt, args);
    va_end(args);

    gchar *ts      = _iso8601_now();
    const gchar *req = pcv_log_req_id_get();


    gchar *msg_esc = _json_escape(msg);
    gchar *dom_esc = _json_escape(domain   ? domain    : "purecvisor");
    gchar *op_esc  = _json_escape(operation ? operation : "unknown");
    gchar *tgt_esc = _json_escape(target   ? target    : "-");
    gchar *req_esc = _json_escape(req);


    gchar *line = g_strdup_printf(
        "{\"t\":\"%s\",\"lvl\":\"AUDIT\",\"dom\":\"%s\","
        "\"req\":\"%s\",\"op\":\"%s\",\"target\":\"%s\",\"msg\":\"%s\"}",
        ts, dom_esc, req_esc, op_esc, tgt_esc, msg_esc);


    FILE *dest = g_pcv_log_state.audit_fp ? g_pcv_log_state.audit_fp : stderr;


    g_mutex_lock(&g_pcv_log_state.audit_mutex);
    fprintf(dest, "%s\n", line);
    fflush(dest);
    g_mutex_unlock(&g_pcv_log_state.audit_mutex);


    if (g_pcv_log_state.use_journal) {
        fprintf(stderr, "<5>%s\n", line);
    } else {
        fprintf(stderr, "%s\n", line);
    }
    fflush(stderr);


    g_free(line);
    g_free(msg_esc); g_free(dom_esc); g_free(op_esc);
    g_free(tgt_esc); g_free(req_esc);
    g_free(ts);
    g_free(msg);
}






void
pcv_log_set_global_level(PcvLogLevel level)
{
    g_pcv_log_state.global_level = level;
}




PcvLogLevel
pcv_log_get_global_level(void)
{
    return g_pcv_log_state.global_level;
}






void
pcv_log_set_module_level(const gchar *domain, PcvLogLevel level)
{
    if (!domain || !g_pcv_log_state.module_levels)
        return;

    g_mutex_lock(&g_pcv_log_state.module_levels_mu);
    g_hash_table_insert(g_pcv_log_state.module_levels,
                        g_strdup(domain), GINT_TO_POINTER((gint)level));
    g_mutex_unlock(&g_pcv_log_state.module_levels_mu);
}






PcvLogLevel
pcv_log_get_module_level(const gchar *domain)
{
    if (domain && g_pcv_log_state.module_levels) {
        g_mutex_lock(&g_pcv_log_state.module_levels_mu);










        gpointer val = NULL;
        gboolean found = g_hash_table_lookup_extended(
            g_pcv_log_state.module_levels, domain, NULL, &val);
        g_mutex_unlock(&g_pcv_log_state.module_levels_mu);
        if (found)
            return (PcvLogLevel)GPOINTER_TO_INT(val);
    }
    return g_pcv_log_state.global_level;
}















void
pcv_log_load_module_levels(void)
{
    if (!g_pcv_log_state.module_levels)
        return;


    const gchar *global_str = pcv_config_get_string("logging", "level", NULL);
    if (global_str) {
        g_pcv_log_state.global_level = _parse_log_level_str(global_str);
    } else {

        const gchar *daemon_level = pcv_config_get_log_level();
        g_pcv_log_state.global_level = _parse_log_level_str(daemon_level);
    }


    g_mutex_lock(&g_pcv_log_state.module_levels_mu);
    g_hash_table_remove_all(g_pcv_log_state.module_levels);
    g_mutex_unlock(&g_pcv_log_state.module_levels_mu);


    const gchar *section = "logging";



    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, PCV_CONFIG_FILE_PATH,
                                  G_KEY_FILE_NONE, NULL)) {
        gsize n_keys = 0;
        gchar **keys = g_key_file_get_keys(kf, section, &n_keys, NULL);
        if (keys) {
            GString *overrides = g_string_new(NULL);
            for (gsize i = 0; i < n_keys; i++) {

                if (g_strcmp0(keys[i], "level") == 0)
                    continue;

                gchar *val = g_key_file_get_string(kf, section, keys[i], NULL);
                if (val) {
                    g_strstrip(val);
                    PcvLogLevel lvl = _parse_log_level_str(val);

                    g_mutex_lock(&g_pcv_log_state.module_levels_mu);
                    g_hash_table_insert(g_pcv_log_state.module_levels,
                                        g_strdup(keys[i]),
                                        GINT_TO_POINTER((gint)lvl));
                    g_mutex_unlock(&g_pcv_log_state.module_levels_mu);

                    if (overrides->len > 0)
                        g_string_append(overrides, ", ");
                    g_string_append_printf(overrides, "%s=%s",
                                           keys[i], _level_name(lvl));
                    g_free(val);
                }
            }


            _pcv_log(G_LOG_LEVEL_MESSAGE, LOG_DOM,
                     "Log levels: global=%s%s%s",
                     _level_name(g_pcv_log_state.global_level),
                     overrides->len > 0 ? ", overrides: " : "",
                     overrides->len > 0 ? overrides->str : " (no module overrides)");

            g_string_free(overrides, TRUE);
            g_strfreev(keys);
        }
    }
    g_key_file_free(kf);
}












gchar *
pcv_generate_request_id(void)
{
    guint32 r = g_random_int();
    return g_strdup_printf("req-%08x", r);
}

















static void
_trace_fill_random_hex(gchar *buf, gint byte_count)
{
    for (gint i = 0; i < byte_count; i++) {
        guint8 b = (guint8)g_random_int_range(0, 256);
        g_snprintf(buf + i * 2, 3, "%02x", b);
    }
}




static gboolean
_trace_is_valid_hex(const gchar *s, gsize expected_len)
{
    if (!s) return FALSE;
    for (gsize i = 0; i < expected_len; i++) {
        if (!g_ascii_isxdigit(s[i])) return FALSE;
    }
    return s[expected_len] == '-' || s[expected_len] == '\0';
}

PcvTraceContext *
pcv_trace_context_new(void)
{
    PcvTraceContext *ctx = g_new0(PcvTraceContext, 1);
    _trace_fill_random_hex(ctx->trace_id, 16);
    _trace_fill_random_hex(ctx->span_id, 8);
    memset(ctx->parent_id, '0', 16);
    ctx->parent_id[16] = '\0';
    ctx->flags = 0x01;
    return ctx;
}

PcvTraceContext *
pcv_trace_context_parse(const gchar *traceparent)
{











    if (!traceparent || strlen(traceparent) < 55)
        return NULL;


    if (traceparent[0] != '0' || traceparent[1] != '0' || traceparent[2] != '-')
        return NULL;


    if (!_trace_is_valid_hex(traceparent + 3, 32))
        return NULL;


    if (traceparent[35] != '-')
        return NULL;
    if (!_trace_is_valid_hex(traceparent + 36, 16))
        return NULL;


    if (traceparent[52] != '-')
        return NULL;

    PcvTraceContext *ctx = g_new0(PcvTraceContext, 1);


    g_strlcpy(ctx->trace_id, traceparent + 3, 33);


    g_strlcpy(ctx->parent_id, traceparent + 36, 17);


    _trace_fill_random_hex(ctx->span_id, 8);


    gchar flags_str[3] = { traceparent[53], traceparent[54], '\0' };
    ctx->flags = (guint8)g_ascii_strtoull(flags_str, NULL, 16);

    return ctx;
}

gchar *
pcv_trace_context_format(const PcvTraceContext *ctx)
{
    if (!ctx) return g_strdup("00-00000000000000000000000000000000-0000000000000000-00");
    return g_strdup_printf("00-%s-%s-%02x", ctx->trace_id, ctx->span_id, ctx->flags);
}

void
pcv_trace_context_free(PcvTraceContext *ctx)
{
    g_free(ctx);
}
