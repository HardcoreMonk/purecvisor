   
                       
                                               
  
                           
                                                               
                                          
  
                                                    
  
                                                                  
                                                                    
                                                             
                                                                                      
                                                    
   
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <gio/gio.h>
#include <string.h>

#include "modules/dispatcher/vnc_port_xml.h"

gchar *pcv_vnc_port_from_domain_xml(const gchar *domain_xml, GError **error)
{
    if (!domain_xml || !*domain_xml) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Domain XML is required");
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

    gboolean found_vnc = FALSE;
    gchar *result = NULL;
    xmlNodePtr root = xmlDocGetRootElement(doc);
    for (xmlNodePtr devices = root ? root->children : NULL;
         devices && !found_vnc; devices = devices->next) {
        if (devices->type != XML_ELEMENT_NODE ||
            xmlStrcmp(devices->name, BAD_CAST "devices") != 0)
            continue;

        for (xmlNodePtr gfx = devices->children;
             gfx && !found_vnc; gfx = gfx->next) {
            if (gfx->type != XML_ELEMENT_NODE ||
                xmlStrcmp(gfx->name, BAD_CAST "graphics") != 0)
                continue;

            xmlChar *type = xmlGetProp(gfx, BAD_CAST "type");
            gboolean is_vnc = type && xmlStrcmp(type, BAD_CAST "vnc") == 0;
            xmlFree(type);
            if (!is_vnc)
                continue;

            found_vnc = TRUE;
            xmlChar *port = xmlGetProp(gfx, BAD_CAST "port");
            if (port)
                result = g_strdup((const gchar *)port);
            xmlFree(port);
        }
    }

    xmlFreeDoc(doc);
    if (!result)
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                            found_vnc ? "VNC port not found in XML"
                                      : "VNC Graphics adapter not found in XML");
    return result;
}
