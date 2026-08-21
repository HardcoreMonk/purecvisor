                                                                                         
                                                                                        
                                                                     
                                
                                                             
                            
                   
  
                                                                  
                          
  
                                                  
                                                                 
                                                   
  
                              
                                                                                      
                                                                               
  
                              
                                              
                                                                                 
                                                                         
                                                                 
                                                                           
                                                                           
                                                                    
                                                    
  
                       
                                                                           
                                                                            
                                                              
                                                                       
                                                                   
                                                                   
  
                 
                                                                          
                                                                 
                                                                  
                                                                     
                                                                       
                                                                     
                                                                 
                                                              
                                                                       
                                                                  
                                                                 
                                                                   
                                                                           
                                                                    
                                                
                                                             
                                                                             
                                                                       
                                                                       
                                                               
                                                               
                                                       
                                                                             
                                          
                                                               
                                                                  
   
#include <glib.h>
#include <glib/gstdio.h>                                              
#include <errno.h>                                            
#include <string.h>                                                 
#include <sys/stat.h>                                            

#include "modules/storage/pcv_lio.h"

                                                                              
                                                    
static void
lio_remove_tree(const gchar *path)
{
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
        lio_remove_tree(child);
        g_free(child);
    }
    g_dir_close(dir);
    g_assert_cmpint(g_rmdir(path), ==, 0);
}

static void
lio_remove_tree_and_free(gchar *path)
{
    lio_remove_tree(path);
    g_free(path);
}

static void
test_paths(void)
{
    gchar *b = pcv_lio_path_backstore("/tmp/r", "web");
    g_assert_cmpstr(b, ==, "/tmp/r/core/iblock_0/web");

    gchar *t = pcv_lio_path_target("/tmp/r", "iqn.x:web");
    g_assert_cmpstr(t, ==, "/tmp/r/iscsi/iqn.x:web");

    gchar *g = pcv_lio_path_tpg("/tmp/r", "iqn.x:web");
    g_assert_cmpstr(g, ==, "/tmp/r/iscsi/iqn.x:web/tpgt_1");

    gchar *q = pcv_lio_iqn_for_vm("web");
    g_assert_cmpstr(q, ==, PCV_LIO_IQN_PREFIX ":web");

    g_free(b);
    g_free(t);
    g_free(g);
    g_free(q);
}

static void
test_mkdir_idempotent(void)
{
    gchar *dir = g_dir_make_tmp("pcvlioXXXXXX", NULL);
    gchar *p = g_build_filename(dir, "a", "b", NULL);
    GError *e = NULL;

    g_assert_true(pcv_lio_mkdir_p(p, &e));                 
    g_assert_no_error(e);
    g_assert_true(pcv_lio_mkdir_p(p, &e));                                 
    g_assert_no_error(e);
    g_assert_true(g_file_test(p, G_FILE_TEST_IS_DIR));

    g_free(p);
    lio_remove_tree_and_free(dir);
}

static void
test_mkdir_p_rejects_non_directory(void)
{
                                                 
                                                
                                               
    gchar *dir = g_dir_make_tmp("pcvlioXXXXXX", NULL);
    gchar *file_path = g_build_filename(dir, "not-a-dir", NULL);
    GError *e = NULL;

    g_assert_true(g_file_set_contents(file_path, "x", -1, NULL));
    g_assert_false(pcv_lio_mkdir_p(file_path, &e));
    g_assert_error(e, G_FILE_ERROR, G_FILE_ERROR_NOTDIR);

    g_error_free(e);
    g_free(file_path);
    lio_remove_tree_and_free(dir);
}

static void
test_write_attr_and_readback(void)
{
    gchar *dir = g_dir_make_tmp("pcvlioXXXXXX", NULL);
    GError *e = NULL;

                                                            
                                                   
                                                                
    g_assert_true(pcv_lio_write_attr(dir, "udev_path", "/dev/zvol/p/vms/web", &e));
    g_assert_no_error(e);
    gchar *got = pcv_lio_read_attr(dir, "udev_path");
    g_assert_cmpstr(got, ==, "/dev/zvol/p/vms/web");                     
    g_free(got);

                                    
    g_assert_true(pcv_lio_write_attr(dir, "attrib/authentication", "1", &e));
    g_assert_no_error(e);
    gchar *a = pcv_lio_read_attr(dir, "attrib/authentication");
    g_assert_cmpstr(a, ==, "1");
    g_free(a);

                                                           
                                                
                                                    
                                                       
                                                       
                                                                
                                                      
                                                        
                                                  
             
    g_assert_true(pcv_lio_write_attr(dir, "chap_password", "supersecret123", &e));
    g_assert_no_error(e);
    g_assert_true(pcv_lio_write_attr(dir, "chap_password", "abc", &e));
    g_assert_no_error(e);
    gchar *shorter = pcv_lio_read_attr(dir, "chap_password");
    g_assert_cmpstr(shorter, ==, "abc");                                       
    g_free(shorter);

    lio_remove_tree_and_free(dir);
}

static void
test_write_attr_rejects_empty_value(void)
{
                                                                  
                                               
                                         
    gchar *dir = g_dir_make_tmp("pcvlioXXXXXX", NULL);
    GError *e = NULL;

    g_assert_false(pcv_lio_write_attr(dir, "chap_password", "", &e));
    g_assert_error(e, G_FILE_ERROR, G_FILE_ERROR_INVAL);

    g_error_free(e);
    lio_remove_tree_and_free(dir);
}

static void
test_path_traversal_rejected(void)
{
                                                                
                                                
                                                               
                      
                                                         
                                                           
                                                        
                                                                   
                                                  
                                                           
    static const gchar *bad_names[] = { "../x", "a/b", "../../etc/passwd", "." };

    for (guint i = 0; i < G_N_ELEMENTS(bad_names); i++) {
        g_assert_null(pcv_lio_path_backstore("/tmp/r", bad_names[i]));
        g_assert_null(pcv_lio_iqn_for_vm(bad_names[i]));
        g_assert_null(pcv_lio_path_target("/tmp/r", bad_names[i]));
        g_assert_null(pcv_lio_path_tpg("/tmp/r", bad_names[i]));
    }

                                            
                                       
    {
        gchar *root = g_dir_make_tmp("pcvlioXXXXXX", NULL);
        GError *e = NULL;

        g_assert_false(pcv_lio_target_create_at(root, ".", "/dev/zvol/p/x",
                                                NULL, NULL, &e));
        g_assert_error(e, G_FILE_ERROR, G_FILE_ERROR_INVAL);
        g_clear_error(&e);

        g_assert_false(pcv_lio_target_delete_at(root, ".", &e));
        g_assert_error(e, G_FILE_ERROR, G_FILE_ERROR_INVAL);
        g_clear_error(&e);

        lio_remove_tree_and_free(root);
    }

                       
    gchar *b = pcv_lio_path_backstore("/tmp/r", "web-01");
    g_assert_nonnull(b);
    g_free(b);
    gchar *q = pcv_lio_iqn_for_vm("web-01");
    g_assert_nonnull(q);
    g_free(q);
}

static void
test_rmdir_missing_is_ok(void)
{
    GError *e = NULL;

    g_assert_true(pcv_lio_rmdir("/tmp/pcv-lio-does-not-exist-xyz", &e));                   
    g_assert_no_error(e);
}

                                     
  
                                                                
                                                
                                                    
                                                           
                                              
  
                                                            
                                              
static void
test_available_is_root_not_fabric_dir(void)
{
    gchar *dir = g_dir_make_tmp("pcvlioXXXXXX", NULL);

                                                              
                                         
    gchar *iscsi_dir = g_build_filename(dir, "iscsi", NULL);
    g_assert_false(g_file_test(iscsi_dir, G_FILE_TEST_IS_DIR));
    g_assert_true(pcv_lio_available(dir));

                                                      
    g_assert_true(g_mkdir_with_parents(iscsi_dir, 0755) == 0);
    g_assert_true(pcv_lio_available(dir));

                                                                    
    gchar *absent = g_build_filename(dir, "no-such-configfs-root", NULL);
    g_assert_false(pcv_lio_available(absent));

                                                  
    gchar *as_file = g_build_filename(dir, "root-is-a-file", NULL);
    g_assert_true(g_file_set_contents(as_file, "x", 1, NULL));
    g_assert_false(pcv_lio_available(as_file));

    g_free(as_file);
    g_free(absent);
    g_free(iscsi_dir);
    lio_remove_tree_and_free(dir);
}

                                                         
                                                         
                                                          
static void
test_ensure_fabric_creates_and_is_idempotent(void)
{
    gchar *dir = g_dir_make_tmp("pcvliofXXXXXX", NULL);
    gchar *iscsi_dir = g_build_filename(dir, "iscsi", NULL);
    GError *e = NULL;

                 
    g_assert_false(g_file_test(iscsi_dir, G_FILE_TEST_IS_DIR));
    g_assert_true(pcv_lio_ensure_fabric(dir, &e));
    g_assert_no_error(e);
    g_assert_true(g_file_test(iscsi_dir, G_FILE_TEST_IS_DIR));

                                             
    g_assert_true(pcv_lio_ensure_fabric(dir, &e));
    g_assert_no_error(e);
    g_assert_true(g_file_test(iscsi_dir, G_FILE_TEST_IS_DIR));

    g_free(iscsi_dir);
    lio_remove_tree_and_free(dir);
}

                                                
                                                       
                                                  
static void
test_ensure_fabric_reports_error(void)
{
    gchar *dir = g_dir_make_tmp("pcvliogXXXXXX", NULL);
    gchar *iscsi_dir = g_build_filename(dir, "iscsi", NULL);
    GError *e = NULL;

    g_assert_true(g_file_set_contents(iscsi_dir, "x", 1, NULL));               
    g_assert_false(pcv_lio_ensure_fabric(dir, &e));
    g_assert_nonnull(e);
    g_assert_nonnull(strstr(e->message, "iscsi"));                                   
    g_clear_error(&e);

    g_free(iscsi_dir);
    lio_remove_tree_and_free(dir);
}

                                                                            
                          
  
                                              
                                                
                                            
                                                             
                                             
                                            
                                        
                                                                               

#define LIO_T2_VM     "web"
#define LIO_T2_ZVOL   "/dev/zvol/p/vms/web"

                                              
                                                  
                                               
            
static guint
lio_count_entries(const gchar *path)
{
    GDir *dir = g_dir_open(path, 0, NULL);

    if (!dir)
        return 0;

    guint n = 0;
    const gchar *name;

    while ((name = g_dir_read_name(dir)) != NULL) {
        gchar *child = g_build_filename(path, name, NULL);
        GStatBuf st;

        n++;
        if (g_lstat(child, &st) == 0 && S_ISDIR(st.st_mode))
            n += lio_count_entries(child);
        g_free(child);
    }

    g_dir_close(dir);
    return n;
}

                                                              
                            
static gchar *
lio_tpg_of(const gchar *root)
{
    gchar *iqn = pcv_lio_iqn_for_vm(LIO_T2_VM);
    gchar *tpg = pcv_lio_path_tpg(root, iqn);

    g_free(iqn);
    return tpg;
}

   
                                               
                                                            
                                       
                                                                
                                                  
                                                                
                                  
                                                   
                                                              
                                                        
                                                         
                                                              
                                    
   
static void
test_create_rejects_rebind_of_enabled_store(void)
{
    gchar *root = g_dir_make_tmp("pcvlioXXXXXX", NULL);
    GError *e = NULL;
    gchar *store = pcv_lio_path_backstore(root, LIO_T2_VM);

                                                
                                                            
    g_assert_cmpint(g_mkdir_with_parents(store, 0755), ==, 0);
    gchar *store_enable = g_build_filename(store, "enable", NULL);
    gchar *store_udev = g_build_filename(store, "udev_path", NULL);

    g_assert_true(g_file_set_contents(store_enable, "1\n", -1, NULL));
    g_assert_true(g_file_set_contents(store_udev, LIO_T2_ZVOL "\n", -1, NULL));

                                            
    g_assert_false(pcv_lio_target_create_at(root, LIO_T2_VM,
                                            "/dev/zvol/tank2/web", NULL, NULL, &e));
    g_assert_error(e, G_FILE_ERROR, G_FILE_ERROR_EXIST);
    g_clear_error(&e);

                                       
    gchar *bound = pcv_lio_read_attr(store, "udev_path");

    g_assert_cmpstr(bound, ==, LIO_T2_ZVOL);
    g_free(bound);

                                      
    gchar *tpg = lio_tpg_of(root);

    g_assert_false(g_file_test(tpg, G_FILE_TEST_EXISTS));
    g_free(tpg);

                                               
                                            
    g_assert_true(pcv_lio_target_create_at(root, LIO_T2_VM, LIO_T2_ZVOL,
                                           NULL, NULL, &e));
    g_assert_no_error(e);

    g_free(store_udev);
    g_free(store_enable);
    g_free(store);
    lio_remove_tree_and_free(root);
}

                                                     
                                                                       
                                                  
#define LIO_F5_INFO_FMT \
    "Status: ACTIVATED  Max Queue Depth: 0  SectorSize: 512  HwMaxSectors: 32768\n" \
    "        iBlock device: zd144  UDEV PATH: %s  readonly: 0\n" \
    "  exclusive: 1\n" \
    "        Major: 230 Minor: 144  CLAIMED: IBLOCK\n"

                                                      
                                                      
                                                           
static void
lio_make_enabled_store(const gchar *root, const gchar *attr_bound,
                       const gchar *info_bound)
{
    gchar *store = pcv_lio_path_backstore(root, LIO_T2_VM);

    g_assert_cmpint(g_mkdir_with_parents(store, 0755), ==, 0);

    gchar *enable = g_build_filename(store, "enable", NULL);
    gchar *udev = g_build_filename(store, "udev_path", NULL);

    g_assert_true(g_file_set_contents(enable, "1\n", -1, NULL));
    g_assert_true(g_file_set_contents(udev, attr_bound, -1, NULL));

    if (info_bound) {
        gchar *info_path = g_build_filename(store, "info", NULL);
        gchar *info = g_strdup_printf(LIO_F5_INFO_FMT, info_bound);

        g_assert_true(g_file_set_contents(info_path, info, -1, NULL));
        g_free(info);
        g_free(info_path);
    }

    g_free(udev);
    g_free(enable);
    g_free(store);
}

   
                                         
                                                      
                                                                
                                        
  
                                                      
                                                       
                                                          
                                                         
                                                
                    
   
static void
test_rebind_guard_prefers_kernel_info(void)
{
                                                 
    gchar *root = g_dir_make_tmp("pcvlioXXXXXX", NULL);
    GError *e = NULL;

    lio_make_enabled_store(root, "/dev/zvol/tank2/web", LIO_T2_ZVOL);
    g_assert_false(pcv_lio_target_create_at(root, LIO_T2_VM, "/dev/zvol/tank2/web",
                                            NULL, NULL, &e));
    g_assert_error(e, G_FILE_ERROR, G_FILE_ERROR_EXIST);
                                               
                                               
    g_assert_nonnull(strstr(e->message, "커널 실바인딩(info)"));
    g_assert_nonnull(strstr(e->message, LIO_T2_ZVOL));                   
    g_clear_error(&e);
    lio_remove_tree_and_free(root);

                                                       
    root = g_dir_make_tmp("pcvlioXXXXXX", NULL);
    lio_make_enabled_store(root, "/dev/zvol/tank2/web", LIO_T2_ZVOL);
    g_assert_true(pcv_lio_target_create_at(root, LIO_T2_VM, LIO_T2_ZVOL,
                                           NULL, NULL, &e));
    g_assert_no_error(e);
    lio_remove_tree_and_free(root);
}

   
                                             
                                                           
                                                  
                                               
                               
   
static void
test_rebind_guard_falls_back_without_info(void)
{
    GError *e = NULL;

                                                 
    gchar *root = g_dir_make_tmp("pcvlioXXXXXX", NULL);

    lio_make_enabled_store(root, LIO_T2_ZVOL, NULL);
    g_assert_false(pcv_lio_target_create_at(root, LIO_T2_VM, "/dev/zvol/tank2/web",
                                            NULL, NULL, &e));
    g_assert_error(e, G_FILE_ERROR, G_FILE_ERROR_EXIST);
    g_assert_nonnull(strstr(e->message, "대리 지표"));
    g_clear_error(&e);
    lio_remove_tree_and_free(root);

                                                                 
                                              
    root = g_dir_make_tmp("pcvlioXXXXXX", NULL);
    lio_make_enabled_store(root, LIO_T2_ZVOL, NULL);

    gchar *store = pcv_lio_path_backstore(root, LIO_T2_VM);
    gchar *info_path = g_build_filename(store, "info", NULL);

    g_assert_true(g_file_set_contents(info_path,
                                      "Status: ACTIVATED  SectorSize: 512\n"
                                      "        Major: 230 Minor: 144\n", -1, NULL));
    g_assert_true(pcv_lio_target_create_at(root, LIO_T2_VM, LIO_T2_ZVOL,
                                           NULL, NULL, &e));
    g_assert_no_error(e);

    g_free(info_path);
    g_free(store);
    lio_remove_tree_and_free(root);
}

static void
test_create_tree(void)
{
    gchar *root = g_dir_make_tmp("pcvlioXXXXXX", NULL);
    GError *e = NULL;

    g_assert_true(pcv_lio_target_create_at(root, LIO_T2_VM, LIO_T2_ZVOL,
                                           NULL, NULL, &e));
    g_assert_no_error(e);

    gchar *store = pcv_lio_path_backstore(root, LIO_T2_VM);
    gchar *tpg = lio_tpg_of(root);

                                                                         
                                               
                                                                      
                                                               
    gchar *control = pcv_lio_read_attr(store, "control");
    g_assert_nonnull(control);
    g_assert_nonnull(strstr(control, "udev_path=" LIO_T2_ZVOL));
    g_free(control);

                    
    gchar *store_enable = pcv_lio_read_attr(store, "enable");
    g_assert_cmpstr(store_enable, ==, "1");
    g_free(store_enable);

                                                      
    gchar *link = g_build_filename(tpg, "lun", "lun_0", LIO_T2_VM, NULL);
    g_assert_true(g_file_test(link, G_FILE_TEST_IS_SYMLINK));
    gchar *link_target = g_file_read_link(link, NULL);
    g_assert_cmpstr(link_target, ==, store);
    g_free(link_target);
    g_free(link);

              
    gchar *np = g_build_filename(tpg, "np", "0.0.0.0:3260", NULL);
    g_assert_true(g_file_test(np, G_FILE_TEST_IS_DIR));
    g_free(np);

                                                          
                                                                    
    gchar *acls = pcv_lio_read_attr(tpg, "attrib/generate_node_acls");
    g_assert_cmpstr(acls, ==, "1");
    g_free(acls);
    gchar *wp = pcv_lio_read_attr(tpg, "attrib/demo_mode_write_protect");
    g_assert_cmpstr(wp, ==, "0");
    g_free(wp);

                                                             
    gchar *auth = pcv_lio_read_attr(tpg, "attrib/authentication");
    g_assert_cmpstr(auth, ==, "0");
    g_free(auth);
    gchar *userid = g_build_filename(tpg, "auth", "userid", NULL);
    g_assert_false(g_file_test(userid, G_FILE_TEST_EXISTS));
    g_free(userid);

                           
    gchar *tpg_enable = pcv_lio_read_attr(tpg, "enable");
    g_assert_cmpstr(tpg_enable, ==, "1");
    g_free(tpg_enable);

    g_free(tpg);
    g_free(store);
    lio_remove_tree_and_free(root);
}

static void
test_create_idempotent(void)
{
    gchar *root = g_dir_make_tmp("pcvlioXXXXXX", NULL);
    GError *e = NULL;

    g_assert_true(pcv_lio_target_create_at(root, LIO_T2_VM, LIO_T2_ZVOL,
                                           NULL, NULL, &e));
    g_assert_no_error(e);

    guint before = lio_count_entries(root);

                                                 
                                                              
    g_assert_true(pcv_lio_target_create_at(root, LIO_T2_VM, LIO_T2_ZVOL,
                                           NULL, NULL, &e));
    g_assert_no_error(e);
    g_assert_cmpuint(lio_count_entries(root), ==, before);

    lio_remove_tree_and_free(root);
}

static void
test_create_skips_when_already_enabled(void)
{
                                                         
                                                        
                                                                     
                                                         
      
                                                              
                                                
                                                          
                                                      
                       
    gchar *root = g_dir_make_tmp("pcvlioXXXXXX", NULL);
    GError *e = NULL;
    gchar *store = pcv_lio_path_backstore(root, LIO_T2_VM);
    gchar *tpg = lio_tpg_of(root);

    g_assert_cmpint(g_mkdir_with_parents(store, 0755), ==, 0);
    g_assert_cmpint(g_mkdir_with_parents(tpg, 0755), ==, 0);

    gchar *store_enable = g_build_filename(store, "enable", NULL);
    gchar *tpg_enable = g_build_filename(tpg, "enable", NULL);

    g_assert_true(g_file_set_contents(store_enable, "1\n", -1, NULL));
    g_assert_true(g_file_set_contents(tpg_enable, "1\n", -1, NULL));

    g_assert_true(pcv_lio_target_create_at(root, LIO_T2_VM, LIO_T2_ZVOL,
                                           NULL, NULL, &e));
    g_assert_no_error(e);

    gchar *raw = NULL;

    g_assert_true(g_file_get_contents(store_enable, &raw, NULL, NULL));
    g_assert_cmpstr(raw, ==, "1\n");                        
    g_free(raw);
    raw = NULL;
    g_assert_true(g_file_get_contents(tpg_enable, &raw, NULL, NULL));
    g_assert_cmpstr(raw, ==, "1\n");
    g_free(raw);

                                                    
                                                          
    gchar *control = g_build_filename(store, "control", NULL);
    g_assert_false(g_file_test(control, G_FILE_TEST_EXISTS));
    g_free(control);

                                                           
                                                           
                                                     
                                              
                                                                    
                                                      
                              
    gchar *udev = pcv_lio_read_attr(store, "udev_path");
    g_assert_cmpstr(udev, ==, LIO_T2_ZVOL);
    g_free(udev);

    g_free(tpg_enable);
    g_free(store_enable);
    g_free(tpg);
    g_free(store);
    lio_remove_tree_and_free(root);
}

static void
test_create_requires_control_before_enable(void)
{
                                                        
                                     
                                                                     
                                                                       
                                                       
                                        
      
                                                            
                                                            
                                                      
                                                       
                                            
    gchar *root = g_dir_make_tmp("pcvlioXXXXXX", NULL);
    GError *e = NULL;
    gchar *store = pcv_lio_path_backstore(root, LIO_T2_VM);
    gchar *control_dir = g_build_filename(store, "control", NULL);
    gchar *store_enable = g_build_filename(store, "enable", NULL);

    g_assert_cmpint(g_mkdir_with_parents(control_dir, 0755), ==, 0);

    g_assert_false(pcv_lio_target_create_at(root, LIO_T2_VM, LIO_T2_ZVOL,
                                            NULL, NULL, &e));
    g_assert_nonnull(e);
    g_error_free(e);

                                                      
                                  
    g_assert_false(g_file_test(store_enable, G_FILE_TEST_EXISTS));

                                                          
                                                       
    gchar *udev = pcv_lio_read_attr(store, "udev_path");
    g_assert_cmpstr(udev, ==, LIO_T2_ZVOL);
    g_free(udev);

    g_free(store_enable);
    g_free(control_dir);
    g_free(store);
    lio_remove_tree_and_free(root);
}

static void
test_delete_skips_disable_when_already_inactive(void)
{
                                                         
                                                                 
                                                                       
                                                   
                                           
      
                                                            
                                                      
                                                    
                                               
                     
    gchar *root = g_dir_make_tmp("pcvlioXXXXXX", NULL);
    GError *e = NULL;
    gchar *iqn = pcv_lio_iqn_for_vm(LIO_T2_VM);
    gchar *tpg = pcv_lio_path_tpg(root, iqn);
    gchar *tpg_enable = g_build_filename(tpg, "enable", NULL);
    gchar *blocker = g_build_filename(tpg, "lun", "lun_0", "live", "deeper", NULL);

    g_assert_cmpint(g_mkdir_with_parents(blocker, 0755), ==, 0);
    g_assert_true(g_file_set_contents(tpg_enable, "0\n", -1, NULL));

                             
    g_assert_false(pcv_lio_target_delete_at(root, LIO_T2_VM, &e));
    g_assert_nonnull(e);
    g_error_free(e);

                                                     
                                                        
    gchar *raw = NULL;
    g_assert_true(g_file_get_contents(tpg_enable, &raw, NULL, NULL));
    g_assert_cmpstr(raw, ==, "0\n");
    g_free(raw);

    g_free(blocker);
    g_free(tpg_enable);
    g_free(tpg);
    g_free(iqn);
    lio_remove_tree_and_free(root);
}

static void
test_create_rollback(void)
{
                                                          
                                                          
                                                                
                     
                                              
                                                         
                                                      
    gchar *root = g_dir_make_tmp("pcvlioXXXXXX", NULL);
    GError *e = NULL;

                                                   
                                          
    g_assert_true(pcv_lio_target_create_at(root, "other", "/dev/zvol/p/vms/other",
                                           NULL, NULL, &e));
    g_assert_no_error(e);
    guint other_entries = lio_count_entries(root);

    gchar *iqn = pcv_lio_iqn_for_vm(LIO_T2_VM);
    gchar *target = pcv_lio_path_target(root, iqn);

    g_assert_true(g_file_set_contents(target, "x", -1, NULL));

    g_assert_false(pcv_lio_target_create_at(root, LIO_T2_VM, LIO_T2_ZVOL,
                                            NULL, NULL, &e));
    g_assert_nonnull(e);
    g_error_free(e);

                                                 
                                                           
    gchar *store = pcv_lio_path_backstore(root, LIO_T2_VM);
    g_assert_false(g_file_test(store, G_FILE_TEST_EXISTS));

                                               
    g_assert_true(g_file_test(target, G_FILE_TEST_IS_REGULAR));

                                         
    g_assert_cmpuint(lio_count_entries(root), ==, other_entries + 1);

    g_free(store);
    g_free(target);
    g_free(iqn);
    lio_remove_tree_and_free(root);
}

static void
test_create_rollback_keeps_preexisting(void)
{
                                                 
                                                
                                              
                                                  
      
                                                           
                                                  
                                                          
    gchar *root = g_dir_make_tmp("pcvlioXXXXXX", NULL);
    GError *e = NULL;
    gchar *store = pcv_lio_path_backstore(root, LIO_T2_VM);
    gchar *iqn = pcv_lio_iqn_for_vm(LIO_T2_VM);
    gchar *target = pcv_lio_path_target(root, iqn);
    gchar *iscsi_dir = g_build_filename(root, "iscsi", NULL);
    gchar *store_control = g_build_filename(store, "control", NULL);
    gchar *store_enable = g_build_filename(store, "enable", NULL);

    g_assert_cmpint(g_mkdir_with_parents(store, 0755), ==, 0);
    g_assert_true(g_file_set_contents(store_control,
                                      "udev_path=" LIO_T2_ZVOL, -1, NULL));
    g_assert_true(g_file_set_contents(store_enable, "1\n", -1, NULL));

    g_assert_cmpint(g_mkdir_with_parents(iscsi_dir, 0755), ==, 0);
    g_assert_true(g_file_set_contents(target, "x", -1, NULL));

    g_assert_false(pcv_lio_target_create_at(root, LIO_T2_VM, LIO_T2_ZVOL,
                                            NULL, NULL, &e));
    g_assert_nonnull(e);
    g_error_free(e);

                                                      
    g_assert_true(g_file_test(store, G_FILE_TEST_IS_DIR));
    gchar *raw = NULL;
    g_assert_true(g_file_get_contents(store_enable, &raw, NULL, NULL));
    g_assert_cmpstr(raw, ==, "1\n");
    g_free(raw);
    g_assert_true(g_file_test(store_control, G_FILE_TEST_IS_REGULAR));

    g_free(store_enable);
    g_free(store_control);
    g_free(iscsi_dir);
    g_free(target);
    g_free(iqn);
    g_free(store);
    lio_remove_tree_and_free(root);
}

static void
test_delete_reverse_order(void)
{
    gchar *root = g_dir_make_tmp("pcvlioXXXXXX", NULL);
    GError *e = NULL;

    g_assert_true(pcv_lio_target_create_at(root, LIO_T2_VM, LIO_T2_ZVOL,
                                           NULL, NULL, &e));
    g_assert_no_error(e);
    g_assert_true(pcv_lio_target_delete_at(root, LIO_T2_VM, &e));
    g_assert_no_error(e);

                                                                
                                                   
                                                                     
                                                    
                                                    
                             
    gchar *store = pcv_lio_path_backstore(root, LIO_T2_VM);
    gchar *iblock = g_build_filename(root, "core", "iblock_0", NULL);
    gchar *iscsi_dir = g_build_filename(root, "iscsi", NULL);

    g_assert_false(g_file_test(store, G_FILE_TEST_EXISTS));
    g_assert_cmpuint(lio_count_entries(iblock), ==, 0);
    g_assert_cmpuint(lio_count_entries(iscsi_dir), ==, 0);

                                               
    g_assert_true(pcv_lio_target_delete_at(root, LIO_T2_VM, &e));
    g_assert_no_error(e);
    g_assert_cmpuint(lio_count_entries(iblock), ==, 0);
    g_assert_cmpuint(lio_count_entries(iscsi_dir), ==, 0);

    g_free(iscsi_dir);
    g_free(iblock);
    g_free(store);
    lio_remove_tree_and_free(root);
}

static void
test_delete_missing_is_ok(void)
{
    gchar *root = g_dir_make_tmp("pcvlioXXXXXX", NULL);
    GError *e = NULL;

    g_assert_true(pcv_lio_target_delete_at(root, "ghost", &e));
    g_assert_no_error(e);

                                                      
                                                       
                                            
    g_assert_cmpuint(lio_count_entries(root), ==, 0);

                                                      
                                                  
    g_assert_false(pcv_lio_target_delete_at(root, "../escape", &e));
    g_assert_nonnull(e);
    g_error_free(e);

    lio_remove_tree_and_free(root);
}

                                                                            
                               
  
                                                  
                                                         
                                                         
                                                   
                                                  
                                                
                                    
                                                                               

                                              
                                       
#define LIO_T3_USER   "user1"
#define LIO_T3_PASS   "s3cr3t-original-value"
#define LIO_T3_PASS2  "n3w"

                                                
                                                 
                                              
                                                  
                                                      
static guint
lio_count_files_containing(const gchar *path, const gchar *needle,
                           gchar **found_path)
{
    GDir *dir = g_dir_open(path, 0, NULL);

    if (!dir)
        return 0;

    guint hits = 0;
    const gchar *name;

    while ((name = g_dir_read_name(dir)) != NULL) {
        gchar *child = g_build_filename(path, name, NULL);
        GStatBuf st;

        if (g_lstat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
            hits += lio_count_files_containing(child, needle, found_path);
        } else if (g_lstat(child, &st) == 0 && S_ISREG(st.st_mode)) {
            gchar *contents = NULL;

            if (g_file_get_contents(child, &contents, NULL, NULL)) {
                if (strstr(contents, needle) != NULL) {
                    hits++;
                    if (found_path && !*found_path)
                        *found_path = g_strdup(child);
                }
                g_free(contents);
            }
        }
        g_free(child);
    }

    g_dir_close(dir);
    return hits;
}

static void
test_create_with_chap(void)
{
    gchar *root = g_dir_make_tmp("pcvlioXXXXXX", NULL);
    GError *e = NULL;

    g_assert_true(pcv_lio_target_create_at(root, LIO_T2_VM, LIO_T2_ZVOL,
                                           LIO_T3_USER, LIO_T3_PASS, &e));
    g_assert_no_error(e);

    gchar *tpg = lio_tpg_of(root);

                                                   
    gchar *userid = pcv_lio_read_attr(tpg, "auth/userid");
    g_assert_cmpstr(userid, ==, LIO_T3_USER);
    g_free(userid);
    gchar *password = pcv_lio_read_attr(tpg, "auth/password");
    g_assert_cmpstr(password, ==, LIO_T3_PASS);
    g_free(password);
    gchar *authn = pcv_lio_read_attr(tpg, "attrib/authentication");
    g_assert_cmpstr(authn, ==, "1");
    g_free(authn);

                                                      
                                                
                                                    
                                                                 
                                                      
                                              
    gchar *pw_file = g_build_filename(tpg, "auth", "password", NULL);
    gchar *raw = NULL;

    g_assert_true(g_file_get_contents(pw_file, &raw, NULL, NULL));
    g_assert_cmpstr(raw, ==, LIO_T3_PASS);
    g_free(raw);

                                                             
                                                         
                                                                   
                                                             
                                          
                                                         
                                               
    g_assert_true(pcv_lio_target_create_at(root, LIO_T2_VM, LIO_T2_ZVOL,
                                           LIO_T3_USER, LIO_T3_PASS2, &e));
    g_assert_no_error(e);

    raw = NULL;
    g_assert_true(g_file_get_contents(pw_file, &raw, NULL, NULL));
    g_assert_cmpstr(raw, ==, LIO_T3_PASS2);                         
    g_free(raw);

    g_free(pw_file);
    g_free(tpg);
    lio_remove_tree_and_free(root);
}

                                                  
                      
static void
lio_assert_chap_absent(const gchar *user, const gchar *password)
{
    gchar *root = g_dir_make_tmp("pcvlioXXXXXX", NULL);
    GError *e = NULL;

    g_assert_true(pcv_lio_target_create_at(root, LIO_T2_VM, LIO_T2_ZVOL,
                                           user, password, &e));
    g_assert_no_error(e);

    gchar *tpg = lio_tpg_of(root);
    gchar *authn = pcv_lio_read_attr(tpg, "attrib/authentication");

    g_assert_cmpstr(authn, ==, "0");
    g_free(authn);

                                                       
                                                      
    gchar *auth_dir = g_build_filename(tpg, "auth", NULL);
    gchar *userid = g_build_filename(auth_dir, "userid", NULL);
    gchar *pw = g_build_filename(auth_dir, "password", NULL);

    g_assert_false(g_file_test(userid, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(pw, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(auth_dir, G_FILE_TEST_EXISTS));

    g_free(pw);
    g_free(userid);
    g_free(auth_dir);
    g_free(tpg);
    lio_remove_tree_and_free(root);
}

static void
test_chap_absent_disables_auth(void)
{
                                               
                                                  
                                                       
                                                      
    lio_assert_chap_absent(LIO_T3_USER, NULL);             
    lio_assert_chap_absent(NULL, LIO_T3_PASS);              
    lio_assert_chap_absent(LIO_T3_USER, "");                 
    lio_assert_chap_absent("", LIO_T3_PASS);                
    lio_assert_chap_absent(NULL, NULL);                             
}

static void
test_no_argv_exposure(void)
{
                                                 
                                                                   
                                      
                                                       
                         
                                                    
                                          
                                               
                                                                   
    gchar *root = g_dir_make_tmp("pcvlioXXXXXX", NULL);
    GError *e = NULL;

    g_assert_true(pcv_lio_target_create_at(root, LIO_T2_VM, LIO_T2_ZVOL,
                                           LIO_T3_USER, LIO_T3_PASS, &e));
    g_assert_no_error(e);

    gchar *found = NULL;
    guint hits = lio_count_files_containing(root, LIO_T3_PASS, &found);

    g_assert_cmpuint(hits, ==, 1);

    gchar *tpg = lio_tpg_of(root);
    gchar *expected = g_build_filename(tpg, "auth", "password", NULL);

    g_assert_cmpstr(found, ==, expected);

    g_free(expected);
    g_free(found);
    g_free(tpg);
    lio_remove_tree_and_free(root);

                                                             
                                                        
                      
    gchar *root2 = g_dir_make_tmp("pcvlioXXXXXX", NULL);
    gchar *tpg2 = lio_tpg_of(root2);
    gchar *pw_dir = g_build_filename(tpg2, "auth", "password", NULL);

    g_assert_cmpint(g_mkdir_with_parents(pw_dir, 0755), ==, 0);

    GError *e2 = NULL;

    g_assert_false(pcv_lio_target_create_at(root2, LIO_T2_VM, LIO_T2_ZVOL,
                                            LIO_T3_USER, LIO_T3_PASS, &e2));
    g_assert_nonnull(e2);
                                            
                                    
    g_assert_nonnull(strstr(e2->message, "auth/password"));
    g_assert_null(strstr(e2->message, LIO_T3_PASS));
    g_error_free(e2);

    g_free(pw_dir);
    g_free(tpg2);
    lio_remove_tree_and_free(root2);
}

void
test_lio_register(void)
{
    g_test_add_func("/lio/paths", test_paths);
    g_test_add_func("/lio/mkdir_idempotent", test_mkdir_idempotent);
    g_test_add_func("/lio/mkdir_p_rejects_file", test_mkdir_p_rejects_non_directory);
    g_test_add_func("/lio/write_attr", test_write_attr_and_readback);
    g_test_add_func("/lio/write_attr_rejects_empty", test_write_attr_rejects_empty_value);
    g_test_add_func("/lio/path_traversal_rejected", test_path_traversal_rejected);
    g_test_add_func("/lio/rmdir_missing", test_rmdir_missing_is_ok);
    g_test_add_func("/lio/available", test_available_is_root_not_fabric_dir);
    g_test_add_func("/lio/ensure_fabric", test_ensure_fabric_creates_and_is_idempotent);
    g_test_add_func("/lio/ensure_fabric_error", test_ensure_fabric_reports_error);

                                 
    g_test_add_func("/lio/create_tree", test_create_tree);
    g_test_add_func("/lio/create_idempotent", test_create_idempotent);
    g_test_add_func("/lio/create_skips_when_already_enabled",
                    test_create_skips_when_already_enabled);
    g_test_add_func("/lio/create_rejects_rebind_of_enabled_store",
                    test_create_rejects_rebind_of_enabled_store);
    g_test_add_func("/lio/rebind_guard_prefers_kernel_info",
                    test_rebind_guard_prefers_kernel_info);
    g_test_add_func("/lio/rebind_guard_falls_back_without_info",
                    test_rebind_guard_falls_back_without_info);
    g_test_add_func("/lio/create_requires_control_before_enable",
                    test_create_requires_control_before_enable);
    g_test_add_func("/lio/delete_skips_disable_when_already_inactive",
                    test_delete_skips_disable_when_already_inactive);
    g_test_add_func("/lio/create_rollback", test_create_rollback);
    g_test_add_func("/lio/create_rollback_keeps_preexisting",
                    test_create_rollback_keeps_preexisting);
    g_test_add_func("/lio/delete_reverse_order", test_delete_reverse_order);
    g_test_add_func("/lio/delete_missing_is_ok", test_delete_missing_is_ok);

                                      
    g_test_add_func("/lio/create_with_chap", test_create_with_chap);
    g_test_add_func("/lio/chap_absent_disables_auth", test_chap_absent_disables_auth);
    g_test_add_func("/lio/no_argv_exposure", test_no_argv_exposure);
}
