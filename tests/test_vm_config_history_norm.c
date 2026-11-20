#include <glib.h>
#include <string.h>

#include "../src/modules/virt/vm_config_history_norm.h"

#define FIX "tests/fixtures/vm_config_history/"
#define MARK "S3cr3t-PCV"

static const guint8 k_salt[4] = {1, 2, 3, 4};

static gchar *
load(const gchar *name)
{
    gchar *path = g_strconcat(FIX, name, NULL);
    gchar *text = NULL;
    g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
    g_free(path);
    return text;
}

static gchar *
replace(const gchar *s, const gchar *from, const gchar *to)
{
    gchar **parts = g_strsplit(s, from, -1);
    g_assert_cmpuint(g_strv_length(parts), >=, 2);
    gchar *out = g_strjoinv(to, parts);
    g_strfreev(parts);
    return out;
}

static gchar *
ins_before(const gchar *s, const gchar *anchor, const gchar *insert)
{
    gchar *to = g_strconcat(insert, anchor, NULL);
    gchar *out = replace(s, anchor, to);
    g_free(to);
    return out;
}

static void
norm_ok(const gchar *inactive, const gchar *live, PcvVmCfgSnapshot *s)
{
    const gchar *err = NULL;
    g_assert_true(pcv_vm_cfg_normalize(inactive, strlen(inactive), live, live ? strlen(live) : 0, TRUE, k_salt,
                                       sizeof k_salt, s, &err));
    g_assert_null(err);
}

static GString *g_logged;

static void
capture_log(const gchar *domain, GLogLevelFlags level, const gchar *message, gpointer ud)
{
    (void)domain;
    (void)level;
    (void)ud;
    g_string_append(g_logged, message ? message : "");
}

static void
test_allowlist_pcv_vm(void)
{
    gchar *xml = load("pcv_vm.xml");
    gchar *expected = load("pcv_vm.expected.json");
    g_strstrip(expected);
    PcvVmCfgSnapshot s;
    norm_ok(xml, NULL, &s);
    g_assert_cmpstr(s.config_json, ==, expected);
    g_assert_cmpstr(s.owner, ==, "alice");
    g_assert_cmpstr(s.name, ==, "web");
    g_assert_cmpstr(s.uuid, ==, "6f1c2d3e-4a5b-4c6d-8e9f-0a1b2c3d4e5f");
    g_assert_cmpint(s.other_device_count, ==, 5);
    g_assert_null(s.live_overlay_json);
    g_assert_null(s.live_digest);
    g_assert_cmpuint(strlen(s.config_digest), ==, 64);
    pcv_vm_cfg_snapshot_clear(&s);
    g_free(xml);
    g_free(expected);
}

static void
test_runtime_invariant(void)
{
    gchar *stopped = load("pcv_vm.xml"), *running = load("pcv_vm_inactive_running.xml");
    gchar *la = load("live_a.xml"), *lb = load("live_b.xml"), *lp = load("live_pinned.xml");
    PcvVmCfgSnapshot s1, s2, a, b, p;
    norm_ok(stopped, NULL, &s1);
    norm_ok(running, NULL, &s2);
    g_assert_cmpstr(s1.config_digest, ==, s2.config_digest);
    norm_ok(running, la, &a);
    norm_ok(running, lb, &b);
    g_assert_cmpstr(a.config_digest, ==, s1.config_digest);
    g_assert_cmpstr(a.live_digest, ==, b.live_digest);
    g_assert_cmpstr(a.live_overlay_json, ==, "{}");
    g_assert_null(strstr(a.live_overlay_json, "current_kib"));
    g_assert_null(strstr(a.live_overlay_json, "backingStore"));
    g_assert_null(strstr(a.live_overlay_json, "base.qcow2"));
    norm_ok(running, lp, &p);
    g_assert_nonnull(strstr(p.live_overlay_json, "cputune.vcpupin"));
    g_assert_cmpstr(p.live_digest, !=, a.live_digest);
    pcv_vm_cfg_snapshot_clear(&s1);
    pcv_vm_cfg_snapshot_clear(&s2);
    pcv_vm_cfg_snapshot_clear(&a);
    pcv_vm_cfg_snapshot_clear(&b);
    pcv_vm_cfg_snapshot_clear(&p);
    g_free(stopped);
    g_free(running);
    g_free(la);
    g_free(lb);
    g_free(lp);
}

static void
test_redaction_corpus(void)
{
    gchar *xml = load("secrets_corpus.xml");
    g_assert_nonnull(strstr(xml, MARK));
    gchar *changed = replace(xml, MARK "-", MARK "-v2-");
    g_logged = g_string_new(NULL);
    GLogFunc prev = g_log_set_default_handler(capture_log, NULL);
    PcvVmCfgSnapshot a, b;
    norm_ok(xml, xml, &a);
    norm_ok(changed, changed, &b);
    guint count = 0;
    gchar *diff = pcv_vm_cfg_diff(a.config_json, b.config_json, 0, &count);
    gchar *ldiff = pcv_vm_cfg_diff(a.live_overlay_json, b.live_overlay_json, 0, NULL);
    const gchar *err = NULL;
    PcvVmCfgSnapshot bad;
    gchar *broken = g_strconcat(xml, "<" MARK, NULL);
    g_assert_false(pcv_vm_cfg_normalize(broken, strlen(broken) - 3, NULL, 0, FALSE, k_salt, 4, &bad, &err));
    g_log_set_default_handler(prev, NULL);
    const gchar *outputs[] = {a.config_json, b.config_json, a.live_overlay_json, b.live_overlay_json, diff, ldiff,
                              err, g_logged->str, NULL};
    for (guint i = 0; i < G_N_ELEMENTS(outputs) - 1; i++) {
        g_assert_nonnull(outputs[i]);
        g_assert_null(strstr(outputs[i], MARK));
        g_assert_null(strstr(outputs[i], "vncpass"));
    }
    g_assert_cmpuint(count, >, 0);
    g_assert_nonnull(strstr(diff, "opaque.qemu_commandline"));
    g_assert_nonnull(strstr(a.config_json, "\"disk[vda].auth\":true"));
    g_assert_nonnull(strstr(a.config_json, "\"disk[vdb].encrypted\":true"));
    pcv_vm_cfg_snapshot_clear(&a);
    pcv_vm_cfg_snapshot_clear(&b);
    g_free(diff);
    g_free(ldiff);
    g_free(broken);
    g_free(changed);
    g_free(xml);
    g_string_free(g_logged, TRUE);
}

static void
test_opaque_changes_visible(void)
{
    gchar *xml = load("secrets_corpus.xml");
    struct {
        const gchar *from, *to, *path;
    } cases[] = {
        {"ds=nocloud-net", "ds=nocloud", "opaque.cmdline"},
        {"seed=", "seed2=", "opaque.description"},
        {"name=opt/secret", "name=opt/secret2", "opaque.qemu_commandline"},
        {"token=", "token2=", "opaque.metadata_foreign"},
        {"<tpm model", "<serial type='pty'><target port='1'/></serial><tpm model", "opaque.other_devices"},
    };
    PcvVmCfgSnapshot base;
    norm_ok(xml, NULL, &base);
    for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
        gchar *v = replace(xml, cases[i].from, cases[i].to);
        PcvVmCfgSnapshot s;
        norm_ok(v, NULL, &s);
        guint count = 0;
        gchar *diff = pcv_vm_cfg_diff(base.config_json, s.config_json, 0, &count);
        gchar *needle = g_strdup_printf("\"path\":\"%s\"", cases[i].path);
        g_assert_nonnull(strstr(diff, needle));
        if (i < 4)
            g_assert_cmpuint(count, ==, 1);
        g_free(needle);
        g_free(diff);
        pcv_vm_cfg_snapshot_clear(&s);
        g_free(v);
    }
    pcv_vm_cfg_snapshot_clear(&base);
    g_free(xml);
}

static void
test_fingerprint_props(void)
{
    const guint8 s2[4] = {1, 2, 3, 5};
    gchar *a = pcv_vm_cfg_fingerprint(k_salt, 4, (const guint8 *)"abcdef", 6);
    gchar *b = pcv_vm_cfg_fingerprint(k_salt, 4, (const guint8 *)"abcdef", 6);
    gchar *c = pcv_vm_cfg_fingerprint(k_salt, 4, (const guint8 *)"abcdeg", 6);
    gchar *d = pcv_vm_cfg_fingerprint(s2, 4, (const guint8 *)"abcdef", 6);
    g_assert_cmpstr(a, ==, b);
    g_assert_cmpstr(a, !=, c);
    g_assert_cmpstr(a, !=, d);
    g_assert_cmpuint(strlen(a), ==, 32);
    for (const gchar *p = a; *p; p++)
        g_assert_true(g_ascii_isxdigit(*p));
    g_free(a);
    g_free(b);
    g_free(c);
    g_free(d);
}

static guint
diff_count(const gchar *x1, const gchar *x2, gchar **out_diff)
{
    PcvVmCfgSnapshot a, b;
    norm_ok(x1, NULL, &a);
    norm_ok(x2, NULL, &b);
    guint count = 0;
    gchar *diff = pcv_vm_cfg_diff(a.config_json, b.config_json, 0, &count);
    if (out_diff)
        *out_diff = diff;
    else
        g_free(diff);
    pcv_vm_cfg_snapshot_clear(&a);
    pcv_vm_cfg_snapshot_clear(&b);
    return count;
}

static void
test_diff_shapes(void)
{
    gchar *xml = load("pcv_vm.xml");
    const gchar *disk2 = "<disk type='file' device='disk'><driver name='qemu' type='qcow2'/>"
                         "<source file='/var/lib/pcv/data.qcow2'/><target dev='vdb' bus='virtio'/></disk>";
    const gchar *nic2 = "<interface type='bridge'><mac address='52:54:00:aa:bb:02'/><source bridge='br1'/>"
                        "<model type='virtio'/></interface>";
    gchar *with_disk = ins_before(xml, "<controller type='usb'", disk2);
    gchar *diff = NULL;
    g_assert_cmpuint(diff_count(xml, with_disk, &diff), ==, 4);
    g_assert_nonnull(strstr(diff, "\"path\":\"disk[vdb].source\""));
    g_assert_nonnull(strstr(diff, "\"before\":null"));
    g_free(diff);
    g_assert_cmpuint(diff_count(with_disk, xml, &diff), ==, 4);
    g_assert_nonnull(strstr(diff, "\"after\":null"));
    g_free(diff);

    gchar *front = ins_before(xml, "<disk type='block'", disk2);
    g_assert_cmpuint(diff_count(with_disk, front, NULL), ==, 0);

    gchar *n_after = ins_before(xml, "<serial type='pty'>", nic2);
    gchar *n_before = ins_before(xml, "<interface type='bridge'>", nic2);
    g_assert_cmpuint(diff_count(n_after, n_before, NULL), ==, 0);
    gchar *n_br = replace(n_after, "<source bridge='br1'/>", "<source bridge='br2'/>");
    g_assert_cmpuint(diff_count(n_after, n_br, &diff), ==, 1);
    g_assert_nonnull(strstr(diff, "\"path\":\"nic[52:54:00:aa:bb:02].source\""));
    g_free(diff);

    gchar *pin_swap = replace(xml, "<vcpupin vcpu='1' cpuset='3'/>\n    <vcpupin vcpu='0' cpuset='2'/>",
                              "<vcpupin vcpu='0' cpuset='2'/>\n    <vcpupin vcpu='1' cpuset='3'/>");
    g_assert_cmpuint(diff_count(xml, pin_swap, NULL), ==, 0);
    gchar *pin_new = replace(xml, "cpuset='3'", "cpuset='5'");
    g_assert_cmpuint(diff_count(xml, pin_new, &diff), ==, 1);
    g_assert_nonnull(strstr(diff, "cputune.vcpupin"));
    g_free(diff);

    gchar *mt_unit = replace(xml, "<hard_limit unit='KiB'>5242880</hard_limit>", "<hard_limit unit='MiB'>5120</hard_limit>");
    g_assert_cmpuint(diff_count(xml, mt_unit, NULL), ==, 0);
    gchar *mt_val = replace(xml, "5242880", "6291456");
    g_assert_cmpuint(diff_count(xml, mt_val, &diff), ==, 1);
    g_assert_nonnull(strstr(diff, "\"path\":\"memtune.hard_limit\""));
    g_free(diff);

    PcvVmCfgSnapshot a, b;
    norm_ok(xml, NULL, &a);
    norm_ok(with_disk, NULL, &b);
    guint count = 0;
    gchar *capped = pcv_vm_cfg_diff(a.config_json, b.config_json, 2, &count);
    g_assert_cmpuint(count, ==, 4);
    g_assert_nonnull(strstr(capped, "disk[vdb].bus"));
    g_assert_null(strstr(capped, "disk[vdb].source"));
    g_free(capped);
    pcv_vm_cfg_snapshot_clear(&a);
    pcv_vm_cfg_snapshot_clear(&b);
    g_free(mt_unit);
    g_free(mt_val);
    g_free(pin_swap);
    g_free(pin_new);
    g_free(n_after);
    g_free(n_before);
    g_free(n_br);
    g_free(front);
    g_free(with_disk);
    g_free(xml);
}

static void
test_diff_filtered(void)
{
    const gchar *before = "{\"cpu.mode\":\"a\",\"opaque.title\":\"x1\"}";
    const gchar *after = "{\"cpu.mode\":\"a\",\"opaque.title\":\"x2\",\"vcpu.max\":\"4\"}";
    guint count = 0;
    g_free(pcv_vm_cfg_diff_filtered(before, after, 0, PCV_VMCH_DIFF_ALL, &count));
    g_assert_cmpuint(count, ==, 2);
    g_free(pcv_vm_cfg_diff_filtered(before, after, 0, PCV_VMCH_DIFF_IGNORE_OPAQUE, &count));
    g_assert_cmpuint(count, ==, 1);
    g_free(pcv_vm_cfg_diff_filtered(before, after, 0, PCV_VMCH_DIFF_IGNORE_NEW_PATHS, &count));
    g_assert_cmpuint(count, ==, 1);
    g_free(pcv_vm_cfg_diff_filtered(before, after, 0, PCV_VMCH_DIFF_IGNORE_OPAQUE | PCV_VMCH_DIFF_IGNORE_NEW_PATHS,
                                    &count));
    g_assert_cmpuint(count, ==, 0);
    g_assert_null(pcv_vm_cfg_diff("not json", after, 0, &count));
    g_assert_true(pcv_vm_cfg_path_is_host_path("os.nvram.path"));
    g_assert_true(pcv_vm_cfg_path_is_host_path("nic[52:54:00:00:00:03].script"));
    g_assert_false(pcv_vm_cfg_path_is_host_path("disk[vda].source"));
}

static void
test_limits(void)
{
    gsize n = PCV_VMCH_XML_LIMIT + 1;
    gchar *big = g_malloc(n + 1);
    memset(big, ' ', n);
    memcpy(big, "<domain>", 8);
    big[n] = '\0';
    PcvVmCfgSnapshot s;
    const gchar *err = NULL;
    g_assert_false(pcv_vm_cfg_normalize(big, n, NULL, 0, FALSE, k_salt, 4, &s, &err));
    g_assert_cmpstr(err, ==, "oversize");
    g_assert_null(s.config_json);
    gchar *xml = load("pcv_vm.xml");
    g_assert_false(pcv_vm_cfg_normalize(xml, strlen(xml), big, n, FALSE, k_salt, 4, &s, &err));
    g_assert_cmpstr(err, ==, "oversize");
    const gchar *broken = "<domain><name>x</nam";
    g_assert_false(pcv_vm_cfg_normalize(broken, strlen(broken), NULL, 0, FALSE, k_salt, 4, &s, &err));
    g_assert_cmpstr(err, ==, "parse");
    const gchar *dtd = "<!DOCTYPE domain [<!ENTITY x \"" MARK "\">]><domain><name>&x;</name></domain>";
    g_assert_false(pcv_vm_cfg_normalize(dtd, strlen(dtd), NULL, 0, FALSE, k_salt, 4, &s, &err));
    g_assert_cmpstr(err, ==, "parse");
    const gchar *other = "<notdomain/>";
    g_assert_false(pcv_vm_cfg_normalize(other, strlen(other), NULL, 0, FALSE, k_salt, 4, &s, &err));
    g_assert_cmpstr(err, ==, "parse");
    g_free(xml);
    g_free(big);
}

static void
test_network_source_query_dropped(void)
{
    gchar *xml = load("pcv_vm.xml");
    gchar *x2 = ins_before(xml, "</devices>",
                           "<disk type='network' device='disk'><source protocol='http' name='/images/a.qcow2?token=PCVQMARK42'>"
                           "<host name='img.example'/></source><target dev='vdz' bus='virtio'/></disk>");
    PcvVmCfgSnapshot s;
    norm_ok(x2, NULL, &s);
    g_assert_nonnull(strstr(s.config_json, "http:/images/a.qcow2"));
    g_assert_null(strstr(s.config_json, "PCVQMARK42"));
    g_assert_null(strstr(s.config_json, "token="));
    pcv_vm_cfg_snapshot_clear(&s);
    g_free(x2);
    g_free(xml);
}

void test_vm_config_history_norm_register(void);
void
test_vm_config_history_norm_register(void)
{
    g_test_add_func("/vm_config_history/norm/allowlist_pcv_vm", test_allowlist_pcv_vm);
    g_test_add_func("/vm_config_history/norm/runtime_invariant", test_runtime_invariant);
    g_test_add_func("/vm_config_history/norm/redaction_corpus", test_redaction_corpus);
    g_test_add_func("/vm_config_history/norm/opaque_changes_visible", test_opaque_changes_visible);
    g_test_add_func("/vm_config_history/norm/fingerprint_props", test_fingerprint_props);
    g_test_add_func("/vm_config_history/norm/diff_shapes", test_diff_shapes);
    g_test_add_func("/vm_config_history/norm/diff_filtered", test_diff_filtered);
    g_test_add_func("/vm_config_history/norm/limits", test_limits);
    g_test_add_func("/vm_config_history/norm/network_source_query_dropped", test_network_source_query_dropped);
}
