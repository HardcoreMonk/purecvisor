                                                                                             
                                                               
                                                                      
                                                                                         
#include <glib.h>
#include <gio/gio.h>
#include "../src/modules/dispatcher/vm_delete_plan.h"

#define CDROM "<disk type='file' device='cdrom'><source file='/iso/install.iso'/><target dev='sda'/></disk>"
#define ZVOL(ds) "<disk type='block' device='disk'><source dev='/dev/zvol/" ds "'/><target dev='vda'/></disk>"
#define FILE_DISK(p) "<disk type='file' device='disk'><driver name='qemu' type='qcow2'/><source file='" p "'/><target dev='vda'/></disk>"
#define DOMAIN(body) "<domain><name>web</name><devices>" body "</devices></domain>"

static void plan_of(const gchar *xml, PcvVmDeleteStoragePlan *plan)
{
    GError *error = NULL;
    g_assert_true(pcv_vm_delete_plan_from_xml(xml, "web", plan, &error));
    g_assert_no_error(error);
}

static gboolean kept_has(const PcvVmDeleteStoragePlan *plan, const gchar *value)
{
    for (guint i = 0; i < plan->kept->len; i++)
        if (g_strcmp0(g_ptr_array_index(plan->kept, i), value) == 0)
            return TRUE;
    return FALSE;
}

                                                             
static void test_owned_standard(void)
{
    PcvVmDeleteStoragePlan p;
    plan_of(DOMAIN(CDROM ZVOL("tank/vms/web")), &p);
    g_assert_cmpint(p.kind, ==, PCV_VM_DELETE_STORAGE_ZVOL);
    g_assert_cmpstr(p.dataset, ==, "tank/vms/web");
    g_assert_cmpstr(p.path, ==, "/dev/zvol/tank/vms/web");
    g_assert_cmpuint(p.kept->len, ==, 0);
    pcv_vm_delete_plan_clear(&p);
    for (const gchar *const *f = (const gchar *const[]){ "/img/web.qcow2", "/img/web.raw", "/img/web.img", NULL }; *f; f++) {
        g_autofree gchar *xml = g_strdup_printf("<domain><devices>" CDROM
            "<disk type='file' device='disk'><source file='%s'/></disk></devices></domain>", *f);
        plan_of(xml, &p);
        g_assert_cmpint(p.kind, ==, PCV_VM_DELETE_STORAGE_FILE);
        g_assert_cmpstr(p.path, ==, *f);
        g_assert_null(p.dataset);
        g_assert_cmpuint(p.kept->len, ==, 0);
        pcv_vm_delete_plan_clear(&p);
    }
}

                                                       
static void test_file_boot_with_attached_zvol(void)
{
    PcvVmDeleteStoragePlan p;
    plan_of(DOMAIN(FILE_DISK("/img/web.qcow2") ZVOL("tank/other")), &p);
    g_assert_cmpint(p.kind, ==, PCV_VM_DELETE_STORAGE_FILE);
    g_assert_cmpstr(p.path, ==, "/img/web.qcow2");
    g_assert_cmpuint(p.kept->len, ==, 1);
    g_assert_true(kept_has(&p, "tank/other"));
    pcv_vm_delete_plan_clear(&p);
}

                                                                        
static void test_zvol_boot_with_extra_disks(void)
{
    PcvVmDeleteStoragePlan p;
    plan_of(DOMAIN(ZVOL("tank/web") FILE_DISK("/img/extra.raw") ZVOL("tank/other") FILE_DISK("/img/web.qcow2")), &p);
    g_assert_cmpint(p.kind, ==, PCV_VM_DELETE_STORAGE_ZVOL);
    g_assert_cmpstr(p.dataset, ==, "tank/web");
    g_assert_cmpuint(p.kept->len, ==, 3);
    g_assert_true(kept_has(&p, "/img/extra.raw"));
    g_assert_true(kept_has(&p, "tank/other"));
    g_assert_true(kept_has(&p, "/img/web.qcow2"));
    pcv_vm_delete_plan_clear(&p);
}

                                                                                        
static void test_first_block_not_owned(void)
{
    const struct { const gchar *xml; const gchar *kept; } rows[] = {
        { DOMAIN(ZVOL("tank/other")), "tank/other" },
        { DOMAIN(ZVOL("web")), "web" },                                                                  
        { DOMAIN(ZVOL("tank/web2")), "tank/web2" },                                  
        { DOMAIN(ZVOL("tank/web@snap")), "tank/web@snap" },                        
        { DOMAIN(ZVOL("tank/../web")), "tank/../web" },                           
        { DOMAIN(FILE_DISK("/img/other.qcow2")), "/img/other.qcow2" },
        { DOMAIN(FILE_DISK("/img/web.vmdk")), "/img/web.vmdk" },                     
        { DOMAIN(FILE_DISK("web.qcow2")), "web.qcow2" },                          
        { DOMAIN(FILE_DISK("/img/../web.qcow2")), "/img/../web.qcow2" },
        { DOMAIN("<disk type='block' device='disk'><source dev='/dev/sdb'/></disk>"), "/dev/sdb" },
        { DOMAIN("<disk type='file' device='disk'><source file='/dev/web.img'/></disk>"), "/dev/web.img" },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(rows); i++) {
        PcvVmDeleteStoragePlan p;
        plan_of(rows[i].xml, &p);
        g_assert_cmpint(p.kind, ==, PCV_VM_DELETE_STORAGE_NONE);
        g_assert_null(p.path);
        g_assert_true(kept_has(&p, rows[i].kept));
        pcv_vm_delete_plan_clear(&p);
    }
                                                                       
    PcvVmDeleteStoragePlan p;
    plan_of(DOMAIN(ZVOL("tank/other") ZVOL("tank/web")), &p);
    g_assert_cmpint(p.kind, ==, PCV_VM_DELETE_STORAGE_NONE);
    g_assert_cmpuint(p.kept->len, ==, 2);
    pcv_vm_delete_plan_clear(&p);
}

                                                               
static void test_legacy_file_attr_zvol(void)
{
    PcvVmDeleteStoragePlan p;
    plan_of(DOMAIN("<disk type='file' device='disk'><source file='/dev/zvol/tank/web'/></disk>"), &p);
    g_assert_cmpint(p.kind, ==, PCV_VM_DELETE_STORAGE_ZVOL);
    g_assert_cmpstr(p.dataset, ==, "tank/web");
    pcv_vm_delete_plan_clear(&p);
}

                                        
static void test_nothing_to_delete(void)
{
    PcvVmDeleteStoragePlan p;
    plan_of("<domain><devices/></domain>", &p);
    g_assert_cmpint(p.kind, ==, PCV_VM_DELETE_STORAGE_NONE);
    g_assert_cmpuint(p.kept->len, ==, 0);
    pcv_vm_delete_plan_clear(&p);
    plan_of("<domain/>", &p);
    g_assert_cmpint(p.kind, ==, PCV_VM_DELETE_STORAGE_NONE);
    pcv_vm_delete_plan_clear(&p);
    plan_of(DOMAIN(CDROM), &p);
    g_assert_cmpint(p.kind, ==, PCV_VM_DELETE_STORAGE_NONE);
    g_assert_cmpuint(p.kept->len, ==, 0);                              
    pcv_vm_delete_plan_clear(&p);
                                
    plan_of(DOMAIN("<disk type='file' device='disk'><target dev='vda'/></disk>" ZVOL("tank/web")), &p);
    g_assert_cmpint(p.kind, ==, PCV_VM_DELETE_STORAGE_NONE);
    g_assert_true(kept_has(&p, "tank/web"));
    pcv_vm_delete_plan_clear(&p);
    plan_of(DOMAIN("<disk type='network' device='disk'><source protocol='rbd' name='p/i'/><target dev='vda'/></disk>"), &p);
    g_assert_cmpint(p.kind, ==, PCV_VM_DELETE_STORAGE_NONE);
    g_assert_true(kept_has(&p, "network disk vda"));
    pcv_vm_delete_plan_clear(&p);
                                        
    plan_of(DOMAIN("<disk type='block' device='lun'><source dev='/dev/zvol/tank/web'/></disk>"), &p);
    g_assert_cmpint(p.kind, ==, PCV_VM_DELETE_STORAGE_NONE);
    g_assert_cmpuint(p.kept->len, ==, 0);
    pcv_vm_delete_plan_clear(&p);
}

                                                         
static void test_unreadable_xml_blocks(void)
{
    for (const gchar *const *x = (const gchar *const[]){ "not xml", "<domain><devices>", "<other/>", "", NULL }; *x; x++) {
        PcvVmDeleteStoragePlan p;
        GError *error = NULL;
        g_assert_false(pcv_vm_delete_plan_from_xml(*x, "web", &p, &error));
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
        g_clear_error(&error);
        g_assert_cmpint(p.kind, ==, PCV_VM_DELETE_STORAGE_NONE);
        g_assert_null(p.kept);
        pcv_vm_delete_plan_clear(&p);
    }
    PcvVmDeleteStoragePlan p;
    GError *error = NULL;
    g_assert_false(pcv_vm_delete_plan_from_xml(NULL, "web", &p, &error));
    g_clear_error(&error);
}

                                                          
static void test_live_only_disk_added_to_kept(void)
{
    PcvVmDeleteStoragePlan p;
    plan_of(DOMAIN(ZVOL("tank/web") ZVOL("tank/conf")), &p);
    GError *error = NULL;
    g_assert_true(pcv_vm_delete_plan_add_kept_from_xml(DOMAIN(ZVOL("tank/web") ZVOL("tank/live")), &p, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(p.kept->len, ==, 2);
    g_assert_true(kept_has(&p, "tank/conf"));
    g_assert_true(kept_has(&p, "tank/live"));
    g_assert_false(pcv_vm_delete_plan_add_kept_from_xml("broken", &p, &error));
    g_clear_error(&error);
    pcv_vm_delete_plan_clear(&p);
}

                   
static void test_absent_default_path(void)
{
    PcvVmDeleteStoragePlan p;
    pcv_vm_delete_plan_for_absent("tank/vms", "web", &p);
    g_assert_cmpint(p.kind, ==, PCV_VM_DELETE_STORAGE_ZVOL);
    g_assert_cmpstr(p.dataset, ==, "tank/vms/web");
    g_assert_cmpstr(p.path, ==, "/dev/zvol/tank/vms/web");
    g_assert_cmpuint(p.kept->len, ==, 0);
    pcv_vm_delete_plan_clear(&p);
}

                                                                
static void test_disk_refs(void)
{
    GError *error = NULL;
    GPtrArray *refs = pcv_vm_delete_xml_disk_refs(DOMAIN(CDROM
        "<disk type='file' device='disk'><driver type='qcow2'/><source file='/img/ovl.qcow2'/>"
        "<backingStore type='file'><source file='/img/mid.qcow2'/>"
        "<backingStore type='block'><source dev='/dev/zvol/tank/base'/></backingStore></backingStore>"
        "<mirror type='file'><source file='/img/mirror.qcow2'/></mirror></disk>"
        ZVOL("tank/web")), &error);
    g_assert_no_error(error);
    g_assert_cmpuint(refs->len, ==, 6);
    const gchar *expected[] = { "/iso/install.iso", "/img/ovl.qcow2", "/img/mid.qcow2", "/dev/zvol/tank/base", "/img/mirror.qcow2", "/dev/zvol/tank/web" };
    for (guint i = 0; i < refs->len; i++)
        g_assert_cmpstr(((PcvVmDeleteDiskRef *)g_ptr_array_index(refs, i))->path, ==, expected[i]);
    g_assert_cmpstr(((PcvVmDeleteDiskRef *)g_ptr_array_index(refs, 1))->format, ==, "qcow2");
    g_assert_null(((PcvVmDeleteDiskRef *)g_ptr_array_index(refs, 2))->format);
    g_ptr_array_unref(refs);
    g_assert_null(pcv_vm_delete_xml_disk_refs("broken", &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
}

                           
static void test_snapshot_clones(void)
{
    GPtrArray *c = pcv_vm_delete_parse_snapshot_clones(
        "tank/web@a\t-\n"
        "tank/web@b\ttank/c1,tank/c2\n"
        "tank/web@c\t\n"
        "\n"
        "garbage line\n"
        "tank/web@d\ttank/c3\n");
    g_assert_cmpuint(c->len, ==, 3);
    g_assert_cmpstr(g_ptr_array_index(c, 0), ==, "tank/c1");
    g_assert_cmpstr(g_ptr_array_index(c, 1), ==, "tank/c2");
    g_assert_cmpstr(g_ptr_array_index(c, 2), ==, "tank/c3");
    g_ptr_array_unref(c);
    c = pcv_vm_delete_parse_snapshot_clones("");
    g_assert_cmpuint(c->len, ==, 0);
    g_ptr_array_unref(c);
    c = pcv_vm_delete_parse_snapshot_clones(NULL);
    g_assert_cmpuint(c->len, ==, 0);
    g_ptr_array_unref(c);
}

                                                                                 
static void test_backing_chain(void)
{
    GError *error = NULL;
    GPtrArray *f = pcv_vm_delete_parse_backing_chain(
        "[{\"filename\":\"/img/ovl.qcow2\",\"full-backing-filename\":\"/img/base.qcow2\",\"backing-filename\":\"base.qcow2\"},"
        "{\"filename\":\"/img/base.qcow2\",\"backing-filename\":\"/img/root.qcow2\"},"
        "{\"filename\":\"/img/root.qcow2\"}]", &error);
    g_assert_no_error(error);
    g_assert_cmpuint(f->len, ==, 2);
    g_assert_cmpstr(g_ptr_array_index(f, 0), ==, "/img/base.qcow2");
    g_assert_cmpstr(g_ptr_array_index(f, 1), ==, "/img/root.qcow2");
    g_ptr_array_unref(f);
    g_assert_null(pcv_vm_delete_parse_backing_chain("{\"filename\":\"x\"}", &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    g_assert_null(pcv_vm_delete_parse_backing_chain("not json", &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_assert_null(pcv_vm_delete_parse_backing_chain(NULL, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
}

void test_vm_delete_plan_register(void)
{
    g_test_add_func("/vm_delete_plan/owned_standard", test_owned_standard);
    g_test_add_func("/vm_delete_plan/file_boot_with_attached_zvol", test_file_boot_with_attached_zvol);
    g_test_add_func("/vm_delete_plan/zvol_boot_with_extra_disks", test_zvol_boot_with_extra_disks);
    g_test_add_func("/vm_delete_plan/first_block_not_owned", test_first_block_not_owned);
    g_test_add_func("/vm_delete_plan/legacy_file_attr_zvol", test_legacy_file_attr_zvol);
    g_test_add_func("/vm_delete_plan/nothing_to_delete", test_nothing_to_delete);
    g_test_add_func("/vm_delete_plan/unreadable_xml_blocks", test_unreadable_xml_blocks);
    g_test_add_func("/vm_delete_plan/live_only_disk_added_to_kept", test_live_only_disk_added_to_kept);
    g_test_add_func("/vm_delete_plan/absent_default_path", test_absent_default_path);
    g_test_add_func("/vm_delete_plan/disk_refs", test_disk_refs);
    g_test_add_func("/vm_delete_plan/snapshot_clones", test_snapshot_clones);
    g_test_add_func("/vm_delete_plan/backing_chain", test_backing_chain);
}
