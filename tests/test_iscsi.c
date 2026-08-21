                                                                              
                                                                                                 
                                                                             
                                            
                                                              
                                 
                     
  
                                                           
  
                                                       
                                           
                                                   
  
                                
                                                            
                                                     
                                                                   
                                                              
                                                       
                                            
                                                     
  
                 
                                                                               
                                                                     
                                                                                
                                                                         
                                                                    
                                                                     
                                                                           
  
                                             
                                              
                                                      
                                                     
                                                                     
                                   
                                                             
                                                      
                               
                                           
                                                                 
                                                          
                                                  
  
                 
                                                                
                                                         
                                        
   
#include <glib.h>
#include <glib/gstdio.h>                                   
#include <errno.h>                                            
#include <string.h>
#include <sys/stat.h>                   
#include <unistd.h>                           

#include "modules/storage/iscsi_manager.h"
#include "modules/storage/pcv_iscsi_node_db.h"
#include "modules/storage/pcv_lio.h"
#include "utils/pcv_config.h"

                                                         
static void
iscsi_remove_tree(const gchar *path)
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
        iscsi_remove_tree(child);
        g_free(child);
    }
    g_dir_close(dir);
    g_assert_cmpint(g_rmdir(path), ==, 0);
}

                                                         
                                             
#define ISCSI_T4_VM   "web-prod"
                                                     
                                            
#define ISCSI_F3_ZVOL "/dev/zvol/pcvpool/vms/web-prod"

   
                                
                                                               
                                                                   
                                                 
   
static void
test_list_shape_without_init(void)
{
    JsonArray *arr = pcv_iscsi_target_list();

    g_assert_nonnull(arr);
    g_assert_cmpuint(json_array_get_length(arr), ==, 0);
    json_array_unref(arr);
}

   
                                  
                                                            
                                                 
                                                           
                               
   
static void
test_create_without_init_fails(void)
{
    GError *e = NULL;

    g_assert_false(pcv_iscsi_target_create(ISCSI_T4_VM, "/dev/zvol/p/vms/web", &e));
    g_assert_nonnull(e);
    g_error_free(e);
}

   
                   
                                                 
                                                 
                                                             
                         
   
static void
test_iqn_format(void)
{
    g_assert_cmpstr(PCV_LIO_IQN_PREFIX, ==, "iqn.2026-03.purecvisor");

    gchar *iqn = pcv_lio_iqn_for_vm(ISCSI_T4_VM);

    g_assert_cmpstr(iqn, ==, "iqn.2026-03.purecvisor:" ISCSI_T4_VM);
    g_free(iqn);
}

                                                                  
static void
test_node_db_runtime_root_constants(void)
{
    g_assert_cmpstr(PCV_ISCSI_NODE_DB_PRIMARY_ROOT, ==,
                    "/var/lib/iscsi/nodes");
    g_assert_cmpstr(PCV_ISCSI_NODE_DB_LEGACY_ROOT, ==,
                    "/etc/iscsi/nodes");
    g_assert_cmpstr(PCV_ISCSI_NODE_DB_LOCK_ROOT, ==,
                    "/run/lock/iscsi");
}

   
                          
                                                           
                                                     
                               
   
static void
test_chap_absent_is_ok(void)
{
    GError *e = NULL;

    g_assert_true(pcv_iscsi_chap_validate(NULL, NULL, &e));
    g_assert_no_error(e);
    g_assert_true(pcv_iscsi_chap_validate("", "", &e));
    g_assert_no_error(e);
    g_assert_true(pcv_iscsi_chap_validate(NULL, "", &e));
    g_assert_no_error(e);
}

   
                                      
                                                   
                                                              
                                                       
                                                
                                                                      
                                                   
                                                    
                                       
                                            
   
static void
test_chap_half_config_fails_closed(void)
{
    GError *e = NULL;

                                                
    g_assert_false(pcv_iscsi_chap_validate("pcvuser", NULL, &e));
    g_assert_error(e, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&e);

                             
    g_assert_false(pcv_iscsi_chap_validate("pcvuser", "", &e));
    g_assert_error(e, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&e);

                                       
    g_assert_false(pcv_iscsi_chap_validate(NULL, "s3cr3t-pass", &e));
    g_assert_error(e, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&e);

                                             
    g_assert_true(pcv_iscsi_chap_validate("pcvuser", "s3cr3t-pass", &e));
    g_assert_no_error(e);
}

   
                                   
                                                   
                                                                    
                                               
                                     
                                                  
                                                   
            
   
static void
test_chap_kernel_traps_rejected(void)
{
    GError *e = NULL;
                                                 
    g_assert_false(pcv_iscsi_chap_validate("pcvuser", "NULLxyz", &e));
    g_assert_error(e, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&e);
    g_assert_false(pcv_iscsi_chap_validate("NULLuser", "s3cr3t-pass", &e));
    g_assert_error(e, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&e);

                                     
    gchar *too_long = g_strnfill(256, 'a');
    gchar *at_limit = g_strnfill(255, 'a');

    g_assert_false(pcv_iscsi_chap_validate("pcvuser", too_long, &e));
    g_assert_error(e, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&e);
    g_assert_true(pcv_iscsi_chap_validate("pcvuser", at_limit, &e));
    g_assert_no_error(e);
    g_free(too_long);
    g_free(at_limit);

                                    
    g_assert_false(pcv_iscsi_chap_validate("pcvuser", "s3cr3t-pass\n", &e));
    g_assert_error(e, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&e);
    g_assert_false(pcv_iscsi_chap_validate("pcvuser", "s3c\nr3t", &e));
    g_assert_error(e, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&e);
    g_assert_false(pcv_iscsi_chap_validate("pcv\ruser", "s3cr3t-pass", &e));
    g_assert_error(e, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&e);
}

   
                                        
                                                
                                                
                                           
   
static void
test_chap_error_never_carries_secret(void)
{
    static const gchar *sentinel = "NULLtopsecret-canary";
    GError *e = NULL;

    g_assert_false(pcv_iscsi_chap_validate("pcvuser", sentinel, &e));
    g_assert_nonnull(e);
    g_assert_null(strstr(e->message, sentinel));
    g_assert_null(strstr(e->message, "topsecret-canary"));
    g_clear_error(&e);

    gchar *too_long = g_strnfill(300, 'z');

    g_assert_false(pcv_iscsi_chap_validate("pcvuser", too_long, &e));
    g_assert_nonnull(e);
    g_assert_null(strstr(e->message, too_long));
    g_clear_error(&e);
    g_free(too_long);
}

                                                                            
                                          
  
                                                
                                                
                                  
                                                                               

   
                       
                                          
                                        
                                                        
                                        
   
static guint
iscsi_count_entries(const gchar *path)
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
            n += iscsi_count_entries(child);
        g_free(child);
    }

    g_dir_close(dir);
    return n;
}

   
                                           
                              
  
                                                  
                                             
                                                                
                                                  
                                                   
                                             
  
                                         
                                    
                                   
                                                       
  
                                                  
                          
   
static void
test_delete_reaps_orphan_without_record(void)
{
    gchar *root = g_dir_make_tmp("pcv-iscsi-orphan-XXXXXX", NULL);
    GError *e = NULL;

    g_assert_nonnull(root);

                                              
                                             
    g_assert_true(pcv_lio_target_create_at(root, ISCSI_T4_VM, ISCSI_F3_ZVOL,
                                           NULL, NULL, &e));
    g_assert_no_error(e);

                                                        
                                                   
    pcv_iscsi_init_at(root);

                              
    JsonArray *before = pcv_iscsi_target_list();

    g_assert_cmpuint(json_array_get_length(before), ==, 0);
    json_array_unref(before);

                            
    gchar *store = pcv_lio_path_backstore(root, ISCSI_T4_VM);
    gchar *iblock = g_build_filename(root, "core", "iblock_0", NULL);
    gchar *iscsi_dir = g_build_filename(root, "iscsi", NULL);

    g_assert_true(g_file_test(store, G_FILE_TEST_IS_DIR));
    g_assert_cmpuint(iscsi_count_entries(iscsi_dir), >, 0);

                                  
    g_assert_true(pcv_iscsi_target_delete_at(root, ISCSI_T4_VM, &e));
    g_assert_no_error(e);

    g_assert_false(g_file_test(store, G_FILE_TEST_EXISTS));
    g_assert_cmpuint(iscsi_count_entries(iblock), ==, 0);
    g_assert_cmpuint(iscsi_count_entries(iscsi_dir), ==, 0);

    pcv_iscsi_shutdown();

    g_free(iscsi_dir);
    g_free(iblock);
    g_free(store);
    iscsi_remove_tree(root);
    g_free(root);
}

   
                                
                                                         
                                                    
                                              
                                            
   
static void
test_create_delete_roundtrip(void)
{
    gchar *root = g_dir_make_tmp("pcv-iscsi-rt-XXXXXX", NULL);
    GError *e = NULL;

    g_assert_nonnull(root);
    pcv_iscsi_init_at(root);

    g_assert_true(pcv_iscsi_target_create_at(root, ISCSI_T4_VM, ISCSI_F3_ZVOL, &e));
    g_assert_no_error(e);

                                                   
    JsonArray *arr = pcv_iscsi_target_list();

    g_assert_cmpuint(json_array_get_length(arr), ==, 1);
    JsonObject *o = json_array_get_object_element(arr, 0);
    gchar *iqn = pcv_lio_iqn_for_vm(ISCSI_T4_VM);

    g_assert_cmpstr(json_object_get_string_member(o, "vm_name"), ==, ISCSI_T4_VM);
    g_assert_cmpstr(json_object_get_string_member(o, "iqn"), ==, iqn);
    g_free(iqn);
    json_array_unref(arr);

                                           
    gchar *store = pcv_lio_path_backstore(root, ISCSI_T4_VM);
    gchar *iblock = g_build_filename(root, "core", "iblock_0", NULL);
    gchar *iscsi_dir = g_build_filename(root, "iscsi", NULL);

    g_assert_true(g_file_test(store, G_FILE_TEST_IS_DIR));

                                                    
    g_assert_true(pcv_iscsi_target_create_at(root, ISCSI_T4_VM, ISCSI_F3_ZVOL, &e));
    g_assert_no_error(e);
    arr = pcv_iscsi_target_list();
    g_assert_cmpuint(json_array_get_length(arr), ==, 1);                 
    json_array_unref(arr);

                               
    g_assert_true(pcv_iscsi_target_delete_at(root, ISCSI_T4_VM, &e));
    g_assert_no_error(e);

    arr = pcv_iscsi_target_list();
    g_assert_cmpuint(json_array_get_length(arr), ==, 0);
    json_array_unref(arr);
    g_assert_false(g_file_test(store, G_FILE_TEST_EXISTS));
    g_assert_cmpuint(iscsi_count_entries(iblock), ==, 0);
    g_assert_cmpuint(iscsi_count_entries(iscsi_dir), ==, 0);

    pcv_iscsi_shutdown();

    g_free(iscsi_dir);
    g_free(iblock);
    g_free(store);
    iscsi_remove_tree(root);
    g_free(root);
}

   
                                        
                                
  
                                                           
                                                  
                                                                  
                                            
  
                                                             
                                                             
                                                       
                                                          
                                      
  
                                                          
                                    
   
static void
test_create_refuses_half_chap_config(void)
{
    gchar *root = g_dir_make_tmp("pcv-iscsi-chap-XXXXXX", NULL);
    gchar *cfg_dir = g_dir_make_tmp("pcv-iscsi-cfg-XXXXXX", NULL);
    gchar *cfg = g_build_filename(cfg_dir, "daemon.conf", NULL);
    gchar *old_config_path = g_strdup(g_getenv("PCV_CONFIG_PATH"));
    gchar *old_secret = g_strdup(g_getenv("PCV_SECRET_ISCSI_CHAP_PASSWORD"));
    GError *e = NULL;

    g_assert_nonnull(root);
    g_assert_nonnull(cfg_dir);

                                              
                                           
    g_assert_true(g_file_set_contents(cfg, "[iscsi]\nchap_user = pcvuser\n", -1, NULL));
    g_unsetenv("PCV_SECRET_ISCSI_CHAP_PASSWORD");
    g_setenv("PCV_CONFIG_PATH", cfg, TRUE);
    pcv_config_init();

    g_assert_cmpstr(pcv_config_get_string("iscsi", "chap_user", NULL), ==, "pcvuser");

    pcv_iscsi_init_at(root);

                                            
    g_assert_false(pcv_iscsi_target_create_at(root, ISCSI_T4_VM, ISCSI_F3_ZVOL, &e));
    g_assert_error(e, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&e);

    gchar *store = pcv_lio_path_backstore(root, ISCSI_T4_VM);
    gchar *iblock = g_build_filename(root, "core", "iblock_0", NULL);
    gchar *iscsi_dir = g_build_filename(root, "iscsi", NULL);

    g_assert_false(g_file_test(store, G_FILE_TEST_EXISTS));
    g_assert_cmpuint(iscsi_count_entries(iblock), ==, 0);
    g_assert_cmpuint(iscsi_count_entries(iscsi_dir), ==, 0);                              

                                                     
    JsonArray *arr = pcv_iscsi_target_list();

    g_assert_cmpuint(json_array_get_length(arr), ==, 0);
    json_array_unref(arr);

    pcv_iscsi_shutdown();
    pcv_config_shutdown();
    if (old_config_path)
        g_setenv("PCV_CONFIG_PATH", old_config_path, TRUE);
    else
        g_unsetenv("PCV_CONFIG_PATH");
    if (old_secret)
        g_setenv("PCV_SECRET_ISCSI_CHAP_PASSWORD", old_secret, TRUE);
    else
        g_unsetenv("PCV_SECRET_ISCSI_CHAP_PASSWORD");

    g_free(iscsi_dir);
    g_free(iblock);
    g_free(store);
    g_free(cfg);
    iscsi_remove_tree(cfg_dir);
    iscsi_remove_tree(root);
    g_free(old_secret);
    g_free(old_config_path);
    g_free(cfg_dir);
    g_free(root);
}

                                                                            
                                                         
  
                                                                                
                                                             
                                                                               

#define ISCSI_NODE_IQN "iqn.2026-03.purecvisor:chap-fixture"
#define ISCSI_NODE_IP "192.0.2.19"
#define ISCSI_NODE_SECRET "pcv-d4f1-synthetic-secret"

typedef struct {
    gchar *root;
    gchar *nodes;
    gchar *locks;
    gchar *target;
    gchar *portal;
} IscsiNodeFixture;

static IscsiNodeFixture
iscsi_node_fixture_new(void)
{
    IscsiNodeFixture f = {0};

    f.root = g_dir_make_tmp("pcv-iscsi-node-db-XXXXXX", NULL);
    g_assert_nonnull(f.root);
    f.nodes = g_build_filename(f.root, "nodes", NULL);
    f.locks = g_build_filename(f.root, "lock", NULL);
    f.target = g_build_filename(f.nodes, ISCSI_NODE_IQN, NULL);
    f.portal = g_build_filename(f.target, ISCSI_NODE_IP ",3260,1", NULL);
    g_assert_cmpint(g_mkdir_with_parents(f.portal, 0700), ==, 0);
    g_assert_cmpint(g_mkdir_with_parents(f.locks, 0700), ==, 0);
    return f;
}

static void
iscsi_node_fixture_clear(IscsiNodeFixture *f)
{
    iscsi_remove_tree(f->root);
    g_free(f->portal);
    g_free(f->target);
    g_free(f->locks);
    g_free(f->nodes);
    g_free(f->root);
    memset(f, 0, sizeof(*f));
}

static gchar *
iscsi_node_record_text_for(const gchar *iface, const gchar *address,
                           gboolean duplicate_password)
{
    return g_strdup_printf(
        "# BEGIN RECORD 2.1-11\n"
        "node.name = %s\n"
        "node.tpgt = 1\n"
        "node.startup = manual\n"
        "iface.iscsi_ifacename = %s\n"
        "node.conn[0].address = %s\n"
        "node.conn[0].port = 3260\n"
        "node.session.auth.authmethod = None\n"
        "node.session.auth.username = old-user\n"
        "node.session.auth.password = old-password\n"
        "%s"
        "node.session.timeo.replacement_timeout = 120\n",
        ISCSI_NODE_IQN, iface, address,
        duplicate_password ? "node.session.auth.password = duplicate\n" : "");
}

static gchar *
iscsi_node_record_text(const gchar *iface, gboolean duplicate_password)
{
    return iscsi_node_record_text_for(iface, ISCSI_NODE_IP,
                                      duplicate_password);
}

static gchar *
iscsi_node_write_record(const gchar *dir, const gchar *name,
                        const gchar *text)
{
    gchar *path = g_build_filename(dir, name, NULL);

    g_assert_true(g_file_set_contents(path, text, -1, NULL));
    g_assert_cmpint(g_chmod(path, 0644), ==, 0);
    return path;
}

static void
iscsi_assert_record_has_new_chap(const gchar *path)
{
    gchar *text = NULL;
    GStatBuf st;

    g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
    g_assert_nonnull(strstr(text, "node.session.auth.authmethod = CHAP\n"));
    g_assert_nonnull(strstr(text, "node.session.auth.username = new-user\n"));
    g_assert_nonnull(strstr(text, "node.session.auth.password = "
                                  ISCSI_NODE_SECRET "\n"));
    g_assert_null(strstr(text, "old-password"));
    g_assert_cmpint(g_stat(path, &st), ==, 0);
    g_assert_cmpuint(st.st_mode & 0777, ==, 0600);
    g_free(text);
}

static gboolean
iscsi_tree_has_private_temp(const gchar *path)
{
    GDir *dir = g_dir_open(path, 0, NULL);
    const gchar *name;

    if (!dir)
        return FALSE;
    while ((name = g_dir_read_name(dir)) != NULL) {
        if (g_str_has_prefix(name, ".pcv-chap-")) {
            g_dir_close(dir);
            return TRUE;
        }
        gchar *child = g_build_filename(path, name, NULL);
        GStatBuf st;
        gboolean found = FALSE;
        if (g_lstat(child, &st) == 0 && S_ISDIR(st.st_mode))
            found = iscsi_tree_has_private_temp(child);
        g_free(child);
        if (found) {
            g_dir_close(dir);
            return TRUE;
        }
    }
    g_dir_close(dir);
    return FALSE;
}

                                                               
static void
test_node_db_single_record_atomic_update(void)
{
    IscsiNodeFixture f = iscsi_node_fixture_new();
    gchar *old = iscsi_node_record_text("default", FALSE);
    gchar *record = iscsi_node_write_record(f.portal, "default", old);
    GError *e = NULL;

    g_assert_true(pcv_iscsi_node_db_set_chap_at(
        f.nodes, f.locks, 20, ISCSI_NODE_IQN, ISCSI_NODE_IP,
        "new-user", ISCSI_NODE_SECRET, NULL, &e));
    g_assert_no_error(e);
    iscsi_assert_record_has_new_chap(record);
    g_assert_false(iscsi_tree_has_private_temp(f.root));
    gchar *write_lock = g_build_filename(f.locks, "lock.write", NULL);
    g_assert_false(g_file_test(write_lock, G_FILE_TEST_EXISTS));

    g_free(write_lock);
    g_free(record);
    g_free(old);
    iscsi_node_fixture_clear(&f);
}

                                                                 
static void
test_node_db_selects_primary_runtime_root(void)
{
    IscsiNodeFixture f = iscsi_node_fixture_new();
    gchar *legacy = g_build_filename(f.root, "legacy-nodes", NULL);
    gchar *old = iscsi_node_record_text("default", FALSE);
    gchar *record = iscsi_node_write_record(f.portal, "default", old);
    GError *e = NULL;

    g_assert_cmpint(g_mkdir_with_parents(legacy, 0700), ==, 0);
    g_assert_true(pcv_iscsi_node_db_set_chap_candidates_at(
        f.nodes, legacy, f.locks, 20, ISCSI_NODE_IQN, ISCSI_NODE_IP,
        "new-user", ISCSI_NODE_SECRET, &e));
    g_assert_no_error(e);
    iscsi_assert_record_has_new_chap(record);

    g_free(record);
    g_free(old);
    g_free(legacy);
    iscsi_node_fixture_clear(&f);
}

                                                              
static void
test_node_db_selects_legacy_runtime_root(void)
{
    IscsiNodeFixture f = iscsi_node_fixture_new();
    gchar *primary = g_build_filename(f.root, "primary-nodes", NULL);
    gchar *old = iscsi_node_record_text("default", FALSE);
    gchar *record = iscsi_node_write_record(f.portal, "default", old);
    GError *e = NULL;

    g_assert_cmpint(g_mkdir_with_parents(primary, 0700), ==, 0);
    g_assert_true(pcv_iscsi_node_db_set_chap_candidates_at(
        primary, f.nodes, f.locks, 20, ISCSI_NODE_IQN, ISCSI_NODE_IP,
        "new-user", ISCSI_NODE_SECRET, &e));
    g_assert_no_error(e);
    iscsi_assert_record_has_new_chap(record);

    g_free(record);
    g_free(old);
    g_free(primary);
    iscsi_node_fixture_clear(&f);
}

                                                      
static void
test_node_db_rejects_ambiguous_runtime_roots(void)
{
    IscsiNodeFixture f = iscsi_node_fixture_new();
    gchar *legacy_nodes = g_build_filename(f.root, "legacy-nodes", NULL);
    gchar *legacy_target = g_build_filename(legacy_nodes, ISCSI_NODE_IQN, NULL);
    gchar *legacy_portal = g_build_filename(
        legacy_target, ISCSI_NODE_IP ",3260,1", NULL);
    gchar *old = iscsi_node_record_text("default", FALSE);
    gchar *primary_record = iscsi_node_write_record(f.portal, "default", old);
    gchar *legacy_record;
    gchar *primary_after = NULL;
    gchar *legacy_after = NULL;
    GError *e = NULL;

    g_assert_cmpint(g_mkdir_with_parents(legacy_portal, 0700), ==, 0);
    legacy_record = iscsi_node_write_record(legacy_portal, "default", old);
    g_assert_false(pcv_iscsi_node_db_set_chap_candidates_at(
        f.nodes, legacy_nodes, f.locks, 20, ISCSI_NODE_IQN, ISCSI_NODE_IP,
        "new-user", ISCSI_NODE_SECRET, &e));
    g_assert_error(e, G_IO_ERROR, G_IO_ERROR_EXISTS);
    g_assert_null(strstr(e->message, ISCSI_NODE_SECRET));
    g_clear_error(&e);
    g_assert_true(g_file_get_contents(primary_record, &primary_after, NULL, NULL));
    g_assert_true(g_file_get_contents(legacy_record, &legacy_after, NULL, NULL));
    g_assert_cmpstr(primary_after, ==, old);
    g_assert_cmpstr(legacy_after, ==, old);

    g_free(legacy_after);
    g_free(primary_after);
    g_free(legacy_record);
    g_free(primary_record);
    g_free(old);
    g_free(legacy_portal);
    g_free(legacy_target);
    g_free(legacy_nodes);
    iscsi_node_fixture_clear(&f);
}

                                              
static void
test_node_db_updates_all_matching_ifaces(void)
{
    IscsiNodeFixture f = iscsi_node_fixture_new();
    gchar *old_default = iscsi_node_record_text("default", FALSE);
    gchar *old_iface = iscsi_node_record_text("iface0", FALSE);
    gchar *record_default = iscsi_node_write_record(f.portal, "default", old_default);
    gchar *record_iface = iscsi_node_write_record(f.portal, "iface0", old_iface);
    GError *e = NULL;

    g_assert_true(pcv_iscsi_node_db_set_chap_at(
        f.nodes, f.locks, 20, ISCSI_NODE_IQN, ISCSI_NODE_IP,
        "new-user", ISCSI_NODE_SECRET, NULL, &e));
    g_assert_no_error(e);
    iscsi_assert_record_has_new_chap(record_default);
    iscsi_assert_record_has_new_chap(record_iface);
    g_assert_false(iscsi_tree_has_private_temp(f.root));

    g_free(record_iface);
    g_free(record_default);
    g_free(old_iface);
    g_free(old_default);
    iscsi_node_fixture_clear(&f);
}

                                                                    
static void
test_node_db_updates_old_layout(void)
{
    IscsiNodeFixture f = iscsi_node_fixture_new();
    gchar *old = iscsi_node_record_text("default", FALSE);
    gchar *old_portal = g_build_filename(f.target, ISCSI_NODE_IP ",3260", NULL);
    GError *e = NULL;

    g_assert_cmpint(g_rmdir(f.portal), ==, 0);
    g_free(f.portal);
    f.portal = old_portal;
    g_assert_true(g_file_set_contents(f.portal, old, -1, NULL));
    g_assert_cmpint(g_chmod(f.portal, 0644), ==, 0);

    g_assert_true(pcv_iscsi_node_db_set_chap_at(
        f.nodes, f.locks, 20, ISCSI_NODE_IQN, ISCSI_NODE_IP,
        "new-user", ISCSI_NODE_SECRET, NULL, &e));
    g_assert_no_error(e);
    iscsi_assert_record_has_new_chap(f.portal);

    g_free(old);
    iscsi_node_fixture_clear(&f);
}

                                                       
static void
test_node_db_updates_ipv6_unknown_tpgt(void)
{
    const gchar *ipv6 = "2001:db8::19";
    IscsiNodeFixture f = iscsi_node_fixture_new();
    gchar *old = iscsi_node_record_text_for("default", ipv6, FALSE);
    GError *e = NULL;

    g_assert_cmpint(g_rmdir(f.portal), ==, 0);
    g_free(f.portal);
    f.portal = g_build_filename(f.target, "2001:db8::19,3260,-1", NULL);
    g_assert_cmpint(g_mkdir_with_parents(f.portal, 0700), ==, 0);
    gchar *record = iscsi_node_write_record(f.portal, "default", old);

    g_assert_true(pcv_iscsi_node_db_set_chap_at(
        f.nodes, f.locks, 20, ISCSI_NODE_IQN, "[2001:db8::19]:3260",
        "new-user", ISCSI_NODE_SECRET, NULL, &e));
    g_assert_no_error(e);
    iscsi_assert_record_has_new_chap(record);

    g_free(record);
    g_free(old);
    iscsi_node_fixture_clear(&f);
}

                                                       
static void
test_node_db_rejects_malformed_requested_port(void)
{
    IscsiNodeFixture f = iscsi_node_fixture_new();
    gchar *old = iscsi_node_record_text("default", FALSE);
    gchar *record = iscsi_node_write_record(f.portal, "default", old);
    gchar *after = NULL;
    GError *e = NULL;

    g_assert_false(pcv_iscsi_node_db_set_chap_at(
        f.nodes, f.locks, 20, ISCSI_NODE_IQN, ISCSI_NODE_IP ":not-a-port",
        "new-user", ISCSI_NODE_SECRET, NULL, &e));
    g_assert_error(e, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_assert_null(strstr(e->message, ISCSI_NODE_SECRET));
    g_clear_error(&e);
    g_assert_true(g_file_get_contents(record, &after, NULL, NULL));
    g_assert_cmpstr(after, ==, old);
    g_assert_false(iscsi_tree_has_private_temp(f.root));

    g_free(after);
    g_free(record);
    g_free(old);
    iscsi_node_fixture_clear(&f);
}

static gboolean
iscsi_node_fail_second_commit(guint index, const gchar *record_path,
                              gpointer user_data, GError **error)
{
    (void)record_path;
    (void)user_data;
    if (index != 1)
        return TRUE;
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "synthetic second commit failure");
    return FALSE;
}

                                                                     
static void
test_node_db_multi_record_failure_rolls_back(void)
{
    IscsiNodeFixture f = iscsi_node_fixture_new();
    gchar *old_default = iscsi_node_record_text("default", FALSE);
    gchar *old_iface = iscsi_node_record_text("iface0", FALSE);
    gchar *record_default = iscsi_node_write_record(f.portal, "default", old_default);
    gchar *record_iface = iscsi_node_write_record(f.portal, "iface0", old_iface);
    PcvIscsiNodeDbHooks hooks = {
        .before_commit = iscsi_node_fail_second_commit,
        .user_data = NULL,
    };
    GError *e = NULL;
    gchar *after_default = NULL;
    gchar *after_iface = NULL;

    g_assert_false(pcv_iscsi_node_db_set_chap_at(
        f.nodes, f.locks, 20, ISCSI_NODE_IQN, ISCSI_NODE_IP,
        "new-user", ISCSI_NODE_SECRET, &hooks, &e));
    g_assert_error(e, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_null(strstr(e->message, ISCSI_NODE_SECRET));
    g_clear_error(&e);
    g_assert_true(g_file_get_contents(record_default, &after_default, NULL, NULL));
    g_assert_true(g_file_get_contents(record_iface, &after_iface, NULL, NULL));
    g_assert_cmpstr(after_default, ==, old_default);
    g_assert_cmpstr(after_iface, ==, old_iface);
    g_assert_false(iscsi_tree_has_private_temp(f.root));

    g_free(after_iface);
    g_free(after_default);
    g_free(record_iface);
    g_free(record_default);
    g_free(old_iface);
    g_free(old_default);
    iscsi_node_fixture_clear(&f);
}

                                                                  
static void
test_node_db_lock_contention_is_bounded(void)
{
    IscsiNodeFixture f = iscsi_node_fixture_new();
    gchar *old = iscsi_node_record_text("default", FALSE);
    gchar *record = iscsi_node_write_record(f.portal, "default", old);
    gchar *lock_path = g_build_filename(f.locks, "lock", NULL);
    gchar *write_lock = g_build_filename(f.locks, "lock.write", NULL);
    gchar *after = NULL;
    GError *e = NULL;

    g_assert_true(g_file_set_contents(lock_path, "", 0, NULL));
    g_assert_cmpint(link(lock_path, write_lock), ==, 0);
    g_assert_false(pcv_iscsi_node_db_set_chap_at(
        f.nodes, f.locks, 10, ISCSI_NODE_IQN, ISCSI_NODE_IP,
        "new-user", ISCSI_NODE_SECRET, NULL, &e));
    g_assert_error(e, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
    g_clear_error(&e);
    g_assert_true(g_file_get_contents(record, &after, NULL, NULL));
    g_assert_cmpstr(after, ==, old);

    g_free(after);
    g_free(write_lock);
    g_free(lock_path);
    g_free(record);
    g_free(old);
    iscsi_node_fixture_clear(&f);
}

                                                                        
static void
test_node_db_rejects_unsafe_records_without_secret_error(void)
{
    IscsiNodeFixture f = iscsi_node_fixture_new();
    gchar *decoy = g_build_filename(f.root, "decoy", NULL);
    gchar *link_path = g_build_filename(f.portal, "default", NULL);
    gchar *decoy_after = NULL;
    GError *e = NULL;

    g_assert_true(g_file_set_contents(decoy, "decoy-unchanged", -1, NULL));
    g_assert_cmpint(symlink(decoy, link_path), ==, 0);
    g_assert_false(pcv_iscsi_node_db_set_chap_at(
        f.nodes, f.locks, 20, ISCSI_NODE_IQN, ISCSI_NODE_IP,
        "new-user", ISCSI_NODE_SECRET, NULL, &e));
    g_assert_nonnull(e);
    g_assert_null(strstr(e->message, ISCSI_NODE_SECRET));
    g_clear_error(&e);
    g_assert_true(g_file_get_contents(decoy, &decoy_after, NULL, NULL));
    g_assert_cmpstr(decoy_after, ==, "decoy-unchanged");
    g_free(decoy_after);
    g_assert_cmpint(g_unlink(link_path), ==, 0);

    gchar *duplicate = iscsi_node_record_text("default", TRUE);
    gchar *record = iscsi_node_write_record(f.portal, "default", duplicate);
    g_assert_false(pcv_iscsi_node_db_set_chap_at(
        f.nodes, f.locks, 20, ISCSI_NODE_IQN, ISCSI_NODE_IP,
        "new-user", ISCSI_NODE_SECRET, NULL, &e));
    g_assert_nonnull(e);
    g_assert_null(strstr(e->message, ISCSI_NODE_SECRET));
    g_clear_error(&e);
    g_assert_false(iscsi_tree_has_private_temp(f.root));

    g_free(record);
    g_free(duplicate);
    g_free(link_path);
    g_free(decoy);
    iscsi_node_fixture_clear(&f);
}

                                                                 
static void
test_node_db_no_match_fails_closed(void)
{
    IscsiNodeFixture f = iscsi_node_fixture_new();
    GError *e = NULL;

    g_assert_false(pcv_iscsi_node_db_set_chap_at(
        f.nodes, f.locks, 20, "iqn.2026-03.purecvisor:not-found", ISCSI_NODE_IP,
        "new-user", ISCSI_NODE_SECRET, NULL, &e));
    g_assert_error(e, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_assert_null(strstr(e->message, ISCSI_NODE_SECRET));
    g_clear_error(&e);
    g_assert_false(iscsi_tree_has_private_temp(f.root));
    iscsi_node_fixture_clear(&f);
}

void
test_iscsi_register(void)
{
    g_test_add_func("/iscsi/list_shape", test_list_shape_without_init);
    g_test_add_func("/iscsi/create_without_init_fails", test_create_without_init_fails);
    g_test_add_func("/iscsi/iqn_format", test_iqn_format);
    g_test_add_func("/iscsi/node_db/runtime_root_constants",
                    test_node_db_runtime_root_constants);
    g_test_add_func("/iscsi/chap_absent_is_ok", test_chap_absent_is_ok);
    g_test_add_func("/iscsi/chap_half_config_fails_closed",
                    test_chap_half_config_fails_closed);
    g_test_add_func("/iscsi/chap_kernel_traps_rejected", test_chap_kernel_traps_rejected);
    g_test_add_func("/iscsi/chap_error_never_carries_secret",
                    test_chap_error_never_carries_secret);
                                                     
                                                     
                                             
    g_test_add_func("/iscsi/delete_reaps_orphan_without_record",
                    test_delete_reaps_orphan_without_record);
    g_test_add_func("/iscsi/create_delete_roundtrip", test_create_delete_roundtrip);
    g_test_add_func("/iscsi/create_refuses_half_chap_config",
                    test_create_refuses_half_chap_config);
    g_test_add_func("/iscsi/node_db/single_record_atomic_update",
                    test_node_db_single_record_atomic_update);
    g_test_add_func("/iscsi/node_db/selects_primary_runtime_root",
                    test_node_db_selects_primary_runtime_root);
    g_test_add_func("/iscsi/node_db/selects_legacy_runtime_root",
                    test_node_db_selects_legacy_runtime_root);
    g_test_add_func("/iscsi/node_db/rejects_ambiguous_runtime_roots",
                    test_node_db_rejects_ambiguous_runtime_roots);
    g_test_add_func("/iscsi/node_db/updates_all_matching_ifaces",
                    test_node_db_updates_all_matching_ifaces);
    g_test_add_func("/iscsi/node_db/updates_old_layout",
                    test_node_db_updates_old_layout);
    g_test_add_func("/iscsi/node_db/updates_ipv6_unknown_tpgt",
                    test_node_db_updates_ipv6_unknown_tpgt);
    g_test_add_func("/iscsi/node_db/rejects_malformed_requested_port",
                    test_node_db_rejects_malformed_requested_port);
    g_test_add_func("/iscsi/node_db/multi_record_failure_rolls_back",
                    test_node_db_multi_record_failure_rolls_back);
    g_test_add_func("/iscsi/node_db/lock_contention_is_bounded",
                    test_node_db_lock_contention_is_bounded);
    g_test_add_func("/iscsi/node_db/rejects_unsafe_records_without_secret_error",
                    test_node_db_rejects_unsafe_records_without_secret_error);
    g_test_add_func("/iscsi/node_db/no_match_fails_closed",
                    test_node_db_no_match_fails_closed);
}
