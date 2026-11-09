   
                                
                                           
  
                           
                                                                 
                                                                   
  
                                                   
                            
                                                          
   
#include <glib.h>
#include <glib/gstdio.h>
#include <unistd.h>
#include <gio/gio.h>
#include <string.h>

#include "modules/dispatcher/hotplug_disk_xml.h"

static const gchar *TWO_DISKS =
    "<domain><devices>"
    "<disk type='file' device='disk'><source file='/a.img'/>"
    "<target dev='vda' bus='virtio'/></disk>"
    "<disk type='file' device='disk'><source file='/b.img'/>"
    "<target dev='vdb' bus='virtio'/></disk>"
    "</devices></domain>";

static void test_select_second_of_two(void)
{
    GError *error = NULL;
    gchar *xml = pcv_hotplug_select_disk_xml(TWO_DISKS, "vdb", &error);

    g_assert_no_error(error);
    g_assert_nonnull(xml);
    g_assert_nonnull(strstr(xml, "dev=\"vdb\""));
    g_assert_null(strstr(xml, "dev=\"vda\""));
    g_assert_true(g_str_has_prefix(xml, "<disk"));
    g_assert_true(g_str_has_suffix(xml, "</disk>"));
    g_free(xml);
}

static void test_cdrom_sda(void)
{
    const gchar *src =
        "<domain><devices>"
        "<disk type='file' device='cdrom'><source file='/x.iso'/>"
        "<target dev='sda' bus='sata'/><readonly/></disk>"
        "</devices></domain>";
    GError *error = NULL;
    gchar *xml = pcv_hotplug_select_disk_xml(src, "sda", &error);

    g_assert_no_error(error);
    g_assert_nonnull(xml);
    g_assert_nonnull(strstr(xml, "device=\"cdrom\""));
    g_free(xml);
}

static void test_non_disk_target_vnet0(void)
{
    const gchar *src =
        "<domain><devices>"
        "<interface type='network'><mac address='52:54:00:11:22:33'/>"
        "<target dev='vnet0'/></interface>"
        "<disk type='file' device='disk'><target dev='vda' bus='virtio'/></disk>"
        "</devices></domain>";
    GError *error = NULL;
    gchar *xml = pcv_hotplug_select_disk_xml(src, "vnet0", &error);

    g_assert_null(xml);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_assert_cmpstr(error->message, ==, "Device not found in live XML");
    g_clear_error(&error);
}

static void test_prefix_target_exact(void)
{
    const gchar *src =
        "<domain><devices>"
        "<disk type='file' device='disk'><target dev='vdaa' bus='virtio'/></disk>"
        "</devices></domain>";
    GError *error = NULL;
    gchar *xml = pcv_hotplug_select_disk_xml(src, "vda", &error);

    g_assert_null(xml);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error(&error);

    xml = pcv_hotplug_select_disk_xml(src, "vdaa", &error);
    g_assert_no_error(error);
    g_assert_nonnull(xml);
    g_assert_nonnull(strstr(xml, "dev=\"vdaa\""));
    g_free(xml);
}

static void test_disk_without_target_skipped(void)
{
    const gchar *src =
        "<domain><devices>"
        "<disk type='file' device='disk'><source file='/n.img'/></disk>"
        "<disk type='file' device='disk'><source file='/b.img'/>"
        "<target dev='vdb' bus='virtio'/></disk>"
        "</devices></domain>";
    GError *error = NULL;
    gchar *xml = pcv_hotplug_select_disk_xml(src, "vdb", &error);

    g_assert_no_error(error);
    g_assert_nonnull(xml);
    g_assert_nonnull(strstr(xml, "/b.img"));
    g_assert_null(strstr(xml, "/n.img"));
    g_free(xml);
}

static void test_malformed_xml(void)
{
    GError *error = NULL;
    gchar *xml = pcv_hotplug_select_disk_xml("<domain><devices><disk>",
                                             "vda", &error);

    g_assert_null(xml);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
}

static void test_null_and_empty_inputs(void)
{
    const gchar *xmls[] = { NULL, "", TWO_DISKS, TWO_DISKS };
    const gchar *targets[] = { "vda", "vda", NULL, "" };

    for (guint i = 0; i < G_N_ELEMENTS(xmls); i++) {
        GError *error = NULL;
        gchar *xml = pcv_hotplug_select_disk_xml(xmls[i], targets[i], &error);

        g_assert_null(xml);
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
        g_clear_error(&error);
    }
}

static void test_external_entity_not_expanded(void)
{
    gchar *path = NULL;
    gint fd = g_file_open_tmp("pcv-disk-xml-XXXXXX", &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_true(g_file_set_contents(path, "vdz", -1, NULL));
    close(fd);

    gchar *src = g_strdup_printf(
        "<!DOCTYPE domain [<!ENTITY x SYSTEM \"file://%s\">]>"
        "<domain><devices><disk type='file' device='disk'>"
        "<target dev='&x;' bus='virtio'/></disk></devices></domain>", path);
    GError *error = NULL;
    gchar *xml = pcv_hotplug_select_disk_xml(src, "vdz", &error);

    g_assert_null(xml);
    g_assert_nonnull(error);
    g_assert_true(g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) ||
                  g_error_matches(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA));
    g_clear_error(&error);
    g_unlink(path);
    g_free(path);
    g_free(src);
}

                                                                                         
static const gchar *PLAN_LIVE_VDB =
    "<domain><devices><disk type='block' device='disk'><source dev='/dev/live'/>"
    "<target dev='vdb' bus='virtio'/></disk></devices></domain>";
static const gchar *PLAN_CFG_VDB =
    "<domain><devices><disk type='block' device='disk'><source dev='/dev/cfg'/>"
    "<target dev='vdb' bus='virtio'/></disk></devices></domain>";
static const gchar *PLAN_LIVE_SAME =
    "<domain><devices><disk type='block' device='disk'><source dev='/dev/same'/>"
    "<driver name='qemu' type='raw' cache='none'/><target dev='vdb' bus='virtio'/>"
    "<alias name='virtio-disk1'/></disk></devices></domain>";
static const gchar *PLAN_CFG_SAME =
    "<domain><devices><disk type='block' device='disk'><source dev='/dev/same'/>"
    "<target dev='vdb' bus='virtio'/></disk></devices></domain>";
static const gchar *PLAN_NONE = "<domain><devices></devices></domain>";

typedef struct {
    const gchar *name;
    const gchar *live, *cfg;
    gboolean active;
    gboolean ok, want_live, want_cfg;
    const gchar *want_src;                                      
    gint err_code;                        
    const gchar *err_msg;
} PlanRow;

static void test_detach_plan_table(void)
{
    const PlanRow rows[] = {
        { "running both same",   PLAN_LIVE_SAME, PLAN_CFG_SAME, TRUE,  TRUE,  TRUE,  TRUE,  "/dev/same", 0, NULL },
        { "running live only",   PLAN_LIVE_VDB,  PLAN_NONE,     TRUE,  TRUE,  TRUE,  FALSE, "/dev/live", 0, NULL },
        { "running config only", PLAN_NONE,      PLAN_CFG_VDB,  TRUE,  TRUE,  FALSE, TRUE,  "/dev/cfg",  0, NULL },
        { "running differs",     PLAN_LIVE_VDB,  PLAN_CFG_VDB,  TRUE,  FALSE, FALSE, FALSE, NULL, G_IO_ERROR_FAILED,
          "Disk 'vdb' differs between the running VM and its persistent definition; nothing was changed" },
        { "off config",          NULL,           PLAN_CFG_VDB,  FALSE, TRUE,  FALSE, TRUE,  "/dev/cfg",  0, NULL },
        { "off live ignored",    PLAN_LIVE_VDB,  PLAN_CFG_VDB,  FALSE, TRUE,  FALSE, TRUE,  "/dev/cfg",  0, NULL },
        { "neither (running)",   PLAN_NONE,      PLAN_NONE,     TRUE,  FALSE, FALSE, FALSE, NULL, G_IO_ERROR_NOT_FOUND,
          "Device not found in live or persistent XML" },
        { "neither (off)",       NULL,           PLAN_NONE,     FALSE, FALSE, FALSE, FALSE, NULL, G_IO_ERROR_NOT_FOUND,
          "Device not found in live or persistent XML" },
    };
    for (guint i = 0; i < G_N_ELEMENTS(rows); i++) {
        const PlanRow *r = &rows[i];
        GError *error = NULL;
        gboolean live = TRUE, cfg = TRUE;
        gchar *dev = NULL;
        gboolean ok = pcv_hotplug_disk_detach_plan(r->live, r->cfg, "vdb", r->active,
                                                   &live, &cfg, &dev, &error);
        g_test_message("row: %s", r->name);
        g_assert_cmpint(ok, ==, r->ok);
        g_assert_cmpint(live, ==, r->want_live);
        g_assert_cmpint(cfg, ==, r->want_cfg);
        if (r->ok) {
            g_assert_no_error(error);
            g_assert_nonnull(dev);
            g_assert_nonnull(strstr(dev, r->want_src));
            g_assert_true(g_str_has_prefix(dev, "<disk"));
            g_free(dev);
        } else {
            g_assert_null(dev);
            g_assert_error(error, G_IO_ERROR, r->err_code);
            g_assert_cmpstr(error->message, ==, r->err_msg);
            g_clear_error(&error);
        }
    }

                                                                             
    {
        GError *error = NULL;
        gboolean live = FALSE, cfg = FALSE;
        gchar *dev = NULL;
        g_assert_true(pcv_hotplug_disk_detach_plan(PLAN_LIVE_SAME, PLAN_CFG_SAME, "vdb", TRUE,
                                                   &live, &cfg, &dev, &error));
        g_assert_no_error(error);
        g_assert_nonnull(dev);
        g_assert_nonnull(strstr(dev, "virtio-disk1"));
        g_free(dev);
    }

                                                  
    const gchar *targets[] = { NULL, "" };
    for (guint i = 0; i < G_N_ELEMENTS(targets); i++) {
        GError *error = NULL;
        gboolean live = TRUE, cfg = TRUE;
        gchar *dev = NULL;
        g_assert_false(pcv_hotplug_disk_detach_plan(PLAN_LIVE_VDB, PLAN_CFG_VDB, targets[i], TRUE,
                                                    &live, &cfg, &dev, &error));
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
        g_assert_null(dev);
        g_clear_error(&error);
    }

                                             
    GError *error = NULL;
    gboolean live = TRUE, cfg = TRUE;
    gchar *dev = NULL;
    g_assert_false(pcv_hotplug_disk_detach_plan(NULL, "<domain><devices><disk>", "vdb", FALSE,
                                                &live, &cfg, &dev, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
}

static void test_detach_plan_source_attrs(void)
{
#define SRC_ATTRS_FMT "<domain><devices><disk type='%s' device='disk'><source %s/>" \
                      "<target dev='vdb' bus='virtio'/></disk></devices></domain>"
    struct { const gchar *a, *b; gboolean same; } cases[] = {
        { "dev='/dev/a'",  "dev='/dev/b'",  FALSE },
        { "file='/a.img'", "file='/b.img'", FALSE },
        { "protocol='rbd' name='p/a'", "protocol='rbd' name='p/b'", FALSE },
        { "dev='/dev/a' file='/x' name='n'", "dev='/dev/a' file='/x' name='n'", TRUE },
    };
    for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
        gchar *a = g_strdup_printf(SRC_ATTRS_FMT, "block", cases[i].a);
        gchar *b = g_strdup_printf(SRC_ATTRS_FMT, "block", cases[i].b);
        GError *error = NULL;
        gboolean live, cfg;
        gchar *dev = NULL;
        gboolean ok = pcv_hotplug_disk_detach_plan(a, b, "vdb", TRUE, &live, &cfg, &dev, &error);
        g_assert_cmpint(ok, ==, cases[i].same);
        if (ok) { g_assert_no_error(error); g_free(dev); }
        else    { g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED); g_clear_error(&error); }
        g_free(a); g_free(b);
    }

                                   
    GError *error = NULL;
    gboolean live = FALSE, cfg = FALSE;
    gchar *dev = NULL;
    g_assert_true(pcv_hotplug_disk_detach_plan(PLAN_LIVE_SAME, PLAN_CFG_SAME, "vdb", TRUE,
                                               &live, &cfg, &dev, &error));
    g_assert_no_error(error);
    g_assert_true(live);
    g_assert_true(cfg);
    g_free(dev);
}

void test_hotplug_disk_xml_register(void)
{
    g_test_add_func("/hotplug/disk_xml/select_second_of_two",
                    test_select_second_of_two);
    g_test_add_func("/hotplug/disk_xml/cdrom_sda", test_cdrom_sda);
    g_test_add_func("/hotplug/disk_xml/non_disk_target_vnet0",
                    test_non_disk_target_vnet0);
    g_test_add_func("/hotplug/disk_xml/prefix_target_exact",
                    test_prefix_target_exact);
    g_test_add_func("/hotplug/disk_xml/disk_without_target_skipped",
                    test_disk_without_target_skipped);
    g_test_add_func("/hotplug/disk_xml/malformed_xml", test_malformed_xml);
    g_test_add_func("/hotplug/disk_xml/null_and_empty_inputs",
                    test_null_and_empty_inputs);
    g_test_add_func("/hotplug/disk_xml/external_entity_not_expanded",
                    test_external_entity_not_expanded);
    g_test_add_func("/hotplug/disk_xml/detach_plan_table",
                    test_detach_plan_table);
    g_test_add_func("/hotplug/disk_xml/detach_plan_source_attrs",
                    test_detach_plan_source_attrs);
}
