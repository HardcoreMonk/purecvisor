
#include "pcv_tls.h"
#include "pcv_log.h"
#include "pcv_config.h"
#include <json-glib/json-glib.h>
#include <gio/gio.h>
#include <string.h>

#define TLS_LOG_DOM "pcv_tls"

struct _PcvTlsCtx {
    gchar *cert_path;
    gchar *key_path;
    gchar *ca_path;
    gboolean enabled;
};

static struct {
    PcvTlsCtx *ctx;
    gboolean   initialized;
} G = {0};

PcvTlsCtx *pcv_tls_ctx_new(const gchar *cert, const gchar *key,
                             const gchar *ca, GError **error)
{

    if (!cert || !key || !ca) {
        g_set_error(error, g_quark_from_static_string("tls"), 1,
                    "cert, key, ca paths required");
        return NULL;
    }

    if (!g_file_test(cert, G_FILE_TEST_EXISTS) ||
        !g_file_test(key, G_FILE_TEST_EXISTS) ||
        !g_file_test(ca, G_FILE_TEST_EXISTS)) {
        g_set_error(error, g_quark_from_static_string("tls"), 2,
                    "Certificate files not found");
        return NULL;
    }

    PcvTlsCtx *ctx = g_new0(PcvTlsCtx, 1);
    ctx->cert_path = g_strdup(cert);
    ctx->key_path = g_strdup(key);
    ctx->ca_path = g_strdup(ca);
    ctx->enabled = TRUE;

    GError *cert_err = NULL;
    GTlsCertificate *tls_cert = g_tls_certificate_new_from_file(cert, &cert_err);
    if (tls_cert) {
        GDateTime *not_after = g_tls_certificate_get_not_valid_after(tls_cert);
        if (not_after) {
            GDateTime *now = g_date_time_new_now_utc();
            if (g_date_time_compare(now, not_after) > 0) {
                PCV_LOG_WARN(TLS_LOG_DOM, "TLS certificate has EXPIRED — connections may fail");
            }
            g_date_time_unref(now);
            g_date_time_unref(not_after);
        }

        GDateTime *not_before = g_tls_certificate_get_not_valid_before(tls_cert);
        if (not_before && not_after == NULL)
            not_after = g_tls_certificate_get_not_valid_after(tls_cert);
        gchar *nb_str = not_before ? g_date_time_format_iso8601(not_before) : g_strdup("unknown");
        GDateTime *na_recheck = g_tls_certificate_get_not_valid_after(tls_cert);
        gchar *na_str = na_recheck ? g_date_time_format_iso8601(na_recheck) : g_strdup("unknown");
        PCV_LOG_INFO(TLS_LOG_DOM, "TLS certificate loaded: %s (valid: %s ~ %s)", cert, nb_str, na_str);
        g_free(nb_str);
        g_free(na_str);
        if (not_before) g_date_time_unref(not_before);
        if (na_recheck) g_date_time_unref(na_recheck);

        g_object_unref(tls_cert);
    } else {
        PCV_LOG_WARN(TLS_LOG_DOM, "Could not parse certificate for validation: %s",
                     cert_err ? cert_err->message : "unknown");
        if (cert_err) g_error_free(cert_err);
    }

    PCV_LOG_INFO(TLS_LOG_DOM, "TLS context created (cert=%s)", cert);
    return ctx;
}

void pcv_tls_ctx_free(PcvTlsCtx *ctx)
{
    if (!ctx) return;
    g_free(ctx->cert_path);
    g_free(ctx->key_path);
    g_free(ctx->ca_path);
    g_free(ctx);
}

gboolean pcv_tls_is_enabled(void)
{
    return G.initialized && G.ctx && G.ctx->enabled;
}

const gchar *pcv_tls_get_cert_path(void)
{
    return (G.ctx) ? G.ctx->cert_path : NULL;
}

const gchar *pcv_tls_get_key_path(void)
{
    return (G.ctx) ? G.ctx->key_path : NULL;
}

const gchar *pcv_tls_get_ca_path(void)
{
    return (G.ctx) ? G.ctx->ca_path : NULL;
}

JsonObject *pcv_tls_status(void)
{
    JsonObject *obj = json_object_new();
    json_object_set_boolean_member(obj, "enabled", pcv_tls_is_enabled());
    if (G.ctx) {
        json_object_set_string_member(obj, "cert", G.ctx->cert_path);
        json_object_set_string_member(obj, "ca", G.ctx->ca_path);

    }
    return obj;
}

gboolean pcv_tls_pki_init(const gchar *pki_dir, GError **error)
{
    if (!pki_dir) {
        g_set_error(error, g_quark_from_static_string("tls"), 1, "pki_dir required");
        return FALSE;
    }

    if (g_mkdir_with_parents(pki_dir, 0700) < 0) {
        g_set_error(error, g_quark_from_static_string("tls"), 2,
                    "Failed to create PKI dir: %s", pki_dir);
        return FALSE;
    }
    PCV_LOG_INFO(TLS_LOG_DOM, "PKI directory ready: %s", pki_dir);
    return TRUE;
}

gint64
pcv_tls_get_cert_expiry_days(void)
{
    if (!G.ctx || !G.ctx->cert_path)
        return -1;

    GError *err = NULL;
    GTlsCertificate *cert = g_tls_certificate_new_from_file(G.ctx->cert_path, &err);
    if (!cert) {
        if (err) g_error_free(err);
        return -1;
    }

    GDateTime *not_after = g_tls_certificate_get_not_valid_after(cert);
    if (!not_after) {
        g_object_unref(cert);
        return -1;
    }

    GDateTime *now = g_date_time_new_now_utc();
    GTimeSpan diff = g_date_time_difference(not_after, now);
    gint64 days = diff / G_TIME_SPAN_DAY;

    g_date_time_unref(now);
    g_date_time_unref(not_after);
    g_object_unref(cert);

    return days;
}

void
pcv_tls_check_expiry_warning(void)
{
    gint64 days = pcv_tls_get_cert_expiry_days();
    if (days < 0)
        return;

    if (days < 7) {
        PCV_LOG_ERROR(TLS_LOG_DOM,
                      "TLS certificate expires in %" G_GINT64_FORMAT " days! Renew immediately!",
                      days);
    } else if (days < 30) {
        PCV_LOG_WARN(TLS_LOG_DOM,
                     "TLS certificate expires in %" G_GINT64_FORMAT " days — plan renewal",
                     days);
    }
}

void pcv_tls_init_from_config(void)
{

    const gchar *min_version = pcv_config_get_string("tls", "min_version", "1.2");
    const gchar *tls_priority;
    if (g_strcmp0(min_version, "1.3") == 0) {
        tls_priority = "NORMAL:-VERS-ALL:+VERS-TLS1.3";
    } else {
        if (g_strcmp0(min_version, "1.2") != 0)
            PCV_LOG_WARN(TLS_LOG_DOM,
                         "알 수 없는 [tls] min_version=%s — TLS 1.2 기준선으로 폴백",
                         min_version);
        tls_priority = "NORMAL:-VERS-ALL:+VERS-TLS1.2:+VERS-TLS1.3";
    }
    g_setenv("G_TLS_GNUTLS_PRIORITY", tls_priority, TRUE);
    PCV_LOG_INFO(TLS_LOG_DOM,
                 "TLS 최소 버전 고정: min_version=%s (GnuTLS priority=%s)",
                 min_version, tls_priority);

    const gchar *enabled = pcv_config_get_string("tls", "enabled", "false");
    if (g_strcmp0(enabled, "true") != 0) {
        PCV_LOG_INFO(TLS_LOG_DOM, "TLS disabled (daemon.conf [tls] enabled != true)");
        G.initialized = TRUE;
        return;
    }

    const gchar *cert = pcv_config_get_string("tls", "cert", "/etc/purecvisor/pki/node.crt");
    const gchar *key = pcv_config_get_string("tls", "key", "/etc/purecvisor/pki/node.key");
    const gchar *ca = pcv_config_get_string("tls", "ca", "/etc/purecvisor/pki/ca.crt");

    GError *err = NULL;
    G.ctx = pcv_tls_ctx_new(cert, key, ca, &err);
    if (!G.ctx) {

        PCV_LOG_WARN(TLS_LOG_DOM, "TLS init failed: %s — running without TLS",
                     err ? err->message : "unknown");
        if (err) g_error_free(err);
    }
    G.initialized = TRUE;
}
