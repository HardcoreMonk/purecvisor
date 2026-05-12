































#include "workload_predict.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include "modules/daemons/prometheus_exporter.h"
#include "modules/daemons/ebpf_telemetry.h"
#include "modules/ai/self_healing.h"
#include "utils/pcv_log.h"
























#define PREDICT_LOG_DOM  "predict"
constexpr int    PREDICT_WINDOW  = 60;
constexpr int    PREDICT_HORIZON = 60;
constexpr double EMA_ALPHA       = 0.3;
constexpr int    MAX_NODES       = 8;











typedef struct {
    gchar    ip[32];
    gchar    name[32];

    gdouble  cpu_ring[PREDICT_WINDOW];
    gdouble  cpu_ema;
    gdouble  cpu_trend;
    gdouble  cpu_predicted_5m;

    gdouble  mem_ring[PREDICT_WINDOW];
    gdouble  mem_ema;
    gdouble  mem_trend;
    gdouble  mem_predicted_5m;

    gint     pos;
    gint     count;
    gboolean ema_primed;
} NodeForecast;



static struct {
    NodeForecast nodes[MAX_NODES];
    gint         node_count;
    GMutex       mu;
    gboolean     initialized;
} G = {0};




















static gdouble
_trend_slope(const gdouble *ring, gint pos, gint count)
{
    if (count < 10) return 0.0;

    gint n = count < PREDICT_WINDOW ? count : PREDICT_WINDOW;
    gdouble sum_x = 0, sum_y = 0, sum_xy = 0, sum_xx = 0;

    for (gint i = 0; i < n; i++) {
        gint idx = (pos - n + i + PREDICT_WINDOW) % PREDICT_WINDOW;
        gdouble x = (gdouble)i;
        gdouble y = ring[idx];
        sum_x += x;
        sum_y += y;
        sum_xy += x * y;
        sum_xx += x * x;
    }

    gdouble denom = (gdouble)n * sum_xx - sum_x * sum_x;
    if (fabs(denom) < 1e-9) return 0.0;

    return ((gdouble)n * sum_xy - sum_x * sum_y) / denom;
}













static NodeForecast *
_get_node(const gchar *ip, const gchar *name)
{
    for (gint i = 0; i < G.node_count; i++) {
        if (g_strcmp0(G.nodes[i].ip, ip) == 0)
            return &G.nodes[i];
    }
    if (G.node_count >= MAX_NODES) return NULL;

    NodeForecast *n = &G.nodes[G.node_count++];
    memset(n, 0, sizeof(*n));
    g_strlcpy(n->ip, ip, sizeof(n->ip));
    if (name) g_strlcpy(n->name, name, sizeof(n->name));
    return n;
}

















static void
_push_sample(NodeForecast *n, gdouble cpu, gdouble mem)
{

    n->cpu_ring[n->pos] = cpu;
    n->mem_ring[n->pos] = mem;
    n->pos = (n->pos + 1) % PREDICT_WINDOW;
    if (n->count < PREDICT_WINDOW) n->count++;


    if (!n->ema_primed) {
        n->cpu_ema = cpu;
        n->mem_ema = mem;
        n->ema_primed = TRUE;
    } else {
        n->cpu_ema = EMA_ALPHA * cpu + (1.0 - EMA_ALPHA) * n->cpu_ema;
        n->mem_ema = EMA_ALPHA * mem + (1.0 - EMA_ALPHA) * n->mem_ema;
    }


    n->cpu_trend = _trend_slope(n->cpu_ring, n->pos, n->count);
    n->mem_trend = _trend_slope(n->mem_ring, n->pos, n->count);


    n->cpu_predicted_5m = n->cpu_ema + n->cpu_trend * PREDICT_HORIZON;
    if (n->cpu_predicted_5m < 0) n->cpu_predicted_5m = 0;
    if (n->cpu_predicted_5m > 100) n->cpu_predicted_5m = 100;

    n->mem_predicted_5m = n->mem_ema + n->mem_trend * PREDICT_HORIZON;
    if (n->mem_predicted_5m < 0) n->mem_predicted_5m = 0;
    if (n->mem_predicted_5m > 100) n->mem_predicted_5m = 100;
}




void
pcv_predict_init(void)
{
    g_mutex_init(&G.mu);
    G.initialized = TRUE;
    PCV_LOG_INFO(PREDICT_LOG_DOM, "Workload predictor initialized");
}


void
pcv_predict_shutdown(void)
{
    if (!G.initialized) return;
    G.initialized = FALSE;
    g_mutex_clear(&G.mu);
}













void
pcv_predict_evaluate(void)
{
    if (!G.initialized) return;


    JsonObject *host = pcv_ebpf_telemetry_get_host();
    if (!host) return;

    gdouble cpu = json_object_get_double_member(host, "cpu_percent");
    gdouble mem = json_object_get_double_member(host, "mem_percent");


    json_object_unref(host);

    g_mutex_lock(&G.mu);

    if (cpu >= 0 && mem >= 0) {
        NodeForecast *n = _get_node("local", "local");
        if (n) {
            _push_sample(n, cpu, mem);


            pcv_prom_gauge_set_labels("purecvisor_predict_cpu_5m", "", n->cpu_predicted_5m);
            pcv_prom_gauge_set_labels("purecvisor_predict_mem_5m", "", n->mem_predicted_5m);
            pcv_prom_gauge_set_labels("purecvisor_predict_trend_cpu", "", n->cpu_trend);
            pcv_prom_gauge_set_labels("purecvisor_predict_trend_mem", "", n->mem_trend);


            extern void pcv_ws_broadcast(const gchar *type, const gchar *payload);
            extern gint pcv_ws_client_count(void);
            if (pcv_ws_client_count() > 0) {
                gchar payload[256];
                g_snprintf(payload, sizeof(payload),
                    "{\"cpu\":%.1f,\"mem\":%.1f,"
                    "\"cpu_pred\":%.1f,\"mem_pred\":%.1f,"
                    "\"cpu_trend\":%.4f,\"mem_trend\":%.4f}",
                    cpu, mem, n->cpu_predicted_5m, n->mem_predicted_5m,
                    n->cpu_trend, n->mem_trend);
                pcv_ws_broadcast("forecast", payload);
            }




            gdouble cp = n->cpu_predicted_5m;
            gdouble mp = n->mem_predicted_5m;
            gdouble ct = n->cpu_trend;
            gdouble mt = n->mem_trend;
            g_mutex_unlock(&G.mu);
            pcv_healing_on_prediction(cp, mp, ct, mt);
            return;
        }
    }

    g_mutex_unlock(&G.mu);
}












gchar *
pcv_predict_get_forecast_json(void)
{
    g_mutex_lock(&G.mu);

    GString *buf = g_string_new("[");
    for (gint i = 0; i < G.node_count; i++) {
        NodeForecast *n = &G.nodes[i];
        if (buf->len > 1) g_string_append_c(buf, ',');
        g_string_append_printf(buf,
            "{\"node\":\"%s\",\"cpu_ema\":%.1f,\"mem_ema\":%.1f,"
            "\"cpu_pred\":%.1f,\"mem_pred\":%.1f,"
            "\"cpu_trend\":%.4f,\"mem_trend\":%.4f,"
            "\"samples\":%d}",
            n->name, n->cpu_ema, n->mem_ema,
            n->cpu_predicted_5m, n->mem_predicted_5m,
            n->cpu_trend, n->mem_trend, n->count);
    }
    g_string_append_c(buf, ']');

    g_mutex_unlock(&G.mu);
    return g_string_free(buf, FALSE);
}
