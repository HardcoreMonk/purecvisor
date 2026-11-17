                                                                             
                              
                                                                                                                        
  
                           
                                                                                                     
                                                                                                        
                                                                    
                                                                        
                                                                              
                                                                                               
  
                       
                                                                                 
                                                                                
#include "modules/virt/vm_owner.h"

#include <libxml/parser.h>
#include <string.h>

                                                                   
static gboolean
_is_pcv_owner(const xmlNode *n)
{
    return n && n->type == XML_ELEMENT_NODE && n->name && xmlStrEqual(n->name, BAD_CAST "owner") &&
           n->ns && n->ns->href && xmlStrEqual(n->ns->href, BAD_CAST PCV_VM_OWNER_NS_URI);
}

                                                              
static gboolean
_is_plain(const xmlNode *n, const gchar *name)
{
    return n && n->type == XML_ELEMENT_NODE && !n->ns && n->name && xmlStrEqual(n->name, BAD_CAST name);
}

                                                                          
static gboolean
_is_ws(xmlChar c)
{
    return c == 0x09 || c == 0x0A || c == 0x0C || c == 0x0D || c == 0x20;
}

                                                              
                                            
static gchar *
_owner_value(xmlNode *n)
{
    for (xmlNode *c = n->children; c; c = c->next) {
        if (c->type != XML_TEXT_NODE && c->type != XML_CDATA_SECTION_NODE)
            return NULL;                                        
    }
    xmlChar *content = xmlNodeGetContent(n);
    if (!content)
        return NULL;
    gchar *value = NULL;
    gsize len = strlen((const gchar *)content);
    if (len > 0 && !_is_ws(content[0]) && !_is_ws(content[len - 1]))
        value = g_strdup((const gchar *)content);
    xmlFree(content);
    return value;
}

                         
static gboolean
_has_dtd(const xmlNode *n)
{
    const xmlDoc *doc = n ? n->doc : NULL;
    return doc && (doc->intSubset || doc->extSubset);
}

                                                            
static xmlNode *
_metadata_owner(xmlNode *metadata)
{
    xmlNode *found = NULL;
    for (xmlNode *c = metadata->children; c; c = c->next) {
        if (!_is_pcv_owner(c))
            continue;
        if (found)
            return NULL;                    
        found = c;
    }
    return found;
}

                                                                   
gchar *
pcv_vm_owner_from_domain_node(xmlNode *root)
{
    if (!_is_plain(root, "domain") || _has_dtd(root))
        return NULL;
    xmlNode *metadata = NULL;
    for (xmlNode *c = root->children; c; c = c->next) {
        if (!_is_plain(c, "metadata"))
            continue;
        if (metadata)
            return NULL;                                                 
        metadata = c;
    }
    xmlNode *owner = metadata ? _metadata_owner(metadata) : NULL;
    return owner ? _owner_value(owner) : NULL;
}

                                               
static xmlDocPtr
_parse(const gchar *xml, const gchar *url)
{
    if (!xml || !*xml)
        return NULL;
    return xmlReadMemory(xml, (int)strlen(xml), url, NULL, XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
}

                                                             
gchar *
pcv_vm_owner_from_domain_xml(const gchar *xml)
{
    xmlDocPtr doc = _parse(xml, "pcv-vm-owner.xml");
    if (!doc)
        return NULL;
    gchar *owner = pcv_vm_owner_from_domain_node(xmlDocGetRootElement(doc));
    xmlFreeDoc(doc);
    return owner;
}
