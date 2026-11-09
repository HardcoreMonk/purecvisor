                                                                                     
                                                                                 
                                                                            
                                                                      
                      
   
                                 
                                                                     
   
#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <string.h>

#include "api/uds_server.h"
#include "modules/network/network_manager.h"
#include "modules/network/network_qos_rules.h"
#include "modules/network/pcv_qos.h"
#include "utils/pcv_spawn.h"

                                                     
void pcv_network_qos_rules_test_set_paths(const gchar *rules_path, const gchar *sysfs_net_root);

                                                                    
void pcv_test_uds_response_reset(void);
const gchar *pcv_test_last_uds_response(void);

typedef struct {
    gchar *base;
    gchar *rules;
    gchar *sysnet;
    gchar *tc_log;
    gchar *saved_path;
} RulesFx;

static void
_write_exec(const gchar *path, const gchar *body)
{
    g_assert_true(g_file_set_contents(path, body, -1, NULL));
    g_assert_cmpint(g_chmod(path, 0755), ==, 0);
}

static void
_fx_setup(RulesFx *f, gconstpointer unused)
{
    (void)unused;
    pcv_spawn_launcher_shutdown();
    f->base = g_dir_make_tmp("pcv-qos-rules-XXXXXX", NULL);
    g_assert_nonnull(f->base);
    f->rules = g_build_filename(f->base, "qos_rules.json", NULL);
    f->sysnet = g_build_filename(f->base, "sysnet", NULL);
    f->tc_log = g_build_filename(f->base, "tc.log", NULL);
    g_assert_cmpint(g_mkdir(f->sysnet, 0700), ==, 0);

                                                              
                                                           
    gchar *bin = g_build_filename(f->base, "bin", NULL);
    g_assert_cmpint(g_mkdir(bin, 0700), ==, 0);
    gchar *quoted = g_shell_quote(f->tc_log);
    gchar *tc_body = g_strdup_printf("#!/bin/sh\nprintf '%%s\\n' \"$*\" >> %s\nexit 0\n", quoted);
    gchar *tc = g_build_filename(bin, "tc", NULL);
    gchar *ip = g_build_filename(bin, "ip", NULL);
    gchar *virsh = g_build_filename(bin, "virsh", NULL);
    _write_exec(tc, tc_body);
    _write_exec(ip, "#!/bin/sh\nexit 0\n");
    _write_exec(virsh,
        "#!/bin/sh\n"
        "printf ' Interface   Type     Source   Model    MAC\\n"
        "---------------------------------------------------\\n"
        " vnet11      bridge   pcvbr0   virtio   52:54:00:00:00:11\\n'\n");
    f->saved_path = g_strdup(g_getenv("PATH"));
    gchar *mock_path = g_strdup_printf("%s:%s", bin, f->saved_path ? f->saved_path : "");
    g_setenv("PATH", mock_path, TRUE);
    g_free(mock_path); g_free(virsh); g_free(ip); g_free(tc);
    g_free(tc_body); g_free(quoted); g_free(bin);

    pcv_network_qos_rules_test_set_paths(f->rules, f->sysnet);
    pcv_test_uds_response_reset();
}

static void
_remove_tree(const gchar *path)
{
    GDir *dir = g_dir_open(path, 0, NULL);
    if (dir) {
        const gchar *name;
        while ((name = g_dir_read_name(dir))) {
            gchar *child = g_build_filename(path, name, NULL);
            if (g_file_test(child, G_FILE_TEST_IS_DIR) &&
                !g_file_test(child, G_FILE_TEST_IS_SYMLINK))
                _remove_tree(child);
            else
                g_remove(child);
            g_free(child);
        }
        g_dir_close(dir);
    }
    g_rmdir(path);
}

static void
_fx_teardown(RulesFx *f, gconstpointer unused)
{
    (void)unused;
    pcv_network_qos_rules_test_set_paths(NULL, NULL);
    if (f->saved_path) g_setenv("PATH", f->saved_path, TRUE); else g_unsetenv("PATH");
    pcv_spawn_launcher_shutdown();
    _remove_tree(f->base);
    g_free(f->saved_path); g_free(f->tc_log); g_free(f->sysnet);
    g_free(f->rules); g_free(f->base);
}

static void
_touch_iface(RulesFx *f, const gchar *iface)
{
    gchar *path = g_build_filename(f->sysnet, iface, NULL);
    g_assert_true(g_file_set_contents(path, "", 0, NULL));
    g_free(path);
}

static gchar *
_slurp(const gchar *path)
{
    gchar *text = NULL;
    if (!g_file_get_contents(path, &text, NULL, NULL))
        return g_strdup("");
    return text;
}

static gboolean
_file_has(const gchar *path, const gchar *needle)
{
    gchar *text = _slurp(path);
    gboolean found = strstr(text, needle) != NULL;
    g_free(text);
    return found;
}

static JsonObject *
_params(const gchar *json)
{
    JsonParser *parser = json_parser_new();
    g_assert_true(json_parser_load_from_data(parser, json, -1, NULL));
    JsonObject *object = json_object_ref(json_node_get_object(json_parser_get_root(parser)));
    g_object_unref(parser);
    return object;
}

                                                          
static void
test_save_skips_vnet(RulesFx *f, gconstpointer unused)
{
    (void)unused;
    g_assert_false(pcv_network_qos_rules_save("vnet3", "ingress", 5, 256));
    g_assert_false(_file_has(f->rules, "vnet3"));
    g_assert_true(pcv_network_qos_rules_save("pcvbr9", "egress", 100, 256));
    g_assert_true(_file_has(f->rules, "pcvbr9:egress"));
    g_assert_false(_file_has(f->rules, "vnet3"));
}

                                                                     
                                                       
                                                           
static void
test_vnet_prefix_bridge_is_saved(RulesFx *f, gconstpointer unused)
{
    (void)unused;
    g_assert_true(pcv_network_qos_rules_save("vnetlab", "egress", 100, 256));
    g_assert_true(pcv_network_qos_rules_save("vnet3x", "egress", 100, 256));
    g_assert_false(pcv_network_qos_rules_save("vnet12", "egress", 100, 256));
    g_assert_true(_file_has(f->rules, "vnetlab:egress"));
    g_assert_true(_file_has(f->rules, "vnet3x:egress"));
    g_assert_false(_file_has(f->rules, "vnet12"));

    _touch_iface(f, "vnetlab");
    g_assert_true(g_file_set_contents(f->rules,
        "{\"vnet3:ingress\":{\"interface\":\"vnet3\",\"direction\":\"ingress\","
        "\"rate_mbps\":5,\"burst_kb\":256},"
        "\"vnetlab:egress\":{\"interface\":\"vnetlab\",\"direction\":\"egress\","
        "\"rate_mbps\":100,\"burst_kb\":256}}", -1, NULL));
    pcv_qos_restore();
    g_assert_true(_file_has(f->tc_log, "class replace dev vnetlab"));
    g_assert_true(_file_has(f->rules, "vnetlab:egress"));
    g_assert_false(_file_has(f->rules, "vnet3:ingress"));
}

                                                                          
                                                            
                                                                   
static void
test_save_reports_write_failure(RulesFx *f, gconstpointer unused)
{
    (void)unused;
    gchar *blocker = g_build_filename(f->base, "blocker", NULL);
    gchar *unwritable = g_build_filename(blocker, "qos_rules.json", NULL);
    g_assert_true(g_file_set_contents(blocker, "", 0, NULL));
    pcv_network_qos_rules_test_set_paths(unwritable, f->sysnet);

    g_assert_false(pcv_network_qos_rules_save("pcvbr9", "egress", 100, 256));
    JsonObject *params = _params("{\"interface\":\"pcvbr9\",\"rate_mbps\":7}");
    handle_network_qos_set(params, "1", NULL, NULL);
    json_object_unref(params);
    g_assert_nonnull(strstr(pcv_test_last_uds_response(), "\"persisted\":false"));
    g_assert_nonnull(strstr(pcv_test_last_uds_response(), "\"status\":\"ok\""));
    g_assert_true(_file_has(f->tc_log, "class replace dev pcvbr9"));

    pcv_test_uds_response_reset();
    pcv_network_qos_rules_test_set_paths(f->rules, f->sysnet);
    params = _params("{\"interface\":\"pcvbr9\",\"rate_mbps\":7}");
    handle_network_qos_set(params, "2", NULL, NULL);
    json_object_unref(params);
    g_assert_nonnull(strstr(pcv_test_last_uds_response(), "\"persisted\":true"));
    g_free(unwritable);
    g_free(blocker);
}

                                                     
                                          
static void
test_restore_drops_saved_vnet(RulesFx *f, gconstpointer unused)
{
    (void)unused;
    _touch_iface(f, "vnet3");
    _touch_iface(f, "pcvbr9");
    g_assert_true(g_file_set_contents(f->rules,
        "{\"vnet3:ingress\":{\"interface\":\"vnet3\",\"direction\":\"ingress\","
        "\"rate_mbps\":5,\"burst_kb\":256},"
        "\"pcvbr9:egress\":{\"interface\":\"pcvbr9\",\"direction\":\"egress\","
        "\"rate_mbps\":100,\"burst_kb\":256}}", -1, NULL));

    pcv_qos_restore();

    g_assert_false(_file_has(f->tc_log, "dev vnet3"));
    g_assert_true(_file_has(f->tc_log, "class replace dev pcvbr9"));
    g_assert_false(_file_has(f->rules, "vnet3"));
    g_assert_true(_file_has(f->rules, "pcvbr9:egress"));
}

                                                                    
                                                   
static void
test_restore_skips_d09_managed_ingress(RulesFx *f, gconstpointer unused)
{
    (void)unused;
    PcvQosSla sla = { .tenant = "qrt", .vm = "qrvm", .min_mbps = 0, .max_mbps = 10, .burst_kb = 256 };
    GError *error = NULL;
    g_assert_true(pcv_qos_ensure_root(1000, &error));
    g_assert_true(pcv_qos_apply_vm("hvethqr0", &sla, &error));
    g_assert_no_error(error);
    g_assert_true(pcv_qos_iface_is_managed("hvethqr0"));

    _touch_iface(f, "hvethqr0");
    g_assert_true(g_file_set_contents(f->rules,
        "{\"hvethqr0:ingress\":{\"interface\":\"hvethqr0\",\"direction\":\"ingress\","
        "\"rate_mbps\":5,\"burst_kb\":256}}", -1, NULL));
    g_assert_true(g_file_set_contents(f->tc_log, "", 0, NULL));

    pcv_qos_restore();

    g_assert_false(_file_has(f->tc_log, "qdisc del dev hvethqr0 ingress"));
    g_assert_false(_file_has(f->tc_log, "police"));
    g_assert_true(_file_has(f->rules, "hvethqr0:ingress"));

    g_assert_true(pcv_qos_remove_vm("hvethqr0", "qrt", "qrvm", &error));
    g_assert_no_error(error);
    g_assert_false(pcv_qos_iface_is_managed("hvethqr0"));

                                                       
    g_assert_true(g_file_set_contents(f->tc_log, "", 0, NULL));
    pcv_qos_restore();
    g_assert_true(_file_has(f->tc_log, "qdisc del dev hvethqr0 ingress"));
    pcv_qos_uplink_clear();
}

                                                                            
                                                              
static void
test_remove_handler_gates_d09_managed(RulesFx *f, gconstpointer unused)
{
    (void)unused;
    PcvQosSla sla = { .tenant = "qrt", .vm = "qrvm2", .min_mbps = 0, .max_mbps = 10, .burst_kb = 256 };
    GError *error = NULL;
    g_assert_true(pcv_qos_ensure_root(1000, &error));
    g_assert_true(pcv_qos_apply_vm("hvethqr1", &sla, &error));
    g_assert_no_error(error);
    g_assert_true(g_file_set_contents(f->tc_log, "", 0, NULL));

    JsonObject *ingress = _params("{\"interface\":\"hvethqr1\",\"direction\":\"ingress\"}");
    handle_network_qos_remove(ingress, "1", NULL, NULL);
    json_object_unref(ingress);
    g_assert_nonnull(strstr(pcv_test_last_uds_response(), "\"error\""));
    g_assert_nonnull(strstr(pcv_test_last_uds_response(), "D09"));
    g_assert_false(_file_has(f->tc_log, "qdisc del"));

    pcv_test_uds_response_reset();
    JsonObject *all = _params("{\"interface\":\"hvethqr1\",\"direction\":\"all\"}");
    handle_network_qos_remove(all, "2", NULL, NULL);
    json_object_unref(all);
    g_assert_nonnull(strstr(pcv_test_last_uds_response(), "\"error\""));
    g_assert_false(_file_has(f->tc_log, "qdisc del"));

    pcv_test_uds_response_reset();
    JsonObject *egress = _params("{\"interface\":\"hvethqr1\",\"direction\":\"egress\"}");
    handle_network_qos_remove(egress, "3", NULL, NULL);
    json_object_unref(egress);
    g_assert_nonnull(strstr(pcv_test_last_uds_response(), "\"result\""));
    g_assert_true(_file_has(f->tc_log, "qdisc del dev hvethqr1 root"));

    pcv_test_uds_response_reset();
    JsonObject *other = _params("{\"interface\":\"pcvbr9\",\"direction\":\"ingress\"}");
    handle_network_qos_remove(other, "4", NULL, NULL);
    json_object_unref(other);
    g_assert_nonnull(strstr(pcv_test_last_uds_response(), "\"result\""));
    g_assert_true(_file_has(f->tc_log, "qdisc del dev pcvbr9 ingress"));

    g_assert_true(pcv_qos_remove_vm("hvethqr1", "qrt", "qrvm2", &error));
    g_assert_no_error(error);
    pcv_qos_uplink_clear();
}

                                                                       
static void
test_set_by_vm_name_applies_session_only(RulesFx *f, gconstpointer unused)
{
    (void)unused;
    JsonObject *by_vm = _params("{\"vm_name\":\"qrvm3\",\"rate_mbps\":7}");
    handle_network_qos_set(by_vm, "1", NULL, NULL);
    json_object_unref(by_vm);
    g_assert_nonnull(strstr(pcv_test_last_uds_response(), "vnet11"));
    g_assert_nonnull(strstr(pcv_test_last_uds_response(), "persisted"));
    g_assert_true(_file_has(f->tc_log, "class replace dev vnet11"));
    g_assert_false(_file_has(f->rules, "vnet11"));

    pcv_test_uds_response_reset();
    JsonObject *bridge = _params("{\"interface\":\"pcvbr9\",\"rate_mbps\":7}");
    handle_network_qos_set(bridge, "2", NULL, NULL);
    json_object_unref(bridge);
    g_assert_true(_file_has(f->tc_log, "class replace dev pcvbr9"));
    g_assert_true(_file_has(f->rules, "pcvbr9:egress"));
}

void test_network_qos_rules_register(void);

void
test_network_qos_rules_register(void)
{
#define QR_ADD(path, fn) \
    g_test_add("/network/qos_rules/" path, RulesFx, NULL, _fx_setup, fn, _fx_teardown)
    QR_ADD("save_skips_vnet", test_save_skips_vnet);
    QR_ADD("vnet_prefix_bridge_is_saved", test_vnet_prefix_bridge_is_saved);
    QR_ADD("save_reports_write_failure", test_save_reports_write_failure);
    QR_ADD("restore_drops_saved_vnet", test_restore_drops_saved_vnet);
    QR_ADD("restore_skips_d09_managed_ingress", test_restore_skips_d09_managed_ingress);
    QR_ADD("remove_handler_gates_d09_managed", test_remove_handler_gates_d09_managed);
    QR_ADD("set_by_vm_name_applies_session_only", test_set_by_vm_name_applies_session_only);
#undef QR_ADD
}
