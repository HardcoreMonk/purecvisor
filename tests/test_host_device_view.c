   
                                
                                                                                           
  
                           
                                                                                                    
                                                                                         
                                                                             
                                                          
                                                                                                             
                                                      
                                                                               
                                           
  
                                                                                  
          
                                                                              
   
#include <ftw.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

#include "modules/dispatcher/host_device_view.h"

                                                                     

static int hv_rm_cb(const char *path, const struct stat *sb, int typeflag, struct FTW *ftwbuf)
{
    (void)sb; (void)typeflag; (void)ftwbuf;
    return remove(path);
}

                                                                        
                                 
static void hv_ids(gboolean available, PcvHwIds **usb_ids, PcvHwIds **pci_ids)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *root = g_dir_make_tmp("pcv-hostdev-view-XXXXXX", &error);
    g_assert_no_error(error);
    if (available) {
        g_autofree gchar *misc = g_build_filename(root, "usr/share/misc", NULL);
        g_assert_cmpint(g_mkdir_with_parents(misc, 0700), ==, 0);
        g_autofree gchar *u = g_build_filename(misc, "usb.ids", NULL);
        g_autofree gchar *p = g_build_filename(misc, "pci.ids", NULL);
        g_assert_true(g_file_set_contents(u, "1038  SteelSeries ApS\n248a  Maxxter\n"
                                             "8087  Intel Corp.\n\t0029  AX200 Bluetooth\n"
                                             "1d6b  Linux Foundation\n\t0002  2.0 root hub\n", -1, NULL));
        g_assert_true(g_file_set_contents(p, "10de  NVIDIA Corporation\n\t2482  GA104 [GeForce RTX 3070 Ti]\n"
                                             "\t228b  GA104 High Definition Audio Controller\n", -1, NULL));
    }
    *usb_ids = pcv_hw_ids_load(root, PCV_HW_IDS_USB);
    *pci_ids = pcv_hw_ids_load(root, PCV_HW_IDS_PCI);
    g_assert_cmpint(nftw(root, hv_rm_cb, 16, FTW_DEPTH | FTW_PHYS), ==, 0);
    g_assert_false(g_file_test(root, G_FILE_TEST_EXISTS));
}

                                          
static PcvHostScan *hv_scan(gboolean names)
{
    PcvHostScan *s = g_new0(PcvHostScan, 1);
    s->usb = g_ptr_array_new();
    s->gpus = g_ptr_array_new();
    s->pci_bdfs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    hv_ids(names, &s->usb_ids, &s->pci_ids);
    return s;
}

                                             
static PcvHostUsbDevice *hv_usb(PcvHostScan *s, const gchar *port, guint16 v, guint16 p, guint bus, guint dev,
                                guint traits, const gchar *mfr, const gchar *prod)
{
    PcvHostUsbDevice *d = g_new0(PcvHostUsbDevice, 1);
    d->port = g_strdup(port);
    d->vendor = v;
    d->product = p;
    d->bus = bus;
    d->devnum = dev;
    d->device_class = g_strdup("00");
    d->manufacturer = g_strdup(mfr);
    d->product_name = g_strdup(prod);
    d->ifaces = g_ptr_array_new();
    d->traits = traits;
    g_ptr_array_add(s->usb, d);
    return d;
}

                         
static void hv_fn(PcvHostPciFunc *fn, const gchar *bdf, const gchar *cls, const gchar *dev_id, const gchar *driver,
                  const gchar *group)
{
    fn->bdf = g_strdup(bdf);
    fn->class_code = g_strdup(cls);
    fn->vendor_id = g_strdup("10de");
    fn->device_id = g_strdup(dev_id);
    fn->driver = g_strdup(driver);
    fn->iommu_group = g_strdup(group);
}

                                                                    
static PcvHostGpu *hv_gpu(PcvHostScan *s, const gchar *bdf, const gchar *driver, gboolean host_display, gboolean safe,
                          gboolean ready)
{
    PcvHostGpu *g = g_new0(PcvHostGpu, 1);
    hv_fn(&g->fn, bdf, "030000", "2482", driver, "17");
    g->host_display = host_display;
    g->boot_vga = host_display;
    g->group_safe = safe;
    g->group_vfio_ready = ready;
    g->companions = g_ptr_array_new();
    g_ptr_array_add(s->gpus, g);
    g_hash_table_add(s->pci_bdfs, g_strdup(bdf));
    return g;
}

                                 
static void hv_companion(PcvHostScan *s, PcvHostGpu *g, const gchar *bdf)
{
    PcvHostPciFunc *fn = g_new0(PcvHostPciFunc, 1);
    hv_fn(fn, bdf, "040300", "228b", "vfio-pci", "17");
    g_ptr_array_add(g->companions, fn);
    g_hash_table_add(s->pci_bdfs, g_strdup(bdf));
}

                                                     
static PcvHostVmDef *hv_vm(GPtrArray *vms, const gchar *name, const gchar *owner, gboolean active, gboolean persistent,
                           const gchar *live_devs, const gchar *config_devs)
{
    PcvHostVmDef *v = g_new0(PcvHostVmDef, 1);
    v->name = g_strdup(name);
    v->owner = g_strdup(owner);
    v->active = active;
    v->persistent = persistent;
    v->live_xml = live_devs ? g_strdup_printf("<domain type='kvm'><name>%s</name><devices>%s</devices></domain>", name,
                                              live_devs) : NULL;
    v->config_xml = config_devs ? g_strdup_printf("<domain type='kvm'><name>%s</name><devices>%s</devices></domain>", name,
                                                  config_devs) : NULL;
    g_ptr_array_add(vms, v);
    return v;
}

static GPtrArray *hv_vms(void)
{
    return g_ptr_array_new_with_free_func(pcv_host_vm_def_free);
}

static gboolean hv_vis_all(gpointer ctx, PcvResKind kind, const gchar *id)
{
    (void)ctx; (void)kind; (void)id;
    return TRUE;
}

static gboolean hv_vis_none(gpointer ctx, PcvResKind kind, const gchar *id)
{
    (void)ctx; (void)kind; (void)id;
    return FALSE;
}

                                                                        
static gboolean hv_vis_except(gpointer ctx, PcvResKind kind, const gchar *id)
{
    g_assert_true(kind == (strlen(id) == 12 ? PCV_RES_PCI : PCV_RES_USB));
    return g_strcmp0(id, ctx) != 0;
}

static const PcvHostViewCaller ADMIN = { 2, "root", hv_vis_none, NULL };                           
static const PcvHostViewCaller ALICE = { 1, "alice", hv_vis_all, NULL };                             
static const PcvHostViewCaller VIEWER = { 0, "vic", hv_vis_all, NULL };
static const PcvHostViewCaller NOTENANT = { 1, "nob", hv_vis_none, NULL };

                       
static JsonObject *hv_build(const PcvHostScan *s, const GPtrArray *vms, const PcvHostViewCaller *c, guint max)
{
    g_autoptr(GError) error = NULL;
    JsonObject *o = pcv_host_device_view_build(s, vms, c, max, &error);
    g_assert_no_error(error);
    g_assert_nonnull(o);
    return o;
}

                                         
static JsonObject *hv_dev(JsonObject *resp, const gchar *id)
{
    JsonArray *devs = json_object_get_array_member(resp, "devices");
    for (guint i = 0; i < json_array_get_length(devs); i++) {
        JsonObject *d = json_array_get_object_element(devs, i);
        if (g_strcmp0(json_object_get_string_member(d, "id"), id) == 0)
            return d;
    }
    return NULL;
}

                 
static JsonObject *hv_asg(JsonObject *dev, guint i)
{
    JsonArray *a = json_object_get_array_member(dev, "assignments");
    g_assert_cmpuint(i, <, json_array_get_length(a));
    return json_array_get_object_element(a, i);
}

static guint hv_nasg(JsonObject *dev)
{
    return json_array_get_length(json_object_get_array_member(dev, "assignments"));
}

static gboolean hv_bool(JsonObject *o, const gchar *k)
{
    g_assert_true(json_object_has_member(o, k));
    return json_object_get_boolean_member(o, k);
}

                                  
static const gchar *hv_str(JsonObject *o, const gchar *k)
{
    g_assert_true(json_object_has_member(o, k));
    return json_object_get_null_member(o, k) ? NULL : json_object_get_string_member(o, k);
}

                                             
static gchar *hv_ids_csv(JsonObject *resp)
{
    JsonArray *devs = json_object_get_array_member(resp, "devices");
    GString *s = g_string_new(NULL);
    for (guint i = 0; i < json_array_get_length(devs); i++) {
        if (i)
            g_string_append_c(s, ',');
        g_string_append(s, json_object_get_string_member(json_array_get_object_element(devs, i), "id"));
    }
    return g_string_free(s, FALSE);
}

                                        
static gchar *hv_vm_csv(JsonObject *dev)
{
    JsonArray *a = json_object_get_array_member(dev, "assignments");
    GString *s = g_string_new(NULL);
    for (guint i = 0; i < json_array_get_length(a); i++) {
        const gchar *vm = hv_str(json_array_get_object_element(a, i), "vm");
        g_string_append_printf(s, "%s%s", i ? "," : "", vm ? vm : "-");
    }
    return g_string_free(s, FALSE);
}

                             
static gchar *hv_keys(JsonObject *o)
{
    GList *names = g_list_sort(json_object_get_members(o), (GCompareFunc)g_strcmp0);
    GString *s = g_string_new(NULL);
    for (GList *l = names; l; l = l->next)
        g_string_append_printf(s, "%s%s", l == names ? "" : ",", (const gchar *)l->data);
    g_list_free(names);
    return g_string_free(s, FALSE);
}

#define DOM(devs) devs
#define USB_ADDR(v, p, bus, dev) "<hostdev mode='subsystem' type='usb' managed='no'><source><vendor id='" v "'/>" \
    "<product id='" p "'/><address bus='" bus "' device='" dev "'/></source></hostdev>"
#define USB_PLAIN(v, p) "<hostdev mode='subsystem' type='usb' managed='no'><source><vendor id='" v "'/>" \
    "<product id='" p "'/></source></hostdev>"
#define USB_POL(pol, v, p) "<hostdev mode='subsystem' type='usb' managed='no'><source startupPolicy='" pol "'>" \
    "<vendor id='" v "'/><product id='" p "'/></source></hostdev>"
#define USB_MISSING(v, p) "<hostdev mode='subsystem' type='usb' managed='no'><source startupPolicy='optional' " \
    "missing='yes'><vendor id='" v "'/><product id='" p "'/></source></hostdev>"
#define PCI_HD(managed_attr, bus, slot, fn) "<hostdev mode='subsystem' type='pci'" managed_attr "><source><address " \
    "domain='0x0000' bus='" bus "' slot='" slot "' function='" fn "'/></source></hostdev>"
                                                                      
#define LIVE_22AC "<hostdev mode='subsystem' type='usb' managed='yes'><source><vendor id='0x1038'/><product id='0x22ac'/>" \
                  "<address bus='3' device='26'/></source><alias name='hostdev8'/><address type='usb' bus='0' port='3.4'/>" \
                  "</hostdev>"
#define INACTIVE_22AC "<hostdev mode='subsystem' type='usb' managed='yes'><source startupPolicy='optional'>" \
                      "<vendor id='0x1038'/><product id='0x22ac'/></source></hostdev>"

                                                                           

static const PcvPciHostdevEntry *hv_pci_entry(GPtrArray *a, const gchar *bdf)
{
    for (guint i = 0; i < a->len; i++) {
        const PcvPciHostdevEntry *e = g_ptr_array_index(a, i);
        if (g_strcmp0(e->bdf, bdf) == 0)
            return e;
    }
    return NULL;
}

                                                                         
static void test_pci_hostdev_parse(void)
{
    const gchar *live = "<domain><devices>"
        PCI_HD(" managed='no'", "0x01", "0x00", "0x0") PCI_HD(" managed='yes'", "0x0A", "0x00", "0x1")
        PCI_HD("", "0x02", "0x00", "0x0") USB_PLAIN("0x1234", "0x5678")
        "<hostdev mode='subsystem' type='pci'><source><address domain='0x0000' bus='0x05' slot='0x00'/></source></hostdev>"
        "</devices></domain>";
    const gchar *config = "<domain><devices>"
        PCI_HD(" managed='no'", "0x01", "0x00", "0x0") PCI_HD(" managed='no'", "0x0a", "0x00", "0x1")
        PCI_HD(" managed='no'", "0x03", "0x00", "0x0") PCI_HD(" managed='no'", "0x03", "0x00", "0x0")
        PCI_HD(" managed='no'", "0x04", "0x00", "0x9")
        "</devices></domain>";
    GPtrArray *out = NULL;
    g_autoptr(GError) error = NULL;
    g_assert_true(pcv_pci_hostdev_list(live, config, &out, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(out->len, ==, 4);
    const PcvPciHostdevEntry *e = g_ptr_array_index(out, 0);                     
    g_assert_cmpstr(e->bdf, ==, "0000:01:00.0");
    g_assert_true(e->live && e->persistent);
    g_assert_cmpstr(e->managed, ==, "no");
    e = hv_pci_entry(out, "0000:0a:00.1");                                                              
    g_assert_true(e->live && e->persistent);
    g_assert_cmpstr(e->managed, ==, "no");
    e = hv_pci_entry(out, "0000:03:00.0");                                              
    g_assert_true(!e->live && e->persistent);
    e = g_ptr_array_index(out, 3);                                                                           
    g_assert_cmpstr(e->bdf, ==, "0000:02:00.0");
    g_assert_true(e->live && !e->persistent);
    g_assert_null(e->managed);
    g_assert_null(hv_pci_entry(out, "0000:04:00.9"));                                         
    g_ptr_array_unref(out);

    g_assert_true(pcv_pci_hostdev_list(NULL, NULL, &out, &error));
    g_assert_cmpuint(out->len, ==, 0);
    g_ptr_array_unref(out);
    out = (GPtrArray *)0x1;
    g_assert_false(pcv_pci_hostdev_list("<domain><devices>", NULL, &out, &error));
    g_assert_null(out);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    g_assert_false(pcv_pci_hostdev_list(NULL, "", &out, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

                                                                       

                                                                                                             
                                                                                                          
static void test_read_outcome_table(void)
{
    static const PcvResDomState states[] = { PCV_RES_DOM_ACTIVE, PCV_RES_DOM_INACTIVE, PCV_RES_DOM_GONE,
                                             PCV_RES_DOM_UNKNOWN };
    for (guint ok = 0; ok < 2; ok++)
        for (guint nd = 0; nd < 2; nd++)
            for (guint s = 0; s < G_N_ELEMENTS(states); s++)
                for (guint live = 0; live < 2; live++) {
                    PcvHostReadOutcome want;
                    if (ok)
                        want = PCV_HOST_READ_USE;
                    else if (nd || states[s] == PCV_RES_DOM_GONE)
                        want = PCV_HOST_READ_SKIP;
                    else if (live && states[s] == PCV_RES_DOM_INACTIVE)
                        want = PCV_HOST_READ_INACTIVE_ONLY;
                    else
                        want = PCV_HOST_READ_FAIL;
                    g_test_message("ok=%u no_domain=%u state=%d live=%u", ok, nd, states[s], live);
                    g_assert_cmpint(pcv_host_device_read_outcome(ok, nd, states[s], live), ==, want);
                }
                                           
    g_assert_cmpint(pcv_host_device_read_outcome(FALSE, FALSE, PCV_RES_DOM_UNKNOWN, FALSE), ==, PCV_HOST_READ_FAIL);
    g_assert_cmpint(pcv_host_device_read_outcome(FALSE, FALSE, PCV_RES_DOM_INACTIVE, FALSE), ==, PCV_HOST_READ_FAIL);
    g_assert_cmpint(pcv_host_device_read_outcome(FALSE, TRUE, PCV_RES_DOM_INACTIVE, FALSE), ==, PCV_HOST_READ_SKIP);
    g_assert_cmpint(pcv_host_device_read_outcome(FALSE, FALSE, PCV_RES_DOM_GONE, FALSE), ==, PCV_HOST_READ_SKIP);
    g_assert_cmpint(pcv_host_device_read_outcome(FALSE, FALSE, PCV_RES_DOM_ACTIVE, TRUE), ==, PCV_HOST_READ_FAIL);
}

                                                                                            
static void test_read_error_message(void)
{
    g_autofree gchar *a = pcv_host_device_read_error_message(&ADMIN, "win-guest1");
    g_assert_cmpstr(a, ==, "cannot read the definition of VM 'win-guest1'");
    g_autofree gchar *o = pcv_host_device_read_error_message(&ALICE, "alice-vm");
    g_assert_cmpstr(o, ==, "cannot read a VM definition");
    g_autofree gchar *v = pcv_host_device_read_error_message(&VIEWER, "win-guest1");
    g_assert_cmpstr(v, ==, "cannot read a VM definition");
    g_autofree gchar *n = pcv_host_device_read_error_message(&ADMIN, NULL);
    g_assert_cmpstr(n, ==, "cannot read a VM definition");

    PcvHostScan *s = hv_scan(FALSE);
    hv_usb(s, "3-1", 0x248a, 0x8327, 3, 7, 0, "Telink", "Wireless Receiver");
    GPtrArray *vms = hv_vms();
    hv_vm(vms, "good", "alice", FALSE, TRUE, NULL, USB_PLAIN("0x248a", "0x8327"));
    PcvHostVmDef *bad = hv_vm(vms, "alice-vm", "alice", FALSE, TRUE, NULL, NULL);
    bad->config_xml = g_strdup("<domain><devices><hostdev");                                        
    const PcvHostViewCaller *callers[] = { &ADMIN, &ALICE };
    const gchar *want[] = { "cannot read the definition of VM 'alice-vm'", "cannot read a VM definition" };
    for (guint i = 0; i < G_N_ELEMENTS(callers); i++) {
        g_autoptr(GError) error = NULL;
        JsonObject *r = pcv_host_device_view_build(s, vms, callers[i], 0, &error);
        g_assert_null(r);                                                               
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
        g_assert_cmpstr(error->message, ==, want[i]);
    }
    g_ptr_array_unref(vms);
    pcv_host_device_scan_free(s);
}

                                                                         

static void test_usb_match_rules(void)
{
    PcvHostScan *s = hv_scan(TRUE);
    hv_usb(s, "3-3.2.2", 0x248a, 0x8327, 3, 7, PCV_HOST_USB_KEYBOARD | PCV_HOST_USB_MOUSE, "Telink", "Wireless Receiver");
    hv_usb(s, "3-3.1", 0x1038, 0x22ad, 3, 5, PCV_HOST_USB_AUDIO, "SteelSeries", "Arctis Nova 7X");
    PcvHostUsbDevice *k1 = hv_usb(s, "3-5", 0x0951, 0x1666, 3, 20, PCV_HOST_USB_STORAGE, "Kingston", "DataTraveler");
    PcvHostUsbDevice *k2 = hv_usb(s, "3-9", 0x0951, 0x1666, 3, 21, PCV_HOST_USB_STORAGE, "Kingston", "DataTraveler");
    k1->duplicate_on_host = k2->duplicate_on_host = TRUE;
    hv_usb(s, "3-3.4", 0x1c7a, 0x0577, 3, 10, 0, "EgisTec", "EgisTec EH577");
    GPtrArray *vms = hv_vms();
                                                                                        
    hv_vm(vms, "win-guest1", NULL, TRUE, TRUE,
          USB_ADDR("0x248a", "0x8327", "3", "7") LIVE_22AC USB_ADDR("0x1038", "0x22ad", "3", "99")
          USB_ADDR("0x0951", "0x1666", "3", "21"),
          USB_POL("optional", "0x248a", "0x8327") INACTIVE_22AC USB_POL("optional", "0x1038", "0x22ad")
          USB_POL("optional", "0x0951", "0x1666"));
    hv_vm(vms, "w-liveonly", NULL, TRUE, FALSE, USB_ADDR("0x1038", "0x22ac", "3", "26"), NULL);                       
    hv_vm(vms, "w-amb", NULL, FALSE, TRUE, NULL, USB_PLAIN("0x0951", "0x1666"));                               
    hv_vm(vms, "w-amb-stale", NULL, TRUE, FALSE, USB_ADDR("0x0951", "0x1666", "3", "30"), NULL);
    hv_vm(vms, "w-off", NULL, FALSE, TRUE, NULL, USB_PLAIN("0x046d", "0xc52b") USB_PLAIN("0x1d6b", "0x0002"));
    hv_vm(vms, "w-placeholder", NULL, TRUE, TRUE, USB_MISSING("0x1c7a", "0x0577"), USB_POL("optional", "0x1c7a", "0x0577"));
    JsonObject *r = hv_build(s, vms, &ADMIN, 0);

    JsonObject *telink = hv_dev(r, "248a:8327");                                          
    g_assert_cmpuint(hv_nasg(telink), ==, 1);
    JsonObject *a = hv_asg(telink, 0);
    g_assert_true(hv_bool(a, "live") && hv_bool(a, "persistent") && hv_bool(a, "vm_active"));
    g_assert_false(hv_bool(a, "stale_live_address"));
    g_assert_false(hv_bool(a, "ambiguous"));
    g_assert_false(hv_bool(a, "missing"));
    g_assert_cmpstr(hv_str(a, "startup_policy"), ==, "optional");

    JsonObject *gone = hv_dev(r, "1038:22ac");                                                            
    g_assert_nonnull(gone);
    g_assert_false(hv_bool(gone, "present"));
    g_assert_cmpstr(hv_str(gone, "kind"), ==, "usb");
    g_assert_cmpstr(hv_str(gone, "group"), ==, "usb");
    g_assert_cmpstr(hv_str(gone, "blocked_reason"), ==, "not_present");
    g_assert_false(hv_bool(gone, "assignable"));
    g_assert_null(hv_str(gone, "name"));                                            
    g_assert_cmpstr(hv_str(gone, "vendor_name"), ==, "SteelSeries ApS");
    g_assert_cmpstr(hv_str(gone, "vendor_id"), ==, "0x1038");
    g_assert_cmpstr(hv_str(gone, "product_id"), ==, "0x22ac");
    g_assert_cmpuint(hv_nasg(gone), ==, 2);
    a = hv_asg(gone, 0);
    g_assert_cmpstr(hv_str(a, "vm"), ==, "w-liveonly");
    g_assert_true(hv_bool(a, "live") && !hv_bool(a, "persistent") && hv_bool(a, "stale_live_address"));
    a = hv_asg(gone, 1);
    g_assert_cmpstr(hv_str(a, "vm"), ==, "win-guest1");
    g_assert_true(hv_bool(a, "live") && hv_bool(a, "persistent") && hv_bool(a, "vm_active"));
    g_assert_true(hv_bool(a, "stale_live_address"));
    g_assert_false(hv_bool(a, "missing"));
    g_assert_false(hv_bool(a, "ambiguous"));
    g_assert_cmpstr(hv_str(a, "startup_policy"), ==, "optional");                                        

    JsonObject *renum = hv_dev(r, "1038:22ad");                                                               
    g_assert_true(hv_bool(renum, "present"));
    a = hv_asg(renum, 0);
    g_assert_true(hv_bool(a, "stale_live_address"));
    g_assert_false(hv_bool(a, "ambiguous"));

    JsonArray *devs = json_object_get_array_member(r, "devices");
    JsonObject *rows[2] = { NULL, NULL };
    guint nk = 0;
    for (guint i = 0; i < json_array_get_length(devs); i++) {
        JsonObject *d = json_array_get_object_element(devs, i);
        if (g_strcmp0(json_object_get_string_member(d, "id"), "0951:1666") == 0 && nk < 2)
            rows[nk++] = d;
    }
    g_assert_cmpuint(nk, ==, 2);
    g_assert_cmpstr(hv_str(rows[0], "port"), ==, "3-5");
    g_assert_cmpstr(hv_str(rows[1], "port"), ==, "3-9");
    g_autofree gchar *k1vms = hv_vm_csv(rows[0]);
    g_autofree gchar *k2vms = hv_vm_csv(rows[1]);
    g_assert_cmpstr(k1vms, ==, "w-amb,w-amb-stale");                                                     
    g_assert_cmpstr(k2vms, ==, "w-amb,w-amb-stale,win-guest1");
    for (guint k = 0; k < 2; k++) {
        JsonObject *amb = hv_asg(rows[k], 0);                                                             
        g_assert_true(hv_bool(amb, "ambiguous"));
        g_assert_false(hv_bool(amb, "stale_live_address"));
        JsonObject *as = hv_asg(rows[k], 1);                                                         
        g_assert_true(hv_bool(as, "ambiguous"));
        g_assert_true(hv_bool(as, "stale_live_address"));
    }
    JsonObject *exact = hv_asg(rows[1], 2);
    g_assert_false(hv_bool(exact, "ambiguous"));
    g_assert_false(hv_bool(exact, "stale_live_address"));

    JsonObject *off = hv_dev(r, "046d:c52b");                                          
    g_assert_false(hv_bool(off, "present"));
    a = hv_asg(off, 0);
    g_assert_true(!hv_bool(a, "live") && hv_bool(a, "persistent") && !hv_bool(a, "vm_active"));
    g_assert_false(hv_bool(a, "stale_live_address"));
    g_assert_cmpstr(hv_str(a, "startup_policy"), ==, "mandatory");                           

    JsonObject *roothub = hv_dev(r, "1d6b:0002");                                                                   
    g_assert_nonnull(roothub);
    g_assert_false(hv_bool(roothub, "present"));
    g_assert_cmpstr(hv_str(roothub, "name"), ==, "Linux Foundation 2.0 root hub");
    g_assert_cmpstr(hv_str(hv_asg(roothub, 0), "vm"), ==, "w-off");

    JsonObject *ph = hv_dev(r, "1c7a:0577");                                                     
    a = hv_asg(ph, 0);
    g_assert_true(hv_bool(a, "missing"));
    g_assert_false(hv_bool(a, "stale_live_address"));

    json_object_unref(r);
    g_ptr_array_unref(vms);
    pcv_host_device_scan_free(s);
}

                                                                           

static void test_pci_match_managed(void)
{
    PcvHostScan *s = hv_scan(TRUE);
    PcvHostGpu *g = hv_gpu(s, "0000:01:00.0", "vfio-pci", FALSE, TRUE, TRUE);
    hv_companion(s, g, "0000:01:00.1");
    g_hash_table_add(s->pci_bdfs, g_strdup("0000:08:00.0"));                                           
    GPtrArray *vms = hv_vms();
    hv_vm(vms, "vm-a", NULL, FALSE, TRUE, NULL,
          PCI_HD(" managed='yes'", "0x01", "0x00", "0x0") PCI_HD(" managed='no'", "0x08", "0x00", "0x0")
          PCI_HD(" managed='no'", "0x09", "0x00", "0x0") PCI_HD(" managed='yes'", "0x0a", "0x00", "0x0")
          PCI_HD("", "0x0b", "0x00", "0x0"));
    JsonObject *r = hv_build(s, vms, &ADMIN, 0);
    g_autofree gchar *ids = hv_ids_csv(r);
    g_assert_cmpstr(ids, ==, "0000:01:00.0,0000:09:00.0");
    JsonObject *gpu = hv_dev(r, "0000:01:00.0");
    JsonObject *a = hv_asg(gpu, 0);
    g_assert_cmpstr(hv_str(a, "managed"), ==, "yes");                                                 
    g_assert_true(!hv_bool(a, "live") && hv_bool(a, "persistent"));
    JsonObject *gone = hv_dev(r, "0000:09:00.0");
    g_assert_cmpstr(hv_str(gone, "kind"), ==, "pci");
    g_assert_cmpstr(hv_str(gone, "group"), ==, "gpu");
    g_assert_false(hv_bool(gone, "present"));
    g_assert_false(hv_bool(gone, "assignable"));
    g_assert_cmpstr(hv_str(gone, "blocked_reason"), ==, "not_present");
    g_assert_cmpstr(hv_str(gone, "pci_addr"), ==, "0000:09:00.0");
    g_assert_cmpstr(hv_str(hv_asg(gone, 0), "managed"), ==, "no");
    json_object_unref(r);
    g_ptr_array_unref(vms);
    pcv_host_device_scan_free(s);
}

                                                          
static void test_companions_partial(void)
{
    PcvHostScan *s = hv_scan(TRUE);
    PcvHostGpu *g1 = hv_gpu(s, "0000:01:00.0", "vfio-pci", FALSE, TRUE, TRUE);
    hv_companion(s, g1, "0000:01:00.1");
    PcvHostGpu *g2 = hv_gpu(s, "0000:02:00.0", "vfio-pci", FALSE, TRUE, TRUE);
    hv_companion(s, g2, "0000:02:00.1");
    hv_gpu(s, "0000:03:00.0", "vfio-pci", FALSE, TRUE, TRUE);
    GPtrArray *vms = hv_vms();
    hv_vm(vms, "vm-a", NULL, FALSE, TRUE, NULL,
          PCI_HD(" managed='no'", "0x01", "0x00", "0x0") PCI_HD(" managed='no'", "0x01", "0x00", "0x1"));
    hv_vm(vms, "vm-e", NULL, TRUE, TRUE, PCI_HD(" managed='no'", "0x01", "0x00", "0x0"),                        
          PCI_HD(" managed='no'", "0x01", "0x00", "0x0") PCI_HD(" managed='no'", "0x01", "0x00", "0x1"));
    hv_vm(vms, "vm-b", NULL, FALSE, TRUE, NULL, PCI_HD(" managed='no'", "0x02", "0x00", "0x0"));
    hv_vm(vms, "vm-c", NULL, FALSE, TRUE, NULL, PCI_HD(" managed='no'", "0x03", "0x00", "0x0"));
    JsonObject *r = hv_build(s, vms, &ADMIN, 0);

    JsonObject *d1 = hv_dev(r, "0000:01:00.0");
    g_assert_false(hv_bool(d1, "partial_assignment"));
    JsonArray *comps = json_object_get_array_member(d1, "companions");
    g_assert_cmpuint(json_array_get_length(comps), ==, 1);
    JsonObject *c = json_array_get_object_element(comps, 0);
    g_assert_cmpstr(hv_str(c, "pci_addr"), ==, "0000:01:00.1");
    g_assert_cmpstr(hv_str(c, "class_code"), ==, "040300");
    g_assert_cmpstr(hv_str(c, "name"), ==, "NVIDIA Corporation GA104 High Definition Audio Controller");
    g_autofree gchar *cvms = hv_vm_csv(c);
    g_assert_cmpstr(cvms, ==, "vm-a,vm-e");
    g_assert_cmpstr(hv_str(hv_asg(c, 0), "managed"), ==, "no");
    g_assert_true(hv_bool(hv_dev(r, "0000:02:00.0"), "partial_assignment"));                           
    g_assert_false(hv_bool(hv_dev(r, "0000:03:00.0"), "partial_assignment"));             
    json_object_unref(r);
    g_ptr_array_unref(vms);
    pcv_host_device_scan_free(s);
}

                                                                          

                                                                                        
                              
static guint hv_walk_names(JsonObject *r, const gchar *allowed, guint *hidden_out)
{
    JsonArray *devs = json_object_get_array_member(r, "devices");
    guint shown = 0, hidden = 0;
    for (guint i = 0; i < json_array_get_length(devs); i++) {
        JsonObject *d = json_array_get_object_element(devs, i);
        GPtrArray *lists = g_ptr_array_new();
        g_ptr_array_add(lists, json_object_get_array_member(d, "assignments"));
        if (json_object_has_member(d, "companions")) {
            JsonArray *cs = json_object_get_array_member(d, "companions");
            for (guint k = 0; k < json_array_get_length(cs); k++)
                g_ptr_array_add(lists, json_object_get_array_member(json_array_get_object_element(cs, k), "assignments"));
        }
        for (guint l = 0; l < lists->len; l++) {
            JsonArray *as = g_ptr_array_index(lists, l);
            for (guint k = 0; k < json_array_get_length(as); k++) {
                JsonObject *a = json_array_get_object_element(as, k);
                const gchar *vm = hv_str(a, "vm");
                g_assert_cmpint(hv_bool(a, "vm_visible"), ==, vm != NULL);
                if (vm) {
                    g_assert_cmpstr(vm, ==, allowed);
                    shown++;
                } else {
                    hidden++;
                    g_assert_true(json_object_has_member(a, "live") && json_object_has_member(a, "vm_active"));
                }
            }
        }
        g_ptr_array_unref(lists);
    }
    *hidden_out = hidden;
    return shown;
}

static gchar *hv_vms_csv(JsonObject *r)
{
    JsonArray *a = json_object_get_array_member(r, "vms");
    GString *s = g_string_new(NULL);
    for (guint i = 0; i < json_array_get_length(a); i++)
        g_string_append_printf(s, "%s%s", i ? "," : "",
                               json_object_get_string_member(json_array_get_object_element(a, i), "name"));
    return g_string_free(s, FALSE);
}

static void test_visibility(void)
{
    PcvHostScan *s = hv_scan(TRUE);
    PcvHostGpu *g = hv_gpu(s, "0000:01:00.0", "vfio-pci", FALSE, TRUE, TRUE);
    hv_companion(s, g, "0000:01:00.1");
    hv_usb(s, "3-3.2.2", 0x248a, 0x8327, 3, 7, 0, "Telink", "Wireless Receiver");
    GPtrArray *vms = hv_vms();
    hv_vm(vms, "win", "alice", TRUE, TRUE, USB_ADDR("0x248a", "0x8327", "3", "7"), USB_POL("optional", "0x248a", "0x8327"));
    hv_vm(vms, "other", "bob", FALSE, TRUE, NULL,                                                         
          PCI_HD(" managed='no'", "0x01", "0x00", "0x0") PCI_HD(" managed='no'", "0x01", "0x00", "0x1")
          INACTIVE_22AC PCI_HD(" managed='no'", "0x09", "0x00", "0x0"));
    hv_vm(vms, "orphan", NULL, FALSE, TRUE, NULL, USB_PLAIN("0x248a", "0x8327"));               
    guint hidden = 0;

    JsonObject *r = hv_build(s, vms, &ADMIN, 0);                                                     
    g_autofree gchar *ids = hv_ids_csv(r);
    g_assert_cmpstr(ids, ==, "0000:01:00.0,0000:09:00.0,248a:8327,1038:22ac");
    JsonArray *devs = json_object_get_array_member(r, "devices");
    guint named = 0;
    for (guint i = 0; i < json_array_get_length(devs); i++) {
        g_autofree gchar *names = hv_vm_csv(json_array_get_object_element(devs, i));
        named += strchr(names, '-') == NULL;
    }
    g_assert_cmpuint(named, ==, 4);
    g_autofree gchar *avms = hv_vms_csv(r);
    g_assert_cmpstr(avms, ==, "orphan,other,win");
    json_object_unref(r);

    r = hv_build(s, vms, &ALICE, 0);                                                                        
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(r, "devices")), ==, 4);
    g_assert_cmpuint(hv_walk_names(r, "win", &hidden), ==, 1);
    g_assert_cmpuint(hidden, ==, 5);                                                                   
    JsonObject *comp = json_array_get_object_element(json_object_get_array_member(hv_dev(r, "0000:01:00.0"), "companions"), 0);
    JsonObject *ca = hv_asg(comp, 0);
    g_assert_null(hv_str(ca, "vm"));
    g_assert_false(hv_bool(ca, "vm_visible"));
    g_assert_true(hv_bool(ca, "persistent"));                                                     
    g_autofree gchar *telink_vms = hv_vm_csv(hv_dev(r, "248a:8327"));
    g_assert_cmpstr(telink_vms, ==, "win,-");                                         
    g_autofree gchar *ovms = hv_vms_csv(r);
    g_assert_cmpstr(ovms, ==, "win");
    json_object_unref(r);

    r = hv_build(s, vms, &VIEWER, 0);                                                                
    g_assert_cmpuint(hv_walk_names(r, NULL, &hidden), ==, 0);
    g_assert_cmpuint(hidden, ==, 6);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(r, "vms")), ==, 0);
    json_object_unref(r);

    r = hv_build(s, vms, &NOTENANT, 0);                                                              
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(r, "devices")), ==, 0);
    g_autofree gchar *nvms = hv_vms_csv(r);
    g_assert_cmpstr(nvms, ==, "");                                                      
    json_object_unref(r);

    PcvHostViewCaller part = { 1, "alice", hv_vis_except, (gpointer)"0000:01:00.0" };                 
    r = hv_build(s, vms, &part, 0);
    g_autofree gchar *pids = hv_ids_csv(r);
    g_assert_cmpstr(pids, ==, "0000:09:00.0,248a:8327,1038:22ac");                              
    json_object_unref(r);
    g_ptr_array_unref(vms);
    pcv_host_device_scan_free(s);
}

                                                                       

static void test_blocked_and_order(void)
{
    PcvHostScan *s = hv_scan(FALSE);
    hv_gpu(s, "0000:07:00.0", "vfio-pci", FALSE, TRUE, TRUE);                                
    hv_gpu(s, "0000:00:02.0", "i915", TRUE, FALSE, FALSE);                                  
    hv_gpu(s, "0000:03:00.0", "vfio-pci", FALSE, FALSE, FALSE);                             
    hv_gpu(s, "0000:04:00.0", "vfio-pci", FALSE, TRUE, FALSE);
    hv_usb(s, "3-10", 0x8087, 0x0029, 3, 6, PCV_HOST_USB_BLUETOOTH, NULL, NULL);
    PcvHostUsbDevice *h1 = hv_usb(s, "3-3", 0x2109, 0x2815, 3, 2, PCV_HOST_USB_HUB, "VIA Labs, Inc.", "USB2.0 Hub");
    PcvHostUsbDevice *h2 = hv_usb(s, "3-3.10", 0x2109, 0x2815, 3, 4, PCV_HOST_USB_HUB, "VIA Labs, Inc.", "USB2.0 Hub");
    h1->duplicate_on_host = h2->duplicate_on_host = TRUE;                                      
    PcvHostUsbDevice *d1 = hv_usb(s, "3-3.2", 0x0bda, 0x8153, 3, 5, 0, "Realtek", "LAN");
    PcvHostUsbDevice *d2 = hv_usb(s, "3-9", 0x0bda, 0x8153, 3, 9, 0, "Realtek", "LAN");
    d1->duplicate_on_host = d2->duplicate_on_host = TRUE;
    hv_usb(s, "2-1", 0x0781, 0x5581, 2, 2, PCV_HOST_USB_STORAGE, "SanDisk", "Ultra");
    hv_usb(s, "3-2", 0x046d, 0xc52b, 3, 3, PCV_HOST_USB_KEYBOARD, "Logitech", "USB Receiver");
    GPtrArray *vms = hv_vms();
    hv_vm(vms, "zeta", "alice", FALSE, TRUE, NULL, USB_PLAIN("0x046d", "0xc52b") USB_PLAIN("0xffff", "0x0002")
          PCI_HD(" managed='no'", "0x0c", "0x00", "0x0"));
    hv_vm(vms, "alpha", "alice", FALSE, TRUE, NULL, USB_PLAIN("0x046d", "0xc52b") USB_PLAIN("0x1038", "0x22ac")
          PCI_HD(" managed='no'", "0x0b", "0x00", "0x0"));
    hv_vm(vms, "mid", "bob", FALSE, TRUE, NULL, USB_PLAIN("0x046d", "0xc52b"));
    JsonObject *r = hv_build(s, vms, &ALICE, 0);

    g_autofree gchar *ids = hv_ids_csv(r);                                                                       
    g_assert_cmpstr(ids, ==, "0000:00:02.0,0000:03:00.0,0000:04:00.0,0000:07:00.0,0000:0b:00.0,0000:0c:00.0,"
                             "0781:5581,046d:c52b,2109:2815,0bda:8153,2109:2815,0bda:8153,1038:22ac,ffff:0002,8087:0029");
    JsonArray *devs = json_object_get_array_member(r, "devices");
    const gchar *ports[] = { "2-1", "3-2", "3-3", "3-3.2", "3-3.10", "3-9" };               
    for (guint i = 0; i < G_N_ELEMENTS(ports); i++)
        g_assert_cmpstr(hv_str(json_array_get_object_element(devs, 6 + i), "port"), ==, ports[i]);

    const struct { const gchar *id; const gchar *reason; } gpus[] = {
        { "0000:00:02.0", "host_display" }, { "0000:03:00.0", "iommu_group_unsafe" },
        { "0000:04:00.0", "not_vfio_bound" }, { "0000:07:00.0", NULL },
    };
    for (guint i = 0; i < G_N_ELEMENTS(gpus); i++) {
        JsonObject *d = hv_dev(r, gpus[i].id);
        g_assert_cmpstr(hv_str(d, "blocked_reason"), ==, gpus[i].reason);
        g_assert_cmpint(hv_bool(d, "assignable"), ==, gpus[i].reason == NULL);
        g_assert_false(hv_bool(d, "hidden_by_default"));
    }
    JsonObject *hub = json_array_get_object_element(devs, 8);
    g_assert_cmpstr(hv_str(hub, "blocked_reason"), ==, "hub");
    g_assert_true(hv_bool(hub, "hidden_by_default"));
    g_assert_true(hv_bool(hub, "duplicate_on_host"));
    JsonObject *lan = json_array_get_object_element(devs, 9);
    g_assert_cmpstr(hv_str(lan, "blocked_reason"), ==, "duplicate_on_host");
    g_assert_false(hv_bool(lan, "assignable"));
    g_assert_false(hv_bool(lan, "hidden_by_default"));
    JsonObject *stor = hv_dev(r, "0781:5581");
    g_assert_true(hv_bool(stor, "hidden_by_default"));
    g_assert_true(hv_bool(stor, "assignable"));                                                
    JsonObject *bt = hv_dev(r, "8087:0029");
    g_assert_cmpstr(hv_str(bt, "group"), ==, "bluetooth");
    g_assert_null(hv_str(bt, "blocked_reason"));

    g_autofree gchar *order = hv_vm_csv(hv_dev(r, "046d:c52b"));                         
    g_assert_cmpstr(order, ==, "alpha,zeta,-");
    json_object_unref(r);
    r = hv_build(s, vms, &ADMIN, 0);
    g_autofree gchar *aorder = hv_vm_csv(hv_dev(r, "046d:c52b"));
    g_assert_cmpstr(aorder, ==, "alpha,mid,zeta");
    g_autofree gchar *vorder = hv_vms_csv(r);
    g_assert_cmpstr(vorder, ==, "alpha,mid,zeta");
    json_object_unref(r);
    g_ptr_array_unref(vms);
    pcv_host_device_scan_free(s);
}

                                                                         

static void test_truncate_and_sources(void)
{
    PcvHostScan *s = hv_scan(FALSE);
    hv_gpu(s, "0000:01:00.0", "vfio-pci", FALSE, TRUE, TRUE);
    hv_usb(s, "3-1", 0x248a, 0x8327, 3, 7, 0, "Telink", "Wireless Receiver");
    hv_usb(s, "3-2", 0x3554, 0xfa09, 3, 8, 0, "Compx", "2.4G Wireless Receiver");
    s->usb_absent = FALSE;
    GPtrArray *vms = hv_vms();
    for (guint i = 0; i < 6; i++) {                                                                
        g_autofree gchar *name = g_strdup_printf("vm-%u", i);
        hv_vm(vms, name, NULL, FALSE, TRUE, NULL, i == 5 ? PCI_HD(" managed='no'", "0x01", "0x00", "0x0") : "");
    }
    hv_vm(vms, "vm-gone", NULL, FALSE, TRUE, NULL, USB_PLAIN("0x1038", "0x22ac"));
    JsonObject *r = hv_build(s, vms, &ADMIN, 2);
    g_assert_true(hv_bool(r, "truncated"));
    g_autofree gchar *ids = hv_ids_csv(r);
    g_assert_cmpstr(ids, ==, "0000:01:00.0,248a:8327");
    g_autofree gchar *gvms = hv_vm_csv(hv_dev(r, "0000:01:00.0"));
    g_assert_cmpstr(gvms, ==, "vm-5");
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(r, "vms")), ==, 7);                     
    JsonObject *src = json_object_get_object_member(r, "sources");
    g_assert_cmpstr(hv_str(src, "usb"), ==, "available");
    g_assert_cmpstr(hv_str(src, "pci"), ==, "available");
    g_assert_cmpstr(hv_str(src, "names"), ==, "unavailable");
    g_assert_null(hv_str(hv_dev(r, "0000:01:00.0"), "name"));
    json_object_unref(r);

    r = hv_build(s, vms, &ADMIN, 0);                                                           
    g_assert_false(hv_bool(r, "truncated"));
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(r, "devices")), ==, 4);
    json_object_unref(r);
    r = hv_build(s, vms, &ADMIN, 4);                                                       
    g_assert_false(hv_bool(r, "truncated"));
    json_object_unref(r);
    g_ptr_array_unref(vms);
    pcv_host_device_scan_free(s);

    PcvHostScan *absent = hv_scan(TRUE);                                                                
    absent->usb_absent = TRUE;
    GPtrArray *vms2 = hv_vms();
    hv_vm(vms2, "w", NULL, FALSE, TRUE, NULL, USB_PLAIN("0x248a", "0x8327"));
    r = hv_build(absent, vms2, &ADMIN, 0);
    src = json_object_get_object_member(r, "sources");
    g_assert_cmpstr(hv_str(src, "usb"), ==, "absent");
    g_assert_cmpstr(hv_str(src, "pci"), ==, "available");
    g_assert_cmpstr(hv_str(src, "names"), ==, "available");
    g_assert_false(hv_bool(hv_dev(r, "248a:8327"), "present"));
    json_object_unref(r);
    g_ptr_array_unref(vms2);
    pcv_host_device_scan_free(absent);
}

                                                                     

static void hv_assert_keys(JsonObject *o, const gchar *want)
{
    g_autofree gchar *got = hv_keys(o);
    g_assert_cmpstr(got, ==, want);
}

static void test_json_shape(void)
{
    PcvHostScan *s = hv_scan(TRUE);
    PcvHostGpu *g = hv_gpu(s, "0000:01:00.0", "vfio-pci", FALSE, TRUE, TRUE);
    hv_companion(s, g, "0000:01:00.1");
    PcvHostUsbDevice *bt = hv_usb(s, "3-10", 0x8087, 0x0029, 3, 6, PCV_HOST_USB_BLUETOOTH, NULL, NULL);
    PcvHostUsbIface *f = g_new0(PcvHostUsbIface, 1);
    f->cls = g_strdup("e0");
    f->sub = g_strdup("01");
    f->proto = g_strdup("01");
    f->driver = g_strdup("btusb");
    g_ptr_array_add(bt->ifaces, f);
    PcvHostUsbIface *f2 = g_new0(PcvHostUsbIface, 1);                                           
    f2->cls = g_strdup("e0");
    f2->sub = g_strdup("01");
    f2->proto = g_strdup("01");
    g_ptr_array_add(bt->ifaces, f2);
    bt->host_driver_bound = TRUE;
    GPtrArray *vms = hv_vms();
    hv_vm(vms, "win-guest1", "alice", TRUE, TRUE,
          PCI_HD(" managed='no'", "0x01", "0x00", "0x0") PCI_HD(" managed='no'", "0x01", "0x00", "0x1") LIVE_22AC,
          PCI_HD(" managed='no'", "0x01", "0x00", "0x0") PCI_HD(" managed='no'", "0x01", "0x00", "0x1") INACTIVE_22AC
          USB_PLAIN("0x8087", "0x0029") PCI_HD(" managed='no'", "0x09", "0x00", "0x0"));
    JsonObject *r = hv_build(s, vms, &ADMIN, 0);
    hv_assert_keys(r, "devices,sources,truncated,vms");
    hv_assert_keys(json_object_get_object_member(r, "sources"), "names,pci,usb");
    hv_assert_keys(json_array_get_object_element(json_object_get_array_member(r, "vms"), 0), "active,name,persistent");

    JsonObject *gpu = hv_dev(r, "0000:01:00.0");
    hv_assert_keys(gpu, "assignable,assignments,blocked_reason,boot_vga,companions,device_id,display_connected,driver,"
                        "group,group_safe,group_vfio_ready,hidden_by_default,host_display,id,iommu_group,kind,name,"
                        "partial_assignment,pci_addr,present,vendor_id,vendor_name");
    g_assert_cmpstr(hv_str(gpu, "name"), ==, "NVIDIA Corporation GA104 [GeForce RTX 3070 Ti]");
    g_assert_cmpstr(hv_str(gpu, "vendor_name"), ==, "NVIDIA Corporation");
    g_assert_cmpstr(hv_str(gpu, "iommu_group"), ==, "17");
    JsonObject *comp = json_array_get_object_element(json_object_get_array_member(gpu, "companions"), 0);
    hv_assert_keys(comp, "assignments,class_code,driver,name,pci_addr");
    hv_assert_keys(hv_asg(gpu, 0), "live,managed,persistent,vm,vm_active,vm_visible");
    hv_assert_keys(hv_asg(comp, 0), "live,managed,persistent,vm,vm_active,vm_visible");

    JsonObject *usb = hv_dev(r, "8087:0029");
    hv_assert_keys(usb, "assignable,assignments,blocked_reason,bus,device,device_class,duplicate_on_host,group,"
                        "hidden_by_default,host_driver_bound,id,interfaces,kind,name,port,present,product_id,traits,"
                        "vendor_id,vendor_name");
    g_assert_cmpint(json_object_get_int_member(usb, "bus"), ==, 3);
    g_assert_cmpint(json_object_get_int_member(usb, "device"), ==, 6);
    g_assert_cmpstr(hv_str(usb, "name"), ==, "Intel Corp. AX200 Bluetooth");
    g_assert_cmpstr(hv_str(usb, "vendor_id"), ==, "0x8087");
    g_assert_cmpstr(hv_str(usb, "product_id"), ==, "0x0029");
    JsonArray *traits = json_object_get_array_member(usb, "traits");
    g_assert_cmpuint(json_array_get_length(traits), ==, 1);
    g_assert_cmpstr(json_array_get_string_element(traits, 0), ==, "bluetooth");
    JsonArray *ifs = json_object_get_array_member(usb, "interfaces");
    g_assert_cmpuint(json_array_get_length(ifs), ==, 2);
    hv_assert_keys(json_array_get_object_element(ifs, 0), "class,driver,protocol,subclass");
    g_assert_cmpstr(hv_str(json_array_get_object_element(ifs, 0), "driver"), ==, "btusb");
    g_assert_null(hv_str(json_array_get_object_element(ifs, 1), "driver"));
    hv_assert_keys(hv_asg(usb, 0), "ambiguous,live,missing,persistent,stale_live_address,startup_policy,vm,vm_active,"
                                   "vm_visible");

    hv_assert_keys(hv_dev(r, "1038:22ac"), "assignable,assignments,blocked_reason,group,hidden_by_default,id,kind,name,"
                                           "present,product_id,vendor_id,vendor_name");
    hv_assert_keys(hv_dev(r, "0000:09:00.0"), "assignable,assignments,blocked_reason,group,hidden_by_default,id,kind,"
                                              "name,pci_addr,present,vendor_name");

    g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(node, r);
    g_autofree gchar *text = json_to_string(node, FALSE);
    g_assert_null(strstr(text, "serial"));                                                            
    json_object_unref(r);
    g_ptr_array_unref(vms);
    pcv_host_device_scan_free(s);
}

void test_host_device_view_register(void);
void test_host_device_view_register(void)
{
    g_test_add_func("/host_device/view/pci_hostdev_parse", test_pci_hostdev_parse);
    g_test_add_func("/host_device/view/read_outcome_table", test_read_outcome_table);
    g_test_add_func("/host_device/view/read_error_message", test_read_error_message);
    g_test_add_func("/host_device/view/usb_match_rules", test_usb_match_rules);
    g_test_add_func("/host_device/view/pci_match_managed", test_pci_match_managed);
    g_test_add_func("/host_device/view/companions_partial", test_companions_partial);
    g_test_add_func("/host_device/view/visibility", test_visibility);
    g_test_add_func("/host_device/view/blocked_and_order", test_blocked_and_order);
    g_test_add_func("/host_device/view/truncate_and_sources", test_truncate_and_sources);
    g_test_add_func("/host_device/view/json_shape", test_json_shape);
}
