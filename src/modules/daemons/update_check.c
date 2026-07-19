
#include "update_check.h"
#include <json-glib/json-glib.h>
#include <string.h>
#include <stdio.h>
#include <libsoup/soup.h>
#include "utils/pcv_ssrf.h"
#include "utils/pcv_config.h"
#include "purecvisor/version.h"
#include "utils/pcv_log.h"

#define UC_LOG_DOM "update_check"

static gboolean _parse_semver(const char *s, int *maj, int *min, int *pat)
{
    if (!s || !*s) return FALSE;
    if (*s == 'v' || *s == 'V') s++;
    int a = 0, b = 0, c = 0;
    if (sscanf(s, "%d.%d.%d", &a, &b, &c) != 3) return FALSE;
    if (a < 0 || b < 0 || c < 0) return FALSE;
    *maj = a; *min = b; *pat = c;
    return TRUE;
}

gboolean pcv_update_check_compare(const char *current, const char *latest,
                                  gboolean *update_available)
{

    int cM, cm, cp, lM, lm, lp;
    if (!_parse_semver(current, &cM, &cm, &cp)) return FALSE;
    if (!_parse_semver(latest,  &lM, &lm, &lp)) return FALSE;
    gboolean up = (lM > cM) ||
                  (lM == cM && lm > cm) ||
                  (lM == cM && lm == cm && lp > cp);
    if (update_available) *update_available = up;
    return TRUE;
}

gboolean pcv_update_check_parse_release(const char *json, gssize len,
                                        char **tag_out, char **url_out)
{
    if (tag_out) *tag_out = nullptr;
    if (url_out) *url_out = nullptr;
    if (!json) return FALSE;

    JsonParser *jp = json_parser_new();
    GError *err = nullptr;
    if (!json_parser_load_from_data(jp, json, len, &err)) {
        g_clear_error(&err);
        g_object_unref(jp);
        return FALSE;
    }
    JsonNode *root_node = json_parser_get_root(jp);
    if (!root_node || !JSON_NODE_HOLDS_OBJECT(root_node)) { g_object_unref(jp); return FALSE; }
    JsonObject *root = json_node_get_object(root_node);

    JsonNode *tn = json_object_get_member(root, "tag_name");
    if (!tn || !JSON_NODE_HOLDS_VALUE(tn) || json_node_get_value_type(tn) != G_TYPE_STRING) {
        g_object_unref(jp); return FALSE;
    }
    const char *tag = json_node_get_string(tn);
    int a, b, c;
    if (!_parse_semver(tag, &a, &b, &c)) { g_object_unref(jp); return FALSE; }

    if (tag_out) *tag_out = g_strdup_printf("%d.%d.%d", a, b, c);

    if (url_out) {
        JsonNode *un = json_object_get_member(root, "html_url");
        if (un && JSON_NODE_HOLDS_VALUE(un) && json_node_get_value_type(un) == G_TYPE_STRING) {
            const char *url = json_node_get_string(un);
            if (url && g_str_has_prefix(url, "https://github.com/"))
                *url_out = g_strdup(url);
        }
    }
    g_object_unref(jp);
    return TRUE;
}

#define UC_MAX_BODY (256 * 1024)
#define UC_TIMEOUT_SEC 8

static struct {
    gboolean enabled;
    char     url[512];
    gint     interval_sec;

    GMutex   mu;
    char     latest[32];
    char     url_cache[256];
    gint64   checked_at;
    gint64   last_attempt_mono;
    char     state[16];
    gboolean in_flight;
} G;

static gboolean _fetch_latest(char **tag_out, char **url_out)
{
    GError *ssrf_err = nullptr;
    if (!pcv_url_target_allowed(G.url, &ssrf_err)) {
        PCV_LOG_WARN(UC_LOG_DOM, "update check URL rejected (SSRF): %s",
                     ssrf_err ? ssrf_err->message : "blocked");
        g_clear_error(&ssrf_err);
        return FALSE;
    }

    SoupSession *sess = soup_session_new();
    g_object_set(sess, "timeout", UC_TIMEOUT_SEC, nullptr);
    SoupMessage *msg = soup_message_new("GET", G.url);
    if (!msg) { g_object_unref(sess); return FALSE; }
    soup_message_set_flags(msg, SOUP_MESSAGE_NO_REDIRECT);
    SoupMessageHeaders *hdrs = soup_message_get_request_headers(msg);
    soup_message_headers_replace(hdrs, "Accept", "application/vnd.github+json");
    soup_message_headers_replace(hdrs, "User-Agent", "purecvisor-single");

    GBytes *resp = soup_session_send_and_read(sess, msg, nullptr, nullptr);
    gboolean ok = FALSE;
    if (resp && soup_message_get_status(msg) == 200) {
        gsize sz = 0;
        const char *data = g_bytes_get_data(resp, &sz);
        if (sz > 0 && sz <= UC_MAX_BODY)
            ok = pcv_update_check_parse_release(data, (gssize)sz, tag_out, url_out);
    }
    if (resp) g_bytes_unref(resp);
    g_object_unref(msg);
    g_object_unref(sess);
    return ok;
}

static gpointer _refresh_thread(gpointer unused)
{
    (void)unused;
    char *tag = nullptr, *url = nullptr;
    gboolean ok = _fetch_latest(&tag, &url);
    g_mutex_lock(&G.mu);
    if (ok && tag) {
        g_strlcpy(G.latest, tag, sizeof G.latest);
        g_strlcpy(G.url_cache, url ? url : "", sizeof G.url_cache);
        G.checked_at = g_get_real_time() / G_USEC_PER_SEC;
        g_strlcpy(G.state, "ok", sizeof G.state);
    } else {
        g_strlcpy(G.state, "unknown", sizeof G.state);
    }
    G.in_flight = FALSE;
    g_mutex_unlock(&G.mu);
    g_free(tag); g_free(url);
    return nullptr;
}

void pcv_update_check_init(void)
{
    memset(&G, 0, sizeof G);
    g_mutex_init(&G.mu);
    const char *en = pcv_config_get_string("update", "check_enabled", "true");
    G.enabled = (g_ascii_strcasecmp(en, "true") == 0 || g_strcmp0(en, "1") == 0);
    const char *url = pcv_config_get_string("update", "check_url",
        "https://api.github.com/repos/HardcoreMonk/purecvisor/releases/latest");
    g_strlcpy(G.url, url, sizeof G.url);
    gint hours = pcv_config_get_int("update", "check_interval_hours", 24);
    if (hours < 1) hours = 24;
    G.interval_sec = hours * 3600;
    g_strlcpy(G.state, G.enabled ? "unknown" : "disabled", sizeof G.state);
}

PcvUpdateStatus pcv_update_check_get(void)
{
    PcvUpdateStatus s;
    memset(&s, 0, sizeof s);
    s.enabled = G.enabled;
    g_strlcpy(s.current, PCV_PRODUCT_VERSION, sizeof s.current);

    g_mutex_lock(&G.mu);
    gint64 mono_now = g_get_monotonic_time() / G_USEC_PER_SEC;
    gboolean stale = (G.last_attempt_mono == 0) || (mono_now - G.last_attempt_mono >= G.interval_sec);

    if (G.enabled && stale && !G.in_flight) {
        G.in_flight = TRUE;
        G.last_attempt_mono = mono_now;
        GThread *t = g_thread_try_new("update-check", _refresh_thread, nullptr, nullptr);
        if (t) g_thread_unref(t); else { G.in_flight = FALSE; G.last_attempt_mono = mono_now; }
    }
    g_strlcpy(s.state, G.enabled ? G.state : "disabled", sizeof s.state);
    g_strlcpy(s.latest, G.latest, sizeof s.latest);
    g_strlcpy(s.url, G.url_cache, sizeof s.url);
    s.checked_at = G.checked_at;
    g_mutex_unlock(&G.mu);

    if (G.enabled && s.latest[0] && g_strcmp0(s.state, "ok") == 0)
        pcv_update_check_compare(s.current, s.latest, &s.update_available);
    return s;
}
