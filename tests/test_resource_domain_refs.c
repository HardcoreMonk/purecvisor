   
                                    
                                                                                                       
  
                           
                                                                                                  
                                                                                                         
                                                                                                             
                                                                                                      
                                                                                                           
                                                                                             
                                                                                                                    
                                                                                                
  
                                                                           
                                          
   
#include <glib.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "modules/resource/resource_domain_refs.h"

#define FX "tests/fixtures/resource_domain_refs/"

                                 
static void
parse_fixture(const gchar *name, PcvResDomainRefs *out)
{
    g_autofree gchar *path = g_strconcat(FX, name, NULL);
    g_autofree gchar *xml = NULL;
    g_autoptr(GError) err = NULL;
    g_assert_true(g_file_get_contents(path, &xml, NULL, &err));
    g_assert_true(pcv_res_domain_refs_parse(xml, out, &err));
    g_assert_no_error(err);
}

static void
test_full_domain_refs(void)
{
    PcvResDomainRefs r = { 0 };
    parse_fixture("full.xml", &r);
    g_assert_cmpstr(r.owner, ==, "alice");
    g_assert_cmpuint(r.refs->len, ==, 7);
    g_assert_true(pcv_res_domain_refs_uses(&r, PCV_RES_IMAGE, "/srv/vm/a.qcow2"));
    g_assert_true(pcv_res_domain_refs_uses(&r, PCV_RES_STORAGE, "/srv/vm"));
    g_assert_true(pcv_res_domain_refs_uses(&r, PCV_RES_STORAGE, "tank/vms"));
    g_assert_true(pcv_res_domain_refs_uses(&r, PCV_RES_ISO, "/isos/u.iso"));
    g_assert_true(pcv_res_domain_refs_uses(&r, PCV_RES_PCI, "0000:3b:00.1"));
    g_assert_true(pcv_res_domain_refs_uses(&r, PCV_RES_USB, "1d6b:0002"));
    g_assert_true(pcv_res_domain_refs_uses(&r, PCV_RES_BRIDGE, "br-x"));
                                                 
    g_assert_false(pcv_res_domain_refs_uses(&r, PCV_RES_IMAGE, "/isos/u.iso"));
    g_assert_false(pcv_res_domain_refs_uses(&r, PCV_RES_STORAGE, "/isos"));
    pcv_res_domain_refs_clear(&r);
    pcv_res_domain_refs_clear(&r);                  
}

static void
test_no_owner_and_network_iface(void)
{
    PcvResDomainRefs r = { 0 };
    parse_fixture("network_iface.xml", &r);
    g_assert_null(r.owner);
    g_assert_cmpuint(r.refs->len, ==, 0);
    pcv_res_domain_refs_clear(&r);
}

static void
test_live_only_usb_counts(void)
{
    PcvResDomainRefs def = { 0 }, live = { 0 };
    parse_fixture("noowner.xml", &def);
    g_assert_false(pcv_res_domain_refs_uses(&def, PCV_RES_USB, "1d6b:0002"));
    parse_fixture("live_usb.xml", &live);
    pcv_res_domain_refs_merge(&def, &live);
    g_assert_true(pcv_res_domain_refs_uses(&def, PCV_RES_USB, "1d6b:0002"));
                                       
    pcv_res_domain_refs_merge(&def, &live);
    g_assert_cmpuint(def.refs->len, ==, 1);
    pcv_res_domain_refs_clear(&def);
    pcv_res_domain_refs_clear(&live);
}

static void
test_uses_storage_containment(void)
{
    PcvResDomainRefs r = { 0 };
    parse_fixture("full.xml", &r);
    g_assert_true(pcv_res_domain_refs_uses(&r, PCV_RES_STORAGE, "/srv"));
    g_assert_false(pcv_res_domain_refs_uses(&r, PCV_RES_STORAGE, "/srv/vmx"));
    g_assert_true(pcv_res_domain_refs_uses(&r, PCV_RES_STORAGE, "tank"));
    g_assert_false(pcv_res_domain_refs_uses(&r, PCV_RES_STORAGE, "tan"));
                                       
    g_assert_false(pcv_res_domain_refs_uses(&r, PCV_RES_ISO, "/isos"));
    pcv_res_domain_refs_clear(&r);
}

static void
test_rejects_external_entities(void)
{
    const gchar *xml =
        "<?xml version=\"1.0\"?>\n"
        "<!DOCTYPE domain [ <!ENTITY xxe SYSTEM \"file:///etc/passwd\"> ]>\n"
        "<domain type='kvm'><name>x</name>"
        "<metadata><owner>&xxe;</owner></metadata>"
        "<devices/></domain>";
    PcvResDomainRefs r = { 0 };
    g_autoptr(GError) err = NULL;
                                                             
    g_assert_true(pcv_res_domain_refs_parse(xml, &r, &err));
    g_assert_cmpuint(r.refs->len, ==, 0);
    g_assert_true(r.owner == NULL || strstr(r.owner, "root:") == NULL);
    pcv_res_domain_refs_clear(&r);
}

                                                                                                        
static void
test_zvol_block_disk_storage(void)
{
    const gchar *xml =
        "<domain type='kvm'><name>z</name><devices>"
        "<disk type='block' device='disk'><source dev='/dev/zvol/tank/a'/></disk>"
        "</devices></domain>";
    PcvResDomainRefs r = { 0 };
    g_autoptr(GError) err = NULL;
    g_assert_true(pcv_res_domain_refs_parse(xml, &r, &err));
    g_assert_cmpuint(r.refs->len, ==, 1);
    const PcvResRef *x = g_ptr_array_index(r.refs, 0);
    g_assert_cmpint(x->kind, ==, PCV_RES_STORAGE);
    g_assert_cmpstr(x->ref, ==, "tank");
    g_assert_true(pcv_res_domain_refs_uses(&r, PCV_RES_STORAGE, "tank"));
    g_assert_false(pcv_res_domain_refs_uses(&r, PCV_RES_STORAGE, "tank/a"));
    pcv_res_domain_refs_clear(&r);
}

                                                                                   
                                                         
static void
test_canonicalize_resolves_links(void)
{
    g_autofree gchar *raw = g_dir_make_tmp("pcv-domrefs-XXXXXX", NULL);
    g_assert_nonnull(raw);
    gchar *tmp = realpath(raw, NULL);
    g_assert_nonnull(tmp);
    g_autofree gchar *real = g_build_filename(tmp, "real", NULL);
    g_autofree gchar *link = g_build_filename(tmp, "link", NULL);
    g_assert_cmpint(g_mkdir_with_parents(real, 0755), ==, 0);
    g_assert_cmpint(symlink(real, link), ==, 0);
    g_autofree gchar *iso_real = g_build_filename(real, "a.iso", NULL);
    g_autofree gchar *img_real = g_build_filename(real, "d.qcow2", NULL);
    g_assert_true(g_file_set_contents(iso_real, "I", -1, NULL));
    g_assert_true(g_file_set_contents(img_real, "Q", -1, NULL));
    g_autofree gchar *xml = g_strdup_printf(
        "<domain type='kvm'><name>c</name><devices>"
        "<disk type='file' device='disk'><source file='%s/d.qcow2'/></disk>"
        "<disk type='file' device='cdrom'><source file='%s/a.iso'/></disk>"
        "<disk type='file' device='cdrom'><source file='/nonexistent-pcv/x.iso'/></disk>"
        "</devices></domain>", link, link);
    PcvResDomainRefs r = { 0 };
    g_autoptr(GError) err = NULL;
    g_assert_true(pcv_res_domain_refs_parse(xml, &r, &err));
    g_assert_false(pcv_res_domain_refs_uses(&r, PCV_RES_ISO, iso_real));                           
    g_assert_false(pcv_res_domain_refs_uses(&r, PCV_RES_STORAGE, real));
    pcv_res_domain_refs_canonicalize(&r);
    g_assert_true(pcv_res_domain_refs_uses(&r, PCV_RES_ISO, iso_real));
    g_assert_true(pcv_res_domain_refs_uses(&r, PCV_RES_IMAGE, img_real));
    g_assert_true(pcv_res_domain_refs_uses(&r, PCV_RES_STORAGE, real));
    g_assert_true(pcv_res_domain_refs_uses(&r, PCV_RES_ISO, "/nonexistent-pcv/x.iso"));                
    g_assert_cmpuint(r.refs->len, ==, 4);
    pcv_res_domain_refs_clear(&r);
    gchar *argv[] = { "rm", "-rf", tmp, NULL };
    g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
    free(tmp);
}

                                                                                                          
                                                     
static void
test_canonicalize_link_disk_real_dir(void)
{
    g_autofree gchar *raw = g_dir_make_tmp("pcv-domrefs-XXXXXX", NULL);
    g_assert_nonnull(raw);
    gchar *tmp = realpath(raw, NULL);
    g_assert_nonnull(tmp);
    g_autofree gchar *vm = g_build_filename(tmp, "vm", NULL);
    g_autofree gchar *real = g_build_filename(tmp, "real", NULL);
    g_assert_cmpint(g_mkdir_with_parents(vm, 0755), ==, 0);
    g_assert_cmpint(g_mkdir_with_parents(real, 0755), ==, 0);
    g_autofree gchar *img_real = g_build_filename(real, "a.qcow2", NULL);
    g_autofree gchar *img_link = g_build_filename(vm, "a.qcow2", NULL);
    g_assert_true(g_file_set_contents(img_real, "Q", -1, NULL));
    g_assert_cmpint(symlink(img_real, img_link), ==, 0);
    g_autofree gchar *xml = g_strdup_printf(
        "<domain type='kvm'><name>l</name><devices>"
        "<disk type='file' device='disk'><source file='%s'/></disk>"
        "</devices></domain>", img_link);
    PcvResDomainRefs r = { 0 };
    g_autoptr(GError) err = NULL;
    g_assert_true(pcv_res_domain_refs_parse(xml, &r, &err));
    g_assert_false(pcv_res_domain_refs_uses(&r, PCV_RES_STORAGE, real));                            
    pcv_res_domain_refs_canonicalize(&r);
    g_assert_true(pcv_res_domain_refs_uses(&r, PCV_RES_IMAGE, img_real));
    g_assert_true(pcv_res_domain_refs_uses(&r, PCV_RES_STORAGE, vm));
    g_assert_true(pcv_res_domain_refs_uses(&r, PCV_RES_STORAGE, real));
    g_assert_cmpuint(r.refs->len, ==, 3);                                                             
    pcv_res_domain_refs_clear(&r);
    gchar *argv[] = { "rm", "-rf", tmp, NULL };
    g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
    free(tmp);
}

                                                                                                    
                                  
static void
test_active_state_fail_closed(void)
{
    g_assert_cmpint(pcv_res_domain_active_state(1, FALSE), ==, PCV_RES_DOM_ACTIVE);
    g_assert_cmpint(pcv_res_domain_active_state(0, FALSE), ==, PCV_RES_DOM_INACTIVE);
    g_assert_cmpint(pcv_res_domain_active_state(-1, TRUE), ==, PCV_RES_DOM_GONE);
    g_assert_cmpint(pcv_res_domain_active_state(-1, FALSE), ==, PCV_RES_DOM_UNKNOWN);
                                                                  
    g_assert_cmpint(pcv_res_domain_active_state(1, TRUE), ==, PCV_RES_DOM_ACTIVE);
    g_assert_cmpint(pcv_res_domain_active_state(0, TRUE), ==, PCV_RES_DOM_INACTIVE);
}

void test_resource_domain_refs_register(void);

                                                        
void
test_resource_domain_refs_register(void)
{
    g_test_add_func("/resource_domain_refs/full_domain_refs", test_full_domain_refs);
    g_test_add_func("/resource_domain_refs/no_owner_and_network_iface", test_no_owner_and_network_iface);
    g_test_add_func("/resource_domain_refs/live_only_usb_counts", test_live_only_usb_counts);
    g_test_add_func("/resource_domain_refs/uses_storage_containment", test_uses_storage_containment);
    g_test_add_func("/resource_domain_refs/rejects_external_entities", test_rejects_external_entities);
    g_test_add_func("/resource_domain_refs/zvol_block_disk_storage", test_zvol_block_disk_storage);
    g_test_add_func("/resource_domain_refs/canonicalize_resolves_links", test_canonicalize_resolves_links);
    g_test_add_func("/resource_domain_refs/canonicalize_link_disk_real_dir", test_canonicalize_link_disk_real_dir);
    g_test_add_func("/resource_domain_refs/active_state_fail_closed", test_active_state_fail_closed);
}
