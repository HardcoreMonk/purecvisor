   
                                 
                                                                                     
  
                           
                                                                                              
                                                                                      
                                                                                         
                                                                                   
                                                                                                        
                                                                                    
                                                                                                                
                                     
                                                                                 
                                                                                               
                                                    
                                             
  
                                                                                
                                                                       
   
#include <glib.h>
#include <glib/gstdio.h>
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "modules/resource/resource_registry.h"
#include "utils/pcv_config.h"

typedef struct {
    gchar *tmp;                          
    gchar *db;
} Fx;

                                                                                      
static Fx *
fx_new(void)
{
    Fx *f = g_new0(Fx, 1);
    gchar *raw = g_dir_make_tmp("pcv-resreg-XXXXXX", NULL);
    g_assert_nonnull(raw);
    f->tmp = realpath(raw, NULL);
    g_free(raw);
    g_assert_nonnull(f->tmp);
    const gchar *dirs[] = { "iso", "img", "disks", "other", "def" };
    for (guint i = 0; i < G_N_ELEMENTS(dirs); i++) {
        gchar *d = g_build_filename(f->tmp, dirs[i], NULL);
        g_assert_cmpint(g_mkdir_with_parents(d, 0755), ==, 0);
        g_free(d);
    }
    const gchar *files[][2] = { { "iso/a.iso", "A" }, { "iso/b.iso", "B" }, { "img/base.qcow2", "Q" } };
    for (guint i = 0; i < G_N_ELEMENTS(files); i++) {
        gchar *p = g_build_filename(f->tmp, files[i][0], NULL);
        g_assert_true(g_file_set_contents(p, files[i][1], -1, NULL));
        g_free(p);
    }
    f->db = g_build_filename(f->tmp, "reg.db", NULL);
    GError *err = NULL;
    g_assert_true(pcv_res_registry_open(f->db, &err));
    g_assert_no_error(err);
    return f;
}

static gchar *
fx_path(const Fx *f, const gchar *rel)
{
    return g_build_filename(f->tmp, rel, NULL);
}

                                                    
static void
fx_free(Fx *f)
{
    pcv_res_registry_close();
    gchar *argv[] = { "rm", "-rf", f->tmp, NULL };
    g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
    g_free(f->tmp);
    g_free(f->db);
    g_free(f);
}

                                                                           
static gboolean
allows(const Fx *f, PcvResKind kind, const gchar *value, const gchar *tenant)
{
    (void)f;
    PcvResNorm n = {0};
    gboolean r = pcv_res_normalize(kind, value, &n, NULL) && pcv_res_tenant_allows(kind, &n, tenant);
    pcv_res_norm_clear(&n);
    return r;
}

static void
test_normalize_kinds(void)
{
    Fx *f = fx_new();
    PcvResNorm n = {0};
    gchar *p, *want;

    g_assert_true(pcv_res_normalize(PCV_RES_PCI, "3B:00.1", &n, NULL));
    g_assert_cmpstr(n.ref, ==, "0000:3b:00.1");
    pcv_res_norm_clear(&n);
    g_assert_true(pcv_res_normalize(PCV_RES_USB, "0x1D6B:0x0002", &n, NULL));
    g_assert_cmpstr(n.ref, ==, "1d6b:0002");
    pcv_res_norm_clear(&n);
    g_assert_false(pcv_res_normalize(PCV_RES_USB, "1d6b:2", &n, NULL));
    g_assert_null(n.ref);

    p = fx_path(f, "iso/../iso/a.iso");
    want = fx_path(f, "iso/a.iso");
    g_assert_true(pcv_res_normalize(PCV_RES_ISO, p, &n, NULL));
    g_assert_cmpstr(n.ref, ==, want);
    g_assert_true(n.has_identity);
    pcv_res_norm_clear(&n);
    g_free(p);
    g_free(want);

    p = fx_path(f, "iso");
    g_assert_false(pcv_res_normalize(PCV_RES_ISO, p, &n, NULL));                      
    g_free(p);
    p = fx_path(f, "img/base.qcow2");
    g_assert_false(pcv_res_normalize(PCV_RES_ISO, p, &n, NULL));                     
    g_assert_true(pcv_res_normalize(PCV_RES_IMAGE, p, &n, NULL));
    pcv_res_norm_clear(&n);
    g_free(p);

    g_assert_true(pcv_res_normalize(PCV_RES_STORAGE, "tank/vms", &n, NULL));
    g_assert_cmpstr(n.ref, ==, "tank/vms");
    pcv_res_norm_clear(&n);
    g_assert_false(pcv_res_normalize(PCV_RES_STORAGE, "tank//vms", &n, NULL));

    p = fx_path(f, "disks/new/sub");
    g_assert_true(pcv_res_normalize(PCV_RES_STORAGE, p, &n, NULL));                    
    g_assert_cmpstr(n.ref, ==, p);
    g_assert_false(n.has_identity);
    pcv_res_norm_clear(&n);
    g_free(p);

    g_assert_false(pcv_res_normalize(PCV_RES_BRIDGE, "br0;x", &n, NULL));
    fx_free(f);
}

static void
test_register_refresh_and_conflicts(void)
{
    Fx *f = fx_new();
    GError *err = NULL;
    gboolean refreshed = TRUE;
    gchar *a = fx_path(f, "iso/a.iso"), *b = fx_path(f, "iso/b.iso");

    g_assert_true(pcv_res_register(PCV_RES_ISO, "iso-a", a, "admin", &refreshed, &err));
    g_assert_no_error(err);
    g_assert_false(refreshed);
    g_assert_true(pcv_res_register(PCV_RES_ISO, "iso-a", a, "admin", &refreshed, &err));                 
    g_assert_true(refreshed);

    g_assert_false(pcv_res_register(PCV_RES_ISO, "iso-a", b, "admin", &refreshed, &err));                  
    g_assert_error(err, PCV_RES_ERROR, PCV_RES_ERROR_CONFLICT);
    g_clear_error(&err);
    g_assert_false(pcv_res_register(PCV_RES_ISO, "iso-x", a, "admin", &refreshed, &err));                       
    g_assert_error(err, PCV_RES_ERROR, PCV_RES_ERROR_CONFLICT);
    g_clear_error(&err);
    g_assert_false(pcv_res_register(PCV_RES_ISO, "bad name!", b, "admin", &refreshed, &err));
    g_assert_error(err, PCV_RES_ERROR, PCV_RES_ERROR_INVALID);
    g_clear_error(&err);

    PcvResKind k;
    gchar *ref = NULL;
    g_assert_true(pcv_res_lookup("iso-a", &k, &ref, &err));
    g_assert_cmpint(k, ==, PCV_RES_ISO);
    g_assert_cmpstr(ref, ==, a);
    g_free(ref);
    g_assert_false(pcv_res_lookup("nope", &k, &ref, &err));
    g_assert_error(err, PCV_RES_ERROR, PCV_RES_ERROR_NOT_FOUND);
    g_clear_error(&err);

                                                                    
    g_assert_true(pcv_res_grant("iso-a", "ta", &err));
    g_assert_true(pcv_res_grant("iso-a", "ta", &err));                                              
    JsonArray *l = pcv_res_list(TRUE, PCV_RES_ISO, &err);
    g_assert_nonnull(l);
    g_assert_cmpuint(json_array_get_length(l), ==, 1);
    JsonObject *o = json_array_get_object_element(l, 0);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(o, "tenants")), ==, 1);
    json_array_unref(l);
    g_assert_false(pcv_res_revoke("iso-a", "tb", &err));
    g_assert_error(err, PCV_RES_ERROR, PCV_RES_ERROR_NOT_FOUND);
    g_clear_error(&err);
    g_assert_true(pcv_res_revoke("iso-a", "ta", &err));
    g_assert_true(pcv_res_unregister("iso-a", &err));
    g_assert_false(pcv_res_unregister("iso-a", &err));
    g_assert_error(err, PCV_RES_ERROR, PCV_RES_ERROR_NOT_FOUND);
    g_clear_error(&err);
    l = pcv_res_list(FALSE, PCV_RES_ISO, &err);
    g_assert_cmpuint(json_array_get_length(l), ==, 0);
    json_array_unref(l);

    g_free(a);
    g_free(b);
    fx_free(f);
}

static void
test_identity_rejects_hardlink_and_replacement(void)
{
    Fx *f = fx_new();
    GError *err = NULL;
    gboolean refreshed = FALSE;
    gchar *a = fx_path(f, "iso/a.iso"), *oa = fx_path(f, "other/a.iso"), *ol = fx_path(f, "other/l.iso");

    g_assert_true(pcv_res_register(PCV_RES_ISO, "iso-a", a, "admin", &refreshed, &err));
    g_assert_true(pcv_res_grant("iso-a", "ta", &err));
    g_assert_true(allows(f, PCV_RES_ISO, a, "ta"));
    g_assert_false(allows(f, PCV_RES_ISO, a, "tb"));

    g_assert_cmpint(link(a, oa), ==, 0);                                                  
    g_assert_false(allows(f, PCV_RES_ISO, oa, "ta"));
    g_assert_cmpint(symlink(a, ol), ==, 0);                                           
    g_assert_true(allows(f, PCV_RES_ISO, ol, "ta"));

    g_assert_cmpint(g_unlink(a), ==, 0);                                            
                                                                                   
    g_assert_true(g_file_set_contents(a, "A2", -1, NULL));
    g_assert_false(allows(f, PCV_RES_ISO, a, "ta"));

    g_assert_true(pcv_res_register(PCV_RES_ISO, "iso-a", a, "admin", &refreshed, &err));              
    g_assert_true(refreshed);
    g_assert_true(allows(f, PCV_RES_ISO, a, "ta"));

                                                                             
    g_assert_cmpint(g_unlink(a), ==, 0);
    g_assert_cmpint(g_unlink(oa), ==, 0);
    g_usleep(5000);
    g_assert_true(g_file_set_contents(a, "A3", -1, NULL));
    g_assert_false(allows(f, PCV_RES_ISO, a, "ta"));

    g_free(a);
    g_free(oa);
    g_free(ol);
    fx_free(f);
}

static void
test_storage_dir_containment_through_symlink(void)
{
    Fx *f = fx_new();
    GError *err = NULL;
    gboolean refreshed;
    gchar *disks = fx_path(f, "disks"), *deep = fx_path(f, "disks/x/y"), *sib = fx_path(f, "disksx");
    gchar *other = fx_path(f, "other"), *esc = fx_path(f, "disks/esc"), *escv = fx_path(f, "disks/esc/v");

    g_assert_true(pcv_res_register(PCV_RES_STORAGE, "st", disks, "admin", &refreshed, &err));
    g_assert_true(pcv_res_grant("st", "ta", &err));
    gchar *nope = fx_path(f, "nope");                                                            
    g_assert_false(pcv_res_register(PCV_RES_STORAGE, "st2", nope, "admin", &refreshed, &err));
    g_assert_error(err, PCV_RES_ERROR, PCV_RES_ERROR_NOT_FOUND);
    g_clear_error(&err);
    g_free(nope);
    g_assert_true(allows(f, PCV_RES_STORAGE, deep, "ta"));
    g_assert_false(allows(f, PCV_RES_STORAGE, sib, "ta"));
    g_assert_cmpint(symlink(other, esc), ==, 0);
    g_assert_false(allows(f, PCV_RES_STORAGE, escv, "ta"));                                  

    g_assert_true(pcv_res_register(PCV_RES_STORAGE, "ds", "tank/vms", "admin", &refreshed, &err));
    g_assert_true(pcv_res_grant("ds", "ta", &err));
    g_assert_true(allows(f, PCV_RES_STORAGE, "tank/vms/a", "ta"));
    g_assert_false(allows(f, PCV_RES_STORAGE, "tank/vmsx", "ta"));
    g_assert_false(allows(f, PCV_RES_STORAGE, "tank", "ta"));

    g_free(disks); g_free(deep); g_free(sib); g_free(other); g_free(esc); g_free(escv);
    fx_free(f);
}

static void
test_defaults_allowed_for_every_tenant(void)
{
    Fx *f = fx_new();
    gchar *cfg = fx_path(f, "daemon.conf");
    gchar *def = fx_path(f, "def");
    gchar *body = g_strdup_printf("[storage]\nimage_dir=%s\nzvol_pool=defpool/vms\n[network]\ndefault_bridge=pcvnat0\n", def);
    g_assert_true(g_file_set_contents(cfg, body, -1, NULL));
    g_setenv("PCV_CONFIG_PATH", cfg, TRUE);
    pcv_config_init();

    gchar *sub = fx_path(f, "def/sub"), *a = fx_path(f, "iso/a.iso");
    g_assert_true(allows(f, PCV_RES_STORAGE, sub, "tz"));
    g_assert_true(allows(f, PCV_RES_STORAGE, "defpool/vms", "tz"));
    g_assert_true(allows(f, PCV_RES_BRIDGE, "pcvnat0", "tz"));
    g_assert_true(allows(f, PCV_RES_BRIDGE, "virbr0", "tz"));
    g_assert_false(allows(f, PCV_RES_BRIDGE, "br-mgmt", "tz"));
    g_assert_false(allows(f, PCV_RES_ISO, a, NULL));                                   
    g_assert_false(allows(f, PCV_RES_STORAGE, sub, NULL));
    g_assert_false(allows(f, PCV_RES_ISO, a, "tz"));                                     
    g_assert_true(pcv_res_covered(PCV_RES_BRIDGE, "pcvnat0"));
    g_assert_false(pcv_res_covered(PCV_RES_BRIDGE, "br-mgmt"));

    pcv_config_shutdown();
    g_setenv("PCV_CONFIG_PATH", "/nonexistent/pcv-test-isolated.conf", TRUE);
    g_free(cfg); g_free(def); g_free(body); g_free(sub); g_free(a);
    fx_free(f);
}

static void
test_identity_survives_same_file(void)
{
    Fx *f = fx_new();
    gchar *a = fx_path(f, "iso/a.iso");
    PcvResNorm n1 = {0}, n2 = {0};
    g_assert_true(pcv_res_normalize(PCV_RES_ISO, a, &n1, NULL));
    g_assert_true(pcv_res_normalize(PCV_RES_ISO, a, &n2, NULL));
    g_assert_cmpuint(n1.dev, ==, n2.dev);
    g_assert_cmpuint(n1.ino, ==, n2.ino);
    g_assert_cmpuint(n1.gen, ==, n2.gen);
    g_assert_cmpuint(n1.dev, !=, 0);

                                                                       
    GError *err = NULL;
    g_assert_true(pcv_res_register(PCV_RES_ISO, "iso-s", a, "admin", NULL, &err));
    g_assert_true(pcv_res_grant("iso-s", "ta", &err));
    g_assert_true(pcv_res_tenant_allows(PCV_RES_ISO, &n1, "ta"));
    n1.dev += 1;
    g_assert_false(pcv_res_tenant_allows(PCV_RES_ISO, &n1, "ta"));
    n1.dev -= 1;
    if (n1.gen == 0) {
        g_test_skip("test tmp filesystem reports no birth time (statx btime)");
    } else {
        n1.gen += 1;
        g_assert_false(pcv_res_tenant_allows(PCV_RES_ISO, &n1, "ta"));
        n1.gen -= 1;
        g_assert_true(pcv_res_tenant_allows(PCV_RES_ISO, &n1, "ta"));
    }
    pcv_res_norm_clear(&n1);
    pcv_res_norm_clear(&n2);
    g_free(a);
    fx_free(f);
}

static void
test_db_file_mode_0600(void)
{
    Fx *f = fx_new();
    GStatBuf st;
    g_assert_cmpint(g_stat(f->db, &st), ==, 0);
    g_assert_cmpuint(st.st_mode & 07777, ==, 0600);
    pcv_res_registry_close();                                                                     
    gchar *pre = fx_path(f, "pre.db");
    g_assert_true(g_file_set_contents(pre, "", 0, NULL));
    g_assert_cmpint(g_chmod(pre, 0644), ==, 0);
    GError *err = NULL;
    g_assert_true(pcv_res_registry_open(pre, &err));
    g_assert_no_error(err);
    g_assert_cmpint(g_stat(pre, &st), ==, 0);
    g_assert_cmpuint(st.st_mode & 07777, ==, 0600);
    g_free(pre);
    fx_free(f);
}

static void
import_item_free(PcvResImportItem *it)
{
    g_free(it->name);
    g_free(it->ref);
    g_ptr_array_unref(it->tenants);
}

static PcvResImportItem
import_item(PcvResKind kind, const gchar *name, const gchar *ref, const gchar *tenant)
{
    PcvResImportItem it = { .kind = kind, .name = g_strdup(name), .ref = g_strdup(ref),
                            .tenants = g_ptr_array_new_with_free_func(g_free) };
    g_ptr_array_add(it.tenants, g_strdup(tenant));
    return it;
}

static void
test_import_all_or_nothing(void)
{
    Fx *f = fx_new();
    GError *err = NULL;
    guint reg = 99, grants = 99;
    gchar *a = fx_path(f, "iso/a.iso"), *b = fx_path(f, "iso/b.iso");
    PcvResImportItem it0 = import_item(PCV_RES_ISO, "iso-b", b, "ta");
    PcvResImportItem it1 = import_item(PCV_RES_ISO, "bad name!", a, "ta");
    GPtrArray *items = g_ptr_array_new();
    g_ptr_array_add(items, &it0);
    g_ptr_array_add(items, &it1);

    g_assert_false(pcv_res_import(items, "admin", &reg, &grants, &err));
    g_assert_error(err, PCV_RES_ERROR, PCV_RES_ERROR_INVALID);
    g_assert_nonnull(strstr(err->message, "item 2"));
    g_clear_error(&err);
    g_assert_false(pcv_res_lookup("iso-b", NULL, NULL, &err));
    g_assert_error(err, PCV_RES_ERROR, PCV_RES_ERROR_NOT_FOUND);
    g_clear_error(&err);

    g_free(it1.name);
    it1.name = g_strdup("iso-a");                                                
    g_assert_true(pcv_res_import(items, "admin", &reg, &grants, &err));
    g_assert_no_error(err);
    g_assert_cmpuint(reg, ==, 2);
    g_assert_cmpuint(grants, ==, 2);
    g_assert_true(allows(f, PCV_RES_ISO, b, "ta"));

    g_assert_true(pcv_res_import(items, "admin", &reg, &grants, &err));           
    g_assert_cmpuint(reg, ==, 0);
    g_assert_cmpuint(grants, ==, 0);

    import_item_free(&it0);
    import_item_free(&it1);
    g_ptr_array_unref(items);
    g_free(a);
    g_free(b);
    fx_free(f);
}

                                                                                        
                                                                            
                                                        
                                                                     
static void
test_import_error_item_index(void)
{
    Fx *f = fx_new();
    GError *err = NULL;
    gchar *a = fx_path(f, "iso/a.iso");
    PcvResImportItem it0 = import_item(PCV_RES_ISO, "iso-a", a, "ta");
    PcvResImportItem it1 = import_item(PCV_RES_ISO, "bad name!", a, "ta");
    it0.index = 2;                                                                                                 
    it1.index = 4;
    GPtrArray *items = g_ptr_array_new();
    g_ptr_array_add(items, &it0);
    g_ptr_array_add(items, &it1);
    g_assert_false(pcv_res_import(items, "admin", NULL, NULL, &err));
    g_assert_error(err, PCV_RES_ERROR, PCV_RES_ERROR_INVALID);
    g_assert_true(g_str_has_prefix(err->message, "item 4: "));
    g_clear_error(&err);
    it1.index = 0;                                                                           
    g_assert_false(pcv_res_import(items, "admin", NULL, NULL, &err));
    g_assert_true(g_str_has_prefix(err->message, "item 2: "));
    g_clear_error(&err);
    import_item_free(&it0);
    import_item_free(&it1);
    g_ptr_array_unref(items);
    g_free(a);
    fx_free(f);
}

                                                                                        
                                                                              
static void
test_revoke_in_use_by_tenant_owner(void)
{
    g_assert_true(pcv_res_vm_counts_for_revoke("ta", "ta"));
    g_assert_false(pcv_res_vm_counts_for_revoke("tb", "ta"));
    g_assert_false(pcv_res_vm_counts_for_revoke(NULL, "ta"));
    g_assert_false(pcv_res_vm_counts_for_revoke("", ""));
    g_assert_false(pcv_res_vm_counts_for_revoke("ta", NULL));
}

                                                                                                 
                                                                                    
                                                                                
                                                                
static void
test_pin_matches(void)
{
    Fx *f = fx_new();
    gchar *img = fx_path(f, "img/base.qcow2"), *keep = fx_path(f, "other/keep.qcow2"), *dir = fx_path(f, "disks");
    PcvResNorm n = {0};
    g_assert_true(pcv_res_normalize(PCV_RES_IMAGE, img, &n, NULL));
    const PcvResPin none = { 0, 0, 0 }, pin = { n.dev, n.ino, n.gen };
    pcv_res_norm_clear(&n);
    g_assert_true(pcv_res_pin_matches(img, NULL));
    g_assert_true(pcv_res_pin_matches(img, &none));
    g_assert_true(pcv_res_pin_matches(img, &pin));

    g_assert_cmpint(link(img, keep), ==, 0);                                                  
    g_assert_cmpint(g_unlink(img), ==, 0);
    g_assert_true(g_file_set_contents(img, "Q2", -1, NULL));
    g_assert_false(pcv_res_pin_matches(img, &pin));
    g_assert_true(pcv_res_pin_matches(img, &none));                             

    g_assert_cmpint(g_unlink(img), ==, 0);                                                         
    g_assert_cmpint(symlink(keep, img), ==, 0);
    g_assert_true(pcv_res_pin_matches(keep, &pin));
    g_assert_false(pcv_res_pin_matches(img, &pin));

    PcvResNorm kn = {0};                                                                          
    g_assert_true(pcv_res_normalize(PCV_RES_IMAGE, keep, &kn, NULL));
    const PcvResPin kdev = { kn.dev ^ 1, kn.ino, kn.gen }, kgen = { kn.dev, kn.ino, kn.gen + 1 };
    g_assert_false(pcv_res_pin_matches(keep, &kdev));
    if (kn.gen == 0)
        g_test_skip("test tmp filesystem reports no birth time (statx btime)");
    else
        g_assert_false(pcv_res_pin_matches(keep, &kgen));
    pcv_res_norm_clear(&kn);

    g_assert_true(pcv_res_normalize(PCV_RES_STORAGE, dir, &n, NULL));                               
    g_assert_true(n.has_identity);
    const PcvResPin dpin = { n.dev, n.ino, n.gen };
    pcv_res_norm_clear(&n);
    g_assert_false(pcv_res_pin_matches(dir, &dpin));

    g_assert_cmpint(g_unlink(img), ==, 0);                                                        
    g_assert_cmpint(mkfifo(img, 0600), ==, 0);
    g_assert_false(pcv_res_pin_matches(img, &pin));

    g_free(img);
    g_free(keep);
    g_free(dir);
    fx_free(f);
}

                                                                               
static void
test_unregistered_suggest_name(void)
{
    GHashTable *taken = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    gchar *s = pcv_res_unregistered_suggest_name(PCV_RES_ISO, "/isos/Ubuntu 24.04.iso", taken);
    g_assert_cmpstr(s, ==, "iso-ubuntu-24.04");
    g_free(s);
    s = pcv_res_unregistered_suggest_name(PCV_RES_ISO, "/isos/Ubuntu 24.04.iso", taken);                 
    g_assert_cmpstr(s, ==, "iso-ubuntu-24.04-2");
    g_free(s);
    g_assert_true(g_hash_table_contains(taken, "iso-ubuntu-24.04"));
    g_assert_true(g_hash_table_contains(taken, "iso-ubuntu-24.04-2"));
    s = pcv_res_unregistered_suggest_name(PCV_RES_PCI, "0000:3b:00.1", taken);
    g_assert_cmpstr(s, ==, "pci-0000-3b-00.1");
    g_free(s);

    gchar *longbase = g_strnfill(100, 'A');
    gchar *longref = g_strdup_printf("/isos/%s.iso", longbase);
    gchar *first = pcv_res_unregistered_suggest_name(PCV_RES_ISO, longref, taken);
    gchar *second = pcv_res_unregistered_suggest_name(PCV_RES_ISO, longref, taken);
    g_assert_cmpuint(strlen(first), <=, 63);
    g_assert_true(pcv_res_name_is_valid(first));
    g_assert_cmpuint(strlen(second), <=, 63);
    g_assert_true(pcv_res_name_is_valid(second));
    g_assert_cmpstr(first, !=, second);
    g_assert_true(g_str_has_suffix(second, "-2"));
    g_free(first);
    g_free(second);
    g_free(longref);
    g_free(longbase);
    g_hash_table_unref(taken);
}

                                                                        
static gint
db_user_version(const gchar *db)
{
    sqlite3 *s = NULL;
    sqlite3_stmt *st = NULL;
    gint ver = -1;
    g_assert_cmpint(sqlite3_open_v2(db, &s, SQLITE_OPEN_READONLY, NULL), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_prepare_v2(s, "PRAGMA user_version", -1, &st, NULL), ==, SQLITE_OK);
    if (sqlite3_step(st) == SQLITE_ROW)
        ver = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    sqlite3_close(s);
    return ver;
}

                                                
static void
assert_pool(GPtrArray *got, const gchar *const *want)
{
    g_assert_nonnull(got);
    guint n = 0;
    while (want[n])
        n++;
    g_assert_cmpuint(got->len, ==, n);
    for (guint i = 0; i < n; i++)
        g_assert_cmpstr(g_ptr_array_index(got, i), ==, want[i]);
    g_ptr_array_unref(got);
}

                                                                                                        
                                                                                         
                                                                                
                                                         
                                                                            
                                                                              
static void
test_vpc_pool_roundtrip(void)
{
    gchar *raw = g_dir_make_tmp("pcv-respool-XXXXXX", NULL);
    g_assert_nonnull(raw);
    gchar *db = g_build_filename(raw, "v1.db", NULL);
    sqlite3 *s = NULL;                                                                               
    g_assert_cmpint(sqlite3_open(db, &s), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_exec(s,
        "CREATE TABLE resources (name TEXT PRIMARY KEY, kind TEXT NOT NULL, ref TEXT NOT NULL, dev INTEGER, ino INTEGER,"
        " gen INTEGER, created_by TEXT, created_at INTEGER NOT NULL, UNIQUE(kind, ref));"
        "CREATE TABLE resource_grants (name TEXT NOT NULL REFERENCES resources(name) ON DELETE CASCADE,"
        " tenant TEXT NOT NULL, PRIMARY KEY(name, tenant));"
        "INSERT INTO resources(name, kind, ref, created_by, created_at) VALUES('br-a', 'bridge', 'br-a', 'admin', 1);"
        "INSERT INTO resource_grants(name, tenant) VALUES('br-a', 'ta');"
        "PRAGMA user_version=1;", NULL, NULL, NULL), ==, SQLITE_OK);
    sqlite3_close(s);

    GError *err = NULL;
    g_assert_true(pcv_res_registry_open(db, &err));
    g_assert_no_error(err);
    g_assert_cmpint(db_user_version(db), ==, 2);
    PcvResKind kind = PCV_RES_IMAGE;
    gchar *ref = NULL;
    g_assert_true(pcv_res_lookup("br-a", &kind, &ref, &err));                                
    g_assert_cmpint(kind, ==, PCV_RES_BRIDGE);
    g_assert_cmpstr(ref, ==, "br-a");
    g_free(ref);
    JsonArray *rows = pcv_res_list(FALSE, PCV_RES_IMAGE, &err);
    g_assert_nonnull(rows);
    g_assert_cmpuint(json_array_get_length(rows), ==, 1);
    JsonArray *tenants = json_object_get_array_member(json_array_get_object_element(rows, 0), "tenants");
    g_assert_cmpuint(json_array_get_length(tenants), ==, 1);
    g_assert_cmpstr(json_array_get_string_element(tenants, 0), ==, "ta");
    json_array_unref(rows);

    GPtrArray *in = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(in, g_strdup("10.20.0.0/24"));
    g_ptr_array_add(in, g_strdup("10.10.0.0/16"));
    g_assert_true(pcv_res_pool_set("ta", in, &err));
    g_assert_no_error(err);
    assert_pool(pcv_res_pool_list("ta"), (const gchar *const[]){ "10.10.0.0/16", "10.20.0.0/24", NULL });
    GPtrArray *tb = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(tb, g_strdup("10.168.50.0/24"));
    g_assert_true(pcv_res_pool_set("tb", tb, &err));                                     
    assert_pool(pcv_res_pool_list("ta"), (const gchar *const[]){ "10.10.0.0/16", "10.20.0.0/24", NULL });
    JsonArray *all = pcv_res_pool_list_all();
    g_assert_nonnull(all);
    g_assert_cmpuint(json_array_get_length(all), ==, 2);
    JsonObject *first = json_array_get_object_element(all, 0);
    g_assert_cmpstr(json_object_get_string_member(first, "tenant"), ==, "ta");
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(first, "cidrs")), ==, 2);
    g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(all, 1), "tenant"), ==, "tb");
    json_array_unref(all);

    GPtrArray *empty = g_ptr_array_new_with_free_func(g_free);
    g_assert_true(pcv_res_pool_set("ta", empty, &err));                             
    assert_pool(pcv_res_pool_list("ta"), (const gchar *const[]){ NULL });
    assert_pool(pcv_res_pool_list("tb"), (const gchar *const[]){ "10.168.50.0/24", NULL });

    g_assert_true(pcv_res_pool_add("ta", "10.30.0.0/24", &err));                
    g_assert_true(pcv_res_pool_add("ta", "10.30.0.0/24", &err));
    g_assert_no_error(err);
    assert_pool(pcv_res_pool_list("ta"), (const gchar *const[]){ "10.30.0.0/24", NULL });

    g_assert_false(pcv_res_pool_set("bad name!", in, &err));                      
    g_assert_error(err, PCV_RES_ERROR, PCV_RES_ERROR_INVALID);
    g_clear_error(&err);
    g_assert_false(pcv_res_pool_add("bad name!", "10.30.0.0/24", &err));
    g_assert_error(err, PCV_RES_ERROR, PCV_RES_ERROR_INVALID);
    g_clear_error(&err);
    assert_pool(pcv_res_pool_list("bad name!"), (const gchar *const[]){ NULL });
    const gchar *bad_cidrs[] = { "10.30.0.1/24", "8.8.8.0/24", "10.0.0.0/7", "172.16.0.0/11", "10.30.0.0", "",
                                 "100.128.0.0/24", "100.0.0.0/9", "100.64.0.0/9" };
    for (guint i = 0; i < G_N_ELEMENTS(bad_cidrs); i++) {                                            
        g_assert_false(pcv_res_pool_cidr_is_valid(bad_cidrs[i]));
        g_assert_false(pcv_res_pool_add("ta", bad_cidrs[i], &err));
        g_assert_error(err, PCV_RES_ERROR, PCV_RES_ERROR_INVALID);
        g_clear_error(&err);
    }
    g_ptr_array_add(in, g_strdup("10.20.0.1/24"));
    g_assert_false(pcv_res_pool_set("ta", in, &err));                                          
    g_assert_error(err, PCV_RES_ERROR, PCV_RES_ERROR_INVALID);
    g_clear_error(&err);
    assert_pool(pcv_res_pool_list("ta"), (const gchar *const[]){ "10.30.0.0/24", NULL });
    g_assert_true(pcv_res_pool_cidr_is_valid("172.16.0.0/12"));
    g_assert_true(pcv_res_pool_cidr_is_valid("10.168.3.0/24"));
    g_assert_true(pcv_res_pool_cidr_is_valid("100.64.10.0/24"));                                             
    g_assert_true(pcv_res_pool_cidr_is_valid("100.64.0.0/10"));
    g_assert_true(pcv_res_pool_cidr_is_valid("100.127.255.252/30"));

    pcv_res_registry_close();                                                              
    g_assert_true(pcv_res_registry_open(db, &err));
    g_assert_no_error(err);
    assert_pool(pcv_res_pool_list("ta"), (const gchar *const[]){ "10.30.0.0/24", NULL });
    pcv_res_registry_close();

    gchar *newer = g_build_filename(raw, "v3.db", NULL);                                
    g_assert_cmpint(sqlite3_open(newer, &s), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_exec(s, "PRAGMA user_version=3;", NULL, NULL, NULL), ==, SQLITE_OK);
    sqlite3_close(s);
    g_assert_false(pcv_res_registry_open(newer, &err));
    g_assert_error(err, PCV_RES_ERROR, PCV_RES_ERROR_IO);
    g_clear_error(&err);

    g_ptr_array_unref(in);
    g_ptr_array_unref(tb);
    g_ptr_array_unref(empty);
    gchar *argv[] = { "rm", "-rf", raw, NULL };
    g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
    g_free(newer);
    g_free(db);
    g_free(raw);
}

                                                                                                     
                                                                                             
                                                                                    
static void
test_vpc_pool_fresh_file(void)
{
    gchar *raw = g_dir_make_tmp("pcv-respool0-XXXXXX", NULL);
    g_assert_nonnull(raw);
    gchar *db = g_build_filename(raw, "fresh.db", NULL);
    g_assert_false(g_file_test(db, G_FILE_TEST_EXISTS));
    GError *err = NULL;
    g_assert_true(pcv_res_registry_open(db, &err));
    g_assert_no_error(err);
    g_assert_cmpint(db_user_version(db), ==, 2);
    sqlite3 *s = NULL;
    sqlite3_stmt *st = NULL;
    gint tables = -1;
    g_assert_cmpint(sqlite3_open_v2(db, &s, SQLITE_OPEN_READONLY, NULL), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_prepare_v2(s, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name IN"
                                          " ('resources','resource_grants','vpc_pools')", -1, &st, NULL), ==, SQLITE_OK);
    if (sqlite3_step(st) == SQLITE_ROW)
        tables = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    sqlite3_close(s);
    g_assert_cmpint(tables, ==, 3);
    GPtrArray *in = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(in, g_strdup("10.40.0.0/16"));
    g_assert_true(pcv_res_pool_set("ta", in, &err));
    g_assert_no_error(err);
    assert_pool(pcv_res_pool_list("ta"), (const gchar *const[]){ "10.40.0.0/16", NULL });
    pcv_res_registry_close();                                                                    
    g_assert_true(pcv_res_registry_open(db, &err));
    g_assert_no_error(err);
    assert_pool(pcv_res_pool_list("ta"), (const gchar *const[]){ "10.40.0.0/16", NULL });
    pcv_res_registry_close();
    g_ptr_array_unref(in);
    gchar *argv[] = { "rm", "-rf", raw, NULL };
    g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
    g_free(db);
    g_free(raw);
}

                                                                                        
                                                                                               
                                                                           
                                                            
                                           
                                                                                     
static void
test_vpc_pool_no_overlap(void)
{
    Fx *f = fx_new();
    GError *err = NULL;
    GPtrArray *in = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(in, g_strdup("10.10.0.0/16"));
    g_assert_true(pcv_res_pool_set("ta", in, &err));
    g_assert_no_error(err);
    g_assert_true(pcv_res_pool_add("ta", "10.10.0.0/16", &err));                  
    g_assert_no_error(err);
    const gchar *overlap[] = { "10.10.1.0/24", "10.0.0.0/8", "10.10.0.0/15" };
    for (guint i = 0; i < G_N_ELEMENTS(overlap); i++) {                             
        g_assert_false(pcv_res_pool_add("ta", overlap[i], &err));
        g_assert_error(err, PCV_RES_ERROR, PCV_RES_ERROR_INVALID);
        g_assert_cmpstr(err->message, ==, "overlaps existing pool cidr 10.10.0.0/16");
        g_clear_error(&err);
    }
    assert_pool(pcv_res_pool_list("ta"), (const gchar *const[]){ "10.10.0.0/16", NULL });
    g_assert_true(pcv_res_pool_add("ta", "10.11.0.0/16", &err));              
    g_assert_true(pcv_res_pool_add("tb", "10.10.1.0/24", &err));                         
    g_assert_no_error(err);
    assert_pool(pcv_res_pool_list("ta"), (const gchar *const[]){ "10.10.0.0/16", "10.11.0.0/16", NULL });

    GPtrArray *pool = pcv_res_pool_list("ta");                                
    g_assert_null(pcv_res_pool_conflict(pool, "10.10.0.0/16"));
    g_assert_null(pcv_res_pool_conflict(pool, "10.12.0.0/16"));
    g_assert_cmpstr(pcv_res_pool_conflict(pool, "10.11.128.0/17"), ==, "10.11.0.0/16");
    g_assert_cmpstr(pcv_res_pool_conflict(pool, "10.10.0.0/15"), ==, "10.10.0.0/16");
    g_assert_null(pcv_res_pool_conflict(NULL, "10.10.0.0/16"));
    g_ptr_array_unref(pool);

    GPtrArray *bad = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(bad, g_strdup("10.20.0.0/16"));
    g_ptr_array_add(bad, g_strdup("10.20.5.0/24"));
    g_assert_false(pcv_res_pool_set("ta", bad, &err));                                      
    g_assert_error(err, PCV_RES_ERROR, PCV_RES_ERROR_INVALID);
    g_clear_error(&err);
    assert_pool(pcv_res_pool_list("ta"), (const gchar *const[]){ "10.10.0.0/16", "10.11.0.0/16", NULL });
    g_ptr_array_set_size(bad, 0);
    g_ptr_array_add(bad, g_strdup("10.20.0.0/16"));
    g_ptr_array_add(bad, g_strdup("10.20.0.0/16"));
    g_assert_true(pcv_res_pool_set("ta", bad, &err));                                
    g_assert_no_error(err);
    assert_pool(pcv_res_pool_list("ta"), (const gchar *const[]){ "10.20.0.0/16", NULL });
    g_ptr_array_unref(bad);
    g_ptr_array_unref(in);
    fx_free(f);
}

                                                                                   
                                                         
static void
test_normalize_pci_forms(void)
{
    const struct { const gchar *in; const gchar *want; } ok[] = {
        { "01:00.0", "0000:01:00.0" },
        { "0000:3B:00.1", "0000:3b:00.1" },
    };
    const gchar *bad[] = { "", "garbage", "0000:00:20.0", "0000:3b:10.8", " 01:00.0", "0000:3b:00.1\n" };
    PcvResNorm n = {0};

    for (gsize i = 0; i < G_N_ELEMENTS(ok); i++) {
        g_assert_true(pcv_res_normalize(PCV_RES_PCI, ok[i].in, &n, NULL));
        g_assert_cmpstr(n.ref, ==, ok[i].want);
        pcv_res_norm_clear(&n);
    }
    for (gsize i = 0; i < G_N_ELEMENTS(bad); i++) {
        g_assert_false(pcv_res_normalize(PCV_RES_PCI, bad[i], &n, NULL));
        g_assert_null(n.ref);
    }
}

void
test_resource_registry_register(void)
{
    g_test_add_func("/resource_registry/normalize_kinds", test_normalize_kinds);
    g_test_add_func("/resource_registry/normalize_pci_forms", test_normalize_pci_forms);
    g_test_add_func("/resource_registry/register_refresh_and_conflicts", test_register_refresh_and_conflicts);
    g_test_add_func("/resource_registry/identity_rejects_hardlink_and_replacement",
                    test_identity_rejects_hardlink_and_replacement);
    g_test_add_func("/resource_registry/storage_dir_containment_through_symlink",
                    test_storage_dir_containment_through_symlink);
    g_test_add_func("/resource_registry/identity_survives_same_file", test_identity_survives_same_file);
    g_test_add_func("/resource_registry/db_file_mode_0600", test_db_file_mode_0600);
    g_test_add_func("/resource_registry/defaults_allowed_for_every_tenant", test_defaults_allowed_for_every_tenant);
    g_test_add_func("/resource_registry/import_all_or_nothing", test_import_all_or_nothing);
    g_test_add_func("/resource_registry/import_error_item_index", test_import_error_item_index);
    g_test_add_func("/resource_registry/revoke_in_use_by_tenant_owner", test_revoke_in_use_by_tenant_owner);
    g_test_add_func("/resource_registry/unregistered_suggest_name", test_unregistered_suggest_name);
    g_test_add_func("/resource_registry/pin_matches", test_pin_matches);
    g_test_add_func("/resource_registry/vpc_pool_roundtrip", test_vpc_pool_roundtrip);
    g_test_add_func("/resource_registry/vpc_pool_fresh_file", test_vpc_pool_fresh_file);
    g_test_add_func("/resource_registry/vpc_pool_no_overlap", test_vpc_pool_no_overlap);
}
