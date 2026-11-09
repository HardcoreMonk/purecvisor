   
                           
                                                  
  
                           
                                                                    
                                                      
  
                                                        
  
                                                                  
                                                   
                               
                                                          
                                                                         
                              
                                                          
   
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <gio/gio.h>
#include <string.h>

#include "modules/dispatcher/hotplug_disk_xml.h"

gchar *pcv_hotplug_select_disk_xml(const gchar *domain_xml,
                                   const gchar *target,
                                   GError **error)
{
    if (!domain_xml || !*domain_xml || !target || !*target) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Domain XML and target are required");
        return NULL;
    }

    xmlDocPtr doc = xmlReadMemory(domain_xml, (int)strlen(domain_xml),
                                  "domain.xml", NULL,
                                  XML_PARSE_NONET | XML_PARSE_NOBLANKS |
                                  XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    if (!doc) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Failed to parse domain XML");
        return NULL;
    }

    gchar *result = NULL;
    xmlNodePtr root = xmlDocGetRootElement(doc);
    for (xmlNodePtr devices = root ? root->children : NULL;
         devices && !result; devices = devices->next) {
        if (devices->type != XML_ELEMENT_NODE ||
            xmlStrcmp(devices->name, BAD_CAST "devices") != 0)
            continue;

        for (xmlNodePtr disk = devices->children;
             disk && !result; disk = disk->next) {
            if (disk->type != XML_ELEMENT_NODE ||
                xmlStrcmp(disk->name, BAD_CAST "disk") != 0)
                continue;

            for (xmlNodePtr child = disk->children; child; child = child->next) {
                if (child->type != XML_ELEMENT_NODE ||
                    xmlStrcmp(child->name, BAD_CAST "target") != 0)
                    continue;

                xmlChar *dev = xmlGetProp(child, BAD_CAST "dev");
                gboolean matches = dev &&
                    xmlStrcmp(dev, (const xmlChar *)target) == 0;
                xmlFree(dev);
                if (!matches)
                    continue;

                xmlBufferPtr buffer = xmlBufferCreate();
                if (buffer && xmlNodeDump(buffer, doc, disk, 0, 1) >= 0)
                    result = g_strdup((const gchar *)xmlBufferContent(buffer));
                if (buffer)
                    xmlBufferFree(buffer);
                break;
            }
        }
    }

    xmlFreeDoc(doc);
    if (!result)
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                            "Device not found in live XML");
    return result;
}

                                                                                       
static void disk_source_attrs(const gchar *disk_xml, xmlChar *attrs[3])
{
    static const char *const names[3] = { "dev", "file", "name" };
    attrs[0] = attrs[1] = attrs[2] = NULL;
    xmlDocPtr doc = xmlReadMemory(disk_xml, (int)strlen(disk_xml), "disk.xml", NULL,
                                  XML_PARSE_NONET | XML_PARSE_NOBLANKS |
                                  XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    if (!doc)
        return;
    xmlNodePtr root = xmlDocGetRootElement(doc);
    for (xmlNodePtr c = root ? root->children : NULL; c; c = c->next) {
        if (c->type != XML_ELEMENT_NODE || xmlStrcmp(c->name, BAD_CAST "source") != 0)
            continue;
        for (int i = 0; i < 3; i++)
            attrs[i] = xmlGetProp(c, BAD_CAST names[i]);
        break;
    }
    xmlFreeDoc(doc);
}

                                                                     
static gboolean disk_sources_equal(const gchar *a_xml, const gchar *b_xml)
{
    xmlChar *a[3], *b[3];
    gboolean same = TRUE;
    disk_source_attrs(a_xml, a);
    disk_source_attrs(b_xml, b);
    for (int i = 0; i < 3; i++) {
        if (g_strcmp0((const gchar *)a[i], (const gchar *)b[i]) != 0)
            same = FALSE;
        xmlFree(a[i]);
        xmlFree(b[i]);
    }
    return same;
}

                                                                                  
static gboolean select_optional(const gchar *xml, const gchar *target,
                                gchar **out, GError **error)
{
    GError *local = NULL;
    *out = pcv_hotplug_select_disk_xml(xml, target, &local);
    if (*out)
        return TRUE;
    if (g_error_matches(local, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
        g_clear_error(&local);
        return TRUE;
    }
    g_propagate_error(error, local);
    return FALSE;
}

gboolean pcv_hotplug_disk_detach_plan(const gchar *live_xml,
                                      const gchar *config_xml,
                                      const gchar *target,
                                      gboolean is_active,
                                      gboolean *live_out,
                                      gboolean *config_out,
                                      gchar **device_xml_out,
                                      GError **error)
{
    g_autofree gchar *live_blk = NULL;
    g_autofree gchar *config_blk = NULL;

    *live_out = *config_out = FALSE;
    *device_xml_out = NULL;

    if (!select_optional(config_xml, target, &config_blk, error))
        return FALSE;
    if (is_active && !select_optional(live_xml, target, &live_blk, error))
        return FALSE;

    if (!live_blk && !config_blk) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                            "Device not found in live or persistent XML");
        return FALSE;
    }
    if (live_blk && config_blk && !disk_sources_equal(live_blk, config_blk)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Disk '%s' differs between the running VM and its persistent "
                    "definition; nothing was changed", target);
        return FALSE;
    }

    *live_out = live_blk != NULL;
    *config_out = config_blk != NULL;
                                                                 
    if (live_blk)
        *device_xml_out = g_steal_pointer(&live_blk);
    else
        *device_xml_out = g_steal_pointer(&config_blk);
    return TRUE;
}
