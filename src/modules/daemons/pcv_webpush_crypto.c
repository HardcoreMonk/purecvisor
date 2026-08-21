                                           
                                                                           
  
                           
                                                   
                                                    
                                        
  
                                             
                                                     
                                              
                                              
                                                
                                                 
                                                 
  
                                              
                                                 
                                                
                                                   
                                                      
                                                    
                                         
                                           
                                                  
                                     
                                    
  
                                                                   
                                                   
   
#include "modules/daemons/pcv_webpush_crypto.h"
#include "utils/pcv_secure.h"                                   

#include <string.h>
#include <json-glib/json-glib.h>

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/obj_mac.h>
#include <openssl/param_build.h>
#include <openssl/rand.h>

                                                            

#define WP_PUB_LEN     65u                                                   
#define WP_PRIV_LEN    32u                     
#define WP_AUTH_LEN    16u                                       
#define WP_SALT_LEN    16u                                
#define WP_PRK_LEN     32u                        
#define WP_CEK_LEN     16u                  
#define WP_NONCE_LEN   12u                  
#define WP_TAG_LEN     16u                  
#define WP_ECDH_LEN    32u                          

                                                                     
#define WP_HDR_LEN     (WP_SALT_LEN + 4u + 1u + WP_PUB_LEN)           

                                                    
#define WP_RECORD_SIZE 4096u
#define WP_MAX_PT_LEN  (WP_RECORD_SIZE - WP_TAG_LEN - 1u)               

                                                       
#define WP_VAPID_TTL_SEC 43200

                                                         
#define WP_JWT_HEADER "{\"typ\":\"JWT\",\"alg\":\"ES256\"}"

                                                          
                                                              
static const gchar WP_KEY_INFO_PREFIX[] = "WebPush: info";
static const gchar WP_CEK_INFO[]        = "Content-Encoding: aes128gcm";
static const gchar WP_NONCE_INFO[]      = "Content-Encoding: nonce";

                                                                 
enum {
    WP_ERR_ARG    = 1,                    
    WP_ERR_DECODE = 2,                                   
    WP_ERR_CRYPTO = 3,                      
};

                       
static GQuark
_wp_quark(void)
{
    return g_quark_from_static_string("webpush_crypto");
}

                                                               

  
                                        
                                                  
                                               
   
static gchar *
_b64url_enc(const guchar *data, gsize len)
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
_b64url_dec(const gchar *str, gsize *out_len)
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

    guchar *out = g_base64_decode(padded->str, out_len);
    g_string_free(padded, TRUE);
    return out;
}

  
                                                 
                                               
                                               
                                               
   
static gboolean
_b64url_dec_fixed(const gchar *str, guchar *out, gsize want,
                  const gchar *what, GError **error)
{
    if (str == NULL || *str == '\0') {
        g_set_error(error, _wp_quark(), WP_ERR_ARG, "%s 가 비어 있습니다", what);
        return FALSE;
    }
    gsize   n   = 0;
    guchar *raw = _b64url_dec(str, &n);
    gboolean ok = (raw != NULL && n == want);

    if (ok)
        memcpy(out, raw, want);
    else
        g_set_error(error, _wp_quark(), WP_ERR_DECODE,
                    "%s base64url 길이 불일치: %" G_GSIZE_FORMAT
                    " 바이트 (기대 %" G_GSIZE_FORMAT ")", what, n, want);

    if (raw != NULL) {
        pcv_secure_wipe(raw, n);                               
        g_free(raw);
    }
    return ok;
}

                                                                 

  
                                                       
                                                                 
                                                       
                                                            
   
static gboolean
_hkdf_extract(const guchar *salt, gsize salt_len,
              const guchar *ikm, gsize ikm_len,
              guchar out_prk[WP_PRK_LEN])
{
    unsigned int  n = 0;
    guchar        buf[EVP_MAX_MD_SIZE];
    guchar       *r = HMAC(EVP_sha256(), salt, (int)salt_len, ikm, ikm_len, buf, &n);

    gboolean ok = (r != NULL && n == WP_PRK_LEN);
    if (ok)
        memcpy(out_prk, buf, WP_PRK_LEN);
    pcv_secure_wipe(buf, sizeof buf);
    return ok;
}

  
                                                                              
                                                        
                                          
   
static gboolean
_hkdf_expand(const guchar *prk, const guchar *info, gsize info_len,
             guchar *out, gsize out_len)
{
    if (out_len == 0 || out_len > WP_PRK_LEN)
        return FALSE;

    guchar *t_in = g_malloc(info_len + 1);                            
    memcpy(t_in, info, info_len);
    t_in[info_len] = 0x01;                                 

    unsigned int  n = 0;
    guchar        buf[EVP_MAX_MD_SIZE];
    guchar       *r = HMAC(EVP_sha256(), prk, (int)WP_PRK_LEN,
                           t_in, info_len + 1, buf, &n);
    g_free(t_in);

    gboolean ok = (r != NULL && n >= out_len);
    if (ok)
        memcpy(out, buf, out_len);
    pcv_secure_wipe(buf, sizeof buf);
    return ok;
}

                                                               

  
                                                              
                                             
                                                                                
   
static gboolean
_p256_pub_from_priv(const guchar priv[WP_PRIV_LEN], guchar out_pub[WP_PUB_LEN])
{
    gboolean  ok    = FALSE;
    EC_GROUP *grp   = EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1);
    BN_CTX   *bnctx = BN_CTX_new();
    BIGNUM   *d     = BN_bin2bn(priv, (int)WP_PRIV_LEN, NULL);
    EC_POINT *pt    = (grp != NULL) ? EC_POINT_new(grp) : NULL;

    if (grp != NULL && bnctx != NULL && d != NULL && pt != NULL
        && EC_POINT_mul(grp, pt, d, NULL, NULL, bnctx) == 1
        && EC_POINT_point2oct(grp, pt, POINT_CONVERSION_UNCOMPRESSED,
                              out_pub, WP_PUB_LEN, bnctx) == WP_PUB_LEN)
        ok = TRUE;

    if (pt != NULL)    EC_POINT_free(pt);
    if (d != NULL)     BN_clear_free(d);                            
    if (bnctx != NULL) BN_CTX_free(bnctx);
    if (grp != NULL)   EC_GROUP_free(grp);
    return ok;
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
    if (ok && pub65 != NULL)
        ok = (OSSL_PARAM_BLD_push_octet_string(bld, OSSL_PKEY_PARAM_PUB_KEY,
                                               pub65, WP_PUB_LEN) == 1);
    if (ok && priv32 != NULL) {
                                                                
        d  = BN_bin2bn(priv32, (int)WP_PRIV_LEN, NULL);
        ok = (d != NULL
              && OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_PRIV_KEY, d) == 1);
    }
    if (ok)
        ok = ((par = OSSL_PARAM_BLD_to_param(bld)) != NULL);
    if (ok)
        ok = ((ctx = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL)) != NULL
              && EVP_PKEY_fromdata_init(ctx) == 1);
    if (ok
        && EVP_PKEY_fromdata(ctx, &pkey,
                             (priv32 != NULL) ? EVP_PKEY_KEYPAIR
                                              : EVP_PKEY_PUBLIC_KEY, par) != 1)
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
_es256_sign_jose(EVP_PKEY *pkey, const gchar *msg, guchar out_jose[64])
{
    gboolean      ok     = FALSE;
    EVP_MD_CTX   *md     = EVP_MD_CTX_new();
    guchar       *der    = NULL;
    size_t        derlen = 0;
    ECDSA_SIG    *sig    = NULL;
    const guchar *p      = NULL;
    const BIGNUM *r      = NULL;
    const BIGNUM *s      = NULL;
    gsize         mlen   = strlen(msg);

    if (md == NULL)
        goto out;
    if (EVP_DigestSignInit(md, NULL, EVP_sha256(), NULL, pkey) != 1)
        goto out;
    if (EVP_DigestSign(md, NULL, &derlen, (const guchar *)msg, mlen) != 1)
        goto out;

    der = g_malloc(derlen);
    if (EVP_DigestSign(md, der, &derlen, (const guchar *)msg, mlen) != 1)
        goto out;

    p   = der;
    sig = d2i_ECDSA_SIG(NULL, &p, (long)derlen);
    if (sig == NULL)
        goto out;

    ECDSA_SIG_get0(sig, &r, &s);
    if (BN_bn2binpad(r, out_jose, 32) == 32
        && BN_bn2binpad(s, out_jose + 32, 32) == 32)
        ok = TRUE;

out:
    if (sig != NULL) ECDSA_SIG_free(sig);
    if (der != NULL) g_free(der);
    if (md != NULL)  EVP_MD_CTX_free(md);
    return ok;
}

                                                               

gboolean
pcv_webpush_keypair_generate(gchar **priv_b64url, gchar **pub_b64url, GError **error)
{
    if (priv_b64url == NULL || pub_b64url == NULL) {
        g_set_error(error, _wp_quark(), WP_ERR_ARG, "출력 인자가 NULL 입니다");
        return FALSE;
    }

    gboolean  ok   = FALSE;
    EVP_PKEY *pkey = EVP_PKEY_Q_keygen(NULL, NULL, "EC", "P-256");
    BIGNUM   *d    = NULL;
    guchar    priv_key[WP_PRIV_LEN];                            
    guchar    pub[WP_PUB_LEN];
    size_t    publen = 0;

    if (pkey == NULL) {
        g_set_error(error, _wp_quark(), WP_ERR_CRYPTO, "P-256 키 생성 실패");
        return FALSE;
    }

                                                           
    if (EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_PRIV_KEY, &d) == 1
        && BN_bn2binpad(d, priv_key, (int)WP_PRIV_LEN) == (int)WP_PRIV_LEN
        && EVP_PKEY_get_octet_string_param(pkey, OSSL_PKEY_PARAM_PUB_KEY,
                                           pub, sizeof pub, &publen) == 1
        && publen == WP_PUB_LEN) {
        *priv_b64url = _b64url_enc(priv_key, WP_PRIV_LEN);
        *pub_b64url  = _b64url_enc(pub, WP_PUB_LEN);
        ok = TRUE;
    } else {
        g_set_error(error, _wp_quark(), WP_ERR_CRYPTO, "P-256 키 자료 추출 실패");
    }

    pcv_secure_wipe(priv_key, sizeof priv_key);
    if (d != NULL) BN_clear_free(d);
    EVP_PKEY_free(pkey);
    return ok;
}

  
                                                      
  
                                                
                                                 
  
                                                 
                                                      
  
          
                                                      
                                                          
                                                    
                                                             
                              
                                                                    
                       
                                                        
                               
  
                                                       
                                                         
                                                   
                                     
                                     
                                                                   
                                                        
                                                     
                                                                              
                                           
                                          
                                                     
                                                                       
   
gchar *
pcv_webpush_vapid_jwt(const gchar *aud_origin, const gchar *sub_contact,
                      const gchar *vapid_priv_b64url, gint64 now_epoch,
                      GError **error)
{
    if (aud_origin == NULL || *aud_origin == '\0') {
        g_set_error(error, _wp_quark(), WP_ERR_ARG, "aud(푸시 서비스 origin)가 비었습니다");
        return NULL;
    }

    gchar        *jwt   = NULL;
    EVP_PKEY     *pkey  = NULL;
    JsonBuilder  *bld   = NULL;
    JsonGenerator *gen  = NULL;
    JsonNode     *root  = NULL;
    gchar        *claims = NULL;
    gchar        *h_b64 = NULL;
    gchar        *c_b64 = NULL;
    gchar        *s_b64 = NULL;
    gchar        *signing_input = NULL;
    guchar        priv_key[WP_PRIV_LEN];           
    guchar        pub[WP_PUB_LEN];
    guchar        jose[64];

    if (!_b64url_dec_fixed(vapid_priv_b64url, priv_key, WP_PRIV_LEN,
                           "VAPID 개인키", error))
        return NULL;

                                                   
    if (!_p256_pub_from_priv(priv_key, pub)) {
        g_set_error(error, _wp_quark(), WP_ERR_CRYPTO, "VAPID 공개키 역산 실패");
        goto out;
    }
    pkey = _p256_pkey(pub, priv_key);
    if (pkey == NULL) {
        g_set_error(error, _wp_quark(), WP_ERR_CRYPTO, "VAPID 서명키 로드 실패");
        goto out;
    }

                                                                    
    bld = json_builder_new();
    json_builder_begin_object(bld);
    json_builder_set_member_name(bld, "aud");
    json_builder_add_string_value(bld, aud_origin);
    json_builder_set_member_name(bld, "exp");
    json_builder_add_int_value(bld, now_epoch + WP_VAPID_TTL_SEC);
    if (sub_contact != NULL && *sub_contact != '\0') {
                                                              
        json_builder_set_member_name(bld, "sub");
        json_builder_add_string_value(bld, sub_contact);
    }
    json_builder_end_object(bld);

    gen = json_generator_new();
                                                                            
    root = json_builder_get_root(bld);
    json_generator_set_root(gen, root);
    gsize clen = 0;
    claims = json_generator_to_data(gen, &clen);

    h_b64 = _b64url_enc((const guchar *)WP_JWT_HEADER, sizeof(WP_JWT_HEADER) - 1);
    c_b64 = _b64url_enc((const guchar *)claims, clen);
    signing_input = g_strconcat(h_b64, ".", c_b64, NULL);

    if (!_es256_sign_jose(pkey, signing_input, jose)) {
        g_set_error(error, _wp_quark(), WP_ERR_CRYPTO, "ES256 서명 실패");
        goto out;
    }
    s_b64 = _b64url_enc(jose, sizeof jose);
    jwt   = g_strconcat(signing_input, ".", s_b64, NULL);

out:
    pcv_secure_wipe(priv_key, sizeof priv_key);
    g_free(s_b64);
    g_free(signing_input);
    g_free(c_b64);
    g_free(h_b64);
    g_free(claims);
    if (root != NULL) json_node_unref(root);
    if (gen != NULL)  g_object_unref(gen);
    if (bld != NULL)  g_object_unref(bld);
    if (pkey != NULL) EVP_PKEY_free(pkey);
    return jwt;
}

  
                                                     
  
                                                      
                                                  
                                                   
  
                                                     
                                           
  
                                                     
                                                                   
                                                                    
                  
  
                                                                   
                                                  
                                                       
                                                       
                                                          
  
                                                        
                                                  
                          
                                                
                                                   
                                                    
                                   
                                     
                                                     
                                                                 
                                                               
                                                         
                                                                   
                                                                
                                                 
                                     
                                         
                     
                                                            
                                                        
                                               
   
gboolean
pcv_webpush_encrypt(const guchar *pt, gsize pt_len,
                    const gchar *p256dh_b64url, const gchar *auth_b64url,
                    const guchar *salt16, const gchar *as_priv_b64url,
                    guchar **out_body, gsize *out_len, GError **error)
{
    if (out_body == NULL || out_len == NULL) {
        g_set_error(error, _wp_quark(), WP_ERR_ARG, "출력 인자가 NULL 입니다");
        return FALSE;
    }
    if (pt == NULL) {
        g_set_error(error, _wp_quark(), WP_ERR_ARG, "평문이 NULL 입니다");
        return FALSE;
    }
    if (pt_len > WP_MAX_PT_LEN) {
        g_set_error(error, _wp_quark(), WP_ERR_ARG,
                    "평문이 단일 레코드 한도를 넘습니다: %" G_GSIZE_FORMAT
                    " > %u", pt_len, WP_MAX_PT_LEN);
        return FALSE;
    }

    gboolean        ok      = FALSE;
    EVP_PKEY       *as_pkey = NULL;
    EVP_PKEY       *ua_pkey = NULL;
    EVP_PKEY_CTX   *dctx    = NULL;
    EVP_CIPHER_CTX *cctx    = NULL;
    guchar         *record  = NULL;                         
    guchar         *body    = NULL;

    guchar ua_pub[WP_PUB_LEN];
    guchar as_pub[WP_PUB_LEN];
    guchar auth[WP_AUTH_LEN];                         
    guchar as_priv_key[WP_PRIV_LEN] = { 0 };                        
    guchar salt[WP_SALT_LEN];
    guchar ecdh_key[WP_ECDH_LEN];                     
    guchar prk_key[WP_PRK_LEN];                       
    guchar ikm_key[WP_PRK_LEN];                       
    guchar prk[WP_PRK_LEN];                           
    guchar cek_key[WP_CEK_LEN];                       
    guchar nonce[WP_NONCE_LEN];
    guchar key_info[sizeof(WP_KEY_INFO_PREFIX) + WP_PUB_LEN + WP_PUB_LEN];

                                          
    if (!_b64url_dec_fixed(p256dh_b64url, ua_pub, WP_PUB_LEN, "p256dh", error))
        goto out;
    if (ua_pub[0] != 0x04) {
        g_set_error(error, _wp_quark(), WP_ERR_DECODE,
                    "p256dh 가 uncompressed 포인트가 아닙니다(선두 0x%02x)", ua_pub[0]);
        goto out;
    }
    if (!_b64url_dec_fixed(auth_b64url, auth, WP_AUTH_LEN, "auth", error))
        goto out;

                                                      
    if (salt16 != NULL) {
        memcpy(salt, salt16, WP_SALT_LEN);
    } else if (RAND_bytes(salt, (int)WP_SALT_LEN) != 1) {
        g_set_error(error, _wp_quark(), WP_ERR_CRYPTO, "salt 난수 생성 실패");
        goto out;
    }

                                                  
    if (as_priv_b64url != NULL) {
        if (!_b64url_dec_fixed(as_priv_b64url, as_priv_key, WP_PRIV_LEN,
                               "as_priv", error))
            goto out;
        if (!_p256_pub_from_priv(as_priv_key, as_pub)) {
            g_set_error(error, _wp_quark(), WP_ERR_CRYPTO, "as_pub 역산 실패");
            goto out;
        }
        as_pkey = _p256_pkey(as_pub, as_priv_key);
    } else {
        size_t publen = 0;
        as_pkey = EVP_PKEY_Q_keygen(NULL, NULL, "EC", "P-256");
        if (as_pkey == NULL
            || EVP_PKEY_get_octet_string_param(as_pkey, OSSL_PKEY_PARAM_PUB_KEY,
                                               as_pub, sizeof as_pub, &publen) != 1
            || publen != WP_PUB_LEN) {
            g_set_error(error, _wp_quark(), WP_ERR_CRYPTO, "임시 P-256 키 생성 실패");
            goto out;
        }
    }
    if (as_pkey == NULL) {
        g_set_error(error, _wp_quark(), WP_ERR_CRYPTO, "애플리케이션 서버 키 로드 실패");
        goto out;
    }

    ua_pkey = _p256_pkey(ua_pub, NULL);
    if (ua_pkey == NULL) {
        g_set_error(error, _wp_quark(), WP_ERR_DECODE,
                    "p256dh 가 유효한 P-256 포인트가 아닙니다");
        goto out;
    }

                                                      
    size_t ecdh_len = sizeof ecdh_key;
    dctx = EVP_PKEY_CTX_new_from_pkey(NULL, as_pkey, NULL);
    if (dctx == NULL
        || EVP_PKEY_derive_init(dctx) != 1
        || EVP_PKEY_derive_set_peer(dctx, ua_pkey) != 1
        || EVP_PKEY_derive(dctx, ecdh_key, &ecdh_len) != 1
        || ecdh_len != WP_ECDH_LEN) {
        g_set_error(error, _wp_quark(), WP_ERR_CRYPTO, "ECDH 공유 비밀 유도 실패");
        goto out;
    }

                                                                             
    memcpy(key_info, WP_KEY_INFO_PREFIX, sizeof WP_KEY_INFO_PREFIX);                      
    memcpy(key_info + sizeof WP_KEY_INFO_PREFIX, ua_pub, WP_PUB_LEN);
    memcpy(key_info + sizeof WP_KEY_INFO_PREFIX + WP_PUB_LEN, as_pub, WP_PUB_LEN);

    if (!_hkdf_extract(auth, WP_AUTH_LEN, ecdh_key, WP_ECDH_LEN, prk_key)
        || !_hkdf_expand(prk_key, key_info, sizeof key_info, ikm_key, sizeof ikm_key)
                                                               
        || !_hkdf_extract(salt, WP_SALT_LEN, ikm_key, sizeof ikm_key, prk)
        || !_hkdf_expand(prk, (const guchar *)WP_CEK_INFO, sizeof WP_CEK_INFO,
                         cek_key, WP_CEK_LEN)
        || !_hkdf_expand(prk, (const guchar *)WP_NONCE_INFO, sizeof WP_NONCE_INFO,
                         nonce, WP_NONCE_LEN)) {
        g_set_error(error, _wp_quark(), WP_ERR_CRYPTO, "HKDF 키 파생 실패");
        goto out;
    }

                                                      
    gsize rec_len = pt_len + 1;
    record = g_malloc(rec_len);
    memcpy(record, pt, pt_len);
    record[pt_len] = 0x02;

                                                                 
    body = g_malloc(WP_HDR_LEN + rec_len + WP_TAG_LEN);
    memcpy(body, salt, WP_SALT_LEN);
    body[16] = (guchar)((WP_RECORD_SIZE >> 24) & 0xffu);
    body[17] = (guchar)((WP_RECORD_SIZE >> 16) & 0xffu);
    body[18] = (guchar)((WP_RECORD_SIZE >> 8) & 0xffu);
    body[19] = (guchar)(WP_RECORD_SIZE & 0xffu);
    body[20] = (guchar)WP_PUB_LEN;
    memcpy(body + 21, as_pub, WP_PUB_LEN);

                                                           
    int outl = 0, finl = 0;
    cctx = EVP_CIPHER_CTX_new();
    if (cctx == NULL
        || EVP_EncryptInit_ex(cctx, EVP_aes_128_gcm(), NULL, NULL, NULL) != 1
        || EVP_CIPHER_CTX_ctrl(cctx, EVP_CTRL_GCM_SET_IVLEN,
                               (int)WP_NONCE_LEN, NULL) != 1
        || EVP_EncryptInit_ex(cctx, NULL, NULL, cek_key, nonce) != 1
        || EVP_EncryptUpdate(cctx, body + WP_HDR_LEN, &outl,
                             record, (int)rec_len) != 1
        || EVP_EncryptFinal_ex(cctx, body + WP_HDR_LEN + outl, &finl) != 1
        || EVP_CIPHER_CTX_ctrl(cctx, EVP_CTRL_GCM_GET_TAG, (int)WP_TAG_LEN,
                               body + WP_HDR_LEN + outl + finl) != 1) {
        g_set_error(error, _wp_quark(), WP_ERR_CRYPTO, "AES-128-GCM 봉인 실패");
        goto out;
    }

    *out_body = body;
    *out_len  = WP_HDR_LEN + (gsize)outl + (gsize)finl + WP_TAG_LEN;
    body      = NULL;               
    ok        = TRUE;

out:
                                               
    pcv_secure_wipe(auth, sizeof auth);
    pcv_secure_wipe(as_priv_key, sizeof as_priv_key);
    pcv_secure_wipe(ecdh_key, sizeof ecdh_key);
    pcv_secure_wipe(prk_key, sizeof prk_key);
    pcv_secure_wipe(ikm_key, sizeof ikm_key);
    pcv_secure_wipe(prk, sizeof prk);
    pcv_secure_wipe(cek_key, sizeof cek_key);
    pcv_secure_wipe(nonce, sizeof nonce);
    if (record != NULL) {
        pcv_secure_wipe(record, pt_len + 1);              
        g_free(record);
    }
    g_free(body);                                   
    if (cctx != NULL)    EVP_CIPHER_CTX_free(cctx);
    if (dctx != NULL)    EVP_PKEY_CTX_free(dctx);
    if (ua_pkey != NULL) EVP_PKEY_free(ua_pkey);
    if (as_pkey != NULL) EVP_PKEY_free(as_pkey);
    return ok;
}
