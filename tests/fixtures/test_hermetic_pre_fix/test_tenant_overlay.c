#include <glib.h>

static gboolean
_wg_available(void)
{
    const gchar *probe[] = { "wg", "--version", NULL };
    return pcv_spawn_sync_timeout((const gchar *const *)probe,
                                  NULL, NULL, 5, NULL);
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

void test_tenant_overlay_pre_fix_register(void) {
    g_test_add_func("/tenant_overlay/overlay_dhcp",
                    test_overlay_dhcp);
}
