                                                                             
                                      
                                                                     
  
                           
                                                   
                                                    
                                        
  
                                                                 
                                                       
                                                                            
                                                 
  
                                                           
                                   
                                                                                

#include "disk_image_guard.h"
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <string.h>
#include "../../utils/pcv_spawn.h"

                                                                
                                 
static gboolean
_refuse(GError **error, const gchar *path, const gchar *reason)
{
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Disk image '%s' refused: %s", path, reason);
    return FALSE;
}

                                                                                                  
                                                                 
static gboolean
_vmdk_is_self_contained(JsonObject *data, const gchar *path, GError **error)
{
    const gchar *type = json_object_get_string_member_with_default(data, "create-type", "");
    if (g_strcmp0(type, "monolithicSparse") != 0 && g_strcmp0(type, "streamOptimized") != 0)
        return _refuse(error, path, "vmdk create-type must be monolithicSparse or streamOptimized");
    JsonNode *node = json_object_get_member(data, "extents");
    JsonArray *extents = node && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
    if (!extents || json_array_get_length(extents) == 0)
        return _refuse(error, path, "vmdk extent list is missing");
    for (guint i = 0; i < json_array_get_length(extents); i++) {
        JsonNode *item = json_array_get_element(extents, i);
        const gchar *file = JSON_NODE_HOLDS_OBJECT(item)
            ? json_object_get_string_member_with_default(json_node_get_object(item), "filename", NULL) : NULL;
        if (g_strcmp0(file, path) != 0)
            return _refuse(error, path, "vmdk refers to an external extent");
    }
    return TRUE;
}

                                                                                       
static JsonObject *
_format_specific_data(JsonObject *root)
{
    JsonNode *spec = json_object_get_member(root, "format-specific");
    JsonObject *spec_obj = spec && JSON_NODE_HOLDS_OBJECT(spec) ? json_node_get_object(spec) : NULL;
    JsonNode *data = spec_obj ? json_object_get_member(spec_obj, "data") : NULL;
    return data && JSON_NODE_HOLDS_OBJECT(data) ? json_node_get_object(data) : NULL;
}

                                                                     
gboolean
pcv_disk_image_judge_info(const gchar *info_json, const gchar *path, PcvDiskImageKind kind,
                          gchar **format_out, GError **error)
{
    *format_out = NULL;
    g_autoptr(JsonParser) parser = json_parser_new();
    if (!info_json || !json_parser_load_from_data(parser, info_json, -1, NULL))
        return _refuse(error, path, "qemu-img info output is not JSON");
    JsonNode *root_node = json_parser_get_root(parser);
    if (!root_node || !JSON_NODE_HOLDS_OBJECT(root_node))
        return _refuse(error, path, "qemu-img info output is not an object");
    JsonObject *root = json_node_get_object(root_node);
    if (json_object_has_member(root, "backing-filename") || json_object_has_member(root, "full-backing-filename"))
        return _refuse(error, path, "image has a backing file");
    JsonObject *data = _format_specific_data(root);
    if (data && json_object_has_member(data, "data-file"))
        return _refuse(error, path, "image has an external data file");

    const gchar *format = json_object_get_string_member_with_default(root, "format", NULL);
    if (!format)
        return _refuse(error, path, "qemu-img info did not report a format");
    if (kind == PCV_DISK_IMAGE_OVA_VMDK) {
        if (g_strcmp0(format, "vmdk") != 0)
            return _refuse(error, path, "OVA disk is not a vmdk");
        if (!data)
            return _refuse(error, path, "vmdk details are missing");
        if (!_vmdk_is_self_contained(data, path, error))
            return FALSE;
    } else if (g_strcmp0(format, "raw") != 0 && g_strcmp0(format, "qcow2") != 0 && g_strcmp0(format, "qcow") != 0) {
        return _refuse(error, path, "format must be raw, qcow2, or qcow");
    }
    *format_out = g_strdup(format);
    return TRUE;
}

                                                                    
gboolean
pcv_disk_image_inspect(const gchar *path, PcvDiskImageKind kind, gchar **format_out, GError **error)
{
    *format_out = NULL;
    const gchar *argv[] = {"qemu-img", "info", "--output=json", "--force-share", path, NULL};
    g_autofree gchar *out = NULL;
    g_autofree gchar *err = NULL;
    GError *spawn_error = NULL;
    if (!pcv_spawn_sync_timeout(argv, &out, &err, PCV_DISK_IMAGE_INFO_TIMEOUT_SEC, &spawn_error)) {
        g_set_error(error, G_IO_ERROR, spawn_error ? spawn_error->code : G_IO_ERROR_FAILED,
                    "Disk image '%s' could not be inspected: %s", path,
                    spawn_error ? spawn_error->message : (err && *err ? g_strstrip(err) : "unknown qemu-img error"));
        g_clear_error(&spawn_error);
        return FALSE;
    }
    return pcv_disk_image_judge_info(out, path, kind, format_out, error);
}

                                                
gboolean
pcv_disk_image_recheck(const gchar *path, const gchar *expected_format, GError **error)
{
    g_autofree gchar *format = NULL;
    if (!pcv_disk_image_inspect(path, PCV_DISK_IMAGE_BASE, &format, error))
        return FALSE;
    if (g_strcmp0(format, expected_format) == 0)
        return TRUE;
    return _refuse(error, path, "image format changed after inspection");
}

                                                                       
gboolean
pcv_disk_image_convert_into(const gchar *source, const gchar *source_format,
                            const gchar *target_format, const gchar *target, GError **error)
{
    const gchar *argv[] = {"qemu-img", "convert", "-n", "-f", source_format, "-O", target_format,
                           source, target, NULL};
    g_autofree gchar *err = NULL;
    GError *spawn_error = NULL;
    if (pcv_spawn_sync(argv, NULL, &err, &spawn_error))
        return TRUE;
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        spawn_error ? spawn_error->message
                                    : (err && *err ? g_strstrip(err) : "unknown qemu-img error"));
    g_clear_error(&spawn_error);
    return FALSE;
}

                                                              
static xmlNodePtr
_xml_child(xmlNodePtr parent, const gchar *name)
{
    for (xmlNodePtr child = parent ? parent->children : NULL; child; child = child->next) {
        if (child->type == XML_ELEMENT_NODE && xmlStrEqual(child->name, BAD_CAST name))
            return child;
    }
    return NULL;
}

                                                           
gchar *
pcv_disk_image_driver_format(const gchar *domain_xml, const gchar *target_dev)
{
    if (!domain_xml || !target_dev || !*target_dev)
        return NULL;
    xmlDocPtr doc = xmlReadMemory(domain_xml, (int)strlen(domain_xml), "domain.xml", NULL,
                                  XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    if (!doc)
        return NULL;
    gchar *format = NULL;
    xmlNodePtr devices = _xml_child(xmlDocGetRootElement(doc), "devices");
    for (xmlNodePtr disk = devices ? devices->children : NULL; disk && !format; disk = disk->next) {
        if (disk->type != XML_ELEMENT_NODE || !xmlStrEqual(disk->name, BAD_CAST "disk"))
            continue;
        xmlNodePtr target = _xml_child(disk, "target");
        xmlChar *dev = target ? xmlGetProp(target, BAD_CAST "dev") : NULL;
        const gboolean same = dev && xmlStrEqual(dev, BAD_CAST target_dev);
        xmlFree(dev);
        if (!same)
            continue;
        xmlNodePtr driver = _xml_child(disk, "driver");
        xmlChar *type = driver ? xmlGetProp(driver, BAD_CAST "type") : NULL;
        format = type && type[0] ? g_strdup((const gchar *)type) : NULL;
        xmlFree(type);
        break;
    }
    xmlFreeDoc(doc);
    return format;
}

                                                    
gboolean
pcv_disk_image_resize_file(const gchar *path, const gchar *format, gint size_gb, GError **error)
{
    if (g_strcmp0(format, "raw") != 0 && g_strcmp0(format, "qcow2") != 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "Disk '%s' has driver type '%s'; resize needs raw or qcow2", path, format ? format : "(none)");
        return FALSE;
    }
    g_autofree gchar *size_str = g_strdup_printf("%dG", size_gb);
    const gchar *argv[] = {"qemu-img", "resize", "-f", format, path, size_str, NULL};
    g_autofree gchar *err = NULL;
    GError *spawn_error = NULL;
    if (pcv_spawn_sync(argv, NULL, &err, &spawn_error))
        return TRUE;
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        spawn_error ? spawn_error->message
                                    : (err && *err ? g_strstrip(err) : "unknown qemu-img error"));
    g_clear_error(&spawn_error);
    return FALSE;
}
