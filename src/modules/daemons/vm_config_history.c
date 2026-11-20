#include "modules/daemons/vm_config_history.h"

#include <string.h>

#include "modules/core/vm_config_history_store.h"
#include "modules/daemons/prometheus_exporter.h"
#include "modules/virt/vm_config_history_decide.h"
#include "modules/virt/vm_config_history_norm.h"
#include "utils/pcv_config.h"
#include "utils/pcv_log.h"

#define VMCH_LOG "vm-cfg-hist"
#define STATE_SUPPRESS_US (3600LL * G_USEC_PER_SEC)

typedef enum { M_NOTE, M_STARTED, M_UNDEFINED, M_COV_BEGIN, M_COV_END, M_INTENT_OPEN, M_INTENT_CLOSE, M_READY, M_STOP,
               M_SYNC } MsgKind;

typedef struct {
    MsgKind  k;
    gchar   *uuid, *cause, *method, *target, *actor, *actor_tenant, *result, *dev;
    gint64   bytes;
    gint64   at;
    gpointer sync;
} Msg;

typedef struct {
    GMutex   mu;
    GCond    cond;
    gboolean done;
    gint     refs;
} SyncWait;

typedef struct {
    gint64   due;
    gchar   *cause;
    gboolean settle_close;
} Pending;

typedef struct {
    PcvVmCfgRow         *row;
    PcvVmCfgLatestUpdate lu;
    gchar               *current_name;
    gchar               *run_live_digest;
} Retry;

typedef struct {
    gint                      refs;
    GAsyncQueue              *q;
    GThread                  *thread;
    PcvVmCfgHistConfig        cfg;
    gint                      pending_max;
    const PcvVmCfgHistSource *src;
    gpointer                  src_data;
    gint                      stop;
    gint                      fail_writes;
    GMutex                    exit_mu;
    GCond                     exit_cond;
    gboolean                  exited;
    gint                      ext_off;
    gint                      ext_busy;

    PcvVmCfgStore            *store;
    PcvVmCfgIntents          *intents;
    GHashTable               *pending;
    GHashTable               *settles;
    GHashTable               *run_overlay;
    GHashTable               *filled;
    GQueue                    retry;
    gint64                    retry_due;
    gint                      retry_backoff_ms;
    gboolean                  epoch_failed;
    gint64                    epoch_id;
    gboolean                  covered, ready, scanned_once, need_rescan, first_boot;
    const gchar              *rescan_cause;
    gint64                    gap_floor;
    gint64                    next_reconcile, next_heartbeat, next_prune;
    GPtrArray                *syncs;
} Worker;

static GMutex   g_mu;
static Worker  *g_w;
static gchar   *g_state_name;
static gchar   *g_db_path;
static gint     g_retention_days = 365;
static gboolean g_enabled = TRUE;
static guint64  g_counters[7];
static const gchar *const k_counter_names[] = {"captures_written", "captures_unchanged", "captures_failed",
                                               "tenant_unknown", "coalesced", "write_failures", "source_reads"};
enum { C_WRITTEN, C_UNCHANGED, C_FAILED, C_TENANT_UNKNOWN, C_COALESCED, C_WRITE_FAIL, C_READS };
static GMutex      g_fail_mu;
static GHashTable *g_fail_open;
static GQueue      g_fail_closed;

static gint64
_wall(void)
{
    return g_get_real_time() / 1000;
}

static gint64
_mono(void)
{
    return g_get_monotonic_time() / 1000;
}

static GPrivate g_self;

static gboolean
_ext_enter(void)
{
    Worker *w = g_private_get(&g_self);
    if (!w)
        return TRUE;
    g_atomic_int_inc(&w->ext_busy);
    if (g_atomic_int_get(&w->ext_off)) {
        g_atomic_int_add(&w->ext_busy, -1);
        return FALSE;
    }
    return TRUE;
}

static void
_ext_leave(void)
{
    Worker *w = g_private_get(&g_self);
    if (w)
        g_atomic_int_add(&w->ext_busy, -1);
}

static void
_prom_inc(const gchar *name, const gchar *key, const gchar *value)
{
    if (!_ext_enter())
        return;
    pcv_prom_inc(name, key, value);
    _ext_leave();
}

static void
_prom_gauge(const gchar *name, const gchar *labels, gdouble v)
{
    if (!_ext_enter())
        return;
    pcv_prom_gauge_set_labels(name, labels, v);
    _ext_leave();
}

static void
_count(gint idx)
{
    __atomic_fetch_add(&g_counters[idx], 1, __ATOMIC_RELAXED);
    static const gchar *const result[] = {"written", "unchanged", "failed", "tenant_unknown"};
    if (idx <= C_TENANT_UNKNOWN)
        _prom_inc("purecvisor_vm_config_history_captures_total", "result", result[idx]);
    else if (idx == C_COALESCED)
        _prom_inc("purecvisor_vm_config_history_coalesced_total", NULL, NULL);
    else if (idx == C_WRITE_FAIL)
        _prom_inc("purecvisor_vm_config_history_write_failures_total", NULL, NULL);
}

static void
_msg_free(gpointer p)
{
    Msg *m = p;
    g_free(m->uuid);
    g_free(m->cause);
    g_free(m->method);
    g_free(m->target);
    g_free(m->actor);
    g_free(m->actor_tenant);
    g_free(m->result);
    g_free(m->dev);
    g_free(m);
}

static void
_push(Msg *m)
{
    g_mutex_lock(&g_mu);
    if (g_w && !g_atomic_int_get(&g_w->stop))
        g_async_queue_push(g_w->q, m);
    else
        _msg_free(m);
    g_mutex_unlock(&g_mu);
}

static Msg *
_msg(MsgKind k)
{
    Msg *m = g_new0(Msg, 1);
    m->k = k;
    m->at = _wall();
    return m;
}

static void
_set_state(const gchar *state)
{
    static gint64 last_log_us;
    static gchar last_logged[16];
    g_mutex_lock(&g_mu);
    gboolean changed = g_strcmp0(g_state_name, state) != 0;
    g_free(g_state_name);
    g_state_name = g_strdup(state);
    g_mutex_unlock(&g_mu);
    gint64 now = g_get_monotonic_time();
    if (changed && (g_strcmp0(last_logged, state) || now - last_log_us > STATE_SUPPRESS_US)) {
        if (g_strcmp0(state, "ok"))
            PCV_LOG_WARN(VMCH_LOG, "[vm-cfg-hist] store state=%s (repeats suppressed 3600s)", state);
        else
            PCV_LOG_INFO(VMCH_LOG, "[vm-cfg-hist] store state=%s (repeats suppressed 3600s)", state);
        g_strlcpy(last_logged, state, sizeof last_logged);
        last_log_us = now;
    }
    static const gchar *const states[] = {"ok", "disabled", "degraded", "over_cap", "unavailable", NULL};
    for (guint i = 0; states[i]; i++) {
        gchar labels[64];
        pcv_prom_labels_build(labels, sizeof labels, "state", states[i], NULL);
        _prom_gauge("purecvisor_vm_config_history_store_state", labels, !g_strcmp0(states[i], state));
    }
}

static gboolean
_state_is(const gchar *state)
{
    g_mutex_lock(&g_mu);
    gboolean r = !g_strcmp0(g_state_name, state);
    g_mutex_unlock(&g_mu);
    return r;
}

static void
_capture_failed(const gchar *uuid, const gchar *reason)
{
    _count(C_FAILED);
    PCV_LOG_WARN(VMCH_LOG, "[vm-cfg-hist] capture failed uuid=%s reason=%s", uuid, reason);
    g_mutex_lock(&g_fail_mu);
    if (!g_fail_open)
        g_fail_open = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    if (!g_hash_table_contains(g_fail_open, uuid)) {
        gint64 *t = g_new(gint64, 1);
        *t = _wall();
        g_hash_table_insert(g_fail_open, g_strdup(uuid), t);
    }
    g_mutex_unlock(&g_fail_mu);
}

static void
_capture_ok(const gchar *uuid)
{
    g_mutex_lock(&g_fail_mu);
    gint64 *start = g_fail_open ? g_hash_table_lookup(g_fail_open, uuid) : NULL;
    if (start) {
        JsonObject *o = json_object_new();
        json_object_set_string_member(o, "vm_uuid", uuid);
        json_object_set_int_member(o, "start", *start);
        json_object_set_int_member(o, "end", _wall());
        g_queue_push_tail(&g_fail_closed, o);
        while (g_queue_get_length(&g_fail_closed) > 64)
            json_object_unref(g_queue_pop_head(&g_fail_closed));
        g_hash_table_remove(g_fail_open, uuid);
    }
    g_mutex_unlock(&g_fail_mu);
}

JsonArray *
pcv_vm_cfg_hist_capture_failures(gint64 since, gint64 until)
{
    JsonArray *out = json_array_new();
    g_mutex_lock(&g_fail_mu);
    for (GList *l = g_fail_closed.head; l; l = l->next) {
        JsonObject *o = l->data;
        if (json_object_get_int_member(o, "end") >= since && json_object_get_int_member(o, "start") <= until)
            json_array_add_object_element(out, json_object_ref(o));
    }
    if (g_fail_open) {
        GHashTableIter it;
        gpointer k, v;
        g_hash_table_iter_init(&it, g_fail_open);
        while (g_hash_table_iter_next(&it, &k, &v)) {
            if (*(gint64 *)v > until)
                continue;
            JsonObject *o = json_object_new();
            json_object_set_string_member(o, "vm_uuid", k);
            json_object_set_int_member(o, "start", *(gint64 *)v);
            json_object_set_int_member(o, "end", until);
            json_array_add_object_element(out, o);
        }
    }
    g_mutex_unlock(&g_fail_mu);
    return out;
}

static gint
_cfg_int(const gchar *key, gint def, gint lo, gint hi, gboolean zero_ok)
{
    gint v = pcv_config_get_int("vm_config_history", key, def);
    if ((zero_ok && v == 0) || (v >= lo && v <= hi))
        return v;
    PCV_LOG_WARN(VMCH_LOG, "[vm-cfg-hist] config %s out of range — using default %d", key, def);
    return def;
}

void
pcv_vm_cfg_hist_config_load(PcvVmCfgHistConfig *c)
{
    memset(c, 0, sizeof *c);
    const gchar *en = pcv_config_get_string("vm_config_history", "enabled", "true");
    c->enabled = !(en && (!g_ascii_strcasecmp(en, "false") || !strcmp(en, "0") || !g_ascii_strcasecmp(en, "no")));
    c->db_path = g_strdup(pcv_config_get_string("vm_config_history", "db_path", PCV_VMCH_DEFAULT_DB_PATH));
    c->retention_days = _cfg_int("retention_days", 365, 1, 3650, FALSE);
    c->max_db_mb = _cfg_int("max_db_mb", 128, 16, 4096, FALSE);
    c->actor_retention_days = _cfg_int("actor_retention_days", 30, 1, c->retention_days, FALSE);
    c->reconcile_interval_s = _cfg_int("reconcile_interval_s", 900, 60, 86400, TRUE);
    c->debounce_ms = _cfg_int("debounce_ms", 2000, 200, 10000, FALSE);
    c->undefine_grace_ms = _cfg_int("undefine_grace_ms", 10000, 1000, 60000, FALSE);
    c->wait_mainloop = TRUE;
    c->wait_coverage = TRUE;
    c->scan_gap_ms = 125;
    c->coalesce_ms = PCV_VMCH_COALESCE_MS;
    c->settle_ms = PCV_VMCH_RUN_SETTLE_MS;
    c->shutdown_timeout_ms = 5000;
    c->heartbeat_s = PCV_VMCH_HEARTBEAT_S;
    c->prune_interval_s = PCV_VMCH_PRUNE_INTERVAL_S;
    c->pending_max = PCV_VMCH_PENDING_MAX;
}

void
pcv_vm_cfg_hist_config_clear(PcvVmCfgHistConfig *c)
{
    if (c) {
        g_free(c->db_path);
        c->db_path = NULL;
    }
}

static void
_pending_free(gpointer p)
{
    Pending *x = p;
    g_free(x->cause);
    g_free(x);
}

static void
_pend(Worker *w, const gchar *uuid, const gchar *cause, gint delay_ms, gboolean extend, gboolean settle_close)
{
    if (!uuid || !*uuid)
        return;
    gint64 now = _mono();
    Pending *p = g_hash_table_lookup(w->pending, uuid);
    if (!p) {
        p = g_new0(Pending, 1);
        p->cause = g_strdup(cause);
        p->due = now + delay_ms;
        if (w->store && delay_ms == w->cfg.debounce_ms &&
            pcv_vm_cfg_coalesce_active(pcv_vm_cfg_store_rows_since(w->store, uuid, _wall() - 3600000))) {
            p->due = now + w->cfg.coalesce_ms;
            _count(C_COALESCED);
        }
        g_hash_table_insert(w->pending, g_ascii_strdown(uuid, -1), p);
    } else if (extend) {
        p->due = MAX(p->due, now + delay_ms);
    }
    if (g_strcmp0(cause, "reconcile") && (!g_strcmp0(p->cause, "reconcile") || !g_strcmp0(cause, "libvirt_undefined"))) {
        g_free(p->cause);
        p->cause = g_strdup(cause);
    }
    p->settle_close = p->settle_close || settle_close;
}

static gchar *
_snapshot_part(const gchar *snapshot_json, const gchar *member)
{
    if (!snapshot_json)
        return NULL;
    JsonParser *p = json_parser_new();
    gchar *out = NULL;
    if (json_parser_load_from_data(p, snapshot_json, -1, NULL) && JSON_NODE_HOLDS_OBJECT(json_parser_get_root(p))) {
        JsonNode *n = json_object_get_member(json_node_get_object(json_parser_get_root(p)), member);
        if (n && !JSON_NODE_HOLDS_NULL(n))
            out = json_to_string(n, FALSE);
    }
    g_object_unref(p);
    return out;
}

static gchar *
_snapshot_json(const gchar *config_json, const gchar *overlay_json, gboolean autostart, gboolean transient)
{
    return g_strdup_printf("{\"autostart\":%s,\"config\":%s,\"live_overlay\":%s%s}", autostart ? "true" : "false",
                           config_json ? config_json : "{}", overlay_json ? overlay_json : "null",
                           transient ? ",\"transient\":true" : "");
}

static gchar *
_merge_changes(const gchar *cfg_diff, const gchar *live_diff)
{
    JsonArray *all = json_array_new();
    const gchar *parts[2] = {cfg_diff, live_diff};
    for (guint k = 0; k < 2; k++) {
        if (!parts[k])
            continue;
        JsonParser *p = json_parser_new();
        if (json_parser_load_from_data(p, parts[k], -1, NULL) && JSON_NODE_HOLDS_ARRAY(json_parser_get_root(p))) {
            JsonArray *a = json_node_get_array(json_parser_get_root(p));
            for (guint i = 0; i < json_array_get_length(a); i++) {
                JsonObject *o = json_object_ref(json_array_get_object_element(a, i));
                if (k == 1) {
                    gchar *path = g_strconcat("live.", json_object_get_string_member(o, "path"), NULL);
                    json_object_set_string_member(o, "path", path);
                    g_free(path);
                }
                json_array_add_object_element(all, o);
            }
        }
        g_object_unref(p);
    }
    JsonNode *n = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(n, all);
    gchar *out = json_to_string(n, FALSE);
    json_node_unref(n);
    return out;
}

static void
_retry_free(gpointer p)
{
    Retry *r = p;
    pcv_vm_cfg_row_free(r->row);
    g_free(r->current_name);
    g_free(r->run_live_digest);
    g_free(r);
}

static void
_store_row(Worker *w, PcvVmCfgRow *row, const PcvVmCfgLatestUpdate *lu)
{
    GError *e = NULL;
    gboolean ok = w->retry.length == 0 && !g_atomic_int_get(&w->fail_writes) &&
                  pcv_vm_cfg_store_insert(w->store, row, lu, &e);
    if (ok) {
        _count(C_WRITTEN);
        pcv_vm_cfg_row_free(row);
        return;
    }
    g_clear_error(&e);
    _count(C_WRITE_FAIL);
    Retry *r = g_new0(Retry, 1);
    r->row = row;
    r->lu = *lu;
    r->current_name = g_strdup(lu->current_name);
    r->run_live_digest = g_strdup(lu->run_live_digest);
    r->lu.current_name = r->current_name;
    r->lu.run_live_digest = r->run_live_digest;
    g_queue_push_tail(&w->retry, r);
    while ((gint)w->retry.length > w->pending_max) {
        _retry_free(g_queue_pop_head(&w->retry));
        if (w->epoch_id && !w->epoch_failed) {
            pcv_vm_cfg_store_epoch_end(w->store, w->epoch_id, _wall(), "store_write_failed", NULL);
            PCV_LOG_WARN(VMCH_LOG, "[vm-cfg-hist] epoch end reason=store_write_failed");
            w->epoch_failed = TRUE;
            w->epoch_id = 0;
        }
    }
    if (!w->retry_due) {
        w->retry_backoff_ms = 1000;
        w->retry_due = _mono() + w->retry_backoff_ms;
    }
    _set_state("degraded");
}

static void
_retry_flush(Worker *w)
{
    if (!w->retry.length || _mono() < w->retry_due)
        return;
    while (w->retry.length) {
        Retry *r = g_queue_peek_head(&w->retry);
        if (g_atomic_int_get(&w->fail_writes) || !pcv_vm_cfg_store_insert(w->store, r->row, &r->lu, NULL)) {
            w->retry_backoff_ms = MIN(w->retry_backoff_ms * 2, 60000);
            w->retry_due = _mono() + w->retry_backoff_ms;
            return;
        }
        _count(C_WRITTEN);
        _retry_free(g_queue_pop_head(&w->retry));
    }
    w->retry_due = 0;
    _set_state("ok");
    if (w->epoch_failed) {
        w->epoch_failed = FALSE;
        w->epoch_id = 0;
        w->gap_floor = pcv_vm_cfg_store_last_epoch_end(w->store);
        w->need_rescan = TRUE;
        w->rescan_cause = "startup";
    }
}

typedef enum { CAP_FAILED, CAP_WROTE, CAP_TOUCHED, CAP_SAME } CapResult;

static CapResult
_capture(Worker *w, const gchar *uuid, const gchar *cause, gboolean settle_close, gboolean first_boot, gint64 floor)
{
    PcvVmCfgRaw raw = {.domain_id = -1};
    gboolean exists = TRUE;
    GError *e = NULL;
    _count(C_READS);
    gboolean read_ok = FALSE;
    if (_ext_enter()) {
        read_ok = w->src->read(w->src_data, uuid, &raw, &exists, &e);
        _ext_leave();
    }
    if (!read_ok) {
        g_clear_error(&e);
        pcv_vm_cfg_raw_clear(&raw);
        _capture_failed(uuid, "libvirt");
        return CAP_FAILED;
    }
    PcvVmCfgSnapshot snap = {0};
    gsize salt_len = 0;
    const guint8 *salt = pcv_vm_cfg_store_salt(w->store, &salt_len);
    if (exists) {
        const gchar *err = NULL;
        if (!pcv_vm_cfg_normalize(raw.inactive_xml, raw.inactive_xml ? strlen(raw.inactive_xml) : 0, raw.live_xml,
                                  raw.live_xml ? strlen(raw.live_xml) : 0, raw.autostart, salt, salt_len, &snap, &err)) {
            pcv_vm_cfg_raw_clear(&raw);
            _capture_failed(uuid, err ? err : "parse");
            return CAP_FAILED;
        }
    }
    _capture_ok(uuid);
    PcvVmCfgLatestRow l;
    pcv_vm_cfg_store_latest(w->store, uuid, &l, NULL);
    const gchar *salt_id = pcv_vm_cfg_store_salt_id(w->store);
    PcvVmCfgLatest latest = {.have_row = l.have_row, .present = l.present,
                             .config_digest = l.last ? l.last->config_digest : NULL, .run_id = l.run_id,
                             .run_started_at = l.run_started_at, .run_live_digest = l.run_live_digest,
                             .salt_id = l.last ? l.last->salt_id : salt_id,
                             .normalizer_version = l.last ? l.last->normalizer_version : PCV_VMCH_NORMALIZER_VERSION};
    PcvVmCfgObservation obs = {.exists = exists, .persistent = exists && raw.persistent,
                               .config_digest = snap.config_digest, .live_digest = snap.live_digest,
                               .domain_id = exists ? raw.domain_id : -1, .now_ms = _wall(), .settle_close = settle_close,
                               .salt_id = salt_id, .normalizer_version = PCV_VMCH_NORMALIZER_VERSION,
                               .settle_ms = w->cfg.settle_ms};
    gchar *prev_cfg = l.last ? _snapshot_part(l.last->snapshot_json, "config") : NULL;
    guint flags = (g_strcmp0(latest.salt_id, salt_id) ? PCV_VMCH_DIFF_IGNORE_OPAQUE : 0) |
                  (latest.normalizer_version != PCV_VMCH_NORMALIZER_VERSION ? PCV_VMCH_DIFF_IGNORE_NEW_PATHS : 0);
    if (l.have_row && exists && flags)
        g_free(pcv_vm_cfg_diff_filtered(prev_cfg, snap.config_json, 1, flags, &obs.rebase_changes));
    PcvVmCfgDecision d = pcv_vm_cfg_decide(&latest, &obs, first_boot, floor < 0 ? l.last_confirmed_at : floor);
    const gchar *name = exists ? (snap.name && *snap.name ? snap.name : raw.name) : (l.last ? l.last->vm_name : raw.name);
    PcvVmCfgLatestUpdate lu = {.present = d.present_after, .current_name = name, .run_action = d.run_action,
                               .run_id = obs.domain_id, .run_started_at = obs.now_ms,
                               .run_live_digest = snap.live_digest, .confirmed_at = obs.now_ms};
    CapResult res = CAP_SAME;
    if (d.write_row) {
        PcvVmCfgRow *row = g_new0(PcvVmCfgRow, 1);
        row->vm_uuid = g_ascii_strdown(uuid, -1);
        row->vm_name = g_strdup(name ? name : "");
        row->kind = g_strdup(d.kind);
        row->scope = g_strdup(d.scope);
        row->cause = g_strconcat(cause, d.cause_suffix ? d.cause_suffix : "", NULL);
        row->observed_at = obs.now_ms;
        row->window_start = d.window_start;
        row->normalizer_version = PCV_VMCH_NORMALIZER_VERSION;
        row->salt_id = g_strdup(salt_id);
        row->epoch_id = w->epoch_id;
        if (!g_strcmp0(d.kind, "undefined")) {
            gchar *ov = l.last ? _snapshot_part(l.last->snapshot_json, "live_overlay") : NULL;
            row->snapshot_json = _snapshot_json(prev_cfg, NULL, FALSE, d.transient);
            g_free(ov);
            row->config_digest = g_strdup(l.last ? l.last->config_digest : "");
            row->changes_json = g_strdup("[]");
            row->owner = g_strdup(l.last ? l.last->owner : NULL);
            row->owner_tenant = g_strdup(l.last ? l.last->owner_tenant : NULL);
        } else {
            row->snapshot_json = _snapshot_json(snap.config_json, snap.live_overlay_json, raw.autostart, FALSE);
            row->config_digest = g_strdup(snap.config_digest);
            row->live_digest = g_strdup(snap.live_digest);
            gboolean first = !g_strcmp0(d.kind, "baseline") || !g_strcmp0(d.kind, "created");
            gchar *cd = first ? NULL : pcv_vm_cfg_diff_filtered(prev_cfg, snap.config_json, 0, d.diff_flags, NULL);
            gchar *ld = NULL;
            if (!first && strcmp(d.scope, "config"))
                ld = pcv_vm_cfg_diff(g_hash_table_lookup(w->run_overlay, uuid), snap.live_overlay_json, 0, NULL);
            row->changes_json = first ? g_strdup("[]") : _merge_changes(cd, ld);
            g_free(cd);
            g_free(ld);
            row->owner = g_strdup(snap.owner);
            gchar *tenant = NULL;
            if (snap.owner && w->src->owner_tenant && _ext_enter()) {
                tenant = w->src->owner_tenant(w->src_data, snap.owner);
                _ext_leave();
            }
            if (snap.owner && !tenant)
                _count(C_TENANT_UNKNOWN);
            row->owner_tenant = tenant;
        }
        const gchar *names[] = {uuid, name, l.current_name, NULL};
        PcvVmCfgAttribution a;
        pcv_vm_cfg_intents_match(w->intents, names, d.window_start, obs.now_ms, &a);
        row->attribution = g_strdup(pcv_vm_cfg_attribution_name(a.kind));
        row->actor = g_strdup(a.kind == PCV_VMCH_ATTR_RPC ? a.actor : NULL);
        row->actor_tenant = g_strdup(a.kind == PCV_VMCH_ATTR_RPC ? a.actor_tenant : NULL);
        row->actor_method = g_strdup(a.method);
        row->actor_result = g_strdup(a.kind == PCV_VMCH_ATTR_RPC ? a.result : NULL);
        pcv_vm_cfg_attribution_clear(&a);
        _store_row(w, row, &lu);
        res = CAP_WROTE;
    } else if (l.have_row) {
        gboolean need_touch = d.run_action != PCV_VMCH_RUN_KEEP || d.present_after != l.present ||
                              g_strcmp0(l.current_name, name);
        if (need_touch)
            pcv_vm_cfg_store_touch(w->store, uuid, &lu, NULL);
        res = need_touch ? CAP_TOUCHED : CAP_SAME;
        _count(C_UNCHANGED);
    }
    if (d.run_action == PCV_VMCH_RUN_SET || d.run_action == PCV_VMCH_RUN_UPDATE_LIVE)
        g_hash_table_replace(w->run_overlay, g_ascii_strdown(uuid, -1), g_strdup(snap.live_overlay_json));
    else if (d.run_action == PCV_VMCH_RUN_CLEAR)
        g_hash_table_remove(w->run_overlay, uuid);
    g_free(prev_cfg);
    pcv_vm_cfg_latest_clear(&l);
    pcv_vm_cfg_snapshot_clear(&snap);
    pcv_vm_cfg_raw_clear(&raw);
    return res;
}

static void
_write_resized(Worker *w, const gchar *uuid, const PcvVmCfgAttribution *a)
{
    PcvVmCfgLatestRow l;
    if (!a->resize_dev || a->resize_bytes <= 0 || !pcv_vm_cfg_store_latest(w->store, uuid, &l, NULL) || !l.have_row) {
        if (a->resize_dev)
            pcv_vm_cfg_latest_clear(&l);
        return;
    }
    gint64 before = pcv_vm_cfg_store_last_resized(w->store, uuid, a->resize_dev);
    PcvVmCfgRow *row = g_new0(PcvVmCfgRow, 1);
    row->vm_uuid = g_strdup(uuid);
    row->vm_name = g_strdup(l.last->vm_name);
    row->owner = g_strdup(l.last->owner);
    row->owner_tenant = g_strdup(l.last->owner_tenant);
    row->kind = g_strdup("resized");
    row->scope = g_strdup("config");
    row->cause = g_strdup("rpc");
    row->observed_at = _wall();
    row->attribution = g_strdup("rpc");
    row->actor = g_strdup(a->actor);
    row->actor_tenant = g_strdup(a->actor_tenant);
    row->actor_method = g_strdup(a->method);
    row->actor_result = g_strdup("ok");
    row->config_digest = g_strdup(l.last->config_digest);
    row->live_digest = g_strdup(l.last->live_digest);
    row->snapshot_json = g_strdup(l.last->snapshot_json);
    JsonNode *dev = json_node_new(JSON_NODE_VALUE);
    gchar *path = g_strdup_printf("disk[%s].requested_size_bytes", a->resize_dev);
    json_node_set_string(dev, path);
    gchar *ps = json_to_string(dev, FALSE);
    gchar *bs = before >= 0 ? g_strdup_printf("%" G_GINT64_FORMAT, before) : g_strdup("null");
    row->changes_json = g_strdup_printf("[{\"after\":%" G_GINT64_FORMAT ",\"before\":%s,\"path\":%s}]", a->resize_bytes,
                                        bs, ps);
    row->normalizer_version = l.last->normalizer_version;
    row->salt_id = g_strdup(l.last->salt_id);
    row->epoch_id = w->epoch_id;
    PcvVmCfgLatestUpdate lu = {.present = l.present, .current_name = l.current_name, .run_action = PCV_VMCH_RUN_KEEP,
                               .confirmed_at = row->observed_at};
    _store_row(w, row, &lu);
    g_free(bs);
    g_free(ps);
    g_free(path);
    json_node_unref(dev);
    pcv_vm_cfg_latest_clear(&l);
}

static void
_late_fill(Worker *w, const gchar *uuid, const gchar *method, const gchar *actor, const gchar *actor_tenant,
           const gchar *result)
{
    PcvVmCfgLatestRow l;
    if (!pcv_vm_cfg_store_latest(w->store, uuid, &l, NULL))
        return;
    if (l.last && pcv_vm_cfg_late_fill_allowed(l.last->attribution, l.last->observed_at,
                                               g_hash_table_contains(w->filled, l.last->entry_id), _wall()) &&
        pcv_vm_cfg_store_fill_actor(w->store, l.last->entry_id, actor, actor_tenant, method,
                                    !g_strcmp0(result, "ok") ? "ok" : "fail", NULL))
    {

        if (g_hash_table_size(w->filled) >= 4096)
            g_hash_table_remove_all(w->filled);
        g_hash_table_add(w->filled, g_strdup(l.last->entry_id));
    }
    pcv_vm_cfg_latest_clear(&l);
}

static void
_scan(Worker *w, const gchar *cause, gboolean first_boot, gint64 floor)
{
    GError *e = NULL;
    GPtrArray *uuids = NULL;
    if (_ext_enter()) {
        uuids = w->src->list_domains(w->src_data, &e);
        _ext_leave();
    }
    if (!uuids) {
        g_clear_error(&e);
        PCV_LOG_WARN(VMCH_LOG, "[vm-cfg-hist] capture failed uuid=* reason=libvirt");
        return;
    }
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    GPtrArray *same = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < uuids->len && !g_atomic_int_get(&w->stop); i++) {
        const gchar *u = g_ptr_array_index(uuids, i);
        g_hash_table_add(seen, (gpointer)u);
        g_hash_table_remove(w->pending, u);
        if (_capture(w, u, cause, FALSE, first_boot, floor) == CAP_SAME)
            g_ptr_array_add(same, g_strdup(u));
        if (w->cfg.scan_gap_ms > 0)
            g_usleep((gulong)w->cfg.scan_gap_ms * 1000);
    }
    GPtrArray *present = pcv_vm_cfg_store_present_uuids(w->store);
    for (guint i = 0; i < present->len && !g_atomic_int_get(&w->stop); i++) {
        const gchar *u = g_ptr_array_index(present, i);
        if (!g_hash_table_contains(seen, u))
            _capture(w, u, cause, FALSE, FALSE, floor);
    }
    if (same->len)
        pcv_vm_cfg_store_confirm(w->store, (const gchar *const *)same->pdata, same->len, _wall(), NULL);
    g_ptr_array_unref(present);
    g_ptr_array_unref(same);
    g_hash_table_unref(seen);
    g_ptr_array_unref(uuids);
}

static void
_publish(Worker *w)
{
    PcvVmCfgStoreInfo info;
    if (w->store && pcv_vm_cfg_store_info(w->store, &info)) {
        _prom_gauge("purecvisor_vm_config_history_rows", "", (gdouble)info.rows);
        _prom_gauge("purecvisor_vm_config_history_db_bytes", "", (gdouble)info.bytes);
    }
}

static void
_maybe_scan(Worker *w)
{
    if (!w->need_rescan || !w->ready || !w->covered || !w->store || w->retry.length)
        return;
    GError *e = NULL;
    w->epoch_id = pcv_vm_cfg_store_epoch_begin(w->store, _wall(), &e);
    if (!w->epoch_id) {
        g_clear_error(&e);
        _set_state("degraded");
        return;
    }
    PCV_LOG_INFO(VMCH_LOG, "[vm-cfg-hist] epoch begin reason=%s", w->rescan_cause);
    gboolean first = w->first_boot && !w->scanned_once;
    _scan(w, w->rescan_cause, first, first ? 0 : w->gap_floor);
    w->scanned_once = TRUE;
    w->need_rescan = FALSE;
    w->next_reconcile = _mono() + (gint64)w->cfg.reconcile_interval_s * 1000;
    _publish(w);
}

static void
_handle(Worker *w, Msg *m)
{
    switch (m->k) {
    case M_NOTE:
        _pend(w, m->uuid, m->cause, w->cfg.debounce_ms, FALSE, FALSE);
        break;
    case M_STARTED: {
        _pend(w, m->uuid, "reconcile", w->cfg.debounce_ms, FALSE, FALSE);
        gint64 *due = g_new(gint64, 1);
        *due = _mono() + w->cfg.settle_ms + 500;
        g_hash_table_replace(w->settles, g_ascii_strdown(m->uuid, -1), due);
        break;
    }
    case M_UNDEFINED:
        _pend(w, m->uuid, "libvirt_undefined", w->cfg.undefine_grace_ms, TRUE, FALSE);
        break;
    case M_COV_BEGIN:
        if (!w->covered && w->scanned_once) {
            w->need_rescan = TRUE;
            w->rescan_cause = "reconnect";
            w->gap_floor = w->store ? pcv_vm_cfg_store_last_epoch_end(w->store) : 0;
        }
        w->covered = TRUE;
        break;
    case M_COV_END:
        if (w->covered && w->epoch_id && w->store) {
            pcv_vm_cfg_store_epoch_end(w->store, w->epoch_id, _wall(), m->cause ? m->cause : "listener_lost", NULL);
            PCV_LOG_WARN(VMCH_LOG, "[vm-cfg-hist] epoch end reason=%s", m->cause ? m->cause : "listener_lost");
            w->epoch_id = 0;
        }
        w->covered = FALSE;
        break;
    case M_INTENT_OPEN:
        pcv_vm_cfg_intents_open(w->intents, m->method, m->target, m->actor, m->actor_tenant, m->at, m->dev, m->bytes);
        break;
    case M_INTENT_CLOSE: {
        gchar *tenant = NULL;
        gboolean closed = pcv_vm_cfg_intents_close(w->intents, m->method, m->actor, m->target, m->result, m->at, &tenant);
        if (!g_strcmp0(m->result, "denied") || !w->store) {
            g_free(tenant);
            break;
        }
        gchar *uuid = NULL;
        if (w->src->resolve && _ext_enter()) {
            uuid = w->src->resolve(w->src_data, m->target);
            _ext_leave();
        }
        if (!uuid)
            uuid = pcv_vm_cfg_store_resolve_name(w->store, m->target);
        PcvVmCfgAttribution a;
        if (uuid && closed && !g_strcmp0(m->result, "ok") && w->epoch_id &&
            pcv_vm_cfg_intents_take_resize(w->intents, m->method, m->actor, m->target, &a)) {
            _write_resized(w, uuid, &a);
            pcv_vm_cfg_attribution_clear(&a);
        } else if (uuid && closed) {
            _late_fill(w, uuid, m->method, m->actor, tenant, m->result);
        }
        if (uuid)
            _pend(w, uuid, "rpc", w->cfg.debounce_ms, FALSE, FALSE);
        g_free(uuid);
        g_free(tenant);
        break;
    }
    case M_READY:
        w->ready = TRUE;
        break;
    case M_SYNC:
        g_ptr_array_add(w->syncs, m->sync);
        m->sync = NULL;
        break;
    case M_STOP:
        g_atomic_int_set(&w->stop, 1);
        break;
    }
}

static void
_run_due(Worker *w)
{
    if (!w->store) {
        g_hash_table_remove_all(w->pending);
        g_hash_table_remove_all(w->settles);
        return;
    }
    if (!w->epoch_id || w->retry.length)
        return;
    gint64 now = _mono();
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, w->settles);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        if (*(gint64 *)v <= now) {
            _pend(w, k, "reconcile", 0, FALSE, TRUE);
            g_hash_table_iter_remove(&it);
        }
    }
    GPtrArray *due = g_ptr_array_new_with_free_func(g_free);
    g_hash_table_iter_init(&it, w->pending);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        if (((Pending *)v)->due <= now)
            g_ptr_array_add(due, g_strdup(k));
    }
    for (guint i = 0; i < due->len && !g_atomic_int_get(&w->stop); i++) {
        const gchar *u = g_ptr_array_index(due, i);
        Pending *p = g_hash_table_lookup(w->pending, u);
        gchar *cause = g_strdup(p->cause);
        gboolean settle = p->settle_close;
        g_hash_table_remove(w->pending, u);
        _capture(w, u, cause, settle, FALSE, 0);
        g_free(cause);
    }
    g_ptr_array_unref(due);
}

static void
_timers(Worker *w)
{
    if (!w->store)
        return;
    gint64 now = _mono();
    pcv_vm_cfg_intents_expire(w->intents, _wall());
    static guint64 reported_drops;
    guint64 drops = pcv_vm_cfg_intents_dropped(w->intents);
    for (; reported_drops < drops; reported_drops++)
        _prom_inc("purecvisor_vm_config_history_intents_dropped_total", NULL, NULL);
    _retry_flush(w);
    if (w->epoch_id && now >= w->next_heartbeat) {
        pcv_vm_cfg_store_epoch_heartbeat(w->store, w->epoch_id, _wall(), NULL);
        w->next_heartbeat = now + (gint64)w->cfg.heartbeat_s * 1000;
        _publish(w);
    }
    if (w->epoch_id && !w->retry.length && w->cfg.reconcile_interval_s > 0 && now >= w->next_reconcile) {

        _scan(w, "reconcile", FALSE, -1);
        w->next_reconcile = _mono() + (gint64)w->cfg.reconcile_interval_s * 1000;
    }
    if (now >= w->next_prune) {
        PcvVmCfgPruneResult pr;
        if (pcv_vm_cfg_store_prune(w->store, _wall(), w->cfg.retention_days, w->cfg.actor_retention_days,
                                   (gint64)w->cfg.max_db_mb * 1024 * 1024, PCV_VMCH_PRUNE_BATCH, &w->stop,
                                   PCV_VMCH_PRUNE_BUDGET_MS, &pr, NULL)) {
            if (pr.over_cap && _state_is("ok"))
                _set_state("over_cap");
            else if (!pr.over_cap && _state_is("over_cap"))
                _set_state("ok");
        }

        w->next_prune = _mono() + (pr.interrupted ? 60000 : (gint64)w->cfg.prune_interval_s * 1000);
        _publish(w);
    }
}

static gint64
_next_deadline(Worker *w)
{
    gint64 next = _mono() + 1000;
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, w->pending);
    while (g_hash_table_iter_next(&it, &k, &v))
        next = MIN(next, ((Pending *)v)->due);
    g_hash_table_iter_init(&it, w->settles);
    while (g_hash_table_iter_next(&it, &k, &v))
        next = MIN(next, *(gint64 *)v);
    if (w->retry_due)
        next = MIN(next, w->retry_due);
    return next;
}

static void
_sync_release(SyncWait *s, gboolean done)
{
    g_mutex_lock(&s->mu);
    s->done = s->done || done;
    g_cond_broadcast(&s->cond);
    gboolean last = --s->refs == 0;
    g_mutex_unlock(&s->mu);
    if (last) {
        g_mutex_clear(&s->mu);
        g_cond_clear(&s->cond);
        g_free(s);
    }
}

static void
_answer_syncs(Worker *w)
{
    gboolean idle = g_async_queue_length(w->q) <= 0 && (g_hash_table_size(w->pending) == 0 || w->retry.length) &&
                    g_hash_table_size(w->settles) == 0 && !(w->need_rescan && w->ready && w->covered && w->store);
    if (!idle && !g_atomic_int_get(&w->stop))
        return;
    for (guint i = 0; i < w->syncs->len; i++)
        _sync_release(g_ptr_array_index(w->syncs, i), idle);
    g_ptr_array_set_size(w->syncs, 0);
}

static void
_worker_unref(Worker *w)
{
    if (!g_atomic_int_dec_and_test(&w->refs))
        return;
    Msg *m;
    while ((m = g_async_queue_try_pop(w->q)))
        _msg_free(m);
    g_async_queue_unref(w->q);
    pcv_vm_cfg_hist_config_clear(&w->cfg);
    g_mutex_clear(&w->exit_mu);
    g_cond_clear(&w->exit_cond);
    g_free(w);
}

static gboolean
_on_mainloop_ready(gpointer unused)
{
    (void)unused;
    _push(_msg(M_READY));
    return G_SOURCE_REMOVE;
}

static void
_init_store(Worker *w)
{
    GError *e = NULL;
    if (!w->cfg.enabled) {
        _set_state("disabled");
        gchar *kp = pcv_vm_cfg_store_key_path(w->cfg.db_path);

        gboolean have_key = g_file_test(kp, G_FILE_TEST_EXISTS);
        g_free(kp);
        if (have_key && g_file_test(w->cfg.db_path, G_FILE_TEST_EXISTS)) {
            PcvVmCfgStore *st = pcv_vm_cfg_store_open(w->cfg.db_path, TRUE, NULL);
            if (st) {
                pcv_vm_cfg_store_close_open_epochs_crash(st, NULL);
                gint64 id = pcv_vm_cfg_store_epoch_begin(st, _wall(), NULL);
                if (id)
                    pcv_vm_cfg_store_epoch_end(st, id, _wall(), "disabled", NULL);
                pcv_vm_cfg_store_close(st);
            }
        }
        return;
    }
    w->store = pcv_vm_cfg_store_open(w->cfg.db_path, TRUE, &e);
    if (!w->store) {
        PCV_LOG_WARN(VMCH_LOG, "[vm-cfg-hist] store open failed: %s", e ? e->message : "unknown");
        g_clear_error(&e);
        _set_state("unavailable");
        return;
    }
    w->first_boot = pcv_vm_cfg_store_created(w->store);
    if (pcv_vm_cfg_store_close_open_epochs_crash(w->store, NULL) > 0)
        PCV_LOG_WARN(VMCH_LOG, "[vm-cfg-hist] epoch end reason=crash_inferred");
    w->gap_floor = pcv_vm_cfg_store_last_epoch_end(w->store);
    w->need_rescan = TRUE;
    w->rescan_cause = "startup";
    w->ready = !w->cfg.wait_mainloop;
    w->covered = !w->cfg.wait_coverage;
    if (w->cfg.wait_mainloop)
        g_idle_add(_on_mainloop_ready, NULL);
    gint64 now = _mono();
    w->next_heartbeat = now + (gint64)w->cfg.heartbeat_s * 1000;
    w->next_prune = now + 60000;
    _set_state("ok");
}

static gpointer
_thread(gpointer data)
{
    Worker *w = data;
    g_private_set(&g_self, w);
    _init_store(w);
    while (!g_atomic_int_get(&w->stop)) {
        gint64 wait = MAX(0, _next_deadline(w) - _mono());
        Msg *m = g_async_queue_timeout_pop(w->q, (guint64)MIN(wait, 1000) * 1000);
        while (m) {
            _handle(w, m);
            _msg_free(m);
            m = g_async_queue_try_pop(w->q);
        }
        if (g_atomic_int_get(&w->stop))
            break;
        _maybe_scan(w);
        _run_due(w);
        _timers(w);
        _answer_syncs(w);
    }
    if (w->store) {
        if (w->epoch_id)
            pcv_vm_cfg_store_epoch_end(w->store, w->epoch_id, _wall(), "shutdown", NULL);
        pcv_vm_cfg_store_close(w->store);
    }
    _answer_syncs(w);
    g_ptr_array_unref(w->syncs);
    pcv_vm_cfg_intents_free(w->intents);
    g_hash_table_unref(w->pending);
    g_hash_table_unref(w->settles);
    g_hash_table_unref(w->run_overlay);
    g_hash_table_unref(w->filled);
    g_queue_clear_full(&w->retry, _retry_free);
    g_mutex_lock(&w->exit_mu);
    w->exited = TRUE;
    g_cond_broadcast(&w->exit_cond);
    g_mutex_unlock(&w->exit_mu);
    _worker_unref(w);
    return NULL;
}

gboolean
pcv_vm_cfg_hist_start_with(const PcvVmCfgHistConfig *c, const PcvVmCfgHistSource *src, gpointer src_data)
{
    Worker *w = g_new0(Worker, 1);
    w->refs = 2;
    w->q = g_async_queue_new_full(_msg_free);
    w->cfg = *c;
    w->cfg.db_path = g_strdup(c->db_path ? c->db_path : PCV_VMCH_DEFAULT_DB_PATH);
    w->pending_max = c->pending_max > 0 ? c->pending_max : PCV_VMCH_PENDING_MAX;
    w->src = src;
    w->src_data = src_data;
    g_mutex_init(&w->exit_mu);
    g_cond_init(&w->exit_cond);
    w->intents = pcv_vm_cfg_intents_new(0);
    w->pending = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, _pending_free);
    w->settles = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    w->run_overlay = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    w->filled = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    w->syncs = g_ptr_array_new();
    g_queue_init(&w->retry);
    g_mutex_lock(&g_mu);
    if (g_w) {
        g_mutex_unlock(&g_mu);
        w->refs = 1;
        g_hash_table_unref(w->pending);
        g_hash_table_unref(w->settles);
        g_hash_table_unref(w->run_overlay);
        g_hash_table_unref(w->filled);
        g_ptr_array_unref(w->syncs);
        pcv_vm_cfg_intents_free(w->intents);
        _worker_unref(w);
        return FALSE;
    }
    g_w = w;
    g_free(g_db_path);
    g_db_path = g_strdup(w->cfg.db_path);
    g_retention_days = w->cfg.retention_days;
    g_enabled = w->cfg.enabled;
    g_free(g_state_name);
    g_state_name = g_strdup(w->cfg.enabled ? "ok" : "disabled");
    g_mutex_unlock(&g_mu);
    w->thread = g_thread_new("vm-cfg-hist", _thread, w);
    return TRUE;
}

void
pcv_vm_cfg_hist_shutdown(void)
{
    g_mutex_lock(&g_mu);
    Worker *w = g_w;
    g_w = NULL;
    g_free(g_state_name);
    g_state_name = NULL;
    g_mutex_unlock(&g_mu);
    if (!w)
        return;
    g_atomic_int_set(&w->stop, 1);
    g_async_queue_push(w->q, _msg(M_STOP));
    gint64 end = g_get_monotonic_time() + (gint64)MAX(w->cfg.shutdown_timeout_ms, 1) * 1000;
    g_mutex_lock(&w->exit_mu);
    while (!w->exited && g_cond_wait_until(&w->exit_cond, &w->exit_mu, end))
        ;
    gboolean exited = w->exited;
    g_mutex_unlock(&w->exit_mu);
    if (exited) {
        g_thread_join(w->thread);
    } else {

        g_atomic_int_set(&w->ext_off, 1);
        gint64 fence_end = g_get_monotonic_time() + (gint64)MAX(w->cfg.shutdown_timeout_ms, 1) * 1000;
        while (g_atomic_int_get(&w->ext_busy) > 0 && g_get_monotonic_time() < fence_end)
            g_usleep(1000);
        PCV_LOG_WARN(VMCH_LOG,
                     "[vm-cfg-hist] worker did not stop within %d ms — detached, external calls fenced (in_flight=%d; next start "
                     "closes the epoch)",
                     w->cfg.shutdown_timeout_ms, g_atomic_int_get(&w->ext_busy));
        g_thread_unref(w->thread);
    }
    _worker_unref(w);
}

void
pcv_vm_cfg_hist_note_uuid(const gchar *uuid, const gchar *cause)
{
    if (!uuid)
        return;
    Msg *m = _msg(M_NOTE);
    m->uuid = g_ascii_strdown(uuid, -1);
    m->cause = g_strdup(cause ? cause : "reconcile");
    _push(m);
}

void
pcv_vm_cfg_hist_note_started(const gchar *uuid)
{
    if (!uuid)
        return;
    Msg *m = _msg(M_STARTED);
    m->uuid = g_ascii_strdown(uuid, -1);
    _push(m);
}

void
pcv_vm_cfg_hist_note_undefined(const gchar *uuid)
{
    if (!uuid)
        return;
    Msg *m = _msg(M_UNDEFINED);
    m->uuid = g_ascii_strdown(uuid, -1);
    _push(m);
}

void
pcv_vm_cfg_hist_coverage(gboolean begin, const gchar *reason)
{
    Msg *m = _msg(begin ? M_COV_BEGIN : M_COV_END);
    m->cause = g_strdup(reason);
    _push(m);
}

void
pcv_vm_cfg_hist_intent_open(const gchar *method, const gchar *target, const gchar *actor, const gchar *actor_tenant,
                            const gchar *resize_dev, gint64 resize_bytes)
{
    if (!method || !target || !*target)
        return;
    Msg *m = _msg(M_INTENT_OPEN);
    m->method = g_strdup(method);
    m->target = g_strdup(target);
    m->actor = g_strdup(actor);
    m->actor_tenant = g_strdup(actor_tenant);
    m->dev = g_strdup(resize_dev);
    m->bytes = resize_bytes;
    _push(m);
}

void
pcv_vm_cfg_hist_intent_close(const gchar *method, const gchar *actor, const gchar *target, const gchar *result)
{
    if (!method)
        return;
    Msg *m = _msg(M_INTENT_CLOSE);
    m->method = g_strdup(method);
    m->actor = g_strdup(actor);
    m->target = g_strdup(target);
    m->result = g_strdup(result);
    _push(m);
}

void
pcv_vm_cfg_hist_status(PcvVmCfgHistStatus *out)
{
    g_mutex_lock(&g_mu);
    out->state = g_strdup(g_w && g_state_name ? g_state_name : "unavailable");
    out->db_path = g_strdup(g_db_path ? g_db_path : PCV_VMCH_DEFAULT_DB_PATH);
    out->retention_days = g_retention_days;
    out->enabled = g_w ? g_enabled : FALSE;
    g_mutex_unlock(&g_mu);
}

void
pcv_vm_cfg_hist_status_clear(PcvVmCfgHistStatus *s)
{
    if (!s)
        return;
    g_free(s->state);
    g_free(s->db_path);
    s->state = s->db_path = NULL;
}

void
pcv_vm_cfg_raw_clear(PcvVmCfgRaw *r)
{
    if (!r)
        return;
    g_free(r->inactive_xml);
    g_free(r->live_xml);
    g_free(r->name);
    memset(r, 0, sizeof *r);
    r->domain_id = -1;
}

gboolean
pcv_vm_cfg_hist_sync(guint timeout_ms)
{
    SyncWait *s = g_new0(SyncWait, 1);
    g_mutex_init(&s->mu);
    g_cond_init(&s->cond);
    s->refs = 2;
    Msg *m = _msg(M_SYNC);
    m->sync = s;
    g_mutex_lock(&g_mu);
    gboolean pushed = g_w != NULL;
    if (pushed)
        g_async_queue_push(g_w->q, m);
    g_mutex_unlock(&g_mu);
    if (!pushed) {
        _msg_free(m);
        s->refs = 1;
        _sync_release(s, FALSE);
        return FALSE;
    }
    gint64 end = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
    g_mutex_lock(&s->mu);
    while (!s->done && g_cond_wait_until(&s->cond, &s->mu, end))
        ;
    gboolean done = s->done;
    g_mutex_unlock(&s->mu);
    _sync_release(s, FALSE);
    return done;
}

void
pcv_vm_cfg_hist_test_fail_writes(gboolean fail)
{
    g_mutex_lock(&g_mu);
    if (g_w)
        g_atomic_int_set(&g_w->fail_writes, fail ? 1 : 0);
    g_mutex_unlock(&g_mu);
}

guint64
pcv_vm_cfg_hist_test_counter(const gchar *name)
{
    for (guint i = 0; i < G_N_ELEMENTS(k_counter_names); i++) {
        if (!g_strcmp0(name, k_counter_names[i]))
            return __atomic_load_n(&g_counters[i], __ATOMIC_RELAXED);
    }
    return 0;
}
