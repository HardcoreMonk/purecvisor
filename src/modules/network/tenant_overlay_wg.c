   
                            
                                                                 
  
                           
                                                   
                                                    
                                        
  
                                              
  
                                                 
                                                       
                                                    
                                                    
                                                    
  
               
                                                                     
                                                           
                                                             
                                                            
  
                      
                                                                               
                                                              
                                                                
                                                           
                                                     
                                                                      
                                                                   
                   
                                                                         
                                                                       
                                 
  
             
                                                            
                                                          
                                                            
                                                       
  
              
                                                                                    
                                               
   

#include "modules/network/tenant_overlay.h"
#include "utils/pcv_spawn.h"
#include "utils/pcv_secure.h"
#include "utils/pcv_log.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <string.h>
#include <unistd.h>

                                                           
#define TOVL_LOG_DOM         "TOVL"
                                                          
#define PCV_OVL_BRIDGE       "pcvovlbr"
                               
#define PCV_OVL_TIMEOUT_SEC  30u
                 
#define PCV_OVL_CAP_SECS     5
                                        
#define PCV_OVL_MARKER_MAX   16

                                                                 
                                                   
                          
static gboolean g_force_teardown_fail = FALSE;

                                               
                                                                        
                                              
  
                                              
                                                
                                                  
                                                
  
                                                
                                                    
                                            
void
pcv_tenant_overlay_wg_force_teardown_fail_for_test(gboolean enable)
{
    g_force_teardown_fail = enable;
}

                                                                            
        
                                                                               

                                                            
                                                                       
static gboolean
_run(const gchar *const *argv, gchar **out_opt, GError **error)
{
    gchar *serr = NULL;
    gboolean ok = pcv_spawn_sync_timeout(argv, out_opt, &serr,
                                         PCV_OVL_TIMEOUT_SEC, error);
    g_free(serr);
    return ok;
}

                                                       
static gboolean
_link_exists(const gchar *name)
{
    const gchar *argv[] = { "ip", "link", "show", name, NULL };
    return pcv_spawn_sync_timeout((const gchar *const *)argv,
                                  NULL, NULL, PCV_OVL_TIMEOUT_SEC, NULL);
}

                         
static gboolean
_ensure_bridge(GError **error)
{
    if (!_link_exists(PCV_OVL_BRIDGE)) {
        const gchar *add[] = { "ip", "link", "add", PCV_OVL_BRIDGE,
                               "type", "bridge", NULL };
        if (!_run((const gchar *const *)add, NULL, error))
            return FALSE;
    }
    const gchar *up[] = { "ip", "link", "set", PCV_OVL_BRIDGE, "up", NULL };
    return _run((const gchar *const *)up, NULL, error);
}

                                                       
                                                                 
                                                          
                                               
static gboolean
_teardown_absent_ok(const gchar *stderr_s)
{
    return stderr_s && (strstr(stderr_s, "No such file")                                        
                     || strstr(stderr_s, "Cannot open network namespace")
                     || strstr(stderr_s, "does not exist")
                     || strstr(stderr_s, "Cannot find device")                                 
                                                                      
                                                                
                                                             
                                                                    
                                                     
                     || strstr(stderr_s, "No such device"));
}

                                                    
  
                                                  
                                        
  
                                                     
                                                      
                                                        
                                                         
                                                      
                                                 
static gboolean
_endpoint_teardown(const gchar *ep, GError **error)
{
    if (g_force_teardown_fail) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "endpoint '%s' teardown 강제 실패(테스트 훅)", ep);
        return FALSE;
    }

    gboolean ok = TRUE;
    gchar *hveth = g_strdup_printf("%s-h", ep);

    gchar *serr = NULL;
    GError *nerr = NULL;
    const gchar *del_ns[] = { "ip", "netns", "del", ep, NULL };
    gboolean ns_ok = pcv_spawn_sync_timeout((const gchar *const *)del_ns,
                                            NULL, &serr, PCV_OVL_TIMEOUT_SEC, &nerr);
    if (!ns_ok) {
        const gchar *detail = (serr && *serr) ? serr : (nerr ? nerr->message : NULL);
        if (!_teardown_absent_ok(detail)) {
            PCV_LOG_WARN(TOVL_LOG_DOM,
                         "endpoint '%s' 정리 실패(dangling 가능): %s",
                         ep, detail ? detail : "unknown");
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "netns '%s' 삭제 실패: %s", ep, detail ? detail : "unknown");
            ok = FALSE;
        }
    }
    g_free(serr);
    g_clear_error(&nerr);

                                                         
    gchar *serr2 = NULL;
    const gchar *del_link[] = { "ip", "link", "del", hveth, NULL };
    gboolean l_ok = pcv_spawn_sync_timeout((const gchar *const *)del_link,
                                           NULL, &serr2, PCV_OVL_TIMEOUT_SEC, NULL);
    if (!l_ok && !_teardown_absent_ok(serr2)) {
        PCV_LOG_WARN(TOVL_LOG_DOM,
                     "endpoint '%s' host veth '%s' 정리 실패(dangling 가능): %s",
                     ep, hveth, (serr2 && *serr2) ? serr2 : "unknown");
        if (ok) {                                               
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "host veth '%s' 삭제 실패: %s",
                        hveth, (serr2 && *serr2) ? serr2 : "unknown");
        }
        ok = FALSE;
    }
    g_free(serr2);

    g_free(hveth);
    return ok;
}

                                                   
static gchar *
_marker_to_hex(const gchar *marker)
{
    gsize n = strlen(marker);
    if (n > PCV_OVL_MARKER_MAX)
        n = PCV_OVL_MARKER_MAX;
    GString *hex = g_string_sized_new(n * 2);
    for (gsize i = 0; i < n; i++)
        g_string_append_printf(hex, "%02x", (guint8)marker[i]);
    return g_string_free(hex, FALSE);
}

                                                                            
         
                                                                               

                                                                  
                                                                    
                                                              
                                                      
                                  
                                                       
                        
                                                
gboolean
pcv_tenant_overlay_wg_genkeys(gchar **privkey_out,
                              gchar **pubkey_out,
                              GError **error)
{
    g_return_val_if_fail(privkey_out != NULL && pubkey_out != NULL, FALSE);
    *privkey_out = NULL;
    *pubkey_out = NULL;

                                       
    gchar *raw_priv = NULL;
    const gchar *genkey[] = { "wg", "genkey", NULL };
    if (!_run((const gchar *const *)genkey, &raw_priv, error)) {
        pcv_secure_free_str(&raw_priv);                      
        return FALSE;
    }

    gchar *priv = g_strdup(raw_priv ? raw_priv : "");
    pcv_secure_free_str(&raw_priv);                          
    g_strstrip(priv);
    if (*priv == '\0') {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "wg genkey 가 빈 키를 반환했습니다");
        pcv_secure_free_str(&priv);                          
        return FALSE;
    }

                                                           
                                                           
    gchar *raw_pub = NULL, *pipe_err = NULL;
    const gchar *pubkey_argv[] = { "wg", "pubkey", NULL };
    gboolean ok = pcv_spawn_sync_stdin((const gchar *const *)pubkey_argv, priv, -1,
                                       &raw_pub, &pipe_err, error);
    g_free(pipe_err);

    if (!ok) {
        g_free(raw_pub);                                                
        pcv_secure_free_str(&priv);                          
        return FALSE;
    }

    gchar *pub = g_strdup(raw_pub ? raw_pub : "");
    g_free(raw_pub);                                                    
    g_strstrip(pub);
    if (*pub == '\0') {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "wg pubkey 가 빈 키를 반환했습니다");
        g_free(pub);                                                
        pcv_secure_free_str(&priv);                          
        return FALSE;
    }

    *privkey_out = priv;
    *pubkey_out = pub;
    return TRUE;
}

                                                              
                                                          
                                             
                                      
                                                     
             
                                                       
                                                   
                  
                                                               
gboolean
pcv_tenant_overlay_wg_endpoint_up(const char *ep_name,
                                  const char *privkey,
                                  const char *overlay_ip_cidr,
                                  guint listen_port,
                                  const char *transport_ip_cidr,
                                  GError **error)
{
    g_return_val_if_fail(ep_name && privkey && overlay_ip_cidr
                             && transport_ip_cidr, FALSE);

    if (!_ensure_bridge(error))
        return FALSE;

    gchar *hveth = g_strdup_printf("%s-h", ep_name);
    gchar *tveth = g_strdup_printf("%s-t", ep_name);
    gchar *wgif  = g_strdup_printf("wg-%s", ep_name);
    gchar *port  = g_strdup_printf("%u", listen_port);

                                                                   
                                             
    const gchar *s_addns[]   = { "ip", "netns", "add", ep_name, NULL };
    const gchar *s_addveth[] = { "ip", "link", "add", tveth,
                                 "type", "veth", "peer", "name", hveth, NULL };
    const gchar *s_tns[]     = { "ip", "link", "set", tveth, "netns", ep_name, NULL };
    const gchar *s_hmaster[] = { "ip", "link", "set", hveth,
                                 "master", PCV_OVL_BRIDGE, "up", NULL };
    const gchar *s_taddr[]   = { "ip", "netns", "exec", ep_name,
                                 "ip", "addr", "add", transport_ip_cidr, "dev", tveth, NULL };
    const gchar *s_tup[]     = { "ip", "netns", "exec", ep_name,
                                 "ip", "link", "set", tveth, "up", NULL };
    const gchar *s_loup[]    = { "ip", "netns", "exec", ep_name,
                                 "ip", "link", "set", "lo", "up", NULL };
    const gchar *s_wgadd[]   = { "ip", "netns", "exec", ep_name,
                                 "ip", "link", "add", wgif, "type", "wireguard", NULL };
    const gchar *s_wgaddr[]  = { "ip", "netns", "exec", ep_name,
                                 "ip", "addr", "add", overlay_ip_cidr, "dev", wgif, NULL };
    const gchar *s_wgup[]    = { "ip", "netns", "exec", ep_name,
                                 "ip", "link", "set", wgif, "up", NULL };

                                   
    const gchar *const *steps_pre[] = {
        (const gchar *const *)s_addns,
        (const gchar *const *)s_addveth,
        (const gchar *const *)s_tns,
        (const gchar *const *)s_hmaster,
        (const gchar *const *)s_taddr,
        (const gchar *const *)s_tup,
        (const gchar *const *)s_loup,
        (const gchar *const *)s_wgadd,
    };
                           
    const gchar *const *steps_post[] = {
        (const gchar *const *)s_wgaddr,
        (const gchar *const *)s_wgup,
    };

    gboolean ok = TRUE;
    for (gsize i = 0; i < G_N_ELEMENTS(steps_pre); i++) {
        if (!_run(steps_pre[i], NULL, error)) {
            ok = FALSE;
            break;
        }
    }

                                                                     
                                                           
                                                      
                                 
    if (ok) {
        const gchar *s_wgset[] = { "ip", "netns", "exec", ep_name,
                                   "wg", "set", wgif, "private-key", "/dev/stdin",
                                   "listen-port", port, NULL };
        gchar *set_err = NULL;
        ok = pcv_spawn_sync_stdin((const gchar *const *)s_wgset, privkey, -1,
                                  NULL, &set_err, error);
        g_free(set_err);
    }

    if (ok) {
        for (gsize i = 0; i < G_N_ELEMENTS(steps_post); i++) {
            if (!_run(steps_post[i], NULL, error)) {
                ok = FALSE;
                break;
            }
        }
    }

    if (!ok) {
                                                           
                                                               
                                                            
        (void)_endpoint_teardown(ep_name, NULL);
    }

    g_free(hveth); g_free(tveth); g_free(wgif); g_free(port);
    return ok;
}

                                                               
                                                                           
                                           
                                                  
                  
                                                       
gboolean
pcv_tenant_overlay_wg_peer_add(const char *ep_name,
                               const char *peer_pubkey,
                               const char *peer_overlay_ip,
                               const char *peer_endpoint,
                               GError **error)
{
    g_return_val_if_fail(ep_name && peer_pubkey && peer_overlay_ip
                             && peer_endpoint, FALSE);

    gchar *wgif    = g_strdup_printf("wg-%s", ep_name);
    gchar *allowed = g_strdup_printf("%s/32", peer_overlay_ip);

    const gchar *s_peer[] = { "ip", "netns", "exec", ep_name,
                              "wg", "set", wgif,
                              "peer", peer_pubkey,
                              "allowed-ips", allowed,
                              "endpoint", peer_endpoint, NULL };
    gboolean ok = _run((const gchar *const *)s_peer, NULL, error);

    if (ok) {
        const gchar *s_route[] = { "ip", "netns", "exec", ep_name,
                                   "ip", "route", "add", allowed, "dev", wgif, NULL };
        ok = _run((const gchar *const *)s_route, NULL, error);
    }

    g_free(wgif);
    g_free(allowed);
    return ok;
}

                                                                 
                                             
                                
                                    
                                                          
gboolean
pcv_tenant_overlay_wg_peer_remove(const char *ep_name,
                                  const char *peer_pubkey,
                                  const char *peer_overlay_ip,
                                  GError **error)
{
    g_return_val_if_fail(ep_name && peer_pubkey && peer_overlay_ip, FALSE);

    gchar *wgif    = g_strdup_printf("wg-%s", ep_name);
    gchar *allowed = g_strdup_printf("%s/32", peer_overlay_ip);

    const gchar *s_peer[] = { "ip", "netns", "exec", ep_name,
                              "wg", "set", wgif,
                              "peer", peer_pubkey, "remove", NULL };
    gboolean ok = _run((const gchar *const *)s_peer, NULL, error);

                                                              
                                           
    const gchar *s_route[] = { "ip", "netns", "exec", ep_name,
                               "ip", "route", "del", allowed, "dev", wgif, NULL };
    (void)pcv_spawn_sync_timeout((const gchar *const *)s_route,
                                 NULL, NULL, PCV_OVL_TIMEOUT_SEC, NULL);

    g_free(wgif);
    g_free(allowed);
    return ok;
}

                                                                         
                                                
                                                         
                                                              
gboolean
pcv_tenant_overlay_wg_endpoint_down(const char *ep_name,
                                    GError **error)
{
                                                               
                                                               
                                             
    if (!ep_name) return TRUE;
    return _endpoint_teardown(ep_name, error);
}

                                                                 
                                            
                                                       
                                           
GPtrArray *
pcv_tenant_overlay_wg_list_netns(GError **error)
{
    gchar *out = NULL;
    const gchar *argv[] = { "ip", "netns", "list", NULL };
    if (!_run((const gchar *const *)argv, &out, error)) {
        g_free(out);
        return NULL;
    }

    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    gchar **lines = g_strsplit(out ? out : "", "\n", -1);
    for (guint i = 0; lines && lines[i]; i++) {
        g_strstrip(lines[i]);
        if (!*lines[i]) continue;
        gchar **toks = g_strsplit(lines[i], " ", 2);
        if (toks[0] && *toks[0]) g_ptr_array_add(names, g_strdup(toks[0]));
        g_strfreev(toks);
    }
    g_strfreev(lines);
    g_free(out);
    return names;
}

                                                         
                                                            
                                                                         
                                                             
                                                                  
                                                           
                                                                   
                                                                           
static void
_attach_tap_rollback_besteffort(const gchar *ep, const gchar *tap,
                                const gchar *caller_pid)
{
                                                            
                                                                       
    gchar *serr = NULL;
    const gchar *back[] = { "ip", "netns", "exec", ep,
                           "ip", "link", "set", tap, "netns", caller_pid, NULL };
    gboolean back_ok = pcv_spawn_sync_timeout((const gchar *const *)back,
                                              NULL, &serr, PCV_OVL_TIMEOUT_SEC, NULL);
    if (!back_ok && !_teardown_absent_ok(serr)) {
        PCV_LOG_WARN(TOVL_LOG_DOM,
                     "attach_tap 롤백: tap '%s' 를 netns '%s' 밖으로 되돌리기 실패"
                     "(dangling 가능): %s", tap, ep, serr ? serr : "unknown");
    }
    g_free(serr);
}

                                                                  
                                                            
                                                        
                                                        
                                               
                                                       
                                                   
                                                                  
gboolean
pcv_tenant_overlay_wg_attach_tap(const char *ep_name,
                                 const char *tap_name,
                                 const char *overlay_ip,
                                 const char *overlay_subnet_cidr,
                                 GError **error)
{
    g_return_val_if_fail(ep_name && tap_name && overlay_ip
                             && overlay_subnet_cidr, FALSE);

    gchar *parp_key   = g_strdup_printf("net.ipv4.conf.%s.proxy_arp=1", tap_name);
    gchar *caller_pid = g_strdup_printf("%d", (int)getpid());
    gchar *wgif       = g_strdup_printf("wg-%s", ep_name);

                                                                      
                         
      
                                                     
                                                      
                                                    
                                                 
      
                                                                               
                                                                    
                                                                 
                                                    
                                                                     
                                                                           
                                                                        
                                                   
                                                     
      
                                                                   
                                                                        
                                                                          
                                                          
                                                                   
                                                        
                                                              
    {
        gchar *ov_cidr = g_strdup_printf("%s/32", overlay_ip);

                                                                         
        gchar *routes_out = NULL;
        const gchar *s_rshow[] = { "ip", "netns", "exec", ep_name,
                                   "ip", "-o", "route", "show", "dev", wgif, NULL };
        (void)pcv_spawn_sync_timeout((const gchar *const *)s_rshow,
                                     &routes_out, NULL, PCV_OVL_TIMEOUT_SEC, NULL);

                                                            
                                               
        const gchar *s_deladdr[] = { "ip", "netns", "exec", ep_name,
                                     "ip", "addr", "del", ov_cidr, "dev", wgif, NULL };
        (void)pcv_spawn_sync_timeout((const gchar *const *)s_deladdr,
                                     NULL, NULL, PCV_OVL_TIMEOUT_SEC, NULL);

                                                              
                                                         
                                                                     
                                                                          
                                                                    
                                          
        if (routes_out && *routes_out) {
            gchar **lines = g_strsplit(routes_out, "\n", -1);
            for (guint li = 0; lines && lines[li]; li++) {
                g_strstrip(lines[li]);
                if (!*lines[li]) continue;
                gchar **toks = g_strsplit(lines[li], " ", 2);
                if (toks[0] && *toks[0]) {
                    const gchar *s_radd[] = { "ip", "netns", "exec", ep_name,
                                              "ip", "route", "add", toks[0], "dev", wgif, NULL };
                    (void)pcv_spawn_sync_timeout((const gchar *const *)s_radd,
                                                 NULL, NULL, PCV_OVL_TIMEOUT_SEC, NULL);
                }
                g_strfreev(toks);
            }
            g_strfreev(lines);
        }
        g_free(routes_out);
        g_free(ov_cidr);
    }

                                                                
                                                      
                                                                      
                                                                   
                                                               
                                                                          
                                                                                   
                                                                                  
                                                             
                                                                      
    const gchar *s_move[]   = { "ip", "link", "set", tap_name, "netns", ep_name, NULL };
    const gchar *s_up[]     = { "ip", "netns", "exec", ep_name,
                               "ip", "link", "set", tap_name, "up", NULL };
    const gchar *s_fwd[]    = { "ip", "netns", "exec", ep_name,
                               "sysctl", "-w", "net.ipv4.ip_forward=1", NULL };
    const gchar *s_gwaddr[] = { "ip", "netns", "exec", ep_name,
                               "ip", "addr", "add", overlay_subnet_cidr, "dev", tap_name, NULL };
    const gchar *s_parp[]   = { "ip", "netns", "exec", ep_name,
                               "sysctl", "-w", parp_key, NULL };

    const gchar *const *steps[] = {
        (const gchar *const *)s_move,
        (const gchar *const *)s_up,
        (const gchar *const *)s_fwd,
        (const gchar *const *)s_gwaddr,
        (const gchar *const *)s_parp,
    };

    gboolean ok = TRUE;
    for (gsize i = 0; i < G_N_ELEMENTS(steps); i++) {
        if (!_run(steps[i], NULL, error)) {
            ok = FALSE;
            break;
        }
    }

    if (!ok) {
                                                                     
        _attach_tap_rollback_besteffort(ep_name, tap_name, caller_pid);
    }

    g_free(parp_key);
    g_free(caller_pid);
    g_free(wgif);
    return ok;
}

                                                             
                                                   
                                                 
                                                
                                                
                                                                        
gchar *
pcv_tenant_overlay_wg_capture_test(const char *bridge,
                                   const char *src_ep,
                                   const char *dst_overlay_ip,
                                   const char *marker,
                                   GError **error)
{
    g_return_val_if_fail(bridge && src_ep && dst_overlay_ip && marker, NULL);

                                                 
    gchar *pcap = g_build_filename(g_get_tmp_dir(), "pcvovlcapXXXXXX", NULL);
    gint fd = g_mkstemp(pcap);
    if (fd < 0) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                    "캡처 임시파일 생성 실패: %s", pcap);
        g_free(pcap);
        return NULL;
    }
    close(fd);

                                                                     
                                         
                                                                   
                                                                   
                                                               
    gchar *capsecs = g_strdup_printf("%d", PCV_OVL_CAP_SECS);
    const gchar *cap[] = { "timeout", capsecs,
                           "tcpdump", "-i", bridge, "-w", pcap,
                           "-U", "-n", "-Z", "root", NULL };
    pcv_spawn_fire((const gchar *const *)cap);
    g_usleep(900 * 1000);                               

                                                     
                                                    
                                                  
    gchar *hex = _marker_to_hex(marker);
    const gchar *png[] = { "ip", "netns", "exec", src_ep,
                           "ping", "-c", "6", "-i", "0.3", "-W", "1",
                           "-p", hex, dst_overlay_ip, NULL };
    (void)_run((const gchar *const *)png, NULL, NULL);
    g_free(hex);

                                                
    g_usleep((guint64)(PCV_OVL_CAP_SECS + 1) * 1000 * 1000);

    gchar *text = NULL;
    const gchar *rd[] = { "tcpdump", "-A", "-r", pcap, "-n", "-Z", "root", NULL };
    gboolean ok = _run((const gchar *const *)rd, &text, error);

    g_unlink(pcap);
    g_free(pcap);
    g_free(capsecs);

    if (!ok) {
        g_free(text);
        return NULL;
    }
    return text ? text : g_strdup("");
}
