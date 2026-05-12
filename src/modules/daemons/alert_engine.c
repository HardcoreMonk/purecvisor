
























































































#include "alert_engine.h"
#include "ebpf_telemetry.h"
#include "utils/pcv_config.h"
#include "utils/pcv_log.h"
#include "utils/pcv_spawn.h"
#if PCV_CLUSTER_ENABLED
#include "../cluster/cluster_manager.h"
#endif
#include <libsoup/soup.h>
#include <string.h>
#include <time.h>
#include <sys/statvfs.h>











































#define ALERT_LOG_DOM      "alert_engine"


constexpr int ALERT_CHECK_SEC = 5;


constexpr int ALERT_HISTORY_MAX = 1000;


constexpr int ALERT_DEDUP_WINDOW_SEC = 300;


constexpr int MAX_COMPOSITE_RULES = 8;










typedef enum {
    ALERT_NONE = 0,
    ALERT_WARN,
    ALERT_CRIT
} AlertLevel;








typedef struct {
    gchar      metric[16];
    AlertLevel level;
    gdouble    value;
    gint64     fired_at;
    gchar      message[256];

    gint64     alert_id;
    gboolean   acknowledged;
    gboolean   escalated;
} AlertRecord;

















typedef struct {
    gdouble    warn_thresh;
    gdouble    crit_thresh;
    gint64     warn_since;


    gint64     crit_since;
    gboolean   warn_fired;

    gboolean   crit_fired;
    gint64     last_warn_fired_at;
    gint64     last_crit_fired_at;
} MetricWatch;





typedef enum {
    COMPOSITE_OP_AND = 0,
    COMPOSITE_OP_OR  = 1
} CompositeOp;








typedef struct {
    gboolean    active;
    CompositeOp op;
    gchar       metric_a[16];
    gdouble     thresh_a;
    gchar       metric_b[16];
    gdouble     thresh_b;
    AlertLevel  level;
    gint64      since;
    gboolean    fired;
    gint64      last_fired_at;
} CompositeRule;














static struct {
    GThread       *thread;

    gboolean       running;

    gboolean       enabled;


    gboolean       initialized;



    gchar          webhook_url[512];

    gchar          webhook_secret[128];
    gchar          webhook_crit_url[512];
    gchar          webhook_format[16];

    gchar          telegram_chat_id[64];

    gint           eval_period_sec;


    gint           dedup_window_sec;




    MetricWatch    cpu;
    MetricWatch    mem;
    MetricWatch    disk;
    MetricWatch    data_pool;


    CompositeRule  composite_rules[MAX_COMPOSITE_RULES];
    gint           n_composite_rules;


    AlertRecord    history[ALERT_HISTORY_MAX];

    gint           hist_count;

    gint           hist_idx;

    GMutex         mu;

} G = {0};














static gint64
_mono_now(void)
{
    return g_get_monotonic_time() / G_USEC_PER_SEC;
}
















static volatile gint g_next_alert_id = 1;

static void
_record_alert(const gchar *metric, AlertLevel level, gdouble value, const gchar *msg)
{
    g_mutex_lock(&G.mu);
    AlertRecord *r = &G.history[G.hist_idx];
    g_strlcpy(r->metric, metric, sizeof(r->metric));
    r->level = level;
    r->value = value;
    r->fired_at = (gint64)time(NULL);
    g_strlcpy(r->message, msg, sizeof(r->message));
    r->alert_id = (gint64)g_atomic_int_add(&g_next_alert_id, 1);
    r->acknowledged = FALSE;
    r->escalated = FALSE;
    G.hist_idx = (G.hist_idx + 1) % ALERT_HISTORY_MAX;
    if (G.hist_count < ALERT_HISTORY_MAX) G.hist_count++;
    g_mutex_unlock(&G.mu);
}

void
pcv_alert_record_security_event(const gchar *event_id,
                                const gchar *severity,
                                const gchar *summary)
{
    if (!G.initialized) {
        return;
    }

    AlertLevel level = g_strcmp0(severity, "crit") == 0
        ? ALERT_CRIT
        : ALERT_WARN;
    gchar msg[256];
    g_snprintf(msg, sizeof msg, "[%s] Security event %s: %s",
               severity ? severity : "warn",
               event_id ? event_id : "",
               summary ? summary : "");
    _record_alert("Security", level, 0.0, msg);
}


















constexpr int WEBHOOK_DLQ_MAX     = 1000;
constexpr int WEBHOOK_MAX_RETRIES = 3;


static_assert(ALERT_HISTORY_MAX >= 100, "History buffer too small");
static_assert(MAX_COMPOSITE_RULES <= 16, "Composite rules exceed limit");
static_assert(WEBHOOK_DLQ_MAX >= 100, "DLQ buffer too small");
static_assert(WEBHOOK_MAX_RETRIES >= 1, "Must retry at least once");

static GPtrArray *g_webhook_dlq = nullptr;
static GMutex     g_dlq_mu;









static void
_webhook_dlq_store(const gchar *url, const gchar *payload)
{
    g_mutex_lock(&g_dlq_mu);
    if (!g_webhook_dlq)
        g_webhook_dlq = g_ptr_array_new_with_free_func(g_free);
    if (g_webhook_dlq->len < WEBHOOK_DLQ_MAX) {
        gchar *entry = g_strdup_printf("%s|%s", url, payload);
        g_ptr_array_add(g_webhook_dlq, entry);
        PCV_LOG_WARN(ALERT_LOG_DOM, "Webhook DLQ stored (%u entries)", g_webhook_dlq->len);
    } else {
        PCV_LOG_WARN(ALERT_LOG_DOM, "Webhook DLQ full (%d), dropping entry", WEBHOOK_DLQ_MAX);
    }
    g_mutex_unlock(&g_dlq_mu);
}








static gboolean
_webhook_post(const gchar *url, const gchar *payload)
{
    const gchar *target_url = url ? url : G.webhook_url;
    if (!target_url || !target_url[0]) return FALSE;


    if (!g_str_has_prefix(target_url, "http://") &&
        !g_str_has_prefix(target_url, "https://")) {
        PCV_LOG_WARN(ALERT_LOG_DOM,
                     "Webhook URL rejected (invalid scheme): %.100s", target_url);
        return FALSE;
    }


    if (strstr(target_url, "169.254.") ||
        strstr(target_url, "127.0.0.1") ||
        strstr(target_url, "localhost") ||
        strstr(target_url, "[::1]")) {
        PCV_LOG_WARN(ALERT_LOG_DOM,
                     "Webhook URL rejected (link-local/loopback): %.100s", target_url);
        return FALSE;
    }

    SoupSession *sess = soup_session_new();

    g_object_set(sess, "timeout", 10, NULL);
    SoupMessage *msg = soup_message_new("POST", target_url);
    if (!msg) { g_object_unref(sess); return FALSE; }

    GBytes *body = g_bytes_new(payload, strlen(payload));
    soup_message_set_request_body_from_bytes(msg, "application/json", body);


    if (G.webhook_secret[0]) {
        GHmac *hmac = g_hmac_new(G_CHECKSUM_SHA256, (const guchar *)G.webhook_secret, strlen(G.webhook_secret));
        g_hmac_update(hmac, (const guchar *)payload, strlen(payload));
        gchar *sig = g_strdup_printf("sha256=%s", g_hmac_get_string(hmac));
        SoupMessageHeaders *hdrs = soup_message_get_request_headers(msg);
        soup_message_headers_replace(hdrs, "X-PureCVisor-Signature", sig);
        g_free(sig);
        g_hmac_unref(hmac);
    }


    GBytes *resp = soup_session_send_and_read(sess, msg, NULL, NULL);
    gboolean ok = (resp != nullptr && soup_message_get_status(msg) >= 200
                   && soup_message_get_status(msg) < 300);

    if (resp) g_bytes_unref(resp);
    g_bytes_unref(body);
    g_object_unref(msg);
    g_object_unref(sess);
    return ok;
}









static gboolean
_webhook_post_with_retry(const gchar *url, const gchar *payload, gint max_retries)
{
    const gchar *target_url = url ? url : G.webhook_url;

    for (gint attempt = 0; attempt <= max_retries; attempt++) {
        if (attempt > 0) {
            guint delay_ms = 1000 * (1 << (attempt - 1));
            g_usleep((guint64)delay_ms * 1000);
        }
        if (_webhook_post(target_url, payload)) return TRUE;
        PCV_LOG_WARN(ALERT_LOG_DOM, "Webhook retry %d/%d failed for %.100s",
                     attempt + 1, max_retries, target_url);
    }

    _webhook_dlq_store(target_url, payload);
    return FALSE;
}



typedef struct {
    gchar *url;
    gchar *payload;
} WebhookAsyncCtx;

static void _webhook_async_ctx_free(gpointer p) {
    WebhookAsyncCtx *ctx = p;
    g_free(ctx->url);
    g_free(ctx->payload);
    g_free(ctx);
}

static void
_webhook_async_worker(GTask *task, gpointer src, gpointer data, GCancellable *c)
{
    (void)src; (void)c; (void)task;
    WebhookAsyncCtx *ctx = data;
    _webhook_post_with_retry(ctx->url, ctx->payload, WEBHOOK_MAX_RETRIES);
}






static void
_webhook_post_async(const gchar *url, const gchar *payload)
{
    WebhookAsyncCtx *ctx = g_new0(WebhookAsyncCtx, 1);
    ctx->url = url ? g_strdup(url) : NULL;
    ctx->payload = g_strdup(payload);

    GTask *task = g_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(task, ctx, _webhook_async_ctx_free);
    g_task_run_in_thread(task, _webhook_async_worker);
    g_object_unref(task);
}


























static GHashTable *g_vm_webhook_map = nullptr;
static GMutex g_vm_webhook_mu;







void
pcv_alert_set_vm_webhook(const gchar *vm_name, const gchar *webhook_url)
{
    if (!vm_name) return;
    g_mutex_lock(&g_vm_webhook_mu);
    if (!g_vm_webhook_map)
        g_vm_webhook_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    if (webhook_url && *webhook_url)
        g_hash_table_insert(g_vm_webhook_map, g_strdup(vm_name), g_strdup(webhook_url));
    else
        g_hash_table_remove(g_vm_webhook_map, vm_name);
    g_mutex_unlock(&g_vm_webhook_mu);
}







static const gchar *
_get_vm_webhook(const gchar *metric)
{
    if (!g_vm_webhook_map || !metric) return NULL;
    g_mutex_lock(&g_vm_webhook_mu);
    const gchar *url = g_hash_table_lookup(g_vm_webhook_map, metric);
    g_mutex_unlock(&g_vm_webhook_mu);
    return url;
}

static void
_fire_alert(const gchar *metric, AlertLevel level, gdouble value)
{
    const gchar *sev = (level == ALERT_CRIT) ? "CRIT" : "WARN";
    gchar hostname[64] = "unknown";
    gethostname(hostname, sizeof(hostname));


    gchar ts[32];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);


    gchar msg[256];
    g_snprintf(msg, sizeof(msg), "[%s] %s %.1f%% on %s at %s",
               sev, metric, value, hostname, ts);

    PCV_LOG_WARN(ALERT_LOG_DOM, "%s", msg);
    _record_alert(metric, level, value, msg);


    GString *escaped_msg = g_string_new("");
    for (const char *p = msg; *p; p++) {
        if (*p == '"')       g_string_append(escaped_msg, "\\\"");
        else if (*p == '\\') g_string_append(escaped_msg, "\\\\");
        else if (*p == '\n') g_string_append(escaped_msg, "\\n");
        else                 g_string_append_c(escaped_msg, *p);
    }

    GString *escaped_host = g_string_new("");
    for (const char *p = hostname; *p; p++) {
        if (*p == '"')       g_string_append(escaped_host, "\\\"");
        else if (*p == '\\') g_string_append(escaped_host, "\\\\");
        else                 g_string_append_c(escaped_host, *p);
    }


    gchar payload[1024];
    if (g_strcmp0(G.webhook_format, "slack") == 0) {

        g_snprintf(payload, sizeof(payload),
            "{\"text\":\"PureCVisor Alert: %s\"}", escaped_msg->str);
    } else if (g_strcmp0(G.webhook_format, "telegram") == 0) {

        g_snprintf(payload, sizeof(payload),
            "{\"chat_id\":\"%s\",\"text\":\"PureCVisor Alert: %s\"}",
            G.telegram_chat_id, escaped_msg->str);
    } else {

        g_snprintf(payload, sizeof(payload),
            "{\"severity\":\"%s\",\"metric\":\"%s\",\"value\":%.1f,"
            "\"host\":\"%s\",\"timestamp\":\"%s\"}",
            sev, metric, value, escaped_host->str, ts);
    }
    g_string_free(escaped_msg, TRUE);
    g_string_free(escaped_host, TRUE);


    if (G.webhook_url[0]) {

        const gchar *vm_wh = _get_vm_webhook(metric);

        const gchar *url = vm_wh ? vm_wh
                         : (level == ALERT_CRIT && G.webhook_crit_url[0])
                           ? G.webhook_crit_url : NULL;
        _webhook_post_async(url, payload);
    }
}

































static void
_eval_metric(MetricWatch *w, const gchar *name, gdouble current_pct)
{
    gint64 now = _mono_now();


    if (current_pct >= w->crit_thresh) {

        if (w->crit_since == 0) w->crit_since = now;

        if (!w->crit_fired && (now - w->crit_since) >= G.eval_period_sec) {

            if ((now - w->last_crit_fired_at) >= G.dedup_window_sec) {
                _fire_alert(name, ALERT_CRIT, current_pct);
                w->last_crit_fired_at = now;
            }
            w->crit_fired = TRUE;
        }
    } else {

        w->crit_since = 0;
        w->crit_fired = FALSE;
    }


    if (current_pct >= w->warn_thresh && current_pct < w->crit_thresh) {
        if (w->warn_since == 0) w->warn_since = now;
        if (!w->warn_fired && (now - w->warn_since) >= G.eval_period_sec) {

            if ((now - w->last_warn_fired_at) >= G.dedup_window_sec) {
                _fire_alert(name, ALERT_WARN, current_pct);
                w->last_warn_fired_at = now;
            }
            w->warn_fired = TRUE;
        }
    } else {

        w->warn_since = 0;
        w->warn_fired = FALSE;
    }
}










static gdouble
_get_metric_value(const gchar *name, gdouble cpu, gdouble mem, gdouble disk)
{
    if (g_strcmp0(name, "CPU") == 0)    return cpu;
    if (g_strcmp0(name, "Memory") == 0) return mem;
    if (g_strcmp0(name, "Disk") == 0)   return disk;
    return 0.0;
}











static void
_eval_composite_rules(gdouble cpu_pct, gdouble mem_pct, gdouble disk_pct)
{
    gint64 now = _mono_now();

    for (gint i = 0; i < G.n_composite_rules; i++) {
        CompositeRule *r = &G.composite_rules[i];
        if (!r->active) continue;

        gdouble val_a = _get_metric_value(r->metric_a, cpu_pct, mem_pct, disk_pct);
        gdouble val_b = _get_metric_value(r->metric_b, cpu_pct, mem_pct, disk_pct);

        gboolean cond_a = (val_a >= r->thresh_a);
        gboolean cond_b = (val_b >= r->thresh_b);
        gboolean triggered = (r->op == COMPOSITE_OP_AND)
                              ? (cond_a && cond_b) : (cond_a || cond_b);

        if (triggered) {
            if (r->since == 0) r->since = now;
            if (!r->fired
                && (now - r->since) >= G.eval_period_sec
                && (now - r->last_fired_at) >= G.dedup_window_sec) {
                const gchar *op_str = (r->op == COMPOSITE_OP_AND) ? "AND" : "OR";
                gchar *desc = g_strdup_printf("Composite: %s>=%.0f %s %s>=%.0f",
                    r->metric_a, r->thresh_a, op_str,
                    r->metric_b, r->thresh_b);
                gdouble report_val = (val_a > val_b) ? val_a : val_b;
                _fire_alert(desc, r->level, report_val);
                g_free(desc);
                r->fired = TRUE;
                r->last_fired_at = now;
            }
        } else {
            r->since = 0;
            r->fired = FALSE;
        }
    }
}














static gdouble
_get_disk_percent_path(const gchar *path)
{
    struct statvfs vfs;
    if (statvfs(path, &vfs) != 0) return 0.0;
    guint64 total = (guint64)vfs.f_blocks * vfs.f_frsize;
    guint64 free_b = (guint64)vfs.f_bfree * vfs.f_frsize;
    if (total == 0) return 0.0;
    return 100.0 * (1.0 - (gdouble)free_b / (gdouble)total);
}

static gdouble
_get_disk_percent(void)
{
    return _get_disk_percent_path("/");
}







static gdouble
_get_data_pool_disk_percent(void)
{
    const gchar *pool_path = pcv_config_get_string("storage", "image_dir", "/pcvpool");
    if (!pool_path || !*pool_path) pool_path = "/pcvpool";

    struct statvfs vfs;
    if (statvfs(pool_path, &vfs) != 0) return 0.0;
    return _get_disk_percent_path(pool_path);
}



#define SLA_CHECK_INTERVAL  60
static GHashTable *g_vm_uptime   = nullptr;
static GHashTable *g_vm_downtime = nullptr;
static GMutex g_sla_mu;









static void
_sla_check_vms(void)
{
    g_mutex_lock(&g_sla_mu);
    if (!g_vm_uptime) {
        g_vm_uptime   = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
        g_vm_downtime = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    }
    g_mutex_unlock(&g_sla_mu);

    const gchar *argv[] = {"virsh", "list", "--all", "--name", NULL};
    gchar *out = nullptr;
    if (!pcv_spawn_sync(argv, &out, NULL, NULL) || !out) { g_free(out); return; }

    gchar **vms = g_strsplit(g_strstrip(out), "\n", -1);
    for (gchar **v = vms; *v; v++) {
        if (!**v) continue;
        const gchar *vm = *v;
        const gchar *state_argv[] = {"virsh", "domstate", vm, NULL};
        gchar *state = nullptr;
        gboolean state_ok = pcv_spawn_sync(state_argv, &state, nullptr, nullptr);
        if (!state_ok) {
            PCV_LOG_WARN("ALERT", "SLA: virsh domstate failed for '%s' — skipping this interval", vm);
            g_free(state);
            continue;
        }

        g_mutex_lock(&g_sla_mu);
        gint64 *up   = g_hash_table_lookup(g_vm_uptime, vm);
        gint64 *down = g_hash_table_lookup(g_vm_downtime, vm);
        if (!up)   { up   = g_new0(gint64, 1); g_hash_table_insert(g_vm_uptime,   g_strdup(vm), up); }
        if (!down) { down = g_new0(gint64, 1); g_hash_table_insert(g_vm_downtime, g_strdup(vm), down); }

        if (state && strstr(state, "running"))
            *up += SLA_CHECK_INTERVAL;
        else
            *down += SLA_CHECK_INTERVAL;
        g_mutex_unlock(&g_sla_mu);
        g_free(state);
    }
    g_strfreev(vms);
    g_free(out);
}








JsonObject *
pcv_alert_get_sla(const gchar *vm_name)
{
    JsonObject *obj = json_object_new();
    g_mutex_lock(&g_sla_mu);
    if (g_vm_uptime && vm_name) {
        gint64 *up   = g_hash_table_lookup(g_vm_uptime, vm_name);
        gint64 *down = g_hash_table_lookup(g_vm_downtime, vm_name);
        gint64 u = up ? *up : 0, d = down ? *down : 0;
        gint64 total = u + d;
        gdouble pct = total > 0 ? (100.0 * (gdouble)u / (gdouble)total) : 100.0;
        json_object_set_double_member(obj, "uptime_percent", pct);
        json_object_set_int_member(obj, "uptime_seconds", u);
        json_object_set_int_member(obj, "downtime_seconds", d);
    }
    g_mutex_unlock(&g_sla_mu);
    return obj;
}




















static gpointer
_alert_thread(gpointer data)
{
    (void)data;
    PCV_LOG_INFO(ALERT_LOG_DOM, "Alert engine started (eval=%ds, webhook=%s, format=%s)",
                 G.eval_period_sec,
                 G.webhook_url[0] ? G.webhook_url : "(none)",
                 G.webhook_format);

    while (G.running) {











#if PCV_CLUSTER_ENABLED
        if (pcv_cluster_is_maintenance()) {
            g_usleep((guint64)ALERT_CHECK_SEC * G_USEC_PER_SEC);
            continue;
        }
#endif

        JsonObject *host = pcv_ebpf_telemetry_get_host();
        if (host) {
            gdouble cpu = json_object_get_double_member(host, "cpu_percent");
            gdouble mem = json_object_get_double_member(host, "mem_percent");
            gdouble disk = _get_disk_percent();

            _eval_metric(&G.cpu,  "CPU",    cpu);
            _eval_metric(&G.mem,  "Memory", mem);
            _eval_metric(&G.disk, "Disk",   disk);


            gdouble data_pool_pct = _get_data_pool_disk_percent();
            if (data_pool_pct > 0.0)
                _eval_metric(&G.data_pool, "DataPool", data_pool_pct);


            _eval_composite_rules(cpu, mem, disk);

            json_object_unref(host);
        }


#define ESCALATION_INTERVAL_SEC  600
        {
            gint64 esc_now = (gint64)time(NULL);
            g_mutex_lock(&G.mu);
            for (gint i = 0; i < G.hist_count && i < ALERT_HISTORY_MAX; i++) {
                AlertRecord *r = &G.history[i];
                if (r->level == ALERT_CRIT && !r->acknowledged &&
                    r->fired_at > 0 &&
                    (esc_now - r->fired_at) >= ESCALATION_INTERVAL_SEC &&
                    !r->escalated) {
                    r->escalated = TRUE;
                    gchar esc_msg[512];
                    g_snprintf(esc_msg, sizeof(esc_msg),
                               "[ESCALATION] Unacknowledged CRIT (id=%" G_GINT64_FORMAT "): %s",
                               r->alert_id, r->message);
                    g_mutex_unlock(&G.mu);
                    _webhook_post_async(NULL, esc_msg);
                    PCV_LOG_WARN(ALERT_LOG_DOM, "%s", esc_msg);
                    g_mutex_lock(&G.mu);
                }
            }
            g_mutex_unlock(&G.mu);
        }


        {
            static gint sla_counter = 0;
            if (++sla_counter >= (SLA_CHECK_INTERVAL / ALERT_CHECK_SEC)) {
                sla_counter = 0;
                _sla_check_vms();
            }
        }


        g_usleep(ALERT_CHECK_SEC * G_USEC_PER_SEC);
    }

    PCV_LOG_INFO(ALERT_LOG_DOM, "Alert engine stopped");
    return NULL;
}




























void
pcv_alert_engine_init(void)
{
    g_mutex_init(&G.mu);


    const gchar *enabled_str = pcv_config_get_string("alert", "enabled", "false");
    G.enabled = (g_strcmp0(enabled_str, "true") == 0 || g_strcmp0(enabled_str, "1") == 0);
    if (!G.enabled) {
        PCV_LOG_INFO(ALERT_LOG_DOM, "Alert engine disabled (set [alert] enabled=true to activate)");
        G.initialized = TRUE;
        return;
    }


    G.cpu.warn_thresh  = (gdouble)pcv_config_get_int("alert", "cpu_warn",  80);
    G.cpu.crit_thresh  = (gdouble)pcv_config_get_int("alert", "cpu_crit",  95);
    G.mem.warn_thresh  = (gdouble)pcv_config_get_int("alert", "mem_warn",  85);
    G.mem.crit_thresh  = (gdouble)pcv_config_get_int("alert", "mem_crit",  95);
    G.disk.warn_thresh = (gdouble)pcv_config_get_int("alert", "disk_warn", 80);
    G.disk.crit_thresh = (gdouble)pcv_config_get_int("alert", "disk_crit", 90);

    G.data_pool.warn_thresh = (gdouble)pcv_config_get_int("alert", "data_pool_warn", 80);
    G.data_pool.crit_thresh = (gdouble)pcv_config_get_int("alert", "data_pool_crit", 90);
    G.eval_period_sec  = pcv_config_get_int("alert", "eval_period", 30);
    G.dedup_window_sec = pcv_config_get_int("alert", "dedup_window", ALERT_DEDUP_WINDOW_SEC);


    const gchar *url = pcv_config_get_string("alert", "webhook_url", "");
    g_strlcpy(G.webhook_url, url, sizeof(G.webhook_url));

    const gchar *fmt = pcv_config_get_string("alert", "webhook_format", "generic");
    g_strlcpy(G.webhook_format, fmt, sizeof(G.webhook_format));

    const gchar *chat_id = pcv_config_get_string("alert", "telegram_chat_id", "");
    g_strlcpy(G.telegram_chat_id, chat_id, sizeof(G.telegram_chat_id));


    G.running = TRUE;
    G.initialized = TRUE;
    G.thread = g_thread_new("alert-engine", _alert_thread, NULL);
}














void
pcv_alert_engine_shutdown(void)
{
    if (!G.initialized) return;
    G.running = FALSE;
    if (G.thread) {
        g_thread_join(G.thread);
        G.thread = nullptr;
    }
    g_mutex_clear(&G.mu);
    G.initialized = FALSE;
}





































gboolean
pcv_alert_acknowledge(gint64 alert_id)
{
    g_mutex_lock(&G.mu);
    for (gint i = 0; i < G.hist_count && i < ALERT_HISTORY_MAX; i++) {
        if (G.history[i].alert_id == alert_id) {
            G.history[i].acknowledged = TRUE;
            g_mutex_unlock(&G.mu);
            PCV_LOG_INFO(ALERT_LOG_DOM,
                         "Alert %" G_GINT64_FORMAT " acknowledged", alert_id);
            return TRUE;
        }
    }
    g_mutex_unlock(&G.mu);
    return FALSE;
}

JsonArray *
pcv_alert_engine_get_history(void)
{
    JsonArray *arr = json_array_new();
    g_mutex_lock(&G.mu);


    gint start = (G.hist_count < ALERT_HISTORY_MAX) ? 0 : G.hist_idx;
    for (gint i = 0; i < G.hist_count; i++) {
        gint idx = (start + i) % ALERT_HISTORY_MAX;
        AlertRecord *r = &G.history[idx];
        JsonObject *obj = json_object_new();
        json_object_set_string_member(obj, "metric",  r->metric);
        json_object_set_string_member(obj, "severity", r->level == ALERT_CRIT ? "crit" : "warn");
        json_object_set_double_member(obj, "value",    r->value);
        json_object_set_int_member   (obj, "timestamp",r->fired_at);
        json_object_set_string_member(obj, "message",  r->message);
        json_object_set_int_member   (obj, "alert_id", r->alert_id);
        json_object_set_boolean_member(obj, "acknowledged", r->acknowledged);
        json_object_set_boolean_member(obj, "escalated", r->escalated);
        json_array_add_object_element(arr, obj);
    }
    g_mutex_unlock(&G.mu);
    return arr;
}























JsonObject *
pcv_alert_engine_get_config(void)
{
    JsonObject *obj = json_object_new();
    g_mutex_lock(&G.mu);
    json_object_set_boolean_member(obj, "enabled",        G.enabled);
    json_object_set_int_member   (obj, "cpu_warn",        (gint64)G.cpu.warn_thresh);
    json_object_set_int_member   (obj, "cpu_crit",        (gint64)G.cpu.crit_thresh);
    json_object_set_int_member   (obj, "mem_warn",        (gint64)G.mem.warn_thresh);
    json_object_set_int_member   (obj, "mem_crit",        (gint64)G.mem.crit_thresh);
    json_object_set_int_member   (obj, "disk_warn",       (gint64)G.disk.warn_thresh);
    json_object_set_int_member   (obj, "disk_crit",       (gint64)G.disk.crit_thresh);
    json_object_set_int_member   (obj, "eval_period",     G.eval_period_sec);
    json_object_set_int_member   (obj, "dedup_window",    G.dedup_window_sec);
    json_object_set_string_member(obj, "webhook_url",     G.webhook_url);
    json_object_set_string_member(obj, "webhook_format",  G.webhook_format);
    json_object_set_string_member(obj, "telegram_chat_id",G.telegram_chat_id);
    json_object_set_int_member   (obj, "alert_count",     G.hist_count);


    JsonArray *cr_arr = json_array_new();
    for (gint i = 0; i < G.n_composite_rules; i++) {
        const CompositeRule *r = &G.composite_rules[i];
        JsonObject *cr = json_object_new();
        json_object_set_boolean_member(cr, "active",   r->active);
        json_object_set_string_member (cr, "metric_a", r->metric_a);
        json_object_set_double_member (cr, "thresh_a", r->thresh_a);
        json_object_set_string_member (cr, "op",
                                       r->op == COMPOSITE_OP_AND ? "AND" : "OR");
        json_object_set_string_member (cr, "metric_b", r->metric_b);
        json_object_set_double_member (cr, "thresh_b", r->thresh_b);
        json_object_set_string_member (cr, "level",
                                       r->level == ALERT_CRIT ? "CRIT" : "WARN");
        json_array_add_object_element(cr_arr, cr);
    }
    json_object_set_array_member(obj, "composite_rules", cr_arr);

    g_mutex_unlock(&G.mu);
    return obj;
}























gboolean
pcv_alert_engine_set_config(JsonObject *cfg)
{
    if (!cfg) return FALSE;

    g_mutex_lock(&G.mu);


    if (json_object_has_member(cfg, "enabled"))
        G.enabled = json_object_get_boolean_member(cfg, "enabled");
    if (json_object_has_member(cfg, "cpu_warn"))
        G.cpu.warn_thresh = (gdouble)json_object_get_int_member(cfg, "cpu_warn");
    if (json_object_has_member(cfg, "cpu_crit"))
        G.cpu.crit_thresh = (gdouble)json_object_get_int_member(cfg, "cpu_crit");
    if (json_object_has_member(cfg, "mem_warn"))
        G.mem.warn_thresh = (gdouble)json_object_get_int_member(cfg, "mem_warn");
    if (json_object_has_member(cfg, "mem_crit"))
        G.mem.crit_thresh = (gdouble)json_object_get_int_member(cfg, "mem_crit");
    if (json_object_has_member(cfg, "disk_warn"))
        G.disk.warn_thresh = (gdouble)json_object_get_int_member(cfg, "disk_warn");
    if (json_object_has_member(cfg, "disk_crit"))
        G.disk.crit_thresh = (gdouble)json_object_get_int_member(cfg, "disk_crit");
    if (json_object_has_member(cfg, "eval_period"))
        G.eval_period_sec = (gint)json_object_get_int_member(cfg, "eval_period");
    if (json_object_has_member(cfg, "dedup_window"))
        G.dedup_window_sec = (gint)json_object_get_int_member(cfg, "dedup_window");
    if (json_object_has_member(cfg, "webhook_url"))
        g_strlcpy(G.webhook_url,
                   json_object_get_string_member(cfg, "webhook_url"),
                   sizeof(G.webhook_url));
    if (json_object_has_member(cfg, "webhook_format")) {
        const gchar *fmt = json_object_get_string_member(cfg, "webhook_format");
        if (g_strcmp0(fmt, "slack") == 0 || g_strcmp0(fmt, "telegram") == 0 ||
            g_strcmp0(fmt, "generic") == 0) {
            g_strlcpy(G.webhook_format, fmt, sizeof(G.webhook_format));
        } else {
            PCV_LOG_WARN("alert_engine",
                         "Invalid webhook_format '%s' — keeping current '%s'",
                         fmt, G.webhook_format);
        }
    }
    if (json_object_has_member(cfg, "telegram_chat_id"))
        g_strlcpy(G.telegram_chat_id,
                   json_object_get_string_member(cfg, "telegram_chat_id"),
                   sizeof(G.telegram_chat_id));
    if (json_object_has_member(cfg, "webhook_secret"))
        g_strlcpy(G.webhook_secret,
                   json_object_get_string_member(cfg, "webhook_secret"),
                   sizeof(G.webhook_secret));
    if (json_object_has_member(cfg, "webhook_crit_url"))
        g_strlcpy(G.webhook_crit_url,
                   json_object_get_string_member(cfg, "webhook_crit_url"),
                   sizeof(G.webhook_crit_url));


    if (json_object_has_member(cfg, "composite_rules")) {
        JsonArray *arr = json_object_get_array_member(cfg, "composite_rules");
        guint len = json_array_get_length(arr);
        if (len > MAX_COMPOSITE_RULES) len = MAX_COMPOSITE_RULES;
        G.n_composite_rules = (gint)len;
        memset(G.composite_rules, 0, sizeof(G.composite_rules));
        for (guint i = 0; i < len; i++) {
            JsonObject *elem = json_array_get_object_element(arr, i);
            CompositeRule *r = &G.composite_rules[i];
            r->active = TRUE;
            if (json_object_has_member(elem, "active"))
                r->active = json_object_get_boolean_member(elem, "active");
            if (json_object_has_member(elem, "metric_a"))
                g_strlcpy(r->metric_a,
                           json_object_get_string_member(elem, "metric_a"),
                           sizeof(r->metric_a));
            if (json_object_has_member(elem, "thresh_a"))
                r->thresh_a = json_object_get_double_member(elem, "thresh_a");
            if (json_object_has_member(elem, "op")) {
                const gchar *op = json_object_get_string_member(elem, "op");
                r->op = (g_strcmp0(op, "OR") == 0)
                         ? COMPOSITE_OP_OR : COMPOSITE_OP_AND;
            }
            if (json_object_has_member(elem, "metric_b"))
                g_strlcpy(r->metric_b,
                           json_object_get_string_member(elem, "metric_b"),
                           sizeof(r->metric_b));
            if (json_object_has_member(elem, "thresh_b"))
                r->thresh_b = json_object_get_double_member(elem, "thresh_b");
            if (json_object_has_member(elem, "level")) {
                const gchar *lv = json_object_get_string_member(elem, "level");
                r->level = (g_strcmp0(lv, "CRIT") == 0)
                            ? ALERT_CRIT : ALERT_WARN;
            }
            r->since = 0;
            r->fired = FALSE;
            r->last_fired_at = 0;
        }
        PCV_LOG_INFO(ALERT_LOG_DOM, "Composite rules updated: %d rules", G.n_composite_rules);
    }







    G.cpu.warn_since = G.cpu.crit_since = 0;
    G.cpu.warn_fired = G.cpu.crit_fired = FALSE;
    G.cpu.last_warn_fired_at = G.cpu.last_crit_fired_at = 0;
    G.mem.warn_since = G.mem.crit_since = 0;
    G.mem.warn_fired = G.mem.crit_fired = FALSE;
    G.mem.last_warn_fired_at = G.mem.last_crit_fired_at = 0;
    G.disk.warn_since = G.disk.crit_since = 0;
    G.disk.warn_fired = G.disk.crit_fired = FALSE;
    G.disk.last_warn_fired_at = G.disk.last_crit_fired_at = 0;


    if (G.enabled && !G.running) {
        G.running = TRUE;
        G.thread = g_thread_new("alert-engine", _alert_thread, NULL);
    }

    g_mutex_unlock(&G.mu);

    PCV_LOG_INFO(ALERT_LOG_DOM, "Alert config updated: enabled=%d cpu=%d/%d mem=%d/%d disk=%d/%d eval=%ds dedup=%ds webhook=%s",
                 G.enabled,
                 (int)G.cpu.warn_thresh, (int)G.cpu.crit_thresh,
                 (int)G.mem.warn_thresh, (int)G.mem.crit_thresh,
                 (int)G.disk.warn_thresh, (int)G.disk.crit_thresh,
                 G.eval_period_sec, G.dedup_window_sec, G.webhook_url);
    return TRUE;
}










JsonArray *
pcv_alert_engine_dlq_list(void)
{
    JsonArray *arr = json_array_new();
    g_mutex_lock(&g_dlq_mu);
    if (g_webhook_dlq) {
        for (guint i = 0; i < g_webhook_dlq->len; i++) {
            const gchar *entry = g_ptr_array_index(g_webhook_dlq, i);

            const gchar *sep = strchr(entry, '|');
            JsonObject *obj = json_object_new();
            if (sep) {
                gchar *url = g_strndup(entry, (gsize)(sep - entry));
                json_object_set_string_member(obj, "url", url);
                json_object_set_string_member(obj, "payload", sep + 1);
                g_free(url);
            } else {
                json_object_set_string_member(obj, "url", "");
                json_object_set_string_member(obj, "payload", entry);
            }
            json_object_set_int_member(obj, "index", (gint64)i);
            json_array_add_object_element(arr, obj);
        }
    }
    g_mutex_unlock(&g_dlq_mu);
    return arr;
}








JsonObject *
pcv_alert_engine_dlq_retry(void)
{
    JsonObject *result = json_object_new();
    gint retried = 0, succeeded = 0, failed = 0;

    g_mutex_lock(&g_dlq_mu);
    if (g_webhook_dlq && g_webhook_dlq->len > 0) {

        for (gint i = (gint)g_webhook_dlq->len - 1; i >= 0; i--) {
            const gchar *entry = g_ptr_array_index(g_webhook_dlq, (guint)i);
            const gchar *sep = strchr(entry, '|');
            retried++;
            if (sep) {
                gchar *url = g_strndup(entry, (gsize)(sep - entry));
                if (_webhook_post(url, sep + 1)) {
                    succeeded++;
                    g_ptr_array_remove_index(g_webhook_dlq, (guint)i);
                } else {
                    failed++;
                }
                g_free(url);
            } else {
                failed++;
            }
        }
    }
    g_mutex_unlock(&g_dlq_mu);

    json_object_set_int_member(result, "retried", retried);
    json_object_set_int_member(result, "succeeded", succeeded);
    json_object_set_int_member(result, "failed", failed);
    PCV_LOG_INFO(ALERT_LOG_DOM, "DLQ retry: %d retried, %d succeeded, %d failed",
                 retried, succeeded, failed);
    return result;
}





typedef struct {
    gchar  *metric;
    gint64  until;
    gchar  *reason;
} AlertSilence;

static GPtrArray *g_silences = nullptr;
static GMutex     g_silence_mu;

void
pcv_alert_add_silence(const gchar *metric, gint duration_min, const gchar *reason)
{
    if (!g_silences) {
        g_mutex_init(&g_silence_mu);
        g_silences = g_ptr_array_new_with_free_func(g_free);
    }
    AlertSilence *s = g_new0(AlertSilence, 1);
    s->metric = g_strdup(metric);
    s->until  = g_get_monotonic_time() + (gint64)duration_min * 60 * G_USEC_PER_SEC;
    s->reason = g_strdup(reason ? reason : "");

    g_mutex_lock(&g_silence_mu);
    g_ptr_array_add(g_silences, s);
    g_mutex_unlock(&g_silence_mu);

    PCV_LOG_INFO(ALERT_LOG_DOM, "Alert silenced: metric=%s duration=%dmin reason=%s",
                 metric, duration_min, reason ? reason : "");
}

gboolean
pcv_alert_is_silenced(const gchar *metric)
{
    if (!g_silences || !metric) return FALSE;
    gint64 now = g_get_monotonic_time();
    gboolean silenced = FALSE;

    g_mutex_lock(&g_silence_mu);
    for (guint i = 0; i < g_silences->len; i++) {
        AlertSilence *s = g_ptr_array_index(g_silences, i);
        if (g_strcmp0(s->metric, metric) == 0 && now < s->until) {
            silenced = TRUE;
            break;
        }
    }
    g_mutex_unlock(&g_silence_mu);
    return silenced;
}

JsonArray *
pcv_alert_get_silences(void)
{
    JsonArray *arr = json_array_new();
    if (!g_silences) return arr;
    gint64 now = g_get_monotonic_time();

    g_mutex_lock(&g_silence_mu);
    for (guint i = 0; i < g_silences->len; i++) {
        AlertSilence *s = g_ptr_array_index(g_silences, i);
        if (now < s->until) {
            JsonObject *obj = json_object_new();
            json_object_set_string_member(obj, "metric", s->metric);
            json_object_set_int_member(obj, "remaining_sec",
                (gint64)((s->until - now) / G_USEC_PER_SEC));
            json_object_set_string_member(obj, "reason", s->reason);
            json_array_add_object_element(arr, obj);
        }
    }
    g_mutex_unlock(&g_silence_mu);
    return arr;
}
