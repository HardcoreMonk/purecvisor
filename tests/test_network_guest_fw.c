                                                                                           
                                                                              
                                                                                      
                                                                          
                                                                                       
                                                                  
                                                      
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

#include "modules/network/network_firewall.h"
#include "modules/network/network_firewall_host.h"
#include "modules/network/network_firewall_nft.h"
#include "utils/pcv_config.h"
#include "utils/pcv_spawn.h"

                                        
static PcvGuestFwSnapshot *
_gfw_sample(const gchar *const order[3])
{
    PcvGuestFwSnapshot *snap = pcv_guest_fw_snapshot_new();
    for (gint i = 0; i < 3; i++) {
        if (g_strcmp0(order[i], "r") == 0) pcv_guest_fw_snapshot_add(snap, "pcvgfr0", "routed", "10.93.0.1/24");
        if (g_strcmp0(order[i], "n") == 0) pcv_guest_fw_snapshot_add(snap, "pcvgfn0", "nat", "10.91.0.1/24");
        if (g_strcmp0(order[i], "i") == 0) pcv_guest_fw_snapshot_add(snap, "pcvgfi0", "isolated", "10.92.0.1/24");
    }
    return snap;
}

                                                      
static const gchar GOLDEN[] =
    "add table inet purecvisor\n"
    "add set inet purecvisor guest_nat_ifaces { type ifname; }\n"
    "add set inet purecvisor guest_routed_ifaces { type ifname; }\n"
    "add set inet purecvisor guest_isolated_ifaces { type ifname; }\n"
    "add set inet purecvisor guest_ifaces { type ifname; }\n"
    "add chain inet purecvisor postrouting { type nat hook postrouting priority srcnat; }\n"
    "add chain inet purecvisor forward { type filter hook forward priority filter; }\n"
    "add chain inet purecvisor guest_input { type filter hook input priority filter; policy accept; }\n"
    "flush chain inet purecvisor postrouting\n"
    "flush chain inet purecvisor forward\n"
    "flush chain inet purecvisor guest_input\n"
    "flush set inet purecvisor guest_nat_ifaces\n"
    "flush set inet purecvisor guest_routed_ifaces\n"
    "flush set inet purecvisor guest_isolated_ifaces\n"
    "flush set inet purecvisor guest_ifaces\n"
    "add element inet purecvisor guest_nat_ifaces { \"pcvgfn0\" }\n"
    "add element inet purecvisor guest_routed_ifaces { \"pcvgfr0\" }\n"
    "add element inet purecvisor guest_isolated_ifaces { \"pcvgfi0\" }\n"
    "add element inet purecvisor guest_ifaces { \"pcvgfi0\", \"pcvgfn0\", \"pcvgfr0\" }\n"
    "add rule inet purecvisor guest_input iifname @guest_ifaces ct state established,related accept\n"
    "add rule inet purecvisor guest_input iifname @guest_ifaces udp dport 67 accept\n"
    "add rule inet purecvisor guest_input iifname @guest_ifaces udp dport 547 accept\n"
    "add rule inet purecvisor guest_input iifname @guest_ifaces icmpv6 type { nd-router-solicit, nd-neighbor-solicit, nd-neighbor-advert } accept\n"
    "add rule inet purecvisor guest_input iifname @guest_ifaces fib daddr . iif type local udp dport 53 accept\n"
    "add rule inet purecvisor guest_input iifname @guest_ifaces fib daddr . iif type local tcp dport 53 accept\n"
    "add rule inet purecvisor guest_input iifname @guest_ifaces fib daddr . iif type local icmp type echo-request accept\n"
    "add rule inet purecvisor guest_input iifname @guest_ifaces fib daddr . iif type local icmpv6 type echo-request accept\n"
    "add rule inet purecvisor guest_input iifname @guest_ifaces drop\n"
    "add rule inet purecvisor forward iifname \"pcvgfi0\" oifname \"pcvgfi0\" accept\n"
    "add rule inet purecvisor forward iifname \"pcvgfn0\" oifname \"pcvgfn0\" accept\n"
    "add rule inet purecvisor forward iifname \"pcvgfr0\" oifname \"pcvgfr0\" accept\n"
    "add rule inet purecvisor forward iifname @guest_isolated_ifaces drop\n"
    "add rule inet purecvisor forward oifname @guest_isolated_ifaces drop\n"
    "add rule inet purecvisor forward iifname @guest_ifaces oifname @guest_ifaces drop\n"
    "add rule inet purecvisor forward ct state established,related accept\n"
    "add rule inet purecvisor forward iifname != @guest_ifaces oifname @guest_nat_ifaces ct status dnat accept\n"
    "add rule inet purecvisor forward oifname @guest_nat_ifaces drop\n"
    "add rule inet purecvisor postrouting oifname != \"pcvgfn0\" ip saddr 10.91.0.0/24 masquerade\n";

static void
test_gfw_build_golden(void)
{
    static const gchar *const order[3] = {"r", "n", "i"};
    g_autoptr(PcvGuestFwSnapshot) snap = _gfw_sample(order);
    g_autofree gchar *script = pcv_guest_fw_build_script(snap);
    g_assert_cmpstr(script, ==, GOLDEN);
}

static void
test_gfw_build_order_independent(void)
{
    static const gchar *const orders[][3] = {
        {"n", "i", "r"}, {"i", "r", "n"}, {"r", "i", "n"}, {"n", "r", "i"}, {"i", "n", "r"},
    };
    for (gsize k = 0; k < G_N_ELEMENTS(orders); k++) {
        g_autoptr(PcvGuestFwSnapshot) snap = _gfw_sample(orders[k]);
        g_autofree gchar *script = pcv_guest_fw_build_script(snap);
        g_assert_cmpstr(script, ==, GOLDEN);
    }
}

                                                                      
static gboolean
_gfw_order_ok(const gchar *script)
{
    static const gchar *const seq[] = {
        "guest_input iifname @guest_ifaces ct state established,related accept",
        "guest_input iifname @guest_ifaces udp dport 67 accept",
        "guest_input iifname @guest_ifaces udp dport 547 accept",
        "guest_input iifname @guest_ifaces icmpv6 type { nd-router-solicit",
        "guest_input iifname @guest_ifaces fib daddr . iif type local udp dport 53 accept",
        "guest_input iifname @guest_ifaces fib daddr . iif type local icmp type echo-request accept",
        "guest_input iifname @guest_ifaces drop",
        "forward iifname \"pcvgfr0\" oifname \"pcvgfr0\" accept",
        "forward iifname @guest_isolated_ifaces drop",
        "forward oifname @guest_isolated_ifaces drop",
        "forward iifname @guest_ifaces oifname @guest_ifaces drop",
        "forward ct state established,related accept",
        "forward iifname != @guest_ifaces oifname @guest_nat_ifaces ct status dnat accept",
        "forward oifname @guest_nat_ifaces drop",
    };
    const gchar *prev = script;
    for (gsize k = 0; k < G_N_ELEMENTS(seq); k++) {
        const gchar *at = strstr(script, seq[k]);
        if (!at || at < prev)
            return FALSE;
        prev = at;
    }
    return TRUE;
}

static void
test_gfw_build_rule_order(void)
{
    static const gchar *const order[3] = {"i", "n", "r"};
    g_autoptr(PcvGuestFwSnapshot) snap = _gfw_sample(order);
    g_autofree gchar *script = pcv_guest_fw_build_script(snap);
    g_assert_true(_gfw_order_ok(script));

                                                                     
    const gchar *f3 = "add rule inet purecvisor forward iifname @guest_ifaces oifname @guest_ifaces drop\n";
    const gchar *f4 = "add rule inet purecvisor forward ct state established,related accept\n";
    GString *mutated = g_string_new(script);
    g_assert_cmpint(g_string_replace(mutated, f3, "", 1), ==, 1);
    gchar *pos = strstr(mutated->str, f4);
    g_assert_nonnull(pos);
    g_string_insert(mutated, (gssize)(pos - mutated->str) + (gssize)strlen(f4), f3);
    g_assert_false(_gfw_order_ok(mutated->str));
    g_string_free(mutated, TRUE);
}

                                                                                           
static void
test_gfw_build_owned_objects_only(void)
{
    static const gchar *const order[3] = {"n", "i", "r"};
    g_autoptr(PcvGuestFwSnapshot) snap = _gfw_sample(order);
    g_autofree gchar *script = pcv_guest_fw_build_script(snap);
    static const gchar *const owned[] = {
        "flush chain inet purecvisor postrouting", "flush chain inet purecvisor forward",
        "flush chain inet purecvisor guest_input", "flush set inet purecvisor guest_nat_ifaces",
        "flush set inet purecvisor guest_routed_ifaces", "flush set inet purecvisor guest_isolated_ifaces",
        "flush set inet purecvisor guest_ifaces",
    };
    g_auto(GStrv) lines = g_strsplit(script, "\n", -1);
    for (gchar **l = lines; *l; l++) {
        if (!g_str_has_prefix(*l, "flush") && !g_str_has_prefix(*l, "delete"))
            continue;
        gboolean ok = FALSE;
        for (gsize k = 0; k < G_N_ELEMENTS(owned); k++)
            ok = ok || g_strcmp0(*l, owned[k]) == 0;
        g_assert_true(ok);
    }
    g_assert_null(strstr(script, "inet purecvisor input"));
    g_assert_null(strstr(script, " ips"));
    g_assert_null(strstr(script, "pcv_blocked"));
    g_assert_null(strstr(script, "table inet purecvisor\ndelete"));
    g_assert_null(strstr(script, "flush ruleset"));
}

static void
test_gfw_build_empty_and_unknown_mode(void)
{
    g_autoptr(PcvGuestFwSnapshot) snap = pcv_guest_fw_snapshot_new();
    pcv_guest_fw_snapshot_add(snap, "pcvbr9", "bridge", NULL);                                
    g_autofree gchar *script = pcv_guest_fw_build_script(snap);
    g_assert_null(strstr(script, "add element"));                                 
    g_assert_null(strstr(script, "masquerade"));
    g_assert_null(strstr(script, "pcvbr9"));
    g_assert_nonnull(strstr(script, "guest_input iifname @guest_ifaces drop\n"));
    g_assert_nonnull(strstr(script, "forward oifname @guest_nat_ifaces drop\n"));
}

static void
test_gfw_cidr_to_subnet(void)
{
    GError *err = NULL;
    g_autofree gchar *s = pcv_guest_fw_cidr_to_subnet("10.10.10.1/24", &err);
    g_assert_no_error(err);
    g_assert_cmpstr(s, ==, "10.10.10.0/24");
    g_assert_null(pcv_guest_fw_cidr_to_subnet(NULL, &err));
    g_assert_nonnull(err);
    g_clear_error(&err);
    g_assert_null(pcv_guest_fw_cidr_to_subnet("10.0.0.1", &err));               
    g_assert_nonnull(err);
    g_clear_error(&err);
    g_assert_null(pcv_guest_fw_cidr_to_subnet("fd00::1/64", &err));                   
    g_assert_nonnull(err);
    g_clear_error(&err);
}

static gboolean
_has(GPtrArray *a, const gchar *s)
{
    for (guint i = 0; i < a->len; i++)
        if (g_strcmp0(g_ptr_array_index(a, i), s) == 0)
            return TRUE;
    return FALSE;
}

static void
test_host_fw_plan_ufw_modes(void)
{
    g_autoptr(GPtrArray) nat = pcv_host_fw_plan(PCV_HOST_FW_UFW, "br0", "nat", FALSE);
    g_assert_cmpuint(nat->len, ==, 5);
    g_assert_true(_has(nat, "ufw route allow in on br0"));
    g_assert_true(_has(nat, "ufw allow in on br0 to any port 67 proto udp"));
    g_assert_true(_has(nat, "ufw allow in on br0 to any port 53 proto udp"));
    g_assert_true(_has(nat, "ufw allow in on br0 to any port 53 proto tcp"));
    g_assert_true(_has(nat, "ufw allow in on br0 to any port 547 proto udp"));
    g_assert_false(_has(nat, "ufw route allow out on br0"));                                            
    g_assert_false(_has(nat, "ufw allow in on br0"));                                  

    g_autoptr(GPtrArray) routed = pcv_host_fw_plan(PCV_HOST_FW_UFW, "br0", "routed", FALSE);
    g_assert_cmpuint(routed->len, ==, 6);
    g_assert_true(_has(routed, "ufw route allow out on br0"));                     

    g_autoptr(GPtrArray) iso = pcv_host_fw_plan(PCV_HOST_FW_UFW, "br0", "isolated", FALSE);
    g_assert_cmpuint(iso->len, ==, 4);
    g_assert_false(_has(iso, "ufw route allow in on br0"));

                                                                                                             
    g_autoptr(GPtrArray) rm = pcv_host_fw_plan(PCV_HOST_FW_UFW, "br0", "nat", TRUE);
    g_assert_true(_has(rm, "ufw --force route delete allow in on br0"));
    g_assert_false(_has(rm, "ufw --force delete route allow in on br0"));
    g_assert_true(_has(rm, "ufw --force delete allow in on br0 to any port 547 proto udp"));
}

static void
test_host_fw_plan_iptables_modes(void)
{
    g_autoptr(GPtrArray) nat = pcv_host_fw_plan(PCV_HOST_FW_IPTABLES_DROP, "br0", "nat", FALSE);
    g_assert_cmpuint(nat->len, ==, 6);
    g_assert_true(_has(nat, "iptables -I FORWARD -i br0 -j ACCEPT"));
    g_assert_true(_has(nat, "iptables -I FORWARD -o br0 -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT"));
    g_assert_true(_has(nat, "iptables -I INPUT -i br0 -p icmp --icmp-type echo-request -j ACCEPT"));

    g_autoptr(GPtrArray) routed = pcv_host_fw_plan(PCV_HOST_FW_IPTABLES_DROP, "br0", "routed", FALSE);
    g_assert_true(_has(routed, "iptables -I FORWARD -o br0 -j ACCEPT"));
    g_assert_false(_has(routed, "iptables -I FORWARD -o br0 -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT"));

    g_autoptr(GPtrArray) iso = pcv_host_fw_plan(PCV_HOST_FW_IPTABLES_DROP, "br0", "isolated", TRUE);
    g_assert_cmpuint(iso->len, ==, 4);
    g_assert_true(_has(iso, "iptables -D INPUT -i br0 -p udp --dport 67 -j ACCEPT"));

    g_autoptr(GPtrArray) none = pcv_host_fw_plan(PCV_HOST_FW_IPTABLES_DROP, "br0", NULL, FALSE);
    g_assert_cmpuint(none->len, ==, 0);
    g_autoptr(GPtrArray) open = pcv_host_fw_plan(PCV_HOST_FW_OPEN, "br0", "nat", FALSE);
    g_assert_cmpuint(open->len, ==, 0);
    g_autoptr(GPtrArray) fwd = pcv_host_fw_plan(PCV_HOST_FW_FIREWALLD, "br0", "nat", FALSE);
    g_assert_cmpuint(fwd->len, ==, 0);
}

                                                   
static const gchar UFW_LEGACY[] =
    "Added user rules (see 'ufw status' for running firewall):\n"
    "ufw allow 22/tcp\n"
    "ufw route allow in on br0\n"
    "ufw route allow out on br0\n"
    "ufw allow in on br0\n"
    "ufw allow in on br0x\n"
    "ufw   allow in on br0 to any port 8080 proto tcp\n";

static void
test_host_fw_ufw_diff_legacy_rules(void)
{
    GPtrArray *del = NULL, *add = NULL;
    pcv_host_fw_ufw_diff(UFW_LEGACY, "br0", "nat", &del, &add);
    g_assert_true(_has(del, "ufw --force route delete allow out on br0"));            
    g_assert_true(_has(del, "ufw --force delete allow in on br0"));
    g_assert_cmpuint(del->len, ==, 2);
    g_assert_false(_has(add, "ufw route allow in on br0"));                     
    g_assert_true(_has(add, "ufw allow in on br0 to any port 67 proto udp"));
    g_assert_cmpuint(add->len, ==, 4);
    g_ptr_array_unref(del);
    g_ptr_array_unref(add);
}

static void
test_host_fw_ufw_diff_keeps_operator_rules(void)
{
    GPtrArray *del = NULL, *add = NULL;
    pcv_host_fw_ufw_diff(UFW_LEGACY, "br0", NULL, &del, &add);
    for (guint i = 0; i < del->len; i++) {
        const gchar *c = g_ptr_array_index(del, i);
        g_assert_null(strstr(c, "8080"));                        
        g_assert_null(strstr(c, "br0x"));                 
        g_assert_null(strstr(c, "22/tcp"));
    }
    g_ptr_array_unref(del);
    g_ptr_array_unref(add);

                                                                                   
    static const gchar narrow[] =
        "ufw route allow in on pcvnat0\n"
        "ufw route allow in on eno1 out on pcvnat0 to 10.78.0.5 port 22 proto tcp\n";
    const gchar *modes[] = {"nat", NULL};
    for (guint m = 0; m < G_N_ELEMENTS(modes); m++) {
        pcv_host_fw_ufw_diff(narrow, "pcvnat0", modes[m], &del, &add);
        for (guint i = 0; i < del->len; i++)
            g_assert_null(strstr(g_ptr_array_index(del, i), "10.78.0.5"));
        g_assert_cmpuint(del->len, ==, modes[m] ? 0 : 1);                             
        g_assert_false(_has(add, "ufw route allow out on pcvnat0"));                           
        g_ptr_array_unref(del);
        g_ptr_array_unref(add);
    }
}

static void
test_host_fw_ufw_diff_remove_all_and_converged(void)
{
    GPtrArray *del = NULL, *add = NULL;
    pcv_host_fw_ufw_diff(UFW_LEGACY, "br0", NULL, &del, &add);
    g_assert_cmpuint(del->len, ==, 3);                                     
    g_assert_cmpuint(add->len, ==, 0);
    g_assert_true(_has(del, "ufw --force route delete allow in on br0"));
    g_ptr_array_unref(del);
    g_ptr_array_unref(add);

    const gchar *converged =
        "ufw route allow in on br0\n"
        "ufw allow in on br0 to any port 67 proto udp\n"
        "ufw allow in on br0 to any port 53 proto udp\n"
        "ufw allow in on br0 to any port 53 proto tcp\n"
        "ufw allow in on br0 to any port 547 proto udp\n";
    pcv_host_fw_ufw_diff(converged, "br0", "nat", &del, &add);
    g_assert_cmpuint(del->len, ==, 0);
    g_assert_cmpuint(add->len, ==, 0);
    g_ptr_array_unref(del);
    g_ptr_array_unref(add);
}

static void
test_host_fw_iptables_known_union(void)
{
    g_autoptr(GPtrArray) known = pcv_host_fw_iptables_known("br0", "-D");
    g_assert_true(_has(known, "iptables -D FORWARD -o br0 -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT"));
    g_assert_true(_has(known, "iptables -D FORWARD -o br0 -j ACCEPT"));
    g_assert_cmpuint(known->len, ==, 7);                                      
}

                                                                
                                                                                                
                                                                                      
                                                                               

                          
extern void network_firewall_set_paths_for_test(const gchar *rundir, const gchar *sys_class_net);

typedef struct {
    gchar *dir, *rundir, *sysnet, *bin, *nft_log, *saved_path, *cfg;
} GfwFixture;

static void
_gfw_write_exec(const gchar *dir, const gchar *name, const gchar *body)
{
    g_autofree gchar *p = g_build_filename(dir, name, NULL);
    g_assert_true(g_file_set_contents(p, body, -1, NULL));
    g_assert_cmpint(g_chmod(p, 0755), ==, 0);
}

                                                                                            
static void
_gfw_set_nft(GfwFixture *f, const gchar *fail_if)
{
    g_autofree gchar *body = NULL;
    if (!fail_if)
        body = g_strdup_printf("#!/bin/sh\ncat > '%s'\nexit 0\n", f->nft_log);
    else if (!*fail_if)
        body = g_strdup_printf("#!/bin/sh\ncat > '%s'\nexit 1\n", f->nft_log);
    else
        body = g_strdup_printf("#!/bin/sh\ncat > '%s'\ngrep -qF -- '%s' '%s' && exit 1\nexit 0\n",
                               f->nft_log, fail_if, f->nft_log);
    _gfw_write_exec(f->bin, "nft", body);
}

static void
_gfw_begin(GfwFixture *f)
{
    pcv_spawn_launcher_shutdown();                                              
    f->dir = g_dir_make_tmp("pcv-gfw-XXXXXX", NULL);
    g_assert_nonnull(f->dir);
    f->rundir = g_build_filename(f->dir, "run", NULL);
    f->sysnet = g_build_filename(f->dir, "sys", NULL);
    f->bin = g_build_filename(f->dir, "bin", NULL);
    f->nft_log = g_build_filename(f->dir, "nft.log", NULL);
    g_assert_cmpint(g_mkdir_with_parents(f->rundir, 0700), ==, 0);
    g_assert_cmpint(g_mkdir_with_parents(f->sysnet, 0700), ==, 0);
    g_assert_cmpint(g_mkdir_with_parents(f->bin, 0700), ==, 0);
    _gfw_write_exec(f->bin, "sysctl", "#!/bin/sh\nexit 0\n");                          
    _gfw_set_nft(f, NULL);
    f->saved_path = g_strdup(g_getenv("PATH"));
    g_autofree gchar *path = g_strdup_printf("%s:%s", f->bin, f->saved_path ? f->saved_path : "");
    g_setenv("PATH", path, TRUE);
                                                                         
    f->cfg = g_build_filename(f->dir, "daemon.conf", NULL);
    g_assert_true(g_file_set_contents(f->cfg, "[network]\nfirewall_integration = off\n", -1, NULL));
    g_setenv("PCV_CONFIG_PATH", f->cfg, TRUE);
    pcv_config_init();
    network_firewall_set_paths_for_test(f->rundir, f->sysnet);
}

                                            
static void
_gfw_rm_tree(const gchar *path)
{
    if (g_file_test(path, G_FILE_TEST_IS_DIR) && !g_file_test(path, G_FILE_TEST_IS_SYMLINK)) {
        GDir *d = g_dir_open(path, 0, NULL);
        const gchar *n;
        while (d && (n = g_dir_read_name(d))) {
            g_autofree gchar *child = g_build_filename(path, n, NULL);
            _gfw_rm_tree(child);
        }
        if (d)
            g_dir_close(d);
        g_rmdir(path);
    } else {
        g_remove(path);
    }
}

static void
_gfw_end(GfwFixture *f)
{
    network_firewall_set_paths_for_test(NULL, NULL);
    pcv_config_shutdown();
    g_setenv("PCV_CONFIG_PATH", "/nonexistent/pcv-test-isolated.conf", TRUE);                        
    if (f->saved_path) g_setenv("PATH", f->saved_path, TRUE); else g_unsetenv("PATH");
    _gfw_rm_tree(f->dir);
    g_free(f->dir); g_free(f->rundir); g_free(f->sysnet); g_free(f->bin);
    g_free(f->nft_log); g_free(f->saved_path); g_free(f->cfg);
}

static void
_gfw_add_bridge(GfwFixture *f, const gchar *br)
{
    g_autofree gchar *p = g_build_filename(f->sysnet, br, NULL);
    g_assert_cmpint(g_mkdir_with_parents(p, 0700), ==, 0);
}

static void
_gfw_write_meta(GfwFixture *f, const gchar *br, const gchar *mode, const gchar *cidr)
{
    g_autofree gchar *name = g_strdup_printf("dnsmasq-%s.meta", br);
    g_autofree gchar *p = g_build_filename(f->rundir, name, NULL);
    g_autofree gchar *body = g_strdup_printf("{\"mode\":\"%s\",\"cidr\":\"%s\"}", mode, cidr);
    g_assert_true(g_file_set_contents(p, body, -1, NULL));
}

static gchar *
_gfw_read(const gchar *path)
{
    gchar *s = NULL;
    if (!g_file_get_contents(path, &s, NULL, NULL))
        return g_strdup("");
    return s;
}

static gchar *
_gfw_read_list(GfwFixture *f)
{
    g_autofree gchar *p = g_build_filename(f->rundir, "guest-fw.json", NULL);
    return _gfw_read(p);
}

static void
test_gfw_list_upsert_replaces_mode(void)
{
    GfwFixture f = {0};
    _gfw_begin(&f);
    _gfw_add_bridge(&f, "pcvgft0");
    GError *err = NULL;
    g_assert_true(network_firewall_setup_nat("pcvgft0", "10.81.0.1/24", &err));
    g_assert_no_error(err);
    g_assert_true(network_firewall_setup_isolated("pcvgft0", "10.81.0.1/24", &err));
    g_assert_no_error(err);
    g_autofree gchar *list = _gfw_read_list(&f);
    g_assert_nonnull(strstr(list, "\"mode\":\"isolated\""));
    g_assert_null(strstr(list, "\"mode\":\"nat\""));
    g_autofree gchar *last = _gfw_read(f.nft_log);
    g_assert_nonnull(strstr(last, "guest_isolated_ifaces { \"pcvgft0\" }"));
    g_assert_null(strstr(last, "masquerade"));
    g_autofree gchar *mode_path = g_build_filename(f.rundir, "guest-fw.json", NULL);
    GStatBuf st;
    g_assert_cmpint(g_stat(mode_path, &st), ==, 0);
    g_assert_cmpint(st.st_mode & 0777, ==, 0600);
    _gfw_end(&f);
}

static void
test_gfw_list_remove(void)
{
    GfwFixture f = {0};
    _gfw_begin(&f);
    _gfw_add_bridge(&f, "pcvgft0");
    _gfw_add_bridge(&f, "pcvgft1");
    g_assert_true(network_firewall_setup_nat("pcvgft0", "10.81.0.1/24", NULL));
    g_assert_true(network_firewall_setup_routed("pcvgft1", "10.82.0.1/24", NULL));
    g_assert_true(network_firewall_teardown("pcvgft0", NULL));
    g_autofree gchar *last = _gfw_read(f.nft_log);
    g_assert_null(strstr(last, "pcvgft0"));
    g_assert_nonnull(strstr(last, "guest_routed_ifaces { \"pcvgft1\" }"));
    g_autofree gchar *list = _gfw_read_list(&f);
    g_assert_null(strstr(list, "pcvgft0"));
    _gfw_end(&f);
}

static void
test_gfw_list_apply_failure_restores(void)
{
    GfwFixture f = {0};
    _gfw_begin(&f);
    _gfw_add_bridge(&f, "pcvgft0");
    g_assert_true(network_firewall_setup_nat("pcvgft0", "10.81.0.1/24", NULL));
    g_autofree gchar *before = _gfw_read_list(&f);
    _gfw_set_nft(&f, "");                                       
    GError *err = NULL;
    g_assert_false(network_firewall_setup_isolated("pcvgft0", "10.81.0.1/24", &err));
    g_assert_nonnull(err);
    g_clear_error(&err);
    g_autofree gchar *after = _gfw_read_list(&f);
    g_assert_cmpstr(after, ==, before);                    
    _gfw_set_nft(&f, NULL);
    g_assert_true(network_firewall_reconcile(NULL));
    g_autofree gchar *last = _gfw_read(f.nft_log);
    g_assert_nonnull(strstr(last, "guest_nat_ifaces { \"pcvgft0\" }"));                     
    _gfw_end(&f);
}

static void
test_gfw_list_teardown_apply_failure_still_removes(void)
{
    GfwFixture f = {0};
    _gfw_begin(&f);
    _gfw_add_bridge(&f, "pcvgft0");
    g_assert_true(network_firewall_setup_nat("pcvgft0", "10.81.0.1/24", NULL));
    _gfw_set_nft(&f, "");
    g_assert_true(network_firewall_teardown("pcvgft0", NULL));                     
    g_autofree gchar *list = _gfw_read_list(&f);
    g_assert_null(strstr(list, "pcvgft0"));                                          
    _gfw_set_nft(&f, NULL);
    g_assert_true(network_firewall_reconcile(NULL));
    g_autofree gchar *last = _gfw_read(f.nft_log);
    g_assert_null(strstr(last, "pcvgft0"));                                       
    _gfw_end(&f);
}

static void
test_gfw_list_seed_from_meta(void)
{
    GfwFixture f = {0};
    _gfw_begin(&f);
    _gfw_add_bridge(&f, "pcvgfa0");
    _gfw_add_bridge(&f, "pcvgfc0");
    _gfw_add_bridge(&f, "pcvgfd0");
    _gfw_write_meta(&f, "pcvgfa0", "nat", "10.81.0.1/24");
    _gfw_write_meta(&f, "pcvgfb0", "isolated", "10.82.0.1/24");                    
    _gfw_write_meta(&f, "pcvgfc0", "bridge", "");                                    
    _gfw_write_meta(&f, "pcvgfd0", "routed", "10.84.0.1/24");
    g_assert_true(network_firewall_reconcile(NULL));
    g_autofree gchar *last = _gfw_read(f.nft_log);
    g_assert_nonnull(strstr(last, "guest_ifaces { \"pcvgfa0\", \"pcvgfd0\" }"));
    g_assert_null(strstr(last, "pcvgfb0"));
    g_assert_null(strstr(last, "pcvgfc0"));
    g_autofree gchar *list = _gfw_read_list(&f);
    g_assert_nonnull(strstr(list, "pcvgfa0"));
    _gfw_end(&f);
}

static void
test_gfw_list_file_wins_over_meta(void)
{
    GfwFixture f = {0};
    _gfw_begin(&f);
    _gfw_add_bridge(&f, "pcvgfx0");
    _gfw_write_meta(&f, "pcvgfx0", "nat", "10.85.0.1/24");
    g_autofree gchar *p = g_build_filename(f.rundir, "guest-fw.json", NULL);
    g_assert_true(g_file_set_contents(p,
        "{\"version\":1,\"nets\":[{\"bridge\":\"pcvgfx0\",\"mode\":\"isolated\",\"cidr\":\"10.85.0.1/24\"}]}", -1, NULL));
    g_assert_true(network_firewall_reconcile(NULL));
    g_autofree gchar *last = _gfw_read(f.nft_log);
    g_assert_nonnull(strstr(last, "guest_isolated_ifaces { \"pcvgfx0\" }"));
    g_assert_null(strstr(last, "masquerade"));
    _gfw_end(&f);
}

static void
test_gfw_list_corrupt_file_falls_back_to_meta(void)
{
    GfwFixture f = {0};
    _gfw_begin(&f);
    _gfw_add_bridge(&f, "pcvgfx0");
    _gfw_write_meta(&f, "pcvgfx0", "nat", "10.85.0.1/24");
    g_autofree gchar *p = g_build_filename(f.rundir, "guest-fw.json", NULL);
    g_assert_true(g_file_set_contents(p, "{not json", -1, NULL));
    g_assert_true(network_firewall_reconcile(NULL));
    g_autofree gchar *last = _gfw_read(f.nft_log);
    g_assert_nonnull(strstr(last, "guest_nat_ifaces { \"pcvgfx0\" }"));           
    _gfw_end(&f);
}

static void
test_gfw_list_tampered_entries_dropped(void)
{
    GfwFixture f = {0};
    _gfw_begin(&f);
    _gfw_add_bridge(&f, "pcvgfx0");
    g_autofree gchar *p = g_build_filename(f.rundir, "guest-fw.json", NULL);
    g_assert_true(g_file_set_contents(p,
        "{\"version\":1,\"nets\":["
        "{\"bridge\":\"pcvgfx0\",\"mode\":\"nat\",\"cidr\":\"10.85.0.1/24\\nflush ruleset\"},"
        "{\"bridge\":\"pcv\\nflush ruleset\",\"mode\":\"nat\",\"cidr\":\"10.86.0.1/24\"},"
        "{\"bridge\":\"pcvgfx0\",\"mode\":\"bogus\",\"cidr\":\"10.85.0.1/24\"}]}", -1, NULL));
    g_assert_true(network_firewall_reconcile(NULL));
    g_autofree gchar *last = _gfw_read(f.nft_log);
    g_assert_null(strstr(last, "flush ruleset"));
    g_assert_null(strstr(last, "add element"));                          
    g_autofree gchar *list = _gfw_read_list(&f);
    g_assert_null(strstr(list, "bogus"));                                     
    _gfw_end(&f);
}

                                                                                     
                                                                              
static void
test_gfw_list_invalid_inputs_rejected(void)
{
    GfwFixture f = {0};
    _gfw_begin(&f);
    _gfw_add_bridge(&f, "pcvgfx0");
    static const struct { const gchar *mode, *br, *cidr; } bad[] = {
        {"nat",      "pcvgfx0",    "8.8.8.1/24"},                                    
        {"nat",      "pcvgfx0",    "10.85.0.1/24\nflush ruleset"},               
        {"nat",      "pcvgfx0",    "fd00::1/64"},                                                     
        {"nat",      "pcvgfx0",    NULL},
        {"isolated", "pcvgfx0",    "10.85.0.1/24\nflush ruleset"},
        {"routed",   "pcvgfx0",    "8.8.8.1/24"},
        {"routed",   "pcvgfnone0", "10.86.0.1/24"},                                          
        {"nat",      "pcv\"x",     "10.86.0.1/24"},                                   
    };
    for (gsize k = 0; k < G_N_ELEMENTS(bad); k++) {
        GError *err = NULL;
        gboolean ok;
        if (g_strcmp0(bad[k].mode, "nat") == 0)
            ok = network_firewall_setup_nat(bad[k].br, bad[k].cidr, &err);
        else if (g_strcmp0(bad[k].mode, "isolated") == 0)
            ok = network_firewall_setup_isolated(bad[k].br, bad[k].cidr, &err);
        else
            ok = network_firewall_setup_routed(bad[k].br, bad[k].cidr, &err);
        g_assert_false(ok);
        g_assert_nonnull(err);
        g_clear_error(&err);
    }
    g_assert_false(g_file_test(f.nft_log, G_FILE_TEST_EXISTS));                          
    g_autofree gchar *list_path = g_build_filename(f.rundir, "guest-fw.json", NULL);
    g_assert_false(g_file_test(list_path, G_FILE_TEST_EXISTS));

                                                             
    _gfw_add_bridge(&f, "pcvgfy0");
    _gfw_add_bridge(&f, "pcv.bad0");
    _gfw_write_meta(&f, "pcvgfx0", "nat", "10.85.0.1/24\\nflush ruleset");
    _gfw_write_meta(&f, "pcvgfy0", "routed", "8.8.8.1/24");
    _gfw_write_meta(&f, "pcv.bad0", "nat", "10.87.0.1/24");
    g_assert_true(network_firewall_reconcile(NULL));
    g_autofree gchar *last = _gfw_read(f.nft_log);
    g_assert_null(strstr(last, "flush ruleset"));
    g_assert_null(strstr(last, "8.8.8."));
    g_assert_null(strstr(last, "add element"));                        
    _gfw_end(&f);
}

typedef struct { const gchar *br; const gchar *cidr; gboolean ok; } GfwThreadArg;

static gpointer
_gfw_thread(gpointer data)
{
    GfwThreadArg *a = data;
    a->ok = network_firewall_setup_nat(a->br, a->cidr, NULL);
    return NULL;
}

static void
test_gfw_list_concurrent_creates_keep_both(void)
{
    GfwFixture f = {0};
    _gfw_begin(&f);
    _gfw_add_bridge(&f, "pcvgfa0");
    _gfw_add_bridge(&f, "pcvgfb0");
    GfwThreadArg a = {"pcvgfa0", "10.81.0.1/24", FALSE};
    GfwThreadArg b = {"pcvgfb0", "10.82.0.1/24", FALSE};
    GThread *ta = g_thread_new("gfw-a", _gfw_thread, &a);
    GThread *tb = g_thread_new("gfw-b", _gfw_thread, &b);
    g_thread_join(ta);
    g_thread_join(tb);
    g_assert_true(a.ok);
    g_assert_true(b.ok);
    g_autofree gchar *list = _gfw_read_list(&f);
    g_assert_nonnull(strstr(list, "\"pcvgfa0\""));
    g_assert_nonnull(strstr(list, "\"pcvgfb0\""));
    g_autofree gchar *last = _gfw_read(f.nft_log);
    g_assert_nonnull(strstr(last, "guest_nat_ifaces { \"pcvgfa0\", \"pcvgfb0\" }"));
    _gfw_end(&f);
}

                                                                                          
                                                                                
                                                                                   

                                                                                                   
static void
test_firewall_isolated_nft_failure_propagates(void)
{
    GfwFixture f = {0};
    _gfw_begin(&f);
    _gfw_add_bridge(&f, "pcvbrtest0");
    _gfw_set_nft(&f, "");
    GError *err = NULL;
    g_assert_false(network_firewall_setup_isolated("pcvbrtest0", "10.9.9.1/24", &err));
    g_assert_nonnull(err);
    g_clear_error(&err);
    _gfw_end(&f);
}

                                          
static void
test_firewall_isolated_nft_success_returns_true(void)
{
    GfwFixture f = {0};
    _gfw_begin(&f);
    _gfw_add_bridge(&f, "pcvbrtest0");
    GError *err = NULL;
    g_assert_true(network_firewall_setup_isolated("pcvbrtest0", "10.9.9.1/24", &err));
    g_assert_no_error(err);
    _gfw_end(&f);
}

                                                                         
static void
test_firewall_isolated_drop_rule_failure_propagates(void)
{
    GfwFixture f = {0};
    _gfw_begin(&f);
    _gfw_add_bridge(&f, "pcvbrtest0");
    _gfw_set_nft(&f, "iifname @guest_isolated_ifaces drop");
    GError *err = NULL;
    g_assert_false(network_firewall_setup_isolated("pcvbrtest0", "10.9.9.1/24", &err));
    g_assert_nonnull(err);
    g_clear_error(&err);
    _gfw_end(&f);
}

void
test_network_guest_fw_register(void)
{
    g_test_add_func("/network/guest_fw/build/golden",              test_gfw_build_golden);
    g_test_add_func("/network/guest_fw/build/order_independent",   test_gfw_build_order_independent);
    g_test_add_func("/network/guest_fw/build/rule_order",          test_gfw_build_rule_order);
    g_test_add_func("/network/guest_fw/build/owned_objects_only",  test_gfw_build_owned_objects_only);
    g_test_add_func("/network/guest_fw/build/empty_and_unknown",   test_gfw_build_empty_and_unknown_mode);
    g_test_add_func("/network/guest_fw/cidr_to_subnet",            test_gfw_cidr_to_subnet);
    g_test_add_func("/network/host_fw/plan/ufw_modes",                test_host_fw_plan_ufw_modes);
    g_test_add_func("/network/host_fw/plan/iptables_modes",           test_host_fw_plan_iptables_modes);
    g_test_add_func("/network/host_fw/ufw_diff/legacy_rules",         test_host_fw_ufw_diff_legacy_rules);
    g_test_add_func("/network/host_fw/ufw_diff/keeps_operator_rules", test_host_fw_ufw_diff_keeps_operator_rules);
    g_test_add_func("/network/host_fw/ufw_diff/remove_all_converged", test_host_fw_ufw_diff_remove_all_and_converged);
    g_test_add_func("/network/host_fw/iptables_known_union",          test_host_fw_iptables_known_union);
    g_test_add_func("/network/guest_fw/list/upsert_replaces_mode",            test_gfw_list_upsert_replaces_mode);
    g_test_add_func("/network/guest_fw/list/remove",                          test_gfw_list_remove);
    g_test_add_func("/network/guest_fw/list/apply_failure_restores",          test_gfw_list_apply_failure_restores);
    g_test_add_func("/network/guest_fw/list/teardown_apply_failure_still_removes", test_gfw_list_teardown_apply_failure_still_removes);
    g_test_add_func("/network/guest_fw/list/seed_from_meta",                  test_gfw_list_seed_from_meta);
    g_test_add_func("/network/guest_fw/list/file_wins_over_meta",             test_gfw_list_file_wins_over_meta);
    g_test_add_func("/network/guest_fw/list/corrupt_file_falls_back_to_meta", test_gfw_list_corrupt_file_falls_back_to_meta);
    g_test_add_func("/network/guest_fw/list/tampered_entries_dropped",        test_gfw_list_tampered_entries_dropped);
    g_test_add_func("/network/guest_fw/list/invalid_inputs_rejected",         test_gfw_list_invalid_inputs_rejected);
    g_test_add_func("/network/guest_fw/list/concurrent_creates_keep_both",    test_gfw_list_concurrent_creates_keep_both);
    g_test_add_func("/network/net2/isolated_nft_failure_propagates",          test_firewall_isolated_nft_failure_propagates);
    g_test_add_func("/network/net2/isolated_nft_success_returns_true",        test_firewall_isolated_nft_success_returns_true);
    g_test_add_func("/network/net2/drop_rule_failure_propagates",             test_firewall_isolated_drop_rule_failure_propagates);
}
