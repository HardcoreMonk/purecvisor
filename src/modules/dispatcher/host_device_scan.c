   
                           
                                                                      
  
                           
                                                                                                              
                                                                                          
                                                                                                
                                                                                                    
                                                                                            
                                                 
                                                                                                   
                                                                                            
                                                                                        
                                                                               
                                                                           
  
                                                                                
                                   
   
#include "modules/dispatcher/host_device_scan.h"

#include <errno.h>
#include <string.h>
#include <gio/gio.h>

#include "utils/pcv_pci_sysfs.h"
#include "utils/pcv_validate.h"

#define SCAN_MAX_TEXT_BYTES 256U                                            

                                                                                          
static gchar *scan_path(const gchar *root, const gchar *relative)
{
    if (!root || root[0] == '\0' || g_strcmp0(root, "/") == 0)
        return g_strdup_printf("/%s", relative);
    return g_build_filename(root, relative, NULL);
}

                                                                       
static gchar *scan_read(const gchar *dir, const gchar *name)
{
    g_autofree gchar *path = g_build_filename(dir, name, NULL);
    gchar *text = NULL;
    if (!g_file_get_contents(path, &text, NULL, NULL))
        return NULL;
    g_strstrip(text);
    if (text[0] == '\0')
        g_clear_pointer(&text, g_free);
    return text;
}

                                                                    
                                              
static gchar *scan_read_text(const gchar *dir, const gchar *name)
{
    g_autofree gchar *raw = scan_read(dir, name);
    if (!raw)
        return NULL;
    gchar *valid = g_utf8_make_valid(raw, -1);
    for (gchar *p = valid; *p; p++) {
        if ((guchar)*p < 0x20 || (guchar)*p == 0x7f)
            *p = ' ';
    }
    g_strstrip(valid);
    if (strlen(valid) > SCAN_MAX_TEXT_BYTES) {
        valid[SCAN_MAX_TEXT_BYTES] = '\0';
        while (valid[0] != '\0' && !g_utf8_validate(valid, -1, NULL))
            valid[strlen(valid) - 1] = '\0';
        g_strstrip(valid);
    }
    if (valid[0] == '\0')
        g_clear_pointer(&valid, g_free);
    return valid;
}

                                                                         
                                                                  
static gchar *scan_hex(const gchar *text, guint digits, gboolean strip_0x)
{
    if (!text)
        return NULL;
    if (strip_0x && (g_str_has_prefix(text, "0x") || g_str_has_prefix(text, "0X")))
        text += 2;
    if (strlen(text) != digits)
        return NULL;
    for (guint i = 0; i < digits; i++) {
        if (!g_ascii_isxdigit(text[i]))
            return NULL;
    }
    return g_ascii_strdown(text, -1);
}

                                                                    
static gchar *scan_read_hex(const gchar *dir, const gchar *name, guint digits, gboolean strip_0x)
{
    g_autofree gchar *raw = scan_read(dir, name);
    return scan_hex(raw, digits, strip_0x);
}

                                                                         
static guint scan_read_dec(const gchar *dir, const gchar *name)
{
    g_autofree gchar *raw = scan_read(dir, name);
    guint64 v = 0;
    return raw && g_ascii_string_to_unsigned(raw, 10, 0, G_MAXUINT, &v, NULL) ? (guint)v : 0;
}

                                                                                     
static gchar *scan_link_name(const gchar *dir, const gchar *name)
{
    g_autofree gchar *path = g_build_filename(dir, name, NULL);
    g_autofree gchar *target = g_file_read_link(path, NULL);
    return target ? g_path_get_basename(target) : NULL;
}

                                       
static gint scan_name_cmp(gconstpointer a, gconstpointer b)
{
    return g_strcmp0(*(gchar * const *)a, *(gchar * const *)b);
}

                                                                                
static GPtrArray *scan_list_dir(const gchar *path, GError **error)
{
    GDir *dir = g_dir_open(path, 0, error);
    if (!dir)
        return NULL;
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    const gchar *name = NULL;
    while ((name = g_dir_read_name(dir)) != NULL)
        g_ptr_array_add(names, g_strdup(name));
    g_dir_close(dir);
    g_ptr_array_sort(names, scan_name_cmp);
    return names;
}

                                                            
static gboolean scan_is_absent(const GError *err)
{
    return g_error_matches(err, G_FILE_ERROR, G_FILE_ERROR_NOENT);
}

                                                                             

static void scan_usb_iface_free(PcvHostUsbIface *f)
{
    g_free(f->cls);
    g_free(f->sub);
    g_free(f->proto);
    g_free(f->driver);
    g_free(f);
}

static void scan_usb_device_free(PcvHostUsbDevice *d)
{
    for (guint i = 0; d->ifaces && i < d->ifaces->len; i++)
        scan_usb_iface_free(g_ptr_array_index(d->ifaces, i));
    g_clear_pointer(&d->ifaces, g_ptr_array_unref);
    g_free(d->port);
    g_free(d->device_class);
    g_free(d->manufacturer);
    g_free(d->product_name);
    g_free(d);
}

                                                                          
                                                   
static gboolean scan_is_root_hub(const gchar *name)
{
    if (!g_str_has_prefix(name, "usb") || name[3] == '\0')
        return FALSE;
    for (const gchar *p = name + 3; *p; p++) {
        if (!g_ascii_isdigit(*p))
            return FALSE;
    }
    return TRUE;
}

                                                                                        
                                                                       
static GHashTable *scan_bluetooth_owners(const gchar *root)
{
    GHashTable *owners = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_autofree gchar *class_dir = scan_path(root, "sys/class/bluetooth");
    g_autoptr(GPtrArray) names = scan_list_dir(class_dir, NULL);
    for (guint i = 0; names && i < names->len; i++) {
        const gchar *name = g_ptr_array_index(names, i);
        if (!g_str_has_prefix(name, "hci"))
            continue;
        g_autofree gchar *hci = g_build_filename(class_dir, name, NULL);
        gchar *target = scan_link_name(hci, "device");
        if (!target)
            continue;
        gchar *colon = strchr(target, ':');
        if (colon)
            *colon = '\0';
        g_hash_table_add(owners, target);
    }
    return owners;
}

                                                                                         
static guint scan_iface_traits(const PcvHostUsbIface *f)
{
    guint t = 0;
    if (g_strcmp0(f->cls, "09") == 0)
        t |= PCV_HOST_USB_HUB;
    if (g_strcmp0(f->cls, "08") == 0)
        t |= PCV_HOST_USB_STORAGE;
    if (g_strcmp0(f->cls, "e0") == 0 && g_strcmp0(f->sub, "01") == 0 && g_strcmp0(f->proto, "01") == 0)
        t |= PCV_HOST_USB_BLUETOOTH;
    if (g_strcmp0(f->cls, "03") == 0 && g_strcmp0(f->sub, "01") == 0 && g_strcmp0(f->proto, "01") == 0)
        t |= PCV_HOST_USB_KEYBOARD;
    if (g_strcmp0(f->cls, "03") == 0 && g_strcmp0(f->sub, "01") == 0 && g_strcmp0(f->proto, "02") == 0)
        t |= PCV_HOST_USB_MOUSE;
    if (g_strcmp0(f->cls, "01") == 0)
        t |= PCV_HOST_USB_AUDIO;
    return t;
}

                                                                          
                                                            
static PcvHostUsbDevice *scan_usb_device(const gchar *devices_dir, const gchar *name, GHashTable *bt_owners)
{
    g_autofree gchar *dir = g_build_filename(devices_dir, name, NULL);
    g_autofree gchar *vid = scan_read_hex(dir, "idVendor", 4, FALSE);
    g_autofree gchar *pid = scan_read_hex(dir, "idProduct", 4, FALSE);
    if (!vid || !pid)
        return NULL;
    PcvHostUsbDevice *d = g_new0(PcvHostUsbDevice, 1);
    d->port = g_strdup(name);
    d->vendor = (guint16)g_ascii_strtoull(vid, NULL, 16);
    d->product = (guint16)g_ascii_strtoull(pid, NULL, 16);
    d->bus = scan_read_dec(dir, "busnum");
    d->devnum = scan_read_dec(dir, "devnum");
    d->device_class = scan_read_hex(dir, "bDeviceClass", 2, FALSE);
    d->manufacturer = scan_read_text(dir, "manufacturer");
    d->product_name = scan_read_text(dir, "product");
    d->ifaces = g_ptr_array_new();
    if (g_strcmp0(d->device_class, "09") == 0)
        d->traits |= PCV_HOST_USB_HUB;
    if (g_hash_table_contains(bt_owners, name))
        d->traits |= PCV_HOST_USB_BLUETOOTH;

    g_autofree gchar *prefix = g_strdup_printf("%s:", name);                                
    g_autoptr(GPtrArray) entries = scan_list_dir(dir, NULL);
    for (guint i = 0; entries && i < entries->len; i++) {
        const gchar *entry = g_ptr_array_index(entries, i);
        if (!g_str_has_prefix(entry, prefix))
            continue;
        g_autofree gchar *ifdir = g_build_filename(dir, entry, NULL);
        PcvHostUsbIface *f = g_new0(PcvHostUsbIface, 1);
        f->cls = scan_read_hex(ifdir, "bInterfaceClass", 2, FALSE);
        f->sub = scan_read_hex(ifdir, "bInterfaceSubClass", 2, FALSE);
        f->proto = scan_read_hex(ifdir, "bInterfaceProtocol", 2, FALSE);
        f->driver = scan_link_name(ifdir, "driver");
        d->traits |= scan_iface_traits(f);
        if (f->driver && g_strcmp0(f->driver, "usbfs") != 0)
            d->host_driver_bound = TRUE;                                                     
        g_ptr_array_add(d->ifaces, f);
    }
    return d;
}

                                                          
                                                      
static gboolean scan_usb(const gchar *root, PcvHostScan *scan, GError **error)
{
    g_autofree gchar *devices_dir = scan_path(root, "sys/bus/usb/devices");
    g_autoptr(GError) open_error = NULL;
    g_autoptr(GPtrArray) names = scan_list_dir(devices_dir, &open_error);
    if (!names) {
        if (scan_is_absent(open_error)) {
            scan->usb_absent = TRUE;
            return TRUE;
        }
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "cannot read host USB devices");
        return FALSE;
    }
    g_autoptr(GHashTable) bt_owners = scan_bluetooth_owners(root);
    g_autoptr(GHashTable) counts = g_hash_table_new(g_direct_hash, g_direct_equal);
    for (guint i = 0; i < names->len; i++) {
        const gchar *name = g_ptr_array_index(names, i);
        if (strchr(name, ':') || scan_is_root_hub(name))
            continue;
        PcvHostUsbDevice *d = scan_usb_device(devices_dir, name, bt_owners);
        if (!d)
            continue;
        gpointer key = GUINT_TO_POINTER(((guint)d->vendor << 16) | d->product);
        g_hash_table_replace(counts, key, GUINT_TO_POINTER(GPOINTER_TO_UINT(g_hash_table_lookup(counts, key)) + 1));
        g_ptr_array_add(scan->usb, d);
    }
    for (guint i = 0; i < scan->usb->len; i++) {
        PcvHostUsbDevice *d = g_ptr_array_index(scan->usb, i);
        gpointer key = GUINT_TO_POINTER(((guint)d->vendor << 16) | d->product);
        d->duplicate_on_host = GPOINTER_TO_UINT(g_hash_table_lookup(counts, key)) > 1;
    }
    return TRUE;
}

                                                                             

static void scan_pci_func_clear(PcvHostPciFunc *fn)
{
    g_free(fn->bdf);
    g_free(fn->class_code);
    g_free(fn->vendor_id);
    g_free(fn->device_id);
    g_free(fn->driver);
    g_free(fn->iommu_group);
}

static void scan_gpu_free(PcvHostGpu *g)
{
    for (guint i = 0; g->companions && i < g->companions->len; i++) {
        PcvHostPciFunc *fn = g_ptr_array_index(g->companions, i);
        scan_pci_func_clear(fn);
        g_free(fn);
    }
    g_clear_pointer(&g->companions, g_ptr_array_unref);
    scan_pci_func_clear(&g->fn);
    g_free(g);
}

                                                                                                     
                                               
static void scan_pci_func(const gchar *root, const gchar *devices_dir, const gchar *bdf, PcvHostPciFunc *fn)
{
    g_autofree gchar *dir = g_build_filename(devices_dir, bdf, NULL);
    fn->bdf = g_strdup(bdf);
    fn->class_code = scan_read_hex(dir, "class", 6, TRUE);
    fn->vendor_id = scan_read_hex(dir, "vendor", 4, TRUE);
    fn->device_id = scan_read_hex(dir, "device", 4, TRUE);
    fn->driver = scan_link_name(dir, "driver");
    (void)pcv_pci_iommu_group_dir(root, bdf, NULL, &fn->iommu_group, NULL);                              
}

                                                                              
                                      
static gboolean scan_display_connected(const gchar *dev_dir)
{
    g_autofree gchar *drm = g_build_filename(dev_dir, "drm", NULL);
    g_autoptr(GPtrArray) cards = scan_list_dir(drm, NULL);
    for (guint i = 0; cards && i < cards->len; i++) {
        const gchar *card = g_ptr_array_index(cards, i);
        if (!g_str_has_prefix(card, "card") || strchr(card, '-'))
            continue;
        g_autofree gchar *card_dir = g_build_filename(drm, card, NULL);
        g_autofree gchar *prefix = g_strdup_printf("%s-", card);
        g_autoptr(GPtrArray) conns = scan_list_dir(card_dir, NULL);
        for (guint k = 0; conns && k < conns->len; k++) {
            const gchar *conn = g_ptr_array_index(conns, k);
            if (!g_str_has_prefix(conn, prefix))
                continue;
            g_autofree gchar *conn_dir = g_build_filename(card_dir, conn, NULL);
            g_autofree gchar *status = scan_read(conn_dir, "status");
            if (g_strcmp0(status, "connected") == 0)
                return TRUE;
        }
    }
    return FALSE;
}

                                                                                             
                                                                                            
                                                                                             
                                                                              
                                                                                     
static void scan_gpu_group(const gchar *root, const gchar *devices_dir, PcvHostGpu *g)
{
    g_autofree gchar *group_dir = NULL;
    if (!pcv_pci_iommu_group_dir(root, g->fn.bdf, &group_dir, NULL, NULL) || !group_dir)
        return;
    g_autofree gchar *members_dir = g_build_filename(group_dir, "devices", NULL);
    g_autoptr(GPtrArray) members = scan_list_dir(members_dir, NULL);
    if (!members)
        return;
    gboolean safe = TRUE, ready = g_strcmp0(g->fn.driver, "vfio-pci") == 0;
    guint count = 0;
    for (guint i = 0; i < members->len; i++) {
        const gchar *bdf = g_ptr_array_index(members, i);
        if (!pcv_validate_pci_addr(bdf))
            continue;
        count++;
        if (strncmp(bdf, g->fn.bdf, 10) != 0)                                         
            safe = FALSE;
        if (g_strcmp0(bdf, g->fn.bdf) == 0)
            continue;
        PcvHostPciFunc *fn = g_new0(PcvHostPciFunc, 1);
        scan_pci_func(root, devices_dir, bdf, fn);
        if (g_strcmp0(fn->driver, "vfio-pci") != 0)
            ready = FALSE;
        g_ptr_array_add(g->companions, fn);
    }
    g->group_safe = safe && count > 0;
    g->group_vfio_ready = g->group_safe && ready;
}

                                                                                                
                                                      
static gboolean scan_pci(const gchar *root, PcvHostScan *scan, GError **error)
{
    g_autofree gchar *devices_dir = scan_path(root, "sys/bus/pci/devices");
    g_autoptr(GError) open_error = NULL;
    g_autoptr(GPtrArray) names = scan_list_dir(devices_dir, &open_error);
    if (!names) {
        if (scan_is_absent(open_error)) {
            scan->pci_absent = TRUE;
            return TRUE;
        }
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "cannot read host PCI devices");
        return FALSE;
    }
    for (guint i = 0; i < names->len; i++) {
        const gchar *bdf = g_ptr_array_index(names, i);
        if (!pcv_validate_pci_addr(bdf))
            continue;
        g_hash_table_add(scan->pci_bdfs, g_strdup(bdf));
        g_autofree gchar *dir = g_build_filename(devices_dir, bdf, NULL);
        g_autofree gchar *cls = scan_read_hex(dir, "class", 6, TRUE);
        if (!cls || strncmp(cls, "03", 2) != 0)                                                        
            continue;
        PcvHostGpu *g = g_new0(PcvHostGpu, 1);
        scan_pci_func(root, devices_dir, bdf, &g->fn);
        g->companions = g_ptr_array_new();
        g_autofree gchar *boot = scan_read(dir, "boot_vga");
        g->boot_vga = g_strcmp0(boot, "1") == 0;
        g->display_connected = scan_display_connected(dir);
        g->host_display = (g->boot_vga || g->display_connected) && g_strcmp0(g->fn.driver, "vfio-pci") != 0;
        scan_gpu_group(root, devices_dir, g);
        g_ptr_array_add(scan->gpus, g);                           
    }
    return TRUE;
}

                                                                            
gboolean pcv_host_device_scan(const gchar *root, PcvHostScan **out, GError **error)
{
    *out = NULL;
    PcvHostScan *scan = g_new0(PcvHostScan, 1);
    scan->usb = g_ptr_array_new();
    scan->gpus = g_ptr_array_new();
    scan->pci_bdfs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    if (!scan_usb(root, scan, error) || !scan_pci(root, scan, error)) {
        pcv_host_device_scan_free(scan);
        return FALSE;
    }
    scan->usb_ids = pcv_hw_ids_load(root, PCV_HW_IDS_USB);
    scan->pci_ids = pcv_hw_ids_load(root, PCV_HW_IDS_PCI);
    *out = scan;
    return TRUE;
}

                                                                    
void pcv_host_device_scan_free(PcvHostScan *scan)
{
    if (!scan)
        return;
    for (guint i = 0; scan->usb && i < scan->usb->len; i++)
        scan_usb_device_free(g_ptr_array_index(scan->usb, i));
    for (guint i = 0; scan->gpus && i < scan->gpus->len; i++)
        scan_gpu_free(g_ptr_array_index(scan->gpus, i));
    g_clear_pointer(&scan->usb, g_ptr_array_unref);
    g_clear_pointer(&scan->gpus, g_ptr_array_unref);
    g_clear_pointer(&scan->pci_bdfs, g_hash_table_unref);
    pcv_hw_ids_free(scan->usb_ids);
    pcv_hw_ids_free(scan->pci_ids);
    g_free(scan);
}

                                                               
gchar *pcv_host_usb_name(const PcvHwIds *usb_ids, guint16 vendor, guint16 product, const gchar *manufacturer,
                         const gchar *product_name, gchar **vendor_name_out)
{
    gchar vhex[5], phex[5];
    g_snprintf(vhex, sizeof vhex, "%04x", vendor);
    g_snprintf(phex, sizeof phex, "%04x", product);
    const gchar *dict_vendor = pcv_hw_ids_vendor(usb_ids, vhex);
    const gchar *dict_product = pcv_hw_ids_device(usb_ids, vhex, phex);
    if (vendor_name_out)
        *vendor_name_out = g_strdup(manufacturer ? manufacturer : dict_vendor);
    if (product_name) {
        if (manufacturer && !g_str_has_prefix(product_name, manufacturer))
            return g_strdup_printf("%s %s", manufacturer, product_name);
        return g_strdup(product_name);
    }
    if (!dict_product)
        return NULL;
    return dict_vendor ? g_strdup_printf("%s %s", dict_vendor, dict_product) : g_strdup(dict_product);
}
