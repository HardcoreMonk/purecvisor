#include "modules/virt/vm_config_history_decide.h"

#include <string.h>

#include "modules/virt/vm_config_history_norm.h"

static PcvVmCfgRunAction
_run_for_new(const PcvVmCfgObservation *obs)
{
    return obs->domain_id >= 0 ? PCV_VMCH_RUN_SET : PCV_VMCH_RUN_KEEP;
}

PcvVmCfgDecision
pcv_vm_cfg_decide(const PcvVmCfgLatest *latest, const PcvVmCfgObservation *obs, gboolean first_db_boot,
                  gint64 window_floor_ms)
{
    PcvVmCfgDecision d = {.kind = "changed", .scope = "config", .present_after = latest->present};
    gint64 floor = window_floor_ms > 0 ? window_floor_ms : 0;
    if (!obs->exists || !obs->persistent) {

        if (latest->have_row && latest->present) {
            d.write_row = TRUE;
            d.kind = "undefined";
            d.transient = obs->exists && !obs->persistent;
            d.window_start = floor;
        }
        d.present_after = FALSE;
        d.run_action = PCV_VMCH_RUN_CLEAR;
        return d;
    }
    d.present_after = TRUE;
    if (!latest->have_row) {

        d.write_row = TRUE;
        d.kind = first_db_boot ? "baseline" : "created";
        d.window_start = first_db_boot ? 0 : floor;
        d.run_action = _run_for_new(obs);
        return d;
    }
    if (!latest->present) {

        d.write_row = TRUE;
        d.window_start = floor;
        d.run_action = _run_for_new(obs);
        return d;
    }
    gboolean config_changed = g_strcmp0(latest->config_digest, obs->config_digest) != 0;
    if (g_strcmp0(latest->salt_id, obs->salt_id) != 0) {
        d.cause_suffix = "+salt_rotated";
        d.diff_flags |= PCV_VMCH_DIFF_IGNORE_OPAQUE;
    }
    if (latest->normalizer_version != obs->normalizer_version) {
        d.cause_suffix = d.cause_suffix ? "+salt_rotated+normalizer_upgrade" : "+normalizer_upgrade";
        d.diff_flags |= PCV_VMCH_DIFF_IGNORE_NEW_PATHS;
    }
    if (d.diff_flags)
        config_changed = obs->rebase_changes > 0;
    gboolean live_changed = FALSE;
    if (obs->domain_id < 0) {
        if (latest->run_id >= 0)
            d.run_action = PCV_VMCH_RUN_CLEAR;
    } else if (latest->run_id != obs->domain_id) {
        d.run_action = PCV_VMCH_RUN_SET;
    } else if (obs->settle_close ||
               obs->now_ms < latest->run_started_at + (obs->settle_ms > 0 ? obs->settle_ms : PCV_VMCH_RUN_SETTLE_MS)) {
        if (g_strcmp0(latest->run_live_digest, obs->live_digest) != 0)
            d.run_action = PCV_VMCH_RUN_UPDATE_LIVE;
    } else if (g_strcmp0(latest->run_live_digest, obs->live_digest) != 0) {
        live_changed = TRUE;
        d.run_action = PCV_VMCH_RUN_UPDATE_LIVE;
    }
    if (config_changed || live_changed) {
        d.write_row = TRUE;
        d.scope = (config_changed && live_changed) ? "both" : (live_changed ? "live" : "config");
        d.window_start = floor;
    }
    return d;
}

typedef struct {
    gchar   *method;
    gchar   *target;
    gchar   *actor;
    gchar   *actor_tenant;
    gchar   *result;
    gchar   *resize_dev;
    gint64   resize_bytes;
    gint64   start_ms;
    gint64   end_ms;
    gboolean closed;
} Intent;

struct _PcvVmCfgIntents {
    GQueue  items;
    guint   max;
    guint64 dropped;
};

static void
_intent_free(gpointer p)
{
    Intent *i = p;
    g_free(i->method);
    g_free(i->target);
    g_free(i->actor);
    g_free(i->actor_tenant);
    g_free(i->result);
    g_free(i->resize_dev);
    g_free(i);
}

PcvVmCfgIntents *
pcv_vm_cfg_intents_new(guint max)
{
    PcvVmCfgIntents *t = g_new0(PcvVmCfgIntents, 1);
    g_queue_init(&t->items);
    t->max = max ? max : PCV_VMCH_INTENT_MAX;
    return t;
}

void
pcv_vm_cfg_intents_free(PcvVmCfgIntents *t)
{
    if (!t)
        return;
    g_queue_clear_full(&t->items, _intent_free);
    g_free(t);
}

void
pcv_vm_cfg_intents_open(PcvVmCfgIntents *t, const gchar *method, const gchar *target, const gchar *actor,
                        const gchar *actor_tenant, gint64 start_ms, const gchar *resize_dev, gint64 resize_bytes)
{
    if (!t || !method || !target || !*target)
        return;
    while (g_queue_get_length(&t->items) >= t->max) {
        _intent_free(g_queue_pop_head(&t->items));
        t->dropped++;
    }
    Intent *i = g_new0(Intent, 1);
    i->method = g_strdup(method);
    i->target = g_ascii_strdown(target, -1);
    i->actor = g_strdup(actor);
    i->actor_tenant = g_strdup(actor_tenant);
    i->resize_dev = g_strdup(resize_dev);
    i->resize_bytes = resize_bytes;
    i->start_ms = start_ms;
    g_queue_push_tail(&t->items, i);
}

gboolean
pcv_vm_cfg_intents_close(PcvVmCfgIntents *t, const gchar *method, const gchar *actor, const gchar *target,
                         const gchar *result, gint64 end_ms, gchar **tenant_out)
{
    if (tenant_out)
        *tenant_out = NULL;
    if (!t || !method)
        return FALSE;
    gchar *low = target ? g_ascii_strdown(target, -1) : NULL;
    gboolean done = FALSE;

    GList *hit = NULL;
    for (GList *l = t->items.head; l && !hit; l = l->next) {
        Intent *i = l->data;
        if (!i->closed && !g_strcmp0(i->method, method) && !g_strcmp0(i->actor, actor) && low && !g_strcmp0(i->target, low))
            hit = l;
    }
    for (GList *l = t->items.head; l && !hit; l = l->next) {
        Intent *i = l->data;
        if (!i->closed && !g_strcmp0(i->method, method) && !g_strcmp0(i->actor, actor))
            hit = l;
    }
    if (hit) {
        Intent *i = hit->data;
        if (!g_strcmp0(result, "denied")) {
            g_queue_delete_link(&t->items, hit);
            _intent_free(i);
        } else {
            i->closed = TRUE;
            i->end_ms = end_ms;
            i->result = g_strdup(!g_strcmp0(result, "ok") ? "ok" : "fail");
            if (tenant_out)
                *tenant_out = g_strdup(i->actor_tenant);
            done = TRUE;
        }
    }
    g_free(low);
    return done;
}

static gboolean
_names_has(const gchar *const *names, const gchar *target)
{
    for (guint k = 0; names && names[k]; k++) {
        if (!g_ascii_strcasecmp(names[k], target))
            return TRUE;
    }
    return FALSE;
}

void
pcv_vm_cfg_intents_match(PcvVmCfgIntents *t, const gchar *const *names, gint64 window_start, gint64 observed_at,
                         PcvVmCfgAttribution *out)
{
    memset(out, 0, sizeof *out);
    if (!t)
        return;
    GPtrArray *hits = g_ptr_array_new();
    for (GList *l = t->items.head; l; l = l->next) {
        Intent *i = l->data;
        if (!_names_has(names, i->target) || i->start_ms > observed_at)
            continue;
        if (window_start > 0 && i->closed && i->end_ms < window_start)
            continue;
        g_ptr_array_add(hits, l);
    }
    if (hits->len == 1) {
        GList *l = g_ptr_array_index(hits, 0);
        Intent *i = l->data;
        out->kind = PCV_VMCH_ATTR_RPC;
        out->method = g_strdup(i->method);
        out->actor = g_strdup(i->actor);
        out->actor_tenant = g_strdup(i->actor_tenant);
        out->result = g_strdup(i->result);
        out->resize_dev = g_strdup(i->resize_dev);
        out->resize_bytes = i->resize_bytes;
        if (i->closed) {
            g_queue_delete_link(&t->items, l);
            _intent_free(i);
        }
    } else if (hits->len > 1) {
        out->kind = PCV_VMCH_ATTR_AMBIGUOUS;
        GString *m = g_string_new("[");
        GPtrArray *seen = g_ptr_array_new();
        for (guint k = 0; k < hits->len; k++) {
            Intent *i = ((GList *)g_ptr_array_index(hits, k))->data;
            gboolean dup = FALSE;
            for (guint s = 0; s < seen->len; s++)
                dup = dup || !strcmp(g_ptr_array_index(seen, s), i->method);
            if (dup)
                continue;
            g_ptr_array_add(seen, i->method);
            g_string_append_printf(m, "%s\"%s\"", seen->len > 1 ? "," : "", i->method);
        }
        g_string_append_c(m, ']');
        out->method = g_string_free(m, FALSE);
        g_ptr_array_unref(seen);
    }
    g_ptr_array_unref(hits);
}

gboolean
pcv_vm_cfg_intents_take_resize(PcvVmCfgIntents *t, const gchar *method, const gchar *actor, const gchar *target,
                               PcvVmCfgAttribution *out)
{
    memset(out, 0, sizeof *out);
    if (!t || !method || !target)
        return FALSE;
    gchar *low = g_ascii_strdown(target, -1);
    gboolean found = FALSE;
    for (GList *l = t->items.head; l; l = l->next) {
        Intent *i = l->data;
        if (!i->closed || !i->resize_dev || g_strcmp0(i->method, method) || g_strcmp0(i->actor, actor) ||
            g_strcmp0(i->target, low))
            continue;
        out->kind = PCV_VMCH_ATTR_RPC;
        out->method = g_strdup(i->method);
        out->actor = g_strdup(i->actor);
        out->actor_tenant = g_strdup(i->actor_tenant);
        out->result = g_strdup(i->result);
        out->resize_dev = g_strdup(i->resize_dev);
        out->resize_bytes = i->resize_bytes;
        g_queue_delete_link(&t->items, l);
        _intent_free(i);
        found = TRUE;
        break;
    }
    g_free(low);
    return found;
}

void
pcv_vm_cfg_intents_expire(PcvVmCfgIntents *t, gint64 now_ms)
{
    if (!t)
        return;
    GList *l = t->items.head;
    while (l) {
        GList *next = l->next;
        Intent *i = l->data;
        gboolean old = i->closed ? (now_ms - i->end_ms > PCV_VMCH_INTENT_CLOSED_TTL_MS)
                                 : (now_ms - i->start_ms > PCV_VMCH_INTENT_OPEN_MAX_MS);
        if (old) {
            g_queue_delete_link(&t->items, l);
            _intent_free(i);
        }
        l = next;
    }
}

guint
pcv_vm_cfg_intents_size(const PcvVmCfgIntents *t)
{
    return t ? g_queue_get_length((GQueue *)&t->items) : 0;
}

guint64
pcv_vm_cfg_intents_dropped(const PcvVmCfgIntents *t)
{
    return t ? t->dropped : 0;
}

void
pcv_vm_cfg_attribution_clear(PcvVmCfgAttribution *a)
{
    if (!a)
        return;
    g_free(a->method);
    g_free(a->actor);
    g_free(a->actor_tenant);
    g_free(a->result);
    g_free(a->resize_dev);
    memset(a, 0, sizeof *a);
}

const gchar *
pcv_vm_cfg_attribution_name(PcvVmCfgAttrKind k)
{
    return k == PCV_VMCH_ATTR_RPC ? "rpc" : (k == PCV_VMCH_ATTR_AMBIGUOUS ? "rpc_ambiguous" : "none");
}

gboolean
pcv_vm_cfg_late_fill_allowed(const gchar *attribution, gint64 observed_at, gboolean already_filled, gint64 now_ms)
{
    return !already_filled && !g_strcmp0(attribution, "none") && now_ms >= observed_at &&
           now_ms - observed_at <= PCV_VMCH_LATE_FILL_MS;
}

static void
_gap_add(GArray *out, gint64 s, gint64 e, const gchar *reason, gint64 since, gint64 until)
{
    gint64 a = MAX(s, since), b = MIN(e, until);
    if (b > a) {
        PcvVmCfgGap g = {.start = a, .end = b, .reason = reason};
        g_array_append_val(out, g);
    }
}

static const gchar *
_reason(const gchar *r)
{
    static const gchar *const known[] = {"shutdown", "listener_lost", "crash_inferred", "store_write_failed", "disabled",
                                         NULL};
    for (guint i = 0; known[i]; i++) {
        if (!g_strcmp0(r, known[i]))
            return known[i];
    }
    return "crash_inferred";
}

GArray *
pcv_vm_cfg_gaps(const PcvVmCfgEpoch *epochs, guint n, gint64 since, gint64 until, gint64 pruned_before)
{
    GArray *out = g_array_new(FALSE, FALSE, sizeof(PcvVmCfgGap));
    if (pruned_before > since)
        _gap_add(out, since, pruned_before, "pruned", since, until);
    for (guint i = 0; i < n; i++) {
        const PcvVmCfgEpoch *e = &epochs[i];
        gboolean last = (i + 1 == n);
        if (e->ended_at <= 0 && last)
            break;
        gint64 end = e->ended_at > 0 ? e->ended_at : e->last_alive_at;
        const gchar *reason = e->ended_at > 0 ? _reason(e->end_reason) : "crash_inferred";
        gint64 next = last ? until : epochs[i + 1].started_at;
        _gap_add(out, MAX(end, pruned_before), next, reason, since, until);
    }
    return out;
}

gint
pcv_vm_cfg_prune_pick(const PcvVmCfgUuidCount *counts, guint n, guint *take)
{
    gint best = -1;
    guint second = 0;
    for (guint i = 0; i < n; i++) {
        if (counts[i].rows == 0)
            continue;
        if (best < 0 || counts[i].rows > counts[best].rows ||
            (counts[i].rows == counts[best].rows && g_strcmp0(counts[i].uuid, counts[best].uuid) < 0)) {
            if (best >= 0)
                second = MAX(second, counts[best].rows);
            best = (gint)i;
        } else {
            second = MAX(second, counts[i].rows);
        }
    }
    if (take)
        *take = best < 0 ? 0 : MAX(1u, counts[best].rows - MIN(second, counts[best].rows) + 1u);
    if (take && best >= 0 && *take > counts[best].rows)
        *take = counts[best].rows;
    return best;
}

guint
pcv_vm_cfg_prune_plan(const PcvVmCfgUuidCount *counts, guint n, guint budget, guint *plan)
{
    if (!counts || !plan || !n || !budget)
        return 0;
    PcvVmCfgUuidCount *left = g_new(PcvVmCfgUuidCount, n);
    memcpy(left, counts, n * sizeof *left);
    guint planned = 0;
    while (planned < budget) {
        guint take = 0;
        gint i = pcv_vm_cfg_prune_pick(left, n, &take);
        if (i < 0)
            break;
        take = MIN(take, budget - planned);
        plan[i] += take;
        left[i].rows -= take;
        planned += take;
    }
    g_free(left);
    return planned;
}

gboolean
pcv_vm_cfg_coalesce_active(guint rows_last_hour)
{
    return rows_last_hour > PCV_VMCH_COALESCE_ROWS_PER_H;
}
