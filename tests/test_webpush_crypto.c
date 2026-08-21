                                                                                         
                                                                                                   
                                                              
                                                            
                                                  
                              
  
                                           
  
                                                  
                                             
                                             
                                       
  
         
                                                                    
                                                                         
                                                                  
                                                                   
  
                                              
                                                
                                              
                                                        
                                               
   
#include <glib.h>
#include <string.h>
#include <json-glib/json-glib.h>
#include <openssl/evp.h>
#include <openssl/ec.h>
#include <openssl/bn.h>
#include <openssl/ecdsa.h>
#include <openssl/obj_mac.h>
#include <openssl/param_build.h>
#include <openssl/core_names.h>

#include "modules/daemons/pcv_webpush_crypto.h"

                                                            
                                                      
#define VEC_UA_PUB  "BCVxsr7N_eNgVRqvHtD0zTZsEc6-VV-JvLexhqUzORcxaOzi6-AYWXvTBHm4bjyPjs7Vd8pZGH6SRpkNtoIAiw4"
                           
#define VEC_AUTH    "BTBZMqHH6r4Tts7J_aSIgg"
                                                  
#define VEC_AS_PRIV "yfWPiYE-n46HLnH0KqZOF1fJJU3MYrct3AELtAQ-oRw"
                                     
#define VEC_SALT    "DGv6ra1nlYgDCS1FRnbzlw"
             
#define VEC_PT      "When I grow up, I want to be a watermelon"
                                                                      
#define VEC_BODY \
    "DGv6ra1nlYgDCS1FRnbzlwAAEABBBP4z9KsN6nGRTbVYI_c7VJSPQTBtkgcy27mlmlMoZIIgDll6" \
    "e3vCYLocInmYWAmS6TlzAC8wEqKK6PBru3jl7A_yl95bQpu6cVPTpK4Mqgkf1CXztLVBSt2Ks3oZ" \
    "wbuwXPXLWyouBWLVWGNWQexSgSxsj_Qulcy4a-fN"

                                                            

                                        
static gchar *
_tb64_enc(const guchar *data, gsize len)
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
_tb64_dec(const gchar *str, gsize *out_len)
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

                                                           

  
                                                       
                                              
                                                              
   
static void
test_rfc8291_vector(void)
{
    gsize salt_len = 0;
    guchar *salt = _tb64_dec(VEC_SALT, &salt_len);
    g_assert_cmpuint(salt_len, ==, 16);

    guchar *body = NULL;
    gsize   blen = 0;
    GError *err  = NULL;
    gboolean ok = pcv_webpush_encrypt((const guchar *)VEC_PT, strlen(VEC_PT),
                                      VEC_UA_PUB, VEC_AUTH,
                                      salt, VEC_AS_PRIV,
                                      &body, &blen, &err);
    g_assert_no_error(err);
    g_assert_true(ok);
    g_assert_cmpuint(blen, ==, 144);

    gchar *got = _tb64_enc(body, blen);
    g_assert_cmpstr(got, ==, VEC_BODY);

    g_free(got);
    g_free(body);
    g_free(salt);
}

                                                    
                                             
static void
test_random_path_shape(void)
{
    guchar *b1 = NULL, *b2 = NULL;
    gsize   l1 = 0, l2 = 0;
    GError *err = NULL;

    g_assert_true(pcv_webpush_encrypt((const guchar *)VEC_PT, strlen(VEC_PT),
                                      VEC_UA_PUB, VEC_AUTH, NULL, NULL,
                                      &b1, &l1, &err));
    g_assert_no_error(err);
    g_assert_true(pcv_webpush_encrypt((const guchar *)VEC_PT, strlen(VEC_PT),
                                      VEC_UA_PUB, VEC_AUTH, NULL, NULL,
                                      &b2, &l2, &err));
    g_assert_no_error(err);

                                                                            
    g_assert_cmpuint(l1, ==, 144);
    g_assert_cmpuint(l2, ==, 144);
    g_assert_cmpuint(b1[16], ==, 0x00);                       
    g_assert_cmpuint(b1[17], ==, 0x00);
    g_assert_cmpuint(b1[18], ==, 0x10);
    g_assert_cmpuint(b1[19], ==, 0x00);
    g_assert_cmpuint(b1[20], ==, 65);                
    g_assert_cmpuint(b1[21], ==, 0x04);                                  
                                                     
    g_assert_cmpint(memcmp(b1, b2, 144), !=, 0);

    g_free(b1);
    g_free(b2);
}

                                                             

                                                      
static EVP_PKEY *
_pub_pkey(const guchar *pub65)
{
    EVP_PKEY        *pkey = NULL;
    OSSL_PARAM_BLD  *bld  = OSSL_PARAM_BLD_new();
    OSSL_PARAM      *par  = NULL;
    EVP_PKEY_CTX    *ctx  = NULL;

    if (bld
        && OSSL_PARAM_BLD_push_utf8_string(bld, OSSL_PKEY_PARAM_GROUP_NAME,
                                           "prime256v1", 0) == 1
        && OSSL_PARAM_BLD_push_octet_string(bld, OSSL_PKEY_PARAM_PUB_KEY,
                                            pub65, 65) == 1
        && (par = OSSL_PARAM_BLD_to_param(bld)) != NULL
        && (ctx = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL)) != NULL
        && EVP_PKEY_fromdata_init(ctx) == 1) {
        if (EVP_PKEY_fromdata(ctx, &pkey, EVP_PKEY_PUBLIC_KEY, par) != 1)
            pkey = NULL;
    }
    if (ctx) EVP_PKEY_CTX_free(ctx);
    if (par) OSSL_PARAM_free(par);
    if (bld) OSSL_PARAM_BLD_free(bld);
    return pkey;
}

                                            
static gboolean
_verify_es256(const gchar *signing_input, const guchar *jose64, const guchar *pub65)
{
    gboolean   ok  = FALSE;
    EVP_PKEY  *pk  = _pub_pkey(pub65);
    ECDSA_SIG *sig = ECDSA_SIG_new();
    BIGNUM    *r   = BN_bin2bn(jose64, 32, NULL);
    BIGNUM    *s   = BN_bin2bn(jose64 + 32, 32, NULL);
    guchar    *der = NULL;
    int        derlen = 0;
    EVP_MD_CTX *md = EVP_MD_CTX_new();

    if (pk && sig && r && s && md && ECDSA_SIG_set0(sig, r, s) == 1) {
        r = s = NULL;                                              
        derlen = i2d_ECDSA_SIG(sig, &der);
        if (derlen > 0
            && EVP_DigestVerifyInit(md, NULL, EVP_sha256(), NULL, pk) == 1
            && EVP_DigestVerify(md, der, (size_t)derlen,
                                (const guchar *)signing_input,
                                strlen(signing_input)) == 1)
            ok = TRUE;
    }
    if (der) OPENSSL_free(der);
    if (md)  EVP_MD_CTX_free(md);
    if (sig) ECDSA_SIG_free(sig);
    if (r)   BN_free(r);
    if (s)   BN_free(s);
    if (pk)  EVP_PKEY_free(pk);
    return ok;
}

                                                
static void
_assert_jwt(const gchar *jwt, const gchar *pub_b64url,
            const gchar *want_aud, const gchar *want_sub, gint64 now)
{
    gchar **parts = g_strsplit(jwt, ".", -1);
    g_assert_cmpuint(g_strv_length(parts), ==, 3);

                                             
    gsize hlen = 0;
    guchar *hdr = _tb64_dec(parts[0], &hlen);
    gchar  *hdr_s = g_strndup((const gchar *)hdr, hlen);
    g_assert_cmpstr(hdr_s, ==, "{\"typ\":\"JWT\",\"alg\":\"ES256\"}");

                            
    gsize slen = 0;
    guchar *sig = _tb64_dec(parts[2], &slen);
    g_assert_cmpuint(slen, ==, 64);

    gsize plen = 0;
    guchar *pub = _tb64_dec(pub_b64url, &plen);
    g_assert_cmpuint(plen, ==, 65);

    gchar *signing_input = g_strdup_printf("%s.%s", parts[0], parts[1]);
    g_assert_true(_verify_es256(signing_input, sig, pub));

                
    gsize clen = 0;
    guchar *claims = _tb64_dec(parts[1], &clen);
    JsonParser *jp = json_parser_new();
    GError *perr = NULL;
    g_assert_true(json_parser_load_from_data(jp, (const gchar *)claims,
                                             (gssize)clen, &perr));
    g_assert_no_error(perr);
    JsonObject *o = json_node_get_object(json_parser_get_root(jp));
    g_assert_cmpstr(json_object_get_string_member(o, "aud"), ==, want_aud);
    g_assert_cmpint(json_object_get_int_member(o, "exp"), ==, now + 43200);
    if (want_sub)
        g_assert_cmpstr(json_object_get_string_member(o, "sub"), ==, want_sub);
    else
        g_assert_false(json_object_has_member(o, "sub"));

    g_object_unref(jp);
    g_free(claims);
    g_free(signing_input);
    g_free(pub);
    g_free(sig);
    g_free(hdr_s);
    g_free(hdr);
    g_strfreev(parts);
}

static void
test_vapid_jwt_verify(void)
{
    gchar *priv = NULL, *pub = NULL;
    GError *err = NULL;
    g_assert_true(pcv_webpush_keypair_generate(&priv, &pub, &err));
    g_assert_no_error(err);

    const gint64 now = 1753800000;                             

    gchar *jwt = pcv_webpush_vapid_jwt("https://push.example.net",
                                       "mailto:ops@example.com", priv, now, &err);
    g_assert_no_error(err);
    g_assert_nonnull(jwt);
    _assert_jwt(jwt, pub, "https://push.example.net", "mailto:ops@example.com", now);
    g_free(jwt);

                                               
    jwt = pcv_webpush_vapid_jwt("https://push.example.net", NULL, priv, now, &err);
    g_assert_no_error(err);
    g_assert_nonnull(jwt);
    _assert_jwt(jwt, pub, "https://push.example.net", NULL, now);
    g_free(jwt);

    g_free(priv);
    g_free(pub);
}

                                                        

static void
test_keypair_roundtrip(void)
{
    gchar *priv = NULL, *pub = NULL;
    GError *err = NULL;

    g_assert_true(pcv_webpush_keypair_generate(&priv, &pub, &err));
    g_assert_no_error(err);
    g_assert_nonnull(priv);
    g_assert_nonnull(pub);

                                                      
    g_assert_null(strchr(priv, '='));
    g_assert_null(strchr(pub, '='));
    g_assert_null(strchr(pub, '+'));
    g_assert_null(strchr(pub, '/'));

    gsize plen = 0, dlen = 0;
    guchar *pubraw  = _tb64_dec(pub, &plen);
    guchar *privraw = _tb64_dec(priv, &dlen);
    g_assert_cmpuint(plen, ==, 65);
    g_assert_cmpuint(dlen, ==, 32);
    g_assert_cmpuint(pubraw[0], ==, 0x04);                         

                                     
    gchar *jwt = pcv_webpush_vapid_jwt("https://fcm.googleapis.com", NULL,
                                       priv, 1753800000, &err);
    g_assert_no_error(err);
    g_assert_nonnull(jwt);
    _assert_jwt(jwt, pub, "https://fcm.googleapis.com", NULL, 1753800000);

                                               
    gchar *priv2 = NULL, *pub2 = NULL;
    g_assert_true(pcv_webpush_keypair_generate(&priv2, &pub2, &err));
    g_assert_no_error(err);
    g_assert_cmpstr(priv, !=, priv2);
    g_assert_cmpstr(pub, !=, pub2);

    g_free(pub2);
    g_free(priv2);
    g_free(jwt);
    g_free(privraw);
    g_free(pubraw);
    g_free(pub);
    g_free(priv);
}

                                                          

static void
test_reject_bad_input(void)
{
    guchar *body = NULL;
    gsize   blen = 0;
    GError *err  = NULL;

                               
    g_assert_false(pcv_webpush_encrypt((const guchar *)"x", 1,
                                       "AAAA", VEC_AUTH, NULL, NULL,
                                       &body, &blen, &err));
    g_assert_nonnull(err);
    g_clear_error(&err);

                             
    g_assert_false(pcv_webpush_encrypt((const guchar *)"x", 1,
                                       VEC_UA_PUB, "AAAA", NULL, NULL,
                                       &body, &blen, &err));
    g_assert_nonnull(err);
    g_clear_error(&err);

                 
    g_assert_false(pcv_webpush_encrypt(NULL, 0, VEC_UA_PUB, VEC_AUTH,
                                       NULL, NULL, &body, &blen, &err));
    g_assert_nonnull(err);
    g_clear_error(&err);

                                                      
    guchar *big = g_malloc0(4080);
    g_assert_false(pcv_webpush_encrypt(big, 4080, VEC_UA_PUB, VEC_AUTH,
                                       NULL, NULL, &body, &blen, &err));
    g_assert_nonnull(err);
    g_clear_error(&err);
    g_assert_true(pcv_webpush_encrypt(big, 4079, VEC_UA_PUB, VEC_AUTH,
                                      NULL, NULL, &body, &blen, &err));
    g_assert_no_error(err);
    g_assert_cmpuint(blen, ==, 86 + 4079 + 1 + 16);
    g_free(body);
    body = NULL;
    g_free(big);

                           
    g_assert_null(pcv_webpush_vapid_jwt("https://push.example.net", NULL,
                                        "AAAA", 1753800000, &err));
    g_assert_nonnull(err);
    g_clear_error(&err);

                       
    g_assert_null(pcv_webpush_vapid_jwt(NULL, NULL, VEC_AS_PRIV,
                                        1753800000, &err));
    g_assert_nonnull(err);
    g_clear_error(&err);
}

void
test_webpush_crypto_register(void)
{
    g_test_add_func("/webpush_crypto/rfc8291_vector",    test_rfc8291_vector);
    g_test_add_func("/webpush_crypto/random_path_shape", test_random_path_shape);
    g_test_add_func("/webpush_crypto/vapid_jwt_verify",  test_vapid_jwt_verify);
    g_test_add_func("/webpush_crypto/keypair_roundtrip", test_keypair_roundtrip);
    g_test_add_func("/webpush_crypto/reject_bad_input",  test_reject_bad_input);
}
