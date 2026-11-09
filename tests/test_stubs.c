


































#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include "api/uds_server.h"
#include "modules/audit/pcv_audit_chain.h"
#include "modules/daemons/telemetry.h"
#include "bootstrap/pcv_single_edge_runtime.h"
#include "modules/dispatcher/handler_vm_start.h"
#include "modules/dispatcher/rpc_offload.h"





void pcv_cluster_sync_vm_xml(const gchar *name __attribute__((unused))) { }
void pcv_cluster_remove_vm_xml(const gchar *name __attribute__((unused))) { }
void pcv_cluster_notify_config_reload(void) { }
gboolean pcv_cluster_check_zvol_fence(void) { return TRUE; }

















gboolean
pcv_vpc_bridge_is_managed(const gchar *bridge_name __attribute__((unused)))
{
    return FALSE;
}



static GMutex g_test_prom_mu;
static GHashTable *g_test_prom_counts;
void pcv_prom_inc(const gchar *name, const gchar *label_key, const gchar *label_val)
{
    gchar *key = g_strdup_printf("%s{%s=%s}", name, label_key ? label_key : "", label_val ? label_val : "");
    g_mutex_lock(&g_test_prom_mu);
    if (!g_test_prom_counts)
        g_test_prom_counts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    guint n = GPOINTER_TO_UINT(g_hash_table_lookup(g_test_prom_counts, key));
    g_hash_table_replace(g_test_prom_counts, key, GUINT_TO_POINTER(n + 1));
    g_mutex_unlock(&g_test_prom_mu);
}
guint pcv_test_prom_inc_count(const gchar *key);
guint pcv_test_prom_inc_count(const gchar *key)
{
    g_mutex_lock(&g_test_prom_mu);
    guint n = g_test_prom_counts ? GPOINTER_TO_UINT(g_hash_table_lookup(g_test_prom_counts, key)) : 0;
    g_mutex_unlock(&g_test_prom_mu);
    return n;
}




static GMutex g_test_score_mu;
static GHashTable *g_test_score_labels;
void pcv_prom_gauge_set_labels(const gchar *n, const gchar *l, double v __attribute__((unused)))
{


    if (g_strcmp0(n, "purecvisor_anomaly_score") != 0) return;
    g_mutex_lock(&g_test_score_mu);
    if (!g_test_score_labels)
        g_test_score_labels = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_hash_table_add(g_test_score_labels, g_strdup(l ? l : ""));
    g_mutex_unlock(&g_test_score_mu);
}
void pcv_test_anomaly_scores_reset(void);
void pcv_test_anomaly_scores_reset(void)
{
    g_mutex_lock(&g_test_score_mu);
    if (g_test_score_labels) g_hash_table_remove_all(g_test_score_labels);
    g_mutex_unlock(&g_test_score_mu);
}
gboolean pcv_test_anomaly_score_seen(const gchar *labels);
gboolean pcv_test_anomaly_score_seen(const gchar *labels)
{
    g_mutex_lock(&g_test_score_mu);
    gboolean seen = g_test_score_labels && g_hash_table_contains(g_test_score_labels, labels);
    g_mutex_unlock(&g_test_score_mu);
    return seen;
}




VmMetrics *get_vm_metrics(const gchar *vm_id __attribute__((unused))) { return NULL; }









gchar *pcv_prom_render(void) { return NULL; }





static gint g_test_audit_call_count = 0;
static gchar g_test_audit_last_method[128] = {0};
static gchar g_test_audit_last_target[1024] = {0};
static gchar g_test_audit_last_source[64] = {0};
static gchar g_test_audit_last_result[64] = {0};
static gint g_test_audit_last_error_code = 0;


#define PCV_TEST_AUDIT_ROWS 16
static gchar g_test_audit_usernames[PCV_TEST_AUDIT_ROWS][64];


static gchar g_test_audit_methods[PCV_TEST_AUDIT_ROWS][128];
static gchar g_test_audit_targets[PCV_TEST_AUDIT_ROWS][256];

static gchar g_test_audit_results[PCV_TEST_AUDIT_ROWS][64];
static gchar g_test_audit_sources[PCV_TEST_AUDIT_ROWS][64];
static gchar g_test_uds_last_response[4096] = {0};
static gint g_test_uds_response_count = 0;
static gint g_test_alert_call_count = 0;
static gchar g_test_alert_last_event_id[128] = {0};
static GMutex g_test_telemetry_mu;
static gboolean g_test_telemetry_enabled = FALSE;
static gdouble g_test_telemetry_cpu_percent = 0.0;
static gdouble g_test_telemetry_mem_percent = 0.0;
static gint g_test_telemetry_call_count = 0;

void pcv_test_audit_reset(void)
{
    g_test_audit_call_count = 0;
    g_test_audit_last_method[0] = '\0';
    g_test_audit_last_target[0] = '\0';
    g_test_audit_last_source[0] = '\0';
    g_test_audit_last_result[0] = '\0';
    g_test_audit_last_error_code = 0;
    for (gint i = 0; i < PCV_TEST_AUDIT_ROWS; i++) {
        g_test_audit_usernames[i][0] = '\0';
        g_test_audit_methods[i][0] = '\0';
        g_test_audit_targets[i][0] = '\0';
        g_test_audit_results[i][0] = '\0';
        g_test_audit_sources[i][0] = '\0';
    }
}



const gchar *pcv_test_audit_username_at(gint row)
{
    if (row < 0 || row >= PCV_TEST_AUDIT_ROWS || row >= g_test_audit_call_count)
        return NULL;
    return g_test_audit_usernames[row];
}



const gchar *pcv_test_audit_method_at(gint row)
{
    if (row < 0 || row >= PCV_TEST_AUDIT_ROWS || row >= g_test_audit_call_count)
        return NULL;
    return g_test_audit_methods[row];
}

const gchar *pcv_test_audit_target_at(gint row)
{
    if (row < 0 || row >= PCV_TEST_AUDIT_ROWS || row >= g_test_audit_call_count)
        return NULL;
    return g_test_audit_targets[row];
}

const gchar *pcv_test_audit_result_at(gint row)
{
    if (row < 0 || row >= PCV_TEST_AUDIT_ROWS || row >= g_test_audit_call_count)
        return NULL;
    return g_test_audit_results[row];
}

const gchar *pcv_test_audit_source_at(gint row)
{
    if (row < 0 || row >= PCV_TEST_AUDIT_ROWS || row >= g_test_audit_call_count)
        return NULL;
    return g_test_audit_sources[row];
}

gint pcv_test_audit_call_count(void)
{
    return g_test_audit_call_count;
}

const gchar *pcv_test_audit_last_method(void)
{
    return g_test_audit_last_method;
}

const gchar *pcv_test_audit_last_target(void)
{
    return g_test_audit_last_target;
}


const gchar *pcv_test_audit_last_source(void)
{
    return g_test_audit_last_source;
}

const gchar *pcv_test_audit_last_result(void)
{
    return g_test_audit_last_result;
}

gint pcv_test_audit_last_error_code(void)
{
    return g_test_audit_last_error_code;
}

void pcv_test_uds_response_reset(void)
{
    g_test_uds_last_response[0] = '\0';
}

const gchar *pcv_test_last_uds_response(void)
{
    return g_test_uds_last_response;
}


guint pcv_test_uds_response_count(void)
{
    return (guint)g_atomic_int_get(&g_test_uds_response_count);
}


void pcv_test_uds_responses_reset(void)
{
    g_atomic_int_set(&g_test_uds_response_count, 0);
    g_test_uds_last_response[0] = '\0';
}

void pcv_test_alert_reset(void)
{
    g_test_alert_call_count = 0;
    g_test_alert_last_event_id[0] = '\0';
}

gint pcv_test_alert_call_count(void)
{
    return g_test_alert_call_count;
}

const gchar *pcv_test_alert_last_event_id(void)
{
    return g_test_alert_last_event_id;
}




void pcv_test_alert_record_security_event_hook(
    const gchar *event_id,
    const gchar *severity __attribute__((unused)),
    const gchar *summary __attribute__((unused)))
{
    g_test_alert_call_count++;
    g_strlcpy(g_test_alert_last_event_id, event_id ? event_id : "",
              sizeof g_test_alert_last_event_id);
}

void pcv_audit_log(const gchar *username,
                   const gchar *method,
                   const gchar *target,
                   const gchar *result,
                   gint error_code,
                   gint64 duration_ms __attribute__((unused)),
                   const gchar *src_ip)
{
    if (g_test_audit_call_count >= 0 && g_test_audit_call_count < PCV_TEST_AUDIT_ROWS) {
        g_strlcpy(g_test_audit_usernames[g_test_audit_call_count], username ? username : "",
                  sizeof g_test_audit_usernames[0]);
        g_strlcpy(g_test_audit_methods[g_test_audit_call_count], method ? method : "",
                  sizeof g_test_audit_methods[0]);
        g_strlcpy(g_test_audit_targets[g_test_audit_call_count], target ? target : "",
                  sizeof g_test_audit_targets[0]);
        g_strlcpy(g_test_audit_results[g_test_audit_call_count], result ? result : "",
                  sizeof g_test_audit_results[0]);
        g_strlcpy(g_test_audit_sources[g_test_audit_call_count], src_ip ? src_ip : "",
                  sizeof g_test_audit_sources[0]);
    }
    g_test_audit_call_count++;
    g_strlcpy(g_test_audit_last_method, method ? method : "",
              sizeof g_test_audit_last_method);
    g_strlcpy(g_test_audit_last_target, target ? target : "",
              sizeof g_test_audit_last_target);
    g_strlcpy(g_test_audit_last_result, result ? result : "",
              sizeof g_test_audit_last_result);
    g_strlcpy(g_test_audit_last_source, src_ip ? src_ip : "", sizeof g_test_audit_last_source);
    g_test_audit_last_error_code = error_code;
}

PcvAuditChainHealth pcv_audit_get_chain_health(void)
{
    PcvAuditChainHealth health = {.current_ok = TRUE};
    g_strlcpy(health.reason, "test_stub", sizeof(health.reason));
    return health;
}

void pcv_ws_broadcast_job_complete(const gchar *job_id __attribute__((unused)),
                                   const gchar *method __attribute__((unused)),
                                   const gchar *status __attribute__((unused)),
                                   const gchar *error_msg __attribute__((unused))) { }





void pcv_ws_broadcast_job_complete_mt(const gchar *job_id __attribute__((unused)),
                                      const gchar *method __attribute__((unused)),
                                      const gchar *status __attribute__((unused)),
                                      const gchar *error_msg __attribute__((unused))) { }




void pcv_agent_compare_async(const gchar *metrics_json __attribute__((unused)),
                             const gchar *anomaly_context __attribute__((unused))) { }



void
pcv_test_alert_telemetry_set(gboolean enabled, gdouble cpu_percent,
                             gdouble mem_percent)
{
    g_mutex_lock(&g_test_telemetry_mu);
    g_test_telemetry_enabled = enabled;
    g_test_telemetry_cpu_percent = cpu_percent;
    g_test_telemetry_mem_percent = mem_percent;
    g_test_telemetry_call_count = 0;
    g_mutex_unlock(&g_test_telemetry_mu);
}

gint
pcv_test_alert_telemetry_call_count(void)
{
    g_mutex_lock(&g_test_telemetry_mu);
    gint count = g_test_telemetry_call_count;
    g_mutex_unlock(&g_test_telemetry_mu);
    return count;
}

JsonObject *
pcv_ebpf_telemetry_get_host(void)
{
    g_mutex_lock(&g_test_telemetry_mu);
    if (!g_test_telemetry_enabled) {
        g_mutex_unlock(&g_test_telemetry_mu);
        return NULL;
    }
    g_test_telemetry_call_count++;
    gdouble cpu = g_test_telemetry_cpu_percent;
    gdouble mem = g_test_telemetry_mem_percent;
    g_mutex_unlock(&g_test_telemetry_mu);

    JsonObject *host = json_object_new();
    json_object_set_double_member(host, "cpu_percent", cpu);
    json_object_set_double_member(host, "mem_percent", mem);
    return host;
}
void pcv_ws_broadcast(const gchar *type __attribute__((unused)),
                      const gchar *payload_json __attribute__((unused))) { }
gint pcv_ws_client_count(void) { return 0; }


static gchar g_test_ws_closed_subject[128];
static gint  g_test_ws_close_calls;
void pcv_ws_close_subject(const gchar *username)
{
    g_strlcpy(g_test_ws_closed_subject, username ? username : "", sizeof g_test_ws_closed_subject);
    g_test_ws_close_calls++;
}

const gchar *pcv_test_ws_closed_subject(gint *calls)
{
    if (calls) *calls = g_test_ws_close_calls;
    return g_test_ws_closed_subject;
}

typedef struct _virDomain  *virDomainPtr;
typedef struct _virConnect *virConnectPtr;
virDomainPtr pure_virt_get_domain(virConnectPtr conn __attribute__((unused)),
                                  const gchar *identifier __attribute__((unused))) { return NULL; }





gboolean pcv_etcd_acquire_inflight_lock(PcvEtcdClient *c __attribute__((unused)),
    const gchar *p __attribute__((unused)),
    const gchar *n __attribute__((unused)),
    const gchar *o __attribute__((unused)),
    gint t __attribute__((unused)),
    GError **e __attribute__((unused))) { return TRUE; }







void pure_uds_server_send_response(UdsServer *self __attribute__((unused)),
                                   GSocketConnection *connection __attribute__((unused)),
                                   const gchar *response)
{
    if (pcv_rpc_offload_capture(response))
        return;
    g_strlcpy(g_test_uds_last_response, response ? response : "", sizeof g_test_uds_last_response);
    g_atomic_int_inc(&g_test_uds_response_count);
}








static PcvVmStartSubmit g_vm_start_result = PCV_VM_START_ALREADY_ACTIVE;
static guint g_vm_start_calls;
static gboolean g_vm_start_last_skip;
static gchar *g_vm_start_last_origin;
static gchar *g_vm_start_last_actor;
static GThread *g_vm_start_last_thread;

void pcv_test_vm_start_set_result(PcvVmStartSubmit r) { g_vm_start_result = r; }
guint pcv_test_vm_start_call_count(void) { return g_vm_start_calls; }
gboolean pcv_test_vm_start_last_skip_if_active(void) { return g_vm_start_last_skip; }
const gchar *pcv_test_vm_start_last_origin(void) { return g_vm_start_last_origin; }
const gchar *pcv_test_vm_start_last_actor(void) { return g_vm_start_last_actor; }
gboolean pcv_test_vm_start_last_on_thread(GThread *thread) { return g_vm_start_last_thread == thread; }


void pcv_vm_start_report_rejected(const gchar *vm_id __attribute__((unused)), const gchar *origin __attribute__((unused)),
                                  const gchar *reason __attribute__((unused)), const gchar *actor __attribute__((unused))) {}
JsonObject *pcv_snapshot_schedule_status(void) { return NULL; }

PcvVmStartSubmit pcv_vm_start_submit(const PcvVmStartRequest *req,
                                     PcvVmStartDoneFn done __attribute__((unused)),
                                     gpointer user_data __attribute__((unused)), gchar **err_msg)
{
    g_vm_start_calls++;
    g_vm_start_last_skip = req ? req->skip_if_active : FALSE;
    g_free(g_vm_start_last_origin);
    g_vm_start_last_origin = g_strdup(req && req->origin ? req->origin : "");
    g_free(g_vm_start_last_actor);
    g_vm_start_last_actor = g_strdup(req && req->actor ? req->actor : "");
    g_vm_start_last_thread = g_thread_self();
    if (g_vm_start_result == PCV_VM_START_REJECTED && err_msg) *err_msg = g_strdup("stub: rejected");
    return g_vm_start_result;
}


gboolean pcv_rest_hsts_enabled = FALSE;
