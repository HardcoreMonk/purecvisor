#include "dpdk_pmd.h"
#include "dpdk_manager.h"
#include "utils/pcv_spawn.h"
#include "utils/pcv_log.h"
#include "utils/pcv_config.h"
#include "modules/core/cpu_allocator.h"
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <string.h>

#define PMD_LOG_DOM          "dpdk_pmd"
#define PMD_CMD_TIMEOUT_SEC  2u
#define PMD_OWNER_VHOST_COND "external_ids:purecvisor-owner=dpdk-vhost"

static struct {
    GMutex            mu;
    PcvPmdCacheState  state;
    GArray           *pmd_cores;
    GArray           *mask_cores;
    const gchar      *source;
    gint64            refreshed_us;
    GHashTable       *last_start;
    gchar            *warned_mode;
    PcvDpdkPmdTestRunner runner;
    gpointer          runner_data;
} P;

typedef struct {
    const gchar *reason;
    gboolean     numa_remote;
} PcvPmdStartRecord;

void pcv_dpdk_pmd_test_set_appctl_runner(PcvDpdkPmdTestRunner fn, gpointer user_data);

static gint
_cmp_guint(gconstpointer a, gconstpointer b)
{
    guint x = *(const guint *)a, y = *(const guint *)b;
    return (x > y) - (x < y);
}

static void
_cores_add(GArray *cores, guint v)
{
    for (guint i = 0; i < cores->len; i++) {
        guint c = g_array_index(cores, guint, i);
        if (c == v) return;
        if (c > v) { g_array_insert_val(cores, i, v); return; }
    }
    g_array_append_val(cores, v);
}

static gboolean
_cores_has(const GArray *cores, guint v)
{
    for (guint i = 0; cores && i < cores->len; i++)
        if (g_array_index(cores, guint, i) == v) return TRUE;
    return FALSE;
}

static gchar *
_pmd_clean(const gchar *raw)
{
    gchar *v = g_strstrip(g_strdup(raw ? raw : ""));
    gsize n = strlen(v);
    if (g_strcmp0(v, "[]") == 0) { g_free(v); return g_strdup(""); }
    if (n >= 2 && v[0] == '"' && v[n - 1] == '"') {
        gchar *inner = g_strndup(v + 1, n - 2);
        g_free(v);
        return inner;
    }
    return v;
}

static gboolean
_pmd_uint(const gchar *text, guint *out)
{
    if (!text || !*text) return FALSE;
    for (const gchar *p = text; *p; p++) if (!g_ascii_isdigit(*p)) return FALSE;
    guint64 v = g_ascii_strtoull(text, NULL, 10);
    if (v > G_MAXINT) return FALSE;
    *out = (guint)v;
    return TRUE;
}

static gboolean
_pmd_run(const gchar * const *argv, gchar **out, GError **error)
{
    g_mutex_lock(&P.mu);
    PcvDpdkPmdTestRunner fn = P.runner;
    gpointer data = P.runner_data;
    g_mutex_unlock(&P.mu);
    if (fn)
        return fn(argv, out, error, data);
    return pcv_spawn_sync_timeout(argv, out, NULL, PMD_CMD_TIMEOUT_SEC, error);
}

static gboolean
_pmd_global_get(const gchar *key, gchar **value_out, GError **error)
{
    gchar *column = g_strdup_printf("other_config:%s", key);
    const gchar *argv[] = {
        "ovs-vsctl", "--timeout=2", "--if-exists", "get", "Open_vSwitch", ".", column, NULL
    };
    gchar *out = NULL;
    gboolean ok = _pmd_run(argv, &out, error);
    if (ok) *value_out = _pmd_clean(out);
    g_free(out);
    g_free(column);
    return ok;
}

void
pcv_dpdk_pmd_free(gpointer data)
{
    PcvDpdkPmd *pmd = data;
    if (!pmd) return;
    g_clear_pointer(&pmd->rxqs, g_ptr_array_unref);
    g_free(pmd);
}

static void
_pmd_rxq_free(gpointer data)
{
    PcvDpdkPmdRxq *rxq = data;
    if (!rxq) return;
    g_free(rxq->port);
    g_free(rxq);
}

static gboolean
_parse_pmd_head(const gchar *line, gint *numa, guint *core)
{
    const gchar *prefix = "pmd thread numa_id ";
    if (!g_str_has_prefix(line, prefix) || !g_str_has_suffix(line, ":")) return FALSE;
    gchar **tok = g_strsplit_set(line + strlen(prefix), " \t:", -1);
    GPtrArray *words = g_ptr_array_new();
    for (guint i = 0; tok[i]; i++) if (*tok[i]) g_ptr_array_add(words, tok[i]);
    guint n = 0, c = 0;
    gboolean ok = words->len == 3 && _pmd_uint(words->pdata[0], &n) &&
                  g_strcmp0(words->pdata[1], "core_id") == 0 && _pmd_uint(words->pdata[2], &c);
    if (ok) { *numa = (gint)n; *core = c; }
    g_ptr_array_unref(words);
    g_strfreev(tok);
    return ok;
}

static PcvDpdkPmdRxq *
_parse_rxq_line(const gchar *line)
{
    gchar **tok = g_strsplit_set(line, " \t", -1);
    GPtrArray *w = g_ptr_array_new();
    for (guint i = 0; tok[i]; i++) if (*tok[i]) g_ptr_array_add(w, tok[i]);
    PcvDpdkPmdRxq *rxq = NULL;
    guint q = 0;
    if (w->len >= 5 && g_strcmp0(w->pdata[0], "port:") == 0 &&
        g_strcmp0(w->pdata[2], "queue-id:") == 0 && _pmd_uint(w->pdata[3], &q) &&
        (g_strcmp0(w->pdata[4], "(enabled)") == 0 || g_strcmp0(w->pdata[4], "(disabled)") == 0)) {
        rxq = g_new0(PcvDpdkPmdRxq, 1);
        rxq->port = g_strdup(w->pdata[1]);
        rxq->qid = q;
        rxq->enabled = g_strcmp0(w->pdata[4], "(enabled)") == 0;
    }
    g_ptr_array_unref(w);
    g_strfreev(tok);
    return rxq;
}

gboolean
pcv_dpdk_pmd_parse_rxq_show(const gchar *text, GPtrArray **pmds_out, GError **error)
{
    GPtrArray *pmds = g_ptr_array_new_with_free_func(pcv_dpdk_pmd_free);
    PcvDpdkPmd *cur = NULL;
    gboolean cur_has_isolated = FALSE, ok = TRUE;
    gchar **lines = g_strsplit(text ? text : "", "\n", -1);
    for (guint i = 0; ok && lines[i]; i++) {
        gchar *line = g_strstrip(lines[i]);
        if (!*line || g_str_has_prefix(line, "Displaying ") || g_str_has_prefix(line, "overhead:"))
            continue;
        gint numa = 0; guint core = 0;
        if (_parse_pmd_head(line, &numa, &core)) {
            if (cur && !cur_has_isolated) { ok = FALSE; break; }
            cur = g_new0(PcvDpdkPmd, 1);
            cur->core = core;
            cur->numa = numa;
            cur->rxqs = g_ptr_array_new_with_free_func(_pmd_rxq_free);
            g_ptr_array_add(pmds, cur);
            cur_has_isolated = FALSE;
        } else if (cur && g_str_has_prefix(line, "isolated")) {
            gchar *colon = strchr(line, ':');
            gchar *v = colon ? g_strstrip(colon + 1) : NULL;
            if (!v) ok = FALSE;
            else if (g_strcmp0(v, "true") == 0) cur->isolated = TRUE;
            else if (g_strcmp0(v, "false") == 0) cur->isolated = FALSE;
            else ok = FALSE;
            cur_has_isolated = TRUE;
        } else if (cur && g_str_has_prefix(line, "port:")) {
            PcvDpdkPmdRxq *rxq = _parse_rxq_line(line);
            if (rxq) g_ptr_array_add(cur->rxqs, rxq);
            else ok = FALSE;
        } else {
            ok = FALSE;
        }
    }
    g_strfreev(lines);
    if (ok && (pmds->len == 0 || (cur && !cur_has_isolated)))
        ok = FALSE;
    if (!ok) {
        g_set_error_literal(error, g_quark_from_static_string("dpdk-pmd"), 1,
                            "pmd-rxq-show output is empty or malformed");
        g_ptr_array_unref(pmds);
        return FALSE;
    }
    *pmds_out = pmds;
    return TRUE;
}

gboolean
pcv_dpdk_pmd_parse_mask(const gchar *mask, GArray **cores_out)
{
    gchar *v = _pmd_clean(mask);
    const gchar *hex = v;
    if (g_str_has_prefix(hex, "0x") || g_str_has_prefix(hex, "0X")) hex += 2;
    GArray *cores = g_array_new(FALSE, FALSE, sizeof(guint));
    gsize n = strlen(hex);
    gboolean ok = TRUE;
    for (gsize pos = 0; ok && pos < n; pos++) {
        gint d = g_ascii_xdigit_value(hex[n - 1 - pos]);
        if (d < 0) { ok = FALSE; break; }
        for (guint b = 0; b < 4; b++)
            if (d & (1 << b)) { guint core = (guint)(pos * 4 + b); g_array_append_val(cores, core); }
    }
    g_free(v);
    if (!ok) { g_array_unref(cores); return FALSE; }
    *cores_out = cores;
    return TRUE;
}

PcvPmdMode
pcv_dpdk_pmd_mode_parse(const gchar *value)
{
    if (!value || g_strcmp0(value, "auto") == 0) return PCV_PMD_MODE_AUTO;
    if (g_strcmp0(value, "off") == 0) return PCV_PMD_MODE_OFF;
    return PCV_PMD_MODE_INVALID;
}

PcvPmdMode
pcv_dpdk_pmd_mode_from_config(void)
{
    const gchar *value = pcv_config_get_string("dpdk", "vhost_pmd_affinity", "auto");
    PcvPmdMode mode = pcv_dpdk_pmd_mode_parse(value);
    if (mode != PCV_PMD_MODE_INVALID) return mode;

    g_mutex_lock(&P.mu);
    gboolean fresh = g_strcmp0(P.warned_mode, value) != 0;
    if (fresh) { g_free(P.warned_mode); P.warned_mode = g_strdup(value); }
    g_mutex_unlock(&P.mu);
    if (fresh)
        PCV_LOG_WARN(PMD_LOG_DOM, "[dpdk] vhost_pmd_affinity=%s is not auto|off — PMD affinity left unchanged",
                     value ? value : "");
    return mode;
}

static const gchar *
_mode_name(PcvPmdMode mode)
{
    return mode == PCV_PMD_MODE_AUTO ? "auto" : mode == PCV_PMD_MODE_OFF ? "off" : "invalid";
}

gboolean
pcv_dpdk_pmd_affinity_owned(const gchar *cur_aff, const gchar *marker)
{
    guint want = 0;
    if (!cur_aff || !*cur_aff || !_pmd_uint(marker, &want)) return FALSE;
    gchar **items = g_strsplit(cur_aff, ",", -1);
    gboolean owned = items[0] != NULL;
    for (guint i = 0; owned && items[i]; i++) {
        gchar **qc = g_strsplit(g_strstrip(items[i]), ":", -1);
        guint q = 0, c = 0;
        owned = g_strv_length(qc) == 2 && _pmd_uint(g_strstrip(qc[0]), &q) &&
                _pmd_uint(g_strstrip(qc[1]), &c) && c == want;
        g_strfreev(qc);
    }
    g_strfreev(items);
    return owned;
}

static GArray *
_pmds_cores(const GPtrArray *pmds)
{
    GArray *cores = g_array_new(FALSE, FALSE, sizeof(guint));
    for (guint i = 0; pmds && i < pmds->len; i++)
        _cores_add(cores, ((const PcvDpdkPmd *)g_ptr_array_index((GPtrArray *)pmds, i))->core);
    return cores;
}

static GArray *
_cache_union_locked(void)
{
    GArray *all = g_array_new(FALSE, FALSE, sizeof(guint));
    for (guint i = 0; P.pmd_cores && i < P.pmd_cores->len; i++)
        _cores_add(all, g_array_index(P.pmd_cores, guint, i));
    for (guint i = 0; P.mask_cores && i < P.mask_cores->len; i++)
        _cores_add(all, g_array_index(P.mask_cores, guint, i));
    return all;
}

void
pcv_dpdk_pmd_cache_update(const GPtrArray *pmds, const GArray *mask_cores, const gchar *source)
{
    if (!pmds && !mask_cores) return;
    GArray *new_pmd = pmds ? _pmds_cores(pmds) : NULL;
    GArray *new_mask = NULL;
    if (mask_cores) {
        new_mask = g_array_sized_new(FALSE, FALSE, sizeof(guint), mask_cores->len);
        g_array_append_vals(new_mask, mask_cores->data, mask_cores->len);
        g_array_sort(new_mask, _cmp_guint);
    }
    GArray *old_pmd = NULL, *old_mask = NULL;
    g_mutex_lock(&P.mu);
    if (new_pmd) { old_pmd = P.pmd_cores; P.pmd_cores = new_pmd; }
    if (new_mask) { old_mask = P.mask_cores; P.mask_cores = new_mask; }
    GArray *all = _cache_union_locked();
    P.state = all->len ? PCV_PMD_CACHE_VALID : PCV_PMD_CACHE_EMPTY;
    P.source = source;
    P.refreshed_us = g_get_real_time();
    g_mutex_unlock(&P.mu);
    g_array_unref(all);
    if (old_pmd) g_array_unref(old_pmd);
    if (old_mask) g_array_unref(old_mask);
}

GArray *
pcv_dpdk_pmd_cache_snapshot(PcvPmdCacheState *state_out)
{
    g_mutex_lock(&P.mu);
    GArray *copy = _cache_union_locked();
    if (state_out) *state_out = P.state;
    g_mutex_unlock(&P.mu);
    return copy;
}

void
pcv_dpdk_pmd_cache_init_from_mask(void)
{
    gchar *mask = NULL;
    GError *error = NULL;
    GArray *cores = NULL;
    if (_pmd_global_get("pmd-cpu-mask", &mask, &error) && pcv_dpdk_pmd_parse_mask(mask, &cores)) {

        if (cores->len) pcv_dpdk_pmd_cache_update(NULL, cores, "mask");
    } else
        PCV_LOG_WARN(PMD_LOG_DOM, "PMD cache: pmd-cpu-mask read failed: %s",
                     error ? error->message : "unparseable mask");
    if (cores) g_array_unref(cores);
    g_clear_error(&error);
    g_free(mask);
}

gboolean
pcv_dpdk_pmd_query(GPtrArray **pmds_out, gboolean *isolate_out, GError **error)
{
    const gchar *show[] = {"ovs-appctl", "-T", "2", "dpif-netdev/pmd-rxq-show", NULL};
    gchar *out = NULL, *isolate = NULL;
    GPtrArray *pmds = NULL;
    gboolean ok = _pmd_run(show, &out, error) &&
                  pcv_dpdk_pmd_parse_rxq_show(out, &pmds, error) &&
                  _pmd_global_get("pmd-rxq-isolate", &isolate, error);
    g_free(out);
    if (!ok) {
        if (pmds) g_ptr_array_unref(pmds);
        g_free(isolate);
        return FALSE;
    }

    *isolate_out = g_strcmp0(isolate, "false") != 0;
    g_free(isolate);
    pcv_dpdk_pmd_cache_update(pmds, NULL, "pmd-rxq-show");
    *pmds_out = pmds;
    return TRUE;
}

static gchar *
_bare_map_get(const gchar *map, const gchar *key)
{
    gsize klen = strlen(key);
    const gchar *p = map ? map : "";
    while (*p) {
        while (*p == ' ' || *p == '{' || *p == '}' || *p == ',' || *p == '\t') p++;
        const gchar *start = p;
        gboolean quoted = FALSE;
        while (*p && (quoted || (*p != ' ' && *p != ',' && *p != '}'))) {
            if (*p == '"') quoted = !quoted;
            p++;
        }
        if (p > start && (gsize)(p - start) > klen && strncmp(start, key, klen) == 0 && start[klen] == '=') {
            gchar *raw = g_strndup(start + klen + 1, (gsize)(p - start) - klen - 1);
            gchar *clean = _pmd_clean(raw);
            g_free(raw);
            return clean;
        }
    }
    return NULL;
}

typedef struct {
    gchar *name;
    gchar *vm;
    gchar *marker;
    gchar *affinity;
    gint   port_numa;
} PcvPmdPortRow;

static void
_port_row_free(gpointer data)
{
    PcvPmdPortRow *row = data;
    g_free(row->name); g_free(row->vm); g_free(row->marker); g_free(row->affinity);
    g_free(row);
}

static GPtrArray *
_parse_port_rows(const gchar *text, guint ncols)
{
    GPtrArray *rows = g_ptr_array_new_with_free_func(_port_row_free);
    gchar **lines = g_strsplit(text ? text : "", "\n", -1);
    PcvPmdPortRow *row = NULL;
    for (guint i = 0; lines[i]; i++) {
        gchar *line = g_strstrip(lines[i]);
        guint pos = i % (ncols + 1);
        if (pos == 0) {
            row = NULL;
            if (!*line) continue;
            row = g_new0(PcvPmdPortRow, 1);
            row->port_numa = -1;
            row->name = g_strdup(line);
            g_ptr_array_add(rows, row);
        } else if (row && pos == 1) {
            row->vm = _bare_map_get(line, "purecvisor-vm");
            row->marker = _bare_map_get(line, "purecvisor-pmd");
        } else if (row && pos == 2) {
            row->affinity = _bare_map_get(line, "pmd-rxq-affinity");
        } else if (row && pos == 3) {
            gchar *numa = _bare_map_get(line, "numa");
            guint n = 0;
            if (_pmd_uint(numa, &n)) row->port_numa = (gint)n;
            g_free(numa);
        }
    }
    g_strfreev(lines);
    return rows;
}

static gboolean
_pmd_op_pinned(const PcvDpdkPmd *pmd, GHashTable *port_markers, const gchar *self_port)
{
    if (!pmd->isolated) return FALSE;
    for (guint i = 0; i < pmd->rxqs->len; i++) {
        const PcvDpdkPmdRxq *rxq = g_ptr_array_index(pmd->rxqs, i);
        if (self_port && g_strcmp0(rxq->port, self_port) == 0) continue;
        const gchar *m = port_markers ? g_hash_table_lookup(port_markers, rxq->port) : NULL;
        guint mc = 0;
        if (!m || !_pmd_uint(m, &mc) || mc != pmd->core) return TRUE;
    }
    return FALSE;
}

static gboolean
_pmd_busy(const PcvDpdkPmd *pmd, const gchar *self_port)
{
    if (!pmd->isolated) return FALSE;
    for (guint i = 0; i < pmd->rxqs->len; i++) {
        const PcvDpdkPmdRxq *rxq = g_ptr_array_index(pmd->rxqs, i);
        if (!self_port || g_strcmp0(rxq->port, self_port) != 0) return TRUE;
    }
    return FALSE;
}

static GArray *
_pmd_reserved(const GPtrArray *pmds, GHashTable *port_markers, const gchar *self_port)
{
    GArray *reserved = g_array_new(FALSE, FALSE, sizeof(guint));
    GHashTable *seen = g_hash_table_new(g_direct_hash, g_direct_equal);
    for (guint i = 0; i < pmds->len; i++) {
        gint node = ((const PcvDpdkPmd *)g_ptr_array_index((GPtrArray *)pmds, i))->numa;
        if (g_hash_table_contains(seen, GINT_TO_POINTER(node + 1))) continue;
        g_hash_table_add(seen, GINT_TO_POINTER(node + 1));
        gint free_core = -1, open_core = -1;
        for (guint j = 0; j < pmds->len; j++) {
            const PcvDpdkPmd *p = g_ptr_array_index((GPtrArray *)pmds, j);
            if (p->numa != node) continue;
            if (!_pmd_busy(p, self_port) && (free_core < 0 || p->core < (guint)free_core))
                free_core = (gint)p->core;
            if (!_pmd_op_pinned(p, port_markers, self_port) && (open_core < 0 || p->core < (guint)open_core))
                open_core = (gint)p->core;
        }
        gint pick = free_core >= 0 ? free_core : open_core;
        if (pick >= 0) _cores_add(reserved, (guint)pick);
    }
    g_hash_table_unref(seen);
    return reserved;
}

static GHashTable *
_rows_marker_map(const GPtrArray *rows)
{
    GHashTable *map = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; rows && i < rows->len; i++) {
        PcvPmdPortRow *row = g_ptr_array_index((GPtrArray *)rows, i);
        if (row->marker && *row->marker) g_hash_table_insert(map, row->name, row->marker);
    }
    return map;
}

static JsonArray *
_cores_json(const GArray *cores)
{
    JsonArray *arr = json_array_new();
    for (guint i = 0; cores && i < cores->len; i++)
        json_array_add_int_element(arr, g_array_index(cores, guint, i));
    return arr;
}

static const gchar *
_cache_state_name(PcvPmdCacheState s)
{
    return s == PCV_PMD_CACHE_VALID ? "valid" : s == PCV_PMD_CACHE_EMPTY ? "empty" : "unknown";
}

static JsonArray *
_status_pmds(const GPtrArray *pmds, GHashTable *markers, gboolean isolate, gboolean admin_view)
{
    JsonArray *arr = json_array_new();
    GArray *reserved = isolate ? _pmd_reserved(pmds, markers, NULL) : g_array_new(FALSE, FALSE, sizeof(guint));
    for (guint i = 0; i < pmds->len; i++) {
        const PcvDpdkPmd *pmd = g_ptr_array_index((GPtrArray *)pmds, i);
        JsonObject *o = json_object_new();
        json_object_set_int_member(o, "core", pmd->core);
        json_object_set_int_member(o, "numa", pmd->numa);
        json_object_set_boolean_member(o, "isolated", pmd->isolated);
        json_object_set_boolean_member(o, "reserved", _cores_has(reserved, pmd->core));
        GHashTable *product = g_hash_table_new(g_str_hash, g_str_equal);
        JsonArray *rxqs = json_array_new();
        for (guint j = 0; j < pmd->rxqs->len; j++) {
            const PcvDpdkPmdRxq *rxq = g_ptr_array_index(pmd->rxqs, j);
            const gchar *m = g_hash_table_lookup(markers, rxq->port);
            guint mc = 0;
            if (m && _pmd_uint(m, &mc) && mc == pmd->core) g_hash_table_add(product, rxq->port);
            gchar *item = g_strdup_printf("%s:%u", rxq->port, rxq->qid);
            json_array_add_string_element(rxqs, item);
            g_free(item);
        }
        json_object_set_int_member(o, "product_ports", g_hash_table_size(product));
        g_hash_table_unref(product);
        if (admin_view) json_object_set_array_member(o, "rxqs", rxqs);
        else json_array_unref(rxqs);
        json_array_add_object_element(arr, o);
    }
    g_array_unref(reserved);
    return arr;
}

static PcvPmdStartRecord
_last_start(const gchar *vm)
{
    PcvPmdStartRecord rec = { "unknown", FALSE };
    g_mutex_lock(&P.mu);
    PcvPmdStartRecord *hit = (P.last_start && vm) ? g_hash_table_lookup(P.last_start, vm) : NULL;
    if (hit) rec = *hit;
    g_mutex_unlock(&P.mu);
    return rec;
}

static JsonArray *
_status_ports(const GPtrArray *rows, const GPtrArray *pmds)
{
    JsonArray *arr = json_array_new();
    for (guint i = 0; rows && i < rows->len; i++) {
        PcvPmdPortRow *row = g_ptr_array_index((GPtrArray *)rows, i);
        JsonObject *o = json_object_new();
        json_object_set_string_member(o, "vm", row->vm ? row->vm : "");
        json_object_set_string_member(o, "port", row->name);
        guint marker = 0;
        gboolean has_marker = _pmd_uint(row->marker, &marker);
        gboolean has_aff = row->affinity && *row->affinity;
        gboolean owned = has_aff && has_marker && pcv_dpdk_pmd_affinity_owned(row->affinity, row->marker);
        const gchar *source = !has_aff ? "none" : owned ? "product" : "operator";
        json_object_set_string_member(o, "source", source);
        if (has_marker) json_object_set_int_member(o, "marker_core", marker);
        else json_object_set_null_member(o, "marker_core");
        PcvPmdStartRecord rec = _last_start(row->vm);
        json_object_set_string_member(o, "last_start_reason", rec.reason);
        json_object_set_boolean_member(o, "numa_remote", rec.numa_remote);
        if (row->port_numa >= 0) json_object_set_int_member(o, "port_numa", row->port_numa);
        else json_object_set_null_member(o, "port_numa");
        GArray *actual = g_array_new(FALSE, FALSE, sizeof(guint));
        for (guint j = 0; pmds && j < pmds->len; j++) {
            const PcvDpdkPmd *pmd = g_ptr_array_index((GPtrArray *)pmds, j);
            for (guint k = 0; k < pmd->rxqs->len; k++)
                if (g_strcmp0(((PcvDpdkPmdRxq *)g_ptr_array_index(pmd->rxqs, k))->port, row->name) == 0)
                    _cores_add(actual, pmd->core);
        }
        gboolean drift = owned && actual->len && !(actual->len == 1 && g_array_index(actual, guint, 0) == marker);
        json_object_set_array_member(o, "actual_cores", _cores_json(actual));
        json_object_set_boolean_member(o, "drift", drift);
        g_array_unref(actual);
        json_array_add_object_element(arr, o);
    }
    return arr;
}

static void
_status_warnings(JsonArray *warnings, const GPtrArray *pmds, gboolean isolate)
{
    if (!isolate || !pmds || !pmds->len) return;
    guint unpinned = 0;
    gboolean local_zero = FALSE;
    for (guint i = 0; i < pmds->len; i++) {
        const PcvDpdkPmd *pmd = g_ptr_array_index((GPtrArray *)pmds, i);
        if (!pmd->isolated) unpinned++;
        gboolean node_has_free = FALSE;
        for (guint j = 0; j < pmds->len; j++) {
            const PcvDpdkPmd *q = g_ptr_array_index((GPtrArray *)pmds, j);
            if (q->numa == pmd->numa && !q->isolated) node_has_free = TRUE;
        }
        if (!node_has_free) local_zero = TRUE;
    }
    if (!unpinned) json_array_add_string_element(warnings, "unpinned-capacity=0");
    if (local_zero) json_array_add_string_element(warnings, "unpinned-local=0");
}

JsonObject *
pcv_dpdk_pmd_status_object(gboolean admin_view)
{
    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "mode", _mode_name(pcv_dpdk_pmd_mode_from_config()));
    GPtrArray *pmds = NULL, *rows = NULL;
    gboolean isolate = TRUE, have = FALSE;
    if (pcv_dpdk_is_available()) {
        GError *error = NULL;
        have = pcv_dpdk_pmd_query(&pmds, &isolate, &error);
        if (!have)
            PCV_LOG_WARN(PMD_LOG_DOM, "dpdk.status PMD query failed: %s", error ? error->message : "unknown");
        g_clear_error(&error);
        const gchar *find[] = {
            "ovs-vsctl", "--timeout=2", "--bare", "--columns=name,external_ids,other_config,status",
            "find", "Interface", PMD_OWNER_VHOST_COND, NULL
        };
        gchar *out = NULL;
        if (_pmd_run(find, &out, NULL)) rows = _parse_port_rows(out, 4);
        g_free(out);
    }
    if (have) json_object_set_boolean_member(obj, "rxq_isolate", isolate);
    else json_object_set_null_member(obj, "rxq_isolate");

    PcvPmdCacheState state = PCV_PMD_CACHE_UNKNOWN;
    GArray *cached = pcv_dpdk_pmd_cache_snapshot(&state);
    JsonObject *cache = json_object_new();
    json_object_set_string_member(cache, "state", _cache_state_name(state));
    g_mutex_lock(&P.mu);
    const gchar *source = P.source;
    gint64 refreshed = P.refreshed_us;
    g_mutex_unlock(&P.mu);
    if (source) json_object_set_string_member(cache, "source", source);
    else json_object_set_null_member(cache, "source");
    if (refreshed) {
        GDateTime *dt = g_date_time_new_from_unix_utc(refreshed / G_USEC_PER_SEC);
        gchar *iso = g_date_time_format_iso8601(dt);
        json_object_set_string_member(cache, "refreshed_at", iso);
        g_free(iso);
        g_date_time_unref(dt);
    } else {
        json_object_set_null_member(cache, "refreshed_at");
    }
    json_object_set_object_member(obj, "pmd_cache", cache);
    json_object_set_array_member(obj, "excluded_cores", _cores_json(cached));
    g_array_unref(cached);

    GHashTable *markers = _rows_marker_map(rows);
    json_object_set_array_member(obj, "pmds",
        have ? _status_pmds(pmds, markers, isolate, admin_view) : json_array_new());
    if (admin_view)
        json_object_set_array_member(obj, "ports", _status_ports(rows, have ? pmds : NULL));
    json_object_set_array_member(obj, "overlap", json_array_new());
    JsonArray *warnings = json_array_new();
    _status_warnings(warnings, have ? pmds : NULL, isolate);
    if (state == PCV_PMD_CACHE_UNKNOWN) json_array_add_string_element(warnings, "pmd-cache-unknown");
    json_object_set_array_member(obj, "warnings", warnings);
    g_hash_table_unref(markers);
    if (rows) g_ptr_array_unref(rows);
    if (pmds) g_ptr_array_unref(pmds);
    return obj;
}

JsonObject *
pcv_dpdk_status(void)
{
    JsonObject *obj = json_object_new();
    gboolean available = pcv_dpdk_is_available();
    json_object_set_boolean_member(obj, "available", available);
    if (!available) {
        json_object_set_int_member(obj, "vdev_count", 0);
        json_object_set_string_member(obj, "pmd_cpu_mask", "");
        json_object_set_string_member(obj, "socket_mem", "");
        return obj;
    }
    gchar *mask = NULL, *smem = NULL, *ports = NULL;
    GArray *cores = NULL;
    if (_pmd_global_get("pmd-cpu-mask", &mask, NULL) && *mask) {
        json_object_set_string_member(obj, "pmd_cpu_mask", mask);
        if (pcv_dpdk_pmd_parse_mask(mask, &cores)) pcv_dpdk_pmd_cache_update(NULL, cores, "mask");
    } else {
        json_object_set_string_member(obj, "pmd_cpu_mask", "0x0");
    }
    json_object_set_string_member(obj, "socket_mem",
        _pmd_global_get("dpdk-socket-mem", &smem, NULL) ? smem : "");
    const gchar *find[] = {"ovs-vsctl", "--timeout=2", "--columns=name,type", "find", "Interface", "type=dpdk", NULL};
    gint vdev_count = 0;
    if (_pmd_run(find, &ports, NULL) && ports) {
        gchar **lines = g_strsplit(ports, "\n", -1);
        for (gint i = 0; lines[i]; i++)
            if (g_str_has_prefix(g_strstrip(lines[i]), "name")) vdev_count++;
        g_strfreev(lines);
    }
    json_object_set_int_member(obj, "vdev_count", vdev_count);
    if (cores) g_array_unref(cores);
    g_free(mask); g_free(smem); g_free(ports);
    return obj;
}

void
pcv_dpdk_pmd_input_clear(PcvDpdkPmdInput *in)
{
    if (!in) return;
    g_clear_pointer(&in->pmds, g_ptr_array_unref);
    g_clear_pointer(&in->host_nodes, g_array_unref);
    *in = (PcvDpdkPmdInput){ .vm_numa = -1, .nq = 1 };
}

void
pcv_dpdk_pmd_plan_clear(PcvDpdkPmdPlan *plan)
{
    if (!plan) return;
    g_free(plan->affinity);
    for (guint i = 0; i < G_N_ELEMENTS(plan->extra_buf); i++) g_free(plan->extra_buf[i]);
    memset(plan, 0, sizeof *plan);
}

static void
_plan_set(PcvDpdkPmdPlan *out, PcvPmdAction action, const gchar *reason, gboolean clear)
{
    out->action = action;
    out->reason = reason;
    out->clear_marker = clear;
    out->changed = clear;
}

static gboolean
_pmd_not_needed(const PcvDpdkPmdInput *in)
{
    GArray *nodes = g_array_new(FALSE, FALSE, sizeof(guint));
    for (guint i = 0; in->host_nodes && i < in->host_nodes->len; i++)
        _cores_add(nodes, g_array_index(in->host_nodes, guint, i));
    for (guint i = 0; i < in->pmds->len; i++)
        _cores_add(nodes, (guint)((PcvDpdkPmd *)g_ptr_array_index(in->pmds, i))->numa);
    if (in->vm_numa >= 0) _cores_add(nodes, (guint)in->vm_numa);
    gboolean not_needed = TRUE;
    for (guint n = 0; not_needed && n < nodes->len; n++) {
        guint local = 0;
        for (guint i = 0; i < in->pmds->len; i++)
            if ((guint)((PcvDpdkPmd *)g_ptr_array_index(in->pmds, i))->numa == g_array_index(nodes, guint, n))
                local++;
        not_needed = (local ? local : in->pmds->len) <= 1;
    }
    g_array_unref(nodes);
    return not_needed;
}

static guint
_pmd_pick(const GArray *eligible, const gchar *prev_marker, GHashTable *port_markers,
          const gchar *self_port)
{
    guint prev = 0;
    if (_pmd_uint(prev_marker, &prev) && _cores_has(eligible, prev)) return prev;
    guint best = g_array_index(eligible, guint, 0), best_count = G_MAXUINT;
    for (guint i = 0; i < eligible->len; i++) {
        guint core = g_array_index(eligible, guint, i), count = 0;
        GHashTableIter it;
        gpointer k, v;
        if (port_markers) {
            g_hash_table_iter_init(&it, port_markers);
            while (g_hash_table_iter_next(&it, &k, &v)) {
                guint m = 0;
                if (g_strcmp0(k, self_port) != 0 && _pmd_uint(v, &m) && m == core) count++;
            }
        }
        if (count < best_count) { best = core; best_count = count; }
    }
    return best;
}

static GArray *
_pmd_eligible(const PcvDpdkPmdInput *in, gint numa, const GArray *reserved,
              GHashTable *port_markers, const gchar *self_port)
{
    GArray *e = g_array_new(FALSE, FALSE, sizeof(guint));
    for (guint i = 0; i < in->pmds->len; i++) {
        const PcvDpdkPmd *pmd = g_ptr_array_index(in->pmds, i);
        if (numa >= 0 && pmd->numa != numa) continue;
        if (_cores_has(reserved, pmd->core) || _pmd_op_pinned(pmd, port_markers, self_port)) continue;
        _cores_add(e, pmd->core);
    }
    return e;
}

static gint
_pmd_numa_of(const PcvDpdkPmdInput *in, guint core)
{
    for (guint i = 0; i < in->pmds->len; i++)
        if (((PcvDpdkPmd *)g_ptr_array_index(in->pmds, i))->core == core)
            return ((PcvDpdkPmd *)g_ptr_array_index(in->pmds, i))->numa;
    return -1;
}

static GPtrArray *
_pmds_copy(const GPtrArray *src)
{
    GPtrArray *dst = g_ptr_array_new_with_free_func(pcv_dpdk_pmd_free);
    for (guint i = 0; src && i < src->len; i++) {
        const PcvDpdkPmd *s = g_ptr_array_index((GPtrArray *)src, i);
        PcvDpdkPmd *d = g_new0(PcvDpdkPmd, 1);
        *d = *s;
        d->rxqs = g_ptr_array_new_with_free_func(_pmd_rxq_free);
        for (guint j = 0; j < s->rxqs->len; j++) {
            const PcvDpdkPmdRxq *r = g_ptr_array_index(s->rxqs, j);
            PcvDpdkPmdRxq *c = g_new0(PcvDpdkPmdRxq, 1);
            *c = *r;
            c->port = g_strdup(r->port);
            g_ptr_array_add(d->rxqs, c);
        }
        g_ptr_array_add(dst, d);
    }
    return dst;
}

static void
_pmd_overlay_pin(GPtrArray *pmds, const gchar *port, guint core)
{
    gboolean known = FALSE;
    for (guint i = 0; i < pmds->len; i++)
        if (((PcvDpdkPmd *)g_ptr_array_index(pmds, i))->core == core) known = TRUE;
    if (!known) return;
    for (guint i = 0; i < pmds->len; i++) {
        PcvDpdkPmd *pmd = g_ptr_array_index(pmds, i);
        if (pmd->core == core) continue;
        gboolean had = FALSE;
        for (guint j = pmd->rxqs->len; j-- > 0;)
            if (g_strcmp0(((PcvDpdkPmdRxq *)g_ptr_array_index(pmd->rxqs, j))->port, port) == 0) {
                g_ptr_array_remove_index(pmd->rxqs, j);
                had = TRUE;
            }
        if (had && pmd->isolated && !pmd->rxqs->len) pmd->isolated = FALSE;
    }
    for (guint i = 0; i < pmds->len; i++) {
        PcvDpdkPmd *pmd = g_ptr_array_index(pmds, i);
        if (pmd->core != core) continue;
        if (!pmd->isolated) g_ptr_array_set_size(pmd->rxqs, 0);
        pmd->isolated = TRUE;
        for (guint j = 0; j < pmd->rxqs->len; j++)
            if (g_strcmp0(((PcvDpdkPmdRxq *)g_ptr_array_index(pmd->rxqs, j))->port, port) == 0) return;
        PcvDpdkPmdRxq *rxq = g_new0(PcvDpdkPmdRxq, 1);
        rxq->port = g_strdup(port);
        rxq->enabled = TRUE;
        g_ptr_array_add(pmd->rxqs, rxq);
    }
}

static GPtrArray *
_pmds_with_markers(const GPtrArray *pmds, GHashTable *port_markers, const gchar *self_port)
{
    GPtrArray *eff = _pmds_copy(pmds);
    GHashTableIter it;
    gpointer k, v;
    guint core = 0;
    if (port_markers) {
        g_hash_table_iter_init(&it, port_markers);
        while (g_hash_table_iter_next(&it, &k, &v))
            if (g_strcmp0(k, self_port) != 0 && _pmd_uint(v, &core)) _pmd_overlay_pin(eff, k, core);
    }
    return eff;
}

static gboolean
_pmd_would_strand(const GPtrArray *pmds, guint core, const gchar *self_port)
{
    gint node = -1;
    guint node_free = 0, global_free = 0;
    for (guint i = 0; i < pmds->len; i++) {
        const PcvDpdkPmd *p = g_ptr_array_index((GPtrArray *)pmds, i);
        if (p->core == core) {
            if (_pmd_busy(p, self_port)) return FALSE;
            node = p->numa;
        }
    }
    for (guint i = 0; i < pmds->len; i++) {
        const PcvDpdkPmd *p = g_ptr_array_index((GPtrArray *)pmds, i);
        if (p->core == core || _pmd_busy(p, self_port)) continue;
        global_free++;
        if (p->numa == node) node_free++;
    }
    return node_free == 0 || global_free == 0;
}

void
pcv_dpdk_pmd_choose(const PcvDpdkPmdInput *in, const gchar *self_port,
                    const gchar *cur_aff, const gchar *prev_marker,
                    GHashTable *port_markers, PcvDpdkPmdPlan *out)
{
    memset(out, 0, sizeof *out);
    out->nq = in->nq ? in->nq : 1;
    gboolean has_aff = cur_aff && *cur_aff, has_marker = prev_marker && *prev_marker;
    gboolean owned = pcv_dpdk_pmd_affinity_owned(cur_aff, prev_marker);

    if (in->mode == PCV_PMD_MODE_INVALID) { _plan_set(out, PCV_PMD_SKIP, "config-invalid", FALSE); return; }

    if (has_aff && !owned) { _plan_set(out, PCV_PMD_OPERATOR, "operator", FALSE); return; }
    if (in->mode == PCV_PMD_MODE_OFF) { _plan_set(out, PCV_PMD_CLEAR_OWNED, "off", has_aff || has_marker); return; }

    if (!in->query_ok || !in->pmds || !in->pmds->len) {
        _plan_set(out, PCV_PMD_SKIP, "pmd-query-failed", FALSE);
        return;
    }

    if (_pmd_not_needed(in)) { _plan_set(out, PCV_PMD_NOT_NEEDED, "not-needed", has_aff || has_marker); return; }

    PcvDpdkPmdInput eff = *in;
    eff.pmds = in->isolate ? _pmds_with_markers(in->pmds, port_markers, self_port) : g_ptr_array_ref(in->pmds);
    gboolean local = FALSE;
    for (guint i = 0; eff.vm_numa >= 0 && i < eff.pmds->len; i++)
        if (((PcvDpdkPmd *)g_ptr_array_index(eff.pmds, i))->numa == eff.vm_numa) local = TRUE;

    GArray *reserved = eff.isolate ? _pmd_reserved(eff.pmds, port_markers, self_port)
                                   : g_array_new(FALSE, FALSE, sizeof(guint));
    GArray *eligible = _pmd_eligible(&eff, local ? eff.vm_numa : -1, reserved, port_markers, self_port);

    if (!eligible->len && local) {
        g_array_unref(eligible);
        eligible = _pmd_eligible(&eff, -1, reserved, port_markers, self_port);
    }
    guint pick = eligible->len ? _pmd_pick(eligible, prev_marker, port_markers, self_port) : 0;

    if (eligible->len && eff.isolate && _pmd_would_strand(eff.pmds, pick, self_port))
        g_array_set_size(eligible, 0);
    if (!eligible->len) {
        _plan_set(out, PCV_PMD_SKIP, "no-eligible-pmd", has_aff || has_marker);
    } else {

        out->core = pick;
        GString *aff = g_string_new(NULL);
        for (guint q = 0; q < out->nq; q++)
            g_string_append_printf(aff, "%s%u:%u", q ? "," : "", q, out->core);
        out->affinity = g_string_free(aff, FALSE);
        out->numa_remote = eff.vm_numa >= 0 && _pmd_numa_of(&eff, out->core) != eff.vm_numa;
        _plan_set(out, PCV_PMD_APPLY, "applied", FALSE);
        guint prev = 0;
        out->changed = !(owned && g_strcmp0(cur_aff, out->affinity) == 0 &&
                         _pmd_uint(prev_marker, &prev) && prev == out->core);
    }
    g_array_unref(reserved);
    g_array_unref(eligible);
    g_ptr_array_unref(eff.pmds);
}

static gboolean
_ovs_iface_value_opt_locked(const gchar *iface, const gchar *column, gchar **value_out)
{
    const gchar *argv[] = {
        "ovs-vsctl", "--timeout=10", "--if-exists", "get", "Interface", iface, column, NULL
    };
    gchar *out = NULL;
    gboolean ok = pcv_dpdk_ovs_run_locked(argv, &out, NULL, NULL);
    if (ok) *value_out = _pmd_clean(out);
    g_free(out);
    return ok;
}

static void
_plan_clear_argv(PcvDpdkPmdPlan *plan, const gchar *port)
{
    plan->extra_buf[2] = g_strdup(port);
    const gchar *tail[] = {
        "--", "remove", "Interface", plan->extra_buf[2], "other_config", "pmd-rxq-affinity",
        "--", "remove", "Interface", plan->extra_buf[2], "external_ids", "purecvisor-pmd"
    };
    for (guint i = 0; i < G_N_ELEMENTS(tail); i++) plan->extra[i] = tail[i];
}

void
pcv_dpdk_pmd_plan_locked(const gchar *port, gboolean port_exists,
                         const PcvDpdkPmdInput *in, PcvDpdkPmdPlan *plan)
{
    gchar *cur = NULL, *prev = NULL, *scan = NULL;
    gboolean read_ok = !port_exists ||
        (_ovs_iface_value_opt_locked(port, "other_config:pmd-rxq-affinity", &cur) &&
         _ovs_iface_value_opt_locked(port, "external_ids:purecvisor-pmd", &prev));
    PcvDpdkPmdInput eff = *in;
    GPtrArray *rows = NULL;
    if (read_ok && in->mode == PCV_PMD_MODE_AUTO) {
        const gchar *find[] = {
            "ovs-vsctl", "--timeout=10", "--bare", "--columns=name,external_ids,other_config",
            "find", "Interface", PMD_OWNER_VHOST_COND, NULL
        };
        if (pcv_dpdk_ovs_run_locked(find, &scan, NULL, NULL)) rows = _parse_port_rows(scan, 3);
        else eff.query_ok = FALSE;
    }
    GHashTable *markers = _rows_marker_map(rows);

    GPtrArray *eff_pmds = NULL;
    if (eff.isolate && eff.pmds) {
        eff_pmds = _pmds_copy(eff.pmds);
        for (guint i = 0; rows && i < rows->len; i++) {
            PcvPmdPortRow *row = g_ptr_array_index(rows, i);
            if (g_strcmp0(row->name, port) == 0 || !row->affinity || !*row->affinity ||
                pcv_dpdk_pmd_affinity_owned(row->affinity, row->marker)) continue;
            gchar **items = g_strsplit(row->affinity, ",", -1);
            for (guint k = 0; items[k]; k++) {
                const gchar *c = strchr(items[k], ':');
                guint core = 0;
                if (c && _pmd_uint(g_strstrip((gchar *)c + 1), &core)) _pmd_overlay_pin(eff_pmds, row->name, core);
            }
            g_strfreev(items);
        }
        eff.pmds = eff_pmds;
    }
    if (read_ok) {
        pcv_dpdk_pmd_choose(&eff, port, cur, prev, markers, plan);
    } else {
        memset(plan, 0, sizeof *plan);
        _plan_set(plan, PCV_PMD_SKIP, "pmd-query-failed", FALSE);
    }
    if (plan->action == PCV_PMD_APPLY) {
        plan->extra_buf[0] = g_strdup_printf("other_config:pmd-rxq-affinity=\"%s\"", plan->affinity);
        plan->extra_buf[1] = g_strdup_printf("external_ids:purecvisor-pmd=%u", plan->core);
        plan->extra[0] = plan->extra_buf[0];
        plan->extra[1] = plan->extra_buf[1];
    } else if (plan->clear_marker && port_exists) {
        _plan_clear_argv(plan, port);
    }
    PCV_LOG_INFO(PMD_LOG_DOM, "PMD affinity %s: %s core=%u nq=%u numa_remote=%d vm_numa=%d cur=%s marker=%s",
                 port, plan->reason, plan->core, plan->nq, plan->numa_remote, in->vm_numa,
                 cur && *cur ? cur : "-", prev && *prev ? prev : "-");
    g_hash_table_unref(markers);
    if (eff_pmds) g_ptr_array_unref(eff_pmds);
    if (rows) g_ptr_array_unref(rows);
    g_free(scan); g_free(cur); g_free(prev);
}

gboolean
pcv_dpdk_pmd_verify_locked(const gchar *port, PcvDpdkPmdPlan *plan, GError **error)
{
    if (plan->action != PCV_PMD_APPLY) return TRUE;
    gchar *aff = NULL, *marker = NULL, *want = g_strdup_printf("%u", plan->core);
    gboolean same = _ovs_iface_value_opt_locked(port, "other_config:pmd-rxq-affinity", &aff) &&
                    _ovs_iface_value_opt_locked(port, "external_ids:purecvisor-pmd", &marker) &&
                    g_strcmp0(aff, plan->affinity) == 0 && g_strcmp0(marker, want) == 0;
    g_free(aff); g_free(marker); g_free(want);
    if (same) return TRUE;
    const gchar *undo[] = {
        "ovs-vsctl", "--timeout=10", "--", "remove", "Interface", port, "other_config", "pmd-rxq-affinity",
        "--", "remove", "Interface", port, "external_ids", "purecvisor-pmd", NULL
    };
    if (!pcv_dpdk_ovs_run_locked(undo, NULL, NULL, error)) return FALSE;
    PCV_LOG_WARN(PMD_LOG_DOM, "PMD affinity %s: post-read mismatch — product keys removed, start continues", port);
    plan->action = PCV_PMD_SKIP;
    plan->reason = "verify-failed";
    plan->clear_marker = TRUE;
    plan->changed = TRUE;
    return TRUE;
}

void
pcv_dpdk_pmd_record_start(const gchar *vm_name, const PcvDpdkPmdPlan *plan)
{
    if (!vm_name || !plan || plan->action == PCV_PMD_KEEP || !plan->reason) return;
    PcvPmdStartRecord *rec = g_new0(PcvPmdStartRecord, 1);
    rec->reason = plan->reason;
    rec->numa_remote = plan->action == PCV_PMD_APPLY && plan->numa_remote;
    gchar *key = g_strdup(vm_name);
    g_mutex_lock(&P.mu);
    if (!P.last_start) P.last_start = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_hash_table_replace(P.last_start, key, rec);
    g_mutex_unlock(&P.mu);
}

gchar *
pcv_dpdk_pmd_audit_detail(const PcvDpdkPmdPlan *plan)
{
    if (!plan || !plan->changed) return NULL;
    if (plan->action == PCV_PMD_APPLY)
        return g_strdup_printf("applied core=%u nq=%u", plan->core, plan->nq);
    if (plan->clear_marker)
        return g_strdup_printf("cleared reason=%s", plan->reason);
    return NULL;
}

static xmlNodePtr
_xml_child(xmlNodePtr parent, const gchar *name)
{
    for (xmlNodePtr n = parent ? parent->children : NULL; n; n = n->next)
        if (n->type == XML_ELEMENT_NODE && !n->ns && xmlStrEqual(n->name, (const xmlChar *)name)) return n;
    return NULL;
}

static void
_input_from_xml(PcvDpdkPmdInput *in, const gchar *xml)
{
    gsize len = xml ? strlen(xml) : 0;
    xmlDocPtr doc = len && len <= G_MAXINT
        ? xmlReadMemory(xml, (gint)len, NULL, NULL, XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING)
        : NULL;
    xmlNodePtr root = doc ? xmlDocGetRootElement(doc) : NULL;
    xmlNodePtr mem = _xml_child(_xml_child(root, "numatune"), "memory");
    xmlChar *mode = mem ? xmlGetProp(mem, (const xmlChar *)"mode") : NULL;
    xmlChar *nodeset = mem ? xmlGetProp(mem, (const xmlChar *)"nodeset") : NULL;
    guint node = 0;
    if (nodeset && (!mode || xmlStrEqual(mode, (const xmlChar *)"strict")) &&
        _pmd_uint((const gchar *)nodeset, &node))
        in->vm_numa = (gint)node;
    xmlFree(mode); xmlFree(nodeset);
    for (xmlNodePtr n = _xml_child(root, "devices") ? _xml_child(root, "devices")->children : NULL; n; n = n->next) {
        if (n->type != XML_ELEMENT_NODE || !xmlStrEqual(n->name, (const xmlChar *)"interface")) continue;
        xmlChar *type = xmlGetProp(n, (const xmlChar *)"type");
        gboolean vhost = type && xmlStrEqual(type, (const xmlChar *)"vhostuser");
        xmlFree(type);
        xmlChar *queues = vhost ? xmlGetProp(_xml_child(n, "driver"), (const xmlChar *)"queues") : NULL;
        guint q = 0;
        if (queues && _pmd_uint((const gchar *)queues, &q) && q >= 1 && q <= 256) in->nq = q;
        xmlFree(queues);
    }
    if (doc) xmlFreeDoc(doc);
}

void
pcv_dpdk_pmd_input_prepare(PcvDpdkPmdInput *in, const gchar *domain_xml)
{
    pcv_dpdk_pmd_input_clear(in);
    in->mode = pcv_dpdk_pmd_mode_from_config();
    _input_from_xml(in, domain_xml);
    gchar *has_cpu = NULL;
    if (g_file_get_contents("/sys/devices/system/node/has_cpu", &has_cpu, NULL, NULL))
        in->host_nodes = cpu_allocator_parse_cpulist(has_cpu);
    g_free(has_cpu);
    if (in->mode != PCV_PMD_MODE_AUTO) return;
    GError *error = NULL;
    in->query_ok = pcv_dpdk_pmd_query(&in->pmds, &in->isolate, &error);
    if (!in->query_ok)
        PCV_LOG_WARN(PMD_LOG_DOM, "PMD query failed — affinity skipped this start: %s",
                     error ? error->message : "unknown");
    g_clear_error(&error);
}

void
pcv_dpdk_pmd_cache_refresh(void)
{
    if (!pcv_dpdk_is_available()) return;
    GPtrArray *pmds = NULL;
    gboolean isolate = TRUE;
    if (pcv_dpdk_pmd_query(&pmds, &isolate, NULL)) g_ptr_array_unref(pmds);

    gchar *mask = NULL;
    GArray *cores = NULL;
    if (_pmd_global_get("pmd-cpu-mask", &mask, NULL) && pcv_dpdk_pmd_parse_mask(mask, &cores))
        pcv_dpdk_pmd_cache_update(NULL, cores, "mask");
    if (cores) g_array_unref(cores);
    g_free(mask);
}

void
pcv_dpdk_pmd_test_set_appctl_runner(PcvDpdkPmdTestRunner fn, gpointer user_data)
{
    g_mutex_lock(&P.mu);
    P.runner = fn;
    P.runner_data = user_data;
    g_clear_pointer(&P.pmd_cores, g_array_unref);
    g_clear_pointer(&P.mask_cores, g_array_unref);
    g_clear_pointer(&P.last_start, g_hash_table_unref);
    P.state = PCV_PMD_CACHE_UNKNOWN;
    P.source = NULL;
    P.refreshed_us = 0;
    g_mutex_unlock(&P.mu);
}
