   
                           
                                                                                   
  
                           
                                                                                                   
                                                                                     
                                                                                           
                                                                                 
                                                                                     
                                                                             
                                       
  
                                                                          
                                                                      
   
#include <glib.h>
#include <glib/gstdio.h>

#include "modules/auth/pcv_rbac.h"

typedef struct {
    gchar *dir;
    gchar *db;
} RbacFx;

                                                                                
static void
rbac_fx_open(RbacFx *f)
{
    f->dir = g_dir_make_tmp("pcv-rbac-tenant-XXXXXX", NULL);
    g_assert_nonnull(f->dir);
    f->db = g_build_filename(f->dir, "rbac.db", NULL);
    pcv_rbac_init(f->db);

    static const struct { const gchar *user; const gchar *tenant; } users[] = {
        { "u1", NULL }, { "u2", "ta" }, { "u3", "" }, { "u4", NULL },
    };
    for (guint i = 0; i < G_N_ELEMENTS(users); i++) {
        g_autoptr(GError) error = NULL;
        g_assert_true(pcv_rbac_user_create(users[i].user, "pw", PCV_ROLE_OPERATOR, users[i].tenant, &error));
        g_assert_no_error(error);
    }
}

                                          
static void
rbac_fx_close(RbacFx *f)
{
    pcv_rbac_shutdown();
    g_autofree gchar *wal = g_strconcat(f->db, "-wal", NULL);
    g_autofree gchar *shm = g_strconcat(f->db, "-shm", NULL);
    g_remove(wal);
    g_remove(shm);
    g_remove(f->db);
    g_rmdir(f->dir);
    g_clear_pointer(&f->db, g_free);
    g_clear_pointer(&f->dir, g_free);
}

                                                                                         
static void
test_set_only_when_empty(void)
{
    RbacFx f = { 0 };
    rbac_fx_open(&f);

    g_assert_cmpint(pcv_rbac_user_set_tenant_if_empty("u1", "tb"), ==, PCV_RBAC_TENANT_SET);
    g_assert_cmpstr(pcv_rbac_get_tenant("u1"), ==, "tb");

    g_assert_cmpint(pcv_rbac_user_set_tenant_if_empty("u2", "tb"), ==, PCV_RBAC_TENANT_ALREADY);
    g_assert_cmpstr(pcv_rbac_get_tenant("u2"), ==, "ta");

    g_assert_cmpint(pcv_rbac_user_set_tenant_if_empty("u3", "tb"), ==, PCV_RBAC_TENANT_SET);                      
    g_assert_cmpstr(pcv_rbac_get_tenant("u3"), ==, "tb");

                                                       
    g_assert_cmpint(pcv_rbac_user_set_tenant_if_empty("u1", "tc"), ==, PCV_RBAC_TENANT_ALREADY);
    g_assert_cmpint(pcv_rbac_user_set_tenant_if_empty("u1", "tb"), ==, PCV_RBAC_TENANT_ALREADY);
    g_assert_cmpstr(pcv_rbac_get_tenant("u1"), ==, "tb");

    g_assert_cmpint(pcv_rbac_user_set_tenant_if_empty("ghost", "tb"), ==, PCV_RBAC_TENANT_NO_USER);
    g_assert_null(pcv_rbac_get_tenant("ghost"));

    g_assert_cmpint(pcv_rbac_user_set_tenant_if_empty(NULL, "tb"), ==, PCV_RBAC_TENANT_ERROR);
    g_assert_cmpint(pcv_rbac_user_set_tenant_if_empty("u4", NULL), ==, PCV_RBAC_TENANT_ERROR);
    g_assert_cmpint(pcv_rbac_user_set_tenant_if_empty("u4", ""), ==, PCV_RBAC_TENANT_ERROR);
    g_assert_null(pcv_rbac_get_tenant("u4"));                             

    rbac_fx_close(&f);
                                             
    g_assert_cmpint(pcv_rbac_user_set_tenant_if_empty("u4", "tb"), ==, PCV_RBAC_TENANT_ERROR);
}

                                                 
static gpointer
get_tenant_thread(gpointer data)
{
    (void)data;
    const gchar *t = pcv_rbac_get_tenant("u2");
    g_assert_cmpstr(t, ==, "ta");
    return (gpointer)t;
}

                                                                                      
static void
test_get_tenant_unchanged_after_move(void)
{
    RbacFx f = { 0 };
    rbac_fx_open(&f);

    g_assert_null(pcv_rbac_get_tenant(NULL));
    g_assert_null(pcv_rbac_get_tenant(""));
    g_assert_null(pcv_rbac_get_tenant("ghost"));
    g_assert_null(pcv_rbac_get_tenant("u1"));                        
    g_assert_cmpstr(pcv_rbac_get_tenant("u3"), ==, "");                          

    const gchar *first = pcv_rbac_get_tenant("u2");
    g_assert_cmpstr(first, ==, "ta");
    g_assert_true(pcv_rbac_user_set_tenant_if_empty("u1", "tq") == PCV_RBAC_TENANT_SET);
    const gchar *second = pcv_rbac_get_tenant("u1");
    g_assert_true(first == second);                                             
    g_assert_cmpstr(first, ==, "tq");

    GThread *th = g_thread_new("rbac-tenant-tls", get_tenant_thread, NULL);
    gpointer other = g_thread_join(th);
    g_assert_true(other != (gpointer)first);                              
    g_assert_cmpstr(first, ==, "tq");                                                

    rbac_fx_close(&f);
    g_assert_null(pcv_rbac_get_tenant("u2"));                             
}

void test_rbac_tenant_register(void);

                                               
void
test_rbac_tenant_register(void)
{
    g_test_add_func("/rbac_tenant/set_only_when_empty", test_set_only_when_empty);
    g_test_add_func("/rbac_tenant/get_tenant_unchanged_after_move", test_get_tenant_unchanged_after_move);
}
