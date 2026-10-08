



#include <glib.h>
#include <gio/gio.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <string.h>

#include "api/ova_import_xml.h"

static const gchar *BASE_XML =
    "<domain type='kvm'><name>imported</name><devices>"
    "<disk type='file' device='disk'><source file='/dev/zvol/old'/>"
    "<target dev='vda' bus='virtio'/></disk></devices></domain>";

static xmlNodePtr
find_disk(xmlNodePtr node)
{
    for (xmlNodePtr cur = node; cur; cur = cur->next) {
        if (cur->type == XML_ELEMENT_NODE && xmlStrEqual(cur->name, BAD_CAST "disk"))
            return cur;
        xmlNodePtr found = find_disk(cur->children);
        if (found) return found;
    }
    return NULL;
}

static xmlNodePtr
find_child(xmlNodePtr parent, const gchar *name)
{
    for (xmlNodePtr child = parent->children; child; child = child->next)
        if (child->type == XML_ELEMENT_NODE && xmlStrEqual(child->name, BAD_CAST name))
            return child;
    return NULL;
}

static void
assert_prop(xmlNodePtr node, const gchar *name, const gchar *expected)
{
    xmlChar *value = xmlGetProp(node, BAD_CAST name);
    g_assert_nonnull(value);
    g_assert_cmpstr((const gchar *)value, ==, expected);
    xmlFree(value);
}

static void
test_ova_import_zvol_block_raw(void)
{
    GError *error = NULL;
    gchar *normalized = pcv_ova_import_normalize_domain_xml(
        BASE_XML, "/dev/zvol/pcvpool/vms/imported", TRUE, "raw", &error);
    g_assert_no_error(error);
    g_assert_nonnull(normalized);

    xmlDocPtr doc = xmlReadMemory(normalized, (int)strlen(normalized), NULL, NULL, 0);
    xmlNodePtr disk = find_disk(xmlDocGetRootElement(doc));
    xmlNodePtr source = find_child(disk, "source");
    xmlNodePtr driver = find_child(disk, "driver");
    assert_prop(disk, "type", "block");
    assert_prop(source, "dev", "/dev/zvol/pcvpool/vms/imported");
    xmlChar *file_prop = xmlGetProp(source, BAD_CAST "file");
    g_assert_null(file_prop);
    xmlFree(file_prop);
    assert_prop(driver, "name", "qemu");
    assert_prop(driver, "type", "raw");

    xmlFreeDoc(doc);
    g_free(normalized);
}

static void
test_ova_import_qcow2_file(void)
{
    GError *error = NULL;
    gchar *normalized = pcv_ova_import_normalize_domain_xml(
        BASE_XML, "/var/lib/purecvisor/images/imported.qcow2", FALSE, "qcow2", &error);
    g_assert_no_error(error);
    g_assert_nonnull(normalized);

    xmlDocPtr doc = xmlReadMemory(normalized, (int)strlen(normalized), NULL, NULL, 0);
    xmlNodePtr disk = find_disk(xmlDocGetRootElement(doc));
    xmlNodePtr source = find_child(disk, "source");
    xmlNodePtr driver = find_child(disk, "driver");
    assert_prop(disk, "type", "file");
    assert_prop(source, "file", "/var/lib/purecvisor/images/imported.qcow2");
    xmlChar *dev_prop = xmlGetProp(source, BAD_CAST "dev");
    g_assert_null(dev_prop);
    xmlFree(dev_prop);
    assert_prop(driver, "type", "qcow2");

    xmlFreeDoc(doc);
    g_free(normalized);
}

static void
test_ova_import_xml_rejects_invalid_input(void)
{
    GError *error = NULL;
    g_assert_null(pcv_ova_import_normalize_domain_xml(
        "<domain/>", "/tmp/disk", FALSE, "qcow2", &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    g_assert_null(pcv_ova_import_normalize_domain_xml(
        BASE_XML, "relative", FALSE, "qcow2", &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);
}



static void
test_ova_export_disk_with_cdrom(gconstpointer data)
{
    const gchar *disk = "<disk type='block' device='disk'><driver type='raw'/>"
        "<source dev='/dev/zvol/pcvpool/vms/os'/></disk>";
    const gchar *cdrom = "<disk type='file' device='cdrom'><driver type='raw'/>"
        "<source file='/tmp/seed.iso'/></disk>";
    gboolean cd_first = GPOINTER_TO_INT(data);
    gchar *xml = g_strdup_printf("<domain><devices>%s%s</devices></domain>",
                                 cd_first ? cdrom : disk, cd_first ? disk : cdrom);
    gchar *path = NULL, *format = NULL;
    GError *error = NULL;
    g_assert_true(pcv_ova_export_disk_source(xml, &path, &format, &error));
    g_assert_no_error(error);
    g_assert_cmpstr(path, ==, "/dev/zvol/pcvpool/vms/os");
    g_assert_cmpstr(format, ==, "raw");
    g_free(path); g_free(format); g_free(xml);
}

static void
test_ova_export_file_driver_format(void)
{
    const gchar *xml = "<domain><devices><disk type=\"file\" device=\"disk\">"
        "<driver type=\"qcow2\"/><source file=\"/tmp/os&amp;data.image\"/>"
        "</disk><disk type='file' device='disk'><source file='/tmp/second.raw'/>"
        "</disk></devices></domain>";
    gchar *path = NULL, *format = NULL;
    GError *error = NULL;
    g_assert_true(pcv_ova_export_disk_source(xml, &path, &format, &error));
    g_assert_no_error(error);
    g_assert_cmpstr(path, ==, "/tmp/os&data.image");
    g_assert_cmpstr(format, ==, "qcow2");
    g_free(path); g_free(format);
}

static void
test_ova_export_rejects_missing_data_disk(void)
{
    const gchar *cases[] = {
        "<domain><devices><disk type='file' device='cdrom'><source file='/tmp/cd.iso'/>"
            "</disk></devices></domain>",
        "<domain><devices><disk type='block' device='disk'/></devices></domain>",
        "<domain><devices><disk type='network' device='disk'><source name='volume'/>"
            "</disk></devices></domain>",
        "<domain><devices><disk type='file' device='disk'><source file='relative'/>"
            "</disk></devices></domain>",
        "<domain><devices>", NULL
    };
    for (gsize i = 0; i < G_N_ELEMENTS(cases); i++) {
        gchar *path = NULL, *format = NULL;
        GError *error = NULL;
        g_assert_false(pcv_ova_export_disk_source(cases[i], &path, &format, &error));
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
        g_assert_null(path); g_assert_null(format);
        g_clear_error(&error);
    }
}

static void
test_ova_export_format_fallback(void)
{
    gchar *path = NULL, *format = NULL;
    GError *error = NULL;
    g_assert_true(pcv_ova_export_disk_source(BASE_XML, &path, &format, &error));
    g_assert_no_error(error);
    g_assert_cmpstr(format, ==, "raw");
    g_free(path); g_free(format);
}



static void
test_ova_import_root_virtual_size(void)
{
    const gchar *json = "{\"children\":[{\"info\":{\"virtual-size\":2675834880}}],"
        "\"virtual-size\":10737418240}";
    g_assert_cmpint(pcv_ova_import_virtual_size(json), ==, 10737418240LL);
    g_assert_cmpint(pcv_ova_import_virtual_size("{\"virtual-size\":1048576}"), ==, 1048576);
}

static void
test_ova_import_invalid_virtual_size(void)
{
    const gchar *cases[] = {NULL, "invalid", "[]", "null", "{}",
        "{\"children\":[{\"info\":{\"virtual-size\":10737418240}}]}",
        "{\"virtual-size\":null}", "{\"virtual-size\":\"10737418240\"}",
        "{\"virtual-size\":-1}", "{\"virtual-size\":1}",
        "{\"virtual-size\":10737418240.5}"};
    for (gsize i = 0; i < G_N_ELEMENTS(cases); i++)
        g_assert_cmpint(pcv_ova_import_virtual_size(cases[i]), ==, 0);
}

void
test_ova_import_xml_register(void)
{
    g_test_add_func("/ova/import_xml/zvol_block_raw", test_ova_import_zvol_block_raw);
    g_test_add_func("/ova/import_xml/qcow2_file", test_ova_import_qcow2_file);
    g_test_add_func("/ova/import_xml/invalid", test_ova_import_xml_rejects_invalid_input);
    g_test_add_func("/ova/import_xml/root_virtual_size", test_ova_import_root_virtual_size);
    g_test_add_func("/ova/import_xml/invalid_virtual_size", test_ova_import_invalid_virtual_size);
    g_test_add_data_func("/ova/export_xml/cdrom_first", GINT_TO_POINTER(1), test_ova_export_disk_with_cdrom);
    g_test_add_data_func("/ova/export_xml/cdrom_last", GINT_TO_POINTER(0), test_ova_export_disk_with_cdrom);
    g_test_add_func("/ova/export_xml/file_driver_format", test_ova_export_file_driver_format);
    g_test_add_func("/ova/export_xml/invalid", test_ova_export_rejects_missing_data_disk);
    g_test_add_func("/ova/export_xml/format_fallback", test_ova_export_format_fallback);
}
