













#include "ova_import_xml.h"

#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <string.h>





gint64
pcv_ova_import_virtual_size(const gchar *info_json)
{
    if (!info_json) return 0;
    JsonParser *parser = json_parser_new();
    gint64 bytes = 0;
    if (json_parser_load_from_data(parser, info_json, -1, NULL)) {
        JsonNode *root = json_parser_get_root(parser);
        if (root && JSON_NODE_HOLDS_OBJECT(root)) {
            JsonNode *size = json_object_get_member(json_node_get_object(root), "virtual-size");
            if (size && JSON_NODE_HOLDS_VALUE(size) && json_node_get_value_type(size) == G_TYPE_INT64) {
                bytes = json_node_get_int(size);
                if (bytes < 1024 * 1024) bytes = 0;
            }
        }
    }
    g_object_unref(parser);
    return bytes;
}

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





gboolean
pcv_ova_export_disk_source(const gchar *domain_xml, gchar **disk_path,
                           gchar **format, GError **error)
{
    g_return_val_if_fail(disk_path && format, FALSE);
    *disk_path = NULL;
    *format = NULL;
    xmlDocPtr doc = domain_xml ? xmlReadMemory(domain_xml, (int)strlen(domain_xml),
        "ova-export.xml", NULL, XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING) : NULL;
    xmlNodePtr devices = doc ? _child_named(xmlDocGetRootElement(doc), "devices") : NULL;
    xmlNodePtr disk = _first_domain_disk(devices ? devices->children : NULL);
    xmlNodePtr source = _child_named(disk, "source");
    xmlNodePtr driver = _child_named(disk, "driver");
    xmlChar *type = disk ? xmlGetProp(disk, BAD_CAST "type") : NULL;
    const gchar *attribute = type && xmlStrEqual(type, BAD_CAST "block") ? "dev"
        : type && xmlStrEqual(type, BAD_CAST "file") ? "file" : NULL;
    xmlChar *path = source && attribute ? xmlGetProp(source, BAD_CAST attribute) : NULL;
    xmlChar *driver_format = driver ? xmlGetProp(driver, BAD_CAST "type") : NULL;
    gboolean ok = path && path[0] == '/';
    if (ok) {
        *disk_path = g_strdup((const gchar *)path);
        *format = driver_format && driver_format[0] ? g_strdup((const gchar *)driver_format)
            : g_strdup(g_str_has_suffix(*disk_path, ".qcow2") ? "qcow2" : "raw");
    } else {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "OVA export requires a local file/block data disk source");
    }
    xmlFree(driver_format);
    xmlFree(path);
    xmlFree(type);
    xmlFreeDoc(doc);
    return ok;
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
