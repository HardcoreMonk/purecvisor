/**
 * @file update_check.c
 * @brief 버전 알림 — GitHub 공개 repo 최신 릴리스 조회 + 캐시 + semver 비교.
 *
 * 읽기 전용(정보 표시만): 바이너리 fetch/execve/hot_reload 를 일절 하지 않으므로
 * ADR-0027이 수용한 hot-reload 무결성 리스크를 재개방하지 않는다. UI는 CSP
 * (connect-src 'self')상 GitHub 를 직접 호출할 수 없어, 데몬이 서버측 프록시로
 * 조회·캐시하고 무인증 endpoint(daemon.update_check)로 결과만 노출한다.
 *
 * 무인증 노출의 증폭 방지: 조회는 캐시 TTL(check_interval) + single-flight 로
 * 게이트되어, 요청이 폭주해도 실제 GitHub 조회는 interval당 1회 이하다(실패
 * 경로에서도 — last_attempt_mono 로 앵커).
 */
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

/* 선행 'v' 제거 후 major.minor.patch 3정수 파싱. pre-release/빌드메타는 코어만. */
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
    /* 어느 쪽이든 semver 파싱 실패 시 FALSE("unknown") — 호출자는 이 경우
     * 업데이트 판정을 하지 않고 현재 버전만 표시한다(잘못된 "구버전" 오탐 방지). */
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

    /* tag_name/html_url 멤버는 존재하더라도 타입이 string 이 아닐 수 있다
     * (크래프트된 응답 등) — json_object_get_string_member 를 그대로 쓰면 그
     * 경우 NULL 반환과 함께 g_critical 로그가 남으므로, 값/타입을 먼저
     * 확인해 조용히 실패(tag) / 스킵(url) 한다. */
    JsonNode *tn = json_object_get_member(root, "tag_name");
    if (!tn || !JSON_NODE_HOLDS_VALUE(tn) || json_node_get_value_type(tn) != G_TYPE_STRING) {
        g_object_unref(jp); return FALSE;
    }
    const char *tag = json_node_get_string(tn);
    int a, b, c;
    if (!_parse_semver(tag, &a, &b, &c)) { g_object_unref(jp); return FALSE; }

    /* tag_name/html_url 은 외부(GitHub) 응답 = untrusted. tag 는 위에서 semver
     * 검증을 통과한 것만, url 은 https://github.com/ prefix 인 것만 채택한다
     * (UI가 이 url 을 링크로 여는 만큼, 임의 스킴/호스트 주입을 원천 차단). */
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

#define UC_MAX_BODY (256 * 1024)   /* 응답 본문 상한 */
#define UC_TIMEOUT_SEC 8

static struct {
    gboolean enabled;
    char     url[512];
    gint     interval_sec;
    /* 캐시 */
    GMutex   mu;
    char     latest[32];
    char     url_cache[256];
    gint64   checked_at;      /* 마지막 성공 조회 wall-clock epoch(초), 0=미조회 (표시 전용) */
    gint64   last_attempt_mono; /* 마지막 조회 "시도" monotonic 초(성공/실패 무관), 0=미시도 */
    char     state[16];       /* "ok"/"unknown"/"disabled" */
    gboolean in_flight;
} G;

/* GitHub 1회 조회 — 성공 시 tag/url 반환(호출자 free). 아웃바운드 가드 전부 적용. */
static gboolean _fetch_latest(char **tag_out, char **url_out)
{
    GError *ssrf_err = nullptr;
    if (!pcv_url_target_allowed(G.url, &ssrf_err)) {
        PCV_LOG_WARN(UC_LOG_DOM, "update check URL rejected (SSRF): %s",
                     ssrf_err ? ssrf_err->message : "blocked");
        g_clear_error(&ssrf_err);
        return FALSE;
    }
    /* 아웃바운드 가드 스택(webhook 발송과 동일 관행): SSRF 검사(위)로 대상 IP를
     * resolve해 링크로컬/메타데이터 차단 → 타임아웃으로 정지 서버 무한블록 방지 →
     * NO_REDIRECT로 리다이렉트-우회 차단. TLS 검증은 libsoup 기본. */
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

/* 백그라운드 갱신 스레드 — single-flight 보장은 호출측(get)에서. */
static gpointer _refresh_thread(gpointer unused)
{
    (void)unused;
    char *tag = nullptr, *url = nullptr;
    gboolean ok = _fetch_latest(&tag, &url);
    g_mutex_lock(&G.mu);
    if (ok && tag) {
        g_strlcpy(G.latest, tag, sizeof G.latest);
        g_strlcpy(G.url_cache, url ? url : "", sizeof G.url_cache);
        G.checked_at = g_get_real_time() / G_USEC_PER_SEC;   /* wall-clock epoch(초) */
        g_strlcpy(G.state, "ok", sizeof G.state);
    } else {
        g_strlcpy(G.state, "unknown", sizeof G.state);  /* 직전 latest/url 유지 */
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

    /* stale 판정은 monotonic 앵커(last_attempt_mono)로 — wall-clock(checked_at)은
     * NTP 점프에 취약해 주기 계산엔 부적합하고, 표시 전용으로만 쓴다. 실패해도
     * last_attempt_mono 를 앞당기므로(아래) 실패 폭주 시에도 재조회는 interval당 1회. */
    g_mutex_lock(&G.mu);
    gint64 mono_now = g_get_monotonic_time() / G_USEC_PER_SEC;
    gboolean stale = (G.last_attempt_mono == 0) || (mono_now - G.last_attempt_mono >= G.interval_sec);
    /* single-flight: in_flight 검사·설정을 뮤텍스 안에서 → 동시 get() 이 스레드를
     * 이중 spawn하지 않는다. 네트워크 I/O는 스레드에서만(락 밖) 수행해 get()은 즉시 반환. */
    if (G.enabled && stale && !G.in_flight) {
        G.in_flight = TRUE;
        G.last_attempt_mono = mono_now;   /* 성공/실패 무관하게 모든 시도를 앵커 → interval 전 재조회 금지 */
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
