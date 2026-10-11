                                                                                
                                                                                          
                                        
                                                                     
                 
   
                        
                                                                   
  
                                                                 
                                                              
                                          
   
#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

#include "modules/auth/pcv_rbac.h"

void pcv_dispatcher_init_policy_map(void);
gboolean pcv_dispatcher_check_rbac(const gchar *method, gint caller_role);
gboolean pcv_dispatcher_vm_target_aliases_valid(const gchar *method,
                                                JsonObject *params,
                                                const gchar **reason_out);

static void
test_vpc_rbac_rest_and_dispatcher_agree(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("pcv-vpc-rbac-XXXXXX", NULL);
    g_assert_nonnull(dir);
    g_autofree gchar *db_path = g_build_filename(dir, "rbac.db", NULL);
    pcv_rbac_init(db_path);

    g_autoptr(GError) error = NULL;
    g_assert_true(pcv_rbac_user_create(
        "vpc_viewer", "pw", PCV_ROLE_VIEWER, "tenant-a", &error));
    g_assert_no_error(error);
    g_assert_true(pcv_rbac_user_create(
        "vpc_operator", "pw", PCV_ROLE_OPERATOR, "tenant-a", &error));
    g_assert_no_error(error);
    g_assert_true(pcv_rbac_user_create(
        "vpc_admin", "pw", PCV_ROLE_ADMIN, NULL, &error));
    g_assert_no_error(error);
    pcv_dispatcher_init_policy_map();

    static const gchar *reads[] = {
        "vpc.list", "vpc.get", "vpc.subnet.list", "vpc.attachment.list",
        "vpc.service.list", "vpc.status", "vpc.backend.list",
    };
    for (guint i = 0; i < G_N_ELEMENTS(reads); i++) {
        g_assert_true(pcv_rbac_check_permission("vpc_viewer", reads[i]));
        g_assert_true(pcv_dispatcher_check_rbac(reads[i], PCV_ROLE_VIEWER));
    }

    static const gchar *operator_mutations[] = {
        "vpc.create", "vpc.egress.set", "vpc.subnet.create", "vpc.subnet.delete",
        "vpc.attachment.create", "vpc.attachment.delete", "vpc.service.publish",
        "vpc.service.unpublish",
    };
    for (guint i = 0; i < G_N_ELEMENTS(operator_mutations); i++) {
        g_assert_false(pcv_rbac_check_permission("vpc_viewer", operator_mutations[i]));
        g_assert_false(pcv_dispatcher_check_rbac(operator_mutations[i], PCV_ROLE_VIEWER));
        g_assert_true(pcv_rbac_check_permission("vpc_operator", operator_mutations[i]));
        g_assert_true(pcv_dispatcher_check_rbac(operator_mutations[i], PCV_ROLE_OPERATOR));
    }

    static const gchar *admin_only[] = { "vpc.delete", "vpc.reconcile" };
    for (guint i = 0; i < G_N_ELEMENTS(admin_only); i++) {
        g_assert_false(pcv_rbac_check_permission("vpc_operator", admin_only[i]));
        g_assert_false(pcv_dispatcher_check_rbac(admin_only[i], PCV_ROLE_OPERATOR));
        g_assert_true(pcv_rbac_check_permission("vpc_admin", admin_only[i]));
        g_assert_true(pcv_dispatcher_check_rbac(admin_only[i], PCV_ROLE_ADMIN));
    }

    pcv_rbac_shutdown();
    g_autofree gchar *wal = g_strconcat(db_path, "-wal", NULL);
    g_autofree gchar *shm = g_strconcat(db_path, "-shm", NULL);
    g_remove(wal);
    g_remove(shm);
    g_remove(db_path);
    g_rmdir(dir);
}

                                                             
                                                                      
                                                               
                                                  
  
                                                        
                                                    
static void
test_d2_rbac_rest_and_dispatcher_agree(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("pcv-d2-rbac-XXXXXX", NULL);
    g_assert_nonnull(dir);
    g_autofree gchar *db_path = g_build_filename(dir, "rbac.db", NULL);
    pcv_rbac_init(db_path);

    g_autoptr(GError) error = NULL;
    g_assert_true(pcv_rbac_user_create(
        "d2_viewer", "pw", PCV_ROLE_VIEWER, NULL, &error));
    g_assert_no_error(error);
    g_assert_true(pcv_rbac_user_create(
        "d2_operator", "pw", PCV_ROLE_OPERATOR, NULL, &error));
    g_assert_no_error(error);
    g_assert_true(pcv_rbac_user_create(
        "d2_admin", "pw", PCV_ROLE_ADMIN, NULL, &error));
    g_assert_no_error(error);
    pcv_dispatcher_init_policy_map();

    static const gchar *reads[] = {
        "pool.conninfo",
        "dpdk.status", "dpdk.list", "dpdk.hugepage.info",
        "sriov.status", "sriov.list",
    };
    for (guint i = 0; i < G_N_ELEMENTS(reads); i++) {
        g_assert_true(pcv_rbac_check_permission("d2_viewer", reads[i]));
        g_assert_true(pcv_dispatcher_check_rbac(reads[i], PCV_ROLE_VIEWER));
    }

    static const gchar *admin_mutations[] = {
        "dpdk.set", "dpdk.bind", "dpdk.unbind",
        "dpdk.bridge.create", "dpdk.bridge.delete",
        "sriov.enable", "sriov.disable", "sriov.set",
        "sriov.attach", "sriov.detach",
    };
    for (guint i = 0; i < G_N_ELEMENTS(admin_mutations); i++) {
        g_assert_false(pcv_rbac_check_permission(
            "d2_operator", admin_mutations[i]));
        g_assert_false(pcv_dispatcher_check_rbac(
            admin_mutations[i], PCV_ROLE_OPERATOR));
        g_assert_true(pcv_rbac_check_permission(
            "d2_admin", admin_mutations[i]));
        g_assert_true(pcv_dispatcher_check_rbac(
            admin_mutations[i], PCV_ROLE_ADMIN));
    }

    pcv_rbac_shutdown();
    g_autofree gchar *wal = g_strconcat(db_path, "-wal", NULL);
    g_autofree gchar *shm = g_strconcat(db_path, "-shm", NULL);
    g_remove(wal);
    g_remove(shm);
    g_remove(db_path);
    g_rmdir(dir);
}




                                                                
                                                          


  
                                               
                                                      
static void
test_shared_ui_reads_rest_and_dispatcher_agree(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("pcv-ui-read-rbac-XXXXXX", NULL);
    g_assert_nonnull(dir);
    g_autofree gchar *db_path = g_build_filename(dir, "rbac.db", NULL);
    pcv_rbac_init(db_path);

    g_autoptr(GError) error = NULL;
    g_assert_true(pcv_rbac_user_create(
        "ui_read_viewer", "pw", PCV_ROLE_VIEWER, NULL, &error));
    g_assert_no_error(error);
    g_assert_true(pcv_rbac_user_create(
        "ui_read_operator", "pw", PCV_ROLE_OPERATOR, NULL, &error));
    g_assert_no_error(error);
    g_assert_true(pcv_rbac_user_create(
        "ui_read_admin", "pw", PCV_ROLE_ADMIN, NULL, &error));
    g_assert_no_error(error);
    pcv_dispatcher_init_policy_map();

    static const gchar *reads[] = {
        "alert.history", "healing.pending",
        "healing.history", "agent.history", "config.history", "storage.pool.forecast",
        "vm.delete.status", "vm.guest.agent.status",
    };
    static const struct {
        const gchar *username;
        PcvRole role;
    } callers[] = {
        { "ui_read_viewer", PCV_ROLE_VIEWER },
        { "ui_read_operator", PCV_ROLE_OPERATOR },
        { "ui_read_admin", PCV_ROLE_ADMIN },
    };
    for (guint i = 0; i < G_N_ELEMENTS(reads); i++) {
        for (guint j = 0; j < G_N_ELEMENTS(callers); j++) {
            g_assert_true(pcv_rbac_check_permission(callers[j].username, reads[i]));
            g_assert_true(pcv_dispatcher_check_rbac(reads[i], callers[j].role));
        }
    }

                                                       
    static const gchar *admin_mutations[] = {
        "alert.silence", "healing.set_mode", "config.backup",
        "storage.pool.scrub", "agent.config.set", "agent.compare_manual",
    };
    for (guint i = 0; i < G_N_ELEMENTS(admin_mutations); i++) {
        g_assert_false(pcv_rbac_check_permission(
            "ui_read_viewer", admin_mutations[i]));
        g_assert_false(pcv_dispatcher_check_rbac(
            admin_mutations[i], PCV_ROLE_VIEWER));
                                                              
                                                        
        g_assert_false(pcv_dispatcher_check_rbac(
            admin_mutations[i], PCV_ROLE_OPERATOR));
        g_assert_true(pcv_rbac_check_permission(
            "ui_read_admin", admin_mutations[i]));
        g_assert_true(pcv_dispatcher_check_rbac(
            admin_mutations[i], PCV_ROLE_ADMIN));
    }

    pcv_rbac_shutdown();
    g_autofree gchar *wal = g_strconcat(db_path, "-wal", NULL);
    g_autofree gchar *shm = g_strconcat(db_path, "-shm", NULL);
    g_remove(wal);
    g_remove(shm);
    g_remove(db_path);
    g_rmdir(dir);
}













static void
test_dispatcher_policy_default_deny(void)
{
    pcv_dispatcher_init_policy_map();

    static const gchar *probe = "zz.adr0078.unmapped_probe";
    g_assert_false(pcv_dispatcher_check_rbac(probe, PCV_ROLE_VIEWER));
    g_assert_false(pcv_dispatcher_check_rbac(probe, PCV_ROLE_OPERATOR));
    g_assert_true(pcv_dispatcher_check_rbac(probe, PCV_ROLE_ADMIN));
    g_assert_false(pcv_dispatcher_check_rbac(NULL, PCV_ROLE_OPERATOR));

    static const gchar *explicit_viewer_rows[] = {
        "vm.list", "alert.history", "monitor.fleet", "template.list",
        "jobs.list", "pool.conninfo", "auth.totp.status",
    };
    for (guint i = 0; i < G_N_ELEMENTS(explicit_viewer_rows); i++)
        g_assert_true(pcv_dispatcher_check_rbac(explicit_viewer_rows[i],
                                                PCV_ROLE_VIEWER));
}









static void
test_dispatcher_policy_formerly_unmapped_match_rest(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("pcv-default-deny-rbac-XXXXXX", NULL);
    g_assert_nonnull(dir);
    g_autofree gchar *db_path = g_build_filename(dir, "rbac.db", NULL);
    pcv_rbac_init(db_path);
    static const gchar *users[] = { "dd_viewer", "dd_operator", "dd_admin" };
    for (guint role = 0; role < G_N_ELEMENTS(users); role++) {
        g_autoptr(GError) error = NULL;
        g_assert_true(pcv_rbac_user_create(users[role], "fixture-only",
                                           (PcvRole)role, NULL, &error));
        g_assert_no_error(error);
    }
    pcv_dispatcher_init_policy_map();

    static const struct {
        const gchar *method;
        gint role;
    } table[] = {
        { "agent.config.get", 0 },        { "alert.action.list", 0 },
        { "alert.config.get", 0 },        { "alert.silence.list", 0 },
        { "audit.search", 2 },            { "backup.history", 1 },
        { "backup.policy.list", 0 },      { "container.env.list", 0 },
        { "container.health.check", 1 },  { "container.health.get", 0 },
        { "container.list", 0 },          { "container.logs", 1 },
        { "container.memory.stats", 1 },  { "container.metrics", 0 },
        { "container.nic.list", 0 },      { "container.snapshot.list", 0 },
        { "container.volume.list", 0 },   { "daemon.config.get", 0 },
        { "daemon.version", 2 },          { "db.migration.status", 2 },
        { "device.nic.list", 0 },         { "dpdk.hugepage.info", 0 },
        { "dpdk.list", 0 },               { "dpdk.status", 0 },
        { "gpu.list", 0 },                { "gpu.metrics", 0 },
        { "health.deep", 2 },             { "iscsi.target.list", 0 },
        { "iso.list", 0 },                { "jobs.get", 0 },
        { "jobs.list", 0 },               { "jobs.status", 2 },
        { "monitor.fleet", 0 },           { "monitor.metrics", 0 },
        { "monitor.processes", 0 },       { "network.info", 0 },
        { "network.list", 0 },            { "network.qos.get", 0 },
        { "overlay.info", 0 },            { "overlay.list", 0 },
        { "ovn.acl.list", 0 },            { "ovn.nat.list", 0 },
        { "ovn.router.detail", 1 },       { "ovn.router.list", 0 },
        { "ovn.status", 0 },              { "ovn.switch.detail", 1 },
        { "ovn.switch.list", 0 },         { "pool.conninfo", 0 },
        { "prometheus.sd", 2 },           { "qos.stats", 2 },
        { "qos.tenant.get", 0 },          { "qos.vm.get", 0 },
        { "quota.get", 0 },               { "security_group.list", 0 },
        { "snapshot.schedule.status", 2 },{ "sriov.list", 0 },
        { "sriov.status", 0 },            { "storage.pool.health", 1 },
        { "storage.pool.list", 0 },       { "storage.zvol.list", 0 },
        { "template.get", 0 },            { "template.history", 1 },
        { "template.list", 0 },           { "vm.blkio.get", 0 },
        { "vm.cpu.stats", 1 },            { "vm.event.webhook.list", 0 },
        { "vm.guest.ping", 1 },           { "vm.list", 0 },
        { "vm.memory.stats", 1 },         { "vm.metrics", 0 },
        { "vm.usb.list", 0 },             { "webhook.dlq.list", 0 },
    };
    g_assert_cmpuint(G_N_ELEMENTS(table), ==, 72);
    for (guint i = 0; i < G_N_ELEMENTS(table); i++) {
        for (gint role = PCV_ROLE_VIEWER; role <= PCV_ROLE_ADMIN; role++) {
            gboolean expected = role >= table[i].role;
            if (pcv_rbac_check_permission(users[role], table[i].method) != expected ||
                pcv_dispatcher_check_rbac(table[i].method, role) != expected)
                g_error("%s role=%d: expected %s (min role %d)", table[i].method, role,
                        expected ? "allow" : "deny", table[i].role);
        }
    }

    pcv_rbac_shutdown();
    g_autofree gchar *wal = g_strconcat(db_path, "-wal", NULL);
    g_autofree gchar *shm = g_strconcat(db_path, "-shm", NULL);
    g_remove(wal); g_remove(shm); g_remove(db_path); g_rmdir(dir);
}


static void
assert_vm_aliases(const gchar *method, const gchar *params_json, gboolean expected)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    g_assert_true(json_parser_load_from_data(parser, params_json, -1, NULL));
    JsonObject *params = json_node_get_object(json_parser_get_root(parser));
    const gchar *reason = "sentinel";
    gboolean valid = pcv_dispatcher_vm_target_aliases_valid(method, params, &reason);
    if (valid != expected)
        g_error("%s %s: expected %s", method, params_json, expected ? "valid" : "reject");
    if (expected)
        g_assert_null(reason);
    else
        g_assert_nonnull(reason);
}










static void
test_dispatcher_vm_target_alias_single_target(void)
{

    assert_vm_aliases("vm.delete", "{\"name\":\"mine\",\"vm_id\":\"other\"}", FALSE);
    assert_vm_aliases("vm.stop", "{\"vm_id\":\"other\",\"name\":\"mine\"}", FALSE);
    assert_vm_aliases("get_vnc_info", "{\"name\":\"mine\",\"vm_id\":\"other\"}", FALSE);
    assert_vm_aliases("device.nic.attach",
                      "{\"name\":\"mine\",\"vm_id\":\"other\",\"bridge\":\"br0\"}", FALSE);
    assert_vm_aliases("vm.security_group.set",
                      "{\"name\":\"mine\",\"vm\":\"other\",\"security_group\":\"sg\"}", FALSE);
    assert_vm_aliases("vm.snapshot.schedule.set",
                      "{\"name\":\"mine\",\"vm_name\":\"other\"}", FALSE);
    assert_vm_aliases("vm.mount_iso",
                      "{\"name\":\"mine\",\"vm_id\":\"other\",\"iso_path\":\"/x.iso\"}", FALSE);
    assert_vm_aliases("vm.clone", "{\"source\":\"mine\",\"name\":\"other\"}", FALSE);

    assert_vm_aliases("vm.delete", "{\"name\":\"mine\",\"vm_id\":7}", FALSE);
    assert_vm_aliases("vm.delete", "{\"name\":null}", FALSE);
    assert_vm_aliases("vm.start", "{\"vm_id\":{\"name\":\"mine\"}}", FALSE);
    assert_vm_aliases("vm.clone", "{\"source\":[\"mine\"],\"clone_name\":\"c\"}", FALSE);


    assert_vm_aliases("vm.delete", "{\"vm_id\":\"mine\"}", TRUE);
    assert_vm_aliases("vm.delete", "{\"name\":\"mine\",\"vm_id\":\"mine\"}", TRUE);
    assert_vm_aliases("vm.mount_iso",
                      "{\"name\":\"mine\",\"vm_id\":\"\",\"iso_path\":\"/x.iso\"}", TRUE);
    assert_vm_aliases("vm.security_group.set",
                      "{\"vm\":\"mine\",\"security_group\":\"sg\"}", TRUE);
    assert_vm_aliases("vm.clone",
                      "{\"source\":\"mine\",\"clone_name\":\"copy\",\"name\":\"mine\"}", TRUE);
    assert_vm_aliases("vm.clone", "{\"name\":\"mine\",\"new_name\":\"copy\"}", TRUE);
    assert_vm_aliases("vm.rename", "{\"name\":\"mine\",\"new_name\":\"renamed\"}", TRUE);
    assert_vm_aliases("vm.snapshot.create",
                      "{\"vm_id\":\"mine\",\"name\":\"mine\",\"snap_name\":\"s1\"}", TRUE);


    assert_vm_aliases("vm.batch",
                      "{\"action\":\"stop\",\"vms\":[\"mine\"],\"name\":\"a\",\"vm\":\"b\"}", TRUE);
    assert_vm_aliases("vm.list", "{\"name\":\"a\",\"vm_id\":\"b\"}", TRUE);
    assert_vm_aliases("vm.create", "{\"name\":\"a\",\"vm_id\":\"b\"}", TRUE);
    assert_vm_aliases("container.start", "{\"name\":\"a\",\"container\":\"b\"}", TRUE);


    const gchar *reason = "sentinel";
    g_assert_true(pcv_dispatcher_vm_target_aliases_valid("vm.delete", NULL, &reason));
    g_assert_null(reason);
}

void
test_vpc_rbac_register(void)
{
    g_test_add_func("/dispatcher/rbac/vm_target_alias_single_target",
                    test_dispatcher_vm_target_alias_single_target);
    g_test_add_func("/dispatcher/rbac/default_deny_unmapped",
                    test_dispatcher_policy_default_deny);
    g_test_add_func("/dispatcher/rbac/formerly_unmapped_match_rest",
                    test_dispatcher_policy_formerly_unmapped_match_rest);
    g_test_add_func("/vpc/rbac/rest_dispatcher_agreement",
                    test_vpc_rbac_rest_and_dispatcher_agree);
    g_test_add_func("/d2/rbac/rest_dispatcher_agreement",
                    test_d2_rbac_rest_and_dispatcher_agree);
    g_test_add_func("/ui/rbac/shared_read_agreement",
                    test_shared_ui_reads_rest_and_dispatcher_agree);
}
