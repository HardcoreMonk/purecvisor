







































#include "anomaly_detector.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include "modules/daemons/prometheus_exporter.h"
#include "modules/audit/pcv_audit.h"
#include "modules/ai/self_healing.h"
#include "utils/pcv_log.h"




























#define ANOMALY_LOG_DOM   "anomaly"
constexpr int ANOMALY_WINDOW    = 60;
constexpr int ANOMALY_COOLDOWN  = 30;
constexpr int MAX_WATCHED       = 64;
constexpr int MAX_RECENT_EVENTS = 20;


static_assert(ANOMALY_WINDOW >= 10, "Window too small for meaningful Z-Score");
static_assert(MAX_WATCHED >= 1);












typedef struct {
    gchar    name[128];
    gchar    labels[128];
    gdouble  ring[ANOMALY_WINDOW];
    gint     pos;
    gint     count;
    gdouble  sum;
    gdouble  sum_sq;
    gint64   last_alert_us;
    gdouble  threshold;
    gdouble  last_zscore;
} AnomalyMetric;









typedef struct {
    gchar    metric[128];
    gchar    labels[128];
    gdouble  value;
    gdouble  zscore;
    gdouble  mean;
    gint64   timestamp_us;
} AnomalyEvent;



static struct {
    AnomalyMetric  watched[MAX_WATCHED];
    gint           watch_count;
    AnomalyEvent   recent[MAX_RECENT_EVENTS];
    gint           recent_pos;
    gint           recent_count;
    GMutex         mu;
    gboolean       initialized;
    guint64        total_alerts;
    gint           active_anomalies;
} G = {0};












static gdouble
_mean(const AnomalyMetric *m)
{
    if (m->count == 0) return 0.0;
    return m->sum / (gdouble)m->count;
}












static gdouble
_stddev(const AnomalyMetric *m)
{
    if (m->count < 2) return 0.0;
    gdouble n = (gdouble)m->count;
    gdouble variance = (m->sum_sq / n) - (m->sum / n) * (m->sum / n);
    return variance > 0.0 ? sqrt(variance) : 0.0;
}











static gdouble
_zscore(const AnomalyMetric *m, gdouble value)
{
    gdouble sd = _stddev(m);
    if (sd < 1e-9) return 0.0;
    return fabs(value - _mean(m)) / sd;
}
















static gboolean
_push(AnomalyMetric *m, gdouble value)
{

    if (m->count >= ANOMALY_WINDOW) {
        gdouble old = m->ring[m->pos];
        m->sum -= old;
        m->sum_sq -= old * old;
    } else {
        m->count++;
    }


    m->ring[m->pos] = value;
    m->sum += value;
    m->sum_sq += value * value;
    m->pos = (m->pos + 1) % ANOMALY_WINDOW;


    if (m->count < 10) {
        m->last_zscore = 0.0;
        return FALSE;
    }

    gdouble z = _zscore(m, value);
    m->last_zscore = z;

    if (z > m->threshold) {
        gint64 now = g_get_monotonic_time();

        if (now - m->last_alert_us < ANOMALY_COOLDOWN * G_USEC_PER_SEC)
            return FALSE;
        m->last_alert_us = now;
        return TRUE;
    }
    return FALSE;
}











static void
_add_watch(const gchar *name, const gchar *labels, gdouble threshold)
{
    if (G.watch_count >= MAX_WATCHED) return;
    AnomalyMetric *m = &G.watched[G.watch_count++];
    memset(m, 0, sizeof(*m));
    g_strlcpy(m->name, name, sizeof(m->name));
    if (labels) g_strlcpy(m->labels, labels, sizeof(m->labels));
    m->threshold = threshold;
}















static void
_emit_alert(AnomalyMetric *m, gdouble value)
{
    gdouble z = m->last_zscore;
    gdouble mean = _mean(m);

    G.total_alerts++;


    AnomalyEvent *ev = &G.recent[G.recent_pos];
    g_strlcpy(ev->metric, m->name, sizeof(ev->metric));
    g_strlcpy(ev->labels, m->labels, sizeof(ev->labels));
    ev->value = value;
    ev->zscore = z;
    ev->mean = mean;
    ev->timestamp_us = g_get_real_time();
    G.recent_pos = (G.recent_pos + 1) % MAX_RECENT_EVENTS;
    if (G.recent_count < MAX_RECENT_EVENTS) G.recent_count++;


    gchar lbl[256];
    g_snprintf(lbl, sizeof(lbl), "metric=\"%s\"", m->name);
    pcv_prom_gauge_set_labels("purecvisor_anomaly_score", lbl, z);


    {
        extern void pcv_ws_broadcast(const gchar *type, const gchar *payload);
        extern gint pcv_ws_client_count(void);
        if (pcv_ws_client_count() > 0) {
            gchar payload[512];
            g_snprintf(payload, sizeof(payload),
                "{\"metric\":\"%s\",\"labels\":\"%s\",\"value\":%.2f,"
                "\"zscore\":%.2f,\"mean\":%.2f,\"threshold\":%.1f}",
                m->name, m->labels, value, z, mean, m->threshold);
            pcv_ws_broadcast("anomaly", payload);
        }
    }


    {
        gchar detail[256];
        g_snprintf(detail, sizeof(detail),
            "Z=%.2f (threshold=%.1f) value=%.2f mean=%.2f",
            z, m->threshold, value, mean);
        pcv_audit_log("ai-ops", "anomaly_detected", m->name, detail, 0, 0, "local");
    }

    PCV_LOG_WARN(ANOMALY_LOG_DOM,
        "ANOMALY: %s Z=%.2f (>%.1f) val=%.2f mean=%.2f",
        m->name, z, m->threshold, value, mean);




    pcv_healing_on_anomaly(m->name, value, z, m->threshold);
}













void
pcv_anomaly_init(void)
{
    g_mutex_init(&G.mu);
    G.initialized = TRUE;


    _add_watch("purecvisor_host_cpu_percent", "", 2.5);
    _add_watch("purecvisor_host_memory_percent", "", 2.5);
    _add_watch("node_disk_io_time_seconds_total", "", 3.0);
    _add_watch("node_network_receive_errs_total", "", 2.0);
    _add_watch("purecvisor_rpc_duration_ms", "method=\"vm.list\"", 3.0);
    _add_watch("node_vmstat_pswpout", "", 2.5);
    _add_watch("node_pressure_cpu_some_seconds_total", "", 2.0);
    _add_watch("node_hwmon_temp_celsius", "chip=\"coretemp\",sensor=\"temp1\"", 2.0);
    _add_watch("node_hwmon_temp_celsius", "chip=\"k10temp\",sensor=\"temp1\"", 2.0);
    _add_watch("node_nf_conntrack_entries", "", 2.5);

    PCV_LOG_INFO(ANOMALY_LOG_DOM,
        "Anomaly detector initialized — %d metrics watched", G.watch_count);
}


void
pcv_anomaly_shutdown(void)
{
    if (!G.initialized) return;
    G.initialized = FALSE;
    g_mutex_clear(&G.mu);
}















void
pcv_anomaly_evaluate(void)
{
    if (!G.initialized) return;

    g_mutex_lock(&G.mu);

    gint active = 0;

    for (gint i = 0; i < G.watch_count; i++) {
        AnomalyMetric *m = &G.watched[i];




        extern gchar *pcv_prom_render(void);
        static gchar *last_render = NULL;
        static gint64 last_render_time = 0;


        gint64 now = g_get_monotonic_time();
        if (now - last_render_time > 2 * G_USEC_PER_SEC || !last_render) {


            gchar *new_render = pcv_prom_render();
            if (new_render) {
                g_free(last_render);
                last_render = new_render;
                last_render_time = now;
            }
        }

        if (!last_render) continue;


        gdouble value = NAN;
        gchar search_key[256];
        if (m->labels[0]) {
            g_snprintf(search_key, sizeof(search_key), "%s{%s}", m->name, m->labels);
        } else {
            g_snprintf(search_key, sizeof(search_key), "%s ", m->name);
        }

        const gchar *found = strstr(last_render, search_key);
        if (found) {
            const gchar *space = strrchr(search_key, ' ');
            if (!space) space = strchr(found + strlen(m->name), ' ');
            else space = found + strlen(search_key) - 1;
            if (space) {

                const gchar *val_start = found + strlen(search_key);
                if (m->labels[0]) {
                    val_start = strchr(found, '}');
                    if (val_start) val_start++;
                    while (val_start && *val_start == ' ') val_start++;
                }
                if (val_start) value = g_ascii_strtod(val_start, NULL);
            }
        }

        if (isnan(value)) continue;

        gboolean anomaly = _push(m, value);


        gchar lbl[256];
        g_snprintf(lbl, sizeof(lbl), "metric=\"%s\"", m->name);
        pcv_prom_gauge_set_labels("purecvisor_anomaly_score", lbl, m->last_zscore);

        if (anomaly) {
            _emit_alert(m, value);
            active++;
        } else if (m->last_zscore > m->threshold * 0.8) {
            active++;
        }
    }

    G.active_anomalies = active;


    pcv_prom_gauge_set_labels("purecvisor_anomaly_active", "", (gdouble)active);
    pcv_prom_gauge_set_labels("purecvisor_anomaly_alerts_total", "", (gdouble)G.total_alerts);

    g_mutex_unlock(&G.mu);
}















void
pcv_anomaly_reset_baseline(void)
{
    if (!G.initialized) return;

    g_mutex_lock(&G.mu);


    for (gint i = 0; i < G.watch_count; i++) {
        AnomalyMetric *m = &G.watched[i];
        memset(m->ring, 0, sizeof(m->ring));
        m->pos = 0;
        m->count = 0;
        m->sum = 0.0;
        m->sum_sq = 0.0;
        m->last_alert_us = 0;
        m->last_zscore = 0.0;
    }


    memset(G.recent, 0, sizeof(G.recent));
    G.recent_pos = 0;
    G.recent_count = 0;


    G.total_alerts = 0;
    G.active_anomalies = 0;

    g_mutex_unlock(&G.mu);

    g_message("[ANOMALY] Baseline statistics reset — all %d metrics cleared",
              G.watch_count);
}











gchar *
pcv_anomaly_get_recent_json(void)
{
    g_mutex_lock(&G.mu);

    GString *buf = g_string_new("[");
    gint start = (G.recent_count >= MAX_RECENT_EVENTS)
        ? G.recent_pos : 0;
    gint count = G.recent_count;

    for (gint i = 0; i < count; i++) {
        gint idx = (start + count - 1 - i) % MAX_RECENT_EVENTS;
        AnomalyEvent *ev = &G.recent[idx];
        if (ev->timestamp_us == 0) continue;
        if (buf->len > 1) g_string_append_c(buf, ',');
        g_string_append_printf(buf,
            "{\"metric\":\"%s\",\"labels\":\"%s\",\"value\":%.2f,"
            "\"zscore\":%.2f,\"mean\":%.2f,\"ts\":%ld}",
            ev->metric, ev->labels, ev->value,
            ev->zscore, ev->mean, (long)(ev->timestamp_us / G_USEC_PER_SEC));
    }
    g_string_append_c(buf, ']');

    g_mutex_unlock(&G.mu);
    return g_string_free(buf, FALSE);
}
