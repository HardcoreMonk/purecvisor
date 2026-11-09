                                                                             
                                                                                     
                                                                
                                                     
                       
                         
  
                                            
  
                 
                                                
                                              
                                         
  
                                       
  
                                     
   

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include "purecvisor/pcv_validate.h"
#include "modules/dispatcher/handler_container_mode.h"
#include "modules/dispatcher/rpc_utils.h"
#include "modules/auth/pcv_rbac.h"

                                                            

static void test_container_image_valid(void) {
    g_assert_true(pcv_validate_container_image("ubuntu:22.04"));
    g_assert_true(pcv_validate_container_image("debian:bookworm"));
    g_assert_true(pcv_validate_container_image("alpine:3.18"));
    g_assert_true(pcv_validate_container_image("centos:9-stream"));
}

static void test_container_image_invalid(void) {
    g_assert_false(pcv_validate_container_image(NULL));
    g_assert_false(pcv_validate_container_image(""));
    g_assert_false(pcv_validate_container_image("ubuntu"));                      
    g_assert_false(pcv_validate_container_image(":22.04"));                       
    g_assert_false(pcv_validate_container_image("Ubuntu:22.04"));                      
    g_assert_false(pcv_validate_container_image("ubu ntu:22.04"));           
}

static void test_container_image_boundary(void) {
                                       
    gchar buf[256];
    memset(buf, 'a', 60);
    buf[60] = ':';
    memset(buf + 61, '1', PCV_MAX_CONTAINER_IMAGE - 61);
    buf[PCV_MAX_CONTAINER_IMAGE] = '\0';
    g_assert_true(pcv_validate_container_image(buf));

                           
    buf[PCV_MAX_CONTAINER_IMAGE] = 'x';
    buf[PCV_MAX_CONTAINER_IMAGE + 1] = '\0';
    g_assert_false(pcv_validate_container_image(buf));
}

                                                          

static void test_exec_cmd_valid(void) {
    g_assert_true(pcv_validate_exec_cmd("ls -la"));
    g_assert_true(pcv_validate_exec_cmd("echo hello"));
    g_assert_true(pcv_validate_exec_cmd("cat /etc/hostname"));
    g_assert_true(pcv_validate_exec_cmd("a"));                   
}

static void test_exec_cmd_invalid(void) {
    g_assert_false(pcv_validate_exec_cmd(NULL));
    g_assert_false(pcv_validate_exec_cmd(""));

                                      
    gchar *long_cmd = g_malloc0(PCV_MAX_EXEC_CMD + 2);
    memset(long_cmd, 'a', PCV_MAX_EXEC_CMD + 1);
    g_assert_false(pcv_validate_exec_cmd(long_cmd));
    g_free(long_cmd);
}

                                                       

static void test_container_name_valid(void) {
    g_assert_true(pcv_validate_vm_name("web-ctr-1"));
    g_assert_true(pcv_validate_vm_name("my_container"));
    g_assert_true(pcv_validate_vm_name("ctr01"));
}

static void test_container_name_invalid(void) {
    g_assert_false(pcv_validate_vm_name("web ctr"));                
    g_assert_false(pcv_validate_vm_name("ctr;rm"));                     
    g_assert_false(pcv_validate_vm_name("../../etc"));                  
}

                                                        

static void test_new_container_name_rejects_leading_dash(void) {
    for (const gchar *const *n = (const gchar *const[]){ "--name", "-x", "-", "--help", NULL }; *n; n++) {
        g_assert_false(pcv_validate_new_container_name(*n));
        g_assert_true(pcv_validate_vm_name(*n));
    }
    for (const gchar *const *n = (const gchar *const[]){ "app-ctr", "a-", "a--b", "_x", "ctr01", NULL }; *n; n++)
        g_assert_true(pcv_validate_new_container_name(*n));
    g_assert_false(pcv_validate_new_container_name(NULL));
    g_assert_false(pcv_validate_new_container_name(""));
    g_assert_false(pcv_validate_new_container_name("web ctr"));
    gchar max_name[PCV_MAX_VM_NAME + 2];
    memset(max_name, 'a', PCV_MAX_VM_NAME);
    max_name[PCV_MAX_VM_NAME] = '\0';
    g_assert_true(pcv_validate_new_container_name(max_name));
    max_name[PCV_MAX_VM_NAME] = 'a';
    max_name[PCV_MAX_VM_NAME + 1] = '\0';
    g_assert_false(pcv_validate_new_container_name(max_name));
}

static JsonObject *
mode_params(const gchar *json)
{
    JsonParser *pr = json_parser_new();
    g_assert_true(json_parser_load_from_data(pr, json, -1, NULL));
    JsonObject *o = json_object_ref(json_node_get_object(json_parser_get_root(pr)));
    g_object_unref(pr);
    return o;
}

typedef struct { gchar *dir, *ready, *none; } ModeHost;

static ModeHost
mode_host_new(void)
{
    ModeHost h = { g_dir_make_tmp("pcv-z17-mode-XXXXXX", NULL), NULL, NULL };
    h.ready = g_build_filename(h.dir, "ready", NULL);
    h.none = g_build_filename(h.dir, "none", NULL);
    g_assert_true(g_file_set_contents(h.ready, "root:100000:65536\n", -1, NULL));
    g_assert_true(g_file_set_contents(h.none, "hardcoremonk:100000:65536\n", -1, NULL));
    return h;
}

static void
mode_host_free(ModeHost *h)
{
    g_unlink(h->ready); g_unlink(h->none); g_rmdir(h->dir);
    g_free(h->ready); g_free(h->none); g_free(h->dir);
}

static void
mode_expect(const gchar *json, gboolean default_on, const gchar *subid, gboolean pass, gint value, const gchar *msg_prefix)
{
    PcvLxcUsernsSettings s = { .start = 100000, .count = 65536, .default_on = default_on };
    JsonObject *p = mode_params(json);
    PcvContainerCreateMode m;
    gboolean ok = pcv_container_create_mode_check(p, NULL, &s, subid, subid, &m);
    g_assert_cmpint(ok, ==, pass);
    if (pass) {
        g_assert_cmpint(m.rootless, ==, value);
        g_assert_null(m.message);
    } else {
        g_assert_cmpint(m.code, ==, value);
        g_assert_true(g_str_has_prefix(m.message, msg_prefix));
    }
    g_free(m.message);
    json_object_unref(p);
}

static void
test_mode_rootless_must_be_boolean(void)
{
    ModeHost h = mode_host_new();
    for (const gchar *const *j = (const gchar *const[]){
             "{\"rootless\":\"true\"}", "{\"rootless\":\"false\",\"_pcv_caller_role\":1}",
             "{\"rootless\":1}", "{\"rootless\":null}", NULL }; *j; j++)
        mode_expect(*j, TRUE, h.none, FALSE, PURE_RPC_ERR_INVALID_PARAMS, "rootless must be a boolean");
    mode_host_free(&h);
}

static void
test_mode_privileged_requires_admin(void)
{
    ModeHost h = mode_host_new();
    mode_expect("{\"rootless\":false,\"_pcv_caller_role\":1}", TRUE, h.none, FALSE, PURE_RPC_ERR_FORBIDDEN,
                "privileged containers require admin role");
    mode_expect("{\"rootless\":false,\"_pcv_caller_role\":0}", TRUE, h.ready, FALSE, PURE_RPC_ERR_FORBIDDEN, "privileged");
    mode_expect("{\"rootless\":false,\"_pcv_caller_role\":2}", TRUE, h.none, TRUE, 0, NULL);
    mode_expect("{\"rootless\":false}", TRUE, h.none, TRUE, 0, NULL);
    mode_host_free(&h);
}

static void
test_mode_host_readiness_table(void)
{
    ModeHost h = mode_host_new();
    const gchar *busy = "unprivileged containers are not available on this host: ";
    mode_expect("{\"_pcv_caller_role\":1}", TRUE, h.ready, TRUE, 1, NULL);
    mode_expect("{\"_pcv_caller_role\":1}", TRUE, h.none, FALSE, PURE_RPC_ERR_CONFLICT, busy);
    mode_expect("{\"rootless\":true,\"_pcv_caller_role\":1}", FALSE, h.none, FALSE, PURE_RPC_ERR_CONFLICT, busy);
    mode_expect("{\"rootless\":true,\"_pcv_caller_role\":1}", FALSE, h.ready, TRUE, 1, NULL);
    mode_expect("{\"_pcv_caller_role\":1}", FALSE, h.none, TRUE, 0, NULL);
    mode_host_free(&h);
}

static void
test_info_json_privileged_field(void)
{
    PcvLxcInfo info = { .name = "c1", .state_str = "RUNNING", .ip_addr = "10.0.3.5", .image = "ubuntu:noble",
                        .privileged = TRUE };
    JsonObject *o = pcv_container_info_json(&info);
    g_assert_cmpstr(json_object_get_string_member(o, "name"), ==, "c1");
    g_assert_cmpstr(json_object_get_string_member(o, "state"), ==, "RUNNING");
    g_assert_cmpstr(json_object_get_string_member(o, "ip_addr"), ==, "10.0.3.5");
    g_assert_cmpstr(json_object_get_string_member(o, "image"), ==, "ubuntu:noble");
    g_assert_true(json_object_get_boolean_member(o, "privileged"));
    json_object_unref(o);
    info.privileged = FALSE;
    o = pcv_container_info_json(&info);
    g_assert_true(json_object_has_member(o, "privileged"));
    g_assert_false(json_object_get_boolean_member(o, "privileged"));
    json_object_unref(o);
}

static void
alias_expect(const gchar *json, gboolean pass, const gchar *want, const gchar *msg)
{
    JsonObject *p = mode_params(json);
    gchar *m = NULL;
    g_assert_cmpint(pcv_container_create_bridge_alias(p, &m), ==, pass);
    if (pass) {
        g_assert_null(m);
        g_assert_cmpstr(pcv_rpc_params_get_string(p, "network_bridge"), ==, want);
    } else {
        g_assert_cmpstr(m, ==, msg);
    }
    g_free(m);
    json_object_unref(p);
}

static void
test_bridge_alias_normalizes(void)
{
    alias_expect("{\"bridge\":\"lxcbr0\"}", TRUE, "lxcbr0", NULL);
    alias_expect("{\"network_bridge\":\"br1\"}", TRUE, "br1", NULL);
    alias_expect("{}", TRUE, NULL, NULL);
    gchar *m = NULL;
    g_assert_true(pcv_container_create_bridge_alias(NULL, &m));
    g_assert_null(m);
}

static void
test_bridge_alias_same_value_ok(void)
{
    alias_expect("{\"bridge\":\"lxcbr0\",\"network_bridge\":\"lxcbr0\"}", TRUE, "lxcbr0", NULL);
}

static void
test_bridge_alias_conflict_rejected(void)
{
    const gchar *msg = "bridge and network_bridge must match";
    alias_expect("{\"bridge\":\"lxcbr0\",\"network_bridge\":\"virbr0\"}", FALSE, NULL, msg);
    alias_expect("{\"bridge\":\"lxcbr0\",\"network_bridge\":5}", FALSE, NULL, msg);
    alias_expect("{\"bridge\":\"lxcbr0\",\"network_bridge\":null}", FALSE, NULL, msg);
    JsonObject *p = mode_params("{\"bridge\":\"br-mgmt\",\"network_bridge\":\"virbr0\"}");
    gchar *m = NULL;
    g_assert_false(pcv_container_create_bridge_alias(p, &m));
    g_assert_cmpstr(pcv_rpc_params_get_string(p, "network_bridge"), ==, "virbr0");
    g_free(m);
    json_object_unref(p);
}

static void
test_bridge_alias_non_string_rejected(void)
{
    for (const gchar *const *j = (const gchar *const[]){
             "{\"bridge\":5}", "{\"bridge\":null}", "{\"bridge\":[\"lxcbr0\"]}", "{\"bridge\":{}}",
             "{\"bridge\":true,\"network_bridge\":\"lxcbr0\"}", NULL }; *j; j++)
        alias_expect(*j, FALSE, NULL, "bridge must be a string");
}

static void
clone_gate_expect(gint role, const gchar *config_text, gboolean pass, gint entries)
{
    gint got = -99;
    gchar *msg = NULL;
    g_assert_cmpint(pcv_container_clone_volume_gate(role, config_text, &got, &msg), ==, pass);
    g_assert_cmpint(got, ==, entries);
    if (pass)
        g_assert_null(msg);
    else
        g_assert_cmpstr(msg, ==, "cloning a container with host volumes requires admin role");
    g_free(msg);
}

static void
test_clone_volume_gate_table(void)
{
    const gchar *one = "lxc.uts.name = src\n"
                       "lxc.mount.entry = /srv/data mnt/data none bind,create=dir 0 0\n";
    const gchar *none = "lxc.mount.entry = proc proc proc nodev,noexec,nosuid 0 0\n"
                        "lxc.mount.entry = tmpfs tmp tmpfs defaults 0 0\n"
                        "lxc.mount.entry = rel/dir mnt/r none bind 0 0\n"
                        "# lxc.mount.entry = /srv/old mnt/old none bind 0 0\n"
                        "lxc.mount.entry = /srv/opt mnt/opt none unbindable 0 0\n";
    const gchar *two = "lxc.mount.entry = /srv/a mnt/a none bind,create=dir 0 0\n"
                       "lxc.mount.entry = proc proc proc nodev 0 0\n"
                       "lxc.mount.entry = /srv/b mnt/b none rbind,ro,create=dir 0 0\n";
    clone_gate_expect(PCV_ROLE_OPERATOR, one, FALSE, 1);
    clone_gate_expect(PCV_ROLE_OPERATOR, NULL, FALSE, -1);
    clone_gate_expect(PCV_ROLE_OPERATOR, none, TRUE, 0);
    clone_gate_expect(PCV_ROLE_ADMIN, two, TRUE, 2);
    clone_gate_expect(PCV_ROLE_ADMIN, NULL, TRUE, -1);
    clone_gate_expect(PCV_ROLE_VIEWER, two, FALSE, 2);
    clone_gate_expect(PCV_ROLE_OPERATOR, "", TRUE, 0);

    g_assert_false(pcv_container_clone_volume_gate(PCV_ROLE_OPERATOR, one, NULL, NULL));
    g_assert_true(pcv_container_clone_volume_gate(PCV_ROLE_ADMIN, one, NULL, NULL));
}

typedef struct { const gchar *const *allow; GString *seen; } BridgeFake;

static gboolean
bridge_fake_check(const gchar *bridge, gpointer data)
{
    BridgeFake *f = data;
    g_string_append_printf(f->seen, "%s%s", f->seen->len ? "," : "", bridge);
    for (const gchar *const *a = f->allow; a && *a; a++)
        if (g_str_equal(*a, bridge))
            return TRUE;
    return FALSE;
}

static void
clone_bridge_expect(gint role, const gchar *config_text, gboolean pass, const gchar *seen, const gchar *bad)
{
    static const gchar *const allow[] = { "br-ok", NULL };
    BridgeFake f = { allow, g_string_new(NULL) };
    gchar *bridge = NULL, *msg = NULL;
    g_assert_cmpint(pcv_container_clone_bridge_gate(role, config_text, bridge_fake_check, &f, &bridge, &msg), ==, pass);
    g_assert_cmpstr(f.seen->str, ==, seen);
    g_assert_cmpstr(bridge, ==, bad);
    if (pass) {
        g_assert_null(msg);
    } else if (bad) {
        g_autofree gchar *want = g_strdup_printf("cloning a container connected to bridge '%s' requires admin role: "
                                                 "the bridge is not allowed for this account", bad);
        g_assert_cmpstr(msg, ==, want);
    } else {
        g_assert_cmpstr(msg, ==, "cloning a container whose config cannot be read requires admin role");
    }
    g_free(bridge);
    g_free(msg);
    g_string_free(f.seen, TRUE);
}

static void
test_clone_bridge_gate_table(void)
{
    const gchar *mixed = "lxc.net.0.type = veth\nlxc.net.0.link = virbr0\nlxc.net.0.flags = up\n"
                         "lxc.net.1.type = veth\nlxc.net.1.link = br-x\nlxc.net.2.link = br-y\n";
    clone_bridge_expect(PCV_ROLE_ADMIN, mixed, TRUE, "", NULL);
    clone_bridge_expect(PCV_ROLE_ADMIN, NULL, TRUE, "", NULL);
    clone_bridge_expect(PCV_ROLE_OPERATOR, mixed, FALSE, "br-x", "br-x");
    clone_bridge_expect(PCV_ROLE_VIEWER, mixed, FALSE, "br-x", "br-x");
    clone_bridge_expect(PCV_ROLE_OPERATOR, "lxc.net.0.link = virbr0\n", TRUE, "", NULL);
    clone_bridge_expect(PCV_ROLE_OPERATOR, "lxc.net.0.link = br-ok\nlxc.net.3.link = br-ok\n", TRUE, "br-ok,br-ok", NULL);
    clone_bridge_expect(PCV_ROLE_OPERATOR, "lxc.net.0.link = br-ok\nlxc.net.10.link=br-x\n", FALSE, "br-ok,br-x", "br-x");
    clone_bridge_expect(PCV_ROLE_OPERATOR, "  \tlxc.net.7.link \t=  br-x \t\n", FALSE, "br-x", "br-x");
    clone_bridge_expect(PCV_ROLE_OPERATOR, "lxc.network.link = br-x\n", FALSE, "br-x", "br-x");
    clone_bridge_expect(PCV_ROLE_OPERATOR, "# lxc.net.0.link = br-x\n", TRUE, "", NULL);
    clone_bridge_expect(PCV_ROLE_OPERATOR, "lxc.net.0.linkx = br-x\nlxc.net.a.link = br-x\nlxc.net..link = br-x\n"
                                           "lxc.net.0.link.x = br-x\nxlxc.net.0.link = br-x\n", TRUE, "", NULL);
    clone_bridge_expect(PCV_ROLE_OPERATOR, "lxc.net.0.link =\nlxc.net.1.link =   \n", TRUE, "", NULL);
    clone_bridge_expect(PCV_ROLE_OPERATOR, "", TRUE, "", NULL);
    clone_bridge_expect(PCV_ROLE_OPERATOR, NULL, FALSE, "", NULL);

    static const gchar *const none[] = { NULL };
    BridgeFake f = { none, g_string_new(NULL) };
    g_assert_false(pcv_container_clone_bridge_gate(PCV_ROLE_OPERATOR, mixed, bridge_fake_check, &f, NULL, NULL));
    g_string_free(f.seen, TRUE);
}

void test_container_register(void) {
    g_test_add_func("/container/image/valid",      test_container_image_valid);
    g_test_add_func("/container/image/invalid",    test_container_image_invalid);
    g_test_add_func("/container/image/boundary",   test_container_image_boundary);
    g_test_add_func("/container/exec_cmd/valid",   test_exec_cmd_valid);
    g_test_add_func("/container/exec_cmd/invalid", test_exec_cmd_invalid);
    g_test_add_func("/container/name/valid",       test_container_name_valid);
    g_test_add_func("/container/name/invalid",     test_container_name_invalid);
    g_test_add_func("/container/name/new_rejects_leading_dash", test_new_container_name_rejects_leading_dash);
    g_test_add_func("/container/mode/rootless_must_be_boolean", test_mode_rootless_must_be_boolean);
    g_test_add_func("/container/mode/privileged_requires_admin", test_mode_privileged_requires_admin);
    g_test_add_func("/container/mode/host_readiness_table",      test_mode_host_readiness_table);
    g_test_add_func("/container/list/info_json_privileged",      test_info_json_privileged_field);
    g_test_add_func("/container/mode/bridge_alias_normalizes",       test_bridge_alias_normalizes);
    g_test_add_func("/container/mode/bridge_alias_same_value_ok",    test_bridge_alias_same_value_ok);
    g_test_add_func("/container/mode/bridge_alias_conflict_rejected", test_bridge_alias_conflict_rejected);
    g_test_add_func("/container/mode/bridge_alias_non_string_rejected", test_bridge_alias_non_string_rejected);
    g_test_add_func("/container/clone_volume_gate",                  test_clone_volume_gate_table);
    g_test_add_func("/container/clone_bridge_gate",                  test_clone_bridge_gate_table);
}
