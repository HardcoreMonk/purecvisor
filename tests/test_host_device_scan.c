   
                                
                                                                                          
  
                           
                                                                                                
                                                                                        
                                                      
                                                                                                                 
                                                                                         
                                                                                            
                                                                                           
                                                                                                  
                                                                                        
                                                                                 
                                           
  
                                                                                
                              
                                                                         
   
#include <ftw.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <glib.h>
#include <glib/gstdio.h>

#include "modules/dispatcher/host_device_scan.h"
#include "utils/pcv_pci_sysfs.h"

                                                                           

                                                                       
static void hd_write(const gchar *root, const gchar *relative, const gchar *value)
{
    g_autofree gchar *path = g_build_filename(root, relative, NULL);
    g_autofree gchar *parent = g_path_get_dirname(path);
    g_assert_cmpint(g_mkdir_with_parents(parent, 0700), ==, 0);
    g_assert_true(g_file_set_contents(path, value, -1, NULL));
}

                                                        
static void hd_mkdir(const gchar *root, const gchar *relative)
{
    g_autofree gchar *path = g_build_filename(root, relative, NULL);
    g_assert_cmpint(g_mkdir_with_parents(path, 0700), ==, 0);
}

                                                                                      
static void hd_symlink(const gchar *root, const gchar *relative, const gchar *target)
{
    g_autofree gchar *path = g_build_filename(root, relative, NULL);
    g_autofree gchar *parent = g_path_get_dirname(path);
    g_assert_cmpint(g_mkdir_with_parents(parent, 0700), ==, 0);
    g_assert_cmpint(symlink(target, path), ==, 0);
}

                                                 
static int hd_rm_cb(const char *path, const struct stat *sb, int typeflag, struct FTW *ftwbuf)
{
    (void)sb; (void)typeflag; (void)ftwbuf;
    return remove(path);
}

                                                                             
static void hd_remove_tree(const gchar *root)
{
    g_assert_cmpint(nftw(root, hd_rm_cb, 16, FTW_DEPTH | FTW_PHYS), ==, 0);
    g_assert_false(g_file_test(root, G_FILE_TEST_EXISTS));
}

                                             
static gchar *hd_tmp_root(void)
{
    g_autoptr(GError) error = NULL;
    gchar *root = g_dir_make_tmp("pcv-hostdev-XXXXXX", &error);
    g_assert_no_error(error);
    return root;
}

                                                                 
typedef struct {
    const gchar *cls, *sub, *proto, *driver;
} IfSpec;

                                           
typedef struct {
    const gchar   *port, *vid, *pid;
    guint          bus, dev;
    const gchar   *dev_class, *mfr, *prod;
    const IfSpec  *ifs;
    guint          n_ifs;
} UsbSpec;

                                                                                    
                                      
static void hd_usb(const gchar *root, const UsbSpec *s)
{
    g_autofree gchar *base = g_build_filename("sys/bus/usb/devices", s->port, NULL);
    const struct { const gchar *file; const gchar *value; } attrs[] = {
        { "idVendor", s->vid }, { "idProduct", s->pid }, { "bDeviceClass", s->dev_class },
        { "manufacturer", s->mfr }, { "product", s->prod }, { "serial", "SERIAL-MUST-NOT-LEAK" },
    };
    for (guint i = 0; i < G_N_ELEMENTS(attrs); i++) {
        if (!attrs[i].value)
            continue;
        g_autofree gchar *rel = g_build_filename(base, attrs[i].file, NULL);
        g_autofree gchar *val = g_strdup_printf("%s\n", attrs[i].value);
        hd_write(root, rel, val);
    }
    g_autofree gchar *busrel = g_build_filename(base, "busnum", NULL);
    g_autofree gchar *busval = g_strdup_printf("%u\n", s->bus);
    hd_write(root, busrel, busval);
    g_autofree gchar *devrel = g_build_filename(base, "devnum", NULL);
    g_autofree gchar *devval = g_strdup_printf("%u\n", s->dev);
    hd_write(root, devrel, devval);
    for (guint i = 0; i < s->n_ifs; i++) {
        const IfSpec *f = &s->ifs[i];
        g_autofree gchar *ifname = g_strdup_printf("%s:1.%u", s->port, i);
        g_autofree gchar *ifbase = g_build_filename(base, ifname, NULL);
        g_autofree gchar *c = g_build_filename(ifbase, "bInterfaceClass", NULL);
        g_autofree gchar *sc = g_build_filename(ifbase, "bInterfaceSubClass", NULL);
        g_autofree gchar *pr = g_build_filename(ifbase, "bInterfaceProtocol", NULL);
        g_autofree gchar *cv = g_strdup_printf("%s\n", f->cls);
        g_autofree gchar *sv = g_strdup_printf("%s\n", f->sub);
        g_autofree gchar *pv = g_strdup_printf("%s\n", f->proto);
        hd_write(root, c, cv);
        hd_write(root, sc, sv);
        hd_write(root, pr, pv);
        if (f->driver) {
            g_autofree gchar *drel = g_build_filename(ifbase, "driver", NULL);
            g_autofree gchar *dtarget = g_strdup_printf("../../../../../../bus/usb/drivers/%s", f->driver);
            hd_symlink(root, drel, dtarget);
        }
    }
}

                                                                                
static const PcvHostUsbDevice *hd_find_usb(const PcvHostScan *scan, const gchar *port)
{
    const PcvHostUsbDevice *found = NULL;
    for (guint i = 0; i < scan->usb->len && !found; i++) {
        const PcvHostUsbDevice *d = g_ptr_array_index(scan->usb, i);
        if (g_strcmp0(d->port, port) == 0)
            found = d;
    }
    g_test_message("USB port %s", port);
    g_assert_nonnull(found);
    return found;
}

                              
static const PcvHostGpu *hd_find_gpu(const PcvHostScan *scan, const gchar *bdf)
{
    for (guint i = 0; i < scan->gpus->len; i++) {
        const PcvHostGpu *g = g_ptr_array_index(scan->gpus, i);
        if (g_strcmp0(g->fn.bdf, bdf) == 0)
            return g;
    }
    return NULL;
}

                                                                  
static void hd_assert_name(const PcvHostScan *scan, const PcvHostUsbDevice *d, const gchar *want, const gchar *want_vendor)
{
    g_autofree gchar *vendor = NULL;
    g_autofree gchar *name = pcv_host_usb_name(scan->usb_ids, d->vendor, d->product, d->manufacturer, d->product_name,
                                               &vendor);
    g_assert_cmpstr(name, ==, want);
    g_assert_cmpstr(vendor, ==, want_vendor);
}

                                                                            

static const IfSpec IF_HUB0[] = { { "09", "00", "00", "hub" } };
static const IfSpec IF_HUB2[] = { { "09", "00", "02", "hub" } };
static const IfSpec IF_ARCTIS[] = { { "01", "01", "00", "usbfs" }, { "01", "02", "00", "usbfs" }, { "01", "02", "00", "usbfs" },
                                    { "03", "00", "00", "usbfs" }, { "03", "00", "00", "usbfs" }, { "03", "00", "00", "usbfs" } };
static const IfSpec IF_TELINK[] = { { "03", "01", "02", "usbfs" }, { "03", "01", "01", "usbfs" } };
static const IfSpec IF_LOGI[] = { { "03", "01", "01", "usbhid" }, { "03", "01", "02", "usbhid" }, { "03", "00", "00", "usbhid" } };
static const IfSpec IF_COMPX[] = { { "03", "01", "01", "usbfs" }, { "03", "01", "02", "usbfs" } };
static const IfSpec IF_EGIS[] = { { "ff", "ff", "00", NULL } };
static const IfSpec IF_POROS[] = { { "01", "01", "00", "snd-usb-audio" }, { "01", "02", "00", "snd-usb-audio" },
                                   { "03", "00", "00", "usbhid" } };
static const IfSpec IF_AX200[] = { { "e0", "01", "01", "btusb" }, { "e0", "01", "01", "btusb" } };

#define IFS(a) a, G_N_ELEMENTS(a)
static const UsbSpec LOCALHOST_USB[] = {
    { "2-3",     "174c", "3274", 2, 2,  "09", "ASMT", "ASMT2307", IFS(IF_HUB0) },
    { "2-3.2",   "2109", "0817", 2, 3,  "09", "VIA Labs, Inc.         ", "USB3.0 Hub", IFS(IF_HUB0) },
    { "3-3",     "174c", "2174", 3, 2,  "09", "ASMT", "ASMT2307", IFS(IF_HUB2) },
    { "3-3.2",   "2109", "2817", 3, 3,  "09", "VIA Labs, Inc.         ", "USB2.0 Hub", IFS(IF_HUB2) },
    { "3-6",     "05e3", "0608", 3, 4,  "09", NULL, "USB2.0 Hub", IFS(IF_HUB0) },
    { "3-3.1",   "1038", "22ad", 3, 5,  "00", "SteelSeries", "Arctis Nova 7X", IFS(IF_ARCTIS) },
    { "3-3.2.2", "248a", "8327", 3, 7,  "00", "Telink", "Wireless Receiver", IFS(IF_TELINK) },
    { "3-3.2.3", "046d", "c52b", 3, 8,  "00", "Logitech", "USB Receiver", IFS(IF_LOGI) },
    { "3-3.2.4", "3554", "fa09", 3, 9,  "00", "Compx", "2.4G Wireless Receiver", IFS(IF_COMPX) },
    { "3-3.4",   "1c7a", "0577", 3, 10, "ff", "EgisTec", "EgisTec EH577", IFS(IF_EGIS) },
    { "3-7",     "1d6b", "a4a7", 3, 11, "ef", "POROSVOC", "POROSVOC", IFS(IF_POROS) },
    { "3-10",    "8087", "0029", 3, 6,  "e0", NULL, NULL, IFS(IF_AX200) },
};
static const UsbSpec ROOT_HUBS[] = {
    { "usb1", "1d6b", "0002", 1, 1, "09", "Linux 7.0.0 xhci-hcd", "xHCI Host Controller", IFS(IF_HUB0) },
    { "usb2", "1d6b", "0003", 2, 1, "09", "Linux 7.0.0 xhci-hcd", "xHCI Host Controller", IFS(IF_HUB0) },
    { "usb3", "1d6b", "0002", 3, 1, "09", "Linux 7.0.0 xhci-hcd", "xHCI Host Controller", IFS(IF_HUB0) },
    { "usb4", "1d6b", "0003", 4, 1, "09", "Linux 7.0.0 xhci-hcd", "xHCI Host Controller", IFS(IF_HUB0) },
};

                                                                  
#define USB_IDS \
    "# usb.ids fixture\n" \
    "046d  Logitech, Inc.\n\tc52b  Unifying Receiver\n" \
    "05e3  Genesys Logic, Inc.\n\t0608  Hub\n" \
    "1038  SteelSeries ApS\n" \
    "174c  ASMedia Technology Inc.\n" \
    "1c7a  LighTuning Technology Inc.\n\t0577  Fingerprint Sensor\n" \
    "1d6b  Linux Foundation\n\t0002  2.0 root hub\n\t0003  3.0 root hub\n" \
    "2109  VIA Labs, Inc.\n" \
    "248a  Maxxter\n" \
    "8087  Intel Corp.\n\t0029  AX200 Bluetooth\n" \
    "C 03  Human Interface Device\n"

                                                                                           
static void test_usb_localhost_fixture(void)
{
    g_autofree gchar *root = hd_tmp_root();
    for (guint i = 0; i < G_N_ELEMENTS(LOCALHOST_USB); i++)
        hd_usb(root, &LOCALHOST_USB[i]);
    for (guint i = 0; i < G_N_ELEMENTS(ROOT_HUBS); i++) {
        hd_usb(root, &ROOT_HUBS[i]);
        g_autofree gchar *rel = g_strdup_printf("sys/bus/usb/devices/%s/devpath", ROOT_HUBS[i].port);
        hd_write(root, rel, "0\n");
    }
    hd_mkdir(root, "sys/bus/usb/devices/1-0:1.0");                                            
    hd_write(root, "sys/bus/usb/devices/1-0:1.0/bInterfaceClass", "09\n");
    hd_symlink(root, "sys/class/bluetooth/hci0/device", "../../../bus/usb/devices/3-10/3-10:1.0");
    hd_write(root, "usr/share/misc/usb.ids", USB_IDS);

    PcvHostScan *scan = NULL;
    g_autoptr(GError) error = NULL;
    g_assert_true(pcv_host_device_scan(root, &scan, &error));
    g_assert_no_error(error);
    g_assert_false(scan->usb_absent);
    g_assert_true(scan->pci_absent);                                                              
    g_assert_true(pcv_hw_ids_available(scan->usb_ids));
    g_assert_cmpuint(scan->usb->len, ==, G_N_ELEMENTS(LOCALHOST_USB));
    for (guint i = 0; i < scan->usb->len; i++) {
        const PcvHostUsbDevice *d = g_ptr_array_index(scan->usb, i);
        g_assert_false(g_str_has_prefix(d->port, "usb"));                        
        g_assert_null(strchr(d->port, ':'));                                         
        g_assert_false(d->duplicate_on_host);                                                
        g_assert_true(g_strcmp0(d->manufacturer, "SERIAL-MUST-NOT-LEAK") != 0);
        g_assert_true(g_strcmp0(d->product_name, "SERIAL-MUST-NOT-LEAK") != 0);
    }

    const PcvHostUsbDevice *poros = hd_find_usb(scan, "3-7");                                     
    g_assert_nonnull(poros);
    g_assert_cmpuint(poros->vendor, ==, 0x1d6b);
    g_assert_cmpuint(poros->product, ==, 0xa4a7);
    g_assert_cmpuint(poros->traits, ==, PCV_HOST_USB_AUDIO);
    g_assert_true(poros->host_driver_bound);
    hd_assert_name(scan, poros, "POROSVOC", "POROSVOC");

    guint hubs = 0;
    for (guint i = 0; i < scan->usb->len; i++) {
        const PcvHostUsbDevice *d = g_ptr_array_index(scan->usb, i);
        hubs += (d->traits & (PCV_HOST_USB_HUB | PCV_HOST_USB_STORAGE)) != 0;
    }
    g_assert_cmpuint(hubs, ==, 5);                                                           

    const PcvHostUsbDevice *bt = hd_find_usb(scan, "3-10");
    g_assert_nonnull(bt);
    g_assert_cmpuint(bt->traits, ==, PCV_HOST_USB_BLUETOOTH);
    g_assert_cmpuint(bt->bus, ==, 3);
    g_assert_cmpuint(bt->devnum, ==, 6);
    g_assert_cmpstr(bt->device_class, ==, "e0");
    g_assert_null(bt->manufacturer);
    g_assert_cmpuint(bt->ifaces->len, ==, 2);
    const PcvHostUsbIface *bif = g_ptr_array_index(bt->ifaces, 0);
    g_assert_cmpstr(bif->cls, ==, "e0");
    g_assert_cmpstr(bif->sub, ==, "01");
    g_assert_cmpstr(bif->proto, ==, "01");
    g_assert_cmpstr(bif->driver, ==, "btusb");
    hd_assert_name(scan, bt, "Intel Corp. AX200 Bluetooth", "Intel Corp.");

    const PcvHostUsbDevice *telink = hd_find_usb(scan, "3-3.2.2");
    g_assert_cmpuint(telink->traits, ==, PCV_HOST_USB_KEYBOARD | PCV_HOST_USB_MOUSE);
    g_assert_false(telink->host_driver_bound);                                             
    hd_assert_name(scan, telink, "Telink Wireless Receiver", "Telink");

    const PcvHostUsbDevice *logi = hd_find_usb(scan, "3-3.2.3");
    g_assert_cmpuint(logi->traits, ==, PCV_HOST_USB_KEYBOARD | PCV_HOST_USB_MOUSE);
    g_assert_true(logi->host_driver_bound);                                              
    hd_assert_name(scan, logi, "Logitech USB Receiver", "Logitech");

    const PcvHostUsbDevice *egis = hd_find_usb(scan, "3-3.4");
    g_assert_cmpuint(egis->traits, ==, 0);
    g_assert_false(egis->host_driver_bound);                                    
    hd_assert_name(scan, egis, "EgisTec EH577", "EgisTec");                                          

    const PcvHostUsbDevice *arctis = hd_find_usb(scan, "3-3.1");
    g_assert_cmpuint(arctis->traits, ==, PCV_HOST_USB_AUDIO);                                     
    g_assert_cmpuint(arctis->ifaces->len, ==, 6);

    const PcvHostUsbDevice *via = hd_find_usb(scan, "2-3.2");
    g_assert_cmpuint(via->traits, ==, PCV_HOST_USB_HUB);
    g_assert_cmpstr(via->manufacturer, ==, "VIA Labs, Inc.");                    
    hd_assert_name(scan, via, "VIA Labs, Inc. USB3.0 Hub", "VIA Labs, Inc.");
    const PcvHostUsbDevice *genesys = hd_find_usb(scan, "3-6");
    hd_assert_name(scan, genesys, "USB2.0 Hub", "Genesys Logic, Inc.");                           
    const PcvHostUsbDevice *asm2 = hd_find_usb(scan, "3-3");
    g_assert_cmpuint(asm2->traits, ==, PCV_HOST_USB_HUB);
    hd_assert_name(scan, asm2, "ASMT2307", "ASMT");

    g_autofree gchar *absent_vendor = NULL;                                                                 
    g_autofree gchar *absent_name = pcv_host_usb_name(scan->usb_ids, 0x1038, 0x22ac, NULL, NULL, &absent_vendor);
    g_assert_null(absent_name);
    g_assert_cmpstr(absent_vendor, ==, "SteelSeries ApS");
    g_autofree gchar *hub_vendor = NULL;
    g_autofree gchar *hub_name = pcv_host_usb_name(scan->usb_ids, 0x1d6b, 0x0002, NULL, NULL, &hub_vendor);
    g_assert_cmpstr(hub_name, ==, "Linux Foundation 2.0 root hub");

    pcv_host_device_scan_free(scan);
    hd_remove_tree(root);
}

                                                                                           
static void test_usb_bluetooth_link(void)
{
    static const IfSpec IF_E0[] = { { "e0", "01", "01", "btusb" } };
    static const IfSpec IF_FF[] = { { "ff", "01", "01", "btusb" } };
    static const IfSpec IF_FF_PLAIN[] = { { "ff", "01", "01", NULL } };
    static const UsbSpec devs[] = {
        { "1-1", "8087", "0029", 1, 2, "e0", NULL, NULL, IFS(IF_E0) },
        { "1-2", "0a5c", "21e8", 1, 3, "ff", "Broadcom Corp", "BCM20702A0", IFS(IF_FF) },
        { "1-3", "0a5c", "6412", 1, 4, "ff", "Broadcom Corp", "Vendor Gadget", IFS(IF_FF_PLAIN) },
    };
    g_autofree gchar *root = hd_tmp_root();
    for (guint i = 0; i < G_N_ELEMENTS(devs); i++)
        hd_usb(root, &devs[i]);
    hd_symlink(root, "sys/class/bluetooth/hci1/device", "../../../bus/usb/devices/1-2/1-2:1.0");
    hd_mkdir(root, "sys/class/bluetooth/not-hci");                                            

    PcvHostScan *scan = NULL;
    g_assert_true(pcv_host_device_scan(root, &scan, NULL));
    g_assert_cmpuint(scan->usb->len, ==, 3);
    g_assert_cmpuint(hd_find_usb(scan, "1-1")->traits, ==, PCV_HOST_USB_BLUETOOTH);
    g_assert_cmpuint(hd_find_usb(scan, "1-2")->traits, ==, PCV_HOST_USB_BLUETOOTH);                                 
    g_assert_cmpuint(hd_find_usb(scan, "1-3")->traits, ==, 0);
    g_assert_false(pcv_hw_ids_available(scan->usb_ids));                          
    pcv_host_device_scan_free(scan);
    hd_remove_tree(root);
}

                                                                 
static void test_usb_storage_duplicate(void)
{
    static const IfSpec IF_STOR[] = { { "08", "06", "50", "usb-storage" } };
    static const IfSpec IF_NET[] = { { "ff", "ff", "00", "r8152" } };
    static const UsbSpec devs[] = {
        { "4-1", "0781", "5581", 4, 2, "00", "SanDisk", "Ultra", IFS(IF_STOR) },
        { "4-2", "0bda", "8153", 4, 3, "00", "Realtek", "USB 10/100/1000 LAN", IFS(IF_NET) },
        { "4-3", "0bda", "8153", 4, 4, "00", "Realtek", "USB 10/100/1000 LAN", IFS(IF_NET) },
    };
    g_autofree gchar *root = hd_tmp_root();
    for (guint i = 0; i < G_N_ELEMENTS(devs); i++)
        hd_usb(root, &devs[i]);
    PcvHostScan *scan = NULL;
    g_assert_true(pcv_host_device_scan(root, &scan, NULL));
    const PcvHostUsbDevice *stor = hd_find_usb(scan, "4-1");
    g_assert_cmpuint(stor->traits, ==, PCV_HOST_USB_STORAGE);
    g_assert_true(stor->host_driver_bound);
    g_assert_false(stor->duplicate_on_host);
    g_assert_true(hd_find_usb(scan, "4-2")->duplicate_on_host);
    g_assert_true(hd_find_usb(scan, "4-3")->duplicate_on_host);
    pcv_host_device_scan_free(scan);
    hd_remove_tree(root);
}

                                                                       

                                                                                         
                                            
static void hd_pci(const gchar *root, const gchar *bdf, const gchar *cls, const gchar *vendor, const gchar *device,
                   const gchar *driver, const gchar *group)
{
    g_autofree gchar *base = g_build_filename("sys/bus/pci/devices", bdf, NULL);
    const struct { const gchar *file; const gchar *value; } attrs[] = {
        { "class", cls }, { "vendor", vendor }, { "device", device },
    };
    for (guint i = 0; i < G_N_ELEMENTS(attrs); i++) {
        g_autofree gchar *rel = g_build_filename(base, attrs[i].file, NULL);
        g_autofree gchar *val = g_strdup_printf("%s\n", attrs[i].value);
        hd_write(root, rel, val);
    }
    if (driver) {
        g_autofree gchar *rel = g_build_filename(base, "driver", NULL);
        g_autofree gchar *target = g_strdup_printf("../../../bus/pci/drivers/%s", driver);
        hd_symlink(root, rel, target);
    }
    if (group) {
        g_autofree gchar *rel = g_build_filename(base, "iommu_group", NULL);
        g_autofree gchar *target = g_strdup_printf("../../../../kernel/iommu_groups/%s", group);                 
        hd_symlink(root, rel, target);
        g_autofree gchar *member = g_strdup_printf("sys/kernel/iommu_groups/%s/devices/%s", group, bdf);
        g_autofree gchar *mtarget = g_strdup_printf("../../../../devices/pci0000:00/%s", bdf);
        hd_symlink(root, member, mtarget);
    }
}

                                                                                                
                                                                                         
                                                                                        
                                                                                       
                                                                         
                                             
                                                                     
static void hd_pci_at(const gchar *root, const gchar *path, const gchar *cls, const gchar *driver, const gchar *group)
{
    g_assert_true(g_str_has_prefix(path, "sys/devices/"));
    g_autofree gchar *bdf = g_path_get_basename(path);
    const gchar *under_sys = path + strlen("sys/");
    g_auto(GStrv) parts = g_strsplit(under_sys, "/", -1);
    GString *up = g_string_new(NULL);                                            
    for (guint i = 0; parts[i]; i++)
        g_string_append(up, "../");
    g_autofree gchar *ups = g_string_free(up, FALSE);

    const struct { const gchar *file; const gchar *value; } attrs[] = {
        { "class", cls }, { "vendor", "0x1002" }, { "device", "0x73bf" },
    };
    for (guint i = 0; i < G_N_ELEMENTS(attrs); i++) {
        g_autofree gchar *rel = g_build_filename(path, attrs[i].file, NULL);
        g_autofree gchar *val = g_strdup_printf("%s\n", attrs[i].value);
        hd_write(root, rel, val);
    }
    g_autofree gchar *bus_rel = g_build_filename("sys/bus/pci/devices", bdf, NULL);
    g_autofree gchar *bus_target = g_strdup_printf("../../../%s", under_sys);
    hd_symlink(root, bus_rel, bus_target);
    if (driver) {
        g_autofree gchar *rel = g_build_filename(path, "driver", NULL);
        g_autofree gchar *target = g_strdup_printf("%sbus/pci/drivers/%s", ups, driver);
        hd_symlink(root, rel, target);
    }
    if (group) {
        g_autofree gchar *rel = g_build_filename(path, "iommu_group", NULL);
        g_autofree gchar *target = g_strdup_printf("%skernel/iommu_groups/%s", ups, group);
        hd_symlink(root, rel, target);
        g_autofree gchar *member = g_strdup_printf("sys/kernel/iommu_groups/%s/devices/%s", group, bdf);
        g_autofree gchar *mtarget = g_strdup_printf("../../../../%s", under_sys);
        hd_symlink(root, member, mtarget);
    }
}

                     
#define PCI_IDS \
    "8086  Intel Corporation\n\t46a6  Alder Lake-P GT2 [Iris Xe Graphics]\n" \
    "10de  NVIDIA Corporation\n\t2482  GA104 [GeForce RTX 3070 Ti]\n\t228b  GA104 High Definition Audio Controller\n"

                                                                                                       
static void test_gpu_fixture(void)
{
    g_autofree gchar *root = hd_tmp_root();
                                                 
    hd_pci(root, "0000:00:02.0", "0x030000", "0x8086", "0x46a6", "i915", "0");
    hd_write(root, "sys/bus/pci/devices/0000:00:02.0/boot_vga", "1\n");
    hd_write(root, "sys/bus/pci/devices/0000:00:02.0/drm/card1/card1-DP-2/status", "connected\n");
    hd_write(root, "sys/bus/pci/devices/0000:00:02.0/drm/card1/card1-HDMI-A-1/status", "disconnected\n");
    hd_mkdir(root, "sys/bus/pci/devices/0000:00:02.0/drm/renderD128");
                                           
    hd_pci(root, "0000:01:00.0", "0x030000", "0x10de", "0x2482", "vfio-pci", "17");
    hd_write(root, "sys/bus/pci/devices/0000:01:00.0/boot_vga", "0\n");
    hd_pci(root, "0000:01:00.1", "0x040300", "0x10de", "0x228b", "vfio-pci", "17");
                                  
    hd_pci(root, "0000:03:00.0", "0x030000", "0x10de", "0x2482", "vfio-pci", "30");
    hd_pci(root, "0000:00:1c.0", "0x060400", "0x8086", "0x51b8", "pcieport", "30");
                                                 
    hd_pci(root, "0000:04:00.0", "0x030000", "0x10de", "0x2482", "vfio-pci", "40");
    hd_pci(root, "0000:04:00.1", "0x040300", "0x10de", "0x228b", "snd_hda_intel", "40");
                                                          
    hd_pci(root, "0000:05:00.0", "0x030000", "0x10de", "0x2482", "vfio-pci", "50");
    hd_pci(root, "0000:05:00.1", "0x040300", "0x10de", "0x228b", "vfio-pci", "51");
                          
    hd_pci(root, "0000:06:00.0", "0x030000", "0x10de", "0x2482", "nouveau", NULL);
                                           
    hd_pci(root, "0000:07:00.0", "0x030000", "0x10de", "0x2482", "vfio-pci", "70");
    hd_write(root, "sys/bus/pci/devices/0000:07:00.0/boot_vga", "1\n");
                                 
    hd_pci(root, "0000:08:00.0", "0x020000", "0x8086", "0x125c", "igc", "80");
    hd_mkdir(root, "sys/bus/pci/devices/not-a-bdf");
    hd_write(root, "usr/share/misc/pci.ids", PCI_IDS);

    PcvHostScan *scan = NULL;
    g_autoptr(GError) error = NULL;
    g_assert_true(pcv_host_device_scan(root, &scan, &error));
    g_assert_no_error(error);
    g_assert_true(scan->usb_absent);
    g_assert_false(scan->pci_absent);
    g_assert_true(pcv_hw_ids_available(scan->pci_ids));
    g_assert_cmpuint(scan->gpus->len, ==, 7);
    g_assert_cmpuint(g_hash_table_size(scan->pci_bdfs), ==, 12);
    g_assert_true(g_hash_table_contains(scan->pci_bdfs, "0000:08:00.0"));
    for (guint i = 1; i < scan->gpus->len; i++) {                            
        const PcvHostGpu *a = g_ptr_array_index(scan->gpus, i - 1);
        const PcvHostGpu *b = g_ptr_array_index(scan->gpus, i);
        g_assert_cmpint(g_strcmp0(a->fn.bdf, b->fn.bdf), <, 0);
    }

    const PcvHostGpu *iris = hd_find_gpu(scan, "0000:00:02.0");
    g_assert_true(iris->boot_vga);
    g_assert_true(iris->display_connected);
    g_assert_true(iris->host_display);
    g_assert_cmpstr(iris->fn.driver, ==, "i915");
    g_assert_cmpstr(iris->fn.vendor_id, ==, "8086");
    g_assert_cmpstr(iris->fn.device_id, ==, "46a6");
    g_assert_cmpstr(iris->fn.class_code, ==, "030000");
    g_assert_true(iris->group_safe);
    g_assert_false(iris->group_vfio_ready);
    g_assert_cmpuint(iris->companions->len, ==, 0);

    const PcvHostGpu *rtx = hd_find_gpu(scan, "0000:01:00.0");
    g_assert_false(rtx->boot_vga);
    g_assert_false(rtx->display_connected);
    g_assert_false(rtx->host_display);
    g_assert_cmpstr(rtx->fn.iommu_group, ==, "17");
    g_assert_true(rtx->group_safe);
    g_assert_true(rtx->group_vfio_ready);
    g_assert_cmpuint(rtx->companions->len, ==, 1);
    const PcvHostPciFunc *audio = g_ptr_array_index(rtx->companions, 0);
    g_assert_cmpstr(audio->bdf, ==, "0000:01:00.1");
    g_assert_cmpstr(audio->class_code, ==, "040300");
    g_assert_cmpstr(audio->driver, ==, "vfio-pci");
    g_assert_cmpstr(audio->device_id, ==, "228b");

    const PcvHostGpu *mixed = hd_find_gpu(scan, "0000:03:00.0");
    g_assert_cmpuint(mixed->companions->len, ==, 1);                                             
    g_assert_cmpstr(((const PcvHostPciFunc *)g_ptr_array_index(mixed->companions, 0))->bdf, ==, "0000:00:1c.0");
    g_assert_false(mixed->group_safe);
    g_assert_false(mixed->group_vfio_ready);

    const PcvHostGpu *partial = hd_find_gpu(scan, "0000:04:00.0");
    g_assert_cmpuint(partial->companions->len, ==, 1);
    g_assert_true(partial->group_safe);
    g_assert_false(partial->group_vfio_ready);                                             

    const PcvHostGpu *split = hd_find_gpu(scan, "0000:05:00.0");
    g_assert_cmpuint(split->companions->len, ==, 0);                                               
    g_assert_true(split->group_safe);
    g_assert_true(split->group_vfio_ready);

    const PcvHostGpu *nogroup = hd_find_gpu(scan, "0000:06:00.0");
    g_assert_null(nogroup->fn.iommu_group);
    g_assert_cmpuint(nogroup->companions->len, ==, 0);
    g_assert_false(nogroup->group_safe);
    g_assert_false(nogroup->group_vfio_ready);
    g_assert_false(nogroup->host_display);

    const PcvHostGpu *vfio_boot = hd_find_gpu(scan, "0000:07:00.0");
    g_assert_true(vfio_boot->boot_vga);
    g_assert_false(vfio_boot->host_display);                                                               
    g_assert_true(vfio_boot->group_vfio_ready);

    g_assert_null(hd_find_gpu(scan, "0000:08:00.0"));                                   
    pcv_host_device_scan_free(scan);
    hd_remove_tree(root);
}

                                                                                      
                                                                                      
                                      
                                                               
static void test_gpu_sysfs_depth(void)
{
    g_autofree gchar *root = hd_tmp_root();
                                          
    hd_pci_at(root, "sys/devices/pci0000:00/0000:00:02.0", "0x030000", "i915", "0");
                                                 
    hd_pci_at(root, "sys/devices/pci0000:00/0000:00:01.0", "0x060400", "pcieport", "1");
    hd_pci_at(root, "sys/devices/pci0000:00/0000:00:01.0/0000:01:00.0", "0x030000", "vfio-pci", "17");
    hd_pci_at(root, "sys/devices/pci0000:00/0000:00:01.0/0000:01:00.1", "0x040300", "vfio-pci", "17");
                                                                                                    
    hd_pci_at(root, "sys/devices/pci0000:00/0000:00:01.1", "0x060400", "pcieport", "2");
    hd_pci_at(root, "sys/devices/pci0000:00/0000:00:01.1/0000:0a:00.0", "0x060400", "pcieport", "18");
    hd_pci_at(root, "sys/devices/pci0000:00/0000:00:01.1/0000:0a:00.0/0000:0b:00.0", "0x060400", "pcieport", "19");
    hd_pci_at(root, "sys/devices/pci0000:00/0000:00:01.1/0000:0a:00.0/0000:0b:00.0/0000:0c:00.0",
              "0x030000", "vfio-pci", "20");
    hd_pci_at(root, "sys/devices/pci0000:00/0000:00:01.1/0000:0a:00.0/0000:0b:00.0/0000:0c:00.1",
              "0x040300", "vfio-pci", "20");
                                      
    hd_pci_at(root, "sys/devices/pci0000:00/0000:00:03.0", "0x030000", "vfio-pci", "abc");

    PcvHostScan *scan = NULL;
    g_autoptr(GError) error = NULL;
    g_assert_true(pcv_host_device_scan(root, &scan, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(g_hash_table_size(scan->pci_bdfs), ==, 10);
    g_assert_cmpuint(scan->gpus->len, ==, 4);

    const PcvHostGpu *igpu = hd_find_gpu(scan, "0000:00:02.0");
    g_assert_nonnull(igpu);
    g_assert_cmpstr(igpu->fn.driver, ==, "i915");
    g_assert_cmpstr(igpu->fn.iommu_group, ==, "0");
    g_assert_true(igpu->group_safe);                                                                         
    g_assert_false(igpu->group_vfio_ready);
    g_assert_cmpuint(igpu->companions->len, ==, 0);

    const PcvHostGpu *rtx = hd_find_gpu(scan, "0000:01:00.0");
    g_assert_nonnull(rtx);
    g_assert_cmpstr(rtx->fn.iommu_group, ==, "17");
    g_assert_true(rtx->group_safe);
    g_assert_true(rtx->group_vfio_ready);
    g_assert_cmpuint(rtx->companions->len, ==, 1);
    const PcvHostPciFunc *rtx_audio = g_ptr_array_index(rtx->companions, 0);
    g_assert_cmpstr(rtx_audio->bdf, ==, "0000:01:00.1");
    g_assert_cmpstr(rtx_audio->iommu_group, ==, "17");
    g_assert_cmpstr(rtx_audio->driver, ==, "vfio-pci");

    const PcvHostGpu *navi = hd_find_gpu(scan, "0000:0c:00.0");
    g_assert_nonnull(navi);
    g_assert_cmpstr(navi->fn.iommu_group, ==, "20");
    g_assert_true(navi->group_safe);                                                                      
    g_assert_true(navi->group_vfio_ready);
    g_assert_cmpuint(navi->companions->len, ==, 1);
    const PcvHostPciFunc *navi_audio = g_ptr_array_index(navi->companions, 0);
    g_assert_cmpstr(navi_audio->bdf, ==, "0000:0c:00.1");
    g_assert_cmpstr(navi_audio->class_code, ==, "040300");
    g_assert_cmpstr(navi_audio->iommu_group, ==, "20");

    const PcvHostGpu *odd = hd_find_gpu(scan, "0000:00:03.0");
    g_assert_nonnull(odd);
    g_assert_null(odd->fn.iommu_group);                                                      
    g_assert_false(odd->group_safe);
    g_assert_false(odd->group_vfio_ready);
    g_assert_cmpuint(odd->companions->len, ==, 0);

    pcv_host_device_scan_free(scan);
    hd_remove_tree(root);
}

                                                                                                   
                                                                                  
                                                                      
static void test_iommu_group_dir(void)
{
    g_autofree gchar *root = hd_tmp_root();
    static const struct { const gchar *path, *group; } cases[] = {
        { "sys/devices/pci0000:00/0000:00:02.0", "0" },                                                      
        { "sys/devices/pci0000:00/0000:00:01.0/0000:01:00.0", "17" },                                        
        { "sys/devices/pci0000:00/0000:00:01.1/0000:0a:00.0/0000:0b:00.0/0000:0c:00.0", "20" },              
    };
    for (guint i = 0; i < G_N_ELEMENTS(cases); i++)
        hd_pci_at(root, cases[i].path, "0x030000", "vfio-pci", cases[i].group);
    hd_pci_at(root, "sys/devices/pci0000:00/0000:00:04.0", "0x030000", "vfio-pci", NULL);                  
    static const struct { const gchar *bdf, *target; } bad[] = {
        { "0000:00:05.0", "../../../kernel/iommu_groups/abc" },
        { "0000:00:06.0", "../../../kernel/iommu_groups/12a" },
        { "0000:00:07.0", "../../../kernel/iommu_groups/.." },
        { "0000:00:08.0", "../../../kernel/iommu_groups/" },
        { "0000:00:09.0", "../../../kernel/iommu_groups/12345678901" },                                     
    };
    for (guint i = 0; i < G_N_ELEMENTS(bad); i++) {
        g_autofree gchar *rel = g_strdup_printf("sys/devices/pci0000:00/%s/iommu_group", bad[i].bdf);
        hd_symlink(root, rel, bad[i].target);
        g_autofree gchar *bus_rel = g_strdup_printf("sys/bus/pci/devices/%s", bad[i].bdf);
        g_autofree gchar *bus_target = g_strdup_printf("../../../devices/pci0000:00/%s", bad[i].bdf);
        hd_symlink(root, bus_rel, bus_target);
    }

    for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
        g_autofree gchar *bdf = g_path_get_basename(cases[i].path);
        g_autofree gchar *dir = NULL, *id = NULL;
        g_autoptr(GError) error = NULL;
        g_test_message("depth case %s", bdf);
        g_assert_true(pcv_pci_iommu_group_dir(root, bdf, &dir, &id, &error));
        g_assert_no_error(error);
        g_autofree gchar *want = g_build_filename(root, "sys/kernel/iommu_groups", cases[i].group, NULL);
        g_assert_cmpstr(dir, ==, want);
        g_assert_cmpstr(id, ==, cases[i].group);
        g_autofree gchar *member = g_build_filename(dir, "devices", bdf, NULL);
        g_assert_true(g_file_test(member, G_FILE_TEST_IS_SYMLINK));
        g_autofree gchar *only_id = NULL;                                                     
        g_assert_true(pcv_pci_iommu_group_dir(root, bdf, NULL, &only_id, NULL));
        g_assert_cmpstr(only_id, ==, cases[i].group);
    }

    static const gchar *const absent[] = { "0000:00:04.0", "0000:00:1f.7" };                      
    for (guint i = 0; i < G_N_ELEMENTS(absent); i++) {
        gchar *dir = (gchar *)"sentinel", *id = (gchar *)"sentinel";
        g_autoptr(GError) error = NULL;
        g_assert_true(pcv_pci_iommu_group_dir(root, absent[i], &dir, &id, &error));
        g_assert_no_error(error);
        g_assert_null(dir);
        g_assert_null(id);
    }

    for (guint i = 0; i < G_N_ELEMENTS(bad); i++) {
        gchar *dir = (gchar *)"sentinel", *id = (gchar *)"sentinel";
        g_autoptr(GError) error = NULL;
        g_test_message("bad case %s -> %s", bad[i].bdf, bad[i].target);
        g_assert_false(pcv_pci_iommu_group_dir(root, bad[i].bdf, &dir, &id, &error));
        g_assert_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL);
        g_assert_null(dir);
        g_assert_null(id);
    }

    static const gchar *const not_bdf[] = { "../../../etc", "00:02.0", "", NULL };
    for (guint i = 0; i < G_N_ELEMENTS(not_bdf); i++) {
        gchar *dir = (gchar *)"sentinel";
        g_autoptr(GError) error = NULL;
        g_assert_false(pcv_pci_iommu_group_dir(root, not_bdf[i], &dir, NULL, &error));
        g_assert_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL);
        g_assert_null(dir);
    }
    hd_remove_tree(root);
}

                                                                            
static void test_source_absent_and_error(void)
{
    g_autofree gchar *empty = hd_tmp_root();
    PcvHostScan *scan = NULL;
    g_autoptr(GError) error = NULL;
    g_assert_true(pcv_host_device_scan(empty, &scan, &error));
    g_assert_no_error(error);
    g_assert_true(scan->usb_absent);
    g_assert_true(scan->pci_absent);
    g_assert_cmpuint(scan->usb->len, ==, 0);
    g_assert_cmpuint(scan->gpus->len, ==, 0);
    pcv_host_device_scan_free(scan);
    hd_remove_tree(empty);

    g_autofree gchar *usb_file = hd_tmp_root();
    hd_write(usb_file, "sys/bus/usb/devices", "not a directory\n");
    scan = (PcvHostScan *)0x1;
    g_assert_false(pcv_host_device_scan(usb_file, &scan, &error));
    g_assert_null(scan);
    g_assert_nonnull(error);
    g_assert_cmpstr(error->message, ==, "cannot read host USB devices");
    g_clear_error(&error);
    hd_remove_tree(usb_file);

    g_autofree gchar *pci_file = hd_tmp_root();
    hd_mkdir(pci_file, "sys/bus/usb/devices");
    hd_write(pci_file, "sys/bus/pci/devices", "not a directory\n");
    g_assert_false(pcv_host_device_scan(pci_file, &scan, &error));
    g_assert_null(scan);
    g_assert_cmpstr(error->message, ==, "cannot read host PCI devices");
    g_clear_error(&error);
    hd_remove_tree(pci_file);

    static const IfSpec IF_KB[] = { { "03", "01", "01", "usbhid" } };
    static const UsbSpec ok = { "5-1", "046d", "c31c", 5, 2, "00", "Logitech", "Keyboard", IFS(IF_KB) };
    g_autofree gchar *partial = hd_tmp_root();
    hd_usb(partial, &ok);
    hd_write(partial, "sys/bus/usb/devices/5-2/idProduct", "1234\n");                         
    hd_write(partial, "sys/bus/usb/devices/5-3/idVendor", "zzzz\n");                     
    hd_write(partial, "sys/bus/usb/devices/5-3/idProduct", "1234\n");
    hd_write(partial, "sys/bus/usb/devices/5-4/idVendor", "0951\n");                                       
    hd_write(partial, "sys/bus/usb/devices/5-4/idProduct", "1666\n");
    g_assert_true(pcv_host_device_scan(partial, &scan, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(scan->usb->len, ==, 2);
    const PcvHostUsbDevice *bare = hd_find_usb(scan, "5-4");
    g_assert_nonnull(bare);
    g_assert_cmpuint(bare->bus, ==, 0);
    g_assert_cmpuint(bare->devnum, ==, 0);
    g_assert_null(bare->device_class);
    g_assert_null(bare->product_name);
    g_assert_cmpuint(bare->ifaces->len, ==, 0);
    g_assert_false(bare->host_driver_bound);
    pcv_host_device_scan_free(scan);
    hd_remove_tree(partial);
}

void test_host_device_scan_register(void);
void test_host_device_scan_register(void)
{
    g_test_add_func("/host_device/scan/usb_localhost_fixture", test_usb_localhost_fixture);
    g_test_add_func("/host_device/scan/usb_bluetooth_link", test_usb_bluetooth_link);
    g_test_add_func("/host_device/scan/usb_storage_duplicate", test_usb_storage_duplicate);
    g_test_add_func("/host_device/scan/gpu_fixture", test_gpu_fixture);
    g_test_add_func("/host_device/scan/gpu_sysfs_depth", test_gpu_sysfs_depth);
    g_test_add_func("/host_device/scan/iommu_group_dir", test_iommu_group_dir);
    g_test_add_func("/host_device/scan/source_absent_and_error", test_source_absent_and_error);
}
