   
                   
                                                            
  
                           
                                                   
                                                    
                                        
  
          
                                                
                                            
                                                     
                                       
  
        
                                                              
                                                 
                                                    
                                                             
         
                                                            
                                                            
                                                         
            
            
                                               
                                                   
                                                                                       
  
            
                                     
                                                                          
                                                                                  
                                                               
  
                                                                              
   

#include "pcv_totp.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <limits.h>
#include <string.h>

                                                          
static const gchar B32_ALPHA[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

                                                  
#define TOTP_STEP_SECONDS 30

                                                                
#define TOTP_SECRET_BYTES 20

                                                                   

   
                      
              
              
  
                                          
                                                          
                                                          
                                     
   
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

    g_error("pcv_totp: secure RNG unavailable "
            "(/dev/urandom and OpenSSL RAND_bytes both failed) — refusing to "
            "generate predictable TOTP secret");
}

                                                                   

   
                  
                       
                      
  
                                                            
                                                  
                                                  
  
                                                  
   
static gchar *
_base32_encode(const guchar *data, gsize len)
{
    GString *out = g_string_sized_new(((len * 8) + 4) / 5);
    guint32 buffer = 0;
    gint bits_left = 0;

    for (gsize i = 0; i < len; i++) {
        buffer = (buffer << 8) | data[i];
        bits_left += 8;
        while (bits_left >= 5) {
            bits_left -= 5;
            g_string_append_c(out, B32_ALPHA[(buffer >> bits_left) & 0x1F]);
        }
        buffer &= (1u << bits_left) - 1;                        
    }
    if (bits_left > 0)
        g_string_append_c(out, B32_ALPHA[(buffer << (5 - bits_left)) & 0x1F]);

    return g_string_free(out, FALSE);
}

   
                          
  
                                                 
                                                
                                                    
                                                  
   
gboolean
pcv_totp_base32_decode(const gchar *b32, guchar *out, gsize *out_len)
{
    g_return_val_if_fail(b32 != NULL, FALSE);
    g_return_val_if_fail(out != NULL, FALSE);
    g_return_val_if_fail(out_len != NULL, FALSE);

    guint32 buffer = 0;
    gint bits_left = 0;
    gsize n = 0;

    for (const gchar *p = b32; *p; p++) {
        gchar c = *p;
        if (c == '=' || g_ascii_isspace(c))
            continue;
        c = g_ascii_toupper(c);

        gint val;
        if (c >= 'A' && c <= 'Z')
            val = c - 'A';
        else if (c >= '2' && c <= '7')
            val = (c - '2') + 26;
        else
            return FALSE;                        

        buffer = (buffer << 5) | (guint32)val;
        bits_left += 5;
        if (bits_left >= 8) {
            bits_left -= 8;
            if (n >= 64)
                return FALSE;                                
            out[n++] = (guchar)((buffer >> bits_left) & 0xFF);
        }
        buffer &= (1u << bits_left) - 1;
    }

    *out_len = n;
    return TRUE;
}

                                                                  

   
                    
  
                                                    
                                                                      
                            
  
                                                           
                                                         
                                                          
                                                           
                                                              
                                                         
   
guint
pcv_totp_code_at(const guchar *key, gsize key_len, gint64 step, guint digits)
{
    guchar msg[8];
    gint64 s = step;
    for (gint i = 7; i >= 0; i--) {
        msg[i] = (guchar)(s & 0xFF);
        s >>= 8;
    }

    guchar mac[EVP_MAX_MD_SIZE];
    guint  mac_len = 0;
    guchar *hmac_result = HMAC(EVP_sha1(), key, (int)key_len, msg, sizeof msg, mac, &mac_len);
    if (!hmac_result || mac_len == 0)
        return UINT_MAX;                                              

    guint off = mac[mac_len - 1] & 0x0F;
    guint32 bin = ((guint32)(mac[off] & 0x7F) << 24)
                | ((guint32)mac[off + 1] << 16)
                | ((guint32)mac[off + 2] << 8)
                | (guint32)mac[off + 3];

    guint32 mod = 1;
    for (guint i = 0; i < digits; i++)
        mod *= 10;

    return bin % mod;
}

   
                  
  
                                                             
                                                         
                                                      
                                                                
                                                    
                                                 
   
gboolean
pcv_totp_check(const gchar *secret_b32, const gchar *code,
               gint64 now_unix, gint64 last_step, gint64 *out_step)
{
    if (!secret_b32 || !code || strlen(code) != 6)
        return FALSE;
    for (const gchar *p = code; *p; p++)
        if (!g_ascii_isdigit(*p))
            return FALSE;

    guchar key[64];
    gsize  klen = 0;
    if (!pcv_totp_base32_decode(secret_b32, key, &klen))
        return FALSE;

    guint want = (guint)g_ascii_strtoull(code, NULL, 10);
    gint64 step_now = now_unix / TOTP_STEP_SECONDS;
    gboolean matched = FALSE;

    for (gint d = -1; d <= 1; d++) {
        gint64 cand = step_now + d;
                                                                           
                                                       
                                                          
        if (cand <= last_step)
            continue;                     
        guint got = pcv_totp_code_at(key, klen, cand, 6);
        if (got == UINT_MAX)
            continue;                                                         
        if (got == want) {
            if (out_step)
                *out_step = cand;
            matched = TRUE;
            break;
        }
    }

    OPENSSL_cleanse(key, sizeof key);
    return matched;
}

                                                              

   
                            
  
                                                             
                                              
   
gchar *
pcv_totp_generate_secret(void)
{
    guchar raw[TOTP_SECRET_BYTES];
    _fill_random_bytes(raw, sizeof raw);
    gchar *b32 = _base32_encode(raw, sizeof raw);
    OPENSSL_cleanse(raw, sizeof raw);
    return b32;
}

   
                      
  
                                                        
                                                                                    
   
gchar *
pcv_totp_build_uri(const gchar *username, const gchar *hostname, const gchar *secret_b32)
{
    g_return_val_if_fail(username != NULL, NULL);
    g_return_val_if_fail(hostname != NULL, NULL);
    g_return_val_if_fail(secret_b32 != NULL, NULL);

    gchar *user_esc = g_uri_escape_string(username, NULL, FALSE);
    gchar *host_esc = g_uri_escape_string(hostname, NULL, FALSE);

    gchar *uri = g_strdup_printf(
        "otpauth://totp/PureCVisor:%s@%s?secret=%s&issuer=PureCVisor",
        user_esc, host_esc, secret_b32);

    g_free(user_esc);
    g_free(host_esc);
    return uri;
}
