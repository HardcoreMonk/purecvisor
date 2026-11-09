   
                     
                                                                     
  
                           
                                                                                                     
                                                                                             
                                                                                                         
                                 
  
           
                                                                                                     
                                                                                                    
                                       
                                                                                      
                                        
                                                                                                                 
                                                            
  
                                                                                              
                                                                                                                                      
  
                                                                                         
                                          
   
#include "api/rest_login.h"

#include "api/drain.h"
#include "api/rest_auth.h"
#include "api/rest_response.h"
#include "modules/audit/pcv_audit.h"
#include "modules/auth/pcv_rbac.h"
#include "utils/pcv_config.h"
#include "utils/pcv_jwt.h"
#include "utils/pcv_log.h"
#include "utils/pcv_secure.h"

#include <gio/gio.h>

#define REST_LOGIN_LOG_DOM "pcv_rest"

                                                                    
typedef struct {
    SoupServerMessage  *msg;
    GMainContext       *rest_ctx;
    PcvRestLoginRequest req;
    PcvRestLoginResult  res;
} LoginJob;

static GMutex       g_pool_mu;                         
static GThreadPool *g_pool = NULL;
static gint         g_inflight = 0;                                    
static gint64       g_busy_warned_at = 0;                                   

                                           
static void     (*g_run_fn)(const PcvRestLoginRequest *, PcvRestLoginResult *) = pcv_rest_login_run;
static gboolean (*g_terminating_fn)(void) = pcv_drain_is_terminating;

                                           
void
pcv_rest_login_result_clear(PcvRestLoginResult *out)
{
    g_clear_pointer(&out->json, g_free);
    g_clear_pointer(&out->err_msg, g_free);
    out->err_code = NULL;
    out->retry_after = 0;
    out->status = 0;
}

                                               
static void
_set_error(PcvRestLoginResult *out, guint status, const gchar *code, const gchar *msg, gint retry_after)
{
    out->status = status;
    out->err_code = code;
    out->err_msg = g_strdup(msg);
    out->retry_after = retry_after;
}

                                                            
                                                                   
void
pcv_rest_login_run(const PcvRestLoginRequest *in, PcvRestLoginResult *out)
{
    const gchar *username = in->username;
    const gchar *password = in->password;
    const gchar *client_ip = in->client_ip;
    const gchar *cfg_user = in->cfg_user;
    const gchar *cfg_pass = in->cfg_pass;
    gboolean bootstrap_configured = cfg_user && *cfg_user && cfg_pass && *cfg_pass;

                                                                                            
    gint ip_lockout = pcv_rbac_get_ip_remaining_lockout(client_ip);
    if (ip_lockout > 0) {
        _set_error(out, 429, "TOO_MANY_REQUESTS", "Too many failed login attempts from this IP", ip_lockout);
        return;
    }

    gchar *token = nullptr;
    gchar *refresh = nullptr;
    GError *err = nullptr;

                                                     
                                                          
                                                                                  
    PcvRole role = PCV_ROLE_VIEWER;
    g_autofree gchar *identity_nonce = NULL;
    if (pcv_rbac_password_check_subject(username, password, &role, &identity_nonce, &err)) {
        PcvTotpStatus totp_st = { 0 };
        gboolean have_st = pcv_rbac_totp_status(username, &totp_st);
        gboolean confirmed = have_st && totp_st.confirmed;
                                                                                                         
                                                                     
        gboolean present = identity_nonce != NULL;
        gboolean enroll_forced = present && !confirmed && pcv_rbac_totp_role_required((gint)role);

        if (present && (confirmed || enroll_forced)) {
            GError *pend_err = nullptr;
            gchar *pending = pcv_jwt_sign_scoped(username, 180, client_ip,
                                                  "totp", identity_nonce, &pend_err);
            if (!pending) {
                _set_error(out, 500, "INTERNAL",
                           pend_err ? pend_err->message : "Failed to issue pending token", 0);
                if (pend_err) g_error_free(pend_err);
                return;
            }
            pcv_rbac_ip_record_auth_success(client_ip);                 

            out->status = 200;
            out->json = confirmed
                ? g_strdup_printf("{\"totp_required\":true,\"pending_token\":\"%s\"}", pending)
                : g_strdup_printf("{\"totp_enroll_required\":true,\"pending_token\":\"%s\"}", pending);
            g_free(pending);
            PCV_LOG_INFO(REST_LOGIN_LOG_DOM,
                "TOTP pending token issued for '%s' (%s)",
                username, confirmed ? "totp_required" : "totp_enroll_required");
            return;
        }

        if (present)
            token = pcv_rbac_issue_tokens(username, identity_nonce, &refresh, &err);
    } else if (bootstrap_configured &&
               pcv_rest_auth_should_fallback_bootstrap(username, password,
                   cfg_user, cfg_pass, pcv_rbac_user_exists(username) != PCV_USER_ABSENT)) {
                                                                               
                                                                                   
                                                                     
        PCV_LOG_WARN(REST_LOGIN_LOG_DOM,
            "bootstrap fallback refused for '%s': admin is missing from the RBAC DB — create an account "
            "over the local UDS (pcvctl auth create) or restart the daemon to re-seed the bootstrap admin",
            username);
        pcv_audit_log(username, "auth.bootstrap.fallback",
                      "bootstrap recovery refused", "fail", 401, 0, client_ip);
    }

    if (!token) {
        g_free(refresh);
        pcv_rbac_ip_record_auth_failure(client_ip);             
        pcv_audit_log(username, "auth.failed", "login attempt",
                      "fail", 401, 0, client_ip);

                     
                                             
                                                              
                                                   
                                                      
        gint lockout_sec = pcv_rbac_get_remaining_lockout(username);
        if (lockout_sec > 0) {
            _set_error(out, 429, "TOO_MANY_REQUESTS",
                       err ? err->message : "Account locked due to too many failed attempts", lockout_sec);
        } else {
                                                                   
                                                                                   
            if (err) {
                PCV_LOG_WARN(REST_LOGIN_LOG_DOM, "auth.failed: user=%s reason=%s",
                             username, err->message);
            }
            _set_error(out, 401, "UNAUTHORIZED", "Invalid credentials", 0);
        }
        if (err) g_error_free(err);
        PCV_LOG_WARN(REST_LOGIN_LOG_DOM,
                     "Auth failed for user '%s'", username);
        return;
    }
    if (err) g_error_free(err);

    pcv_rbac_ip_record_auth_success(client_ip);             

    out->status = 200;
    if (refresh) {
        out->json = g_strdup_printf(
            "{\"access_token\":\"%s\","
            "\"refresh_token\":\"%s\","
            "\"token_type\":\"Bearer\","
            "\"expires_in\":900,"
            "\"refresh_expires_in\":604800}", token, refresh);
    } else {
        out->json = g_strdup_printf(
            "{\"access_token\":\"%s\","
            "\"token_type\":\"Bearer\","
            "\"expires_in\":900}", token);
    }
    g_free(token);
    g_free(refresh);
    PCV_LOG_INFO(REST_LOGIN_LOG_DOM, "Token issued for '%s'", username);
}

                                                  
static void
_request_clear(PcvRestLoginRequest *req)
{
    g_clear_pointer(&req->username, g_free);
    g_clear_pointer(&req->client_ip, g_free);
    g_clear_pointer(&req->cfg_user, g_free);
    pcv_secure_free_str(&req->password);
    pcv_secure_free_str(&req->cfg_pass);
}

                                                                                 
static void
_deliver(SoupServerMessage *msg, const PcvRestLoginResult *r)
{
    if (r->retry_after > 0) {
        gchar retry_buf[32];
        g_snprintf(retry_buf, sizeof(retry_buf), "%d", r->retry_after);
        soup_message_headers_replace(soup_server_message_get_response_headers(msg), "Retry-After", retry_buf);
    }
    if (r->json)
        pcv_rest_send_json(msg, r->status, r->json);
    else
        pcv_rest_send_error(msg, r->status, r->err_code, r->err_msg);
}

                                                                                             
                                         
static gboolean
_login_done(gpointer data)
{
    LoginJob *job = data;
    _deliver(job->msg, &job->res);
    soup_server_message_unpause(job->msg);
    g_object_unref(job->msg);
    pcv_rest_login_result_clear(&job->res);
    g_free(job);
    return G_SOURCE_REMOVE;
}

                                                                           
                                        
static void
_login_worker(gpointer data, gpointer user_data)
{
    (void)user_data;
    LoginJob *job = data;
    if (g_terminating_fn())
        _set_error(&job->res, 503, "SERVICE_UNAVAILABLE", "Server is shutting down", 0);
    else
        g_run_fn(&job->req, &job->res);
    _request_clear(&job->req);
    g_atomic_int_add(&g_inflight, -1);
    pcv_drain_idle(job->rest_ctx, _login_done, job, NULL);
    pcv_drain_work_release();                             
}

                                                                           
static GThreadPool *
_pool_get(void)
{
    g_mutex_lock(&g_pool_mu);
    if (!g_pool) {
        GError *err = NULL;
        g_pool = g_thread_pool_new(_login_worker, NULL, PCV_REST_LOGIN_WORKERS, TRUE, &err);
        if (!g_pool) {
            PCV_LOG_ERROR(REST_LOGIN_LOG_DOM, "login worker pool creation failed: %s", err ? err->message : "?");
            g_clear_error(&err);
        }
    }
    GThreadPool *pool = g_pool;
    g_mutex_unlock(&g_pool_mu);
    return pool;
}

                                                                     
static void
_send_busy(SoupServerMessage *msg)
{
    PcvRestLoginResult busy = { 0 };
    _set_error(&busy, 429, "TOO_MANY_REQUESTS", "Login service is busy", PCV_REST_LOGIN_BUSY_RETRY_SEC);
    _deliver(msg, &busy);
    pcv_rest_login_result_clear(&busy);
    gint64 now = g_get_monotonic_time();
    if (g_busy_warned_at == 0 || now - g_busy_warned_at >= 60 * G_USEC_PER_SEC) {
        g_busy_warned_at = now;
        PCV_LOG_WARN(REST_LOGIN_LOG_DOM, "login worker pool is full (%d in flight) — answering 429 until a slot frees",
                     PCV_REST_LOGIN_INFLIGHT_MAX);
    }
}

                                                                             
void
pcv_rest_login_start(SoupServerMessage *msg, GMainContext *ctx,
                     const gchar *username, const gchar *password, const gchar *client_ip)
{
    GThreadPool *pool = _pool_get();
    if (!pool) {
        _send_busy(msg);
        return;
    }
    if (g_atomic_int_add(&g_inflight, 1) >= PCV_REST_LOGIN_INFLIGHT_MAX) {
        g_atomic_int_add(&g_inflight, -1);
        _send_busy(msg);
        return;
    }
    LoginJob *job = g_new0(LoginJob, 1);
    job->msg = g_object_ref(msg);
    job->rest_ctx = ctx;
    job->req.username = g_strdup(username);
    job->req.password = g_strdup(password);
    job->req.client_ip = g_strdup(client_ip);
    job->req.cfg_user = g_strdup(pcv_config_get_admin_user());
    job->req.cfg_pass = g_strdup(pcv_config_get_admin_password());
    soup_server_message_pause(msg);
    pcv_drain_work_acquire();
    g_thread_pool_push(pool, job, NULL);
}

                                            
void
pcv_rest_login_pool_shutdown(void)
{
    g_mutex_lock(&g_pool_mu);
    GThreadPool *pool = g_steal_pointer(&g_pool);
    g_mutex_unlock(&g_pool_mu);
    if (pool)
        g_thread_pool_free(pool, FALSE, TRUE);
}

                                                  

void pcv_rest_login_set_hooks_for_test(void (*run_fn)(const PcvRestLoginRequest *, PcvRestLoginResult *),
                                       gboolean (*terminating_fn)(void));
gint pcv_rest_login_inflight_for_test(void);

                                       
void
pcv_rest_login_set_hooks_for_test(void (*run_fn)(const PcvRestLoginRequest *, PcvRestLoginResult *),
                                  gboolean (*terminating_fn)(void))
{
    g_run_fn = run_fn ? run_fn : pcv_rest_login_run;
    g_terminating_fn = terminating_fn ? terminating_fn : pcv_drain_is_terminating;
}

              
gint
pcv_rest_login_inflight_for_test(void)
{
    return g_atomic_int_get(&g_inflight);
}
