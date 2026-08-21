                                                                                         
                                                                                       
                                                                  
                                                              
                        
                    
  
                                                                       
  
                 
                                                                   
                                                         
                                            
  
                                  
  
          
                                                               
                                                    
  
                                         
   

#include <glib.h>
#include "../src/utils/pcv_totp.h"

static const guchar RFC_KEY[] = "12345678901234567890";             

                                                             
static void test_hotp_rfc4226_vectors(gpointer *f, gconstpointer d)
{
    (void)f; (void)d;
    static const guint expected[] = { 755224, 287082, 359152, 969429, 338314, 254676 };
    for (gint64 c = 0; c < 6; c++)
        g_assert_cmpuint(pcv_totp_code_at(RFC_KEY, 20, c, 6), ==, expected[c]);
}

                                                                       
static void test_totp_rfc6238_sha1_vectors(gpointer *f, gconstpointer d)
{
    (void)f; (void)d;
    g_assert_cmpuint(pcv_totp_code_at(RFC_KEY, 20, 59 / 30, 8), ==, 94287082);
    g_assert_cmpuint(pcv_totp_code_at(RFC_KEY, 20, 1111111109LL / 30, 8), ==, 7081804);
    g_assert_cmpuint(pcv_totp_code_at(RFC_KEY, 20, 2000000000LL / 30, 8), ==, 69279037);
}

                                                         
static void test_base32_roundtrip(gpointer *f, gconstpointer d)
{
    (void)f; (void)d;
    gchar *b32 = pcv_totp_generate_secret();
    g_assert_nonnull(b32);
    g_assert_cmpuint(strlen(b32), ==, 32);                                
    guchar buf[64]; gsize len = 0;
    g_assert_true(pcv_totp_base32_decode(b32, buf, &len));
    g_assert_cmpuint(len, ==, 20);
                                                   
    g_assert_true(pcv_totp_base32_decode("GEZDGNBVGY3TQOJQ", buf, &len));
    g_assert_cmpuint(len, ==, 10);
    g_assert_cmpmem(buf, len, "1234567890", 10);
    g_free(b32);
}

                                                                      
static void test_check_window_and_replay(gpointer *f, gconstpointer d)
{
    (void)f; (void)d;
    gchar *b32 = pcv_totp_generate_secret();
    guchar key[64]; gsize klen; pcv_totp_base32_decode(b32, key, &klen);
    gint64 now = 1700000000;                                  
    gint64 step_now = now / 30;
    gchar code[8];
    g_snprintf(code, sizeof code, "%06u", pcv_totp_code_at(key, klen, step_now, 6));
    gint64 matched = 0;
               
    g_assert_true(pcv_totp_check(b32, code, now, 0, &matched));
    g_assert_cmpint(matched, ==, step_now);
                                           
    g_assert_false(pcv_totp_check(b32, code, now, matched, &matched));
                             
    g_snprintf(code, sizeof code, "%06u", pcv_totp_code_at(key, klen, step_now - 1, 6));
    g_assert_true(pcv_totp_check(b32, code, now, 0, &matched));
    g_assert_cmpint(matched, ==, step_now - 1);
                   
    g_snprintf(code, sizeof code, "%06u", pcv_totp_code_at(key, klen, step_now - 2, 6));
    g_assert_false(pcv_totp_check(b32, code, now, 0, &matched));
                  
    g_assert_false(pcv_totp_check(b32, "12345", now, 0, &matched));
    g_assert_false(pcv_totp_check(b32, "abcdef", now, 0, &matched));
    g_free(b32);
}

void test_totp_register(void)
{
#define ADD(name, func) g_test_add("/totp/" name, gpointer, NULL, NULL, func, NULL)
    ADD("hotp_rfc4226", test_hotp_rfc4226_vectors);
    ADD("totp_rfc6238_sha1", test_totp_rfc6238_sha1_vectors);
    ADD("base32_roundtrip", test_base32_roundtrip);
    ADD("window_replay", test_check_window_and_replay);
#undef ADD
}
