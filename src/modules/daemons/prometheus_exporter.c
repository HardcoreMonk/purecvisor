


































































#include "prometheus_exporter.h"
#include "purecvisor/version.h"
#include "utils/pcv_log.h"
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <errno.h>






















#define PROM_LOG_DOM "prom_export"








typedef struct {
    gchar  name[128];
    gchar  labels[128];


    gdouble value;
    gboolean is_counter;
    gint64  last_update;
} PromMetric;










static struct {
    PromMetric  metrics[PCV_PROM_MAX_METRICS];
    gint        count;
    GMutex      mu;
    gboolean    initialized;
    GHashTable *index;
} G = {0};


















static gint
_find_or_create(const gchar *name, const gchar *labels, gboolean is_counter)
{



    gint64 now = g_get_monotonic_time();
    if (G.index) {

        gchar *key = g_strdup_printf("%s\x01%s", name, labels ? labels : "");
        gpointer stored;
        gboolean found = g_hash_table_lookup_extended(G.index, key, NULL, &stored);
        if (found) {
            gint idx = GPOINTER_TO_INT(stored);
            G.metrics[idx].last_update = now;
            g_free(key);
            return idx;
        }

        if (G.count >= PCV_PROM_MAX_METRICS) {

            static gboolean warned = FALSE;
            if (!warned) {
                warned = TRUE;
                PCV_LOG_WARN(PROM_LOG_DOM,
                    "Metric pool saturated (%d) — dropping new labels. "
                    "Consider increasing MAX_METRICS or reducing label cardinality",
                    PCV_PROM_MAX_METRICS);
            }
            g_free(key);
            return -1;
        }
        gint idx = G.count++;
        g_strlcpy(G.metrics[idx].name,   name,             sizeof(G.metrics[idx].name));
        g_strlcpy(G.metrics[idx].labels, labels ? labels : "", sizeof(G.metrics[idx].labels));
        G.metrics[idx].value      = 0;
        G.metrics[idx].is_counter = is_counter;
        G.metrics[idx].last_update = now;

        g_hash_table_insert(G.index, key, GINT_TO_POINTER(idx));
        return idx;
    }



    for (gint i = 0; i < G.count; i++) {
        if (g_strcmp0(G.metrics[i].name, name) == 0 &&
            g_strcmp0(G.metrics[i].labels, labels) == 0) {
            G.metrics[i].last_update = now;
            return i;
        }
    }
    if (G.count >= PCV_PROM_MAX_METRICS) {
        static gboolean warned = FALSE;
        if (!warned) {
            warned = TRUE;
            PCV_LOG_WARN(PROM_LOG_DOM,
                "Metric pool saturated (%d) — dropping new labels. "
                "Consider increasing MAX_METRICS or reducing label cardinality",
                PCV_PROM_MAX_METRICS);
        }
        return -1;
    }
    gint idx = G.count++;
    g_strlcpy(G.metrics[idx].name,   name,             sizeof(G.metrics[idx].name));
    g_strlcpy(G.metrics[idx].labels, labels ? labels : "", sizeof(G.metrics[idx].labels));
    G.metrics[idx].value      = 0;
    G.metrics[idx].is_counter = is_counter;
    G.metrics[idx].last_update = now;
    return idx;
}






#define PCV_PROM_STALE_TTL_US (5 * 60 * G_USEC_PER_SEC)


#define PCV_PROM_CHECKPOINT_PATH "/var/lib/purecvisor/prom_counters.json"
#define PCV_PROM_CHECKPOINT_INTERVAL_SEC 60
static guint g_checkpoint_timer = 0;








static void
_counters_save_unlocked(void)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "saved_at");
    json_builder_add_int_value(b, (gint64)g_get_real_time() / G_USEC_PER_SEC);
    json_builder_set_member_name(b, "counters");
    json_builder_begin_array(b);
    for (gint i = 0; i < G.count; i++) {
        if (!G.metrics[i].is_counter) continue;
        if (G.metrics[i].value <= 0.0) continue;
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "name");
        json_builder_add_string_value(b, G.metrics[i].name);
        json_builder_set_member_name(b, "labels");
        json_builder_add_string_value(b, G.metrics[i].labels);
        json_builder_set_member_name(b, "value");
        json_builder_add_double_value(b, G.metrics[i].value);
        json_builder_end_object(b);
    }
    json_builder_end_array(b);
    json_builder_end_object(b);

    JsonNode *root = json_builder_get_root(b);
    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, root);
    gchar *data = json_generator_to_data(gen, NULL);
    json_node_free(root);
    g_object_unref(gen);
    g_object_unref(b);



    if (g_mkdir_with_parents("/var/lib/purecvisor", 0755) != 0) {
        PCV_LOG_WARN(PROM_LOG_DOM,
            "Cannot create checkpoint dir /var/lib/purecvisor: %s",
            g_strerror(errno));
        g_free(data);
        return;
    }
    gchar *tmp_path = g_strdup_printf("%s.tmp", PCV_PROM_CHECKPOINT_PATH);
    GError *werr = NULL;
    if (g_file_set_contents(tmp_path, data, -1, &werr)) {
        if (g_rename(tmp_path, PCV_PROM_CHECKPOINT_PATH) != 0) {
            PCV_LOG_WARN(PROM_LOG_DOM, "checkpoint rename failed: %s",
                         g_strerror(errno));
        }
    } else {
        PCV_LOG_WARN(PROM_LOG_DOM, "checkpoint write failed: %s",
                     werr ? werr->message : "unknown");
        g_clear_error(&werr);
    }
    g_free(tmp_path);
    g_free(data);
}





static void
_counters_load(void)
{
    gchar *content = NULL;
    gsize len = 0;
    if (!g_file_get_contents(PCV_PROM_CHECKPOINT_PATH, &content, &len, NULL)) {
        return;
    }

    JsonParser *jp = json_parser_new();
    if (!json_parser_load_from_data(jp, content, (gssize)len, NULL)) {
        g_object_unref(jp);
        g_free(content);
        PCV_LOG_WARN(PROM_LOG_DOM, "checkpoint parse failed — discarding");
        return;
    }


    JsonNode *root_n = json_parser_get_root(jp);
    if (!root_n || !JSON_NODE_HOLDS_OBJECT(root_n)) {
        PCV_LOG_WARN(PROM_LOG_DOM,
            "checkpoint root is not an object — discarding");
        g_object_unref(jp);
        g_free(content);
        return;
    }
    JsonObject *root = json_node_get_object(root_n);
    if (!json_object_has_member(root, "counters")) {
        g_object_unref(jp);
        g_free(content);
        return;
    }

    JsonArray *arr = json_object_get_array_member(root, "counters");
    guint n = json_array_get_length(arr);
    gint restored = 0;

    for (guint i = 0; i < n; i++) {
        JsonObject *o = json_array_get_object_element(arr, i);
        if (!o) continue;
        const gchar *name = json_object_get_string_member(o, "name");
        const gchar *labels = json_object_get_string_member(o, "labels");
        gdouble value = json_object_get_double_member(o, "value");
        if (!name || !labels) continue;
        gint idx = _find_or_create(name, labels, TRUE);
        if (idx >= 0) {
            G.metrics[idx].value = value;
            restored++;
        }
    }

    g_object_unref(jp);
    g_free(content);
    PCV_LOG_INFO(PROM_LOG_DOM, "Counter checkpoint restored (%d entries)",
                 restored);
}





static gboolean
_checkpoint_timer_cb(gpointer data __attribute__((unused)))
{

    if (!g_atomic_int_get(&G.initialized)) return G_SOURCE_REMOVE;
    g_mutex_lock(&G.mu);
    _counters_save_unlocked();
    g_mutex_unlock(&G.mu);
    return G_SOURCE_CONTINUE;
}

static gint
_sweep_stale_gauges(void)
{
    gint64 now = g_get_monotonic_time();
    gint removed = 0;

    for (gint i = G.count - 1; i >= 0; i--) {
        if (G.metrics[i].is_counter) continue;
        if (now - G.metrics[i].last_update < PCV_PROM_STALE_TTL_US) continue;

        if (!strstr(G.metrics[i].labels, "vm=") &&
            !strstr(G.metrics[i].labels, "vm_id=") &&
            !strstr(G.metrics[i].labels, "vm_name=")) continue;


        if (G.index) {
            gchar *del_key = g_strdup_printf("%s\x01%s",
                                             G.metrics[i].name,
                                             G.metrics[i].labels);
            g_hash_table_remove(G.index, del_key);
            g_free(del_key);
        }

        if (i < G.count - 1) {

            if (G.index) {
                gchar *moved_key = g_strdup_printf("%s\x01%s",
                                                   G.metrics[G.count - 1].name,
                                                   G.metrics[G.count - 1].labels);
                g_hash_table_insert(G.index, moved_key, GINT_TO_POINTER(i));

            }
            G.metrics[i] = G.metrics[G.count - 1];
        }
        G.count--;
        removed++;
    }
    return removed;
}









void pcv_prom_init(void)
{
    g_mutex_init(&G.mu);
    G.count = 0;

    g_atomic_int_set(&G.initialized, TRUE);







    G.index = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);


    g_mutex_lock(&G.mu);
    gchar *info_labels = g_strdup_printf("version=\"%s\"", PCV_PRODUCT_VERSION);
    gint idx = _find_or_create("purecvisor_info", info_labels, FALSE);
    g_free(info_labels);
    if (idx >= 0) G.metrics[idx].value = 1;



    _counters_load();
    g_mutex_unlock(&G.mu);


    g_checkpoint_timer = g_timeout_add_seconds(
        PCV_PROM_CHECKPOINT_INTERVAL_SEC, _checkpoint_timer_cb, NULL);

    PCV_LOG_INFO(PROM_LOG_DOM,
                 "Prometheus exporter initialized (counter checkpoint=%ds)",
                 PCV_PROM_CHECKPOINT_INTERVAL_SEC);
}







void pcv_prom_shutdown(void)
{



    if (g_checkpoint_timer > 0) {
        g_source_remove(g_checkpoint_timer);
        g_checkpoint_timer = 0;
    }
    if (g_atomic_int_get(&G.initialized)) {
        g_mutex_lock(&G.mu);
        _counters_save_unlocked();
        g_mutex_unlock(&G.mu);
    }

    g_atomic_int_set(&G.initialized, FALSE);


    if (G.index) {
        g_hash_table_destroy(G.index);
        G.index = NULL;
    }
    g_mutex_clear(&G.mu);
}

















void pcv_prom_inc(const gchar *name, const gchar *label_key, const gchar *label_val)
{
    if (!G.initialized) return;
    gchar labels[128];
    g_snprintf(labels, sizeof(labels), "%s=\"%s\"",
               label_key ? label_key : "", label_val ? label_val : "");

    g_mutex_lock(&G.mu);
    gint idx = _find_or_create(name, labels, TRUE);
    if (idx >= 0) G.metrics[idx].value += 1.0;
    g_mutex_unlock(&G.mu);
}













void pcv_prom_gauge_set(const gchar *name, const gchar *label_key,
                         const gchar *label_val, gdouble value)
{
    if (!G.initialized) return;
    gchar labels[128];
    g_snprintf(labels, sizeof(labels), "%s=\"%s\"",
               label_key ? label_key : "", label_val ? label_val : "");

    g_mutex_lock(&G.mu);
    gint idx = _find_or_create(name, labels, FALSE);
    if (idx >= 0) G.metrics[idx].value = value;
    g_mutex_unlock(&G.mu);
}
















void pcv_prom_gauge_set_labels(const gchar *name, const gchar *labels,
                                gdouble value)
{
    if (!G.initialized) return;
    g_mutex_lock(&G.mu);
    gint idx = _find_or_create(name, labels ? labels : "", FALSE);
    if (idx >= 0) G.metrics[idx].value = value;
    g_mutex_unlock(&G.mu);
}










void pcv_prom_rpc_start(const gchar *method)
{
    if (!G.initialized || !method) return;

}















void pcv_prom_rpc_end(const gchar *method, gboolean success, gdouble duration_ms)
{
    if (!G.initialized || !method) return;


    gchar labels[128];
    g_snprintf(labels, sizeof(labels), "method=\"%s\",status=\"%s\"",
               method, success ? "ok" : "error");

    g_mutex_lock(&G.mu);
    gint idx = _find_or_create("purecvisor_rpc_requests_total", labels, TRUE);
    if (idx >= 0) G.metrics[idx].value += 1.0;


    gchar dur_labels[128];
    g_snprintf(dur_labels, sizeof(dur_labels), "method=\"%s\"", method);
    gint didx = _find_or_create("purecvisor_rpc_duration_ms", dur_labels, FALSE);
    if (didx >= 0) G.metrics[didx].value = duration_ms;
    g_mutex_unlock(&G.mu);
}







void pcv_prom_zfs_inflight_lock_observe(const gchar *pool_name,
                                        const gchar *op,
                                        const gchar *result,
                                        gdouble wait_ms)
{
    (void)pool_name;
    if (!G.initialized || !result) return;
    const gchar *safe_op = (op && *op) ? op : "unknown";
    const gchar *safe_result = *result ? result : "unknown";
    if (wait_ms < 0.0) wait_ms = 0.0;

    gchar labels[128];
    g_snprintf(labels, sizeof(labels), "op=\"%s\",result=\"%s\"",
               safe_op, safe_result);

    g_mutex_lock(&G.mu);
    gint idx = _find_or_create(
        "purecvisor_zfs_inflight_lock_acquired_total", labels, TRUE);
    if (idx >= 0) G.metrics[idx].value += 1.0;

    static const gdouble buckets[] = {10.0, 50.0, 100.0, 500.0, 1000.0, 5000.0};
    for (guint i = 0; i < G_N_ELEMENTS(buckets); i++) {
        gchar bucket_labels[160];
        g_snprintf(bucket_labels, sizeof(bucket_labels),
                   "op=\"%s\",result=\"%s\",le=\"%.0f\"",
                   safe_op, safe_result, buckets[i]);
        gint bidx = _find_or_create(
            "purecvisor_zfs_inflight_lock_wait_ms_bucket", bucket_labels, TRUE);
        if (bidx >= 0 && wait_ms <= buckets[i]) G.metrics[bidx].value += 1.0;
    }

    gchar inf_labels[160];
    g_snprintf(inf_labels, sizeof(inf_labels),
               "op=\"%s\",result=\"%s\",le=\"+Inf\"",
               safe_op, safe_result);
    gint inf_idx = _find_or_create(
        "purecvisor_zfs_inflight_lock_wait_ms_bucket", inf_labels, TRUE);
    if (inf_idx >= 0) G.metrics[inf_idx].value += 1.0;

    gint sum_idx = _find_or_create(
        "purecvisor_zfs_inflight_lock_wait_ms_sum", labels, TRUE);
    if (sum_idx >= 0) G.metrics[sum_idx].value += wait_ms;
    gint count_idx = _find_or_create(
        "purecvisor_zfs_inflight_lock_wait_ms_count", labels, TRUE);
    if (count_idx >= 0) G.metrics[count_idx].value += 1.0;
    g_mutex_unlock(&G.mu);
}



















gchar *pcv_prom_render(void)
{
    if (!G.initialized)
        return g_strdup("# purecvisor prometheus exporter not initialized\n");

    GString *buf = g_string_new("");

    g_mutex_lock(&G.mu);

    gint sweeped = _sweep_stale_gauges();
    if (sweeped > 0) {
        PCV_LOG_INFO("prom_exporter",
            "Swept %d stale gauge metrics (VM labels, TTL=5min)", sweeped);
    }















    GHashTable *first_seen = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, NULL);
    for (gint i = 0; i < G.count; i++) {
        PromMetric *m = &G.metrics[i];
        gpointer val;
        gboolean exists = g_hash_table_lookup_extended(
            first_seen, m->name, NULL, &val);
        if (!exists) {
            g_hash_table_insert(first_seen, g_strdup(m->name), GINT_TO_POINTER(i));
            g_string_append_printf(buf, "# TYPE %s %s\n", m->name,
                m->is_counter ? "counter" : "gauge");
        } else {
            gint j = GPOINTER_TO_INT(val);
            if (G.metrics[j].is_counter != m->is_counter) {
                PCV_LOG_WARN(PROM_LOG_DOM,
                    "Metric type mismatch: %s (slot %d=%s, slot %d=%s) — "
                    "using first",
                    m->name, j,
                    G.metrics[j].is_counter ? "counter" : "gauge",
                    i, m->is_counter ? "counter" : "gauge");
            }
        }
        if (m->labels[0] && m->labels[0] != '=')
            g_string_append_printf(buf, "%s{%s} %.6g\n", m->name, m->labels, m->value);
        else
            g_string_append_printf(buf, "%s %.6g\n", m->name, m->value);
    }
    g_hash_table_destroy(first_seen);
    g_mutex_unlock(&G.mu);

    return g_string_free(buf, FALSE);
}
