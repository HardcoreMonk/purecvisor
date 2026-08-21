                                                                                       
                                                                             
                                                                                                  
                                 
                                                                 
                                  
   
                                     
                                                                 
  
                                   
                                                                 
                                                                       
  
                                                                               
                                     
                                                                               
                                                        
                                         
                                                               
                                                          
                                                          
                
                           
                          
                                                                      
                                                          
  
                                                 
                                                           
                                                  
                                                               
                          
  
                                              
                                                         
                                                                          
                                                                               
   
#include <glib.h>
#include <glib/gstdio.h>
#include <errno.h>                                            
#include <sys/stat.h>
#include "../src/modules/lxc/lxc_owner.h"
#include "../src/utils/pcv_config.h"

  
                                                                            
                                                                
                                                         
                                           
   
static gboolean owner_match_decision(const gchar *name, const gchar *caller_sub) {
    gchar *owner = pcv_lxc_read_owner(name);
    gboolean allowed = (owner && g_strcmp0(owner, caller_sub) == 0);
    g_free(owner);
    return allowed;
}

                                                                   
typedef struct {
    gchar *root;                        
    gchar *conf;                           
} OwnerFixture;

                                                        
static void owner_remove_tree(const gchar *path) {
    GStatBuf st;
    if (!path)
        return;
    if (g_lstat(path, &st) != 0) {
        g_assert_cmpint(errno, ==, ENOENT);
        return;
    }
    if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
        g_assert_cmpint(g_unlink(path), ==, 0);
        return;
    }

    GError *error = NULL;
    GDir *dir = g_dir_open(path, 0, &error);
    g_assert_no_error(error);
    g_assert_nonnull(dir);
    const gchar *name = NULL;
    while ((name = g_dir_read_name(dir)) != NULL) {
        gchar *child = g_build_filename(path, name, NULL);
        owner_remove_tree(child);
        g_free(child);
    }
    g_dir_close(dir);
    g_assert_cmpint(g_rmdir(path), ==, 0);
}

static void owner_fixture_setup(OwnerFixture *fx) {
    fx->root = g_dir_make_tmp("pcv-owner-scope-XXXXXX", NULL);
    g_assert_nonnull(fx->root);

    gchar *conf_body = g_strdup_printf("[container]\nlxc_path=%s\n", fx->root);
    fx->conf = g_build_filename(fx->root, "daemon.conf", NULL);
    g_assert_true(g_file_set_contents(fx->conf, conf_body, -1, NULL));
    g_free(conf_body);

    g_setenv("PCV_CONFIG_PATH", fx->conf, TRUE);
    pcv_config_init();
    g_assert_cmpstr(pcv_config_get_container_path(), ==, fx->root);
}

static void owner_fixture_teardown(OwnerFixture *fx) {
    pcv_config_shutdown();
                                                                 
                                                                     
    g_setenv("PCV_CONFIG_PATH", "/nonexistent/pcv-test-isolated.conf", TRUE);
    owner_remove_tree(fx->root);
    g_free(fx->conf);
    g_free(fx->root);
    fx->conf = NULL;
    fx->root = NULL;
}

                                                            
static void make_container_dir(OwnerFixture *fx, const gchar *name) {
    gchar *dir = g_build_filename(fx->root, name, NULL);
    g_assert_cmpint(g_mkdir_with_parents(dir, 0700), ==, 0);
    g_free(dir);
}

                                              
static void test_owner_match_allows_mismatch_denies(void) {
    OwnerFixture fx = {0};
    owner_fixture_setup(&fx);

    make_container_dir(&fx, "c1");
    g_assert_true(pcv_lxc_stamp_owner("c1", "alice"));

    gchar *back = pcv_lxc_read_owner("c1");
    g_assert_cmpstr(back, ==, "alice");                       
    g_free(back);

    g_assert_true(owner_match_decision("c1", "alice"));                
    g_assert_false(owner_match_decision("c1", "bob"));                   

    owner_fixture_teardown(&fx);
}

                                                            
static void test_absent_owner_denies(void) {
    OwnerFixture fx = {0};
    owner_fixture_setup(&fx);

    make_container_dir(&fx, "legacy");                                      
    g_assert_null(pcv_lxc_read_owner("legacy"));
    g_assert_false(owner_match_decision("legacy", "alice"));
    g_assert_false(owner_match_decision("legacy", ""));

    owner_fixture_teardown(&fx);
}

                                              
static void test_empty_owner_sub_not_stamped(void) {
    OwnerFixture fx = {0};
    owner_fixture_setup(&fx);

    make_container_dir(&fx, "c2");
    g_assert_false(pcv_lxc_stamp_owner("c2", NULL));
    g_assert_false(pcv_lxc_stamp_owner("c2", ""));
    g_assert_null(pcv_lxc_read_owner("c2"));
    g_assert_false(owner_match_decision("c2", "alice"));

    owner_fixture_teardown(&fx);
}

                                                    
static void test_per_container_isolation(void) {
    OwnerFixture fx = {0};
    owner_fixture_setup(&fx);

    make_container_dir(&fx, "c_a");
    make_container_dir(&fx, "c_b");
    g_assert_true(pcv_lxc_stamp_owner("c_a", "alice"));
    g_assert_true(pcv_lxc_stamp_owner("c_b", "bob"));

    g_assert_true(owner_match_decision("c_a", "alice"));
    g_assert_false(owner_match_decision("c_a", "bob"));                              
    g_assert_true(owner_match_decision("c_b", "bob"));
    g_assert_false(owner_match_decision("c_b", "alice"));

    owner_fixture_teardown(&fx);
}

                                                        
static void test_stamp_missing_dir_fails(void) {
    OwnerFixture fx = {0};
    owner_fixture_setup(&fx);

                                                  
    g_assert_false(pcv_lxc_stamp_owner("nope", "alice"));
    g_assert_null(pcv_lxc_read_owner("nope"));

    owner_fixture_teardown(&fx);
}

void test_container_owner_scope_register(void) {
    g_test_add_func("/container_owner_scope/owner_match_allows_mismatch_denies",
                    test_owner_match_allows_mismatch_denies);
    g_test_add_func("/container_owner_scope/absent_owner_denies",
                    test_absent_owner_denies);
    g_test_add_func("/container_owner_scope/empty_owner_sub_not_stamped",
                    test_empty_owner_sub_not_stamped);
    g_test_add_func("/container_owner_scope/per_container_isolation",
                    test_per_container_isolation);
    g_test_add_func("/container_owner_scope/stamp_missing_dir_fails",
                    test_stamp_missing_dir_fails);
}
