









#include "snapshot_compat.h"

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <string.h>

static gboolean
_xml_has_required_invtsc(xmlNodePtr node)
{
    for (xmlNodePtr cur = node; cur; cur = cur->next) {
        if (cur->type == XML_ELEMENT_NODE &&
            xmlStrEqual(cur->name, BAD_CAST "feature")) {
            xmlChar *name = xmlGetProp(cur, BAD_CAST "name");
            xmlChar *policy = xmlGetProp(cur, BAD_CAST "policy");
            gboolean match = name && policy &&
                xmlStrEqual(name, BAD_CAST "invtsc") &&
                xmlStrEqual(policy, BAD_CAST "require");
            xmlFree(name);
            xmlFree(policy);
            if (match) return TRUE;
        }
        if (cur->children && _xml_has_required_invtsc(cur->children))
            return TRUE;
    }
    return FALSE;
}

gboolean
pcv_snapshot_xml_requires_invtsc_offline(const gchar *domain_xml)
{
    if (!domain_xml || !*domain_xml) return FALSE;
    xmlDocPtr doc = xmlReadMemory(domain_xml, (int)strlen(domain_xml),
                                  "domain.xml", NULL,
                                  XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    if (!doc) return FALSE;
    gboolean required = _xml_has_required_invtsc(xmlDocGetRootElement(doc));
    xmlFreeDoc(doc);
    return required;
}
