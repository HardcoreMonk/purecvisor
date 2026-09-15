










#include "ova_import_xml.h"

#include <gio/gio.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <string.h>

static xmlNodePtr
_first_domain_disk(xmlNodePtr node)
{
    for (xmlNodePtr cur = node; cur; cur = cur->next) {
        if (cur->type == XML_ELEMENT_NODE && xmlStrEqual(cur->name, BAD_CAST "disk")) {
            xmlChar *device = xmlGetProp(cur, BAD_CAST "device");
            gboolean match = device && xmlStrEqual(device, BAD_CAST "disk");
            xmlFree(device);
            if (match) return cur;
        }
        if (cur->children) {
            xmlNodePtr found = _first_domain_disk(cur->children);
            if (found) return found;
        }
    }
    return NULL;
}

static xmlNodePtr
_child_named(xmlNodePtr parent, const gchar *name)
{
    for (xmlNodePtr child = parent ? parent->children : NULL; child; child = child->next) {
        if (child->type == XML_ELEMENT_NODE &&
            xmlStrEqual(child->name, BAD_CAST name))
            return child;
    }
    return NULL;
}

gchar *
pcv_ova_import_normalize_domain_xml(const gchar *domain_xml,
                                    const gchar *disk_path,
                                    gboolean block_device,
                                    const gchar *format,
                                    GError **error)
{
    if (!domain_xml || !*domain_xml || !disk_path || disk_path[0] != '/' ||
        !format || !*format) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "OVA domain XML, absolute disk_path and format are required");
        return NULL;
    }

    xmlDocPtr doc = xmlReadMemory(domain_xml, (int)strlen(domain_xml),
                                  "ova-domain.xml", NULL,
                                  XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    if (!doc) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "virt-install returned malformed domain XML");
        return NULL;
    }

    xmlNodePtr disk = _first_domain_disk(xmlDocGetRootElement(doc));
    if (!disk) {
        xmlFreeDoc(doc);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "virt-install domain XML has no device=disk node");
        return NULL;
    }

    xmlSetProp(disk, BAD_CAST "type", BAD_CAST (block_device ? "block" : "file"));
    xmlSetProp(disk, BAD_CAST "device", BAD_CAST "disk");

    xmlNodePtr source = _child_named(disk, "source");
    if (!source) source = xmlNewChild(disk, NULL, BAD_CAST "source", NULL);
    xmlUnsetProp(source, BAD_CAST "file");
    xmlUnsetProp(source, BAD_CAST "dev");
    xmlUnsetProp(source, BAD_CAST "name");
    xmlSetProp(source, BAD_CAST (block_device ? "dev" : "file"), BAD_CAST disk_path);

    xmlNodePtr driver = _child_named(disk, "driver");
    if (!driver) {
        driver = xmlNewNode(NULL, BAD_CAST "driver");
        xmlAddPrevSibling(source, driver);
    }
    xmlSetProp(driver, BAD_CAST "name", BAD_CAST "qemu");
    xmlSetProp(driver, BAD_CAST "type", BAD_CAST format);

    xmlChar *serialized = NULL;
    int serialized_len = 0;
    xmlDocDumpFormatMemoryEnc(doc, &serialized, &serialized_len, "UTF-8", 1);
    gchar *result = serialized && serialized_len > 0
        ? g_strndup((const gchar *)serialized, (gsize)serialized_len) : NULL;
    xmlFree(serialized);
    xmlFreeDoc(doc);

    if (!result) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "failed to serialize normalized OVA domain XML");
    }
    return result;
}
