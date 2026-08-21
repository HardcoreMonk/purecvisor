   
                  
                                         
  
                           
                                                   
                                                    
                                        
  
                                                      
                                                       
                                                    
                                                   
                                                     
                                                        
                                               
  
                                                       
                                                        
                                                  
                                                      
                                                    
                                               
                             
                                                       
                                              
  
            
                                                             
                                                                   
                                                        
                                               
  
                                                 
                                              
                                                                    
                                                                 
                                                     
                                                                        
                                               
                                                          
                                                       
                                                                         
  
          
                                 
            
                                                                          
                                                                              
                                                                              
                                                                                      
  
                                              
                                                         
                                                           
                                                                
               
                                              
                                                      
                                                        
                                                
                                                
  
               
                                        
                                                                  
                                                   
                                                   
                                                         
  
        
                                                           
                                                                    
  
         
                              
                                                             
                                                             
                                                 
                                                                   
                                                              
  
                                                               
                                                             
                                                  
                                                                            
                                                              
                                         
                                                                     
                                                               
                                                                           
                                                                          
                                                            
                                                                      
   
#include "pcv_tls.h"
#include "pcv_log.h"
#include "pcv_config.h"
#include <json-glib/json-glib.h>
#include <gio/gio.h>
#include <string.h>
#include <errno.h>                                      
#include <unistd.h>                        
#include <ifaddrs.h>                             
#include <net/if.h>                         
#include <netinet/in.h>
#include <arpa/inet.h>
#include <glib/gstdio.h>               
#include "pcv_spawn.h"

                                                                    
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
                                                       
                                                                 
                                            
    if (!cert || !key) {
        g_set_error(error, g_quark_from_static_string("tls"), 1,
                    "cert, key paths required");
        return NULL;
    }

    if (!g_file_test(cert, G_FILE_TEST_EXISTS) ||
        !g_file_test(key, G_FILE_TEST_EXISTS)) {
        g_set_error(error, g_quark_from_static_string("tls"), 2,
                    "Certificate files not found");
        return NULL;
    }

                                                      
    PcvTlsCtx *ctx = g_new0(PcvTlsCtx, 1);
    ctx->cert_path = g_strdup(cert);
    ctx->key_path = g_strdup(key);
                                                                   
    ctx->ca_path = (ca && g_file_test(ca, G_FILE_TEST_EXISTS)) ? g_strdup(ca) : NULL;
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
        if (G.ctx->ca_path)
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

                                                               

                                                                           
                                                 
                                             
                          
                                                    
                                                      
                                                              
static gchar *_autogen_san_string(const gchar *hostname)
{
    GString *san = g_string_new(NULL);
    g_string_append_printf(san, "DNS:%s", hostname);                      
    struct ifaddrs *ifs = NULL;
    if (getifaddrs(&ifs) == 0) {                                          
        for (struct ifaddrs *a = ifs; a; a = a->ifa_next) {
            if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET) continue;              
            if (a->ifa_flags & IFF_LOOPBACK) continue;                                
            char buf[INET_ADDRSTRLEN];
            struct sockaddr_in *sin = (struct sockaddr_in *)a->ifa_addr;
            if (!inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf))) continue;
            g_string_append_printf(san, ",IP:%s", buf);
        }
        freeifaddrs(ifs);
    }
    return g_string_free(san, FALSE);
}

   
                                                        
                                                         
                                                      
                                                     
                                                      
                                  
                                  
                             
  
                                                            
                                                 
                  
  
                            
                                                         
                                            
                                                                      
                                                                                
                                                    
                                                    
                                           
                                          
                                                
                                         
  
                                        
                                                 
                                         
                                                 
                                         
                                                                    
                                          
                
  
                                                     
                                                     
                                           
                                 
   
gboolean pcv_tls_autogen_selfsigned(const gchar *cert_path,
                                    const gchar *key_path, GError **error)
{
    g_return_val_if_fail(cert_path && key_path, FALSE);

    gchar host[256];
    if (gethostname(host, sizeof(host) - 1) != 0)
        g_strlcpy(host, "purecvisor", sizeof(host));
    host[sizeof(host) - 1] = '\0';

    gchar *subj      = g_strdup_printf("/CN=%s", host);
    gchar *san       = _autogen_san_string(host);
    gchar *san_ext   = g_strdup_printf("subjectAltName=%s", san);
    gchar *cert_tmp  = g_strdup_printf("%s.tmp", cert_path);
    gchar *key_tmp   = g_strdup_printf("%s.tmp", key_path);
    gchar *marker    = g_strdup_printf("%s.autogen", cert_path);
    gchar *marker_tmp = g_strdup_printf("%s.tmp", marker);
    gchar *errout    = NULL;
    gboolean success = FALSE;

                                                              
                                                              
                                                                
                                
                                                       
                                      
    const gchar *argv[] = {
        "openssl", "req", "-x509",
        "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256",
        "-keyout", key_tmp, "-out", cert_tmp,
        "-days", "3650", "-nodes",
        "-subj", subj, "-addext", san_ext,
        NULL
    };

    if (!pcv_spawn_sync_timeout(argv, NULL, &errout, 30, error)) {
        PCV_LOG_ERROR(TLS_LOG_DOM, "자가서명 생성 실패: %s",
                      errout ? errout : "unknown");
        goto cleanup;
    }

                                                
                                                     
    if (g_chmod(key_tmp, 0600) != 0) {
                                                           
                                               
        gchar *msg = g_strdup_printf("키 파일 권한 고정 실패: %s", key_tmp);
        g_set_error_literal(error, g_quark_from_static_string("tls"), 3, msg);
        PCV_LOG_ERROR(TLS_LOG_DOM, "%s", msg);
        g_free(msg);
        goto cleanup;
    }

                                          
                                                    
                                 
    {
        GDateTime *now = g_date_time_new_now_utc();
        gchar *ts = g_date_time_format_iso8601(now);
        gboolean marker_ok = g_file_set_contents(marker_tmp, ts, -1, NULL);
        g_free(ts);
        g_date_time_unref(now);
        if (!marker_ok) {
            gchar *msg = g_strdup_printf("autogen 마커 임시 기록 실패: %s", marker_tmp);
            g_set_error_literal(error, g_quark_from_static_string("tls"), 4, msg);
            PCV_LOG_ERROR(TLS_LOG_DOM, "%s", msg);
            g_free(msg);
            goto cleanup;
        }
    }

                                                         
                                          
                                                
                                                          
                                                
    if (g_rename(cert_tmp, cert_path) != 0) {
        gchar *msg = g_strdup_printf("cert rename 실패: %s -> %s (%s)",
                                     cert_tmp, cert_path, g_strerror(errno));
        g_set_error_literal(error, g_quark_from_static_string("tls"), 5, msg);
        PCV_LOG_ERROR(TLS_LOG_DOM, "%s", msg);
        g_free(msg);
        goto cleanup;
    }
    if (g_rename(key_tmp, key_path) != 0) {
        gchar *msg = g_strdup_printf("key rename 실패: %s -> %s (%s)",
                                     key_tmp, key_path, g_strerror(errno));
        g_set_error_literal(error, g_quark_from_static_string("tls"), 6, msg);
        PCV_LOG_ERROR(TLS_LOG_DOM, "%s", msg);
        g_free(msg);
        goto cleanup;
    }
    if (g_rename(marker_tmp, marker) != 0) {
        gchar *msg = g_strdup_printf("marker rename 실패: %s -> %s (%s)",
                                     marker_tmp, marker, g_strerror(errno));
        g_set_error_literal(error, g_quark_from_static_string("tls"), 7, msg);
        PCV_LOG_ERROR(TLS_LOG_DOM, "%s", msg);
        g_free(msg);
        goto cleanup;
    }

    PCV_LOG_INFO(TLS_LOG_DOM, "자가서명 인증서 생성: %s (CN=%s, SAN=%s, 3650일)",
                 cert_path, host, san);
    success = TRUE;

cleanup:
                                                   
                                           
                                                      
    if (!success) {
        g_unlink(cert_tmp);
        g_unlink(key_tmp);
        g_unlink(marker_tmp);
    }
    g_free(errout); g_free(subj); g_free(san); g_free(san_ext);
    g_free(cert_tmp); g_free(key_tmp); g_free(marker); g_free(marker_tmp);
    return success;
}

   
                                                          
                                                     
                                                       
                                                     
                              
                              
  
                                                  
                                                  
                                              
                                            
                                                       
                                     
                                                              
                                                               
                                            
               
  
                                                 
                                                       
                                        
                            
  
                                           
  
                                                        
   
gboolean pcv_tls_cert_key_pair_ok(const gchar *cert_path, const gchar *key_path)
{
    if (!cert_path || !key_path) return FALSE;

    const gchar *cert_argv[] = { "openssl", "x509", "-noout", "-pubkey",
                                  "-in", cert_path, NULL };
    const gchar *key_argv[]  = { "openssl", "pkey", "-pubout",
                                  "-in", key_path, NULL };

                                                                
    gchar *cert_pub = NULL, *key_pub = NULL;
    gboolean cert_ok = pcv_spawn_sync_timeout(cert_argv, &cert_pub, NULL, 10, NULL);
    gboolean key_ok  = pcv_spawn_sync_timeout(key_argv,  &key_pub,  NULL, 10, NULL);

                                                          
    gboolean match = cert_ok && key_ok && cert_pub && key_pub &&
                     g_strcmp0(cert_pub, key_pub) == 0;

    g_free(cert_pub);
    g_free(key_pub);
    return match;
}

   
                                                     
                                                      
                              
  
                                            
                                                           
          
  
                                                  
   
gint64 pcv_tls_cert_expiry_days_path(const gchar *cert_path)
{
    if (!cert_path) return -1;
    GError *err = NULL;
    GTlsCertificate *cert = g_tls_certificate_new_from_file(cert_path, &err);
    if (!cert) { if (err) g_error_free(err); return -1; }
    GDateTime *not_after = g_tls_certificate_get_not_valid_after(cert);
    if (!not_after) { g_object_unref(cert); return -1; }
    GDateTime *now = g_date_time_new_now_utc();
    gint64 days = g_date_time_difference(not_after, now) / G_TIME_SPAN_DAY;
    g_date_time_unref(now); g_date_time_unref(not_after); g_object_unref(cert);
    return days;
}

                                                                 

   
                                                       
  
                                               
                                       
                                          
  
                                                  
   
gint64
pcv_tls_get_cert_expiry_days(void)
{
    return G.ctx ? pcv_tls_cert_expiry_days_path(G.ctx->cert_path) : -1;
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
                                                               
                                                   
    if (pcv_config_get_string("tls", "enabled", NULL))
        PCV_LOG_WARN(TLS_LOG_DOM,
                     "[tls] enabled 키는 2.0부터 무시됩니다 — TLS 상시 활성(ADR-0029)");

                                                                   
                                                       
                                                       
                                                        
                                                             
      
                                                   
                                                           
                                              
      
                                                           
                                                      
                                                              
                                                      
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

    const gchar *cert = pcv_config_get_string("tls", "cert", "/etc/purecvisor/pki/node.crt");
    const gchar *key  = pcv_config_get_string("tls", "key",  "/etc/purecvisor/pki/node.key");
    const gchar *ca   = pcv_config_get_string("tls", "ca",   "/etc/purecvisor/pki/ca.crt");

    GError *err = NULL;

                                                       
                                    
    gboolean cert_exists = g_file_test(cert, G_FILE_TEST_EXISTS);
    gboolean key_exists  = g_file_test(key,  G_FILE_TEST_EXISTS);

    if (cert_exists && key_exists) {                                                 
                                                 
                                               
                                                              
          
                                                  
                                                                      
                                                             
                                                       
                                                     
                                                                   
                                  
        gchar *marker = g_strdup_printf("%s.autogen", cert);
        if (g_file_test(marker, G_FILE_TEST_EXISTS)) {
            gboolean expiring = pcv_tls_cert_expiry_days_path(cert) < 30;
            gboolean pair_bad = !pcv_tls_cert_key_pair_ok(cert, key);
            if (expiring || pair_bad) {
                PCV_LOG_INFO(TLS_LOG_DOM,
                             "autogen 인증서 재생성 트리거 (만료임박/파손=%d, 쌍불일치=%d)",
                             expiring, pair_bad);
                if (!pcv_tls_autogen_selfsigned(cert, key, &err)) {
                    PCV_LOG_WARN(TLS_LOG_DOM, "재생성 실패: %s — 기존 인증서로 계속",
                                 err ? err->message : "unknown");
                    g_clear_error(&err);
                }
            }
        }
        g_free(marker);
    } else if (!cert_exists && !key_exists) {                                   
                                                       
        gchar *dir = g_path_get_dirname(cert);
        gboolean gen_ok = pcv_tls_pki_init(dir, &err) &&
                          pcv_tls_autogen_selfsigned(cert, key, &err);
        g_free(dir);
        if (!gen_ok) {
            PCV_LOG_ERROR(TLS_LOG_DOM, "자가서명 자동생성 실패: %s — TLS 셋업 실패(degraded)",
                          err ? err->message : "unknown");
            g_clear_error(&err);
            G.initialized = TRUE;
            return;
        }
    } else {                                                        
                                                    
                                                     
        PCV_LOG_ERROR(TLS_LOG_DOM,
                      "cert/key 중 하나만 존재(cert=%d key=%d) — 자동생성 거부, TLS 셋업 실패(degraded)",
                      cert_exists, key_exists);
        G.initialized = TRUE;
        return;
    }

    G.ctx = pcv_tls_ctx_new(cert, key, ca, &err);
    if (!G.ctx) {
                                                          
                                                     
        PCV_LOG_ERROR(TLS_LOG_DOM, "TLS 셋업 실패: %s — 외부 API 표면 차단(degraded)",
                      err ? err->message : "unknown");
        if (err) g_error_free(err);
    }
    G.initialized = TRUE;
}
