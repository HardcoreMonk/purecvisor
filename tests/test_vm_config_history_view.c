#include <glib.h>
#include <json-glib/json-glib.h>
#include <string.h>

#include "../src/modules/auth/pcv_rbac.h"
#include "../src/modules/virt/vm_config_history_view.h"

static PcvVmCfgRow *
mk_row(const gchar *owner, const gchar *owner_tenant, const gchar *actor, const gchar *actor_tenant)
{
    PcvVmCfgRow *r = g_new0(PcvVmCfgRow, 1);
    r->id = 4242;
    r->prev_id = 4100;
    r->entry_id = g_strdup("0123456789abcdef0123456789abcdef");
    r->vm_uuid = g_strdup("6f1c2d3e-4a5b-4c6d-8e9f-0a1b2c3d4e5f");
    r->vm_name = g_strdup("web");
    r->owner = g_strdup(owner);
    r->owner_tenant = g_strdup(owner_tenant);
    r->kind = g_strdup("changed");
    r->scope = g_strdup("config");
    r->cause = g_strdup("rpc");
    r->observed_at = 1760000000000LL;
    r->attribution = g_strdup(actor ? "rpc" : "none");
    r->actor = g_strdup(actor);
    r->actor_tenant = g_strdup(actor_tenant);
    r->actor_method = g_strdup(actor ? "vm.set_vcpu" : NULL);
    r->actor_result = g_strdup(actor ? "ok" : NULL);
    r->snapshot_json = g_strdup("{\"autostart\":false,\"config\":{\"disk[vda].source\":\"/var/lib/pcv/web.qcow2\","
                                "\"os.nvram.path\":\"/var/lib/libvirt/qemu/nvram/web_VARS.fd\",\"owner\":\"alice\"},"
                                "\"live_overlay\":null}");
    r->changes_json = g_strdup("[{\"after\":\"/var/lib/new/VARS.fd\",\"before\":\"/var/lib/old/VARS.fd\","
                               "\"path\":\"os.nvram.path\"},{\"after\":\"alice\",\"before\":\"zed\",\"path\":\"owner\"},"
                               "{\"after\":\"/var/lib/pcv/web2.qcow2\",\"before\":null,\"path\":\"disk[vdb].source\"}]");
    return r;
}

static gboolean
visible(gint role, const gchar *subject, const gchar *tenant, const gchar *owner, const gchar *owner_tenant)
{
    PcvVmReadScope *s = pcv_vm_read_scope_new_parts(role, subject, TRUE, tenant);
    gboolean v = pcv_vm_cfg_row_visible(s, owner, owner_tenant);
    pcv_vm_read_scope_free(s);
    return v;
}

static void
test_view_decide_on_stored_values(void)
{

    g_assert_true(visible(PCV_ROLE_OPERATOR, "bob", "t1", "alice", "t1"));
    g_assert_false(visible(PCV_ROLE_OPERATOR, "carol", "t2", "alice", "t1"));

    g_assert_false(visible(PCV_ROLE_VIEWER, "dave", "t3", "alice", "t1"));

    g_assert_false(visible(PCV_ROLE_OPERATOR, "alice", "t1", NULL, NULL));
    g_assert_true(visible(PCV_ROLE_ADMIN, "root", NULL, NULL, NULL));

    g_assert_true(visible(PCV_ROLE_VIEWER, "alice", "t1", "alice", NULL));
    g_assert_false(visible(PCV_ROLE_VIEWER, "bob", "t1", "alice", NULL));

    g_assert_true(visible(PCV_ROLE_VIEWER, "alice", NULL, "alice", "t1"));
    g_assert_false(visible(PCV_ROLE_VIEWER, "bob", NULL, "alice", "t1"));
    g_assert_false(pcv_vm_cfg_row_visible(NULL, "alice", "t1"));
}

static JsonObject *
proj(const PcvVmCfgRow *r, gint role, const gchar *subject, const gchar *tenant, gboolean full, JsonNode **keep)
{
    *keep = pcv_vm_cfg_row_project(r, role, subject, tenant, full);
    return json_node_get_object(*keep);
}

static void
test_view_actor_hidden(void)
{
    PcvVmCfgRow *r = mk_row("alice", "t1", "eve-admin", NULL);
    JsonNode *n = NULL;
    JsonObject *o = proj(r, PCV_ROLE_OPERATOR, "bob", "t1", FALSE, &n);
    g_assert_true(json_object_get_null_member(o, "actor"));
    g_assert_true(json_object_get_boolean_member(o, "actor_hidden"));
    g_assert_false(json_object_get_boolean_member(o, "actor_expired"));
    g_assert_cmpstr(json_object_get_string_member(o, "actor_method"), ==, "vm.set_vcpu");
    json_node_unref(n);
    o = proj(r, PCV_ROLE_ADMIN, "root", NULL, FALSE, &n);
    g_assert_cmpstr(json_object_get_string_member(o, "actor"), ==, "eve-admin");
    json_node_unref(n);
    pcv_vm_cfg_row_free(r);
    r = mk_row("alice", "t1", "carl", "t1");
    o = proj(r, PCV_ROLE_VIEWER, "bob", "t1", FALSE, &n);
    g_assert_cmpstr(json_object_get_string_member(o, "actor"), ==, "carl");
    g_assert_false(json_object_get_boolean_member(o, "actor_hidden"));
    json_node_unref(n);
    o = proj(r, PCV_ROLE_VIEWER, "carl", NULL, FALSE, &n);
    g_assert_cmpstr(json_object_get_string_member(o, "actor"), ==, "carl");
    json_node_unref(n);
    pcv_vm_cfg_row_free(r);
}

static void
test_view_actor_expired_distinct(void)
{
    PcvVmCfgRow *r = mk_row("alice", "t1", NULL, NULL);
    g_free(r->attribution);
    r->attribution = g_strdup("rpc");
    r->actor_expired = TRUE;
    r->actor_method = g_strdup("vm.limit");
    JsonNode *n = NULL;
    JsonObject *o = proj(r, PCV_ROLE_OPERATOR, "bob", "t1", FALSE, &n);
    g_assert_true(json_object_get_null_member(o, "actor"));
    g_assert_true(json_object_get_boolean_member(o, "actor_expired"));
    g_assert_false(json_object_get_boolean_member(o, "actor_hidden"));
    g_assert_cmpstr(json_object_get_string_member(o, "actor_method"), ==, "vm.limit");
    json_node_unref(n);
    o = proj(r, PCV_ROLE_ADMIN, "root", NULL, FALSE, &n);
    g_assert_true(json_object_get_null_member(o, "actor"));
    g_assert_true(json_object_get_boolean_member(o, "actor_expired"));
    json_node_unref(n);
    pcv_vm_cfg_row_free(r);
}

static const gchar *
change_val(JsonObject *o, const gchar *path, const gchar *key)
{
    JsonArray *a = json_object_get_array_member(o, "changes");
    for (guint i = 0; i < json_array_get_length(a); i++) {
        JsonObject *c = json_array_get_object_element(a, i);
        if (!g_strcmp0(json_object_get_string_member(c, "path"), path)) {
            JsonNode *v = json_object_get_member(c, key);
            return JSON_NODE_HOLDS_NULL(v) ? NULL : json_node_get_string(v);
        }
    }
    g_assert_not_reached();
    return NULL;
}

static void
test_view_owner_change_masked(void)
{
    PcvVmCfgRow *r = mk_row("alice", "t1", NULL, NULL);
    JsonNode *n = NULL;
    JsonObject *o = proj(r, PCV_ROLE_OPERATOR, "bob", "t1", FALSE, &n);
    g_assert_cmpstr(change_val(o, "owner", "before"), ==, PCV_VMCH_OTHER_OWNER);
    g_assert_cmpstr(change_val(o, "owner", "after"), ==, "alice");
    json_node_unref(n);
    o = proj(r, PCV_ROLE_OPERATOR, "zed", "t1", FALSE, &n);
    g_assert_cmpstr(change_val(o, "owner", "before"), ==, "zed");
    json_node_unref(n);
    o = proj(r, PCV_ROLE_ADMIN, "root", NULL, FALSE, &n);
    g_assert_cmpstr(change_val(o, "owner", "before"), ==, "zed");
    json_node_unref(n);
    pcv_vm_cfg_row_free(r);
}

static void
test_view_paths_basename_nonadmin(void)
{
    PcvVmCfgRow *r = mk_row("alice", "t1", NULL, NULL);
    JsonNode *n = NULL;
    JsonObject *o = proj(r, PCV_ROLE_VIEWER, "bob", "t1", TRUE, &n);
    g_assert_cmpstr(change_val(o, "os.nvram.path", "before"), ==, "VARS.fd");
    g_assert_cmpstr(change_val(o, "disk[vdb].source", "after"), ==, "/var/lib/pcv/web2.qcow2");
    JsonObject *cfg = json_object_get_object_member(json_object_get_object_member(o, "snapshot"), "config");
    g_assert_cmpstr(json_object_get_string_member(cfg, "os.nvram.path"), ==, "web_VARS.fd");
    g_assert_cmpstr(json_object_get_string_member(cfg, "disk[vda].source"), ==, "/var/lib/pcv/web.qcow2");
    json_node_unref(n);
    o = proj(r, PCV_ROLE_ADMIN, "root", NULL, TRUE, &n);
    g_assert_cmpstr(change_val(o, "os.nvram.path", "before"), ==, "/var/lib/old/VARS.fd");
    json_node_unref(n);
    pcv_vm_cfg_row_free(r);
}

static void
test_view_live_paths_projected(void)
{
    PcvVmCfgRow *r = mk_row("alice", "t1", NULL, NULL);
    g_free(r->changes_json);
    r->changes_json = g_strdup("[{\"after\":\"/run/pcv/vhu/new.sock\",\"before\":\"/run/pcv/vhu/old.sock\","
                               "\"path\":\"live.nic[52:54:00:00:00:01].vhostuser\"},"
                               "{\"after\":\"/etc/pcv/ifup.sh\",\"before\":null,\"path\":\"live.nic[52:54:00:00:00:01].script\"},"
                               "{\"after\":\"/fw/new/OVMF_CODE.fd\",\"before\":\"/fw/old/OVMF_CODE.fd\",\"path\":\"live.os.loader.path\"},"
                               "{\"after\":\"/nv/new_VARS.fd\",\"before\":\"/nv/old_VARS.fd\",\"path\":\"live.os.nvram.path\"},"
                               "{\"after\":\"/boot/vmlinuz-new\",\"before\":\"/boot/vmlinuz-old\",\"path\":\"live.os.kernel\"},"
                               "{\"after\":\"alice\",\"before\":\"zed\",\"path\":\"live.owner\"},"
                               "{\"after\":\"hmac-new\",\"before\":\"hmac-old\",\"path\":\"opaque.qemu_commandline\"},"
                               "{\"after\":\"hmac-l2\",\"before\":\"hmac-l1\",\"path\":\"live.opaque.other_devices\"}]");
    g_free(r->snapshot_json);
    r->snapshot_json = g_strdup("{\"autostart\":false,\"config\":{\"opaque.qemu_commandline\":\"hmac-new\"},"
                                "\"live_overlay\":{\"os.kernel\":\"/boot/vmlinuz-new\",\"opaque.other_devices\":\"hmac-l2\"}}");
    JsonNode *n = NULL;
    JsonObject *o = proj(r, PCV_ROLE_OPERATOR, "bob", "t1", TRUE, &n);
    g_assert_cmpstr(change_val(o, "live.nic[52:54:00:00:00:01].vhostuser", "before"), ==, "old.sock");
    g_assert_cmpstr(change_val(o, "live.nic[52:54:00:00:00:01].script", "after"), ==, "ifup.sh");
    g_assert_cmpstr(change_val(o, "live.os.loader.path", "after"), ==, "OVMF_CODE.fd");
    g_assert_cmpstr(change_val(o, "live.os.nvram.path", "before"), ==, "old_VARS.fd");
    g_assert_cmpstr(change_val(o, "live.os.kernel", "after"), ==, "vmlinuz-new");
    g_assert_cmpstr(change_val(o, "live.owner", "before"), ==, PCV_VMCH_OTHER_OWNER);
    g_assert_cmpstr(change_val(o, "live.owner", "after"), ==, "alice");
    g_assert_cmpstr(change_val(o, "opaque.qemu_commandline", "before"), ==, PCV_VMCH_OPAQUE_MASK);
    g_assert_cmpstr(change_val(o, "live.opaque.other_devices", "after"), ==, PCV_VMCH_OPAQUE_MASK);
    JsonObject *snap = json_object_get_object_member(o, "snapshot");
    g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(snap, "config"), "opaque.qemu_commandline"),
                    ==, PCV_VMCH_OPAQUE_MASK);
    JsonObject *lo = json_object_get_object_member(snap, "live_overlay");
    g_assert_cmpstr(json_object_get_string_member(lo, "os.kernel"), ==, "vmlinuz-new");
    g_assert_cmpstr(json_object_get_string_member(lo, "opaque.other_devices"), ==, PCV_VMCH_OPAQUE_MASK);

    gchar *txt = json_to_string(n, FALSE);
    g_assert_null(strstr(txt, "/run/pcv/vhu"));
    g_assert_null(strstr(txt, "/boot/"));
    g_assert_null(strstr(txt, "hmac-"));
    g_assert_null(strstr(txt, "zed"));
    g_free(txt);
    json_node_unref(n);
    o = proj(r, PCV_ROLE_ADMIN, "root", NULL, TRUE, &n);
    g_assert_cmpstr(change_val(o, "live.os.kernel", "after"), ==, "/boot/vmlinuz-new");
    g_assert_cmpstr(change_val(o, "opaque.qemu_commandline", "before"), ==, "hmac-old");
    json_node_unref(n);
    pcv_vm_cfg_row_free(r);
}

static void
test_view_no_internal_id(void)
{
    PcvVmCfgRow *r = mk_row("alice", "t1", "alice", "t1");
    JsonNode *n = pcv_vm_cfg_row_project(r, PCV_ROLE_ADMIN, "root", NULL, TRUE);
    gchar *text = json_to_string(n, FALSE);
    g_assert_null(strstr(text, "4242"));
    g_assert_null(strstr(text, "4100"));
    JsonObject *o = json_node_get_object(n);
    g_assert_cmpstr(json_object_get_string_member(o, "id"), ==, r->entry_id);
    g_assert_true(json_object_get_null_member(o, "prev_id"));
    g_free(text);
    json_node_unref(n);
    r->prev_entry_id = g_strdup("fedcba9876543210fedcba9876543210");
    n = pcv_vm_cfg_row_project(r, PCV_ROLE_ADMIN, "root", NULL, TRUE);
    g_assert_cmpstr(json_object_get_string_member(json_node_get_object(n), "prev_id"), ==, r->prev_entry_id);
    json_node_unref(n);

    g_free(r->changes_json);
    GString *many = g_string_new("[");
    for (gint i = 0; i < 12; i++)
        g_string_append_printf(many, "%s{\"after\":%d,\"before\":0,\"path\":\"cputune.p%02d\"}", i ? "," : "", i, i);
    g_string_append_c(many, ']');
    r->changes_json = g_string_free(many, FALSE);
    n = pcv_vm_cfg_row_project(r, PCV_ROLE_ADMIN, "root", NULL, FALSE);
    o = json_node_get_object(n);
    g_assert_cmpint(json_object_get_int_member(o, "change_count"), ==, 12);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(o, "changes")), ==, PCV_VMCH_SUMMARY_CHANGES);
    g_assert_false(json_object_has_member(o, "snapshot"));
    json_node_unref(n);
    pcv_vm_cfg_row_free(r);
}

static void
test_cursor_roundtrip(void)
{
    gchar *c = pcv_vm_cfg_cursor_encode(1760000000123LL, "0123456789abcdef0123456789abcdef");
    gint64 at = 0;
    gchar id[33];
    g_assert_true(pcv_vm_cfg_cursor_decode(c, &at, id));
    g_assert_cmpint(at, ==, 1760000000123LL);
    g_assert_cmpstr(id, ==, "0123456789abcdef0123456789abcdef");
    g_free(c);
}

static void
test_cursor_garbage_rejected(void)
{
    gint64 at = 0;
    gchar id[33];
    const gchar *raw_bad[] = {"1760000000123:42", "1760000000123:0123456789ABCDEF0123456789ABCDEF",
                              "-5:0123456789abcdef0123456789abcdef", "abc:0123456789abcdef0123456789abcdef",
                              "17600000001230123456789abcdef0123456789abcdef", NULL};
    for (guint i = 0; raw_bad[i]; i++) {
        gchar *b64 = g_base64_encode((const guchar *)raw_bad[i], strlen(raw_bad[i]));
        g_assert_false(pcv_vm_cfg_cursor_decode(b64, &at, id));
        g_free(b64);
    }
    g_assert_false(pcv_vm_cfg_cursor_decode("!!!not-base64!!!", &at, id));
    g_assert_false(pcv_vm_cfg_cursor_decode("", &at, id));
    g_assert_false(pcv_vm_cfg_cursor_decode(NULL, &at, id));
    g_assert_true(pcv_vm_cfg_entry_id_valid("0123456789abcdef0123456789abcdef"));
    g_assert_false(pcv_vm_cfg_entry_id_valid("0123456789abcdef0123456789abcde"));
    g_assert_false(pcv_vm_cfg_entry_id_valid("0123456789abcdef0123456789abcdeG"));
}

void test_vm_config_history_view_register(void);
void
test_vm_config_history_view_register(void)
{
    g_test_add_func("/vm_config_history/view/view_decide_on_stored_values", test_view_decide_on_stored_values);
    g_test_add_func("/vm_config_history/view/view_actor_hidden", test_view_actor_hidden);
    g_test_add_func("/vm_config_history/view/view_actor_expired_distinct", test_view_actor_expired_distinct);
    g_test_add_func("/vm_config_history/view/view_owner_change_masked", test_view_owner_change_masked);
    g_test_add_func("/vm_config_history/view/view_paths_basename_nonadmin", test_view_paths_basename_nonadmin);
    g_test_add_func("/vm_config_history/view/view_live_paths_projected", test_view_live_paths_projected);
    g_test_add_func("/vm_config_history/view/view_no_internal_id", test_view_no_internal_id);
    g_test_add_func("/vm_config_history/view/cursor_roundtrip", test_cursor_roundtrip);
    g_test_add_func("/vm_config_history/view/cursor_garbage_rejected", test_cursor_garbage_rejected);
}
