   
                           
                                                                                                        
  
                           
                                                                                                           
                                                                                          
                                                                                     
                                                                                       
                                                                                      
                                                                                   
                                                                                              
                                                                               
  
                                                                              
                         
  
                                                                      
   
#include "modules/dispatcher/host_device_view.h"

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <gio/gio.h>
#include <string.h>

#include "modules/auth/pcv_rbac.h"
#include "modules/dispatcher/hotplug_usb_xml.h"
#include "utils/pcv_validate.h"

                                                                           

                                           
static void pci_entry_free(gpointer p)
{
    PcvPciHostdevEntry *e = p;
    g_free(e->bdf);
    g_free(e->managed);
    g_free(e);
}

                                                             
static xmlNodePtr pci_child(xmlNodePtr node, const char *name)
{
    for (xmlNodePtr c = node ? node->children : NULL; c; c = c->next)
        if (c->type == XML_ELEMENT_NODE && xmlStrcmp(c->name, BAD_CAST name) == 0)
            return c;
    return NULL;
}

                      
static gchar *pci_prop(xmlNodePtr node, const char *name)
{
    xmlChar *v = node ? xmlGetProp(node, BAD_CAST name) : NULL;
    gchar *copy = v ? g_strdup((const gchar *)v) : NULL;
    xmlFree(v);
    return copy;
}

                                                                                  
static gboolean pci_hex(xmlNodePtr node, const char *name, guint64 *v)
{
    g_autofree gchar *s = pci_prop(node, name);
    gchar *end = NULL;
    if (!s || !*s)
        return FALSE;
    *v = g_ascii_strtoull(s, &end, 16);
    return end && *end == '\0';
}

                                                                                                   
static gchar *pci_bdf(xmlNodePtr hostdev)
{
    xmlNodePtr addr = pci_child(pci_child(hostdev, "source"), "address");
    guint64 dom = 0, bus = 0, slot = 0, fn = 0;
    if (!addr || !pci_hex(addr, "domain", &dom) || !pci_hex(addr, "bus", &bus) || !pci_hex(addr, "slot", &slot) ||
        !pci_hex(addr, "function", &fn) || dom > 0xffff || bus > 0xff || slot > 0x1f || fn > 7)
        return NULL;
    gchar *bdf = g_strdup_printf("%04x:%02x:%02x.%x", (guint)dom, (guint)bus, (guint)slot, (guint)fn);
    if (!pcv_validate_pci_addr(bdf))
        g_clear_pointer(&bdf, g_free);
    return bdf;
}

                                                                                
                                   
static gboolean pci_items_parse(const gchar *xml, GPtrArray **out, GError **error)
{
    *out = g_ptr_array_new_with_free_func(pci_entry_free);
    if (!xml)
        return TRUE;
    xmlDocPtr doc = *xml ? xmlReadMemory(xml, (int)strlen(xml), "domain.xml", NULL,
                                         XML_PARSE_NONET | XML_PARSE_NOBLANKS | XML_PARSE_NOERROR | XML_PARSE_NOWARNING)
                         : NULL;
    if (!doc) {
        g_clear_pointer(out, g_ptr_array_unref);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Failed to parse domain XML");
        return FALSE;
    }
    xmlNodePtr root = xmlDocGetRootElement(doc);
    for (xmlNodePtr devices = root ? root->children : NULL; devices; devices = devices->next) {
        if (devices->type != XML_ELEMENT_NODE || xmlStrcmp(devices->name, BAD_CAST "devices") != 0)
            continue;
        for (xmlNodePtr h = devices->children; h; h = h->next) {
            if (h->type != XML_ELEMENT_NODE || xmlStrcmp(h->name, BAD_CAST "hostdev") != 0)
                continue;
            g_autofree gchar *type = pci_prop(h, "type");
            g_autofree gchar *bdf = g_strcmp0(type, "pci") == 0 ? pci_bdf(h) : NULL;
            gboolean dup = FALSE;
            for (guint i = 0; bdf && i < (*out)->len && !dup; i++)
                dup = g_strcmp0(((PcvPciHostdevEntry *)g_ptr_array_index(*out, i))->bdf, bdf) == 0;
            if (!bdf || dup)
                continue;
            PcvPciHostdevEntry *e = g_new0(PcvPciHostdevEntry, 1);
            e->bdf = g_steal_pointer(&bdf);
            e->managed = pci_prop(h, "managed");
            g_ptr_array_add(*out, e);
        }
    }
    xmlFreeDoc(doc);
    return TRUE;
}

                                     
static PcvPciHostdevEntry *pci_entry_dup(const PcvPciHostdevEntry *src, gboolean live, gboolean persistent)
{
    PcvPciHostdevEntry *e = g_new0(PcvPciHostdevEntry, 1);
    e->bdf = g_strdup(src->bdf);
    e->live = live;
    e->persistent = persistent;
    e->managed = g_strdup(src->managed);
    return e;
}

                                                                           
gboolean pcv_pci_hostdev_list(const gchar *live_xml, const gchar *config_xml, GPtrArray **out, GError **error)
{
    g_autoptr(GPtrArray) live = NULL;
    g_autoptr(GPtrArray) cfg = NULL;
    *out = NULL;
    if (!pci_items_parse(config_xml, &cfg, error) || !pci_items_parse(live_xml, &live, error))
        return FALSE;
    g_autofree gboolean *paired = g_new0(gboolean, live->len + 1);
    GPtrArray *res = g_ptr_array_new_with_free_func(pci_entry_free);
    for (guint i = 0; i < cfg->len; i++) {
        const PcvPciHostdevEntry *c = g_ptr_array_index(cfg, i);
        gboolean in_live = FALSE;
        for (guint j = 0; j < live->len && !in_live; j++) {
            if (g_strcmp0(((PcvPciHostdevEntry *)g_ptr_array_index(live, j))->bdf, c->bdf) == 0)
                in_live = paired[j] = TRUE;
        }
        g_ptr_array_add(res, pci_entry_dup(c, in_live, TRUE));
    }
    for (guint j = 0; j < live->len; j++) {
        if (!paired[j])
            g_ptr_array_add(res, pci_entry_dup(g_ptr_array_index(live, j), TRUE, FALSE));
    }
    *out = res;
    return TRUE;
}

                                                                    

                                                                  
PcvHostReadOutcome pcv_host_device_read_outcome(gboolean xml_ok, gboolean err_is_no_domain, PcvResDomState active_state,
                                                gboolean is_live_read)
{
    if (xml_ok)
        return PCV_HOST_READ_USE;
    if (err_is_no_domain || active_state == PCV_RES_DOM_GONE)
        return PCV_HOST_READ_SKIP;
    if (is_live_read && active_state == PCV_RES_DOM_INACTIVE)
        return PCV_HOST_READ_INACTIVE_ONLY;
    return PCV_HOST_READ_FAIL;
}

                                                     
gchar *pcv_host_device_read_error_message(const PcvHostViewCaller *caller, const gchar *vm_name)
{
    if (caller && caller->role >= PCV_ROLE_ADMIN && vm_name)
        return g_strdup_printf("cannot read the definition of VM '%s'", vm_name);
    return g_strdup("cannot read a VM definition");
}

                                    
void pcv_host_vm_def_free(gpointer def)
{
    PcvHostVmDef *v = def;
    if (!v)
        return;
    g_free(v->name);
    g_free(v->owner);
    g_free(v->live_xml);
    g_free(v->config_xml);
    g_free(v);
}

                                                                           

                                                                                                
typedef struct {
    const PcvHostVmDef *vm;
    gboolean usb, live, persistent, missing, stale, ambiguous;
    gchar   *text;
} ViewAsg;

typedef enum { ROW_GPU, ROW_USB, ROW_USB_GONE, ROW_PCI_GONE } ViewRowKind;

                                                                                    
typedef struct {
    ViewRowKind              kind;
    const PcvHostGpu        *gpu;
    const PcvHostUsbDevice  *usb;
    guint16                  vendor, product;
    gchar                   *id;
    GPtrArray               *asg;
    GPtrArray               *comp_asg;
} ViewRow;

typedef struct {
    const PcvHostScan       *scan;
    const PcvHostViewCaller *caller;
    GPtrArray               *rows;                      
    GHashTable              *gone;                                      
} ViewCtx;

static void view_asg_free(gpointer p)
{
    ViewAsg *a = p;
    g_free(a->text);
    g_free(a);
}

static void view_row_free(gpointer p)
{
    ViewRow *r = p;
    g_ptr_array_unref(r->asg);
    if (r->comp_asg)
        g_ptr_array_unref(r->comp_asg);
    g_free(r->id);
    g_free(r);
}

static ViewRow *view_row_add(ViewCtx *cx, ViewRowKind kind, gchar *id)
{
    ViewRow *r = g_new0(ViewRow, 1);
    r->kind = kind;
    r->id = id;
    r->asg = g_ptr_array_new_with_free_func(view_asg_free);
    g_ptr_array_add(cx->rows, r);
    return r;
}

                                                           
static void view_asg_add(GPtrArray *list, const PcvHostVmDef *vm, gboolean usb, gboolean live, gboolean persistent,
                         gboolean missing, gboolean stale, gboolean ambiguous, const gchar *text)
{
    ViewAsg *a = g_new0(ViewAsg, 1);
    a->vm = vm;
    a->usb = usb;
    a->live = live;
    a->persistent = persistent;
    a->missing = missing;
    a->stale = stale;
    a->ambiguous = ambiguous;
    a->text = g_strdup(text);
    g_ptr_array_add(list, a);
}

                                                        
static ViewRow *view_gone_row(ViewCtx *cx, ViewRowKind kind, gchar *id, guint16 vendor, guint16 product)
{
    ViewRow *r = g_hash_table_lookup(cx->gone, id);
    if (r) {
        g_free(id);
        return r;
    }
    r = view_row_add(cx, kind, id);
    r->vendor = vendor;
    r->product = product;
    g_hash_table_insert(cx->gone, r->id, r);
    return r;
}

                                                                                   
static void view_match_usb(ViewCtx *cx, const PcvHostVmDef *vm, const PcvUsbHostdevEntry *e)
{
    g_autoptr(GPtrArray) same = g_ptr_array_new();
    for (guint i = 0; i < cx->rows->len; i++) {
        ViewRow *r = g_ptr_array_index(cx->rows, i);
        if (r->kind == ROW_USB && r->usb->vendor == e->vendor && r->usb->product == e->product)
            g_ptr_array_add(same, r);
    }
    gboolean stale = FALSE;
    if (e->live && e->live_bus != 0 && e->live_device != 0) {
        for (guint i = 0; i < same->len; i++) {                                                      
            ViewRow *r = g_ptr_array_index(same, i);
            if (r->usb->bus == e->live_bus && r->usb->devnum == e->live_device) {
                view_asg_add(r->asg, vm, TRUE, e->live, e->persistent, e->missing, FALSE, FALSE, e->startup_policy);
                return;
            }
        }
        stale = TRUE;                                                                                   
    }
    if (same->len == 0) {                                                                         
        ViewRow *r = view_gone_row(cx, ROW_USB_GONE, g_strdup_printf("%04x:%04x", e->vendor, e->product), e->vendor,
                                   e->product);
        view_asg_add(r->asg, vm, TRUE, e->live, e->persistent, e->missing, stale, FALSE, e->startup_policy);
        return;
    }
    for (guint i = 0; i < same->len; i++) {                                                                       
        ViewRow *r = g_ptr_array_index(same, i);
        view_asg_add(r->asg, vm, TRUE, e->live, e->persistent, e->missing, stale, same->len > 1, e->startup_policy);
    }
}

                                                                        
static void view_match_pci(ViewCtx *cx, const PcvHostVmDef *vm, const PcvPciHostdevEntry *e)
{
    guint hits = 0;
    for (guint i = 0; i < cx->rows->len; i++) {
        ViewRow *r = g_ptr_array_index(cx->rows, i);
        if (r->kind != ROW_GPU)
            continue;
        if (g_strcmp0(r->gpu->fn.bdf, e->bdf) == 0) {
            view_asg_add(r->asg, vm, FALSE, e->live, e->persistent, FALSE, FALSE, FALSE, e->managed);
            hits++;
        }
        for (guint k = 0; k < r->gpu->companions->len; k++) {
            const PcvHostPciFunc *fn = g_ptr_array_index(r->gpu->companions, k);
            if (g_strcmp0(fn->bdf, e->bdf) == 0) {
                view_asg_add(g_ptr_array_index(r->comp_asg, k), vm, FALSE, e->live, e->persistent, FALSE, FALSE, FALSE,
                             e->managed);
                hits++;
            }
        }
    }
    if (hits || g_hash_table_contains(cx->scan->pci_bdfs, e->bdf))                                    
        return;
    if (g_strcmp0(e->managed, "no") != 0)                                                                   
        return;
    ViewRow *r = view_gone_row(cx, ROW_PCI_GONE, g_strdup(e->bdf), 0, 0);
    view_asg_add(r->asg, vm, FALSE, e->live, e->persistent, FALSE, FALSE, FALSE, e->managed);
}

                         
static GHashTable *view_persistent_vms(GPtrArray *asg)
{
    GHashTable *set = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; i < asg->len; i++) {
        const ViewAsg *a = g_ptr_array_index(asg, i);
        if (a->persistent)
            g_hash_table_add(set, a->vm->name);
    }
    return set;
}

                                                                               
                                          
static gboolean view_partial(const ViewRow *r)
{
    g_autoptr(GHashTable) base = view_persistent_vms(r->asg);
    for (guint k = 0; k < r->comp_asg->len; k++) {
        g_autoptr(GHashTable) other = view_persistent_vms(g_ptr_array_index(r->comp_asg, k));
        if (g_hash_table_size(other) != g_hash_table_size(base))
            return TRUE;
        GHashTableIter it;
        gpointer key = NULL;
        g_hash_table_iter_init(&it, other);
        while (g_hash_table_iter_next(&it, &key, NULL)) {
            if (!g_hash_table_contains(base, key))
                return TRUE;
        }
    }
    return FALSE;
}

                                                                        

                                                                                   
                                         
static gboolean view_name_visible(const PcvHostViewCaller *caller, const PcvHostVmDef *vm)
{
    if (caller->role >= PCV_ROLE_ADMIN)
        return TRUE;
    return caller->role >= PCV_ROLE_OPERATOR && vm->owner && caller->subject && g_strcmp0(vm->owner, caller->subject) == 0;
}

                                                                                 
static gboolean view_row_visible(const PcvHostViewCaller *caller, const ViewRow *r)
{
    if (caller->role >= PCV_ROLE_ADMIN)
        return TRUE;
    PcvResKind kind = (r->kind == ROW_GPU || r->kind == ROW_PCI_GONE) ? PCV_RES_PCI : PCV_RES_USB;
    return caller->visible && caller->visible(caller->ctx, kind, r->id);
}

                                                                                 
static gint view_natural_cmp(const gchar *a, const gchar *b)
{
    while (*a && *b) {
        if (g_ascii_isdigit(*a) && g_ascii_isdigit(*b)) {
            guint64 x = 0, y = 0;
            for (; g_ascii_isdigit(*a); a++)
                x = x * 10 + (guint64)(*a - '0');
            for (; g_ascii_isdigit(*b); b++)
                y = y * 10 + (guint64)(*b - '0');
            if (x != y)
                return x < y ? -1 : 1;
        } else {
            if (*a != *b)
                return (guchar)*a < (guchar)*b ? -1 : 1;
            a++;
            b++;
        }
    }
    return (*a != '\0') - (*b != '\0');
}

                                           
static guint view_group_rank(const ViewRow *r)
{
    if (r->kind == ROW_GPU || r->kind == ROW_PCI_GONE)
        return 0;
    return (r->kind == ROW_USB && (r->usb->traits & PCV_HOST_USB_BLUETOOTH)) ? 2 : 1;
}

                                                              
static gint view_row_cmp(gconstpointer pa, gconstpointer pb)
{
    const ViewRow *a = *(ViewRow * const *)pa, *b = *(ViewRow * const *)pb;
    guint ga = view_group_rank(a), gb = view_group_rank(b);
    if (ga != gb)
        return ga < gb ? -1 : 1;
    gboolean gone_a = a->kind == ROW_USB_GONE || a->kind == ROW_PCI_GONE;
    gboolean gone_b = b->kind == ROW_USB_GONE || b->kind == ROW_PCI_GONE;
    if (gone_a != gone_b)
        return gone_a ? 1 : -1;
    if (a->kind == ROW_USB && b->kind == ROW_USB)
        return view_natural_cmp(a->usb->port, b->usb->port);
    return g_strcmp0(a->id, b->id);
}

                                                             
static gint view_asg_cmp(gconstpointer pa, gconstpointer pb, gpointer user)
{
    const PcvHostViewCaller *caller = user;
    const ViewAsg *a = *(ViewAsg * const *)pa, *b = *(ViewAsg * const *)pb;
    gboolean va = view_name_visible(caller, a->vm), vb = view_name_visible(caller, b->vm);
    if (va != vb)
        return va ? -1 : 1;
    return g_strcmp0(a->vm->name, b->vm->name);
}

                                                                             

                    
static void view_set_str(JsonObject *o, const gchar *key, const gchar *value)
{
    if (value)
        json_object_set_string_member(o, key, value);
    else
        json_object_set_null_member(o, key);
}

                                   
static void view_set_uint(JsonObject *o, const gchar *key, guint value)
{
    if (value)
        json_object_set_int_member(o, key, value);
    else
        json_object_set_null_member(o, key);
}

                                                                                     
                                                         
                                                    
static JsonObject *view_assignment(const PcvHostViewCaller *caller, const ViewAsg *a)
{
    JsonObject *o = json_object_new();
    gboolean shown = view_name_visible(caller, a->vm);
    if (shown)
        json_object_set_string_member(o, "vm", a->vm->name);
    else
        json_object_set_null_member(o, "vm");
    json_object_set_boolean_member(o, "vm_visible", shown);
    json_object_set_boolean_member(o, "vm_active", a->vm->active);
    json_object_set_boolean_member(o, "live", a->live);
    json_object_set_boolean_member(o, "persistent", a->persistent);
    if (a->usb) {
        json_object_set_boolean_member(o, "missing", a->missing);
        view_set_str(o, "startup_policy", a->text);
        json_object_set_boolean_member(o, "stale_live_address", a->stale);
        json_object_set_boolean_member(o, "ambiguous", a->ambiguous);
    } else {
        view_set_str(o, "managed", a->text);
    }
    return o;
}

                                     
static JsonArray *view_assignments(const PcvHostViewCaller *caller, GPtrArray *asg)
{
    g_ptr_array_sort_with_data(asg, view_asg_cmp, (gpointer)caller);
    JsonArray *arr = json_array_new();
    for (guint i = 0; i < asg->len; i++)
        json_array_add_object_element(arr, view_assignment(caller, g_ptr_array_index(asg, i)));
    return arr;
}

                                                      
static gchar *view_pci_name(const PcvHwIds *ids, const PcvHostPciFunc *fn, const gchar **vendor_out)
{
    const gchar *vendor = pcv_hw_ids_vendor(ids, fn->vendor_id);
    const gchar *device = pcv_hw_ids_device(ids, fn->vendor_id, fn->device_id);
    if (vendor_out)
        *vendor_out = vendor;
    if (!device)
        return NULL;
    return vendor ? g_strdup_printf("%s %s", vendor, device) : g_strdup(device);
}

                                                                                                                
static JsonObject *view_row_common(const ViewRow *r, const gchar *kind, const gchar *group, gboolean present, gchar *name,
                                   const gchar *vendor_name, gboolean hidden, const gchar *blocked)
{
    JsonObject *o = json_object_new();
    json_object_set_string_member(o, "kind", kind);
    json_object_set_string_member(o, "group", group);
    json_object_set_string_member(o, "id", r->id);
    json_object_set_boolean_member(o, "present", present);
    view_set_str(o, "name", name);
    view_set_str(o, "vendor_name", vendor_name);
    json_object_set_boolean_member(o, "hidden_by_default", hidden);
    json_object_set_boolean_member(o, "assignable", blocked == NULL);
    view_set_str(o, "blocked_reason", blocked);
    g_free(name);
    return o;
}

                                                                                                           
static JsonObject *view_gpu_json(const ViewCtx *cx, const ViewRow *r)
{
    const PcvHostGpu *g = r->gpu;
    const gchar *blocked = g->host_display ? "host_display" : !g->group_safe ? "iommu_group_unsafe"
                         : !g->group_vfio_ready ? "not_vfio_bound" : NULL;
    const gchar *vendor = NULL;
    gchar *name = view_pci_name(cx->scan->pci_ids, &g->fn, &vendor);
    JsonObject *o = view_row_common(r, "pci", "gpu", TRUE, name, vendor, FALSE, blocked);
    json_object_set_string_member(o, "pci_addr", g->fn.bdf);
    view_set_str(o, "vendor_id", g->fn.vendor_id);
    view_set_str(o, "device_id", g->fn.device_id);
    view_set_str(o, "driver", g->fn.driver);
    view_set_str(o, "iommu_group", g->fn.iommu_group);
    json_object_set_boolean_member(o, "boot_vga", g->boot_vga);
    json_object_set_boolean_member(o, "display_connected", g->display_connected);
    json_object_set_boolean_member(o, "host_display", g->host_display);
    json_object_set_boolean_member(o, "group_safe", g->group_safe);
    json_object_set_boolean_member(o, "group_vfio_ready", g->group_vfio_ready);
    json_object_set_boolean_member(o, "partial_assignment", view_partial(r));
    JsonArray *comps = json_array_new();
    for (guint k = 0; k < g->companions->len; k++) {
        const PcvHostPciFunc *fn = g_ptr_array_index(g->companions, k);
        JsonObject *c = json_object_new();
        json_object_set_string_member(c, "pci_addr", fn->bdf);
        view_set_str(c, "class_code", fn->class_code);
        g_autofree gchar *cname = view_pci_name(cx->scan->pci_ids, fn, NULL);
        view_set_str(c, "name", cname);
        view_set_str(c, "driver", fn->driver);
        json_object_set_array_member(c, "assignments", view_assignments(cx->caller, g_ptr_array_index(r->comp_asg, k)));
        json_array_add_object_element(comps, c);
    }
    json_object_set_array_member(o, "companions", comps);
    return o;
}

                                                                   
static JsonObject *view_usb_json(const ViewCtx *cx, const ViewRow *r)
{
    static const struct { guint bit; const gchar *name; } traits[] = {
        { PCV_HOST_USB_HUB, "hub" }, { PCV_HOST_USB_STORAGE, "storage" }, { PCV_HOST_USB_BLUETOOTH, "bluetooth" },
        { PCV_HOST_USB_KEYBOARD, "keyboard" }, { PCV_HOST_USB_MOUSE, "mouse" }, { PCV_HOST_USB_AUDIO, "audio" },
    };
    const PcvHostUsbDevice *d = r->usb;
    const gchar *blocked = (d->traits & PCV_HOST_USB_HUB) ? "hub" : d->duplicate_on_host ? "duplicate_on_host" : NULL;
    g_autofree gchar *vendor = NULL;
    gchar *name = pcv_host_usb_name(cx->scan->usb_ids, d->vendor, d->product, d->manufacturer, d->product_name, &vendor);
    JsonObject *o = view_row_common(r, "usb", (d->traits & PCV_HOST_USB_BLUETOOTH) ? "bluetooth" : "usb", TRUE, name,
                                    vendor, (d->traits & (PCV_HOST_USB_HUB | PCV_HOST_USB_STORAGE)) != 0, blocked);
    g_autofree gchar *vid = g_strdup_printf("0x%04x", d->vendor);
    g_autofree gchar *pid = g_strdup_printf("0x%04x", d->product);
    json_object_set_string_member(o, "vendor_id", vid);
    json_object_set_string_member(o, "product_id", pid);
    view_set_uint(o, "bus", d->bus);
    view_set_uint(o, "device", d->devnum);
    json_object_set_string_member(o, "port", d->port);
    view_set_str(o, "device_class", d->device_class);
    JsonArray *ifs = json_array_new();
    for (guint i = 0; i < d->ifaces->len; i++) {
        const PcvHostUsbIface *f = g_ptr_array_index(d->ifaces, i);
        JsonObject *fo = json_object_new();
        view_set_str(fo, "class", f->cls);
        view_set_str(fo, "subclass", f->sub);
        view_set_str(fo, "protocol", f->proto);
        view_set_str(fo, "driver", f->driver);
        json_array_add_object_element(ifs, fo);
    }
    json_object_set_array_member(o, "interfaces", ifs);
    JsonArray *ta = json_array_new();
    for (guint i = 0; i < G_N_ELEMENTS(traits); i++) {
        if (d->traits & traits[i].bit)
            json_array_add_string_element(ta, traits[i].name);
    }
    json_object_set_array_member(o, "traits", ta);
    json_object_set_boolean_member(o, "host_driver_bound", d->host_driver_bound);
    json_object_set_boolean_member(o, "duplicate_on_host", d->duplicate_on_host);
    return o;
}

                                                                  
static JsonObject *view_row_json(const ViewCtx *cx, ViewRow *r)
{
    JsonObject *o = NULL;
    if (r->kind == ROW_GPU) {
        o = view_gpu_json(cx, r);
    } else if (r->kind == ROW_USB) {
        o = view_usb_json(cx, r);
    } else if (r->kind == ROW_USB_GONE) {
        g_autofree gchar *vendor = NULL;
        gchar *name = pcv_host_usb_name(cx->scan->usb_ids, r->vendor, r->product, NULL, NULL, &vendor);
        o = view_row_common(r, "usb", "usb", FALSE, name, vendor, FALSE, "not_present");
        g_autofree gchar *vid = g_strdup_printf("0x%04x", r->vendor);
        g_autofree gchar *pid = g_strdup_printf("0x%04x", r->product);
        json_object_set_string_member(o, "vendor_id", vid);
        json_object_set_string_member(o, "product_id", pid);
    } else {
        o = view_row_common(r, "pci", "gpu", FALSE, NULL, NULL, FALSE, "not_present");
        json_object_set_string_member(o, "pci_addr", r->id);
    }
    json_object_set_array_member(o, "assignments", view_assignments(cx->caller, r->asg));
    return o;
}

                     
static gint view_vm_cmp(gconstpointer pa, gconstpointer pb)
{
    return g_strcmp0((*(PcvHostVmDef * const *)pa)->name, (*(PcvHostVmDef * const *)pb)->name);
}

                                                                 
static gboolean view_match_vm(ViewCtx *cx, const PcvHostVmDef *vm, GError **error)
{
    g_autoptr(GPtrArray) usb = NULL;
    g_autoptr(GPtrArray) pci = NULL;
    if (!pcv_usb_hostdev_list(vm->live_xml, vm->config_xml, &usb, NULL) ||
        !pcv_pci_hostdev_list(vm->live_xml, vm->config_xml, &pci, NULL)) {
        g_autofree gchar *msg = pcv_host_device_read_error_message(cx->caller, vm->name);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, msg);
        return FALSE;
    }
    for (guint i = 0; i < usb->len; i++)
        view_match_usb(cx, vm, g_ptr_array_index(usb, i));
    for (guint i = 0; i < pci->len; i++)
        view_match_pci(cx, vm, g_ptr_array_index(pci, i));
    return TRUE;
}

                                                  
JsonObject *pcv_host_device_view_build(const PcvHostScan *scan, const GPtrArray *vms, const PcvHostViewCaller *caller,
                                       guint max_devices, GError **error)
{
    if (!scan || !vms || !caller) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "invalid arguments");
        return NULL;
    }
    ViewCtx cx = { scan, caller, g_ptr_array_new_with_free_func(view_row_free),
                   g_hash_table_new(g_str_hash, g_str_equal) };
    for (guint i = 0; i < scan->gpus->len; i++) {
        const PcvHostGpu *g = g_ptr_array_index(scan->gpus, i);
        ViewRow *r = view_row_add(&cx, ROW_GPU, g_strdup(g->fn.bdf));
        r->gpu = g;
        r->comp_asg = g_ptr_array_new_with_free_func((GDestroyNotify)g_ptr_array_unref);
        for (guint k = 0; k < g->companions->len; k++)
            g_ptr_array_add(r->comp_asg, g_ptr_array_new_with_free_func(view_asg_free));
    }
    for (guint i = 0; i < scan->usb->len; i++) {
        const PcvHostUsbDevice *d = g_ptr_array_index(scan->usb, i);
        ViewRow *r = view_row_add(&cx, ROW_USB, g_strdup_printf("%04x:%04x", d->vendor, d->product));
        r->usb = d;
    }
    for (guint i = 0; i < vms->len; i++) {                                                        
        if (!view_match_vm(&cx, g_ptr_array_index((GPtrArray *)vms, i), error)) {
            g_hash_table_unref(cx.gone);
            g_ptr_array_unref(cx.rows);
            return NULL;                                                                         
        }
    }

    g_autoptr(GPtrArray) shown = g_ptr_array_new();
    for (guint i = 0; i < cx.rows->len; i++) {
        ViewRow *r = g_ptr_array_index(cx.rows, i);
        if (view_row_visible(caller, r))
            g_ptr_array_add(shown, r);
    }
    g_ptr_array_sort(shown, view_row_cmp);
    guint cap = max_devices ? max_devices : PCV_HOST_DEVICE_MAX;
    JsonArray *devices = json_array_new();
    for (guint i = 0; i < shown->len && i < cap; i++)
        json_array_add_object_element(devices, view_row_json(&cx, g_ptr_array_index(shown, i)));

    g_autoptr(GPtrArray) vm_rows = g_ptr_array_new();
    for (guint i = 0; i < vms->len; i++) {
        PcvHostVmDef *vm = g_ptr_array_index((GPtrArray *)vms, i);
        if (view_name_visible(caller, vm))
            g_ptr_array_add(vm_rows, vm);
    }
    g_ptr_array_sort(vm_rows, view_vm_cmp);
    JsonArray *vm_arr = json_array_new();
    for (guint i = 0; i < vm_rows->len; i++) {
        const PcvHostVmDef *vm = g_ptr_array_index(vm_rows, i);
        JsonObject *v = json_object_new();
        json_object_set_string_member(v, "name", vm->name);
        json_object_set_boolean_member(v, "active", vm->active);
        json_object_set_boolean_member(v, "persistent", vm->persistent);
        json_array_add_object_element(vm_arr, v);
    }

    JsonObject *sources = json_object_new();
    json_object_set_string_member(sources, "usb", scan->usb_absent ? "absent" : "available");
    json_object_set_string_member(sources, "pci", scan->pci_absent ? "absent" : "available");
    json_object_set_string_member(sources, "names", pcv_hw_ids_available(scan->usb_ids) &&
                                  pcv_hw_ids_available(scan->pci_ids) ? "available" : "unavailable");
    JsonObject *o = json_object_new();
    json_object_set_array_member(o, "devices", devices);
    json_object_set_array_member(o, "vms", vm_arr);
    json_object_set_object_member(o, "sources", sources);
    json_object_set_boolean_member(o, "truncated", shown->len > cap);
    g_hash_table_unref(cx.gone);
    g_ptr_array_unref(cx.rows);
    return o;
}
