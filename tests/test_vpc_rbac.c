                                                                                
                                                                                          
                                        
                                                                     
                 
   
                        
                                                                   
  
                                                                 
                                                              
                                          
   
#include <glib.h>
#include <glib/gstdio.h>

#include "modules/auth/pcv_rbac.h"

void pcv_dispatcher_init_policy_map(void);
gboolean pcv_dispatcher_check_rbac(const gchar *method, gint caller_role);

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
        "alert.history", "healing.pending", "healing.history", "agent.history", "config.history",
        "storage.pool.forecast",
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

void
test_vpc_rbac_register(void)
{
    g_test_add_func("/vpc/rbac/rest_dispatcher_agreement",
                    test_vpc_rbac_rest_and_dispatcher_agree);
    g_test_add_func("/d2/rbac/rest_dispatcher_agreement",
                    test_d2_rbac_rest_and_dispatcher_agree);
    g_test_add_func("/ui/rbac/shared_read_agreement",
                    test_shared_ui_reads_rest_and_dispatcher_agree);
}
