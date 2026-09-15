



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

void
test_ova_import_xml_register(void)
{
    g_test_add_func("/ova/import_xml/zvol_block_raw", test_ova_import_zvol_block_raw);
    g_test_add_func("/ova/import_xml/qcow2_file", test_ova_import_qcow2_file);
    g_test_add_func("/ova/import_xml/invalid", test_ova_import_xml_rejects_invalid_input);
}
