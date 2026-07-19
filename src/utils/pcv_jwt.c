
#include "pcv_jwt.h"
#include "pcv_log.h"

#include <glib.h>
#include <json-glib/json-glib.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <string.h>
#include <time.h>

static gchar  *g_jwt_secret  = NULL;

static GHashTable *g_jti_blacklist = NULL;
static GMutex      g_jti_blacklist_mu;

static gsize   g_jwt_secret_len = 0;

static GMutex  g_jwt_mutex;

#define JWT_LOG_DOM "pcv_jwt"

#define JWT_DEFAULT_EXPIRY 3600

static void
_fill_random_bytes(guchar *buf, gsize len)
{
    g_return_if_fail(buf != NULL);

    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        gboolean ok = fread(buf, 1, len, f) == len;
        fclose(f);
        if (ok) return;
    }

    if (RAND_bytes(buf, (int)len) == 1)
        return;

    g_error("pcv_jwt: secure RNG unavailable "
            "(/dev/urandom and OpenSSL RAND_bytes both failed) — refusing to "
            "generate predictable JWT secret/jti");
}

static gchar *
_b64url_encode(const guchar *data, gsize len)
{

    gchar *b64 = g_base64_encode(data, len);

    for (gchar *p = b64; *p; p++) {
        if (*p == '+') *p = '-';
        else if (*p == '/') *p = '_';
    }

    gsize blen = strlen(b64);
    while (blen > 0 && b64[blen - 1] == '=')
        b64[--blen] = '\0';
    return b64;
}

static guchar *
_b64url_decode(const gchar *str, gsize *out_len)
{

    gsize slen = strlen(str);
    gsize pad  = (4 - (slen % 4)) % 4;
    GString *padded = g_string_new(str);

    for (gchar *p = padded->str; *p; p++) {
        if (*p == '-') *p = '+';
        else if (*p == '_') *p = '/';
    }

    for (gsize i = 0; i < pad; i++)
        g_string_append_c(padded, '=');

    guchar *decoded = g_base64_decode(padded->str, out_len);
    g_string_free(padded, TRUE);
    return decoded;
}

static gboolean
_hmac_sha256(const gchar  *key,      gsize key_len,
             const gchar  *msg,      gsize msg_len,
             guchar       *out_sig,  guint *out_sig_len)
{
    unsigned int len = 0;
    guchar *result = HMAC(EVP_sha256(),
                          key, (int)key_len,
                          (const unsigned char *)msg, msg_len,
                          out_sig, &len);
    if (!result) return FALSE;
    *out_sig_len = len;
    return TRUE;
}

void
pcv_jwt_init(const gchar *secret)
{
    g_mutex_init(&g_jwt_mutex);
    g_mutex_init(&g_jti_blacklist_mu);

    if (secret && *secret) {

        gsize secret_len = strlen(secret);
        if (secret_len < 32) {
            PCV_LOG_WARN(JWT_LOG_DOM,
                         "JWT secret too short (%zu bytes < 32) — falling back to random key. "
                         "Set [auth] jwt_secret to >= 32 bytes in daemon.conf for stable tokens.",
                         secret_len);
            secret = NULL;
        } else {
            g_jwt_secret     = g_strdup(secret);
            g_jwt_secret_len = secret_len;
        }
    }
    if (!secret || !*secret) {

        guchar rnd[32];
        _fill_random_bytes(rnd, sizeof(rnd));

        g_jwt_secret     = (gchar *)g_memdup2(rnd, sizeof(rnd));
        g_jwt_secret_len = sizeof(rnd);
        PCV_LOG_WARN(JWT_LOG_DOM,
                     "JWT secret not configured — using random key "
                     "(tokens invalidated on restart)");
    }
}

void
pcv_jwt_blacklist_add(const gchar *jti, gint64 expiry_unix)
{
    if (!jti || !*jti) return;
    g_mutex_lock(&g_jti_blacklist_mu);
    if (!g_jti_blacklist) {
        g_jti_blacklist = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                  g_free, NULL);
    }

    g_hash_table_replace(g_jti_blacklist, g_strdup(jti),
                         GINT_TO_POINTER((gint)expiry_unix));
    g_mutex_unlock(&g_jti_blacklist_mu);
    PCV_LOG_INFO(JWT_LOG_DOM, "Token revoked: jti=%s", jti);
}

gboolean
pcv_jwt_blacklist_check(const gchar *jti)
{
    if (!jti || !*jti) return FALSE;
    gboolean revoked = FALSE;
    g_mutex_lock(&g_jti_blacklist_mu);
    if (g_jti_blacklist) {
        gpointer val = g_hash_table_lookup(g_jti_blacklist, jti);
        if (val) {
            gint expiry = GPOINTER_TO_INT(val);
            if ((gint)time(NULL) < expiry) {
                revoked = TRUE;
            } else {

                g_hash_table_remove(g_jti_blacklist, jti);
            }
        }
    }
    g_mutex_unlock(&g_jti_blacklist_mu);
    return revoked;
}

void
pcv_jwt_blacklist_sweep(void)
{
    g_mutex_lock(&g_jti_blacklist_mu);
    if (g_jti_blacklist) {
        GHashTableIter it;
        gpointer key, val;
        gint now = (gint)time(NULL);
        GPtrArray *to_remove = g_ptr_array_new();
        g_hash_table_iter_init(&it, g_jti_blacklist);
        while (g_hash_table_iter_next(&it, &key, &val)) {
            if (GPOINTER_TO_INT(val) <= now) {
                g_ptr_array_add(to_remove, key);
            }
        }
        for (guint i = 0; i < to_remove->len; i++) {
            g_hash_table_remove(g_jti_blacklist, g_ptr_array_index(to_remove, i));
        }
        g_ptr_array_free(to_remove, TRUE);
    }
    g_mutex_unlock(&g_jti_blacklist_mu);
}

void
pcv_jwt_shutdown(void)
{
    g_mutex_lock(&g_jwt_mutex);
    g_free(g_jwt_secret);
    g_jwt_secret     = NULL;
    g_jwt_secret_len = 0;
    g_mutex_unlock(&g_jwt_mutex);
    g_mutex_clear(&g_jwt_mutex);

    g_mutex_lock(&g_jti_blacklist_mu);
    if (g_jti_blacklist) {
        g_hash_table_destroy(g_jti_blacklist);
        g_jti_blacklist = NULL;
    }
    g_mutex_unlock(&g_jti_blacklist_mu);
    g_mutex_clear(&g_jti_blacklist_mu);
}

void
pcv_jwt_update_secret(const gchar *new_secret)
{
    if (!new_secret || !*new_secret) return;

    g_mutex_lock(&g_jwt_mutex);
    g_free(g_jwt_secret);
    g_jwt_secret     = g_strdup(new_secret);
    g_jwt_secret_len = strlen(new_secret);
    g_mutex_unlock(&g_jwt_mutex);
}

gchar *
pcv_jwt_sign(const gchar *subject,
             guint        expires_in,
             GError     **error)
{

    g_return_val_if_fail(subject && *subject, NULL);

    if (expires_in == 0)
        expires_in = JWT_DEFAULT_EXPIRY;

    static const gchar header_json[] = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";
    gchar *header_b64 = _b64url_encode((const guchar *)header_json,
                                        strlen(header_json));

    gint64 now = (gint64)time(NULL);
    JsonBuilder *jb = json_builder_new();
    json_builder_begin_object(jb);
    json_builder_set_member_name(jb, "sub");
    json_builder_add_string_value(jb, subject);
    json_builder_set_member_name(jb, "iat");
    json_builder_add_int_value(jb, now);
    json_builder_set_member_name(jb, "exp");
    json_builder_add_int_value(jb, now + (gint64)expires_in);

    {
        guchar jti_bytes[16];
        _fill_random_bytes(jti_bytes, sizeof(jti_bytes));
        gchar jti_hex[33];
        for (int i = 0; i < 16; i++) snprintf(jti_hex + i*2, 3, "%02x", jti_bytes[i]);
        jti_hex[32] = '\0';
        json_builder_set_member_name(jb, "jti");
        json_builder_add_string_value(jb, jti_hex);
    }

    json_builder_end_object(jb);

    JsonNode *jwt_root = json_builder_get_root(jb);
    gchar *payload_json = json_to_string(jwt_root, FALSE);
    json_node_free(jwt_root);
    g_object_unref(jb);

    gchar *payload_b64 = _b64url_encode((const guchar *)payload_json,
                                         strlen(payload_json));
    g_free(payload_json);

    gchar *signing_input = g_strdup_printf("%s.%s", header_b64, payload_b64);

    guchar sig[EVP_MAX_MD_SIZE];
    guint  sig_len = 0;

    g_mutex_lock(&g_jwt_mutex);
    gboolean ok = _hmac_sha256(g_jwt_secret, g_jwt_secret_len,
                                signing_input, strlen(signing_input),
                                sig, &sig_len);
    g_mutex_unlock(&g_jwt_mutex);

    if (!ok) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "HMAC-SHA256 computation failed");
        g_free(header_b64); g_free(payload_b64); g_free(signing_input);
        return NULL;
    }

    gchar *sig_b64 = _b64url_encode(sig, sig_len);
    gchar *token   = g_strdup_printf("%s.%s", signing_input, sig_b64);

    g_free(header_b64);
    g_free(payload_b64);
    g_free(signing_input);
    g_free(sig_b64);

    return token;
}

gchar *
pcv_jwt_sign_with_ip(const gchar *subject,
                     guint        expires_in,
                     const gchar *client_ip,
                     GError     **error)
{
    g_return_val_if_fail(subject && *subject, NULL);

    if (expires_in == 0)
        expires_in = JWT_DEFAULT_EXPIRY;

    static const gchar header_json[] = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";
    gchar *header_b64 = _b64url_encode((const guchar *)header_json,
                                        strlen(header_json));

    gint64 now = (gint64)time(NULL);
    JsonBuilder *jb = json_builder_new();
    json_builder_begin_object(jb);
    json_builder_set_member_name(jb, "sub");
    json_builder_add_string_value(jb, subject);
    json_builder_set_member_name(jb, "iat");
    json_builder_add_int_value(jb, now);
    json_builder_set_member_name(jb, "exp");
    json_builder_add_int_value(jb, now + (gint64)expires_in);

    if (client_ip && *client_ip) {
        json_builder_set_member_name(jb, "ip");
        json_builder_add_string_value(jb, client_ip);
    }

    json_builder_end_object(jb);

    JsonNode *jwt_root2 = json_builder_get_root(jb);
    gchar *payload_json = json_to_string(jwt_root2, FALSE);
    json_node_free(jwt_root2);
    g_object_unref(jb);

    gchar *payload_b64 = _b64url_encode((const guchar *)payload_json,
                                         strlen(payload_json));
    g_free(payload_json);

    gchar *signing_input = g_strdup_printf("%s.%s", header_b64, payload_b64);

    guchar sig[EVP_MAX_MD_SIZE];
    guint  sig_len = 0;

    g_mutex_lock(&g_jwt_mutex);
    gboolean ok = _hmac_sha256(g_jwt_secret, g_jwt_secret_len,
                                signing_input, strlen(signing_input),
                                sig, &sig_len);
    g_mutex_unlock(&g_jwt_mutex);

    if (!ok) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "HMAC-SHA256 computation failed");
        g_free(header_b64); g_free(payload_b64); g_free(signing_input);
        return NULL;
    }

    gchar *sig_b64 = _b64url_encode(sig, sig_len);
    gchar *token   = g_strdup_printf("%s.%s", signing_input, sig_b64);

    g_free(header_b64);
    g_free(payload_b64);
    g_free(signing_input);
    g_free(sig_b64);

    return token;
}

gchar *
pcv_jwt_verify(const gchar *token_or_bearer,
               GError     **error)
{
    g_return_val_if_fail(token_or_bearer, NULL);

    const gchar *token = token_or_bearer;
    if (g_str_has_prefix(token, "Bearer "))
        token += 7;
    if (g_str_has_prefix(token, "bearer "))
        token += 7;

    gchar **parts = g_strsplit(token, ".", 3);
    if (!parts || !parts[0] || !parts[1] || !parts[2]) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Invalid JWT format");
        g_strfreev(parts);
        return NULL;
    }

    gchar *signing_input = g_strdup_printf("%s.%s", parts[0], parts[1]);

    guchar expected_sig[EVP_MAX_MD_SIZE];
    guint  expected_len = 0;

    g_mutex_lock(&g_jwt_mutex);
    gboolean ok = _hmac_sha256(g_jwt_secret, g_jwt_secret_len,
                                signing_input, strlen(signing_input),
                                expected_sig, &expected_len);
    g_mutex_unlock(&g_jwt_mutex);

    g_free(signing_input);

    if (!ok) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "HMAC computation failed");
        g_strfreev(parts);
        return NULL;
    }

    gsize   recv_len = 0;
    guchar *recv_sig = _b64url_decode(parts[2], &recv_len);

    if (!recv_sig) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "JWT signature decode failed");
        g_strfreev(parts);
        return NULL;
    }

    gboolean sig_ok = (recv_len == expected_len) &&
                      (CRYPTO_memcmp(recv_sig, expected_sig, expected_len) == 0);
    g_free(recv_sig);

    if (!sig_ok) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                    "JWT signature mismatch");
        g_strfreev(parts);
        return NULL;
    }

    gsize payload_len = 0;
    guchar *payload_raw = _b64url_decode(parts[1], &payload_len);
    g_strfreev(parts);

    if (!payload_raw) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "JWT payload decode failed");
        return NULL;
    }

    gchar *payload_str = g_strndup((const gchar *)payload_raw, payload_len);
    g_free(payload_raw);

    JsonParser *parser = json_parser_new();
    GError *parse_err  = NULL;
    if (!json_parser_load_from_data(parser, payload_str, -1, &parse_err)) {
        g_free(payload_str);
        g_object_unref(parser);
        g_propagate_error(error, parse_err);
        return NULL;
    }
    g_free(payload_str);

    JsonObject *obj = json_node_get_object(json_parser_get_root(parser));
    gint64 exp = json_object_get_int_member_with_default(obj, "exp", 0);
    const gchar *sub = json_object_get_string_member_with_default(obj, "sub", NULL);

    const gchar *jti = json_object_get_string_member_with_default(obj, "jti", NULL);
    gchar *jti_copy = jti ? g_strdup(jti) : NULL;

    gchar *subject = sub ? g_strdup(sub) : NULL;
    g_object_unref(parser);

    if (exp == 0 || (gint64)time(NULL) > exp) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                    "JWT token expired");
        g_free(subject); g_free(jti_copy);
        return NULL;
    }

    if (!subject) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "JWT missing 'sub' claim");
        g_free(jti_copy);
        return NULL;
    }

    if (jti_copy && pcv_jwt_blacklist_check(jti_copy)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                    "JWT token revoked (logout)");
        g_free(subject); g_free(jti_copy);
        return NULL;
    }
    g_free(jti_copy);

    return subject;
}

gchar *
pcv_jwt_verify_with_ip(const gchar *token_or_bearer,
                       const gchar *client_ip,
                       GError     **error)
{

    gchar *subject = pcv_jwt_verify(token_or_bearer, error);
    if (!subject) return NULL;

    if (client_ip && *client_ip) {

        const gchar *tok = token_or_bearer;
        if (g_str_has_prefix(tok, "Bearer ") || g_str_has_prefix(tok, "bearer "))
            tok += 7;

        gchar **parts = g_strsplit(tok, ".", 4);
        if (parts && parts[0] && parts[1]) {
            gsize dec_len = 0;
            guchar *dec = g_base64_decode(parts[1], &dec_len);
            if (dec) {
                JsonParser *jp = json_parser_new();
                if (json_parser_load_from_data(jp, (const gchar *)dec, (gssize)dec_len, NULL)) {
                    JsonObject *payload = json_node_get_object(json_parser_get_root(jp));
                    if (payload && json_object_has_member(payload, "ip")) {
                        const gchar *bound_ip = json_object_get_string_member(payload, "ip");
                        if (bound_ip && g_strcmp0(bound_ip, client_ip) != 0) {
                            PCV_LOG_WARN(JWT_LOG_DOM,
                                         "JWT IP mismatch: bound=%s actual=%s user=%s",
                                         bound_ip, client_ip, subject);
                            g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                        "JWT IP binding mismatch");
                            g_free(subject);
                            subject = NULL;
                        }
                    }
                }
                g_object_unref(jp);
                g_free(dec);
            }
        }
        g_strfreev(parts);
    }

    return subject;
}
