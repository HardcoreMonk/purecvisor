                           
                                                                    
                                                                  
                                                           
  
                       
                                                       
                                                        
   
   
                                         
                                                                         
   
#include <glib.h>
#include <glib/gstdio.h>
#include <unistd.h>
#include <string.h>

#include "modules/network/pcv_shared_bridge.h"
#include "utils/pcv_spawn.h"

typedef struct {
    gchar bridge[16];
    gchar physical[16];
    gchar lan_peer[16];
    gchar guest_port[16];
    gchar guest_peer[16];
    gchar portal_bridge[16];
    gchar portal_tc[16];
    gchar lan_ns[48];
    gchar guest_ns[48];
    gchar *dnsmasq_pidfile;
} SharedFixture;

static gboolean
_shared_run(const gchar *const argv[], gchar **stdout_out, gchar **stderr_out)
{
    GError *error = NULL;
    gboolean ok = pcv_spawn_sync(argv, stdout_out, stderr_out, &error);
    if (!ok && error) g_test_message("command failed: %s", error->message);
    g_clear_error(&error);
    return ok;
}

static gboolean
_shared_ip(const gchar *a1, const gchar *a2, const gchar *a3,
           const gchar *a4, const gchar *a5, const gchar *a6,
           const gchar *a7, const gchar *a8, const gchar *a9)
{
    const gchar *argv[] = {"ip", a1, a2, a3, a4, a5, a6, a7, a8, a9, NULL};
    return _shared_run(argv, NULL, NULL);
}

static gboolean
_shared_ping_host(const gchar *destination)
{
    const gchar *argv[] = {"ping", "-c", "2", "-W", "1", destination, NULL};
    return _shared_run(argv, NULL, NULL);
}

static gboolean
_shared_ping_ns(const gchar *netns, const gchar *destination)
{
    const gchar *argv[] = {
        "ip", "netns", "exec", netns,
        "ping", "-c", "2", "-W", "1", destination, NULL
    };
    return _shared_run(argv, NULL, NULL);
}

static void
_shared_fixture_init(SharedFixture *fixture)
{
    guint suffix = (guint)getpid() & 0xffffU;
    g_snprintf(fixture->bridge, sizeof(fixture->bridge), "psb%04x", suffix);
    g_snprintf(fixture->physical, sizeof(fixture->physical), "psp%04x", suffix);
    g_snprintf(fixture->lan_peer, sizeof(fixture->lan_peer), "psl%04x", suffix);
    g_snprintf(fixture->guest_port, sizeof(fixture->guest_port), "psg%04x", suffix);
    g_snprintf(fixture->guest_peer, sizeof(fixture->guest_peer), "psx%04x", suffix);
    g_snprintf(fixture->lan_ns, sizeof(fixture->lan_ns), "pcv-shared-lan-%u", suffix);
    g_snprintf(fixture->guest_ns, sizeof(fixture->guest_ns), "pcv-shared-guest-%u", suffix);
    pcv_shared_bridge_portal_names(fixture->bridge,
                                   fixture->portal_bridge, fixture->portal_tc);
    fixture->dnsmasq_pidfile = g_strdup_printf(
        "/tmp/pcv-shared-dnsmasq-%u.pid", suffix);
}

static void
_shared_fixture_cleanup(SharedFixture *fixture)
{
    GError *detach_error = NULL;
    pcv_shared_bridge_detach(fixture->physical, fixture->portal_tc, &detach_error);
    g_clear_error(&detach_error);

    gchar *pid_text = NULL;
    if (fixture->dnsmasq_pidfile
        && g_file_get_contents(fixture->dnsmasq_pidfile, &pid_text, NULL, NULL)) {
        gchar *end = NULL;
        gint64 pid = g_ascii_strtoll(pid_text, &end, 10);
        if (end != pid_text && pid > 1) {
            gchar pid_string[32];
            g_snprintf(pid_string, sizeof(pid_string), "%" G_GINT64_FORMAT, pid);
            const gchar *kill_argv[] = {"kill", pid_string, NULL};
            _shared_run(kill_argv, NULL, NULL);
        }
    }
    g_free(pid_text);
    if (fixture->dnsmasq_pidfile) g_unlink(fixture->dnsmasq_pidfile);

    const gchar *delete_guest_ns[] = {"ip", "netns", "delete", fixture->guest_ns, NULL};
    const gchar *delete_lan_ns[] = {"ip", "netns", "delete", fixture->lan_ns, NULL};
    const gchar *delete_bridge[] = {"ip", "link", "delete", fixture->bridge, NULL};
    const gchar *delete_physical[] = {"ip", "link", "delete", fixture->physical, NULL};
    _shared_run(delete_guest_ns, NULL, NULL);
    _shared_run(delete_lan_ns, NULL, NULL);
    _shared_run(delete_bridge, NULL, NULL);
    _shared_run(delete_physical, NULL, NULL);
    g_clear_pointer(&fixture->dnsmasq_pidfile, g_free);
}

static gboolean
_shared_fixture_create(SharedFixture *f)
{
    const gchar *add_lan_ns[] = {"ip", "netns", "add", f->lan_ns, NULL};
    const gchar *add_guest_ns[] = {"ip", "netns", "add", f->guest_ns, NULL};
    if (!_shared_run(add_lan_ns, NULL, NULL) || !_shared_run(add_guest_ns, NULL, NULL))
        return FALSE;
    if (!_shared_ip("link", "add", f->physical, "type", "veth", "peer", "name",
                    f->lan_peer, NULL)
        || !_shared_ip("link", "set", f->lan_peer, "netns", f->lan_ns,
                       NULL, NULL, NULL, NULL)
        || !_shared_ip("link", "add", "name", f->bridge, "type", "bridge",
                       "stp_state", "0", NULL)
        || !_shared_ip("link", "add", f->portal_bridge, "type", "veth", "peer",
                       "name", f->portal_tc, NULL)
        || !_shared_ip("link", "add", f->guest_port, "type", "veth", "peer", "name",
                       f->guest_peer, NULL)
        || !_shared_ip("link", "set", f->guest_peer, "netns", f->guest_ns,
                       NULL, NULL, NULL, NULL)
        || !_shared_ip("link", "set", f->portal_bridge, "master", f->bridge,
                       NULL, NULL, NULL, NULL)
        || !_shared_ip("link", "set", f->guest_port, "master", f->bridge,
                       NULL, NULL, NULL, NULL))
        return FALSE;

    const gchar *host_commands[][11] = {
        {"ip", "link", "set", "dev", f->bridge, "up", NULL},
        {"ip", "link", "set", "dev", f->physical, "address", "02:44:00:00:00:01", NULL},
        {"ip", "addr", "add", "198.18.44.1/24", "dev", f->physical, NULL},
        {"ip", "link", "set", "dev", f->physical, "promisc", "on", NULL},
        {"ip", "link", "set", "dev", f->physical, "up", NULL},
        {"ip", "link", "set", "dev", f->portal_bridge, "up", NULL},
        {"ip", "link", "set", "dev", f->portal_tc, "up", NULL},
        {"ip", "link", "set", "dev", f->guest_port, "up", NULL},
    };
    for (guint i = 0; i < G_N_ELEMENTS(host_commands); i++)
        if (!_shared_run(host_commands[i], NULL, NULL)) return FALSE;

    const gchar *lan_setup[][14] = {
        {"ip", "netns", "exec", f->lan_ns, "ip", "link", "set", "lo", "up", NULL},
        {"ip", "netns", "exec", f->lan_ns, "ip", "link", "set", f->lan_peer,
         "name", "eth0", NULL},
        {"ip", "netns", "exec", f->lan_ns, "ip", "link", "set", "eth0",
         "address", "02:44:00:00:00:02", NULL},
        {"ip", "netns", "exec", f->lan_ns, "ip", "addr", "add", "198.18.44.2/24",
         "dev", "eth0", NULL},
        {"ip", "netns", "exec", f->lan_ns, "ip", "link", "set", "eth0", "up", NULL},
    };
    const gchar *guest_setup[][14] = {
        {"ip", "netns", "exec", f->guest_ns, "ip", "link", "set", "lo", "up", NULL},
        {"ip", "netns", "exec", f->guest_ns, "ip", "link", "set", f->guest_peer,
         "name", "eth0", NULL},
        {"ip", "netns", "exec", f->guest_ns, "ip", "link", "set", "eth0",
         "address", "02:44:00:00:00:03", NULL},
        {"ip", "netns", "exec", f->guest_ns, "ip", "addr", "add", "198.18.44.3/24",
         "dev", "eth0", NULL},
        {"ip", "netns", "exec", f->guest_ns, "ip", "link", "set", "eth0", "up", NULL},
    };
    for (guint i = 0; i < G_N_ELEMENTS(lan_setup); i++)
        if (!_shared_run(lan_setup[i], NULL, NULL)) return FALSE;
    for (guint i = 0; i < G_N_ELEMENTS(guest_setup); i++)
        if (!_shared_run(guest_setup[i], NULL, NULL)) return FALSE;
    return TRUE;
}

static gboolean
_shared_dhcp_probe(SharedFixture *f)
{
    const gchar *delete_addr[] = {
        "ip", "netns", "exec", f->guest_ns,
        "ip", "addr", "del", "198.18.44.3/24", "dev", "eth0", NULL
    };
    if (!_shared_run(delete_addr, NULL, NULL)) return FALSE;
    gchar *pid_option = g_strdup_printf("--pid-file=%s", f->dnsmasq_pidfile);
    const gchar *dnsmasq[] = {
        "ip", "netns", "exec", f->lan_ns, "dnsmasq",
        "--interface=eth0", "--bind-interfaces", "--port=0", "--no-hosts", "--no-resolv",
        "--dhcp-range=198.18.44.50,198.18.44.60,255.255.255.0,1m",
        "--dhcp-option=3", "--leasefile-ro", pid_option, NULL
    };
    gboolean ok = _shared_run(dnsmasq, NULL, NULL);
    g_free(pid_option);
    if (!ok) return FALSE;
    const gchar *client[] = {
        "ip", "netns", "exec", f->guest_ns, "busybox", "udhcpc",
        "-i", "eth0", "-n", "-q", "-t", "5", "-T", "1", "-s", "/bin/true", NULL
    };
    gchar *stdout_text = NULL;
    gchar *stderr_text = NULL;
    ok = _shared_run(client, &stdout_text, &stderr_text);
    gboolean leased = ok && ((stdout_text && strstr(stdout_text, "198.18.44."))
                             || (stderr_text && strstr(stderr_text, "198.18.44.")));
    g_test_message("udhcpc stdout=%s stderr=%s",
                   stdout_text ? stdout_text : "", stderr_text ? stderr_text : "");
    g_free(stdout_text);
    g_free(stderr_text);
    return leased;
}

static void
test_shared_bridge_packet_path_root(void)
{
    if (geteuid() != 0 || g_strcmp0(g_getenv("PCV_SHARED_BRIDGE_LIVE"), "1") != 0) {
        g_test_skip("root + PCV_SHARED_BRIDGE_LIVE=1 required");
        return;
    }
    if (!g_file_test("build/bpf/pcv_shared_bridge.bpf.o", G_FILE_TEST_IS_REGULAR)) {
        g_test_skip("run make bpf first");
        return;
    }

    SharedFixture fixture = {0};
    _shared_fixture_init(&fixture);
    _shared_fixture_cleanup(&fixture);
    _shared_fixture_init(&fixture);

    GError *error = NULL;
    gboolean created = _shared_fixture_create(&fixture);
    gboolean baseline_host_lan = created && _shared_ping_host("198.18.44.2");
    gboolean prepared = created
        && pcv_shared_bridge_bpf_prepare("build/bpf", &error);
    if (!prepared && error) g_test_message("prepare failed: %s", error->message);
    g_clear_error(&error);
    const guint8 host_mac[6] = {0x02, 0x44, 0x00, 0x00, 0x00, 0x01};
    gboolean attached = prepared && pcv_shared_bridge_attach(
        fixture.physical, fixture.portal_tc, host_mac, 1500, 44, &error);
    if (!attached && error) g_test_message("attach failed: %s", error->message);
    g_clear_error(&error);

    gboolean host_lan_during = attached && _shared_ping_host("198.18.44.2");
    gboolean host_to_guest = attached && _shared_ping_host("198.18.44.3");
    gboolean guest_to_host = attached && _shared_ping_ns(fixture.guest_ns, "198.18.44.1");
    gboolean lan_to_guest = attached && _shared_ping_ns(fixture.lan_ns, "198.18.44.3");
    gboolean guest_to_lan = attached && _shared_ping_ns(fixture.guest_ns, "198.18.44.2");
    gboolean dhcp = attached && _shared_dhcp_probe(&fixture);

    gboolean detached = attached && pcv_shared_bridge_detach(
        fixture.physical, fixture.portal_tc, &error);
    if (!detached && error) g_test_message("detach failed: %s", error->message);
    g_clear_error(&error);
    gboolean host_lan_after = detached && _shared_ping_host("198.18.44.2");
    _shared_fixture_cleanup(&fixture);

    g_assert_true(created);
    g_assert_true(baseline_host_lan);
    g_assert_true(prepared);
    g_assert_true(attached);
    g_assert_true(host_lan_during);
    g_assert_true(host_to_guest);
    g_assert_true(guest_to_host);
    g_assert_true(lan_to_guest);
    g_assert_true(guest_to_lan);
    g_assert_true(dhcp);
    g_assert_true(detached);
    g_assert_true(host_lan_after);
}

void
test_shared_bridge_integration_register(void)
{
    g_test_add_func("/network/shared_bridge_packet_path",
                    test_shared_bridge_packet_path_root);
}
