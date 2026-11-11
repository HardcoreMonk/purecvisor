   
                          
                                                                                    
  
                           
                                                                                  
                                                                  
                                                                                        
                                                                            
                                                                                   
                     
  
                                                                    
  
                                                                       
                                            
                                                             
                                                                                   
                            
                                                                        
   
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <gio/gio.h>
#include <string.h>

#include "modules/dispatcher/hotplug_usb_xml.h"

                                                                
typedef struct {
    gchar   *vendor_id;
    gchar   *product_id;
    guint16  vendor;
    guint16  product;
    gchar   *policy;                                         
    gboolean missing;                             
    guint    bus;                                                               
    guint    device;                                               
} UsbItem;

                                                       
static void usb_item_free(gpointer p)
{
    UsbItem *it = p;
    g_free(it->vendor_id);
    g_free(it->product_id);
    g_free(it->policy);
    g_free(it);
}

                                                                 
static void usb_entry_free(gpointer p)
{
    PcvUsbHostdevEntry *e = p;
    g_free(e->vendor_id);
    g_free(e->product_id);
    g_free(e->startup_policy);
    g_free(e);
}

                                             
                                                              
static gboolean usb_id_parse(const gchar *s, guint16 *out)
{
    if (!s || s[0] != '0' || (s[1] != 'x' && s[1] != 'X'))
        return FALSE;
    gsize n = strlen(s + 2);
    if (n < 1 || n > 4)
        return FALSE;
    guint v = 0;
    for (gsize i = 0; i < n; i++) {
        gint d = g_ascii_xdigit_value(s[2 + i]);
        if (d < 0)
            return FALSE;
        v = v * 16 + (guint)d;
    }
    *out = (guint16)v;
    return TRUE;
}

                                                                       
static xmlNodePtr usb_child(xmlNodePtr node, const char *name)
{
    for (xmlNodePtr c = node ? node->children : NULL; c; c = c->next)
        if (c->type == XML_ELEMENT_NODE && xmlStrcmp(c->name, BAD_CAST name) == 0)
            return c;
    return NULL;
}

                                                                  
static gchar *usb_prop(xmlNodePtr node, const char *name)
{
    xmlChar *v = node ? xmlGetProp(node, BAD_CAST name) : NULL;
    gchar *copy = v ? g_strdup((const gchar *)v) : NULL;
    xmlFree(v);
    return copy;
}

                                                                             
static guint usb_dec_prop(xmlNodePtr node, const char *name)
{
    g_autofree gchar *v = usb_prop(node, name);
    guint64 n = 0;
    return v && g_ascii_string_to_unsigned(v, 10, 0, G_MAXUINT, &n, NULL) ? (guint)n : 0;
}

                                                                                  
                                                              
static UsbItem *usb_item_from_node(xmlNodePtr hostdev)
{
    xmlNodePtr source = usb_child(hostdev, "source");
    g_autofree gchar *vid = usb_prop(usb_child(source, "vendor"), "id");
    g_autofree gchar *pid = usb_prop(usb_child(source, "product"), "id");
    guint16 v = 0, p = 0;
    if (!source || !usb_id_parse(vid, &v) || !usb_id_parse(pid, &p))
        return NULL;

    UsbItem *it = g_new0(UsbItem, 1);
    it->vendor_id = g_steal_pointer(&vid);
    it->product_id = g_steal_pointer(&pid);
    it->vendor = v;
    it->product = p;
    it->policy = usb_prop(source, "startupPolicy");
    g_autofree gchar *missing = usb_prop(source, "missing");
    it->missing = g_strcmp0(missing, "yes") == 0;
    xmlNodePtr addr = usb_child(source, "address");                                                                  
    it->bus = usb_dec_prop(addr, "bus");
    it->device = usb_dec_prop(addr, "device");
    return it;
}

                                                                 
                                                             
                                                          
static gboolean usb_items_parse(const gchar *xml, GPtrArray **out, GError **error)
{
    *out = g_ptr_array_new_with_free_func(usb_item_free);
    if (!xml)
        return TRUE;

    xmlDocPtr doc = *xml ? xmlReadMemory(xml, (int)strlen(xml), "domain.xml", NULL,
                                         XML_PARSE_NONET | XML_PARSE_NOBLANKS |
                                         XML_PARSE_NOERROR | XML_PARSE_NOWARNING)
                         : NULL;
    if (!doc) {
        g_clear_pointer(out, g_ptr_array_unref);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Failed to parse domain XML");
        return FALSE;
    }

    xmlNodePtr root = xmlDocGetRootElement(doc);
    for (xmlNodePtr devices = root ? root->children : NULL; devices; devices = devices->next) {
        if (devices->type != XML_ELEMENT_NODE || xmlStrcmp(devices->name, BAD_CAST "devices") != 0)
            continue;
        for (xmlNodePtr h = devices->children; h; h = h->next) {
            if (h->type != XML_ELEMENT_NODE || xmlStrcmp(h->name, BAD_CAST "hostdev") != 0)
                continue;
            g_autofree gchar *type = usb_prop(h, "type");
            UsbItem *it = g_strcmp0(type, "usb") == 0 ? usb_item_from_node(h) : NULL;
            if (it)
                g_ptr_array_add(*out, it);
        }
    }
    xmlFreeDoc(doc);
    return TRUE;
}

                                                   
                                                   
static gint usb_nth_match(GPtrArray *items, guint16 v, guint16 p, guint nth)
{
    for (guint i = 0; i < items->len; i++) {
        const UsbItem *it = g_ptr_array_index(items, i);
        if (it->vendor == v && it->product == p && nth-- == 0)
            return (gint)i;
    }
    return -1;
}

                                                                                        
                                                                 
                                                                                 
static PcvUsbHostdevEntry *usb_entry_new(const UsbItem *src, const UsbItem *live_item, gboolean persistent,
                                         gboolean missing, const gchar *policy)
{
    PcvUsbHostdevEntry *e = g_new0(PcvUsbHostdevEntry, 1);
    e->vendor_id = g_strdup(src->vendor_id);
    e->product_id = g_strdup(src->product_id);
    e->vendor = src->vendor;
    e->product = src->product;
    e->live = live_item != NULL;
    e->persistent = persistent;
    e->missing = missing;
    e->startup_policy = g_strdup(policy ? policy : "mandatory");
    e->live_bus = live_item ? live_item->bus : 0;
    e->live_device = live_item ? live_item->device : 0;
    return e;
}

                                                          
                                                           
                                     
gboolean pcv_usb_hostdev_list(const gchar *live_xml,
                              const gchar *config_xml,
                              GPtrArray **entries_out,
                              GError **error)
{
    g_autoptr(GPtrArray) live = NULL;
    g_autoptr(GPtrArray) cfg = NULL;

    *entries_out = NULL;
    if (!usb_items_parse(config_xml, &cfg, error) || !usb_items_parse(live_xml, &live, error))
        return FALSE;

    g_autofree gboolean *paired = g_new0(gboolean, live->len + 1);
    GPtrArray *out = g_ptr_array_new_with_free_func(usb_entry_free);
    for (guint i = 0; i < cfg->len; i++) {
        const UsbItem *c = g_ptr_array_index(cfg, i);
        guint rank = 0;                                                          
        for (guint k = 0; k < i; k++) {
            const UsbItem *prev = g_ptr_array_index(cfg, k);
            rank += prev->vendor == c->vendor && prev->product == c->product;
        }
        gint j = usb_nth_match(live, c->vendor, c->product, rank);
        const UsbItem *l = j >= 0 ? g_ptr_array_index(live, (guint)j) : NULL;
        if (l)
            paired[j] = TRUE;
        g_ptr_array_add(out, usb_entry_new(c, l, TRUE, l && l->missing, c->policy));
    }
    for (guint j = 0; j < live->len; j++) {
        const UsbItem *l = g_ptr_array_index(live, j);
        if (!paired[j])
            g_ptr_array_add(out, usb_entry_new(l, l, FALSE, l->missing, l->policy));
    }
    *entries_out = out;
    return TRUE;
}


                                                                        

G_DEFINE_QUARK(pcv-usb-error-quark, pcv_usb_error)

                                                           
static const gchar *const USB_POLICY_NAMES[] = { "mandatory", "requisite", "optional" };

                                            
gboolean pcv_usb_policy_parse(const gchar *s, PcvUsbPolicy *out)
{
    for (guint i = 0; s && i < G_N_ELEMENTS(USB_POLICY_NAMES); i++) {
        if (strcmp(s, USB_POLICY_NAMES[i]) == 0) {
            *out = (PcvUsbPolicy)i;
            return TRUE;
        }
    }
    return FALSE;
}

                                                                  
const gchar *pcv_usb_policy_name(PcvUsbPolicy policy)
{
    return (guint)policy < G_N_ELEMENTS(USB_POLICY_NAMES) ? USB_POLICY_NAMES[policy] : "mandatory";
}

                                                            
guint16 pcv_usb_id_value(const gchar *validated_id)
{
    guint16 v = 0;
    return usb_id_parse(validated_id, &v) ? v : 0;
}

                                                      
gchar *pcv_usb_hostdev_xml(guint16 vendor, guint16 product, const PcvUsbPolicy *policy_or_null)
{
    g_autofree gchar *policy = policy_or_null
        ? g_strdup_printf(" startupPolicy='%s'", pcv_usb_policy_name(*policy_or_null)) : g_strdup("");
    return g_strdup_printf("<hostdev mode='subsystem' type='usb'>\n"
                           "  <source%s>\n"
                           "    <vendor id='0x%04x'/>\n"
                           "    <product id='0x%04x'/>\n"
                           "  </source>\n"
                           "</hostdev>", policy, vendor, product);
}

                                                                                   
                                                                                    
            
                                                                       
                                                                           
                                                                
gboolean pcv_usb_attach_plan(const gchar *live_xml,
                             const gchar *config_xml,
                             gboolean is_active,
                             gboolean has_persistent,
                             guint16 vendor,
                             guint16 product,
                             PcvUsbPolicy policy,
                             gboolean persist,
                             const gchar *vm_name,
                             PcvUsbAttachKind *kind_out,
                             gchar **persistent_policy_out,
                             GError **error)
{
    g_autoptr(GPtrArray) live = NULL;
    g_autoptr(GPtrArray) cfg = NULL;

    *kind_out = PCV_USB_ATTACH_NOOP;
    *persistent_policy_out = NULL;
    if (!usb_items_parse(has_persistent ? config_xml : NULL, &cfg, error) ||
        !usb_items_parse(is_active ? live_xml : NULL, &live, error))
        return FALSE;

    g_autofree gchar *dev = g_strdup_printf("0x%04x:0x%04x", vendor, product);
    gint li = usb_nth_match(live, vendor, product, 0);
    gint ci = usb_nth_match(cfg, vendor, product, 0);
    const UsbItem *l = li >= 0 ? g_ptr_array_index(live, (guint)li) : NULL;
    const UsbItem *c = ci >= 0 ? g_ptr_array_index(cfg, (guint)ci) : NULL;
    const gchar *want = pcv_usb_policy_name(policy);
    const gchar *have = c ? (c->policy ? c->policy : "mandatory") : NULL;

    if (!persist && !is_active) {
        g_set_error(error, PCV_USB_ERROR, PCV_USB_ERROR_CONFLICT,
                    "USB device %s cannot be attached to VM '%s' without saving it: the VM is not running", dev, vm_name);
        return FALSE;
    }
    if (usb_nth_match(live, vendor, product, 1) >= 0 || usb_nth_match(cfg, vendor, product, 1) >= 0) {
        g_set_error(error, PCV_USB_ERROR, PCV_USB_ERROR_CONFLICT,
                    "multiple USB hostdev entries match %s in VM '%s'; nothing was changed", dev, vm_name);
        return FALSE;
    }
    if (persist && c && strcmp(have, want) != 0) {                                             
        g_set_error(error, PCV_USB_ERROR, PCV_USB_ERROR_CONFLICT,
                    "USB device %s is already in the persistent definition of VM '%s' with startupPolicy '%s'; "
                    "detach it first to change the policy", dev, vm_name, have);
        return FALSE;
    }
    if (l && l->missing) {
        g_set_error(error, PCV_USB_ERROR, PCV_USB_ERROR_CONFLICT,
                    "USB device %s is assigned to VM '%s' but missing on the host (startupPolicy placeholder); "
                    "detach and attach it again, or restart the VM", dev, vm_name);
        return FALSE;
    }

    if (!is_active)
        *kind_out = c ? PCV_USB_ATTACH_NOOP : PCV_USB_ATTACH_CONFIG_STOPPED;
    else if (!has_persistent || !persist)                                                       
        *kind_out = l ? PCV_USB_ATTACH_NOOP : PCV_USB_ATTACH_LIVE_ONLY;
    else if (!l)
        *kind_out = c ? PCV_USB_ATTACH_LIVE_ONLY : PCV_USB_ATTACH_TWO_STEP;
    else
        *kind_out = c ? PCV_USB_ATTACH_NOOP : PCV_USB_ATTACH_CONFIG_LEGACY;
    if (has_persistent || !is_active)                                                            
        *persistent_policy_out = g_strdup(c ? have : (persist ? want : NULL));
    return TRUE;
}


                                                                        

                                                              
                                                                          
gboolean pcv_usb_detach_plan(const gchar *live_xml,
                             const gchar *config_xml,
                             gboolean is_active,
                             gboolean has_persistent,
                             guint16 vendor,
                             guint16 product,
                             const gchar *vm_name,
                             gboolean *live_out,
                             gboolean *config_out,
                             GError **error)
{
    g_autoptr(GPtrArray) live = NULL;
    g_autoptr(GPtrArray) cfg = NULL;

    *live_out = *config_out = FALSE;
    if (!usb_items_parse(has_persistent ? config_xml : NULL, &cfg, error) ||
        !usb_items_parse(is_active ? live_xml : NULL, &live, error))
        return FALSE;

    g_autofree gchar *dev = g_strdup_printf("0x%04x:0x%04x", vendor, product);
    if (usb_nth_match(live, vendor, product, 1) >= 0 || usb_nth_match(cfg, vendor, product, 1) >= 0) {
        g_set_error(error, PCV_USB_ERROR, PCV_USB_ERROR_CONFLICT,
                    "multiple USB hostdev entries match %s in VM '%s'; nothing was changed", dev, vm_name);
        return FALSE;
    }
    gboolean in_live = usb_nth_match(live, vendor, product, 0) >= 0;
    gboolean in_cfg = usb_nth_match(cfg, vendor, product, 0) >= 0;
    if (!in_live && !in_cfg) {
        g_set_error(error, PCV_USB_ERROR, PCV_USB_ERROR_NOT_ATTACHED,
                    "USB device %s is not attached to VM '%s' (live or persistent)", dev, vm_name);
        return FALSE;
    }
    *live_out = in_live;
    *config_out = in_cfg;
    return TRUE;
}
