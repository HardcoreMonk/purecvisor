                                                                                  
                                                                                                  
                                                                        
                                                                    
                                                                        
                              
  
                                                                               
                                                        
                                                                               
                                                        
                                                     
  
                                                            
                                                                      
                                            
                                                                               
   

#include <glib.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <json-glib/json-glib.h>

#include "modules/network/tenant_overlay.h"
#include "modules/network/network_dhcp.h"
#include "modules/security/security_store.h"
#include "modules/dispatcher/handler_tenant_overlay.h"
#include "utils/pcv_spawn.h"
#include "utils/pcv_validate.h"

                                             
                                                                         
                                               
                                                            
                                                       
gint pcv_tenant_overlay_count_members_for_test(const gchar *tenant);
void pcv_tenant_overlay_reset_for_test(void);

                                                
void pcv_tenant_overlay_wg_force_teardown_fail_for_test(gboolean enable);
gboolean pcv_tenant_overlay_slot_used_for_test(guint slot);
void pcv_tenant_overlay_set_slot_cap_for_test(guint cap);

                                     
static gboolean
_wg_available(void)
{
    const gchar *probe[] = { "wg", "--version", NULL };
    return pcv_spawn_sync_timeout((const gchar *const *)probe,
                                  NULL, NULL, 5, NULL);
}

                                                       
static guint
_count_substr(const gchar *hay, const gchar *needle)
{
    guint n = 0;
    gsize step = strlen(needle);
    for (const gchar *p = hay; (p = strstr(p, needle)) != NULL; p += step) n++;
    return n;
}

                                                                 
static gboolean
_busybox_udhcpc_available(void)
{
    const gchar *probe[] = { "busybox", "--list", NULL };
    gchar *out = NULL;
    gboolean ok = pcv_spawn_sync_timeout((const gchar *const *)probe,
                                         &out, NULL, 5, NULL);
    gboolean has = ok && out && (strstr(out, "udhcpc") != NULL);
    g_free(out);
    return has;
}

                                                
                                          
static void
test_overlay_ciphertext_only_on_bridge(void)
{
    if (geteuid() != 0) { g_test_skip("root(+netns) 필요"); return; }
    if (!_wg_available()) { g_test_skip("wg(wireguard-tools) 미설치"); return; }

    GError *err = NULL;

                                                         
    gchar *privA = NULL, *pubA = NULL, *privB = NULL, *pubB = NULL;
    g_assert_true(pcv_tenant_overlay_wg_genkeys(&privA, &pubA, &err));
    g_assert_no_error(err);
    g_assert_true(pcv_tenant_overlay_wg_genkeys(&privB, &pubB, &err));
    g_assert_no_error(err);

                                            
    (void)pcv_tenant_overlay_wg_endpoint_down("ovlA", NULL);
    (void)pcv_tenant_overlay_wg_endpoint_down("ovlB", NULL);

    g_assert_true(pcv_tenant_overlay_wg_endpoint_up(
        "ovlA", privA, "10.100.9.10/32", 51820, "172.31.0.10/24", &err));
    g_assert_no_error(err);
    g_assert_true(pcv_tenant_overlay_wg_endpoint_up(
        "ovlB", privB, "10.100.9.20/32", 51821, "172.31.0.20/24", &err));
    g_assert_no_error(err);

    g_assert_true(pcv_tenant_overlay_wg_peer_add(
        "ovlA", pubB, "10.100.9.20", "172.31.0.20:51821", &err));
    g_assert_no_error(err);
    g_assert_true(pcv_tenant_overlay_wg_peer_add(
        "ovlB", pubA, "10.100.9.10", "172.31.0.10:51820", &err));
    g_assert_no_error(err);

                                                    
                                                    
    const gchar *marker = "PCVSECRETMARKER";
    gchar *cap = pcv_tenant_overlay_wg_capture_test(
        "pcvovlbr", "ovlA", "10.100.9.20", marker, &err);
    g_assert_no_error(err);
    g_assert_nonnull(cap);
    g_assert_null(strstr(cap, marker));                             
    g_assert_nonnull(strstr(cap, "51820"));                               
    g_free(cap);

    (void)pcv_tenant_overlay_wg_endpoint_down("ovlA", NULL);
    (void)pcv_tenant_overlay_wg_endpoint_down("ovlB", NULL);
    g_free(privA); g_free(pubA); g_free(privB); g_free(pubB);
}

                                                                               
                                                                          
                                                                               
                                                       
                                                    
                                                              
                                  
  
                                                      
                                
                                                                  
                                       
                                                                
  
                                         
                                                                          
                     
                                                                               
                                                               
                              
                                                  
  
                                                            
                                                                               
   
static void
test_overlay_guest_confinement(void)
{
    if (geteuid() != 0) { g_test_skip("root(+netns) 필요"); return; }
    if (!_wg_available()) { g_test_skip("wg(wireguard-tools) 미설치"); return; }

    GError *err = NULL;
    const gchar *epA = "ovlga";                             
    const gchar *epB = "ovlgb";                    
    const gchar *gns = "ovlgg";                         
    const gchar *marker = "PCVGUESTMARKER";                                     

                                                                   
                                                     
    (void)pcv_tenant_overlay_wg_endpoint_down(epA, NULL);
    (void)pcv_tenant_overlay_wg_endpoint_down(epB, NULL);
    const gchar *pre_delg[] = { "ip", "netns", "del", gns, NULL };
    (void)pcv_spawn_sync_timeout((const gchar *const *)pre_delg, NULL, NULL, 5, NULL);
    const gchar *pre_delv[] = { "ip", "link", "del", "veth-tap", NULL };
    (void)pcv_spawn_sync_timeout((const gchar *const *)pre_delv, NULL, NULL, 5, NULL);

                                   
    gchar *privA = NULL, *pubA = NULL, *privB = NULL, *pubB = NULL;
    g_assert_true(pcv_tenant_overlay_wg_genkeys(&privA, &pubA, &err));
    g_assert_no_error(err);
    g_assert_true(pcv_tenant_overlay_wg_genkeys(&privB, &pubB, &err));
    g_assert_no_error(err);

                                                      
    g_assert_true(pcv_tenant_overlay_wg_endpoint_up(
        epA, privA, "10.100.9.1/32", 51820, "172.31.0.30/16", &err));
    g_assert_no_error(err);
                                             
    g_assert_true(pcv_tenant_overlay_wg_endpoint_up(
        epB, privB, "10.100.9.3/32", 51821, "172.31.0.40/16", &err));
    g_assert_no_error(err);

                                                                     
                                                       
                                    
    g_assert_true(pcv_tenant_overlay_wg_peer_add(
        epA, pubB, "10.100.9.3", "172.31.0.40:51821", &err));
    g_assert_no_error(err);
    g_assert_true(pcv_tenant_overlay_wg_peer_add(
        epB, pubA, "10.100.9.2", "172.31.0.30:51820", &err));
    g_assert_no_error(err);

                                                                        
    const gchar *g_addns[] = { "ip", "netns", "add", gns, NULL };
    g_assert_true(pcv_spawn_sync_timeout((const gchar *const *)g_addns, NULL, NULL, 10, NULL));
    const gchar *g_addveth[] = { "ip", "link", "add", "veth-guest",
                                 "type", "veth", "peer", "name", "veth-tap", NULL };
    g_assert_true(pcv_spawn_sync_timeout((const gchar *const *)g_addveth, NULL, NULL, 10, NULL));
    const gchar *g_gns[] = { "ip", "link", "set", "veth-guest", "netns", gns, NULL };
    g_assert_true(pcv_spawn_sync_timeout((const gchar *const *)g_gns, NULL, NULL, 10, NULL));
    const gchar *g_gaddr[] = { "ip", "netns", "exec", gns,
                               "ip", "addr", "add", "10.100.9.2/24", "dev", "veth-guest", NULL };
    g_assert_true(pcv_spawn_sync_timeout((const gchar *const *)g_gaddr, NULL, NULL, 10, NULL));
    const gchar *g_gup[] = { "ip", "netns", "exec", gns,
                             "ip", "link", "set", "veth-guest", "up", NULL };
    g_assert_true(pcv_spawn_sync_timeout((const gchar *const *)g_gup, NULL, NULL, 10, NULL));
    const gchar *g_glo[] = { "ip", "netns", "exec", gns,
                             "ip", "link", "set", "lo", "up", NULL };
    g_assert_true(pcv_spawn_sync_timeout((const gchar *const *)g_glo, NULL, NULL, 10, NULL));
    const gchar *g_grt[] = { "ip", "netns", "exec", gns,
                             "ip", "route", "add", "default", "via", "10.100.9.1", NULL };
    g_assert_true(pcv_spawn_sync_timeout((const gchar *const *)g_grt, NULL, NULL, 10, NULL));

                                                              
                                                                           
                                                                           
                                                                      
                                                 
    g_assert_true(pcv_tenant_overlay_wg_attach_tap(
        epA, "veth-tap", "10.100.9.2", "10.100.9.1/24", &err));
    g_assert_no_error(err);

                                                              
                                                              
                                                            
    gchar *wgifB = g_strdup_printf("wg-%s", epB);
    gchar *wgb_pcap = g_build_filename(g_get_tmp_dir(), "pcvgwbcapXXXXXX", NULL);
    gint wfd = g_mkstemp(wgb_pcap);
    g_assert_cmpint(wfd, >=, 0);
    close(wfd);
    const gchar *wgb_cap[] = { "timeout", "8", "ip", "netns", "exec", epB,
                               "tcpdump", "-i", wgifB, "-w", wgb_pcap,
                               "-U", "-n", "-Z", "root", NULL };
    pcv_spawn_fire((const gchar *const *)wgb_cap);
    g_usleep(300 * 1000);                                                             

                                                                 
    gchar *cap = pcv_tenant_overlay_wg_capture_test(
        "pcvovlbr", gns, "10.100.9.3", marker, &err);
    g_assert_no_error(err);
    g_assert_nonnull(cap);
    g_assert_null(strstr(cap, marker));                                       
    g_assert_nonnull(strstr(cap, "51820"));                            
    g_free(cap);

                                                           
    g_usleep(1200 * 1000);
    gchar *wgb_text = NULL;
    const gchar *wgb_rd[] = { "tcpdump", "-A", "-r", wgb_pcap, "-n", "-Z", "root", NULL };
    g_assert_true(pcv_spawn_sync_timeout((const gchar *const *)wgb_rd,
                                         &wgb_text, NULL, 15, &err));
    g_assert_no_error(err);
    g_assert_nonnull(wgb_text);
                                                           
    g_assert_nonnull(strstr(wgb_text, marker));
    g_free(wgb_text);
    g_unlink(wgb_pcap);
    g_free(wgb_pcap);
    g_free(wgifB);

                                                               
    (void)pcv_tenant_overlay_wg_endpoint_down(epA, NULL);
    (void)pcv_tenant_overlay_wg_endpoint_down(epB, NULL);
    const gchar *post_delg[] = { "ip", "netns", "del", gns, NULL };
    (void)pcv_spawn_sync_timeout((const gchar *const *)post_delg, NULL, NULL, 5, NULL);

    g_free(privA); g_free(pubA); g_free(privB); g_free(pubB);
}

                                                                               
                                                                                
                                                                               
                                              
                                                                 
                                                         
                                              
  
                                                        
                                                                   
                     
                                                                               
  
        
                                                                     
                                                                           
                                                                  
                                                        
  
                                                                             
                                                                               
   
static void
test_overlay_dhcp(void)
{
    if (geteuid() != 0) { g_test_skip("root(+netns) 필요"); return; }
    if (!_wg_available()) { g_test_skip("wg(wireguard-tools) 미설치"); return; }
    if (!_busybox_udhcpc_available()) { g_test_skip("busybox udhcpc 미설치"); return; }

    GError *err = NULL;
    const gchar *ep    = "ovldh";                            
    const gchar *gns   = "ovldhg";                                
    const gchar *tapif = "veth-odt";                                
    const gchar *gif   = "veth-odg";                            
    const gchar *gmac  = "02:00:00:00:09:50";
    const gchar *gw    = "10.100.9.1/24";                              
    const gchar *oip   = "10.100.9.50";                     

                                                                               
    (void)network_dhcp_release_overlay_ip(ep, NULL);
    (void)pcv_tenant_overlay_wg_endpoint_down(ep, NULL);
    const gchar *pre_delg[] = { "ip", "netns", "del", gns, NULL };
    (void)pcv_spawn_sync_timeout((const gchar *const *)pre_delg, NULL, NULL, 5, NULL);
    const gchar *pre_delv[] = { "ip", "link", "del", tapif, NULL };
    (void)pcv_spawn_sync_timeout((const gchar *const *)pre_delv, NULL, NULL, 5, NULL);

                                                        
    gchar *priv = NULL, *pub = NULL;
    g_assert_true(pcv_tenant_overlay_wg_genkeys(&priv, &pub, &err));
    g_assert_no_error(err);
    g_assert_true(pcv_tenant_overlay_wg_endpoint_up(
        ep, priv, "10.100.9.1/32", 51820, "172.31.0.30/16", &err));
    g_assert_no_error(err);

                                                          
                                                                   
    const gchar *g_addns[] = { "ip", "netns", "add", gns, NULL };
    g_assert_true(pcv_spawn_sync_timeout((const gchar *const *)g_addns, NULL, NULL, 10, NULL));
    const gchar *g_addveth[] = { "ip", "link", "add", gif,
                                 "type", "veth", "peer", "name", tapif, NULL };
    g_assert_true(pcv_spawn_sync_timeout((const gchar *const *)g_addveth, NULL, NULL, 10, NULL));
    const gchar *g_gns[] = { "ip", "link", "set", gif, "netns", gns, NULL };
    g_assert_true(pcv_spawn_sync_timeout((const gchar *const *)g_gns, NULL, NULL, 10, NULL));
    const gchar *g_gmac[] = { "ip", "netns", "exec", gns,
                              "ip", "link", "set", gif, "address", gmac, NULL };
    g_assert_true(pcv_spawn_sync_timeout((const gchar *const *)g_gmac, NULL, NULL, 10, NULL));
    const gchar *g_gup[] = { "ip", "netns", "exec", gns,
                             "ip", "link", "set", gif, "up", NULL };
    g_assert_true(pcv_spawn_sync_timeout((const gchar *const *)g_gup, NULL, NULL, 10, NULL));
    const gchar *g_glo[] = { "ip", "netns", "exec", gns,
                             "ip", "link", "set", "lo", "up", NULL };
    g_assert_true(pcv_spawn_sync_timeout((const gchar *const *)g_glo, NULL, NULL, 10, NULL));

                                                          
    g_assert_true(pcv_tenant_overlay_wg_attach_tap(ep, tapif, oip, gw, &err));
    g_assert_no_error(err);

                                                       
    g_assert_true(network_dhcp_reserve_overlay_ip(ep, tapif, gw, gmac, oip, &err));
    g_assert_no_error(err);

                                                              
    gchar *conf_path = g_strdup_printf(
        PCV_NETWORK_RUNDIR "/dnsmasq-ovl-%s.conf", ep);
    gchar *pid_path = g_strdup_printf(
        PCV_NETWORK_RUNDIR "/dnsmasq-ovl-%s.pid", ep);
    gchar *conf_txt = NULL;
    g_assert_true(g_file_get_contents(conf_path, &conf_txt, NULL, &err));
    g_assert_no_error(err);
    g_assert_nonnull(strstr(conf_txt, "dhcp-option-force=26,1420"));
    g_assert_nonnull(strstr(conf_txt, "dhcp-host=02:00:00:00:09:50,10.100.9.50"));
    g_free(conf_txt);

                                                                            
    gchar *pcap = g_build_filename(g_get_tmp_dir(), "pcvovldhcpXXXXXX", NULL);
    gint pfd = g_mkstemp(pcap);
    g_assert_cmpint(pfd, >=, 0);
    close(pfd);
    const gchar *capa[] = { "timeout", "8", "ip", "netns", "exec", ep,
                            "tcpdump", "-i", tapif, "-w", pcap, "-U", "-n",
                            "-Z", "root", "udp port 67 or udp port 68", NULL };
    pcv_spawn_fire((const gchar *const *)capa);
    g_usleep(500 * 1000);                       

                                                                      
                                                   
    gchar *script = g_build_filename(g_get_tmp_dir(), "pcvudhcpcXXXXXX", NULL);
    gint sfd = g_mkstemp(script);
    g_assert_cmpint(sfd, >=, 0);
    close(sfd);
    const gchar *script_body =
        "#!/bin/sh\n"
        "case \"$1\" in\n"
        "  bound|renew) ip addr add \"$ip/24\" dev \"$interface\" ;;\n"
        "esac\n"
        "exit 0\n";
    g_assert_true(g_file_set_contents(script, script_body, -1, &err));
    g_assert_no_error(err);
    g_assert_cmpint(g_chmod(script, 0755), ==, 0);

                              
    const gchar *dhc[] = { "ip", "netns", "exec", gns,
                           "busybox", "udhcpc", "-i", gif,
                           "-q", "-f", "-n", "-t", "5", "-s", script, NULL };
    gchar *dh_out = NULL, *dh_err = NULL;
    gboolean got = pcv_spawn_sync_timeout((const gchar *const *)dhc,
                                          &dh_out, &dh_err, 30, &err);
    g_assert_no_error(err);
    g_assert_true(got);
    g_free(dh_out); g_free(dh_err);

                                                
    gchar *addr_out = NULL;
    const gchar *showa[] = { "ip", "netns", "exec", gns,
                             "ip", "-4", "addr", "show", gif, NULL };
    g_assert_true(pcv_spawn_sync_timeout((const gchar *const *)showa,
                                         &addr_out, NULL, 10, &err));
    g_assert_no_error(err);
    g_assert_nonnull(addr_out);
    g_assert_nonnull(strstr(addr_out, "10.100.9.50"));
    g_test_message("assert(1) guest 'veth-odg' obtained reserved overlay IP 10.100.9.50 via DHCP");
    g_free(addr_out);

                                                                 
                                           
    g_usleep(1200 * 1000);
    gchar *pcap_txt = NULL;
    const gchar *rd[] = { "tcpdump", "-vv", "-r", pcap, "-n", NULL };
    g_assert_true(pcv_spawn_sync_timeout((const gchar *const *)rd,
                                         &pcap_txt, NULL, 15, &err));
    g_assert_no_error(err);
    g_assert_nonnull(pcap_txt);
    g_assert_nonnull(strstr(pcap_txt, "MTU (26), length 2: 1420"));
    g_test_message("assert(2) DHCP OFFER/ACK carried 'MTU (26), length 2: 1420' (server-side, client-independent)");
    g_free(pcap_txt);

                                                                
    g_assert_true(network_dhcp_release_overlay_ip(ep, &err));
    g_assert_no_error(err);
    g_assert_false(g_file_test(conf_path, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(pid_path, G_FILE_TEST_EXISTS));
    g_assert_true(network_dhcp_release_overlay_ip(ep, &err));                  
    g_assert_no_error(err);
    g_test_message("assert(3) release_overlay_ip: dnsmasq stopped + conf/pid removed, idempotent re-call OK");

            
    g_unlink(pcap);   g_free(pcap);
    g_unlink(script); g_free(script);
    g_free(conf_path); g_free(pid_path);
    (void)pcv_tenant_overlay_wg_endpoint_down(ep, NULL);
    const gchar *post_delg[] = { "ip", "netns", "del", gns, NULL };
    (void)pcv_spawn_sync_timeout((const gchar *const *)post_delg, NULL, NULL, 5, NULL);
    g_free(priv); g_free(pub);
}

                                                                               
                                                                   
                                                                               
                                                        
                                                                               
   
static void
test_overlay_ip_alloc(void)
{
    GError *e = NULL;

                                                                
                                                        
                                                          
                                        
    g_assert_true(pcv_security_store_open(":memory:"));
    pcv_tenant_overlay_reset_for_test();

    g_assert_true(pcv_tenant_overlay_create("t1", &e)); g_assert_no_error(e);
    g_assert_true(pcv_tenant_overlay_create("t2", &e)); g_assert_no_error(e);

    gchar *s1 = NULL, *s2 = NULL;
    pcv_tenant_overlay_get_subnet("t1", &s1);
    pcv_tenant_overlay_get_subnet("t2", &s2);
    g_assert_cmpstr(s1, !=, s2);

    gchar *a = pcv_tenant_overlay_alloc_ip("t1", &e);
    gchar *b = pcv_tenant_overlay_alloc_ip("t1", &e);
    g_assert_cmpstr(a, !=, b);
    g_assert_true(g_str_has_prefix(a, "10.100."));

    pcv_tenant_overlay_free_ip("t1", a);
    gchar *c = pcv_tenant_overlay_alloc_ip("t1", &e);
    g_assert_cmpstr(c, ==, a);          

    g_free(a); g_free(b); g_free(c); g_free(s1); g_free(s2);

    g_assert_true(pcv_tenant_overlay_delete("t1", &e));
    g_assert_true(pcv_tenant_overlay_delete("t2", &e));

    pcv_tenant_overlay_reset_for_test();
    pcv_security_store_close();
}

                                                                               
                                                                            
           
                                                                               
                                                                   
                                                                               
   
typedef struct {
    gchar *tmpdir;
    gchar *dbpath;
    GError *error;
    gboolean store_open;
} OverlaySecurityStoreFixture;

static void
overlay_security_store_fixture_setup(OverlaySecurityStoreFixture *fixture,
                                     const gchar *tmp_template)
{
    fixture->tmpdir = g_dir_make_tmp(tmp_template, &fixture->error);
    if (!fixture->tmpdir) {
        g_test_fail_printf("temp directory creation failed: %s",
                           fixture->error->message);
        return;
    }
    fixture->dbpath = g_build_filename(fixture->tmpdir, "pcv_security.db", NULL);
    fixture->store_open = pcv_security_store_open(fixture->dbpath);
    if (!fixture->store_open)
        g_test_fail_printf("temporary security store open failed");
}

static gboolean
overlay_fixture_remove(const gchar *path)
{
    if (path && g_unlink(path) != 0 && errno != ENOENT) {
        g_test_message("cleanup failed for %s: %s", path, g_strerror(errno));
        return FALSE;
    }
    return TRUE;
}

static gboolean
overlay_security_store_fixture_cleanup(OverlaySecurityStoreFixture *fixture)
{
    gboolean cleanup_ok = TRUE;
    if (fixture->store_open)
        pcv_security_store_close();

    gchar *wal = fixture->dbpath
        ? g_strdup_printf("%s-wal", fixture->dbpath) : NULL;
    gchar *shm = fixture->dbpath
        ? g_strdup_printf("%s-shm", fixture->dbpath) : NULL;
    cleanup_ok &= overlay_fixture_remove(fixture->dbpath);
    cleanup_ok &= overlay_fixture_remove(wal);
    cleanup_ok &= overlay_fixture_remove(shm);
    if (fixture->tmpdir && g_rmdir(fixture->tmpdir) != 0 && errno != ENOENT) {
        g_test_message("cleanup failed for %s: %s",
                       fixture->tmpdir, g_strerror(errno));
        cleanup_ok = FALSE;
    }

    g_clear_error(&fixture->error);
    g_free(wal);
    g_free(shm);
    g_free(fixture->dbpath);
    g_free(fixture->tmpdir);
    return cleanup_ok;
}

typedef struct {
    OverlaySecurityStoreFixture store;
    gchar *pub;
    gchar *priv;
    gchar *enc;
} OverlayKeyFixture;

static void
overlay_key_fixture_setup(OverlayKeyFixture *fixture, gconstpointer user_data)
{
    (void)user_data;
    overlay_security_store_fixture_setup(
        &fixture->store, "pcv-overlay-key-roundtrip-XXXXXX");
}

static void
overlay_key_fixture_teardown(OverlayKeyFixture *fixture, gconstpointer user_data)
{
    (void)user_data;
    g_free(fixture->pub);
    g_free(fixture->priv);
    g_free(fixture->enc);
    if (!overlay_security_store_fixture_cleanup(&fixture->store))
        g_test_fail();
}

static void
test_overlay_key_roundtrip(OverlayKeyFixture *fixture, gconstpointer user_data)
{
    (void)user_data;
    if (geteuid()!=0){g_test_skip("wg genkey root 필요");return;}
    if (!fixture->store.store_open)
        return;

    if (!pcv_tenant_overlay_gen_and_store_key(
            "t1", "vmA", "10.100.1.10", &fixture->pub,
            &fixture->store.error)) {
        g_test_fail_printf("key generation/storage failed: %s",
                           fixture->store.error
                               ? fixture->store.error->message : "unknown error");
        return;
    }
    if (!fixture->pub) {
        g_test_fail_printf("public key is NULL");
        return;
    }
    if (!pcv_tenant_overlay_load_privkey(
            "t1", "vmA", &fixture->priv, &fixture->store.error)) {
        g_test_fail_printf("private-key load failed: %s",
                           fixture->store.error
                               ? fixture->store.error->message : "unknown error");
        return;
    }
    if (!fixture->priv) {
        g_test_fail_printf("decrypted private key is NULL");
        return;
    }
                                                             
                                                                            
    if (!pcv_security_store_get_wg_key(
            "t1", "vmA", NULL, &fixture->enc, NULL,
            &fixture->store.error)) {
        g_test_fail_printf("stored-key lookup failed: %s",
                           fixture->store.error
                               ? fixture->store.error->message : "unknown error");
        return;
    }
    if (!g_str_has_prefix(fixture->enc, "ENC2:")) {
        g_test_fail_printf("stored private key lacks ENC2 prefix");
        return;
    }
    if (!pcv_security_store_del_wg_key(
            "t1", "vmA", &fixture->store.error))
        g_test_fail_printf("stored-key deletion failed: %s",
                           fixture->store.error
                               ? fixture->store.error->message : "unknown error");
}

                                                                               
                                                              
                                                                               
                                                                
                                                         
                                                          
                                                          
  
                                                            
                                                                               
   
typedef struct {
    OverlaySecurityStoreFixture store;
    gchar *ip_a;
    gchar *ip_b;
    gchar *ep_a;
    gchar *ep_b;
    gchar *wgif_a;
    gchar *wgif_b;
    gchar *dump_a;
    gchar *dump_b;
    gchar *dump_b2;
    gchar *capture;
} OverlayMeshFixture;

typedef struct {
    OverlaySecurityStoreFixture store;
    gchar *ip_a;
    gchar *ip_b;
    gchar *ip_c;
    gchar *ep_a;
    gchar *ep_b;
    gchar *ep_c;
    gchar *wgif_a;
    gchar *wgif_b;
    gchar *wgif_c;
    gchar *dump_a;
    gchar *dump_b;
    gchar *dump_c;
    gchar *nspath;
    gchar *stale_ep;
    gboolean immutable_applied;
} OverlayRootFixture;

static gboolean overlay_mesh_fixture_endpoint_down(const gchar *ep_name);

static void
overlay_root_fixture_setup(OverlayRootFixture *fixture,
                           gconstpointer user_data)
{
    overlay_security_store_fixture_setup(&fixture->store,
                                         (const gchar *)user_data);
}

static gboolean
overlay_root_fixture_clear_immutable(OverlayRootFixture *fixture)
{
    if (!fixture->immutable_applied)
        return TRUE;

    const gchar *argv[] = { "chattr", "-i", fixture->nspath, NULL };
    gboolean ok = pcv_spawn_sync_timeout((const gchar *const *)argv,
                                          NULL, NULL, 5, NULL);
    if (!ok)
        g_test_message("cleanup failed to clear immutable flag from %s",
                       fixture->nspath);
    else
        fixture->immutable_applied = FALSE;
    return ok;
}

static void
overlay_root_fixture_teardown(OverlayRootFixture *fixture,
                              gconstpointer user_data)
{
    (void)user_data;
    gboolean cleanup_ok = TRUE;
    gboolean immutable_ok = overlay_root_fixture_clear_immutable(fixture);
    gboolean immutable_endpoint_ok = TRUE;

    if (fixture->ep_a)
        immutable_endpoint_ok =
            overlay_mesh_fixture_endpoint_down(fixture->ep_a);
    cleanup_ok &= immutable_endpoint_ok;
                                                           
                                               
                                      
    if (!immutable_ok && !immutable_endpoint_ok)
        cleanup_ok = FALSE;
    if (fixture->ep_b)
        cleanup_ok &= overlay_mesh_fixture_endpoint_down(fixture->ep_b);
    if (fixture->ep_c)
        cleanup_ok &= overlay_mesh_fixture_endpoint_down(fixture->ep_c);
    if (fixture->stale_ep)
        cleanup_ok &= overlay_mesh_fixture_endpoint_down(fixture->stale_ep);
    pcv_tenant_overlay_reset_for_test();
    cleanup_ok &= overlay_security_store_fixture_cleanup(&fixture->store);

    g_free(fixture->stale_ep);
    g_free(fixture->nspath);
    g_free(fixture->dump_c);
    g_free(fixture->dump_b);
    g_free(fixture->dump_a);
    g_free(fixture->wgif_c);
    g_free(fixture->wgif_b);
    g_free(fixture->wgif_a);
    g_free(fixture->ep_c);
    g_free(fixture->ep_b);
    g_free(fixture->ep_a);
    g_free(fixture->ip_c);
    g_free(fixture->ip_b);
    g_free(fixture->ip_a);
    g_clear_error(&fixture->store.error);
    if (!cleanup_ok)
        g_test_fail();
}

static void
overlay_mesh_fixture_setup(OverlayMeshFixture *fixture, gconstpointer user_data)
{
    (void)user_data;
    overlay_security_store_fixture_setup(
        &fixture->store, "pcv-overlay-attach-mesh-XXXXXX");
}

static gboolean
overlay_mesh_fixture_endpoint_down(const gchar *ep_name)
{
    GError *error = NULL;
    gboolean ok = pcv_tenant_overlay_wg_endpoint_down(ep_name, &error);
    if (!ok)
        g_test_message("cleanup failed for owned endpoint %s: %s",
                       ep_name,
                       error ? error->message : "unknown error");
    g_clear_error(&error);
    return ok;
}

static void
overlay_mesh_fixture_teardown(OverlayMeshFixture *fixture,
                              gconstpointer user_data)
{
    (void)user_data;
    gboolean cleanup_ok = TRUE;

                      
                                                    
                                                  
                                                         
                                
      
                     
                                                 
                                             
       
    if (fixture->ep_a)
        cleanup_ok &= overlay_mesh_fixture_endpoint_down(fixture->ep_a);
    if (fixture->ep_b)
        cleanup_ok &= overlay_mesh_fixture_endpoint_down(fixture->ep_b);
    pcv_tenant_overlay_reset_for_test();
    cleanup_ok &= overlay_security_store_fixture_cleanup(&fixture->store);

    g_free(fixture->capture);
    g_free(fixture->dump_b2);
    g_free(fixture->dump_b);
    g_free(fixture->dump_a);
    g_free(fixture->wgif_b);
    g_free(fixture->wgif_a);
    g_free(fixture->ep_b);
    g_free(fixture->ep_a);
    g_free(fixture->ip_b);
    g_free(fixture->ip_a);
    if (!cleanup_ok)
        g_test_fail();
}

static void
test_overlay_attach_detach_mesh(OverlayMeshFixture *fixture,
                                gconstpointer user_data)
{
    (void)user_data;
    if (geteuid() != 0) { g_test_skip("root(+netns) 필요"); return; }
    if (!_wg_available()) { g_test_skip("wg(wireguard-tools) 미설치"); return; }
    if (!fixture->store.store_open)
        return;

    if (!pcv_tenant_overlay_create("tmesh", &fixture->store.error) ||
        fixture->store.error) {
        g_test_fail_printf("tenant create failed: %s",
                           fixture->store.error
                               ? fixture->store.error->message : "unknown error");
        return;
    }

                                                     
    fixture->ip_a = pcv_tenant_overlay_attach_vm(
        "tmesh", "vmA", &fixture->store.error);
    if (!fixture->ip_a || fixture->store.error) {
        g_test_fail_printf("vmA attach failed: %s",
                           fixture->store.error
                               ? fixture->store.error->message : "unknown error");
        return;
    }
    if (!pcv_tenant_overlay_get_member_ep("tmesh", "vmA", &fixture->ep_a) ||
        !fixture->ep_a) {
        g_test_fail_printf("vmA endpoint lookup failed");
        return;
    }
    fixture->ip_b = pcv_tenant_overlay_attach_vm(
        "tmesh", "vmB", &fixture->store.error);
    if (!fixture->ip_b || fixture->store.error) {
        g_test_fail_printf("vmB attach failed: %s",
                           fixture->store.error
                               ? fixture->store.error->message : "unknown error");
        return;
    }
    if (!pcv_tenant_overlay_get_member_ep("tmesh", "vmB", &fixture->ep_b) ||
        !fixture->ep_b) {
        g_test_fail_printf("vmB endpoint lookup failed");
        return;
    }
    if (g_strcmp0(fixture->ip_a, fixture->ip_b) == 0) {
        g_test_fail_printf("attached VMs received the same overlay IP");
        return;
    }

                                                          
    fixture->wgif_a = g_strdup_printf("wg-%s", fixture->ep_a);
    fixture->wgif_b = g_strdup_printf("wg-%s", fixture->ep_b);
    const gchar *show_a[] = {
        "ip", "netns", "exec", fixture->ep_a,
        "wg", "show", fixture->wgif_a, NULL
    };
    const gchar *show_b[] = {
        "ip", "netns", "exec", fixture->ep_b,
        "wg", "show", fixture->wgif_b, NULL
    };
    if (!pcv_spawn_sync_timeout((const gchar *const *)show_a,
                                &fixture->dump_a, NULL, 10, NULL) ||
        !fixture->dump_a || !strstr(fixture->dump_a, "peer:")) {
        g_test_fail_printf("vmA does not show vmB as a peer");
        return;
    }
    if (!pcv_spawn_sync_timeout((const gchar *const *)show_b,
                                &fixture->dump_b, NULL, 10, NULL) ||
        !fixture->dump_b || !strstr(fixture->dump_b, "peer:")) {
        g_test_fail_printf("vmB does not show vmA as a peer");
        return;
    }

                                                          
                                                 
                                            
    const gchar *marker = "PCVATTACHMARKER";
    fixture->capture = pcv_tenant_overlay_wg_capture_test(
        "pcvovlbr", fixture->ep_a, fixture->ip_b, marker,
        &fixture->store.error);
    if (!fixture->capture || fixture->store.error) {
        g_test_fail_printf("bridge capture failed: %s",
                           fixture->store.error
                               ? fixture->store.error->message : "unknown error");
        return;
    }
    if (strstr(fixture->capture, marker)) {
        g_test_fail_printf("plaintext marker found on transport bridge");
        return;
    }
    if (!strstr(fixture->capture, "51820")) {
        g_test_fail_printf("WireGuard UDP traffic absent from bridge capture");
        return;
    }

                                                  
    if (!pcv_tenant_overlay_detach_vm(
            "tmesh", "vmA", &fixture->store.error) ||
        fixture->store.error) {
        g_test_fail_printf("vmA detach failed: %s",
                           fixture->store.error
                               ? fixture->store.error->message : "unknown error");
        return;
    }

    if (!pcv_spawn_sync_timeout((const gchar *const *)show_b,
                                &fixture->dump_b2, NULL, 10, NULL) ||
        !fixture->dump_b2 || strstr(fixture->dump_b2, "peer:")) {
        g_test_fail_printf("vmB retained vmA peer after detach");
        return;
    }

    const gchar *probe_a[] = {
        "ip", "netns", "exec", fixture->ep_a, "true", NULL
    };
    if (pcv_spawn_sync_timeout((const gchar *const *)probe_a,
                               NULL, NULL, 5, NULL)) {
        g_test_fail_printf("vmA endpoint remains after detach");
        return;
    }

    if (!pcv_tenant_overlay_detach_vm(
            "tmesh", "vmB", &fixture->store.error) ||
        fixture->store.error) {
        g_test_fail_printf("vmB detach failed: %s",
                           fixture->store.error
                               ? fixture->store.error->message : "unknown error");
        return;
    }
    if (!pcv_tenant_overlay_delete("tmesh", &fixture->store.error) ||
        fixture->store.error)
        g_test_fail_printf("tenant delete failed: %s",
                           fixture->store.error
                               ? fixture->store.error->message : "unknown error");
}

                                                                               
                                                      
                                                                               
                                                                
                                                                 
                                                 
                                  
  
                                                            
                                                                               
   
static void
test_overlay_on_vm_gone_cleanup(OverlayRootFixture *fixture,
                                gconstpointer user_data)
{
    (void)user_data;
    if (geteuid() != 0) { g_test_skip("root(+netns) 필요"); return; }
    if (!_wg_available()) { g_test_skip("wg(wireguard-tools) 미설치"); return; }
    if (!fixture->store.store_open)
        return;

    if (!pcv_tenant_overlay_create("tgone", &fixture->store.error) ||
        fixture->store.error) {
        g_test_fail_printf("tenant create failed: %s",
                           fixture->store.error
                               ? fixture->store.error->message : "unknown error");
        return;
    }

    fixture->ip_a = pcv_tenant_overlay_attach_vm(
        "tgone", "vmA", &fixture->store.error);
    if (!fixture->ip_a || fixture->store.error) {
        g_test_fail_printf("vmA attach failed: %s",
                           fixture->store.error
                               ? fixture->store.error->message : "unknown error");
        return;
    }
    if (!pcv_tenant_overlay_get_member_ep(
            "tgone", "vmA", &fixture->ep_a) || !fixture->ep_a) {
        g_test_fail_printf("vmA endpoint lookup failed");
        return;
    }

                                                             
    pcv_tenant_overlay_on_vm_gone("vm-does-not-exist");
    gchar *ep_untouched = NULL;
    if (!pcv_tenant_overlay_get_member_ep("tgone", "vmA", &ep_untouched)) {
        g_free(ep_untouched);
        g_test_fail_printf("unknown VM cleanup changed vmA membership");
        return;
    }
    g_free(ep_untouched);

                                                             
    pcv_tenant_overlay_on_vm_gone("vmA");

                                
    gchar *ep_after = NULL;
    if (pcv_tenant_overlay_get_member_ep("tgone", "vmA", &ep_after) ||
        ep_after) {
        g_free(ep_after);
        g_test_fail_printf("vmA membership remains after on_vm_gone");
        return;
    }

                                            
    const gchar *probe[] = {
        "ip", "netns", "exec", fixture->ep_a, "true", NULL
    };
    if (pcv_spawn_sync_timeout((const gchar *const *)probe,
                               NULL, NULL, 5, NULL)) {
        g_test_fail_printf("vmA endpoint remains after on_vm_gone");
        return;
    }

                              
    gchar *enc_after = NULL;
    if (pcv_security_store_get_wg_key(
            "tgone", "vmA", NULL, &enc_after, NULL, NULL)) {
        g_free(enc_after);
        g_test_fail_printf("vmA key remains after on_vm_gone");
        return;
    }
    g_free(enc_after);

                                                          
    pcv_tenant_overlay_on_vm_gone("vmA");

    if (!pcv_tenant_overlay_delete("tgone", &fixture->store.error) ||
        fixture->store.error)
        g_test_fail_printf("tenant delete failed: %s",
                           fixture->store.error
                               ? fixture->store.error->message : "unknown error");
}

                                                                               
                                          
                                                             
                                                          
                                                         
                                                                                  
static void
test_overlay_endpoint_down_result(void)
{
    GError *err = NULL;

                                          
    g_assert_true(pcv_tenant_overlay_wg_endpoint_down("ovlnoexist", &err));
    g_assert_no_error(err);

                                         
    pcv_tenant_overlay_wg_force_teardown_fail_for_test(TRUE);
    g_assert_false(pcv_tenant_overlay_wg_endpoint_down("ovlnoexist", &err));
    g_assert_nonnull(err);
    g_clear_error(&err);
    pcv_tenant_overlay_wg_force_teardown_fail_for_test(FALSE);

                                  
    g_assert_true(pcv_tenant_overlay_wg_endpoint_down(NULL, &err));
    g_assert_no_error(err);
}

                                                                               
                                                  
                                                                               
                                                                     
                                             
                                                
                                                                               
   
static void
test_rpc_validate_missing_tenant(void)
{
    JsonObject *params = json_object_new();
    GError *err = NULL;
    const gchar *tenant = NULL;
    g_assert_false(pcv_tenant_overlay_rpc_validate(params, FALSE, &tenant, NULL, &err));
    g_assert_null(tenant);
    g_assert_nonnull(err);
    g_assert_nonnull(strstr(err->message, "tenant"));
    g_error_free(err);
    json_object_unref(params);
}

static void
test_rpc_validate_null_params(void)
{
    GError *err = NULL;
    const gchar *tenant = NULL;
    g_assert_false(pcv_tenant_overlay_rpc_validate(NULL, FALSE, &tenant, NULL, &err));
    g_assert_null(tenant);
    g_assert_nonnull(err);
    g_error_free(err);
}

static void
test_rpc_validate_empty_tenant(void)
{
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "tenant", "");
    GError *err = NULL;
    const gchar *tenant = NULL;
    g_assert_false(pcv_tenant_overlay_rpc_validate(params, FALSE, &tenant, NULL, &err));
    g_assert_null(tenant);
    g_assert_nonnull(err);
    g_error_free(err);
    json_object_unref(params);
}

static void
test_rpc_validate_invalid_tenant_charset(void)
{
                                                                    
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "tenant", "bad tenant/../name");
    GError *err = NULL;
    const gchar *tenant = NULL;
    g_assert_false(pcv_tenant_overlay_rpc_validate(params, FALSE, &tenant, NULL, &err));
    g_assert_null(tenant);
    g_assert_nonnull(err);
    g_error_free(err);
    json_object_unref(params);
}

static void
test_rpc_validate_tenant_too_long(void)
{
                                                                     
    GString *long_name = g_string_new(NULL);
    for (int i = 0; i < 65; i++) g_string_append_c(long_name, 'a');

    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "tenant", long_name->str);
    GError *err = NULL;
    const gchar *tenant = NULL;
    g_assert_false(pcv_tenant_overlay_rpc_validate(params, FALSE, &tenant, NULL, &err));
    g_assert_null(tenant);
    g_assert_nonnull(err);
    g_error_free(err);
    json_object_unref(params);
    g_string_free(long_name, TRUE);
}

static void
test_rpc_validate_valid_tenant_only(void)
{
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "tenant", "acme-01");
    GError *err = NULL;
    const gchar *tenant = NULL;
    const gchar *vm = (const gchar *)0x1;                                          
    g_assert_true(pcv_tenant_overlay_rpc_validate(params, FALSE, &tenant, &vm, &err));
    g_assert_no_error(err);
    g_assert_cmpstr(tenant, ==, "acme-01");
    g_assert_null(vm);
    json_object_unref(params);
}

static void
test_rpc_validate_require_vm_missing(void)
{
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "tenant", "acme-01");
    GError *err = NULL;
    const gchar *tenant = NULL, *vm = NULL;
    g_assert_false(pcv_tenant_overlay_rpc_validate(params, TRUE, &tenant, &vm, &err));
                                                
    g_assert_null(vm);
    g_assert_nonnull(err);
    g_assert_nonnull(strstr(err->message, "vm"));
    g_error_free(err);
    json_object_unref(params);
}

static void
test_rpc_validate_require_vm_invalid_charset(void)
{
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "tenant", "acme-01");
    json_object_set_string_member(params, "vm", "../../etc/passwd");
    GError *err = NULL;
    const gchar *tenant = NULL, *vm = NULL;
    g_assert_false(pcv_tenant_overlay_rpc_validate(params, TRUE, &tenant, &vm, &err));
    g_assert_null(vm);
    g_assert_nonnull(err);
    g_error_free(err);
    json_object_unref(params);
}

static void
test_rpc_validate_valid_tenant_and_vm(void)
{
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "tenant", "acme-01");
    json_object_set_string_member(params, "vm", "web_01");
    GError *err = NULL;
    const gchar *tenant = NULL, *vm = NULL;
    g_assert_true(pcv_tenant_overlay_rpc_validate(params, TRUE, &tenant, &vm, &err));
    g_assert_no_error(err);
    g_assert_cmpstr(tenant, ==, "acme-01");
    g_assert_cmpstr(vm, ==, "web_01");
    json_object_unref(params);
}

                                                                               
                                                                
                                                                               
                                                       
                                                       
                                                           
  
                      
                                                                               
                                                                     
                                                           
                                               
                                                                    
                             
                                                        
                                                                  
                                                      
                                                           
                      
  
                                   
                                                           
                                                                       
                                                        
                                                          
                                          
                                                                               
   
static void
test_overlay_rehydrate(void)
{
    GError *err = NULL;

                                                        
    gchar *dbpath = g_strdup_printf("%s/pcv-overlay-rehydrate-%u.db",
                                    g_get_tmp_dir(), g_random_int());
    g_assert_true(pcv_security_store_open(dbpath));

                               
    pcv_tenant_overlay_reset_for_test();

                                                                    
                                                           
    g_assert_true(pcv_security_store_put_tenant("rehyA", 7, &err));
    g_assert_no_error(err);
    g_assert_true(pcv_security_store_put_wg_key(
        "rehyA", "vm1", "PUBKEYvm1AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=",
        "ENC2:dummy1", "10.100.7.10", &err));
    g_assert_no_error(err);
    g_assert_true(pcv_security_store_set_wg_key_slot("rehyA", "vm1", 42, &err));
    g_assert_no_error(err);
    g_assert_true(pcv_security_store_put_wg_key(
        "rehyA", "vm2", "PUBKEYvm2BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB=",
        "ENC2:dummy2", "10.100.7.11", &err));
    g_assert_no_error(err);
    g_assert_true(pcv_security_store_set_wg_key_slot("rehyA", "vm2", 43, &err));
    g_assert_no_error(err);

                                                                     
                                                   
                                                               
                                                          
    g_assert_true(pcv_security_store_put_tenant("rehyLegacy", 20, &err));
    g_assert_no_error(err);
    g_assert_true(pcv_security_store_put_wg_key(
        "rehyLegacy", "vmLegacy", "PUBKEYLegacyAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=",
        "ENC2:dummyLegacy", "10.100.20.10", &err));
    g_assert_no_error(err);
                                                                          

    g_assert_true(pcv_security_store_put_tenant("rehyBad", 21, &err));
    g_assert_no_error(err);
    g_assert_true(pcv_security_store_put_wg_key(
        "rehyBad", "vmBadSubnet", "PUBKEYBadAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=",
        "ENC2:dummyBad", "10.100.7.99", &err));                                          
    g_assert_no_error(err);
    g_assert_true(pcv_security_store_set_wg_key_slot("rehyBad", "vmBadSubnet", 60, &err));
    g_assert_no_error(err);

    g_assert_true(pcv_security_store_put_tenant("rehyCollide", 22, &err));
    g_assert_no_error(err);
    g_assert_true(pcv_security_store_put_wg_key(
        "rehyCollide", "vmCollideA", "PUBKEYCollA===================================",
        "ENC2:dummyCollA", "10.100.22.10", &err));
    g_assert_no_error(err);
    g_assert_true(pcv_security_store_set_wg_key_slot("rehyCollide", "vmCollideA", 99, &err));
    g_assert_no_error(err);
    g_assert_true(pcv_security_store_put_wg_key(
        "rehyCollide", "vmCollideB", "PUBKEYCollB===================================",
        "ENC2:dummyCollB", "10.100.22.11", &err));
    g_assert_no_error(err);
    g_assert_true(pcv_security_store_set_wg_key_slot("rehyCollide", "vmCollideB", 99, &err));
    g_assert_no_error(err);                                              

                   
    g_assert_true(pcv_tenant_overlay_rehydrate(&err));
    g_assert_no_error(err);

                                  
    GPtrArray *tenants = pcv_tenant_overlay_list();
    gboolean has_rehyA = FALSE;
    for (guint i = 0; i < tenants->len; i++)
        if (g_strcmp0(g_ptr_array_index(tenants, i), "rehyA") == 0) has_rehyA = TRUE;
    g_assert_true(has_rehyA);
    g_ptr_array_unref(tenants);

    gchar *cidr = NULL;
    g_assert_true(pcv_tenant_overlay_get_subnet("rehyA", &cidr));
    g_assert_cmpstr(cidr, ==, "10.100.7.0/24");                                        
    g_free(cidr);

    gchar *ep1 = NULL, *ep2 = NULL;
    g_assert_true(pcv_tenant_overlay_get_member_ep("rehyA", "vm1", &ep1));
    g_assert_cmpstr(ep1, ==, "ovl0002a");                                        
    g_free(ep1);
    g_assert_true(pcv_tenant_overlay_get_member_ep("rehyA", "vm2", &ep2));
    g_assert_cmpstr(ep2, ==, "ovl0002b");                                        
    g_free(ep2);

    gchar *ep3 = NULL;
    g_assert_false(pcv_tenant_overlay_get_member_ep("rehyA", "vm3", &ep3));
    g_assert_null(ep3);

    g_assert_cmpint(pcv_tenant_overlay_count_members_for_test("rehyA"), ==, 2);

                                                        
                                  
    gchar *ep_legacy = NULL;
    g_assert_false(pcv_tenant_overlay_get_member_ep("rehyLegacy", "vmLegacy", &ep_legacy));
    g_assert_null(ep_legacy);
    g_assert_cmpint(pcv_tenant_overlay_count_members_for_test("rehyLegacy"), ==, 0);

                                                           
                                                     
                                                        
    gchar *ep_bad = NULL;
    g_assert_false(pcv_tenant_overlay_get_member_ep("rehyBad", "vmBadSubnet", &ep_bad));
    g_assert_null(ep_bad);
    g_assert_cmpint(pcv_tenant_overlay_count_members_for_test("rehyBad"), ==, 0);

                                                        
                                                                 
                                                                        
                                                      
                                                    
    gchar *ep_ca = NULL, *ep_cb = NULL;
    gboolean got_ca = pcv_tenant_overlay_get_member_ep("rehyCollide", "vmCollideA", &ep_ca);
    gboolean got_cb = pcv_tenant_overlay_get_member_ep("rehyCollide", "vmCollideB", &ep_cb);
    g_assert_true(got_ca != got_cb);                         
    if (got_ca) g_assert_cmpstr(ep_ca, ==, "ovl00063");                       
    if (got_cb) g_assert_cmpstr(ep_cb, ==, "ovl00063");
    g_free(ep_ca); g_free(ep_cb);
    g_assert_cmpint(pcv_tenant_overlay_count_members_for_test("rehyCollide"), ==, 1);

                                     
    g_assert_true(pcv_tenant_overlay_rehydrate(&err));
    g_assert_no_error(err);
    g_assert_cmpint(pcv_tenant_overlay_count_members_for_test("rehyA"), ==, 2);

                                                                      
    gchar *nip = pcv_tenant_overlay_alloc_ip("rehyA", &err);
    g_assert_no_error(err);
    g_assert_cmpstr(nip, ==, "10.100.7.12");
    pcv_tenant_overlay_free_ip("rehyA", nip);                
    g_free(nip);

                                               
    if (geteuid() == 0 && _wg_available()) {
        (void)pcv_tenant_overlay_wg_endpoint_down("ovl00000", NULL);
        (void)pcv_tenant_overlay_wg_endpoint_down("ovl00001", NULL);
        (void)pcv_tenant_overlay_wg_endpoint_down("ovl00002", NULL);

        g_assert_true(pcv_tenant_overlay_create("rehyB", &err));
        g_assert_no_error(err);
        gchar *ipa = pcv_tenant_overlay_attach_vm("rehyB", "vmA", &err);
        g_assert_no_error(err); g_assert_nonnull(ipa);
        gchar *ipb = pcv_tenant_overlay_attach_vm("rehyB", "vmB", &err);
        g_assert_no_error(err); g_assert_nonnull(ipb);
        gchar *epa = NULL, *epb = NULL;
        g_assert_true(pcv_tenant_overlay_get_member_ep("rehyB", "vmA", &epa));
        g_assert_true(pcv_tenant_overlay_get_member_ep("rehyB", "vmB", &epb));

                                                        
        pcv_tenant_overlay_reset_for_test();
        g_assert_true(pcv_tenant_overlay_rehydrate(&err));
        g_assert_no_error(err);
        gchar *epa2 = NULL;
        g_assert_true(pcv_tenant_overlay_get_member_ep("rehyB", "vmA", &epa2));
        g_assert_cmpstr(epa2, ==, epa);                      
        g_free(epa2);

                                                 
        gchar *ipc = pcv_tenant_overlay_attach_vm("rehyB", "vmC", &err);
        g_assert_no_error(err); g_assert_nonnull(ipc);
        g_assert_cmpstr(ipc, !=, ipa);
        g_assert_cmpstr(ipc, !=, ipb);
        gchar *epc = NULL;
        g_assert_true(pcv_tenant_overlay_get_member_ep("rehyB", "vmC", &epc));
        g_assert_cmpstr(epc, !=, epa);
        g_assert_cmpstr(epc, !=, epb);
        g_free(epc);

        (void)pcv_tenant_overlay_detach_vm("rehyB", "vmA", NULL);
        (void)pcv_tenant_overlay_detach_vm("rehyB", "vmB", NULL);
        (void)pcv_tenant_overlay_detach_vm("rehyB", "vmC", NULL);
        (void)pcv_tenant_overlay_delete("rehyB", NULL);
        g_free(epa); g_free(epb); g_free(ipa); g_free(ipb); g_free(ipc);
    } else {
        g_test_message("collision sub-assertion (real-restart-sim) skipped — needs root+wg");
    }

                                                     
    pcv_tenant_overlay_reset_for_test();
    pcv_security_store_close();
    g_unlink(dbpath);
    gchar *wal = g_strdup_printf("%s-wal", dbpath);
    gchar *shm = g_strdup_printf("%s-shm", dbpath);
    g_unlink(wal); g_unlink(shm);
    g_free(wal); g_free(shm); g_free(dbpath);
}

                                                                               
                                                                   
                                                         
                                                              
                                                    
                                                                
                                                                                  
static void
test_overlay_detach_db_first_retry(void)
{
    GError *err = NULL;
    gchar *dbpath = g_strdup_printf("%s/pcv-overlay-dbfirst-%u.db",
                                    g_get_tmp_dir(), g_random_int());
    g_assert_true(pcv_security_store_open(dbpath));
    pcv_tenant_overlay_reset_for_test();

    g_assert_true(pcv_security_store_put_tenant("dbfA", 40, &err));
    g_assert_no_error(err);
    g_assert_true(pcv_security_store_put_wg_key(
        "dbfA", "vm1", "PUBKEYDBF====================================",
        "ENC2:dummyDbf", "10.100.40.10", &err));
    g_assert_no_error(err);
    g_assert_true(pcv_security_store_set_wg_key_slot("dbfA", "vm1", 300, &err));
    g_assert_no_error(err);
    g_assert_true(pcv_tenant_overlay_rehydrate(&err));
    g_assert_no_error(err);
    g_assert_cmpint(pcv_tenant_overlay_count_members_for_test("dbfA"), ==, 1);

                                                             
    pcv_security_store_close();
    g_assert_false(pcv_tenant_overlay_detach_vm("dbfA", "vm1", &err));
    g_assert_nonnull(err);
    g_clear_error(&err);

                                         
    g_assert_cmpint(pcv_tenant_overlay_count_members_for_test("dbfA"), ==, 1);
    gchar *ep = NULL;
    g_assert_true(pcv_tenant_overlay_get_member_ep("dbfA", "vm1", &ep));
    g_assert_cmpstr(ep, ==, "ovl0012c");                         
    g_free(ep);
    g_assert_true(pcv_tenant_overlay_slot_used_for_test(300));
    gchar *probe = pcv_tenant_overlay_alloc_ip("dbfA", &err);
    g_assert_no_error(err);
    g_assert_cmpstr(probe, ==, "10.100.40.11");                   
    pcv_tenant_overlay_free_ip("dbfA", probe);
    g_free(probe);

                                                        
                                                         
    g_assert_true(pcv_security_store_open(dbpath));
    g_assert_true(pcv_tenant_overlay_detach_vm("dbfA", "vm1", &err));
    g_assert_no_error(err);
    g_assert_cmpint(pcv_tenant_overlay_count_members_for_test("dbfA"), ==, 0);
    g_assert_false(pcv_security_store_get_wg_key("dbfA", "vm1", NULL, NULL, NULL, NULL));
    g_assert_false(pcv_tenant_overlay_slot_used_for_test(300));

                                                  
    g_assert_false(pcv_tenant_overlay_detach_vm("dbfA", "vm1", &err));
    g_assert_nonnull(err);
    g_assert_nonnull(strstr(err->message, "조인되어 있지 않습니다"));
    g_clear_error(&err);

    pcv_tenant_overlay_reset_for_test();
    pcv_security_store_close();
    g_unlink(dbpath);
    gchar *wal = g_strdup_printf("%s-wal", dbpath);
    gchar *shm = g_strdup_printf("%s-shm", dbpath);
    g_unlink(wal); g_unlink(shm);
    g_free(wal); g_free(shm); g_free(dbpath);
}

                                                                               
                                                                 
                                                      
                                                             
                                                             
                                                                                  
static void
test_overlay_detach_partial_cleanup_quarantine(void)
{
    GError *err = NULL;
    gchar *dbpath = g_strdup_printf("%s/pcv-overlay-partial-%u.db",
                                    g_get_tmp_dir(), g_random_int());
    g_assert_true(pcv_security_store_open(dbpath));
    pcv_tenant_overlay_reset_for_test();

    g_assert_true(pcv_security_store_put_tenant("pqA", 41, &err));
    g_assert_no_error(err);
    g_assert_true(pcv_security_store_put_wg_key(
        "pqA", "vm1", "PUBKEYPQ=====================================",
        "ENC2:dummyPq", "10.100.41.10", &err));
    g_assert_no_error(err);
    g_assert_true(pcv_security_store_set_wg_key_slot("pqA", "vm1", 310, &err));
    g_assert_no_error(err);
    g_assert_true(pcv_tenant_overlay_rehydrate(&err));
    g_assert_no_error(err);

    pcv_tenant_overlay_wg_force_teardown_fail_for_test(TRUE);
    g_assert_false(pcv_tenant_overlay_detach_vm("pqA", "vm1", &err));
    g_assert_nonnull(err);
    g_assert_nonnull(strstr(err->message, "멤버십 해제는 확정"));                        
    g_clear_error(&err);
    pcv_tenant_overlay_wg_force_teardown_fail_for_test(FALSE);

                                            
    g_assert_cmpint(pcv_tenant_overlay_count_members_for_test("pqA"), ==, 0);
    g_assert_false(pcv_security_store_get_wg_key("pqA", "vm1", NULL, NULL, NULL, NULL));
    g_assert_false(pcv_tenant_overlay_detach_vm("pqA", "vm1", &err));
    g_assert_nonnull(err);
    g_assert_nonnull(strstr(err->message, "조인되어 있지 않습니다"));
    g_clear_error(&err);

                             
    g_assert_true(pcv_tenant_overlay_slot_used_for_test(310));

    pcv_tenant_overlay_reset_for_test();                             
    pcv_security_store_close();
    g_unlink(dbpath);
    gchar *wal = g_strdup_printf("%s-wal", dbpath);
    gchar *shm = g_strdup_printf("%s-shm", dbpath);
    g_unlink(wal); g_unlink(shm);
    g_free(wal); g_free(shm); g_free(dbpath);
}

                                                                               
                                          
                                                   
                                             
                                             
                                                                                  
static void
test_overlay_attach_cross_tenant_unique(void)
{
    GError *err = NULL;
    gchar *dbpath = g_strdup_printf("%s/pcv-overlay-xtenant-%u.db",
                                    g_get_tmp_dir(), g_random_int());
    g_assert_true(pcv_security_store_open(dbpath));
    pcv_tenant_overlay_reset_for_test();

    g_assert_true(pcv_security_store_put_tenant("xtenA", 30, &err));
    g_assert_no_error(err);
    g_assert_true(pcv_security_store_put_tenant("xtenB", 31, &err));
    g_assert_no_error(err);
    g_assert_true(pcv_security_store_put_wg_key(
        "xtenA", "vmX", "PUBKEYX=====================================",
        "ENC2:dummyX", "10.100.30.10", &err));
    g_assert_no_error(err);
    g_assert_true(pcv_security_store_set_wg_key_slot("xtenA", "vmX", 200, &err));
    g_assert_no_error(err);
    g_assert_true(pcv_tenant_overlay_rehydrate(&err));
    g_assert_no_error(err);

                                                               
    gchar *ip = pcv_tenant_overlay_attach_vm("xtenB", "vmX", &err);
    g_assert_null(ip);
    g_assert_nonnull(err);
    g_assert_nonnull(strstr(err->message, "xtenA"));
    g_assert_nonnull(strstr(err->message, "다중 소속"));
    g_clear_error(&err);

                                                               
    g_assert_cmpint(pcv_tenant_overlay_count_members_for_test("xtenB"), ==, 0);
    gchar *probe = pcv_tenant_overlay_alloc_ip("xtenB", &err);
    g_assert_no_error(err);
    g_assert_cmpstr(probe, ==, "10.100.31.10");
    pcv_tenant_overlay_free_ip("xtenB", probe);
    g_free(probe);
    g_assert_false(pcv_security_store_get_wg_key("xtenB", "vmX", NULL, NULL, NULL, NULL));

                                                         
    ip = pcv_tenant_overlay_attach_vm("xtenA", "vmX", &err);
    g_assert_null(ip);
    g_assert_nonnull(err);
    g_assert_nonnull(strstr(err->message, "이미 테넌트 'xtenA' 에 조인"));
    g_clear_error(&err);

    pcv_tenant_overlay_reset_for_test();
    pcv_security_store_close();
    g_unlink(dbpath);
    gchar *wal = g_strdup_printf("%s-wal", dbpath);
    gchar *shm = g_strdup_printf("%s-shm", dbpath);
    g_unlink(wal); g_unlink(shm);
    g_free(wal); g_free(shm); g_free(dbpath);
}

                                                                               
                                               
                                                                
                                                  
                                                      
                                      
                                                                                  
static void
test_overlay_on_vm_gone_detach_all(void)
{
    GError *err = NULL;
    gchar *dbpath = g_strdup_printf("%s/pcv-overlay-goneall-%u.db",
                                    g_get_tmp_dir(), g_random_int());
    g_assert_true(pcv_security_store_open(dbpath));
    pcv_tenant_overlay_reset_for_test();

    g_assert_true(pcv_security_store_put_tenant("goneA", 60, &err));
    g_assert_no_error(err);
    g_assert_true(pcv_security_store_put_tenant("goneB", 61, &err));
    g_assert_no_error(err);
    g_assert_true(pcv_security_store_put_wg_key(
        "goneA", "vmX", "PUBKEYGA=====================================",
        "ENC2:dummyGa", "10.100.60.10", &err));
    g_assert_no_error(err);
    g_assert_true(pcv_security_store_set_wg_key_slot("goneA", "vmX", 400, &err));
    g_assert_no_error(err);
    g_assert_true(pcv_security_store_put_wg_key(
        "goneB", "vmX", "PUBKEYGB=====================================",
        "ENC2:dummyGb", "10.100.61.10", &err));
    g_assert_no_error(err);
    g_assert_true(pcv_security_store_set_wg_key_slot("goneB", "vmX", 401, &err));
    g_assert_no_error(err);
    g_assert_true(pcv_tenant_overlay_rehydrate(&err));
    g_assert_no_error(err);
    g_assert_cmpint(pcv_tenant_overlay_count_members_for_test("goneA"), ==, 1);
    g_assert_cmpint(pcv_tenant_overlay_count_members_for_test("goneB"), ==, 1);

    pcv_tenant_overlay_on_vm_gone("vmX");

                                      
    g_assert_cmpint(pcv_tenant_overlay_count_members_for_test("goneA"), ==, 0);
    g_assert_cmpint(pcv_tenant_overlay_count_members_for_test("goneB"), ==, 0);
    g_assert_false(pcv_security_store_get_wg_key("goneA", "vmX", NULL, NULL, NULL, NULL));
    g_assert_false(pcv_security_store_get_wg_key("goneB", "vmX", NULL, NULL, NULL, NULL));
    g_assert_false(pcv_tenant_overlay_slot_used_for_test(400));
    g_assert_false(pcv_tenant_overlay_slot_used_for_test(401));

                           
    pcv_tenant_overlay_on_vm_gone("vmX");

    pcv_tenant_overlay_reset_for_test();
    pcv_security_store_close();
    g_unlink(dbpath);
    gchar *wal = g_strdup_printf("%s-wal", dbpath);
    gchar *shm = g_strdup_printf("%s-shm", dbpath);
    g_unlink(wal); g_unlink(shm);
    g_free(wal); g_free(shm); g_free(dbpath);
}

                                                                               
                                                
                                                         
                                                      
                                           
                                                                                  
static void
test_overlay_slot_exhausted_rollback(void)
{
    if (!_wg_available()) { g_test_skip("wg(wireguard-tools) 미설치"); return; }

    GError *err = NULL;
    gchar *dbpath = g_strdup_printf("%s/pcv-overlay-slotex-%u.db",
                                    g_get_tmp_dir(), g_random_int());
    g_assert_true(pcv_security_store_open(dbpath));
    pcv_tenant_overlay_reset_for_test();

                                             
    pcv_tenant_overlay_set_slot_cap_for_test(2);
    g_assert_true(pcv_security_store_put_tenant("sxA", 50, &err));
    g_assert_no_error(err);
    g_assert_true(pcv_security_store_put_wg_key(
        "sxA", "vm1", "PUBKEYSX1====================================",
        "ENC2:dummySx1", "10.100.50.10", &err));
    g_assert_no_error(err);
    g_assert_true(pcv_security_store_set_wg_key_slot("sxA", "vm1", 0, &err));
    g_assert_no_error(err);
    g_assert_true(pcv_security_store_put_wg_key(
        "sxA", "vm2", "PUBKEYSX2====================================",
        "ENC2:dummySx2", "10.100.50.11", &err));
    g_assert_no_error(err);
    g_assert_true(pcv_security_store_set_wg_key_slot("sxA", "vm2", 1, &err));
    g_assert_no_error(err);
    g_assert_true(pcv_tenant_overlay_rehydrate(&err));
    g_assert_no_error(err);

    gchar *ip = pcv_tenant_overlay_attach_vm("sxA", "vm3", &err);
    g_assert_null(ip);
    g_assert_nonnull(err);
    g_assert_nonnull(strstr(err->message, "슬롯 풀이 소진"));
    g_clear_error(&err);

                                                               
    g_assert_false(pcv_security_store_get_wg_key("sxA", "vm3", NULL, NULL, NULL, NULL));
    gchar *probe = pcv_tenant_overlay_alloc_ip("sxA", &err);
    g_assert_no_error(err);
    g_assert_cmpstr(probe, ==, "10.100.50.12");
    pcv_tenant_overlay_free_ip("sxA", probe);
    g_free(probe);
    g_assert_cmpint(pcv_tenant_overlay_count_members_for_test("sxA"), ==, 2);

    pcv_tenant_overlay_set_slot_cap_for_test(0);
    pcv_tenant_overlay_reset_for_test();
    pcv_security_store_close();
    g_unlink(dbpath);
    gchar *wal = g_strdup_printf("%s-wal", dbpath);
    gchar *shm = g_strdup_printf("%s-shm", dbpath);
    g_unlink(wal); g_unlink(shm);
    g_free(wal); g_free(shm); g_free(dbpath);
}

                                                                               
                                                                     
                                                                                  
static void
test_overlay_subnet_full(void)
{
    GError *err = NULL;
    gchar *dbpath = g_strdup_printf("%s/pcv-overlay-subf-%u.db",
                                    g_get_tmp_dir(), g_random_int());
    g_assert_true(pcv_security_store_open(dbpath));
    pcv_tenant_overlay_reset_for_test();

    g_assert_true(pcv_tenant_overlay_create("subf", &err));
    g_assert_no_error(err);

    GPtrArray *ips = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < 245; i++) {                              
        gchar *a = pcv_tenant_overlay_alloc_ip("subf", &err);
        g_assert_no_error(err);
        g_assert_nonnull(a);
        g_ptr_array_add(ips, a);
    }
    gchar *over = pcv_tenant_overlay_alloc_ip("subf", &err);
    g_assert_null(over);
    g_assert_nonnull(err);
    g_assert_nonnull(strstr(err->message, "포화"));
    g_clear_error(&err);

                                    
    gchar *first = g_strdup(g_ptr_array_index(ips, 0));
    pcv_tenant_overlay_free_ip("subf", first);
    gchar *re = pcv_tenant_overlay_alloc_ip("subf", &err);
    g_assert_no_error(err);
    g_assert_cmpstr(re, ==, first);
    g_free(re);
    g_free(first);
    g_ptr_array_unref(ips);

    pcv_tenant_overlay_reset_for_test();
    pcv_security_store_close();
    g_unlink(dbpath);
    gchar *wal = g_strdup_printf("%s-wal", dbpath);
    gchar *shm = g_strdup_printf("%s-shm", dbpath);
    g_unlink(wal); g_unlink(shm);
    g_free(wal); g_free(shm); g_free(dbpath);
}

                                                                               
                                                            
                                                              
                                                    
                                                                                  
static void
test_overlay_attach_preclean_fail_quarantine(void)
{
    if (!_wg_available()) { g_test_skip("wg(wireguard-tools) 미설치"); return; }

    GError *err = NULL;
    gchar *dbpath = g_strdup_printf("%s/pcv-overlay-preclean-%u.db",
                                    g_get_tmp_dir(), g_random_int());
    g_assert_true(pcv_security_store_open(dbpath));
    pcv_tenant_overlay_reset_for_test();

    g_assert_true(pcv_tenant_overlay_create("pcA", &err));                             
    g_assert_no_error(err);

    pcv_tenant_overlay_wg_force_teardown_fail_for_test(TRUE);
    gchar *ip = pcv_tenant_overlay_attach_vm("pcA", "vmP", &err);
    g_assert_null(ip);
    g_assert_nonnull(err);
    g_clear_error(&err);
    pcv_tenant_overlay_wg_force_teardown_fail_for_test(FALSE);

                                                        
    g_assert_true(pcv_tenant_overlay_slot_used_for_test(0));
    g_assert_false(pcv_security_store_get_wg_key("pcA", "vmP", NULL, NULL, NULL, NULL));
    gchar *probe = pcv_tenant_overlay_alloc_ip("pcA", &err);
    g_assert_no_error(err);
    g_assert_cmpstr(probe, ==, "10.100.1.10");                       
    pcv_tenant_overlay_free_ip("pcA", probe);
    g_free(probe);
    g_assert_cmpint(pcv_tenant_overlay_count_members_for_test("pcA"), ==, 0);

    pcv_tenant_overlay_reset_for_test();
    pcv_security_store_close();
    g_unlink(dbpath);
    gchar *wal = g_strdup_printf("%s-wal", dbpath);
    gchar *shm = g_strdup_printf("%s-shm", dbpath);
    g_unlink(wal); g_unlink(shm);
    g_free(wal); g_free(shm); g_free(dbpath);
}

                                                                               
                                                               
                                                          
                                                              
                                             
                                                                                  
static void
test_overlay_mesh_n3_consistency(OverlayRootFixture *fixture,
                                 gconstpointer user_data)
{
    (void)user_data;
    if (geteuid() != 0) { g_test_skip("root(+netns) 필요"); return; }
    if (!_wg_available()) { g_test_skip("wg(wireguard-tools) 미설치"); return; }
    if (!fixture->store.store_open)
        return;
#define ROOT_REQUIRE(expr, message) \
    G_STMT_START { if (!(expr)) { g_test_fail_printf("%s", message); return; } } G_STMT_END
    ROOT_REQUIRE(pcv_tenant_overlay_create("tn3", &fixture->store.error) &&
                 !fixture->store.error, "tenant create failed");
    fixture->ip_a = pcv_tenant_overlay_attach_vm(
        "tn3", "vmA", &fixture->store.error);
    ROOT_REQUIRE(fixture->ip_a && !fixture->store.error, "vmA attach failed");
    ROOT_REQUIRE(pcv_tenant_overlay_get_member_ep(
                     "tn3", "vmA", &fixture->ep_a) && fixture->ep_a,
                 "vmA endpoint lookup failed");
    fixture->ip_b = pcv_tenant_overlay_attach_vm(
        "tn3", "vmB", &fixture->store.error);
    ROOT_REQUIRE(fixture->ip_b && !fixture->store.error, "vmB attach failed");
    ROOT_REQUIRE(pcv_tenant_overlay_get_member_ep(
                     "tn3", "vmB", &fixture->ep_b) && fixture->ep_b,
                 "vmB endpoint lookup failed");
    fixture->ip_c = pcv_tenant_overlay_attach_vm(
        "tn3", "vmC", &fixture->store.error);
    ROOT_REQUIRE(fixture->ip_c && !fixture->store.error, "vmC attach failed");
    ROOT_REQUIRE(pcv_tenant_overlay_get_member_ep(
                     "tn3", "vmC", &fixture->ep_c) && fixture->ep_c,
                 "vmC endpoint lookup failed");

    fixture->wgif_a = g_strdup_printf("wg-%s", fixture->ep_a);
    fixture->wgif_b = g_strdup_printf("wg-%s", fixture->ep_b);
    fixture->wgif_c = g_strdup_printf("wg-%s", fixture->ep_c);
    const gchar *showA[] = { "ip", "netns", "exec", fixture->ep_a, "wg", "show", fixture->wgif_a, NULL };
    const gchar *showB[] = { "ip", "netns", "exec", fixture->ep_b, "wg", "show", fixture->wgif_b, NULL };
    const gchar *showC[] = { "ip", "netns", "exec", fixture->ep_c, "wg", "show", fixture->wgif_c, NULL };

    ROOT_REQUIRE(pcv_spawn_sync_timeout((const gchar *const *)showA, &fixture->dump_a, NULL, 10, NULL), "vmA wg show failed");
    ROOT_REQUIRE(pcv_spawn_sync_timeout((const gchar *const *)showB, &fixture->dump_b, NULL, 10, NULL), "vmB wg show failed");
    ROOT_REQUIRE(pcv_spawn_sync_timeout((const gchar *const *)showC, &fixture->dump_c, NULL, 10, NULL), "vmC wg show failed");
    ROOT_REQUIRE(_count_substr(fixture->dump_a, "peer:") == 2, "vmA peer count mismatch");
    ROOT_REQUIRE(_count_substr(fixture->dump_b, "peer:") == 2, "vmB peer count mismatch");
    ROOT_REQUIRE(_count_substr(fixture->dump_c, "peer:") == 2, "vmC peer count mismatch");
    g_clear_pointer(&fixture->dump_a, g_free);
    g_clear_pointer(&fixture->dump_b, g_free);
    g_clear_pointer(&fixture->dump_c, g_free);

                                                            
    ROOT_REQUIRE(pcv_tenant_overlay_detach_vm(
                     "tn3", "vmB", &fixture->store.error) &&
                 !fixture->store.error, "vmB detach failed");

    ROOT_REQUIRE(pcv_spawn_sync_timeout((const gchar *const *)showA, &fixture->dump_a, NULL, 10, NULL), "vmA post-detach wg show failed");
    ROOT_REQUIRE(pcv_spawn_sync_timeout((const gchar *const *)showC, &fixture->dump_c, NULL, 10, NULL), "vmC post-detach wg show failed");
    ROOT_REQUIRE(_count_substr(fixture->dump_a, "peer:") == 1, "vmA post-detach peer count mismatch");
    ROOT_REQUIRE(_count_substr(fixture->dump_c, "peer:") == 1, "vmC post-detach peer count mismatch");

    const gchar *probeB[] = { "ip", "netns", "exec", fixture->ep_b, "true", NULL };
    ROOT_REQUIRE(!pcv_spawn_sync_timeout((const gchar *const *)probeB,
                                         NULL, NULL, 5, NULL),
                 "vmB endpoint remains after detach");
    ROOT_REQUIRE(pcv_tenant_overlay_detach_vm("tn3", "vmA", &fixture->store.error) &&
                 !fixture->store.error, "vmA detach failed");
    ROOT_REQUIRE(pcv_tenant_overlay_detach_vm("tn3", "vmC", &fixture->store.error) &&
                 !fixture->store.error, "vmC detach failed");
    ROOT_REQUIRE(pcv_tenant_overlay_delete("tn3", &fixture->store.error) &&
                 !fixture->store.error, "tenant delete failed");
#undef ROOT_REQUIRE
}

                                                                               
                                                                
                                                         
                                                        
                                                                                  
static void
test_overlay_attach_preclean_stale(OverlayRootFixture *fixture,
                                   gconstpointer user_data)
{
    (void)user_data;
    if (geteuid() != 0) { g_test_skip("root(+netns) 필요"); return; }
    if (!_wg_available()) { g_test_skip("wg(wireguard-tools) 미설치"); return; }
    if (!fixture->store.store_open)
        return;

                                                                 
    const gchar *addns[] = { "ip", "netns", "add", "ovl00000", NULL };
    if (!pcv_spawn_sync_timeout((const gchar *const *)addns,
                                NULL, NULL, 10, NULL)) {
        g_test_fail_printf("stale namespace creation failed");
        return;
    }
    fixture->stale_ep = g_strdup("ovl00000");

    if (!pcv_tenant_overlay_create("tstale", &fixture->store.error) ||
        fixture->store.error) {
        g_test_fail_printf("tenant create failed");
        return;
    }
    fixture->ip_a = pcv_tenant_overlay_attach_vm(
        "tstale", "vmS", &fixture->store.error);
    if (!fixture->ip_a || fixture->store.error) {
        g_test_fail_printf("attach did not converge after stale preclean");
        return;
    }

    if (!pcv_tenant_overlay_get_member_ep(
            "tstale", "vmS", &fixture->ep_a) || !fixture->ep_a) {
        g_test_fail_printf("attached endpoint lookup failed");
        return;
    }
    if (g_strcmp0(fixture->ep_a, "ovl00000") != 0) {
        g_test_fail_printf("stale slot was not reused");
        return;
    }
    const gchar *showwg[] = { "ip", "netns", "exec", fixture->ep_a,
                              "ip", "link", "show", "wg-ovl00000", NULL };
    if (!pcv_spawn_sync_timeout((const gchar *const *)showwg,
                                NULL, NULL, 10, NULL)) {
        g_test_fail_printf("recreated WireGuard interface missing");
        return;
    }

    if (!pcv_tenant_overlay_detach_vm(
            "tstale", "vmS", &fixture->store.error) ||
        fixture->store.error) {
        g_test_fail_printf("vm detach failed");
        return;
    }
    if (!pcv_tenant_overlay_delete("tstale", &fixture->store.error) ||
        fixture->store.error)
        g_test_fail_printf("tenant delete failed");
}

                                                                               
                                                                  
                                                                    
                                                           
                                                               
                                                                                  
static void
test_overlay_detach_kernel_quarantine(OverlayRootFixture *fixture,
                                      gconstpointer user_data)
{
    (void)user_data;
    if (geteuid() != 0) { g_test_skip("root(+netns) 필요"); return; }
    if (!_wg_available()) { g_test_skip("wg(wireguard-tools) 미설치"); return; }
    if (!fixture->store.store_open)
        return;
    if (!pcv_tenant_overlay_create("tkq", &fixture->store.error) ||
        fixture->store.error) {
        g_test_fail_printf("tenant create failed");
        return;
    }
    fixture->ip_a = pcv_tenant_overlay_attach_vm(
        "tkq", "vmK", &fixture->store.error);
    if (!fixture->ip_a || fixture->store.error) {
        g_test_fail_printf("vm attach failed");
        return;
    }
    if (!pcv_tenant_overlay_get_member_ep(
            "tkq", "vmK", &fixture->ep_a) || !fixture->ep_a) {
        g_test_fail_printf("endpoint lookup failed");
        return;
    }
    guint slot = 0;
    if (sscanf(fixture->ep_a + 3, "%x", &slot) != 1) {
        g_test_fail_printf("endpoint slot parse failed");
        return;
    }

    fixture->nspath = g_strdup_printf("/run/netns/%s", fixture->ep_a);
    const gchar *chattr_i[] = { "chattr", "+i", fixture->nspath, NULL };
                                                    
                                                          
    fixture->immutable_applied = TRUE;
    if (!pcv_spawn_sync_timeout((const gchar *const *)chattr_i, NULL, NULL, 5, NULL)) {
                                                  
                                                            
                                                               
        (void)overlay_root_fixture_clear_immutable(fixture);
        g_test_skip("chattr +i 미지원(/run tmpfs) — fault-hook 테스트가 시맨틱 커버");
        return;
    }

                                                                 
    if (pcv_tenant_overlay_detach_vm(
            "tkq", "vmK", &fixture->store.error) ||
        !fixture->store.error ||
        !strstr(fixture->store.error->message, "멤버십 해제는 확정")) {
        g_test_fail_printf("detach did not report committed partial cleanup");
        return;
    }
    g_clear_error(&fixture->store.error);
    if (!pcv_tenant_overlay_slot_used_for_test(slot)) {
        g_test_fail_printf("quarantined slot was not retained");
        return;
    }
    gchar *ep_after = NULL;
    if (pcv_tenant_overlay_get_member_ep("tkq", "vmK", &ep_after) ||
        ep_after) {
        g_free(ep_after);
        g_test_fail_printf("membership remains after partial cleanup");
        return;
    }
    if (pcv_tenant_overlay_detach_vm(
            "tkq", "vmK", &fixture->store.error) ||
        !fixture->store.error) {
        g_test_fail_printf("terminal detach result missing");
        return;
    }
    g_clear_error(&fixture->store.error);

                                                      
    if (!overlay_root_fixture_clear_immutable(fixture)) {
        g_test_fail_printf("immutable recovery failed");
        return;
    }
    if (!pcv_tenant_overlay_wg_endpoint_down(
            fixture->ep_a, &fixture->store.error) ||
        fixture->store.error) {
        g_test_fail_printf("endpoint recovery cleanup failed");
        return;
    }

                                               
    if (!pcv_tenant_overlay_delete("tkq", &fixture->store.error) ||
        fixture->store.error)
        g_test_fail_printf("tenant delete failed");
}

                                                                               
                                                           
                                                      
                                 
                                                                                  
static void
test_overlay_list_member_vms(void)
{
    GError *err = NULL;
    gchar *dbpath = g_strdup_printf("%s/pcv-overlay-lsvm-%u.db",
                                    g_get_tmp_dir(), g_random_int());
    g_assert_true(pcv_security_store_open(dbpath));
    pcv_tenant_overlay_reset_for_test();

    GPtrArray *empty = pcv_tenant_overlay_list_member_vms();
    g_assert_nonnull(empty);
    g_assert_cmpuint(empty->len, ==, 0);
    g_ptr_array_unref(empty);

    g_assert_true(pcv_security_store_put_tenant("lsA", 80, &err));
    g_assert_no_error(err);
    g_assert_true(pcv_security_store_put_tenant("lsB", 81, &err));
    g_assert_no_error(err);
    g_assert_true(pcv_security_store_put_wg_key(
        "lsA", "vmA", "PUBKEYLSA====================================",
        "ENC2:dummyLsA", "10.100.80.10", &err));
    g_assert_no_error(err);
    g_assert_true(pcv_security_store_set_wg_key_slot("lsA", "vmA", 500, &err));
    g_assert_no_error(err);
    g_assert_true(pcv_security_store_put_wg_key(
        "lsA", "vmX", "PUBKEYLSX1===================================",
        "ENC2:dummyLsX1", "10.100.80.11", &err));
    g_assert_no_error(err);
    g_assert_true(pcv_security_store_set_wg_key_slot("lsA", "vmX", 501, &err));
    g_assert_no_error(err);
    g_assert_true(pcv_security_store_put_wg_key(
        "lsB", "vmX", "PUBKEYLSX2===================================",
        "ENC2:dummyLsX2", "10.100.81.10", &err));
    g_assert_no_error(err);
    g_assert_true(pcv_security_store_set_wg_key_slot("lsB", "vmX", 502, &err));
    g_assert_no_error(err);
    g_assert_true(pcv_tenant_overlay_rehydrate(&err));
    g_assert_no_error(err);

    GPtrArray *vms = pcv_tenant_overlay_list_member_vms();
    g_assert_cmpuint(vms->len, ==, 2);                              
    gboolean has_a = FALSE, has_x = FALSE;
    for (guint i = 0; i < vms->len; i++) {
        const gchar *v = g_ptr_array_index(vms, i);
        if (g_strcmp0(v, "vmA") == 0) has_a = TRUE;
        if (g_strcmp0(v, "vmX") == 0) has_x = TRUE;
    }
    g_assert_true(has_a);
    g_assert_true(has_x);
    g_ptr_array_unref(vms);

    pcv_tenant_overlay_reset_for_test();
    pcv_security_store_close();
    g_unlink(dbpath);
    gchar *wal = g_strdup_printf("%s-wal", dbpath);
    gchar *shm = g_strdup_printf("%s-shm", dbpath);
    g_unlink(wal); g_unlink(shm);
    g_free(wal); g_free(shm); g_free(dbpath);
}

                                                                               
                                                             
                                                          
                                  
                                                                                  
static void
test_overlay_ep_name_pattern(void)
{
    g_assert_true(pcv_tenant_overlay_ep_name_is_overlay("ovl00000"));
    g_assert_true(pcv_tenant_overlay_ep_name_is_overlay("ovl000aa"));
    g_assert_true(pcv_tenant_overlay_ep_name_is_overlay("ovlfffff"));
    g_assert_true(pcv_tenant_overlay_ep_name_is_overlay("ovl0012c"));

    g_assert_false(pcv_tenant_overlay_ep_name_is_overlay(NULL));
    g_assert_false(pcv_tenant_overlay_ep_name_is_overlay(""));
    g_assert_false(pcv_tenant_overlay_ep_name_is_overlay("ovl"));
    g_assert_false(pcv_tenant_overlay_ep_name_is_overlay("ovl0000"));            
    g_assert_false(pcv_tenant_overlay_ep_name_is_overlay("ovl000000"));         
    g_assert_false(pcv_tenant_overlay_ep_name_is_overlay("ovl000AA"));           
    g_assert_false(pcv_tenant_overlay_ep_name_is_overlay("ovl000g0"));            
    g_assert_false(pcv_tenant_overlay_ep_name_is_overlay("myns"));
    g_assert_false(pcv_tenant_overlay_ep_name_is_overlay("novl00000"));
}

                                                                               
                                                           
                                                      
                                                      
                    
                                                                                  
static void
test_overlay_sweep_orphan_endpoints(void)
{
    if (geteuid() != 0) { g_test_skip("root(+netns) 필요"); return; }
    if (!_wg_available()) { g_test_skip("wg(wireguard-tools) 미설치"); return; }

    GError *err = NULL;
    gchar *dbpath = g_strdup_printf("%s/pcv-overlay-sweep-%u.db",
                                    g_get_tmp_dir(), g_random_int());
    g_assert_true(pcv_security_store_open(dbpath));
    pcv_tenant_overlay_reset_for_test();

                                                               
    g_assert_true(pcv_security_store_put_tenant("swpA", 70, &err));
    g_assert_no_error(err);
    g_assert_true(pcv_security_store_put_wg_key(
        "swpA", "vmS", "PUBKEYSWP====================================",
        "ENC2:dummySwp", "10.100.70.10", &err));
    g_assert_no_error(err);
    g_assert_true(pcv_security_store_set_wg_key_slot("swpA", "vmS", 66, &err));
    g_assert_no_error(err);
    g_assert_true(pcv_tenant_overlay_rehydrate(&err));
    g_assert_no_error(err);

    const gchar *add_owned[]  = { "ip", "netns", "add", "ovl00042", NULL };
    const gchar *add_orphan[] = { "ip", "netns", "add", "ovl000aa", NULL };
    const gchar *add_other[]  = { "ip", "netns", "add", "pcvtestns", NULL };
    (void)pcv_tenant_overlay_wg_endpoint_down("ovl00042", NULL);             
    (void)pcv_tenant_overlay_wg_endpoint_down("ovl000aa", NULL);
    g_assert_true(pcv_spawn_sync_timeout((const gchar *const *)add_owned, NULL, NULL, 10, NULL));
    g_assert_true(pcv_spawn_sync_timeout((const gchar *const *)add_orphan, NULL, NULL, 10, NULL));
    g_assert_true(pcv_spawn_sync_timeout((const gchar *const *)add_other, NULL, NULL, 10, NULL));

    guint fail = 0;
    guint swept = pcv_tenant_overlay_sweep_orphan_endpoints(&fail);
    g_assert_cmpuint(swept, >=, 1);
    g_assert_cmpuint(fail, ==, 0);

                                    
    const gchar *probe_owned[]  = { "ip", "netns", "exec", "ovl00042", "true", NULL };
    const gchar *probe_orphan[] = { "ip", "netns", "exec", "ovl000aa", "true", NULL };
    const gchar *probe_other[]  = { "ip", "netns", "exec", "pcvtestns", "true", NULL };
    g_assert_true(pcv_spawn_sync_timeout((const gchar *const *)probe_owned, NULL, NULL, 5, NULL));
    g_assert_false(pcv_spawn_sync_timeout((const gchar *const *)probe_orphan, NULL, NULL, 5, NULL));
    g_assert_true(pcv_spawn_sync_timeout((const gchar *const *)probe_other, NULL, NULL, 5, NULL));

            
    const gchar *del_owned[] = { "ip", "netns", "del", "ovl00042", NULL };
    const gchar *del_other[] = { "ip", "netns", "del", "pcvtestns", NULL };
    (void)pcv_spawn_sync_timeout((const gchar *const *)del_owned, NULL, NULL, 10, NULL);
    (void)pcv_spawn_sync_timeout((const gchar *const *)del_other, NULL, NULL, 10, NULL);
    pcv_tenant_overlay_reset_for_test();
    pcv_security_store_close();
    g_unlink(dbpath);
    gchar *wal = g_strdup_printf("%s-wal", dbpath);
    gchar *shm = g_strdup_printf("%s-shm", dbpath);
    g_unlink(wal); g_unlink(shm);
    g_free(wal); g_free(shm); g_free(dbpath);
}

void
test_tenant_overlay_register(void)
{
    g_test_add_func("/tenant_overlay/ciphertext_only_on_bridge",
                    test_overlay_ciphertext_only_on_bridge);
                                                                          
    g_test_add_func("/tenant_overlay/guest_confinement",
                    test_overlay_guest_confinement);
    g_test_add_func("/tenant_overlay/overlay_dhcp",
                    test_overlay_dhcp);
    g_test_add_func("/tenant_overlay/ip_alloc", test_overlay_ip_alloc);
                                                                    
    g_test_add_func("/tenant_overlay/rehydrate", test_overlay_rehydrate);
    g_test_add("/tenant_overlay/key_roundtrip", OverlayKeyFixture, NULL,
               overlay_key_fixture_setup, test_overlay_key_roundtrip,
               overlay_key_fixture_teardown);
    g_test_add("/tenant_overlay/attach_detach_mesh", OverlayMeshFixture, NULL,
               overlay_mesh_fixture_setup, test_overlay_attach_detach_mesh,
               overlay_mesh_fixture_teardown);

                                         
    g_test_add("/tenant_overlay/on_vm_gone_cleanup",
               OverlayRootFixture,
               "pcv-overlay-vm-gone-XXXXXX",
               overlay_root_fixture_setup,
               test_overlay_on_vm_gone_cleanup,
               overlay_root_fixture_teardown);

                                      
    g_test_add_func("/tenant_overlay/endpoint_down_result",
                    test_overlay_endpoint_down_result);

                                                            
    g_test_add_func("/tenant_overlay/detach_db_first_retry",
                    test_overlay_detach_db_first_retry);
    g_test_add_func("/tenant_overlay/detach_partial_cleanup_quarantine",
                    test_overlay_detach_partial_cleanup_quarantine);

                                            
    g_test_add_func("/tenant_overlay/attach_cross_tenant_unique",
                    test_overlay_attach_cross_tenant_unique);

                                                          
    g_test_add_func("/tenant_overlay/on_vm_gone_detach_all",
                    test_overlay_on_vm_gone_detach_all);

                                              
    g_test_add_func("/tenant_overlay/rpc_missing_tenant",
                    test_rpc_validate_missing_tenant);
    g_test_add_func("/tenant_overlay/rpc_null_params",
                    test_rpc_validate_null_params);
    g_test_add_func("/tenant_overlay/rpc_empty_tenant",
                    test_rpc_validate_empty_tenant);
    g_test_add_func("/tenant_overlay/rpc_invalid_tenant_charset",
                    test_rpc_validate_invalid_tenant_charset);
    g_test_add_func("/tenant_overlay/rpc_tenant_too_long",
                    test_rpc_validate_tenant_too_long);
    g_test_add_func("/tenant_overlay/rpc_valid_tenant_only",
                    test_rpc_validate_valid_tenant_only);
    g_test_add_func("/tenant_overlay/rpc_require_vm_missing",
                    test_rpc_validate_require_vm_missing);
    g_test_add_func("/tenant_overlay/rpc_require_vm_invalid_charset",
                    test_rpc_validate_require_vm_invalid_charset);
    g_test_add_func("/tenant_overlay/rpc_valid_tenant_and_vm",
                    test_rpc_validate_valid_tenant_and_vm);

                                                         
    g_test_add_func("/tenant_overlay/slot_exhausted_rollback",
                    test_overlay_slot_exhausted_rollback);
    g_test_add_func("/tenant_overlay/subnet_full",
                    test_overlay_subnet_full);
    g_test_add_func("/tenant_overlay/attach_preclean_fail_quarantine",
                    test_overlay_attach_preclean_fail_quarantine);

                                
    g_test_add("/tenant_overlay/mesh_n3_consistency",
               OverlayRootFixture,
               "pcv-overlay-mesh-n3-XXXXXX",
               overlay_root_fixture_setup,
               test_overlay_mesh_n3_consistency,
               overlay_root_fixture_teardown);
    g_test_add("/tenant_overlay/attach_preclean_stale",
               OverlayRootFixture,
               "pcv-overlay-preclean-stale-XXXXXX",
               overlay_root_fixture_setup,
               test_overlay_attach_preclean_stale,
               overlay_root_fixture_teardown);
    g_test_add("/tenant_overlay/detach_kernel_quarantine",
               OverlayRootFixture,
               "pcv-overlay-kernel-quarantine-XXXXXX",
               overlay_root_fixture_setup,
               test_overlay_detach_kernel_quarantine,
               overlay_root_fixture_teardown);

                                     
    g_test_add_func("/tenant_overlay/list_member_vms",
                    test_overlay_list_member_vms);
    g_test_add_func("/tenant_overlay/ep_name_pattern",
                    test_overlay_ep_name_pattern);
    g_test_add_func("/tenant_overlay/sweep_orphan_endpoints",
                    test_overlay_sweep_orphan_endpoints);
}
