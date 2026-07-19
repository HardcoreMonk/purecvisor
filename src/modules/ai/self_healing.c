
#include "self_healing.h"
#include "ai_agent.h"
#include "restart_breaker.h"
#include "self_healing_restart.h" /* 결정 로직 추출 seam (PCV_SAFETY_CONTROL: self-healing-restart) */
#include <string.h>
#include <stdio.h>
#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>
#include "modules/daemons/prometheus_exporter.h"
#include "modules/daemons/ebpf_telemetry.h"
#include "modules/audit/pcv_audit.h"
#include "modules/virt/virt_conn_pool.h"
#include "utils/pcv_worker_pool.h"
#include "utils/pcv_config.h"
#include "utils/pcv_log.h"

extern virDomainPtr pure_virt_get_domain(virConnectPtr conn, const gchar *identifier);

#define HEALING_LOG_DOM       "healing"

constexpr int MAX_POLICIES        = 16;

constexpr int MAX_PENDING         = 8;

constexpr int RATE_LIMIT_WINDOW   = 300;

constexpr int RATE_LIMIT_MAX      = 3;

constexpr int CIRCUIT_BREAKER_MAX = 3;

constexpr int HEALING_HISTORY_MAX = 100;

[[maybe_unused]] constexpr int COOLDOWN_MAX_SEC = 3600;

static_assert(MAX_POLICIES >= 1);
static_assert(MAX_PENDING >= 1);
static_assert(RATE_LIMIT_MAX >= 1);

typedef struct {
    gchar    name[64];
    gchar    trigger_metric[128];
    gdouble  trigger_zscore;
    gdouble  predict_threshold;
    gchar    action[32];
    gint     cooldown_sec;
    gint     base_cooldown_sec;
    gboolean require_approval;
    gboolean enabled;
    gint     policy_dry_run;

    gint64   last_trigger_us;
    gint     consecutive_failures;
} HealingPolicy;

typedef struct {
    gint     id;
    gchar    policy_name[64];
    gchar    action[32];
    gchar    reason[256];
    gint64   created_us;
    gboolean resolved;
} PendingAction;

typedef struct {
    gchar   action[64];
    gchar   target[128];
    gchar   reason[256];
    gchar   result[64];
    gint64  timestamp;
    gint64  duration_ms;
} HealingHistoryEntry;

static HealingHistoryEntry g_healing_history[HEALING_HISTORY_MAX];
static gint g_healing_hist_idx   = 0;
static gint g_healing_hist_count = 0;
static GMutex g_healing_hist_mu;

static struct {
    HealingPolicy policies[MAX_POLICIES];
    gint          policy_count;
    PendingAction pending[MAX_PENDING];
    gint          pending_pos;
    gint          pending_count;
    gint          next_action_id;
    GMutex        mu;
    gboolean      initialized;
    gboolean      dry_run;

    gint64        action_times[RATE_LIMIT_MAX];
    gint          action_time_pos;

    guint64       total_triggered;
    guint64       total_executed;
    guint64       total_pending;
} G = {0};

#define MULTI_ANOMALY_WINDOW_SEC  60
#define MULTI_ANOMALY_THRESHOLD   3
#define MULTI_ANOMALY_MAX_TRACK   16

typedef struct {
    gchar  metric[64];
    gint64 last_seen_us;
} AnomalyTrack;

static AnomalyTrack g_recent_anomalies[MULTI_ANOMALY_MAX_TRACK] = {0};
static gint64       g_last_multi_agent_us = 0;
static GMutex       g_anomaly_mu;

static gboolean
_track_distinct_anomaly(const gchar *metric)
{
    if (!metric || !*metric) return FALSE;
    gint64 now = g_get_monotonic_time();
    gint64 window_us = (gint64)MULTI_ANOMALY_WINDOW_SEC * G_USEC_PER_SEC;
    gboolean trigger = FALSE;

    g_mutex_lock(&g_anomaly_mu);

    gint distinct = 0;
    gint empty = -1;
    for (gint i = 0; i < MULTI_ANOMALY_MAX_TRACK; i++) {
        AnomalyTrack *t = &g_recent_anomalies[i];
        if (t->metric[0] == 0) { if (empty < 0) empty = i; continue; }
        if (now - t->last_seen_us > window_us) {
            t->metric[0] = 0;
            if (empty < 0) empty = i;
            continue;
        }
        if (g_strcmp0(t->metric, metric) == 0) {
            t->last_seen_us = now;
            distinct++;
            continue;
        }
        distinct++;
    }

    gboolean is_new = TRUE;
    for (gint i = 0; i < MULTI_ANOMALY_MAX_TRACK; i++) {
        if (g_strcmp0(g_recent_anomalies[i].metric, metric) == 0) { is_new = FALSE; break; }
    }
    if (is_new && empty >= 0) {
        g_strlcpy(g_recent_anomalies[empty].metric, metric,
                  sizeof(g_recent_anomalies[empty].metric));
        g_recent_anomalies[empty].last_seen_us = now;
        distinct++;
    }

    if (distinct >= MULTI_ANOMALY_THRESHOLD &&
        (now - g_last_multi_agent_us) > 300 * G_USEC_PER_SEC) {
        g_last_multi_agent_us = now;
        trigger = TRUE;
    }

    g_mutex_unlock(&g_anomaly_mu);
    return trigger;
}

gboolean pcv_healing_should_trigger_agent_now(void)
{
    g_mutex_lock(&g_anomaly_mu);
    gboolean recent = (g_get_monotonic_time() - g_last_multi_agent_us) < 60 * G_USEC_PER_SEC;
    g_mutex_unlock(&g_anomaly_mu);
    return recent;
}

static void
_add_policy(const gchar *name, const gchar *trigger_metric,
            gdouble trigger_zscore, gdouble predict_threshold,
            const gchar *action, gint cooldown_sec,
            gboolean require_approval)
{
    if (G.policy_count >= MAX_POLICIES) return;
    HealingPolicy *p = &G.policies[G.policy_count++];
    memset(p, 0, sizeof(*p));
    g_strlcpy(p->name, name, sizeof(p->name));
    g_strlcpy(p->trigger_metric, trigger_metric, sizeof(p->trigger_metric));
    p->trigger_zscore = trigger_zscore;
    p->predict_threshold = predict_threshold;
    g_strlcpy(p->action, action, sizeof(p->action));
    p->cooldown_sec = cooldown_sec;
    p->base_cooldown_sec = cooldown_sec;
    p->require_approval = require_approval;
    p->enabled = TRUE;
    p->policy_dry_run = -1;
}

static gboolean
_rate_check(void)
{
    gint64 now = g_get_monotonic_time();
    gint64 window = RATE_LIMIT_WINDOW * G_USEC_PER_SEC;

    gint recent = 0;
    for (gint i = 0; i < RATE_LIMIT_MAX; i++) {
        if (now - G.action_times[i] < window)
            recent++;
    }
    return recent < RATE_LIMIT_MAX;
}

static void
_rate_record(void)
{
    G.action_times[G.action_time_pos] = g_get_monotonic_time();
    G.action_time_pos = (G.action_time_pos + 1) % RATE_LIMIT_MAX;
}

static void
_record_healing_action(const gchar *action, const gchar *target,
                       const gchar *reason, const gchar *result,
                       gint64 duration_ms)
{
    g_mutex_lock(&g_healing_hist_mu);

    HealingHistoryEntry *e = &g_healing_history[g_healing_hist_idx];
    g_strlcpy(e->action, action ? action : "", sizeof(e->action));
    g_strlcpy(e->target, target ? target : "", sizeof(e->target));
    g_strlcpy(e->reason, reason ? reason : "", sizeof(e->reason));
    g_strlcpy(e->result, result ? result : "", sizeof(e->result));
    e->timestamp   = g_get_real_time() / G_USEC_PER_SEC;
    e->duration_ms = duration_ms;

    g_healing_hist_idx = (g_healing_hist_idx + 1) % HEALING_HISTORY_MAX;
    if (g_healing_hist_count < HEALING_HISTORY_MAX)
        g_healing_hist_count++;

    g_mutex_unlock(&g_healing_hist_mu);
}

typedef struct {
    gchar *policy_name;
    gchar *vm;
    gchar *reason;
} RestartCtx;

static void
_restart_ctx_free(gpointer data)
{
    RestartCtx *c = data;
    if (!c) return;
    g_free(c->policy_name);
    g_free(c->vm);
    g_free(c->reason);
    g_free(c);
}

/**
 * _vm_restart_worker — 워커 스레드에서 실제 VM 재시작을 수행한다.
 *
 * 실행 순서:
 *   1. virt_conn_pool_acquire() 로 libvirt 커넥션 획득
 *   2. pure_virt_get_domain() 다형성 조회 (UUID→이름)
 *   3. running-guard: virDomainIsActive()>0 이면 skip (이미 실행 중 — 재시작 안 함)
 *   4. virDomainCreate() 로 실제 기동
 *
 * 안전판:
 *   - 메인 스레드 블로킹 금지 (워커 풀에서 실행)
 *   - running-guard 로 정상 실행 중 VM 오재시작 차단
 *   - executed 카운트/WS "executed"/actions_total 은 **실제 재시작 성공 시에만** 갱신
 *     (running-guard skip·조회 실패·create 실패는 executed 로 세지 않는다 — 정직 보고)
 *   - 결과(success/skipped/failed)를 이력 링버퍼 + 감사 로그에 기록
 *
 * running-guard + virDomainCreate 결정 로직 자체는 self_healing_restart.c 로
 * 추출됐다(PCV_SAFETY_CONTROL 마커도 그쪽으로 이동 — 효과테스트가 닿는 코드에
 * 마커가 있어야 게이트가 검증 가능). 여기서는 libvirt 어댑터(_sh_domain_create)로
 * 감싸 위임한다.
 */

static int
_sh_domain_create(gpointer dom)
{
    return virDomainCreate((virDomainPtr)dom);
}

static void
_vm_restart_worker(GTask *task, gpointer src, gpointer task_data, GCancellable *c)
{
    (void)src; (void)c;
    RestartCtx *ctx = task_data;
    gint64 start_us = g_get_monotonic_time();
    const gchar *result = "failed";

    gint rb_feedback = 0;

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        PCV_LOG_WARN(HEALING_LOG_DOM,
            "[restart] VM '%s': libvirt 커넥션 획득 실패 — 재시작 중단", ctx->vm);
    } else {
        virDomainPtr dom = pure_virt_get_domain(conn, ctx->vm);
        if (!dom) {
            PCV_LOG_WARN(HEALING_LOG_DOM,
                "[restart] VM '%s' 조회 실패 (UUID/이름 불일치) — 재시작 중단", ctx->vm);
        } else {

            result = pcv_healing_restart_decide(virDomainIsActive(dom),
                                                _sh_domain_create, dom, &rb_feedback);
            if (g_strcmp0(result, "skipped") == 0) {

                PCV_LOG_INFO(HEALING_LOG_DOM,
                    "[restart] VM '%s' 이미 실행 중 — running-guard skip", ctx->vm);
            } else if (g_strcmp0(result, "success") == 0) {
                PCV_LOG_WARN(HEALING_LOG_DOM,
                    "[restart] VM '%s' 재시작 성공 (policy=%s)", ctx->vm, ctx->policy_name);
            } else {
                virErrorPtr err = virGetLastError();
                PCV_LOG_WARN(HEALING_LOG_DOM,
                    "[restart] VM '%s' 재시작 실패: %s", ctx->vm,
                    (err && err->message) ? err->message : "(unknown)");
            }
        }
        if (dom) virDomainFree(dom);
        virt_conn_pool_release(conn);
    }

    if (rb_feedback > 0)      rb_record(ctx->vm, TRUE);
    else if (rb_feedback < 0) rb_record(ctx->vm, FALSE);
    else                      rb_release_probe(ctx->vm);

    gint64 dur_ms = (g_get_monotonic_time() - start_us) / 1000;

    if (g_strcmp0(result, "success") == 0) {
        g_mutex_lock(&G.mu);
        G.total_executed++;
        gchar lbl[128];
        g_snprintf(lbl, sizeof(lbl), "policy=\"%s\",action=\"restart\"", ctx->policy_name);
        pcv_prom_gauge_set_labels("purecvisor_healing_actions_total", lbl,
                                  (gdouble)G.total_executed);
        g_mutex_unlock(&G.mu);

        extern void pcv_ws_broadcast(const gchar*, const gchar*);
        extern gint pcv_ws_client_count(void);
        if (pcv_ws_client_count() > 0) {
            gchar payload[512];
            g_snprintf(payload, sizeof(payload),
                "{\"policy\":\"%s\",\"action\":\"restart\",\"target\":\"%s\","
                "\"reason\":\"%s\",\"dry_run\":false,\"executed\":true}",
                ctx->policy_name, ctx->vm, ctx->reason);
            pcv_ws_broadcast("healing", payload);
        }
    }

    gchar detail[384];
    g_snprintf(detail, sizeof(detail), "action=restart target=%s result=%s reason=%s",
               ctx->vm, result, ctx->reason);
    pcv_audit_log("ai-ops", "healing_action", ctx->policy_name, detail, 0, 0, "local");
    _record_healing_action("restart", ctx->vm, ctx->reason, result, dur_ms);

    g_task_return_boolean(task, TRUE);
}

static void
_dispatch_vm_restart(const gchar *policy_name, const gchar *vm, const gchar *reason)
{
    RestartCtx *ctx = g_new0(RestartCtx, 1);
    ctx->policy_name = g_strdup(policy_name);
    ctx->vm          = g_strdup(vm);
    ctx->reason      = g_strdup(reason);

    GTask *task = g_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(task, ctx, _restart_ctx_free);
    pcv_worker_pool_push(task, _vm_restart_worker);
    g_object_unref(task);
}

static void
_execute_action(HealingPolicy *p, const gchar *reason, const gchar *target_vm)
{
    gint64 start_us = g_get_monotonic_time();

    gboolean restart_with_target =
        (g_strcmp0(p->action, "restart") == 0) && target_vm && *target_vm;

    gboolean effective_dry_run = (p->policy_dry_run >= 0)
        ? (gboolean)p->policy_dry_run : G.dry_run;

    if (effective_dry_run) {

        if (restart_with_target)
            PCV_LOG_INFO(HEALING_LOG_DOM,
                "[DRY RUN] Policy '%s' would restart VM '%s': %s",
                p->name, target_vm, reason);
        else
            PCV_LOG_INFO(HEALING_LOG_DOM,
                "[DRY RUN] Policy '%s' would execute '%s': %s",
                p->name, p->action, reason);

        gchar detail[384];
        g_snprintf(detail, sizeof(detail), "[DRY_RUN] action=%s reason=%s", p->action, reason);
        pcv_audit_log("ai-ops", "healing_dry_run", p->name, detail, 0, 0, "local");

        gchar metric_label[160];
        g_snprintf(metric_label, sizeof(metric_label), "policy=\"%s\"", p->name);
        pcv_prom_gauge_set_labels("purecvisor_healing_policy_triggered_total",
            metric_label, (gdouble)(++G.total_triggered));

        gint64 dur_ms = (g_get_monotonic_time() - start_us) / 1000;
        _record_healing_action(p->action, restart_with_target ? target_vm : p->name,
                               reason, "dry_run", dur_ms);
        return;
    }

    if (restart_with_target) {

        if (!rb_allow(target_vm)) {
            PCV_LOG_WARN(HEALING_LOG_DOM,
                "ACTION: Policy '%s' restart of VM '%s' SKIPPED — 재시작 브레이커 OPEN "
                "(반복 재시작 실패, cooldown 중): %s", p->name, target_vm, reason);

            gchar detail[384];
            g_snprintf(detail, sizeof(detail),
                "action=restart target=%s result=skipped reason=breaker-open trigger=%s",
                target_vm, reason);
            pcv_audit_log("ai-ops", "healing_action_skipped", p->name, detail, 0, 0, "local");

            gchar skip_reason[320];
            g_snprintf(skip_reason, sizeof(skip_reason),
                "breaker-open (재시작 브레이커 차단); trigger=%s", reason);
            gint64 dur_ms = (g_get_monotonic_time() - start_us) / 1000;
            _record_healing_action("restart", target_vm, skip_reason, "skipped", dur_ms);

            G.total_triggered++;
            return;
        }

        PCV_LOG_WARN(HEALING_LOG_DOM,
            "ACTION: Policy '%s' dispatching restart of VM '%s': %s",
            p->name, target_vm, reason);
        _dispatch_vm_restart(p->name, target_vm, reason);

        G.total_triggered++;
        _rate_record();

        p->cooldown_sec = p->base_cooldown_sec;
        p->consecutive_failures = 0;
        return;
    }

    gboolean action_executed = (g_strcmp0(p->action, "alert_only") == 0);

    if (action_executed) {
        PCV_LOG_WARN(HEALING_LOG_DOM,
            "ALERT: Policy '%s' triggered: %s", p->name, reason);
    } else {
        PCV_LOG_WARN(HEALING_LOG_DOM,
            "ACTION-NOT-IMPLEMENTED: Policy '%s' action '%s' is not wired to a real "
            "VM operation — no remediation performed: %s", p->name, p->action, reason);
    }

    gchar detail[384];
    g_snprintf(detail, sizeof(detail), "action=%s reason=%s executed=%s",
               p->action, reason, action_executed ? "true" : "false");
    pcv_audit_log("ai-ops",
                  action_executed ? "healing_action" : "healing_action_skipped",
                  p->name, detail, 0, 0, "local");

    G.total_triggered++;
    if (action_executed) G.total_executed++;
    gchar lbl[128];
    g_snprintf(lbl, sizeof(lbl), "policy=\"%s\",action=\"%s\"", p->name, p->action);
    pcv_prom_gauge_set_labels("purecvisor_healing_actions_total", lbl, (gdouble)G.total_executed);

    extern void pcv_ws_broadcast(const gchar*, const gchar*);
    extern gint pcv_ws_client_count(void);
    if (pcv_ws_client_count() > 0) {
        gchar payload[512];
        g_snprintf(payload, sizeof(payload),
            "{\"policy\":\"%s\",\"action\":\"%s\",\"reason\":\"%s\",\"dry_run\":false,\"executed\":%s}",
            p->name, p->action, reason, action_executed ? "true" : "false");
        pcv_ws_broadcast("healing", payload);
    }

    _rate_record();

    p->cooldown_sec = p->base_cooldown_sec;
    p->consecutive_failures = 0;

    gint64 dur_ms = (g_get_monotonic_time() - start_us) / 1000;
    _record_healing_action(p->action, p->name, reason,
                           action_executed ? "success" : "not_implemented", dur_ms);
}

static void
_queue_approval(HealingPolicy *p, const gchar *reason)
{
    PendingAction *pa = &G.pending[G.pending_pos];

    if (pa->id != 0 && !pa->resolved && G.total_pending > 0) {
        G.total_pending--;
    }
    pa->id = ++G.next_action_id;
    g_strlcpy(pa->policy_name, p->name, sizeof(pa->policy_name));
    g_strlcpy(pa->action, p->action, sizeof(pa->action));
    g_strlcpy(pa->reason, reason, sizeof(pa->reason));
    pa->created_us = g_get_real_time();
    pa->resolved = FALSE;
    G.pending_pos = (G.pending_pos + 1) % MAX_PENDING;
    if (G.pending_count < MAX_PENDING) G.pending_count++;

    G.total_pending++;
    pcv_prom_gauge_set_labels("purecvisor_healing_pending_approvals", "",
        (gdouble)G.total_pending);

    extern void pcv_ws_broadcast(const gchar*, const gchar*);
    extern gint pcv_ws_client_count(void);
    if (pcv_ws_client_count() > 0) {
        gchar payload[512];
        g_snprintf(payload, sizeof(payload),
            "{\"id\":%d,\"policy\":\"%s\",\"action\":\"%s\",\"reason\":\"%s\"}",
            pa->id, p->name, p->action, reason);
        pcv_ws_broadcast("healing-request", payload);
    }

    PCV_LOG_INFO(HEALING_LOG_DOM,
        "Approval required: [%d] policy='%s' action='%s' — %s",
        pa->id, p->name, p->action, reason);
}

static void
_try_policy(HealingPolicy *p, const gchar *metric, gdouble value,
            gdouble zscore, const gchar *reason, const gchar *target_vm)
{
    if (!p->enabled) return;

    gint64 now = g_get_monotonic_time();
    if (now - p->last_trigger_us < p->cooldown_sec * G_USEC_PER_SEC)
        return;

    if (p->consecutive_failures >= CIRCUIT_BREAKER_MAX) {
        PCV_LOG_WARN(HEALING_LOG_DOM,
            "Circuit breaker OPEN for policy '%s' (%d consecutive failures)",
            p->name, p->consecutive_failures);
        return;
    }

    if (g_strcmp0(p->action, "alert_only") != 0 && !_rate_check())
        return;

    p->last_trigger_us = now;

    if (p->require_approval && g_strcmp0(p->action, "alert_only") != 0) {
        _queue_approval(p, reason);
    } else {
        _execute_action(p, reason, target_vm);
    }
}

void
pcv_healing_init(void)
{
    g_mutex_init(&G.mu);
    g_mutex_init(&g_healing_hist_mu);
    g_mutex_init(&g_anomaly_mu);
    G.initialized = TRUE;

    const gchar *mode = pcv_config_get_string("ai", "mode", "dry_run");
    G.dry_run = (g_ascii_strcasecmp(mode, "active") != 0);

    rb_init();
    gint rb_threshold = pcv_config_get_int("ai", "restart_breaker_threshold",
                                           RESTART_BREAKER_THRESHOLD_DEFAULT);
    gint rb_cooldown  = pcv_config_get_int("ai", "restart_breaker_cooldown_sec",
                                           RESTART_BREAKER_COOLDOWN_SEC_DEFAULT);
    rb_configure(rb_threshold, rb_cooldown);

    _add_policy("cpu-overload", "purecvisor_host_cpu_percent",
        3.0, 85.0, "alert_only", 600, FALSE);
    _add_policy("mem-pressure", "purecvisor_host_memory_percent",
        2.5, 90.0, "alert_only", 600, FALSE);
    _add_policy("thermal-alert", "node_hwmon_temp_celsius",
        2.0, 80.0, "alert_only", 1800, FALSE);

    _add_policy("vm-unresponsive", "vm-unresponsive",
        0, 0, "restart", 300, FALSE);
    _add_policy("swap-storm", "node_vmstat_pswpout",
        2.5, 0, "alert_only", 300, FALSE);
    _add_policy("disk-saturated", "node_disk_io_time_seconds_total",
        3.0, 0, "alert_only", 600, FALSE);
    _add_policy("net-errors", "node_network_receive_errs_total",
        2.0, 0, "alert_only", 300, FALSE);
    _add_policy("conntrack-full", "node_nf_conntrack_entries",
        2.5, 0, "alert_only", 600, FALSE);

    _add_policy("vm-reboot-loop", "vm-reboot-loop",
        0, 0, "alert_only", 1200, FALSE);

    _add_policy("vm-migration-failed", "vm-migration-failed",
        0, 0, "alert_only", 1800, FALSE);

    PCV_LOG_INFO(HEALING_LOG_DOM,
        "Self-healing initialized — %d policies, mode=%s",
        G.policy_count, G.dry_run ? "dry_run" : "active");
}

void
pcv_healing_set_mode(gboolean dry_run)
{
    if (!G.initialized) return;
    g_mutex_lock(&G.mu);
    gboolean prev = G.dry_run;
    G.dry_run = dry_run;
    g_mutex_unlock(&G.mu);
    if (prev != dry_run) {
        PCV_LOG_WARN(HEALING_LOG_DOM,
            "Mode changed: %s → %s",
            prev ? "dry_run" : "active",
            dry_run ? "dry_run" : "active");
        pcv_audit_log("ai-ops", "healing_mode_change",
            dry_run ? "dry_run" : "active",
            prev ? "was_dry_run" : "was_active", 0, 0, "local");
    }
}

gboolean
pcv_healing_get_mode(void)
{
    return G.initialized ? G.dry_run : TRUE;
}

void
pcv_healing_shutdown(void)
{
    if (!G.initialized) return;
    G.initialized = FALSE;
    rb_shutdown();
    g_mutex_clear(&G.mu);
    g_mutex_clear(&g_healing_hist_mu);
    g_mutex_clear(&g_anomaly_mu);
}

void
pcv_healing_on_anomaly(const gchar *metric, gdouble value,
                        gdouble zscore, gdouble threshold,
                        const gchar *target_vm)
{
    if (!G.initialized) return;

    g_mutex_lock(&G.mu);

    gint triggered_count = 0;
    gchar triggered_metrics[512] = {0};

    for (gint i = 0; i < G.policy_count; i++) {
        HealingPolicy *p = &G.policies[i];
        if (!p->trigger_metric[0]) continue;
        if (strstr(metric, p->trigger_metric) == NULL) continue;
        if (zscore < p->trigger_zscore) continue;

        triggered_count++;
        gchar frag[128];
        g_snprintf(frag, sizeof(frag), "%s(Z=%.1f) ", metric, zscore);
        g_strlcat(triggered_metrics, frag, sizeof(triggered_metrics));

        gchar reason[256];
        g_snprintf(reason, sizeof(reason),
            "%s Z=%.2f (>%.1f) val=%.2f", metric, zscore, threshold, value);
        _try_policy(p, metric, value, zscore, reason, target_vm);
    }

    g_mutex_unlock(&G.mu);

    gboolean window_trigger = _track_distinct_anomaly(metric);

    if (triggered_count >= 2 || window_trigger) {
        JsonObject *host = pcv_ebpf_telemetry_get_host();
        gchar *host_json = NULL;
        if (host) {
            JsonNode *n = json_node_new(JSON_NODE_OBJECT);
            json_node_set_object(n, host);
            host_json = json_to_string(n, FALSE);
            json_node_free(n);
            json_object_unref(host);
        }

        gchar context[768];
        if (window_trigger) {
            g_snprintf(context, sizeof(context),
                "Multi-anomaly time-window: distinct metrics in 60s exceeded threshold (this metric=%s, Z=%.2f). "
                "Recently triggered: %s",
                metric, zscore, triggered_metrics);
            PCV_LOG_INFO(HEALING_LOG_DOM,
                "Time-window trigger: 3+ distinct anomalies in 60s — dispatching AI Agent");
        } else {
            g_snprintf(context, sizeof(context),
                "Multi-anomaly detected: %d policies triggered simultaneously: %s",
                triggered_count, triggered_metrics);
            PCV_LOG_INFO(HEALING_LOG_DOM,
                "Complex condition: %d simultaneous anomalies — dispatching AI Agent",
                triggered_count);
        }

        pcv_agent_compare_async(host_json, context);
        g_free(host_json);
    }
}

void
pcv_healing_on_prediction(gdouble cpu_pred, gdouble mem_pred,
                           gdouble cpu_trend, gdouble mem_trend)
{
    if (!G.initialized) return;

    g_mutex_lock(&G.mu);

    for (gint i = 0; i < G.policy_count; i++) {
        HealingPolicy *p = &G.policies[i];
        if (p->predict_threshold <= 0) continue;

        if (strstr(p->trigger_metric, "cpu") && cpu_pred > p->predict_threshold) {
            gchar reason[256];
            g_snprintf(reason, sizeof(reason),
                "CPU predicted %.1f%% in 5min (threshold %.0f%%, trend=%.4f)",
                cpu_pred, p->predict_threshold, cpu_trend);
            _try_policy(p, "cpu_predicted", cpu_pred, 0, reason, NULL);
        }
        if (strstr(p->trigger_metric, "memory") && mem_pred > p->predict_threshold) {
            gchar reason[256];
            g_snprintf(reason, sizeof(reason),
                "MEM predicted %.1f%% in 5min (threshold %.0f%%, trend=%.4f)",
                mem_pred, p->predict_threshold, mem_trend);
            _try_policy(p, "mem_predicted", mem_pred, 0, reason, NULL);
        }
    }

    g_mutex_unlock(&G.mu);
}

gchar *
pcv_healing_get_pending_json(void)
{
    g_mutex_lock(&G.mu);

    GString *buf = g_string_new("[");
    for (gint i = 0; i < G.pending_count; i++) {
        gint idx = (G.pending_pos - G.pending_count + i + MAX_PENDING) % MAX_PENDING;
        PendingAction *pa = &G.pending[idx];
        if (pa->resolved) continue;
        if (buf->len > 1) g_string_append_c(buf, ',');
        g_string_append_printf(buf,
            "{\"id\":%d,\"policy\":\"%s\",\"action\":\"%s\","
            "\"reason\":\"%s\",\"ts\":%ld}",
            pa->id, pa->policy_name, pa->action, pa->reason,
            (long)(pa->created_us / G_USEC_PER_SEC));
    }
    g_string_append_c(buf, ']');

    g_mutex_unlock(&G.mu);
    return g_string_free(buf, FALSE);
}

void
pcv_healing_approve(gint action_id)
{
    g_mutex_lock(&G.mu);
    for (gint i = 0; i < MAX_PENDING; i++) {
        if (G.pending[i].id == action_id && !G.pending[i].resolved) {
            G.pending[i].resolved = TRUE;
            G.total_pending--;
            pcv_prom_gauge_set_labels("purecvisor_healing_pending_approvals", "",
                (gdouble)G.total_pending);

            for (gint j = 0; j < G.policy_count; j++) {
                if (g_strcmp0(G.policies[j].name, G.pending[i].policy_name) == 0) {
                    _execute_action(&G.policies[j], G.pending[i].reason, NULL);
                    break;
                }
            }

            PCV_LOG_INFO(HEALING_LOG_DOM, "Action [%d] APPROVED: %s",
                action_id, G.pending[i].reason);
            break;
        }
    }
    g_mutex_unlock(&G.mu);
}

void
pcv_healing_dismiss(gint action_id)
{
    g_mutex_lock(&G.mu);
    for (gint i = 0; i < MAX_PENDING; i++) {
        if (G.pending[i].id == action_id && !G.pending[i].resolved) {
            G.pending[i].resolved = TRUE;
            G.total_pending--;
            pcv_prom_gauge_set_labels("purecvisor_healing_pending_approvals", "",
                (gdouble)G.total_pending);

            PCV_LOG_INFO(HEALING_LOG_DOM, "Action [%d] DISMISSED: %s",
                action_id, G.pending[i].reason);
            break;
        }
    }
    g_mutex_unlock(&G.mu);
}

gchar *
pcv_healing_get_history_json(void)
{
    g_mutex_lock(&g_healing_hist_mu);

    GString *buf = g_string_new("[");
    for (gint i = 0; i < g_healing_hist_count; i++) {

        gint idx = (g_healing_hist_idx - 1 - i + HEALING_HISTORY_MAX) % HEALING_HISTORY_MAX;
        HealingHistoryEntry *e = &g_healing_history[idx];
        if (buf->len > 1) g_string_append_c(buf, ',');
        g_string_append_printf(buf,
            "{\"action\":\"%s\",\"target\":\"%s\",\"reason\":\"%s\","
            "\"result\":\"%s\",\"timestamp\":%ld,\"duration_ms\":%ld}",
            e->action, e->target, e->reason, e->result,
            (long)e->timestamp, (long)e->duration_ms);
    }
    g_string_append_c(buf, ']');

    g_mutex_unlock(&g_healing_hist_mu);
    return g_string_free(buf, FALSE);
}
