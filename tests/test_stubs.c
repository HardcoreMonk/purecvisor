





























#include <glib.h>




void pcv_cluster_sync_vm_xml(const gchar *name __attribute__((unused)),
                              const gchar *xml __attribute__((unused))) { }
void pcv_cluster_remove_vm_xml(const gchar *name __attribute__((unused))) { }
void pcv_cluster_notify_config_reload(void) { }
gboolean pcv_cluster_check_zvol_fence(void) { return TRUE; }




void pcv_prom_gauge_set_labels(const gchar *n __attribute__((unused)),
    const gchar *l __attribute__((unused)),
    double v __attribute__((unused))) { }





static gint g_test_audit_call_count = 0;
static gchar g_test_audit_last_method[128] = {0};
static gchar g_test_audit_last_target[256] = {0};
static gint g_test_alert_call_count = 0;
static gchar g_test_alert_last_event_id[128] = {0};

void pcv_test_audit_reset(void)
{
    g_test_audit_call_count = 0;
    g_test_audit_last_method[0] = '\0';
    g_test_audit_last_target[0] = '\0';
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

void pcv_alert_record_security_event(const gchar *event_id,
                                     const gchar *severity __attribute__((unused)),
                                     const gchar *summary __attribute__((unused)))
{
    g_test_alert_call_count++;
    g_strlcpy(g_test_alert_last_event_id, event_id ? event_id : "",
              sizeof g_test_alert_last_event_id);
}

void pcv_audit_log(const gchar *username __attribute__((unused)),
                   const gchar *method,
                   const gchar *target,
                   const gchar *result __attribute__((unused)),
                   gint error_code __attribute__((unused)),
                   gint64 duration_ms __attribute__((unused)),
                   const gchar *src_ip __attribute__((unused)))
{
    g_test_audit_call_count++;
    g_strlcpy(g_test_audit_last_method, method ? method : "",
              sizeof g_test_audit_last_method);
    g_strlcpy(g_test_audit_last_target, target ? target : "",
              sizeof g_test_audit_last_target);
}

void pcv_ws_broadcast_job_complete(const gchar *job_id __attribute__((unused)),
                                   const gchar *method __attribute__((unused)),
                                   const gchar *status __attribute__((unused)),
                                   const gchar *error_msg __attribute__((unused))) { }

gboolean pcv_rbac_apikey_revoke(const gchar *client_name __attribute__((unused)),
                                GError **error __attribute__((unused)))
{
    return TRUE;
}




typedef struct PcvEtcdClient PcvEtcdClient;
PcvEtcdClient *pcv_cluster_get_etcd(void) { return NULL; }
gboolean pcv_etcd_acquire_inflight_lock(PcvEtcdClient *c __attribute__((unused)),
    const gchar *p __attribute__((unused)),
    const gchar *n __attribute__((unused)),
    const gchar *o __attribute__((unused)),
    gint t __attribute__((unused)),
    GError **e __attribute__((unused))) { return TRUE; }
gboolean pcv_etcd_release_inflight_lock(PcvEtcdClient *c __attribute__((unused)),
    const gchar *p __attribute__((unused)),
    GError **e __attribute__((unused))) { return TRUE; }
gint pcv_etcd_compute_inflight_ttl(const gchar *op __attribute__((unused)),
    gint size_gb __attribute__((unused))) { return 60; }
