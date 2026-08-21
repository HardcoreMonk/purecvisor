   
                          
                                                       
  
                           
                                                                
                                                   
  
                                                  
                                    
  
                                                            
                                                            
                               
                                                     
                                                                         
                              
                                                        
                                    
   
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <gio/gio.h>
#include <string.h>

#include "modules/dispatcher/hotplug_nic_xml.h"

gchar *pcv_hotplug_select_nic_xml(const gchar *domain_xml,
                                  const gchar *mac,
                                  GError **error)
{
    if (!domain_xml || !*domain_xml || !mac || !*mac) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Domain XML and MAC are required");
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

        for (xmlNodePtr iface = devices->children;
             iface && !result; iface = iface->next) {
            if (iface->type != XML_ELEMENT_NODE ||
                xmlStrcmp(iface->name, BAD_CAST "interface") != 0)
                continue;

            for (xmlNodePtr child = iface->children; child; child = child->next) {
                if (child->type != XML_ELEMENT_NODE ||
                    xmlStrcmp(child->name, BAD_CAST "mac") != 0)
                    continue;

                xmlChar *address = xmlGetProp(child, BAD_CAST "address");
                gboolean matches = address &&
                    g_ascii_strcasecmp((const gchar *)address, mac) == 0;
                xmlFree(address);
                if (!matches)
                    continue;

                xmlBufferPtr buffer = xmlBufferCreate();
                if (buffer && xmlNodeDump(buffer, doc, iface, 0, 1) >= 0)
                    result = g_strdup((const gchar *)xmlBufferContent(buffer));
                if (buffer)
                    xmlBufferFree(buffer);
                break;
            }
        }
    }

    xmlFreeDoc(doc);
    if (!result)
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "NIC with MAC %s was not found in persistent domain XML", mac);
    return result;
}
