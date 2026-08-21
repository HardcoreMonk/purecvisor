                                    
                                                            
  
                           
                                                   
                                                    
                                        
  
                                            
                                                   
                                                    
                                                        
                                                     
                           
  
                                               
                                                    
                                
       
                                                             
                                                        
                       
                                                             
                                                         
                                                             
                                             
                                                     
                                         
                                                    
                                                      
                         
   
#include "modules/daemons/pcv_webpush.h"
#include "modules/daemons/pcv_webpush_crypto.h"
#include "modules/audit/pcv_audit.h"
#include "utils/pcv_log.h"
#include "utils/pcv_secure.h"

#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/pem.h>

#define WP_LOG_DOM  "webpush"

                                                            

#define WP_PUB_LEN      65u                                
#define WP_PRIV_LEN     32u                      
#define WP_TTL_SEC      300u                             
#define WP_HTTP_TIMEOUT 10                                   
#define WP_BODY_MAX     2048u                        
#define WP_PT_MAX       4079u                                        
#define WP_EP_SUMMARY   80u                                        
                                                 
                                                                     
                                                                 
                                                     
#define WP_IDLE_WAIT_SEC 30

                                                                         
#define WP_DDL_SUBSCRIPTIONS \
    "CREATE TABLE IF NOT EXISTS subscriptions (" \
    "  id INTEGER PRIMARY KEY," \
    "  username TEXT NOT NULL," \
    "  endpoint TEXT NOT NULL UNIQUE," \
    "  p256dh TEXT NOT NULL," \
    "  auth TEXT NOT NULL," \
    "  created_at INTEGER NOT NULL," \
    "  last_ok_at INTEGER," \
    "  fail_count INTEGER NOT NULL DEFAULT 0)"

                                                                      
                                                                     
                          

                                                         
                       
GQuark
pcv_webpush_error_quark(void)
{
    return g_quark_from_static_string("webpush");
}

                                                                
                                      
static GQuark
_wp_quark(void)
{
    return pcv_webpush_error_quark();
}

                                                            

  
                                  
                                                  
                                        
   
typedef struct {
    gint64  id;
    gchar  *endpoint;
    gchar  *p256dh;
    gchar  *auth;
    guint   status;                                    
} WpSub;

static struct {
    GMutex   mu;                              
    GCond    cond;                                 
    GCancellable *cancel;                                            
    sqlite3 *db;                                            
    gchar   *pem_path;                             
    gchar   *vapid_privkey;                                               
    gchar   *vapid_pub;                              
    gboolean enabled;                           
    gboolean crit_only;                      
    gchar   *contact;                                                        
    gboolean closing;                                      
    guint    inflight;                         
    PcvWebpushPostFn post;                              
} G;

                                                      

  
                                    
                                                 
                                                    
   
static gchar *
_b64url_enc(const guchar *data, gsize len)
{
    gchar *b64 = g_base64_encode(data, len);
    for (gchar *p = b64; *p != '\0'; p++) {
        if (*p == '+')      *p = '-';
        else if (*p == '/') *p = '_';
    }
    gsize blen = strlen(b64);
    while (blen > 0 && b64[blen - 1] == '=')
        b64[--blen] = '\0';
    return b64;
}

  
                                                         
                                                    
   
static gboolean
_b64url_dec_fixed(const gchar *str, guchar *out, gsize expect)
{
    if (str == NULL || str[0] == '\0')
        return FALSE;

    gsize    slen   = strlen(str);
    gsize    pad    = (4u - (slen % 4u)) % 4u;
    GString *padded = g_string_new(str);
    for (gchar *p = padded->str; *p != '\0'; p++) {
        if (*p == '-')      *p = '+';
        else if (*p == '_') *p = '/';
    }
    for (gsize i = 0; i < pad; i++)
        g_string_append_c(padded, '=');

    gsize   n   = 0;
    guchar *raw = g_base64_decode(padded->str, &n);
    g_string_free(padded, TRUE);

    gboolean ok = (raw != NULL && n == expect);
    if (ok)
        memcpy(out, raw, expect);
    if (raw != NULL) {
        pcv_secure_wipe(raw, n);                               
        g_free(raw);
    }
    return ok;
}

                                                             

  
                                                    
                      
   
static gchar *
_truncate_utf8(const gchar *s, gsize max_bytes)
{
    if (s == NULL)
        return g_strdup("");
    if (strlen(s) <= max_bytes)
        return g_strdup(s);

    gchar       *cut = g_strndup(s, max_bytes);
    const gchar *end = NULL;
                                              
    if (!g_utf8_validate(cut, -1, &end))
        *((gchar *)end) = '\0';
    return cut;
}

                                                 
static gchar *
_ep_summary(const gchar *endpoint)
{
    return _truncate_utf8(endpoint != NULL ? endpoint : "", WP_EP_SUMMARY);
}

  
                                                                          
                                    
   
static gchar *
_origin_of(const gchar *endpoint)
{
    GUri *uri = g_uri_parse(endpoint, G_URI_FLAGS_NONE, NULL);
    if (uri == NULL)
        return NULL;

    const gchar *scheme = g_uri_get_scheme(uri);
    const gchar *host   = g_uri_get_host(uri);
    gint         port   = g_uri_get_port(uri);
    gchar       *origin = NULL;

    if (scheme != NULL && host != NULL && host[0] != '\0') {
                                                         
                                      
        if (port > 0 && port != 443)
            origin = g_strdup_printf("%s://%s:%d", scheme, host, port);
        else
            origin = g_strdup_printf("%s://%s", scheme, host);
    }
    g_uri_unref(uri);
    return origin;
}

                                                                

  
                               
  
                                               
  
                                                                     
                                                                               
                                                               
   
static gboolean
_addr_blocked(GInetAddress *addr)
{
    if (addr == NULL)
        return TRUE;                            

    if (g_inet_address_get_is_loopback(addr)
        || g_inet_address_get_is_link_local(addr)
        || g_inet_address_get_is_site_local(addr)
        || g_inet_address_get_is_any(addr))
        return TRUE;

    const guint8 *b = g_inet_address_to_bytes(addr);
    if (b == NULL)
        return TRUE;

    if (g_inet_address_get_family(addr) == G_SOCKET_FAMILY_IPV6) {
                                                       
                                                                    
        static const guint8 v4map_prefix[12] = {
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF
        };
        if (memcmp(b, v4map_prefix, sizeof v4map_prefix) == 0) {
            GInetAddress *inner =
                g_inet_address_new_from_bytes(b + 12, G_SOCKET_FAMILY_IPV4);
            gboolean blocked = _addr_blocked(inner);
            if (inner != NULL)
                g_object_unref(inner);
            return blocked;
        }
                                                                             
        if ((b[0] & 0xFE) == 0xFC)
            return TRUE;
        return FALSE;
    }

    if (b[0] == 0)                                                            
        return TRUE;
    if (b[0] == 100 && (b[1] & 0xC0) == 0x40)                                  
        return TRUE;
    return FALSE;
}

  
                                               
  
                                                 
                                                   
                   
  
             
                                                        
                              
                                                        
                                        
                                                         
                                            
                                                         
  
                                                           
                                                                 
                      
                                            
                                          
                                                   
                                             
                                                              
                                      
                                                    
                                  
                     
   
gboolean
pcv_webpush_endpoint_allowed(const gchar *endpoint, GError **error)
{
    if (endpoint == NULL || endpoint[0] == '\0') {
        g_set_error(error, _wp_quark(), WP_ERR_ARG, "endpoint 가 비어 있습니다");
        return FALSE;
    }

    GUri *uri = g_uri_parse(endpoint, G_URI_FLAGS_NONE, NULL);
    if (uri == NULL) {
        g_set_error(error, _wp_quark(), WP_ERR_ARG,
                    "endpoint URL 파싱 실패: %.80s", endpoint);
        return FALSE;
    }

    const gchar *scheme = g_uri_get_scheme(uri);
    if (scheme == NULL || g_ascii_strcasecmp(scheme, "https") != 0) {
        g_set_error(error, _wp_quark(), WP_ERR_BLOCKED,
                    "endpoint 는 https 만 허용합니다: %.80s", endpoint);
        g_uri_unref(uri);
        return FALSE;
    }

    const gchar *host = g_uri_get_host(uri);
    if (host == NULL || host[0] == '\0') {
        g_set_error(error, _wp_quark(), WP_ERR_ARG,
                    "endpoint 에 host 가 없습니다: %.80s", endpoint);
        g_uri_unref(uri);
        return FALSE;
    }
    gchar *host_dup = g_strdup(host);
    g_uri_unref(uri);

                                                         
                                       
    GInetAddress *literal = g_inet_address_new_from_string(host_dup);
    if (literal != NULL) {
        gboolean blocked = _addr_blocked(literal);
        g_object_unref(literal);
        if (blocked) {
            g_set_error(error, _wp_quark(), WP_ERR_BLOCKED,
                        "endpoint host '%s' 가 내부/예약 대역입니다", host_dup);
            g_free(host_dup);
            return FALSE;
        }
        g_free(host_dup);
        return TRUE;
    }

                                                            
    GError    *rerr  = NULL;
    GResolver *res   = g_resolver_get_default();
    GList     *addrs = g_resolver_lookup_by_name(res, host_dup, NULL, &rerr);
    if (addrs == NULL) {
        g_set_error(error, _wp_quark(), WP_ERR_BLOCKED,
                    "endpoint host '%s' resolve 실패: %s", host_dup,
                    rerr != NULL ? rerr->message : "unknown");
        g_clear_error(&rerr);
        g_object_unref(res);
        g_free(host_dup);
        return FALSE;
    }

    gboolean blocked = FALSE;
    for (GList *l = addrs; l != NULL && !blocked; l = l->next)
        blocked = _addr_blocked(G_INET_ADDRESS(l->data));

    g_resolver_free_addresses(addrs);
    g_object_unref(res);

    if (blocked) {
        g_set_error(error, _wp_quark(), WP_ERR_BLOCKED,
                    "endpoint host '%s' 가 내부/예약 대역으로 resolve 됩니다",
                    host_dup);
        g_free(host_dup);
        return FALSE;
    }
    g_free(host_dup);
    return TRUE;
}

                                                               

  
                                                                      
                                            
                     
   
static EVP_PKEY *
_p256_pkey(const guchar *pub65, const guchar *priv32)
{
    EVP_PKEY       *pkey = NULL;
    OSSL_PARAM_BLD *bld  = OSSL_PARAM_BLD_new();
    OSSL_PARAM     *par  = NULL;
    EVP_PKEY_CTX   *ctx  = NULL;
    BIGNUM         *d    = NULL;
    gboolean        ok   = (bld != NULL);

    if (ok)
        ok = (OSSL_PARAM_BLD_push_utf8_string(bld, OSSL_PKEY_PARAM_GROUP_NAME,
                                              "prime256v1", 0) == 1);
    if (ok)
        ok = (OSSL_PARAM_BLD_push_octet_string(bld, OSSL_PKEY_PARAM_PUB_KEY,
                                               pub65, WP_PUB_LEN) == 1);
    if (ok) {
                                                                
        d  = BN_bin2bn(priv32, (int)WP_PRIV_LEN, NULL);
        ok = (d != NULL
              && OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_PRIV_KEY, d) == 1);
    }
    if (ok)
        ok = ((par = OSSL_PARAM_BLD_to_param(bld)) != NULL);
    if (ok)
        ok = ((ctx = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL)) != NULL
              && EVP_PKEY_fromdata_init(ctx) == 1);
    if (ok && EVP_PKEY_fromdata(ctx, &pkey, EVP_PKEY_KEYPAIR, par) != 1)
        ok = FALSE;

    if (ctx != NULL) EVP_PKEY_CTX_free(ctx);
    if (par != NULL) OSSL_PARAM_free(par);
    if (d != NULL)   BN_clear_free(d);                            
    if (bld != NULL) OSSL_PARAM_BLD_free(bld);
    if (!ok && pkey != NULL) {
        EVP_PKEY_free(pkey);
        pkey = NULL;
    }
    return pkey;
}

  
                                       
  
                                            
                                                 
   
static gboolean
_pem_save(const gchar *path, const gchar *privkey_b64, const gchar *pub_b64,
          GError **error)
{
    guchar priv_raw[WP_PRIV_LEN];
    guchar pub_raw[WP_PUB_LEN];

    if (!_b64url_dec_fixed(privkey_b64, priv_raw, WP_PRIV_LEN)
        || !_b64url_dec_fixed(pub_b64, pub_raw, WP_PUB_LEN)) {
        pcv_secure_wipe(priv_raw, sizeof priv_raw);
        g_set_error(error, _wp_quark(), WP_ERR_KEY, "VAPID 키 디코드 실패");
        return FALSE;
    }

    EVP_PKEY *pkey = _p256_pkey(pub_raw, priv_raw);
    pcv_secure_wipe(priv_raw, sizeof priv_raw);
    if (pkey == NULL) {
        g_set_error(error, _wp_quark(), WP_ERR_KEY, "VAPID EVP_PKEY 구성 실패");
        return FALSE;
    }

                                                         
                                                
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        g_set_error(error, _wp_quark(), WP_ERR_KEY,
                    "VAPID PEM 열기 실패(%s): %s", path, g_strerror(errno));
        EVP_PKEY_free(pkey);
        return FALSE;
    }
    FILE *fp = fdopen(fd, "w");
    if (fp == NULL) {
        g_set_error(error, _wp_quark(), WP_ERR_KEY,
                    "VAPID PEM fdopen 실패: %s", g_strerror(errno));
        close(fd);
        EVP_PKEY_free(pkey);
        return FALSE;
    }

    gboolean ok = (PEM_write_PrivateKey(fp, pkey, NULL, NULL, 0, NULL, NULL) == 1);
    fclose(fp);                                
    EVP_PKEY_free(pkey);
                                                  
    if (ok && g_chmod(path, 0600) != 0)
        PCV_LOG_WARN(WP_LOG_DOM, "VAPID PEM 권한 설정 실패(%s)", path);
    if (!ok)
        g_set_error(error, _wp_quark(), WP_ERR_KEY, "VAPID PEM 쓰기 실패: %s", path);
    return ok;
}

  
                                            
                                                             
   
static gboolean
_pem_load(const gchar *path, gchar **out_privkey, gchar **out_pub, GError **error)
{
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        g_set_error(error, _wp_quark(), WP_ERR_KEY,
                    "VAPID PEM 열기 실패(%s): %s", path, g_strerror(errno));
        return FALSE;
    }
    EVP_PKEY *pkey = PEM_read_PrivateKey(fp, NULL, NULL, NULL);
    fclose(fp);
    if (pkey == NULL) {
        g_set_error(error, _wp_quark(), WP_ERR_KEY, "VAPID PEM 파싱 실패: %s", path);
        return FALSE;
    }

    guchar   priv_raw[WP_PRIV_LEN];
    guchar   pub_raw[WP_PUB_LEN];
    size_t   publen = 0;
    BIGNUM  *d      = NULL;
    gboolean ok     = (EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_PRIV_KEY, &d) == 1
                       && BN_bn2binpad(d, priv_raw, (int)WP_PRIV_LEN)
                              == (int)WP_PRIV_LEN
                       && EVP_PKEY_get_octet_string_param(
                              pkey, OSSL_PKEY_PARAM_ENCODED_PUBLIC_KEY,
                              pub_raw, sizeof pub_raw, &publen) == 1
                       && publen == WP_PUB_LEN);

    if (d != NULL) BN_clear_free(d);
    EVP_PKEY_free(pkey);

    if (!ok) {
        pcv_secure_wipe(priv_raw, sizeof priv_raw);
        g_set_error(error, _wp_quark(), WP_ERR_KEY,
                    "VAPID PEM 이 P-256 키쌍이 아닙니다: %s", path);
        return FALSE;
    }

    *out_privkey = _b64url_enc(priv_raw, WP_PRIV_LEN);
    *out_pub     = _b64url_enc(pub_raw, WP_PUB_LEN);
    pcv_secure_wipe(priv_raw, sizeof priv_raw);
    return TRUE;
}

  
                                                 
                                   
   
static gboolean
_ensure_vapid_locked(GError **error)
{
    if (G.vapid_privkey != NULL && G.vapid_pub != NULL)
        return TRUE;
    if (G.pem_path == NULL) {
        g_set_error(error, _wp_quark(), WP_ERR_NOT_INIT, "webpush 미초기화");
        return FALSE;
    }

    gchar *privkey = NULL;
    gchar *pub     = NULL;
    if (!pcv_webpush_keypair_generate(&privkey, &pub, error))
        return FALSE;
    if (!_pem_save(G.pem_path, privkey, pub, error)) {
        pcv_secure_free_str(&privkey);
        g_free(pub);
        return FALSE;
    }

    pcv_secure_free_str(&G.vapid_privkey);
    g_free(G.vapid_pub);
    G.vapid_privkey = privkey;
    G.vapid_pub     = pub;
    PCV_LOG_INFO(WP_LOG_DOM, "VAPID 키 생성·저장 완료: %s", G.pem_path);
    return TRUE;
}

                                                                

                                                 
static gboolean
_exec_locked(const gchar *sql, GError **error)
{
    gchar *errmsg = NULL;
    int    rc     = sqlite3_exec(G.db, sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        g_set_error(error, _wp_quark(), WP_ERR_DB, "sqlite 실패: %s",
                    errmsg != NULL ? errmsg : sqlite3_errmsg(G.db));
        sqlite3_free(errmsg);
        return FALSE;
    }
    return TRUE;
}

                                                              
static guint
_delete_all_locked(void)
{
    if (!_exec_locked("DELETE FROM subscriptions", NULL))
        return 0;
    int changed = sqlite3_changes(G.db);
    return changed > 0 ? (guint)changed : 0u;
}

                                     
static void
_sub_free(gpointer p)
{
    WpSub *s = p;
    g_free(s->endpoint);
    g_free(s->p256dh);
    g_free(s->auth);
    g_free(s);
}

  
                                             
                                             
                                                 
   
static GPtrArray *
_snapshot_locked(const gchar *username)
{
    GPtrArray    *out  = g_ptr_array_new_with_free_func(_sub_free);
    const gchar  *sql  = (username != NULL)
        ? "SELECT id,endpoint,p256dh,auth FROM subscriptions WHERE username=? ORDER BY id"
        : "SELECT id,endpoint,p256dh,auth FROM subscriptions ORDER BY id";
    sqlite3_stmt *st   = NULL;

    if (sqlite3_prepare_v2(G.db, sql, -1, &st, NULL) != SQLITE_OK)
        return out;
    if (username != NULL)
        sqlite3_bind_text(st, 1, username, -1, SQLITE_TRANSIENT);

    while (sqlite3_step(st) == SQLITE_ROW) {
        WpSub *s    = g_new0(WpSub, 1);
        s->id       = sqlite3_column_int64(st, 0);
        s->endpoint = g_strdup((const gchar *)sqlite3_column_text(st, 1));
        s->p256dh   = g_strdup((const gchar *)sqlite3_column_text(st, 2));
        s->auth     = g_strdup((const gchar *)sqlite3_column_text(st, 3));
        g_ptr_array_add(out, s);
    }
    sqlite3_finalize(st);
    return out;
}

                                                                

  
                                               
  
                                         
                                                 
   
  
                                                       
                                                        
                                                    
                                         
   
static GCancellable *
_cancel_ref(void)
{
    g_mutex_lock(&G.mu);
    GCancellable *c = (G.cancel != NULL) ? g_object_ref(G.cancel) : NULL;
    g_mutex_unlock(&G.mu);
    return c;
}

static guint
_post_real(const gchar *endpoint, const guchar *body, gsize len,
           const gchar *vapid_auth, const gchar *urgency, guint ttl)
{
    SoupSession *sess = soup_session_new();
    g_object_set(sess, "timeout", WP_HTTP_TIMEOUT, NULL);

    SoupMessage *msg = soup_message_new("POST", endpoint);
    if (msg == NULL) {
        g_object_unref(sess);
        return 0;
    }
                                                         
    soup_message_set_flags(msg, SOUP_MESSAGE_NO_REDIRECT);

                                                                       
                                                           
    GBytes *payload = g_bytes_new(body, len);
    soup_message_set_request_body_from_bytes(msg, "application/octet-stream",
                                             payload);

    SoupMessageHeaders *hdrs = soup_message_get_request_headers(msg);
    gchar              *ttls = g_strdup_printf("%u", ttl);
    soup_message_headers_replace(hdrs, "TTL", ttls);
    soup_message_headers_replace(hdrs, "Content-Encoding", "aes128gcm");
    soup_message_headers_replace(hdrs, "Urgency", urgency);
    soup_message_headers_replace(hdrs, "Authorization", vapid_auth);
    g_free(ttls);

                                                           
                                                   
    GCancellable *cancel = _cancel_ref();
    GBytes       *resp   = soup_session_send_and_read(sess, msg, cancel, NULL);
    guint         status = soup_message_get_status(msg);
    if (resp == NULL)
        status = 0;                                

    if (cancel != NULL) g_object_unref(cancel);
    if (resp != NULL) g_bytes_unref(resp);
    g_bytes_unref(payload);
    g_object_unref(msg);
    g_object_unref(sess);
    return status;
}

                                                              

                                           
typedef struct {
    GPtrArray *subs;                      
    gchar     *payload;                
    gsize      payload_len;
    gchar     *urgency;                          
    gchar     *privkey;                                     
    gchar     *pub;                         
    gchar     *contact;                                        
    GCancellable *cancel;                       
    PcvWebpushPostFn post;
} WpBatch;

  
                                   
  
                                             
                                               
  
                                                              
                                                
            
  
                                                     
                                                      
                                                                        
                                                     
   
static void
_batch_free(gpointer p)
{
    WpBatch *b = p;
    g_ptr_array_unref(b->subs);
    g_free(b->payload);
    g_free(b->urgency);
    pcv_secure_free_str(&b->privkey);
    g_free(b->pub);
    g_free(b->contact);
    g_clear_object(&b->cancel);
    g_free(b);
}

  
                                   
                                    
   
static guint
_send_one(const WpBatch *b, const WpSub *sub)
{
    GError *err   = NULL;
    guchar *body  = NULL;
    gsize   blen  = 0;

                                                          
                                                 
    if (!pcv_webpush_encrypt((const guchar *)b->payload, b->payload_len,
                             sub->p256dh, sub->auth, NULL, NULL,
                             &body, &blen, &err)) {
        PCV_LOG_WARN(WP_LOG_DOM, "푸시 봉인 실패(%.80s): %s", sub->endpoint,
                     err != NULL ? err->message : "unknown");
        g_clear_error(&err);
        return 0;
    }

    gchar *aud = _origin_of(sub->endpoint);
                                                         
                                                    
                               
    gchar *jwt = (aud != NULL)
        ? pcv_webpush_vapid_jwt(aud, b->contact, b->privkey,
                                g_get_real_time() / G_USEC_PER_SEC, &err)
        : NULL;
    g_free(aud);
    if (jwt == NULL) {
        PCV_LOG_WARN(WP_LOG_DOM, "VAPID JWT 생성 실패(%.80s): %s", sub->endpoint,
                     err != NULL ? err->message : "unknown");
        g_clear_error(&err);
        g_free(body);
        return 0;
    }

    gchar *authz  = g_strdup_printf("vapid t=%s, k=%s", jwt, b->pub);
    guint  status = b->post(sub->endpoint, body, blen, authz, b->urgency,
                            WP_TTL_SEC);
    g_free(authz);
    g_free(jwt);
    g_free(body);
    return status;
}

  
                                 
                                                                                  
  
                                                                
                                                 
                                                                      
                                                      
                                                         
   
static void
_apply_result_locked(const WpSub *sub)
{
    if (G.db == NULL)
        return;

    sqlite3_stmt *st = NULL;

    if (sub->status == 404 || sub->status == 410) {
        if (sqlite3_prepare_v2(G.db,
                               "DELETE FROM subscriptions WHERE id=? AND endpoint=?",
                               -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, sub->id);
            sqlite3_bind_text(st, 2, sub->endpoint, -1, SQLITE_TRANSIENT);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
                                                                
        gchar *summary = _ep_summary(sub->endpoint);
        PCV_LOG_INFO(WP_LOG_DOM, "죽은 구독 폐기(HTTP %u): %s", sub->status, summary);
        pcv_audit_log(NULL, "push.prune", summary, "ok", 0, 0, NULL);
        g_free(summary);
        return;
    }

    if (sub->status >= 200 && sub->status < 300) {
        if (sqlite3_prepare_v2(G.db,
                               "UPDATE subscriptions SET last_ok_at=?, fail_count=0"
                               " WHERE id=? AND endpoint=?", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, g_get_real_time() / G_USEC_PER_SEC);
            sqlite3_bind_int64(st, 2, sub->id);
            sqlite3_bind_text(st, 3, sub->endpoint, -1, SQLITE_TRANSIENT);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
        return;
    }

    if (sqlite3_prepare_v2(G.db,
                           "UPDATE subscriptions SET fail_count=fail_count+1"
                           " WHERE id=? AND endpoint=?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, sub->id);
        sqlite3_bind_text(st, 2, sub->endpoint, -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    PCV_LOG_WARN(WP_LOG_DOM, "푸시 발송 실패(HTTP %u): %.80s", sub->status,
                 sub->endpoint);
}

  
                                                 
                                                             
   
static void
_batch_worker(GTask *task, gpointer src, gpointer data, GCancellable *cancel)
{
    (void)task; (void)src; (void)cancel;                                  
    WpBatch *b    = data;
    guint    done = 0;                                    

                                                          
    for (guint i = 0; i < b->subs->len; i++) {
                                                         
                                                                
        if (g_cancellable_is_cancelled(b->cancel))
            break;
        WpSub *s  = g_ptr_array_index(b->subs, i);
        s->status = _send_one(b, s);
                                                               
                                       
        if (s->status == 0 && g_cancellable_is_cancelled(b->cancel))
            break;
        done = i + 1;
    }

    g_mutex_lock(&G.mu);
    for (guint i = 0; i < done; i++)
        _apply_result_locked(g_ptr_array_index(b->subs, i));
                                                           
    if (G.inflight > 0)
        G.inflight--;
    g_cond_broadcast(&G.cond);
    g_mutex_unlock(&G.mu);
}

  
                                                            
                                           
   
static void
_dispatch_locked(GPtrArray *subs, gchar *payload, gsize payload_len,
                 gboolean is_crit)
{
    if (subs->len == 0) {
        g_ptr_array_unref(subs);
        g_free(payload);
        return;
    }

    WpBatch *b     = g_new0(WpBatch, 1);
    b->subs        = subs;
    b->payload     = payload;
    b->payload_len = payload_len;
    b->urgency     = g_strdup(is_crit ? "high" : "normal");
    b->privkey     = g_strdup(G.vapid_privkey);
    b->pub         = g_strdup(G.vapid_pub);
    b->contact     = g_strdup(G.contact);                              
    b->cancel      = (G.cancel != NULL) ? g_object_ref(G.cancel)
                                        : g_cancellable_new();
    b->post        = (G.post != NULL) ? G.post : _post_real;

                                                             
                            
    G.inflight++;

    GTask *task = g_task_new(NULL, b->cancel, NULL, NULL);
    g_task_set_task_data(task, b, _batch_free);
    g_task_run_in_thread(task, _batch_worker);
    g_object_unref(task);
}

  
                                                         
  
                                                   
                                                         
                                        
   
static gchar *
_build_payload(const gchar *message, gboolean is_crit, gsize *out_len)
{
    gsize cut = WP_BODY_MAX;

    for (;;) {
        gchar       *body    = _truncate_utf8(message, cut);
        JsonBuilder *builder = json_builder_new();

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "title");
        json_builder_add_string_value(builder, "PureCVisor 알림");
        json_builder_set_member_name(builder, "body");
        json_builder_add_string_value(builder, body);
        json_builder_set_member_name(builder, "severity");
        json_builder_add_string_value(builder, is_crit ? "crit" : "warn");
        json_builder_set_member_name(builder, "ts");
        json_builder_add_int_value(builder, g_get_real_time() / G_USEC_PER_SEC);
        json_builder_set_member_name(builder, "url");
        json_builder_add_string_value(builder, "#mon-alerts");
        json_builder_end_object(builder);

        JsonNode      *root = json_builder_get_root(builder);
        JsonGenerator *gen  = json_generator_new();
        json_generator_set_root(gen, root);
        gsize  len  = 0;
        gchar *json = json_generator_to_data(gen, &len);

        g_object_unref(gen);
        json_node_unref(root);
        g_object_unref(builder);
        g_free(body);

        if (len <= WP_PT_MAX || cut == 0) {
            *out_len = len;
            return json;
        }
        g_free(json);
        cut /= 2;                                    
    }
}

                                                               

gboolean
pcv_webpush_init(const gchar *db_path, const gchar *vapid_pem_path, GError **error)
{
    if (db_path == NULL || db_path[0] == '\0'
        || vapid_pem_path == NULL || vapid_pem_path[0] == '\0') {
        g_set_error(error, _wp_quark(), WP_ERR_ARG, "db/pem 경로가 비어 있습니다");
        return FALSE;
    }

    g_mutex_lock(&G.mu);
    if (G.db != NULL) {
        g_mutex_unlock(&G.mu);
        g_set_error(error, _wp_quark(), WP_ERR_ARG, "webpush 가 이미 초기화됐습니다");
        return FALSE;
    }

    sqlite3 *db = NULL;
    int      rc = sqlite3_open_v2(db_path, &db,
                                  SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE
                                      | SQLITE_OPEN_FULLMUTEX, NULL);
    if (rc != SQLITE_OK) {
        g_set_error(error, _wp_quark(), WP_ERR_DB, "구독 DB 열기 실패(%s): %s",
                    db_path, db != NULL ? sqlite3_errmsg(db) : "unknown");
        if (db != NULL) sqlite3_close(db);
        g_mutex_unlock(&G.mu);
        return FALSE;
    }

    G.db = db;
                                                            
                                                                  
                                               
    if (!_exec_locked("PRAGMA journal_mode=WAL", error)
        || !_exec_locked(WP_DDL_SUBSCRIPTIONS, error)) {
        sqlite3_close(G.db);
        G.db = NULL;
        g_mutex_unlock(&G.mu);
        return FALSE;
    }

                                                              
                                            
    g_clear_object(&G.cancel);
    G.cancel    = g_cancellable_new();
    G.pem_path  = g_strdup(vapid_pem_path);
    G.enabled   = TRUE;                                          
    G.crit_only = FALSE;
    g_free(G.contact);
    G.contact   = NULL;                                         
    G.closing   = FALSE;

                                             
                                                 
    if (g_file_test(vapid_pem_path, G_FILE_TEST_EXISTS)) {
        GError *lerr = NULL;
        if (!_pem_load(vapid_pem_path, &G.vapid_privkey, &G.vapid_pub, &lerr)) {
            PCV_LOG_WARN(WP_LOG_DOM,
                         "VAPID PEM 로드 실패 — 최초 사용 시 재생성(전 구독 무효): %s",
                         lerr != NULL ? lerr->message : "unknown");
            g_clear_error(&lerr);
        }
    }
    g_mutex_unlock(&G.mu);

    PCV_LOG_INFO(WP_LOG_DOM, "Web Push 초기화 완료 (db=%s, pem=%s)",
                 db_path, vapid_pem_path);
    return TRUE;
}

  
                                               
  
                                                   
                     
  
                                                          
                                                            
                                           
                                                    
                                                                 
                                                    
                                                    
   
gboolean
pcv_webpush_wait_idle(void)
{
                                                  
    gint64   deadline = g_get_monotonic_time()
                        + (gint64)WP_IDLE_WAIT_SEC * G_TIME_SPAN_SECOND;
    gboolean idle     = TRUE;

    g_mutex_lock(&G.mu);
    while (G.inflight > 0) {
                                                                
        if (!g_cond_wait_until(&G.cond, &G.mu, deadline)) {
            idle = FALSE;
            break;
        }
    }
    g_mutex_unlock(&G.mu);
    return idle;
}

  
                                               
  
                                                 
                                                  
  
          
                                                          
                                         
                                                            
                                                                   
                                                 
                                                         
                                                      
                                                   
                                                           
                              
  
                                              
                                                          
                                                
                                                        
                                                     
                              
   
void
pcv_webpush_shutdown(void)
{
    g_mutex_lock(&G.mu);
    if (G.db == NULL) {
        g_mutex_unlock(&G.mu);
        return;
    }
    G.closing = TRUE;                                
    if (G.cancel != NULL)
        g_cancellable_cancel(G.cancel);                           
    g_mutex_unlock(&G.mu);

                                                   
                                                
    gboolean drained = pcv_webpush_wait_idle();

    g_mutex_lock(&G.mu);
    if (drained) {
        sqlite3_close(G.db);
    } else {
                                                      
                                                              
                                             
        PCV_LOG_WARN(WP_LOG_DOM,
                     "발송 워커가 %d초 내 종료되지 않음 — DB 핸들을 닫지 않고 분리",
                     WP_IDLE_WAIT_SEC);
    }
    G.db = NULL;
    g_free(G.pem_path);
    G.pem_path = NULL;
    pcv_secure_free_str(&G.vapid_privkey);
    g_free(G.vapid_pub);
    G.vapid_pub = NULL;
    g_free(G.contact);
    G.contact   = NULL;
    G.closing   = FALSE;
                                                          
                                                             
    g_clear_object(&G.cancel);
                                                           
                                                             
    g_mutex_unlock(&G.mu);

    PCV_LOG_INFO(WP_LOG_DOM, "Web Push 종료");
}

  
                                               
  
                                                              
                                                      
                    
  
                                                        
                                                        
                                       
                                               
                                  
                                                               
                                       
                                                                        
   
void
pcv_webpush_set_policy(gboolean enabled, gboolean crit_only, const gchar *contact)
{
    g_mutex_lock(&G.mu);
    G.enabled   = enabled;
    G.crit_only = crit_only;
    g_free(G.contact);
                                                   
                                                 
    G.contact   = (contact != NULL && contact[0] != '\0') ? g_strdup(contact) : NULL;
    g_mutex_unlock(&G.mu);
}

                                                                
                                                           
                   
void
pcv_webpush_set_post_hook(PcvWebpushPostFn fn)
{
    g_mutex_lock(&G.mu);
    G.post = fn;                                                 
    g_mutex_unlock(&G.mu);
}

  
                                               
  
                                                  
                                     
  
                         
                       
                                                                 
                                        
                                                               
                                       
                                                                  
                                                     
                                              
                                                            
  
                                                   
                                   
                                                                   
                                          
                                                
                                                                  
                                        
                                                       
                                           
                                                 
                                  
                           
                                                   
                                   
   
gboolean
pcv_webpush_subscribe(const gchar *username, const gchar *endpoint,
                      const gchar *p256dh, const gchar *auth, GError **error)
{
    if (username == NULL || username[0] == '\0'
        || endpoint == NULL || endpoint[0] == '\0'
        || p256dh == NULL || p256dh[0] == '\0'
        || auth == NULL || auth[0] == '\0') {
        g_set_error(error, _wp_quark(), WP_ERR_ARG, "구독 필드가 비어 있습니다");
        return FALSE;
    }
    if (!pcv_webpush_endpoint_allowed(endpoint, error))
        return FALSE;

                                                 
                              
    guchar probe_pub[WP_PUB_LEN];
    guchar probe_auth[16];
    gboolean keys_ok = _b64url_dec_fixed(p256dh, probe_pub, WP_PUB_LEN)
                       && _b64url_dec_fixed(auth, probe_auth, sizeof probe_auth);
    pcv_secure_wipe(probe_auth, sizeof probe_auth);
    if (!keys_ok) {
        g_set_error(error, _wp_quark(), WP_ERR_ARG,
                    "구독 키 형식이 올바르지 않습니다(p256dh 65B / auth 16B base64url)");
        return FALSE;
    }

    g_mutex_lock(&G.mu);
    if (G.db == NULL) {
        g_mutex_unlock(&G.mu);
        g_set_error(error, _wp_quark(), WP_ERR_NOT_INIT, "webpush 미초기화");
        return FALSE;
    }

                           
      
                                                             
                                                              
                                                                     
                                
      
                                           
                                                   
                                                      
                                                                        
                                       
                                                               
    gboolean owns_existing = FALSE;
    {
        sqlite3_stmt *own = NULL;
        if (sqlite3_prepare_v2(G.db, "SELECT username FROM subscriptions"
                                     " WHERE endpoint=?", -1, &own, NULL) != SQLITE_OK) {
            g_set_error(error, _wp_quark(), WP_ERR_DB, "구독 소유 조회 실패: %s",
                        sqlite3_errmsg(G.db));
            g_mutex_unlock(&G.mu);
            return FALSE;
        }
        sqlite3_bind_text(own, 1, endpoint, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(own) == SQLITE_ROW) {
            const gchar *cur = (const gchar *)sqlite3_column_text(own, 0);
            owns_existing = (g_strcmp0(cur, username) == 0);
        }
        sqlite3_finalize(own);
    }

    if (!owns_existing) {
        sqlite3_stmt *cnt = NULL;
        if (sqlite3_prepare_v2(G.db, "SELECT COUNT(*) FROM subscriptions"
                                     " WHERE username=?", -1, &cnt, NULL) != SQLITE_OK) {
            g_set_error(error, _wp_quark(), WP_ERR_DB, "구독 수 조회 실패: %s",
                        sqlite3_errmsg(G.db));
            g_mutex_unlock(&G.mu);
            return FALSE;
        }
        sqlite3_bind_text(cnt, 1, username, -1, SQLITE_TRANSIENT);
        gint64 have = (sqlite3_step(cnt) == SQLITE_ROW)
                      ? sqlite3_column_int64(cnt, 0) : -1;
        sqlite3_finalize(cnt);

        if (have < 0) {                                       
            g_set_error(error, _wp_quark(), WP_ERR_DB, "구독 수를 확인하지 못했습니다");
            g_mutex_unlock(&G.mu);
            return FALSE;
        }
        if (have >= (gint64)PCV_WEBPUSH_MAX_SUBS_PER_USER) {
            g_mutex_unlock(&G.mu);
            PCV_LOG_WARN(WP_LOG_DOM,
                         "subscribe rejected: user has %" G_GINT64_FORMAT
                         " subscriptions (cap %u)", have,
                         PCV_WEBPUSH_MAX_SUBS_PER_USER);
            g_set_error(error, _wp_quark(), WP_ERR_LIMIT,
                        "구독 상한(%u개)에 도달했습니다 — 쓰지 않는 브라우저의 구독을 "
                        "해제한 뒤 다시 시도하세요",
                        PCV_WEBPUSH_MAX_SUBS_PER_USER);
            return FALSE;
        }
    }

                                               
    static const gchar *SQL =
        "INSERT INTO subscriptions(username,endpoint,p256dh,auth,created_at,"
        "last_ok_at,fail_count) VALUES(?,?,?,?,?,NULL,0) "
        "ON CONFLICT(endpoint) DO UPDATE SET username=excluded.username,"
        "p256dh=excluded.p256dh,auth=excluded.auth";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(G.db, SQL, -1, &st, NULL) != SQLITE_OK) {
        g_set_error(error, _wp_quark(), WP_ERR_DB, "구독 등록 준비 실패: %s",
                    sqlite3_errmsg(G.db));
        g_mutex_unlock(&G.mu);
        return FALSE;
    }
    sqlite3_bind_text(st, 1, username, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, endpoint, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, p256dh, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, auth, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 5, g_get_real_time() / G_USEC_PER_SEC);

    gboolean ok = (sqlite3_step(st) == SQLITE_DONE);
    if (!ok)
        g_set_error(error, _wp_quark(), WP_ERR_DB, "구독 등록 실패: %s",
                    sqlite3_errmsg(G.db));
    sqlite3_finalize(st);
    g_mutex_unlock(&G.mu);
    return ok;
}

  
                                               
  
                                                   
                                     
  
                                                          
                                                 
                                                    
  
                                                                  
                                                             
                                              
  
                                                   
                                                   
                                
                                                      
                                  
                                
   
gboolean
pcv_webpush_unsubscribe(const gchar *endpoint, const gchar *username,
                        GError **error)
{
    if (endpoint == NULL || endpoint[0] == '\0') {
        g_set_error(error, _wp_quark(), WP_ERR_ARG, "endpoint 가 비어 있습니다");
        return FALSE;
    }

    g_mutex_lock(&G.mu);
    if (G.db == NULL) {
        g_mutex_unlock(&G.mu);
        g_set_error(error, _wp_quark(), WP_ERR_NOT_INIT, "webpush 미초기화");
        return FALSE;
    }

                                                                 
    const gchar *sql = (username != NULL)
        ? "DELETE FROM subscriptions WHERE endpoint=? AND username=?"
        : "DELETE FROM subscriptions WHERE endpoint=?";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(G.db, sql, -1, &st, NULL) != SQLITE_OK) {
        g_set_error(error, _wp_quark(), WP_ERR_DB, "구독 삭제 준비 실패: %s",
                    sqlite3_errmsg(G.db));
        g_mutex_unlock(&G.mu);
        return FALSE;
    }
    sqlite3_bind_text(st, 1, endpoint, -1, SQLITE_TRANSIENT);
    if (username != NULL)
        sqlite3_bind_text(st, 2, username, -1, SQLITE_TRANSIENT);

    gboolean stepped = (sqlite3_step(st) == SQLITE_DONE);
    int      changed = sqlite3_changes(G.db);
    sqlite3_finalize(st);
    g_mutex_unlock(&G.mu);

    if (!stepped) {
        g_set_error(error, _wp_quark(), WP_ERR_DB, "구독 삭제 실패");
        return FALSE;
    }
    if (changed <= 0) {
        g_set_error(error, _wp_quark(), WP_ERR_NOT_FOUND,
                    "해당 구독이 없거나 소유자가 다릅니다");
        return FALSE;
    }
    return TRUE;
}

  
                                               
  
                                                
                             
  
                                                                      
                                                       
                                                        
                                
                                                        
  
                                                     
                            
                                                
                                 
                                                                  
   
JsonArray *
pcv_webpush_list(void)
{
    JsonArray *arr = json_array_new();

    g_mutex_lock(&G.mu);
    if (G.db == NULL) {
        g_mutex_unlock(&G.mu);
        return arr;                                    
    }

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(G.db,
                           "SELECT username,endpoint,created_at,last_ok_at,"
                           "fail_count FROM subscriptions ORDER BY id",
                           -1, &st, NULL) != SQLITE_OK) {
        g_mutex_unlock(&G.mu);
        return arr;
    }

    while (sqlite3_step(st) == SQLITE_ROW) {
        JsonObject *o  = json_object_new();
        gchar      *ep = _ep_summary((const gchar *)sqlite3_column_text(st, 1));

        json_object_set_string_member(o, "username",
                                      (const gchar *)sqlite3_column_text(st, 0));
        json_object_set_string_member(o, "endpoint", ep);
        json_object_set_int_member(o, "created_at", sqlite3_column_int64(st, 2));
                                                            
        json_object_set_int_member(o, "last_ok_at",
                                   sqlite3_column_type(st, 3) == SQLITE_NULL
                                       ? 0 : sqlite3_column_int64(st, 3));
        json_object_set_int_member(o, "fail_count", sqlite3_column_int64(st, 4));
        json_array_add_object_element(arr, o);
        g_free(ep);
    }
    sqlite3_finalize(st);
    g_mutex_unlock(&G.mu);
    return arr;
}

  
                                               
  
                                              
                                           
  
                                                        
                                                                  
                                                    
                    
  
                                                        
                                                         
                                                       
                                        
  
                        
                                                
                                       
                                                  
   
JsonArray *
pcv_webpush_list_mine(const gchar *username)
{
    JsonArray *arr = json_array_new();

                                                
                                                                        
                                                  
    if (username == NULL || username[0] == '\0')
        return arr;

    g_mutex_lock(&G.mu);
    if (G.db == NULL) {
        g_mutex_unlock(&G.mu);
        return arr;                                              
    }

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(G.db,
                           "SELECT endpoint,created_at,last_ok_at,fail_count"
                           " FROM subscriptions WHERE username=? ORDER BY id",
                           -1, &st, NULL) != SQLITE_OK) {
        g_mutex_unlock(&G.mu);
        return arr;
    }
    sqlite3_bind_text(st, 1, username, -1, SQLITE_TRANSIENT);

    while (sqlite3_step(st) == SQLITE_ROW) {
        const gchar *full = (const gchar *)sqlite3_column_text(st, 0);
        JsonObject  *o    = json_object_new();
        gchar       *ep   = _ep_summary(full);
                                                             
                                                        
        gchar *digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256,
                                                      full != NULL ? full : "", -1);

        json_object_set_string_member(o, "endpoint", ep);
        json_object_set_string_member(o, "digest", digest);
        json_object_set_int_member(o, "created_at", sqlite3_column_int64(st, 1));
        json_object_set_int_member(o, "last_ok_at",
                                   sqlite3_column_type(st, 2) == SQLITE_NULL
                                       ? 0 : sqlite3_column_int64(st, 2));
        json_object_set_int_member(o, "fail_count", sqlite3_column_int64(st, 3));
        json_array_add_object_element(arr, o);
        g_free(digest);
        g_free(ep);
    }
    sqlite3_finalize(st);
    g_mutex_unlock(&G.mu);
    return arr;
}

  
                                               
  
                                             
                                             
  
                                                      
                         
  
                                                             
                                                         
                              
                               
                                                       
   
gchar *
pcv_webpush_vapid_public(void)
{
    g_mutex_lock(&G.mu);
    if (G.db == NULL) {
        g_mutex_unlock(&G.mu);
        return NULL;
    }

    GError *err = NULL;
    if (!_ensure_vapid_locked(&err)) {
        PCV_LOG_WARN(WP_LOG_DOM, "VAPID 공개키 준비 실패: %s",
                     err != NULL ? err->message : "unknown");
        g_clear_error(&err);
        g_mutex_unlock(&G.mu);
        return NULL;
    }
    gchar *pub = g_strdup(G.vapid_pub);
    g_mutex_unlock(&G.mu);
    return pub;
}

  
                                                 
  
                                                 
                                               
  
                                                      
                                                  
                                               
  
                                                     
                                                         
                                               
  
                                                
                                                 
                                                    
                                                   
                                                     
                                                         
                                 
                               
                                     
   
guint
pcv_webpush_vapid_rotate(GError **error)
{
    g_mutex_lock(&G.mu);
    if (G.db == NULL) {
        g_mutex_unlock(&G.mu);
        g_set_error(error, _wp_quark(), WP_ERR_NOT_INIT, "webpush 미초기화");
        return 0;
    }

                                                        
    pcv_secure_free_str(&G.vapid_privkey);
    g_free(G.vapid_pub);
    G.vapid_pub = NULL;

    if (!_ensure_vapid_locked(error)) {
        g_mutex_unlock(&G.mu);
        return 0;
    }
                                                  
    guint revoked = _delete_all_locked();
    g_mutex_unlock(&G.mu);

    PCV_LOG_INFO(WP_LOG_DOM, "VAPID 키 회전 — 구독 %u건 폐기", revoked);
    return revoked;
}

  
                                               
  
                                              
  
                                                      
                                              
                               
  
                                                       
                                                      
  
                           
                                                   
                                                      
                                          
                    
   
guint
pcv_webpush_remove_user(const gchar *username)
{
    if (username == NULL || username[0] == '\0')
        return 0;

    g_mutex_lock(&G.mu);
    if (G.db == NULL) {
        g_mutex_unlock(&G.mu);
        return 0;
    }

    sqlite3_stmt *st = NULL;
    guint         n  = 0;
    if (sqlite3_prepare_v2(G.db, "DELETE FROM subscriptions WHERE username=?",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, username, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_DONE) {
            int changed = sqlite3_changes(G.db);
            n = changed > 0 ? (guint)changed : 0u;
        }
        sqlite3_finalize(st);
    }
    g_mutex_unlock(&G.mu);

    if (n > 0)
        PCV_LOG_INFO(WP_LOG_DOM, "사용자 삭제 cascade — 구독 %u건 정리", n);
    return n;
}

  
                                                
  
                                                     
                                                  
  
                      
                                                                     
                                             
                                   
                                                      
                                                      
                                                                    
                                                         
                                                           
  
                                                       
                                            
                                                      
                                                   
                                               
                                                           
                                                
                                                       
                                        
   
void
pcv_webpush_notify(const gchar *source, gboolean is_crit, const gchar *message)
{
    g_mutex_lock(&G.mu);
                                                                 
    if (G.db == NULL || G.closing) {
        g_mutex_unlock(&G.mu);
        return;
    }
    if (!G.enabled || (G.crit_only && !is_crit)) {
        g_mutex_unlock(&G.mu);
        return;
    }

                                                              
                                              
                                                               
                                                     
                                                    
                    
                                                              
                                                 
    GPtrArray *subs = _snapshot_locked(NULL);
    if (subs->len == 0) {
        g_ptr_array_unref(subs);
        g_mutex_unlock(&G.mu);
        return;
    }

    GError *err = NULL;
    if (!_ensure_vapid_locked(&err)) {
        PCV_LOG_WARN(WP_LOG_DOM, "VAPID 키 준비 실패 — 푸시 생략: %s",
                     err != NULL ? err->message : "unknown");
        g_clear_error(&err);
        g_ptr_array_unref(subs);                                 
        g_mutex_unlock(&G.mu);
        return;
    }

    gsize  plen    = 0;
    gchar *payload = _build_payload(message, is_crit, &plen);
    PCV_LOG_INFO(WP_LOG_DOM, "푸시 발송 큐잉 (source=%s, crit=%d, 구독 %u건)",
                 source != NULL ? source : "-", is_crit ? 1 : 0, subs->len);
    _dispatch_locked(subs, payload, plen, is_crit);
    g_mutex_unlock(&G.mu);
}

  
                                                 
  
                                                
  
                                      
                                                      
                                          
                                                             
                                                    
                    
  
                                                      
                                                  
                                                              
                                                 
                                                      
                        
   
gboolean
pcv_webpush_send_test(const gchar *username, GError **error)
{
    g_mutex_lock(&G.mu);
    if (G.db == NULL || G.closing) {
        g_mutex_unlock(&G.mu);
        g_set_error(error, _wp_quark(), WP_ERR_NOT_INIT, "webpush 미초기화");
        return FALSE;
    }
    if (!_ensure_vapid_locked(error)) {
        g_mutex_unlock(&G.mu);
        return FALSE;
    }

                                                      
    GPtrArray *subs = _snapshot_locked(username);
    if (subs->len == 0) {
        g_ptr_array_unref(subs);
        g_mutex_unlock(&G.mu);
        g_set_error(error, _wp_quark(), WP_ERR_NOT_FOUND, "대상 구독이 없습니다");
        return FALSE;
    }

    gsize  plen    = 0;
    gchar *payload = _build_payload("PureCVisor 테스트 알림입니다.", FALSE, &plen);
    _dispatch_locked(subs, payload, plen, FALSE);
    g_mutex_unlock(&G.mu);
    return TRUE;
}
