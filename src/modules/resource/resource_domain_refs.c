                                                                             
                                              
                                                                                                 
  
                           
                                                                                                          
                                                                   
                                                                                           
                                                                                    
                                                           
  
                                                                         
                                                                                
#include "modules/resource/resource_domain_refs.h"

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <string.h>

                     
static gboolean
node_is(const xmlNode *n, const char *name)
{
    return n && n->type == XML_ELEMENT_NODE && n->name && xmlStrcmp(n->name, BAD_CAST name) == 0;
}

                                   
static gchar *
attr_dup(xmlNode *n, const char *name)
{
    xmlChar *v = xmlGetProp(n, BAD_CAST name);
    if (!v)
        return NULL;
    gchar *s = g_strdup((const gchar *)v);
    xmlFree(v);
    return s;
}

                                   
static xmlNode *
child_named(xmlNode *parent, const char *name)
{
    for (xmlNode *c = parent ? parent->children : NULL; c; c = c->next)
        if (node_is(c, name))
            return c;
    return NULL;
}

static void
ref_free(gpointer p)
{
    PcvResRef *r = p;
    if (!r)
        return;
    g_free(r->ref);
    g_free(r);
}

                            
static gboolean
has_ref(const GPtrArray *refs, PcvResKind kind, const gchar *ref)
{
    for (guint i = 0; i < refs->len; i++) {
        const PcvResRef *r = g_ptr_array_index((GPtrArray *)refs, i);
        if (r->kind == kind && g_strcmp0(r->ref, ref) == 0)
            return TRUE;
    }
    return FALSE;
}

                                           
static void
add_ref(PcvResDomainRefs *out, PcvResKind kind, const gchar *ref)
{
    if (!ref || !*ref || has_ref(out->refs, kind, ref))
        return;
    PcvResRef *r = g_new0(PcvResRef, 1);
    r->kind = kind;
    r->ref = g_strdup(ref);
    g_ptr_array_add(out->refs, r);
}

                                                         
static void
add_normalized(PcvResDomainRefs *out, PcvResKind kind, const gchar *value)
{
    PcvResNorm n;
    if (pcv_res_normalize(kind, value, &n, NULL))
        add_ref(out, kind, n.ref);
    pcv_res_norm_clear(&n);
}

                                                                                                 
static void
collect_disk(xmlNode *disk, PcvResDomainRefs *out)
{
    g_autofree gchar *device = attr_dup(disk, "device");
    xmlNode *src = child_named(disk, "source");
    if (!src)
        return;
    g_autofree gchar *file = attr_dup(src, "file");
    g_autofree gchar *dev = attr_dup(src, "dev");
    gboolean is_cdrom = device && g_strcmp0(device, "cdrom") == 0;
    gboolean is_disk = !device || g_strcmp0(device, "disk") == 0;

    if (is_cdrom && file && file[0] == '/') {
        add_ref(out, PCV_RES_ISO, file);
        return;
    }
    if (!is_disk)
        return;
    if (file && file[0] == '/') {
        add_ref(out, PCV_RES_IMAGE, file);
        g_autofree gchar *dir = g_path_get_dirname(file);
        add_ref(out, PCV_RES_STORAGE, dir);
    } else if (dev && g_str_has_prefix(dev, "/dev/zvol/")) {
        const gchar *rest = dev + strlen("/dev/zvol/");
        const gchar *slash = strrchr(rest, '/');
        if (slash && slash > rest) {
            g_autofree gchar *ds = g_strndup(rest, (gsize)(slash - rest));
            add_normalized(out, PCV_RES_STORAGE, ds);
        }
    }
}

                                        
static gboolean
hex_attr(xmlNode *n, const char *name, guint64 *v)
{
    g_autofree gchar *s = attr_dup(n, name);
    if (!s || !*s)
        return FALSE;
    gchar *end = NULL;
    *v = g_ascii_strtoull(s, &end, 16);
    return end && *end == '\0';
}

                                                
static void
collect_hostdev(xmlNode *hd, PcvResDomainRefs *out)
{
    g_autofree gchar *type = attr_dup(hd, "type");
    xmlNode *src = child_named(hd, "source");
    if (!type || !src)
        return;
    if (g_strcmp0(type, "pci") == 0) {
        xmlNode *addr = child_named(src, "address");
        guint64 dom, bus, slot, fn;
        if (addr && hex_attr(addr, "domain", &dom) && hex_attr(addr, "bus", &bus) &&
            hex_attr(addr, "slot", &slot) && hex_attr(addr, "function", &fn)) {
            g_autofree gchar *v = g_strdup_printf("%04x:%02x:%02x.%x", (guint)dom, (guint)bus, (guint)slot, (guint)fn);
            add_normalized(out, PCV_RES_PCI, v);
        }
    } else if (g_strcmp0(type, "usb") == 0) {
        xmlNode *vendor = child_named(src, "vendor");
        xmlNode *product = child_named(src, "product");
        guint64 vid, pid;
        if (vendor && product && hex_attr(vendor, "id", &vid) && hex_attr(product, "id", &pid)) {
            g_autofree gchar *v = g_strdup_printf("%04x:%04x", (guint)vid, (guint)pid);
            add_normalized(out, PCV_RES_USB, v);
        }
    }
}

                                                                             
static void
collect_interface(xmlNode *ifc, PcvResDomainRefs *out)
{
    g_autofree gchar *type = attr_dup(ifc, "type");
    if (g_strcmp0(type, "bridge") != 0)
        return;
    xmlNode *src = child_named(ifc, "source");
    g_autofree gchar *br = src ? attr_dup(src, "bridge") : NULL;
    add_ref(out, PCV_RES_BRIDGE, br);
}

                                                    
static gchar *
find_owner(xmlNode *node)
{
    for (xmlNode *cur = node; cur; cur = cur->next) {
        if (node_is(cur, "owner")) {
            xmlChar *content = xmlNodeGetContent(cur);
            if (!content)
                return NULL;
            gchar *owner = g_strdup((const gchar *)content);
            xmlFree(content);
            g_strstrip(owner);
            if (*owner)
                return owner;
            g_free(owner);
            return NULL;
        }
        gchar *child = find_owner(cur->children);
        if (child)
            return child;
    }
    return NULL;
}

                                                                                
gboolean
pcv_res_domain_refs_parse(const gchar *xml, PcvResDomainRefs *out, GError **error)
{
    g_return_val_if_fail(out != NULL, FALSE);
    out->owner = NULL;
    out->refs = g_ptr_array_new_with_free_func(ref_free);
    if (!xml || !*xml) {
        g_set_error_literal(error, PCV_RES_ERROR, PCV_RES_ERROR_INVALID, "empty domain xml");
        return FALSE;
    }
                                                                   
    xmlDocPtr doc = xmlReadMemory(xml, (int)strlen(xml), "pcv-domain-refs.xml", NULL,
                                  XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    xmlNode *root = doc ? xmlDocGetRootElement(doc) : NULL;
    if (!root) {
        if (doc)
            xmlFreeDoc(doc);
        g_set_error_literal(error, PCV_RES_ERROR, PCV_RES_ERROR_INVALID, "domain xml is not parseable");
        return FALSE;
    }
    out->owner = find_owner(root);
    xmlNode *devices = child_named(root, "devices");
    for (xmlNode *d = devices ? devices->children : NULL; d; d = d->next) {
        if (node_is(d, "disk"))
            collect_disk(d, out);
        else if (node_is(d, "hostdev"))
            collect_hostdev(d, out);
        else if (node_is(d, "interface"))
            collect_interface(d, out);
    }
    xmlFreeDoc(doc);
    return TRUE;
}

                                                                         
void
pcv_res_domain_refs_merge(PcvResDomainRefs *into, const PcvResDomainRefs *from)
{
    g_return_if_fail(into != NULL);
    if (!into->refs)
        into->refs = g_ptr_array_new_with_free_func(ref_free);
    if (!into->owner && from && from->owner)
        into->owner = g_strdup(from->owner);
    if (!from || !from->refs)
        return;
    for (guint i = 0; i < from->refs->len; i++) {
        const PcvResRef *r = g_ptr_array_index(from->refs, i);
        add_ref(into, r->kind, r->ref);
    }
}

                                 
void
pcv_res_domain_refs_clear(PcvResDomainRefs *r)
{
    if (!r)
        return;
    g_free(r->owner);
    r->owner = NULL;
    if (r->refs)
        g_ptr_array_unref(r->refs);
    r->refs = NULL;
}

                                                                          
gboolean
pcv_res_domain_refs_uses(const PcvResDomainRefs *r, PcvResKind kind, const gchar *item_ref)
{
    if (!r || !r->refs || !item_ref || !*item_ref)
        return FALSE;
    gsize ilen = strlen(item_ref);
    for (guint i = 0; i < r->refs->len; i++) {
        const PcvResRef *x = g_ptr_array_index(r->refs, i);
        if (x->kind != kind)
            continue;
        if (g_strcmp0(x->ref, item_ref) == 0)
            return TRUE;
        if (kind == PCV_RES_STORAGE) {
                                                
            gboolean sep = item_ref[ilen - 1] == '/';
            if (g_str_has_prefix(x->ref, item_ref) && (sep || x->ref[ilen] == '/'))
                return TRUE;
        }
    }
    return FALSE;
}

                                                                                             
                                                                      
void
pcv_res_domain_refs_canonicalize(PcvResDomainRefs *r)
{
    if (!r || !r->refs)
        return;
    GPtrArray *old = r->refs;
    r->refs = g_ptr_array_new_with_free_func(ref_free);
    for (guint i = 0; i < old->len; i++) {
        const PcvResRef *x = g_ptr_array_index(old, i);
        gboolean path_kind = x->kind == PCV_RES_IMAGE || x->kind == PCV_RES_ISO ||
                             (x->kind == PCV_RES_STORAGE && x->ref[0] == '/');
        PcvResNorm n = { 0 };
        if (path_kind && pcv_res_normalize(x->kind, x->ref, &n, NULL)) {
            add_ref(r, x->kind, n.ref);
            if (x->kind == PCV_RES_IMAGE) {                                                           
                g_autofree gchar *real_dir = g_path_get_dirname(n.ref);
                add_ref(r, PCV_RES_STORAGE, real_dir);
            }
        } else {
            add_ref(r, x->kind, x->ref);
        }
        pcv_res_norm_clear(&n);
    }
    g_ptr_array_unref(old);
}

                                                                                                
PcvResDomState
pcv_res_domain_active_state(int is_active_rc, gboolean no_domain_error)
{
    if (is_active_rc > 0)
        return PCV_RES_DOM_ACTIVE;
    if (is_active_rc == 0)
        return PCV_RES_DOM_INACTIVE;
    return no_domain_error ? PCV_RES_DOM_GONE : PCV_RES_DOM_UNKNOWN;
}
