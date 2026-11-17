   
                             
                                                                                                      
  
                           
                                                                                                 
                                                                      
                                                                                                        
  
                                                                                   
                                  
   
#include <gio/gio.h>
#include <glib.h>
#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>
#include <string.h>

#include "modules/virt/vm_owner_meta.h"

void pcv_vm_owner_meta_set_read_fn_for_test(gchar *(*fn)(virDomainPtr dom, gboolean live));
guint pcv_vm_owner_meta_restores_for_test(void);

                                                      
static void
test_fragment_shape(void)
{
    gchar *a = pcv_vm_owner_metadata_fragment("alice");
    g_assert_cmpstr(a, ==, "<owner>alice</owner>");
    gchar *b = pcv_vm_owner_metadata_fragment("a<b&c'd\"e");
    g_assert_cmpstr(b, ==, "<owner>a&lt;b&amp;c&apos;d&quot;e</owner>");
    g_assert_null(strstr(b, "pcv:"));
    g_assert_null(strstr(b, "xmlns"));
    g_assert_null(pcv_vm_owner_metadata_fragment(NULL));
    g_assert_null(pcv_vm_owner_metadata_fragment(""));
    g_free(a);
    g_free(b);
}

static void
_quiet(void *ud, virErrorPtr err)
{
    (void)ud;
    (void)err;
}

                                                          
static void
test_set_config_and_live(void)
{
    virSetErrorFunc(NULL, _quiet);
    virConnectPtr c = virConnectOpen("test:///default");
    g_assert_nonnull(c);
    virDomainPtr run = virDomainLookupByName(c, "test");
    g_assert_nonnull(run);
    g_assert_cmpint(virDomainIsActive(run), ==, 1);
    gboolean live = FALSE;
    GError *err = NULL;
    g_assert_true(pcv_vm_owner_metadata_set(run, "alice", NULL, &live, &err));
    g_assert_no_error(err);
    g_assert_true(live);
    gchar *cfg = pcv_vm_owner_metadata_read(run, FALSE), *lv = pcv_vm_owner_metadata_read(run, TRUE);
    g_assert_cmpstr(cfg, ==, "alice");
    g_assert_cmpstr(lv, ==, "alice");
    g_free(cfg);
    g_free(lv);

    virDomainPtr off = virDomainDefineXML(c, "<domain type='test'><name>owner-meta-off</name><memory>8192</memory>"
                                             "<os><type>hvm</type></os></domain>");
    g_assert_nonnull(off);
    g_assert_true(pcv_vm_owner_metadata_set(off, "bob", "alice", &live, &err));
    g_assert_no_error(err);
    g_assert_false(live);
    cfg = pcv_vm_owner_metadata_read(off, FALSE);
    g_assert_cmpstr(cfg, ==, "bob");
    g_free(cfg);
    virDomainUndefine(off);
    virDomainFree(off);
    virDomainFree(run);
    virConnectClose(c);
    virSetErrorFunc(NULL, NULL);
}

static gchar *
_read_mallory(virDomainPtr dom, gboolean live_flag)
{
    (void)dom;
    (void)live_flag;
    return g_strdup("mallory");
}

                                                             
static void
test_verify_mismatch_rollback(void)
{
    virSetErrorFunc(NULL, _quiet);
    virConnectPtr c = virConnectOpen("test:///default");
    virDomainPtr run = virDomainLookupByName(c, "test");
    g_assert_nonnull(run);
    pcv_vm_owner_meta_set_read_fn_for_test(_read_mallory);
    guint before = pcv_vm_owner_meta_restores_for_test();
    gboolean live = TRUE;
    GError *err = NULL;
    g_assert_false(pcv_vm_owner_metadata_set(run, "carol", "alice", &live, &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_cmpstr(err->message, ==, "verify");
    g_assert_false(live);
    g_assert_cmpuint(pcv_vm_owner_meta_restores_for_test() - before, ==, 1);
    pcv_vm_owner_meta_set_read_fn_for_test(NULL);
    gchar *cfg = pcv_vm_owner_metadata_read(run, FALSE);
    g_assert_cmpstr(cfg, ==, "alice");                    
    g_free(cfg);
    g_clear_error(&err);
    virDomainFree(run);
    virConnectClose(c);
    virSetErrorFunc(NULL, NULL);
}

void test_vm_owner_meta_register(void);
void
test_vm_owner_meta_register(void)
{
    g_test_add_func("/vm_owner_meta/fragment_shape", test_fragment_shape);
    g_test_add_func("/vm_owner_meta/set_config_and_live", test_set_config_and_live);
    g_test_add_func("/vm_owner_meta/verify_mismatch_rollback", test_verify_mismatch_rollback);
}
