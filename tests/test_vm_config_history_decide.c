#include <glib.h>
#include <string.h>

#include "../src/modules/virt/vm_config_history_decide.h"
#include "../src/modules/virt/vm_config_history_norm.h"

#define T0 1760000000000LL

static PcvVmCfgLatest
latest_stopped(void)
{
    return (PcvVmCfgLatest){.have_row = TRUE, .present = TRUE, .config_digest = "C1", .run_id = -1,
                            .salt_id = "S1", .normalizer_version = PCV_VMCH_NORMALIZER_VERSION};
}

static PcvVmCfgObservation
obs_stopped(gint64 now)
{
    return (PcvVmCfgObservation){.exists = TRUE, .persistent = TRUE, .config_digest = "C1", .domain_id = -1,
                                 .now_ms = now, .salt_id = "S1", .normalizer_version = PCV_VMCH_NORMALIZER_VERSION};
}

static void
apply(PcvVmCfgLatest *l, const PcvVmCfgObservation *o, const PcvVmCfgDecision *d)
{
    l->present = d->present_after;
    if (d->write_row) {
        l->have_row = TRUE;
        l->config_digest = o->config_digest;
    }
    switch (d->run_action) {
    case PCV_VMCH_RUN_SET:
        l->run_id = o->domain_id;
        l->run_started_at = o->now_ms;
        l->run_live_digest = o->live_digest;
        break;
    case PCV_VMCH_RUN_UPDATE_LIVE:
        l->run_live_digest = o->live_digest;
        break;
    case PCV_VMCH_RUN_CLEAR:
        l->run_id = -1;
        l->run_live_digest = NULL;
        break;
    default:
        break;
    }
}

static void
test_decide_same_no_row(void)
{
    PcvVmCfgLatest l = latest_stopped();
    PcvVmCfgObservation o = obs_stopped(T0);
    PcvVmCfgDecision d = pcv_vm_cfg_decide(&l, &o, FALSE, 0);
    g_assert_false(d.write_row);
    g_assert_true(d.present_after);
}

static void
test_decide_config_change_row(void)
{
    PcvVmCfgLatest l = latest_stopped();
    PcvVmCfgObservation o = obs_stopped(T0);
    o.config_digest = "C2";
    PcvVmCfgDecision d = pcv_vm_cfg_decide(&l, &o, FALSE, T0 - 900000);
    g_assert_true(d.write_row);
    g_assert_cmpstr(d.kind, ==, "changed");
    g_assert_cmpstr(d.scope, ==, "config");
    g_assert_cmpint(d.window_start, ==, T0 - 900000);
    g_assert_null(d.cause_suffix);
}

static void
test_decide_start_stop_no_row(void)
{
    PcvVmCfgLatest l = latest_stopped();
    guint rows = 0;
    gint64 now = T0;
    for (gint run = 0; run < 10; run++) {
        PcvVmCfgObservation started = obs_stopped(now);
        started.domain_id = 100 + run;
        started.live_digest = "L-empty";
        PcvVmCfgDecision d = pcv_vm_cfg_decide(&l, &started, FALSE, 0);
        rows += d.write_row;
        apply(&l, &started, &d);
        now += 3000;
        PcvVmCfgObservation pinned = started;
        pinned.now_ms = now;
        pinned.live_digest = "L-pinned";
        d = pcv_vm_cfg_decide(&l, &pinned, FALSE, 0);
        rows += d.write_row;
        apply(&l, &pinned, &d);
        now += 900000;
        PcvVmCfgObservation scan = pinned;
        scan.now_ms = now;
        d = pcv_vm_cfg_decide(&l, &scan, FALSE, now - 900000);
        rows += d.write_row;
        apply(&l, &scan, &d);
        now += 1000;
        PcvVmCfgObservation stopped = obs_stopped(now);
        d = pcv_vm_cfg_decide(&l, &stopped, FALSE, 0);
        rows += d.write_row;
        g_assert_cmpint(d.run_action, ==, PCV_VMCH_RUN_CLEAR);
        apply(&l, &stopped, &d);
    }
    g_assert_cmpuint(rows, ==, 0);
}

static void
test_decide_settle_absorbs_pin(void)
{
    PcvVmCfgLatest l = latest_stopped();
    PcvVmCfgObservation o = obs_stopped(T0);
    o.domain_id = 7;
    o.live_digest = "L0";
    PcvVmCfgDecision d = pcv_vm_cfg_decide(&l, &o, FALSE, 0);
    g_assert_false(d.write_row);
    g_assert_cmpint(d.run_action, ==, PCV_VMCH_RUN_SET);
    apply(&l, &o, &d);
    o.now_ms = T0 + 3000;
    o.live_digest = "L-pin";
    d = pcv_vm_cfg_decide(&l, &o, FALSE, 0);
    g_assert_false(d.write_row);
    g_assert_cmpint(d.run_action, ==, PCV_VMCH_RUN_UPDATE_LIVE);
    apply(&l, &o, &d);
    o.now_ms = T0 + 10500;
    o.settle_close = TRUE;
    o.live_digest = "L-pin2";
    d = pcv_vm_cfg_decide(&l, &o, FALSE, 0);
    g_assert_false(d.write_row);
    g_assert_cmpstr(l.run_live_digest, ==, "L-pin");
    apply(&l, &o, &d);
    g_assert_cmpstr(l.run_live_digest, ==, "L-pin2");
}

static void
test_decide_live_change_after_settle(void)
{
    PcvVmCfgLatest l = latest_stopped();
    l.run_id = 7;
    l.run_started_at = T0;
    l.run_live_digest = "L-pin";
    PcvVmCfgObservation o = obs_stopped(T0 + 15000);
    o.domain_id = 7;
    o.live_digest = "L-iotune";
    PcvVmCfgDecision d = pcv_vm_cfg_decide(&l, &o, FALSE, 0);
    g_assert_true(d.write_row);
    g_assert_cmpstr(d.scope, ==, "live");
    g_assert_cmpint(d.run_action, ==, PCV_VMCH_RUN_UPDATE_LIVE);
    o.config_digest = "C9";
    d = pcv_vm_cfg_decide(&l, &o, FALSE, 0);
    g_assert_cmpstr(d.scope, ==, "both");
}

static void
test_decide_balloon_ignored(void)
{
    const gchar *inactive = "<domain><name>b</name><uuid>00000000-0000-4000-8000-000000000001</uuid>"
                            "<memory unit='KiB'>4194304</memory><currentMemory unit='KiB'>4194304</currentMemory></domain>";
    const gchar *live1 = "<domain id='3'><name>b</name><uuid>00000000-0000-4000-8000-000000000001</uuid>"
                         "<memory unit='KiB'>4194304</memory><currentMemory unit='KiB'>4000000</currentMemory></domain>";
    const gchar *live2 = "<domain id='3'><name>b</name><uuid>00000000-0000-4000-8000-000000000001</uuid>"
                         "<memory unit='KiB'>4194304</memory><currentMemory unit='KiB'>3100000</currentMemory></domain>";
    PcvVmCfgSnapshot s1, s2;
    g_assert_true(pcv_vm_cfg_normalize(inactive, strlen(inactive), live1, strlen(live1), FALSE, NULL, 0, &s1, NULL));
    g_assert_true(pcv_vm_cfg_normalize(inactive, strlen(inactive), live2, strlen(live2), FALSE, NULL, 0, &s2, NULL));
    PcvVmCfgLatest l = latest_stopped();
    l.config_digest = s1.config_digest;
    l.run_id = 3;
    l.run_started_at = T0;
    l.run_live_digest = s1.live_digest;
    PcvVmCfgObservation o = obs_stopped(T0 + 600000);
    o.config_digest = s2.config_digest;
    o.domain_id = 3;
    o.live_digest = s2.live_digest;
    PcvVmCfgDecision d = pcv_vm_cfg_decide(&l, &o, FALSE, 0);
    g_assert_false(d.write_row);
    pcv_vm_cfg_snapshot_clear(&s1);
    pcv_vm_cfg_snapshot_clear(&s2);
}

static void
test_decide_transient_is_undefined(void)
{
    PcvVmCfgLatest l = latest_stopped();
    PcvVmCfgObservation o = obs_stopped(T0);
    o.domain_id = 9;
    o.persistent = FALSE;
    PcvVmCfgDecision d = pcv_vm_cfg_decide(&l, &o, FALSE, 0);
    g_assert_true(d.write_row);
    g_assert_cmpstr(d.kind, ==, "undefined");
    g_assert_true(d.transient);
    g_assert_false(d.present_after);
    l.present = FALSE;
    d = pcv_vm_cfg_decide(&l, &o, FALSE, 0);
    g_assert_false(d.write_row);
    o.exists = FALSE;
    l.present = TRUE;
    d = pcv_vm_cfg_decide(&l, &o, FALSE, 0);
    g_assert_cmpstr(d.kind, ==, "undefined");
    g_assert_false(d.transient);
    l.present = FALSE;
    o.exists = TRUE;
    o.persistent = TRUE;
    d = pcv_vm_cfg_decide(&l, &o, FALSE, 0);
    g_assert_true(d.write_row);
    g_assert_cmpstr(d.kind, ==, "changed");
}

static void
test_decide_created_in_gap(void)
{
    PcvVmCfgLatest l = {.run_id = -1};
    PcvVmCfgObservation o = obs_stopped(T0);
    PcvVmCfgDecision d = pcv_vm_cfg_decide(&l, &o, FALSE, T0 - 5000);
    g_assert_true(d.write_row);
    g_assert_cmpstr(d.kind, ==, "created");
    g_assert_cmpint(d.window_start, ==, T0 - 5000);
    o.persistent = FALSE;
    d = pcv_vm_cfg_decide(&l, &o, FALSE, 0);
    g_assert_false(d.write_row);
}

static void
test_decide_first_boot_baseline(void)
{
    PcvVmCfgLatest l = {.run_id = -1};
    PcvVmCfgObservation o = obs_stopped(T0);
    o.domain_id = 4;
    o.live_digest = "L";
    PcvVmCfgDecision d = pcv_vm_cfg_decide(&l, &o, TRUE, T0 - 5000);
    g_assert_cmpstr(d.kind, ==, "baseline");
    g_assert_cmpint(d.window_start, ==, 0);
    g_assert_cmpint(d.run_action, ==, PCV_VMCH_RUN_SET);
}

static void
test_decide_salt_rotated_no_false_change(void)
{
    PcvVmCfgLatest l = latest_stopped();
    PcvVmCfgObservation o = obs_stopped(T0);
    o.salt_id = "S2";
    o.config_digest = "C1-new-salt";
    o.rebase_changes = 0;
    PcvVmCfgDecision d = pcv_vm_cfg_decide(&l, &o, FALSE, 0);
    g_assert_false(d.write_row);
    g_assert_true(d.diff_flags & PCV_VMCH_DIFF_IGNORE_OPAQUE);
    o.rebase_changes = 1;
    d = pcv_vm_cfg_decide(&l, &o, FALSE, 0);
    g_assert_true(d.write_row);
    g_assert_cmpstr(d.cause_suffix, ==, "+salt_rotated");
}

static void
test_decide_normalizer_upgrade(void)
{
    PcvVmCfgLatest l = latest_stopped();
    l.normalizer_version = PCV_VMCH_NORMALIZER_VERSION - 1;
    PcvVmCfgObservation o = obs_stopped(T0);
    o.config_digest = "C1-v2";
    PcvVmCfgDecision d = pcv_vm_cfg_decide(&l, &o, FALSE, 0);
    g_assert_false(d.write_row);
    g_assert_true(d.diff_flags & PCV_VMCH_DIFF_IGNORE_NEW_PATHS);
    o.rebase_changes = 2;
    d = pcv_vm_cfg_decide(&l, &o, FALSE, 0);
    g_assert_cmpstr(d.cause_suffix, ==, "+normalizer_upgrade");
}

static const gchar *const k_names[] = {"6f1c2d3e-4a5b-4c6d-8e9f-0a1b2c3d4e5f", "web", NULL};

static void
test_intents_single(void)
{
    PcvVmCfgIntents *t = pcv_vm_cfg_intents_new(0);
    pcv_vm_cfg_intents_open(t, "vm.set_vcpu", "WEB", "alice", "t1", T0, NULL, 0);
    g_assert_true(pcv_vm_cfg_intents_close(t, "vm.set_vcpu", "alice", "web", "ok", T0 + 100, NULL));
    PcvVmCfgAttribution a;
    pcv_vm_cfg_intents_match(t, k_names, 0, T0 + 2100, &a);
    g_assert_cmpint(a.kind, ==, PCV_VMCH_ATTR_RPC);
    g_assert_cmpstr(a.actor, ==, "alice");
    g_assert_cmpstr(a.actor_tenant, ==, "t1");
    g_assert_cmpstr(a.method, ==, "vm.set_vcpu");
    g_assert_cmpstr(a.result, ==, "ok");
    pcv_vm_cfg_attribution_clear(&a);
    pcv_vm_cfg_intents_match(t, k_names, 0, T0 + 2200, &a);
    g_assert_cmpint(a.kind, ==, PCV_VMCH_ATTR_NONE);
    pcv_vm_cfg_intents_free(t);
}

static void
test_intents_ambiguous(void)
{
    PcvVmCfgIntents *t = pcv_vm_cfg_intents_new(0);
    pcv_vm_cfg_intents_open(t, "vm.set_vcpu", "web", "alice", NULL, T0, NULL, 0);
    pcv_vm_cfg_intents_open(t, "vm.limit", "web", "bob", NULL, T0 + 10, NULL, 0);
    PcvVmCfgAttribution a;
    pcv_vm_cfg_intents_match(t, k_names, 0, T0 + 2000, &a);
    g_assert_cmpint(a.kind, ==, PCV_VMCH_ATTR_AMBIGUOUS);
    g_assert_null(a.actor);
    g_assert_cmpstr(a.method, ==, "[\"vm.set_vcpu\",\"vm.limit\"]");
    g_assert_cmpstr(pcv_vm_cfg_attribution_name(a.kind), ==, "rpc_ambiguous");
    pcv_vm_cfg_attribution_clear(&a);
    pcv_vm_cfg_intents_free(t);
}

static void
test_intents_none(void)
{
    PcvVmCfgIntents *t = pcv_vm_cfg_intents_new(0);
    pcv_vm_cfg_intents_open(t, "vm.set_vcpu", "db", "alice", NULL, T0, NULL, 0);
    pcv_vm_cfg_intents_open(t, "vm.set_vcpu", "web", "alice", NULL, T0 + 5000, NULL, 0);
    PcvVmCfgAttribution a;
    pcv_vm_cfg_intents_match(t, k_names, 0, T0 + 1000, &a);
    g_assert_cmpint(a.kind, ==, PCV_VMCH_ATTR_NONE);
    g_assert_cmpstr(pcv_vm_cfg_attribution_name(a.kind), ==, "none");
    pcv_vm_cfg_intents_free(t);
}

static void
test_intents_late_fill_30s_once(void)
{
    g_assert_true(pcv_vm_cfg_late_fill_allowed("none", T0, FALSE, T0 + 30000));
    g_assert_false(pcv_vm_cfg_late_fill_allowed("none", T0, FALSE, T0 + 30001));
    g_assert_false(pcv_vm_cfg_late_fill_allowed("none", T0, TRUE, T0 + 1000));
    g_assert_false(pcv_vm_cfg_late_fill_allowed("rpc", T0, FALSE, T0 + 1000));
    g_assert_false(pcv_vm_cfg_late_fill_allowed("none", T0, FALSE, T0 - 1));
}

static void
test_intents_async_open_15min(void)
{
    PcvVmCfgIntents *t = pcv_vm_cfg_intents_new(0);
    pcv_vm_cfg_intents_open(t, "vm.limit", "web", "alice", NULL, T0, NULL, 0);
    pcv_vm_cfg_intents_expire(t, T0 + 14 * 60000);
    PcvVmCfgAttribution a;
    pcv_vm_cfg_intents_match(t, k_names, 0, T0 + 14 * 60000, &a);
    g_assert_cmpint(a.kind, ==, PCV_VMCH_ATTR_RPC);
    g_assert_null(a.result);
    pcv_vm_cfg_attribution_clear(&a);
    g_assert_cmpuint(pcv_vm_cfg_intents_size(t), ==, 1);
    pcv_vm_cfg_intents_expire(t, T0 + 15 * 60000 + 1);
    g_assert_cmpuint(pcv_vm_cfg_intents_size(t), ==, 0);
    pcv_vm_cfg_intents_open(t, "vm.limit", "web", "alice", NULL, T0, NULL, 0);
    pcv_vm_cfg_intents_close(t, "vm.limit", "alice", "web", "ok", T0 + 1000, NULL);
    pcv_vm_cfg_intents_expire(t, T0 + 31000);
    g_assert_cmpuint(pcv_vm_cfg_intents_size(t), ==, 1);
    pcv_vm_cfg_intents_expire(t, T0 + 31001);
    g_assert_cmpuint(pcv_vm_cfg_intents_size(t), ==, 0);
    pcv_vm_cfg_intents_free(t);
}

static void
test_intents_fail_result(void)
{
    PcvVmCfgIntents *t = pcv_vm_cfg_intents_new(0);
    pcv_vm_cfg_intents_open(t, "vm.create", "web", "alice", NULL, T0, NULL, 0);
    g_assert_true(pcv_vm_cfg_intents_close(t, "vm.create", "alice", "web", "fail", T0 + 50, NULL));
    PcvVmCfgAttribution a;
    pcv_vm_cfg_intents_match(t, k_names, 0, T0 + 100, &a);
    g_assert_cmpint(a.kind, ==, PCV_VMCH_ATTR_RPC);
    g_assert_cmpstr(a.result, ==, "fail");
    pcv_vm_cfg_attribution_clear(&a);
    pcv_vm_cfg_intents_free(t);
}

static void
test_intents_denied_dropped(void)
{
    PcvVmCfgIntents *t = pcv_vm_cfg_intents_new(0);
    pcv_vm_cfg_intents_open(t, "vm.set_vcpu", "web", "mallory", NULL, T0, NULL, 0);
    g_assert_false(pcv_vm_cfg_intents_close(t, "vm.set_vcpu", "bob", "web", "ok", T0 + 1, NULL));
    g_assert_false(pcv_vm_cfg_intents_close(t, "vm.set_vcpu", "mallory", "web", "denied", T0 + 1, NULL));
    g_assert_cmpuint(pcv_vm_cfg_intents_size(t), ==, 0);
    pcv_vm_cfg_intents_free(t);
}

static void
test_intents_overflow_counted(void)
{
    PcvVmCfgIntents *t = pcv_vm_cfg_intents_new(4);
    for (gint i = 0; i < 6; i++)
        pcv_vm_cfg_intents_open(t, "vm.set_vcpu", i == 0 ? "first" : "web", "a", NULL, T0 + i, NULL, 0);
    g_assert_cmpuint(pcv_vm_cfg_intents_size(t), ==, 4);
    g_assert_cmpuint(pcv_vm_cfg_intents_dropped(t), ==, 2);
    const gchar *const first[] = {"first", NULL};
    PcvVmCfgAttribution a;
    pcv_vm_cfg_intents_match(t, first, 0, T0 + 10, &a);
    g_assert_cmpint(a.kind, ==, PCV_VMCH_ATTR_NONE);
    pcv_vm_cfg_intents_free(t);
}

static void
test_intents_resize_values(void)
{
    PcvVmCfgIntents *t = pcv_vm_cfg_intents_new(0);
    pcv_vm_cfg_intents_open(t, "vm.resize_disk", "web", "alice", NULL, T0, "vdb", 21474836480LL);
    pcv_vm_cfg_intents_close(t, "vm.resize_disk", "alice", "web", "ok", T0 + 10, NULL);
    PcvVmCfgAttribution a;
    pcv_vm_cfg_intents_match(t, k_names, 0, T0 + 20, &a);
    g_assert_cmpstr(a.resize_dev, ==, "vdb");
    g_assert_cmpint(a.resize_bytes, ==, 21474836480LL);
    pcv_vm_cfg_attribution_clear(&a);
    pcv_vm_cfg_intents_free(t);
}

static void
test_gaps_overlap(void)
{
    PcvVmCfgEpoch e[3] = {
        {.id = 1, .started_at = 1000, .last_alive_at = 1900, .ended_at = 2000, .end_reason = "shutdown"},
        {.id = 2, .started_at = 3000, .last_alive_at = 3500, .ended_at = 0},
        {.id = 3, .started_at = 5000, .last_alive_at = 6000, .ended_at = 0},
    };
    GArray *g = pcv_vm_cfg_gaps(e, 3, 0, 10000, 0);
    g_assert_cmpuint(g->len, ==, 2);
    PcvVmCfgGap *a = &g_array_index(g, PcvVmCfgGap, 0), *b = &g_array_index(g, PcvVmCfgGap, 1);
    g_assert_cmpint(a->start, ==, 2000);
    g_assert_cmpint(a->end, ==, 3000);
    g_assert_cmpstr(a->reason, ==, "shutdown");
    g_assert_cmpint(b->start, ==, 3500);
    g_assert_cmpint(b->end, ==, 5000);
    g_assert_cmpstr(b->reason, ==, "crash_inferred");
    g_array_unref(g);
    g = pcv_vm_cfg_gaps(e, 3, 2500, 4000, 0);
    g_assert_cmpuint(g->len, ==, 2);
    g_assert_cmpint(g_array_index(g, PcvVmCfgGap, 0).start, ==, 2500);
    g_assert_cmpint(g_array_index(g, PcvVmCfgGap, 1).end, ==, 4000);
    g_array_unref(g);
}

static void
test_gaps_crash_inferred(void)
{
    PcvVmCfgEpoch e[1] = {{.id = 1, .started_at = 1000, .last_alive_at = 1500, .ended_at = 1500,
                           .end_reason = "crash_inferred"}};
    GArray *g = pcv_vm_cfg_gaps(e, 1, 0, 3000, 0);
    g_assert_cmpuint(g->len, ==, 1);
    g_assert_cmpint(g_array_index(g, PcvVmCfgGap, 0).start, ==, 1500);
    g_assert_cmpstr(g_array_index(g, PcvVmCfgGap, 0).reason, ==, "crash_inferred");
    g_array_unref(g);
    g = pcv_vm_cfg_gaps(NULL, 0, 0, 3000, 0);
    g_assert_cmpuint(g->len, ==, 0);
    g_array_unref(g);
}

static void
test_gaps_pruned(void)
{
    PcvVmCfgEpoch e[2] = {
        {.id = 5, .started_at = 4000, .last_alive_at = 4500, .ended_at = 4500, .end_reason = "listener_lost"},
        {.id = 6, .started_at = 6000, .last_alive_at = 7000, .ended_at = 0},
    };
    GArray *g = pcv_vm_cfg_gaps(e, 2, 0, 9000, 5000);
    g_assert_cmpuint(g->len, ==, 2);
    g_assert_cmpstr(g_array_index(g, PcvVmCfgGap, 0).reason, ==, "pruned");
    g_assert_cmpint(g_array_index(g, PcvVmCfgGap, 0).end, ==, 5000);
    g_assert_cmpint(g_array_index(g, PcvVmCfgGap, 1).start, ==, 5000);
    g_assert_cmpstr(g_array_index(g, PcvVmCfgGap, 1).reason, ==, "listener_lost");
    g_array_unref(g);
}

static void
test_prune_fairness(void)
{
    PcvVmCfgUuidCount c[3] = {{"aaa", 5}, {"noisy", 40}, {"bbb", 2}};
    guint take = 0;
    gint i = pcv_vm_cfg_prune_pick(c, 3, &take);
    g_assert_cmpint(i, ==, 1);
    g_assert_cmpuint(take, ==, 36);
    c[1].rows = 5;
    i = pcv_vm_cfg_prune_pick(c, 3, &take);
    g_assert_cmpint(i, ==, 0);
    g_assert_cmpuint(take, ==, 1);
    PcvVmCfgUuidCount none[2] = {{"a", 0}, {"b", 0}};
    g_assert_cmpint(pcv_vm_cfg_prune_pick(none, 2, &take), ==, -1);
    g_assert_cmpuint(take, ==, 0);
}

static void
test_prune_plan_ties_progress(void)
{
    enum { N = 300 };
    PcvVmCfgUuidCount c[N];
    gchar names[N][8];
    for (guint i = 0; i < N; i++) {
        g_snprintf(names[i], sizeof names[i], "u%03u", i);
        c[i].uuid = names[i];
        c[i].rows = 10;
    }
    guint plan[N] = {0};
    g_assert_cmpuint(pcv_vm_cfg_prune_plan(c, N, 500, plan), ==, 500);
    guint lo = G_MAXUINT, hi = 0, sum = 0;
    for (guint i = 0; i < N; i++) {
        lo = MIN(lo, c[i].rows - plan[i]);
        hi = MAX(hi, c[i].rows - plan[i]);
        sum += plan[i];
    }
    g_assert_cmpuint(sum, ==, 500);
    g_assert_cmpuint(hi - lo, <=, 1);

    PcvVmCfgUuidCount d[3] = {{"aaa", 5}, {"noisy", 40}, {"bbb", 2}};
    guint p2[3] = {0};
    g_assert_cmpuint(pcv_vm_cfg_prune_plan(d, 3, 30, p2), ==, 30);
    g_assert_cmpuint(p2[1], ==, 30);

    guint p3[3] = {0};
    g_assert_cmpuint(pcv_vm_cfg_prune_plan(d, 3, 1000, p3), ==, 47);
}

static void
test_coalesce_threshold(void)
{
    g_assert_false(pcv_vm_cfg_coalesce_active(30));
    g_assert_true(pcv_vm_cfg_coalesce_active(31));
}

void test_vm_config_history_decide_register(void);
void
test_vm_config_history_decide_register(void)
{
    g_test_add_func("/vm_config_history/decide/decide_same_no_row", test_decide_same_no_row);
    g_test_add_func("/vm_config_history/decide/decide_config_change_row", test_decide_config_change_row);
    g_test_add_func("/vm_config_history/decide/decide_start_stop_no_row", test_decide_start_stop_no_row);
    g_test_add_func("/vm_config_history/decide/decide_settle_absorbs_pin", test_decide_settle_absorbs_pin);
    g_test_add_func("/vm_config_history/decide/decide_live_change_after_settle", test_decide_live_change_after_settle);
    g_test_add_func("/vm_config_history/decide/decide_balloon_ignored", test_decide_balloon_ignored);
    g_test_add_func("/vm_config_history/decide/decide_transient_is_undefined", test_decide_transient_is_undefined);
    g_test_add_func("/vm_config_history/decide/decide_created_in_gap", test_decide_created_in_gap);
    g_test_add_func("/vm_config_history/decide/decide_first_boot_baseline", test_decide_first_boot_baseline);
    g_test_add_func("/vm_config_history/decide/decide_salt_rotated_no_false_change",
                    test_decide_salt_rotated_no_false_change);
    g_test_add_func("/vm_config_history/decide/decide_normalizer_upgrade", test_decide_normalizer_upgrade);
    g_test_add_func("/vm_config_history/decide/intents_single", test_intents_single);
    g_test_add_func("/vm_config_history/decide/intents_ambiguous", test_intents_ambiguous);
    g_test_add_func("/vm_config_history/decide/intents_none", test_intents_none);
    g_test_add_func("/vm_config_history/decide/intents_late_fill_30s_once", test_intents_late_fill_30s_once);
    g_test_add_func("/vm_config_history/decide/intents_async_open_15min", test_intents_async_open_15min);
    g_test_add_func("/vm_config_history/decide/intents_fail_result", test_intents_fail_result);
    g_test_add_func("/vm_config_history/decide/intents_denied_dropped", test_intents_denied_dropped);
    g_test_add_func("/vm_config_history/decide/intents_overflow_counted", test_intents_overflow_counted);
    g_test_add_func("/vm_config_history/decide/intents_resize_values", test_intents_resize_values);
    g_test_add_func("/vm_config_history/decide/gaps_overlap", test_gaps_overlap);
    g_test_add_func("/vm_config_history/decide/gaps_crash_inferred", test_gaps_crash_inferred);
    g_test_add_func("/vm_config_history/decide/gaps_pruned", test_gaps_pruned);
    g_test_add_func("/vm_config_history/decide/prune_fairness", test_prune_fairness);
    g_test_add_func("/vm_config_history/decide/prune_plan_ties_progress", test_prune_plan_ties_progress);
    g_test_add_func("/vm_config_history/decide/coalesce_threshold", test_coalesce_threshold);
}
