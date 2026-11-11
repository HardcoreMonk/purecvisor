   
                               
                                                     
  
                           
                                                                       
                                                                                  
                                                              
                                                                    
                                                                                           
                                                                                                          
               
                                                                                                            
                                                                      
  
                                                            
                                                                
   
#include <glib.h>
#include <gio/gio.h>
#include <string.h>

#include "modules/dispatcher/hotplug_usb_xml.h"

                                               
#define DOM(devs) "<domain type='kvm'><name>t</name><devices>" devs "</devices></domain>"
#define USB(v, p) "<hostdev mode='subsystem' type='usb' managed='no'><source>" \
                  "<vendor id='" v "'/><product id='" p "'/></source></hostdev>"
#define USB_POL(pol, v, p) "<hostdev mode='subsystem' type='usb' managed='no'>" \
                  "<source startupPolicy='" pol "'><vendor id='" v "'/><product id='" p "'/>" \
                  "</source></hostdev>"
#define USB_LIVE(v, p) "<hostdev mode='subsystem' type='usb' managed='no'><source>" \
                  "<vendor id='" v "'/><product id='" p "'/><address bus='3' device='5'/>" \
                  "</source><alias name='hostdev0'/><address type='usb' bus='0' port='1'/></hostdev>"
#define USB_MISSING(pol, v, p) "<hostdev mode='subsystem' type='usb' managed='no'>" \
                  "<source startupPolicy='" pol "' missing='yes'><vendor id='" v "'/>" \
                  "<product id='" p "'/></source></hostdev>"
#define PCI_HOSTDEV "<hostdev mode='subsystem' type='pci' managed='yes'><source>" \
                  "<address domain='0x0000' bus='0x01' slot='0x00' function='0x0'/></source></hostdev>"
#define NO_USB DOM("<disk type='file' device='disk'><target dev='vda' bus='virtio'/></disk>")

                                         
typedef struct {
    const gchar *vendor_id;
    const gchar *product_id;
    gboolean     live;
    gboolean     persistent;
    gboolean     missing;
    const gchar *policy;
} WantEntry;

                                       
typedef struct {
    const gchar *name;
    const gchar *live_xml;
    const gchar *config_xml;
    guint        n;
    WantEntry    want[4];
} ListCase;

                                          
static guint16 hex16(const gchar *s)
{
    return (guint16)g_ascii_strtoull(s + 2, NULL, 16);
}

                                                                
static void check_list(const ListCase *c)
{
    GPtrArray *entries = NULL;
    GError *error = NULL;

    g_test_message("list case: %s", c->name);
    gboolean ok = pcv_usb_hostdev_list(c->live_xml, c->config_xml, &entries, &error);
    g_assert_no_error(error);
    g_assert_true(ok);
    g_assert_nonnull(entries);
    g_assert_cmpuint(entries->len, ==, c->n);
    for (guint i = 0; i < c->n; i++) {
        const PcvUsbHostdevEntry *e = g_ptr_array_index(entries, i);
        const WantEntry *w = &c->want[i];
        g_test_message("  entry %u", i);
        g_assert_cmpstr(e->vendor_id, ==, w->vendor_id);
        g_assert_cmpstr(e->product_id, ==, w->product_id);
        g_assert_cmpuint(e->vendor, ==, hex16(w->vendor_id));
        g_assert_cmpuint(e->product, ==, hex16(w->product_id));
        g_assert_cmpint(e->live, ==, w->live);
        g_assert_cmpint(e->persistent, ==, w->persistent);
        g_assert_cmpint(e->missing, ==, w->missing);
        g_assert_cmpstr(e->startup_policy, ==, w->policy);
    }
    g_ptr_array_unref(entries);
}

                            
static void test_list_merge_table(void)
{
    static const ListCase cases[] = {
        { "live_only", DOM(USB_LIVE("0x1234", "0x5678")), NO_USB, 1,
          { { "0x1234", "0x5678", TRUE, FALSE, FALSE, "mandatory" } } },
        { "persistent_only", NO_USB, DOM(USB_POL("optional", "0x1234", "0x5678")), 1,
          { { "0x1234", "0x5678", FALSE, TRUE, FALSE, "optional" } } },
                                              
        { "both_persistent_policy_wins", DOM(USB_LIVE("0x1234", "0x5678")),
          DOM(USB_POL("optional", "0x1234", "0x5678")), 1,
          { { "0x1234", "0x5678", TRUE, TRUE, FALSE, "optional" } } },
        { "placeholder_missing", DOM(USB_MISSING("optional", "0x1234", "0x5678")),
          DOM(USB_POL("optional", "0x1234", "0x5678")), 1,
          { { "0x1234", "0x5678", TRUE, TRUE, TRUE, "optional" } } },
        { "no_policy_attr_is_mandatory", DOM(USB("0x1234", "0x5678")), DOM(USB("0x1234", "0x5678")), 1,
          { { "0x1234", "0x5678", TRUE, TRUE, FALSE, "mandatory" } } },
        { "attribute_and_child_order", NO_USB,
          DOM("<hostdev type='usb' managed='no' mode='subsystem'><source>"
              "<product id='0x5678'/><vendor id='0x1234'/></source></hostdev>"), 1,
          { { "0x1234", "0x5678", FALSE, TRUE, FALSE, "mandatory" } } },
        { "double_quotes", NULL,
          DOM("<hostdev mode=\"subsystem\" type=\"usb\"><source startupPolicy=\"requisite\">"
              "<vendor id=\"0x1234\"/><product id=\"0x5678\"/></source></hostdev>"), 1,
          { { "0x1234", "0x5678", FALSE, TRUE, FALSE, "requisite" } } },
        { "type_on_next_line",
          "<domain type='kvm'>\n  <devices>\n    <hostdev mode='subsystem'\n"
          "             type='usb' managed='no'>\n      <source>\n"
          "        <vendor id='0x1234'/>\n        <product id='0x5678'/>\n"
          "      </source>\n    </hostdev>\n  </devices>\n</domain>\n",
          NO_USB, 1,
          { { "0x1234", "0x5678", TRUE, FALSE, FALSE, "mandatory" } } },
        { "pci_hostdev_ignored", DOM(PCI_HOSTDEV USB("0x1234", "0x5678")), DOM(PCI_HOSTDEV), 1,
          { { "0x1234", "0x5678", TRUE, FALSE, FALSE, "mandatory" } } },
        { "transient_config_null", DOM(USB_LIVE("0x1234", "0x5678")), NULL, 1,
          { { "0x1234", "0x5678", TRUE, FALSE, FALSE, "mandatory" } } },
        { "stopped_live_null", NULL, DOM(USB_POL("optional", "0x1234", "0x5678")), 1,
          { { "0x1234", "0x5678", FALSE, TRUE, FALSE, "optional" } } },
                                        
        { "uppercase_ids_pair", DOM(USB("0X1234", "0X5678")),
          DOM(USB_POL("optional", "0x1234", "0x5678")), 1,
          { { "0x1234", "0x5678", TRUE, TRUE, FALSE, "optional" } } },
                                            
        { "order_persistent_then_unpaired_live",
          DOM(USB("0x0003", "0x0003") USB("0x0002", "0x0002")),
          DOM(USB("0x0001", "0x0001") USB("0x0002", "0x0002")), 3,
          { { "0x0001", "0x0001", FALSE, TRUE, FALSE, "mandatory" },
            { "0x0002", "0x0002", TRUE, TRUE, FALSE, "mandatory" },
            { "0x0003", "0x0003", TRUE, FALSE, FALSE, "mandatory" } } },
                                                                   
        { "unusable_entries_skipped", NO_USB,
          DOM("<hostdev mode='subsystem' type='usb'/>"
              "<hostdev mode='subsystem' type='usb'><source><address bus='3' device='5'/></source></hostdev>"
              USB("0xZZZZ", "0x5678")
              USB("0x1234", "0x5678")), 1,
          { { "0x1234", "0x5678", FALSE, TRUE, FALSE, "mandatory" } } },
    };
    for (guint i = 0; i < G_N_ELEMENTS(cases); i++)
        check_list(&cases[i]);
}

                                                               
static void test_list_duplicate_pairing(void)
{
    static const ListCase cases[] = {
                                                                         
        { "persistent_two_live_one", DOM(USB("0x1234", "0x5678")),
          DOM(USB_POL("optional", "0x1234", "0x5678") USB_POL("mandatory", "0x1234", "0x5678")), 2,
          { { "0x1234", "0x5678", TRUE, TRUE, FALSE, "optional" },
            { "0x1234", "0x5678", FALSE, TRUE, FALSE, "mandatory" } } },
                                                       
        { "persistent_one_live_two", DOM(USB("0x1234", "0x5678") USB_MISSING("optional", "0x1234", "0x5678")),
          DOM(USB_POL("optional", "0x1234", "0x5678")), 2,
          { { "0x1234", "0x5678", TRUE, TRUE, FALSE, "optional" },
            { "0x1234", "0x5678", TRUE, FALSE, TRUE, "optional" } } },
    };
    for (guint i = 0; i < G_N_ELEMENTS(cases); i++)
        check_list(&cases[i]);
}

                                                   
static void test_list_parse_errors(void)
{
    static const struct { const gchar *live; const gchar *config; } bad[] = {
        { "<domain><devices><hostdev", DOM(USB("0x1234", "0x5678")) },
        { DOM(USB("0x1234", "0x5678")), "<domain><devices></domain>" },
        { "", NULL },
        { NULL, "" },
    };
    for (guint i = 0; i < G_N_ELEMENTS(bad); i++) {
        GPtrArray *entries = NULL;
        GError *error = NULL;
        g_test_message("bad case %u", i);
        g_assert_false(pcv_usb_hostdev_list(bad[i].live, bad[i].config, &entries, &error));
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
        g_assert_null(entries);
        g_clear_error(&error);
    }

    GPtrArray *entries = NULL;
    GError *error = NULL;
    g_assert_true(pcv_usb_hostdev_list(NULL, NULL, &entries, &error));
    g_assert_no_error(error);
    g_assert_nonnull(entries);
    g_assert_cmpuint(entries->len, ==, 0);
    g_ptr_array_unref(entries);
}

                                                                                                          
                                                                    
#define LIVE_22AC "<hostdev mode='subsystem' type='usb' managed='yes'><source><vendor id='0x1038'/><product id='0x22ac'/>" \
                  "<address bus='3' device='26'/></source><alias name='hostdev8'/><address type='usb' bus='0' port='3.4'/>" \
                  "</hostdev>"
#define INACTIVE_22AC "<hostdev mode='subsystem' type='usb' managed='yes'><source startupPolicy='optional'>" \
                      "<vendor id='0x1038'/><product id='0x22ac'/></source></hostdev>"

                                                            
typedef struct {
    const gchar *name;
    const gchar *live_xml;
    const gchar *config_xml;
    guint        n;
    struct { const gchar *product_id; guint bus; guint device; gboolean live; gboolean persistent; } want[2];
} AddrCase;

                                                                                                     
                                                                             
                                                            
                                                                
static void test_list_live_address(void)
{
    static const AddrCase cases[] = {
        { "windows11_22ac_verbatim", DOM(LIVE_22AC), DOM(INACTIVE_22AC), 1,
          { { "0x22ac", 3, 26, TRUE, TRUE } } },
        { "persistent_only_address_ignored", NULL,
          DOM("<hostdev mode='subsystem' type='usb' managed='no'><source startupPolicy='optional'><vendor id='0x1038'/>"
              "<product id='0x22ac'/><address bus='7' device='9'/></source></hostdev>"), 1,
          { { "0x22ac", 0, 0, FALSE, TRUE } } },
        { "guest_side_address_not_device", DOM("<hostdev mode='subsystem' type='usb' managed='no'><source>"
              "<vendor id='0x1038'/><product id='0x22ac'/></source><alias name='hostdev1'/>"
              "<address type='usb' bus='1' port='2'/></hostdev>"), NULL, 1,
          { { "0x22ac", 0, 0, TRUE, FALSE } } },
        { "non_decimal_field_is_zero", DOM("<hostdev mode='subsystem' type='usb' managed='no'><source>"
              "<vendor id='0x1038'/><product id='0x22ac'/><address bus='x' device='26'/></source></hostdev>"), NULL, 1,
          { { "0x22ac", 0, 26, TRUE, FALSE } } },
        { "duplicate_pairs_keep_own_address",
          DOM("<hostdev mode='subsystem' type='usb' managed='no'><source><vendor id='0x1038'/><product id='0x22ac'/>"
              "<address bus='3' device='5'/></source></hostdev>"
              "<hostdev mode='subsystem' type='usb' managed='no'><source><vendor id='0x1038'/><product id='0x22ac'/>"
              "<address bus='3' device='6'/></source></hostdev>"),
          DOM(USB_POL("optional", "0x1038", "0x22ac") USB_POL("mandatory", "0x1038", "0x22ac")), 2,
          { { "0x22ac", 3, 5, TRUE, TRUE }, { "0x22ac", 3, 6, TRUE, TRUE } } },
    };
    for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
        const AddrCase *c = &cases[i];
        GPtrArray *entries = NULL;
        GError *error = NULL;
        g_test_message("address case: %s", c->name);
        g_assert_true(pcv_usb_hostdev_list(c->live_xml, c->config_xml, &entries, &error));
        g_assert_no_error(error);
        g_assert_cmpuint(entries->len, ==, c->n);
        for (guint k = 0; k < c->n; k++) {
            const PcvUsbHostdevEntry *e = g_ptr_array_index(entries, k);
            g_assert_cmpstr(e->product_id, ==, c->want[k].product_id);
            g_assert_cmpuint(e->live_bus, ==, c->want[k].bus);
            g_assert_cmpuint(e->live_device, ==, c->want[k].device);
            g_assert_cmpint(e->live, ==, c->want[k].live);
            g_assert_cmpint(e->persistent, ==, c->want[k].persistent);
        }
        if (i == 0) {                                                         
            const PcvUsbHostdevEntry *e = g_ptr_array_index(entries, 0);
            g_assert_cmpstr(e->vendor_id, ==, "0x1038");
            g_assert_cmpstr(e->startup_policy, ==, "optional");
            g_assert_false(e->missing);
        }
        g_ptr_array_unref(entries);
    }
}

                                                                          

#define POL_DIFF(old) "USB device 0x1234:0x5678 is already in the persistent definition of VM 'vm1' " \
                      "with startupPolicy '" old "'; detach it first to change the policy"
#define MULTI "multiple USB hostdev entries match 0x1234:0x5678 in VM 'vm1'; nothing was changed"
#define PLACEHOLDER "USB device 0x1234:0x5678 is assigned to VM 'vm1' but missing on the host " \
                    "(startupPolicy placeholder); detach and attach it again, or restart the VM"

                                                          
typedef struct {
    const gchar     *name;
    const gchar     *live_xml;
    const gchar     *config_xml;
    gboolean         is_active;
    gboolean         has_persistent;
    PcvUsbPolicy     policy;
    gboolean         ok;
    PcvUsbAttachKind kind;
    const gchar     *saved_policy;                                      
    const gchar     *message;                                                      
} AttachCase;

                                                                
                                           
static void check_attach_cases(const AttachCase *cases, guint n, gboolean persist)
{
    for (guint i = 0; i < n; i++) {
        const AttachCase *c = &cases[i];
        PcvUsbAttachKind kind = PCV_USB_ATTACH_TWO_STEP;
        gchar *saved = NULL;
        GError *error = NULL;
        g_test_message("attach case (persist=%d): %s", persist, c->name);
        gboolean ok = pcv_usb_attach_plan(c->live_xml, c->config_xml, c->is_active, c->has_persistent,
                                          0x1234, 0x5678, c->policy, persist, "vm1", &kind, &saved, &error);
        g_assert_cmpint(ok, ==, c->ok);
        if (c->ok) {
            g_assert_no_error(error);
            g_assert_cmpint(kind, ==, c->kind);
            g_assert_cmpstr(saved, ==, c->saved_policy);
        } else {
            g_assert_error(error, PCV_USB_ERROR, PCV_USB_ERROR_CONFLICT);
            g_assert_cmpstr(error->message, ==, c->message);
            g_assert_null(saved);
            g_clear_error(&error);
        }
        g_free(saved);
    }
}

                                                                                                
                                                                                                         
static void test_attach_plan_table(void)
{
    static const AttachCase cases[] = {
        { "running_none_none_two_step", NO_USB, NO_USB, TRUE, TRUE, PCV_USB_POLICY_OPTIONAL,
          TRUE, PCV_USB_ATTACH_TWO_STEP, "optional", NULL },
        { "running_persistent_same_live_only", NO_USB, DOM(USB_POL("optional", "0x1234", "0x5678")), TRUE, TRUE,
          PCV_USB_POLICY_OPTIONAL, TRUE, PCV_USB_ATTACH_LIVE_ONLY, "optional", NULL },
        { "running_live_only_config_legacy", DOM(USB_LIVE("0x1234", "0x5678")), NO_USB, TRUE, TRUE, PCV_USB_POLICY_OPTIONAL,
          TRUE, PCV_USB_ATTACH_CONFIG_LEGACY, "optional", NULL },
        { "running_both_same_noop", DOM(USB_LIVE("0x1234", "0x5678")), DOM(USB_POL("optional", "0x1234", "0x5678")), TRUE, TRUE,
          PCV_USB_POLICY_OPTIONAL, TRUE, PCV_USB_ATTACH_NOOP, "optional", NULL },
        { "stopped_none_config", NULL, NO_USB, FALSE, TRUE, PCV_USB_POLICY_OPTIONAL,
          TRUE, PCV_USB_ATTACH_CONFIG_STOPPED, "optional", NULL },
        { "stopped_same_noop", NULL, DOM(USB_POL("optional", "0x1234", "0x5678")), FALSE, TRUE, PCV_USB_POLICY_OPTIONAL,
          TRUE, PCV_USB_ATTACH_NOOP, "optional", NULL },
        { "transient_none_live_only", NO_USB, NULL, TRUE, FALSE, PCV_USB_POLICY_OPTIONAL,
          TRUE, PCV_USB_ATTACH_LIVE_ONLY, NULL, NULL },
        { "transient_present_noop", DOM(USB_LIVE("0x1234", "0x5678")), NULL, TRUE, FALSE, PCV_USB_POLICY_OPTIONAL,
          TRUE, PCV_USB_ATTACH_NOOP, NULL, NULL },
        { "running_policy_differs", DOM(USB_LIVE("0x1234", "0x5678")), DOM(USB_POL("optional", "0x1234", "0x5678")), TRUE, TRUE,
          PCV_USB_POLICY_MANDATORY, FALSE, PCV_USB_ATTACH_NOOP, NULL, POL_DIFF("optional") },
        { "stopped_policy_differs", NULL, DOM(USB_POL("requisite", "0x1234", "0x5678")), FALSE, TRUE, PCV_USB_POLICY_OPTIONAL,
          FALSE, PCV_USB_ATTACH_NOOP, NULL, POL_DIFF("requisite") },
        { "running_placeholder", DOM(USB_MISSING("optional", "0x1234", "0x5678")), DOM(USB_POL("optional", "0x1234", "0x5678")), TRUE, TRUE,
          PCV_USB_POLICY_OPTIONAL, FALSE, PCV_USB_ATTACH_NOOP, NULL, PLACEHOLDER },
        { "multiple_in_persistent", NO_USB, DOM(USB_POL("optional", "0x1234", "0x5678") USB_POL("optional", "0x1234", "0x5678")), TRUE, TRUE,
          PCV_USB_POLICY_OPTIONAL, FALSE, PCV_USB_ATTACH_NOOP, NULL, MULTI },
        { "multiple_in_live", DOM(USB_LIVE("0x1234", "0x5678") USB("0x1234", "0x5678")), NO_USB, TRUE, TRUE, PCV_USB_POLICY_OPTIONAL,
          FALSE, PCV_USB_ATTACH_NOOP, NULL, MULTI },
                
        { "uppercase_live_matches", DOM(USB("0X1234", "0X5678")), NO_USB, TRUE, TRUE, PCV_USB_POLICY_OPTIONAL,
          TRUE, PCV_USB_ATTACH_CONFIG_LEGACY, "optional", NULL },
        { "uppercase_persistent_matches", NO_USB, DOM(USB_POL("optional", "0X1234", "0X5678")), TRUE, TRUE,
          PCV_USB_POLICY_OPTIONAL, TRUE, PCV_USB_ATTACH_LIVE_ONLY, "optional", NULL },
        { "persistent_no_attr_is_mandatory", NO_USB, DOM(USB("0x1234", "0x5678")), TRUE, TRUE, PCV_USB_POLICY_MANDATORY,
          TRUE, PCV_USB_ATTACH_LIVE_ONLY, "mandatory", NULL },
        { "persistent_no_attr_vs_optional", NULL, DOM(USB("0x1234", "0x5678")), FALSE, TRUE, PCV_USB_POLICY_OPTIONAL,
          FALSE, PCV_USB_ATTACH_NOOP, NULL, POL_DIFF("mandatory") },
        { "pci_and_other_ids_ignored", DOM(PCI_HOSTDEV USB("0x1234", "0x9999")),
          DOM(PCI_HOSTDEV USB("0x4321", "0x5678")), TRUE, TRUE, PCV_USB_POLICY_REQUISITE,
          TRUE, PCV_USB_ATTACH_TWO_STEP, "requisite", NULL },
        { "stopped_ignores_live_xml", DOM(USB("0x1234", "0x5678")), NO_USB, FALSE, TRUE, PCV_USB_POLICY_OPTIONAL,
          TRUE, PCV_USB_ATTACH_CONFIG_STOPPED, "optional", NULL },
        { "transient_ignores_config_xml", NO_USB, DOM(USB_POL("mandatory", "0x1234", "0x5678")), TRUE, FALSE,
          PCV_USB_POLICY_OPTIONAL, TRUE, PCV_USB_ATTACH_LIVE_ONLY, NULL, NULL },
    };
    check_attach_cases(cases, G_N_ELEMENTS(cases), TRUE);                                                 

                                                                  
    PcvUsbAttachKind kind = PCV_USB_ATTACH_NOOP;
    gchar *saved = NULL;
    GError *error = NULL;
    g_assert_false(pcv_usb_attach_plan("<domain><devices>", NO_USB, TRUE, TRUE, 0x1234, 0x5678,
                                       PCV_USB_POLICY_OPTIONAL, TRUE, "vm1", &kind, &saved, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_assert_null(saved);
    g_clear_error(&error);
}

#define STOPPED "USB device 0x1234:0x5678 cannot be attached to VM 'vm1' without saving it: the VM is not running"

                                                                                                            
                                                                        
                                                                                                      
                                        
                                                                               
static void test_attach_plan_live_only_table(void)
{
    static const AttachCase cases[] = {
        { "live_none_persistent_none", NO_USB, NO_USB, TRUE, TRUE, PCV_USB_POLICY_OPTIONAL,
          TRUE, PCV_USB_ATTACH_LIVE_ONLY, NULL, NULL },
        { "live_none_persistent_present", NO_USB, DOM(USB_POL("requisite", "0x1234", "0x5678")), TRUE, TRUE,
          PCV_USB_POLICY_OPTIONAL, TRUE, PCV_USB_ATTACH_LIVE_ONLY, "requisite", NULL },
        { "live_present_persistent_none", DOM(USB_LIVE("0x1234", "0x5678")), NO_USB, TRUE, TRUE, PCV_USB_POLICY_OPTIONAL,
          TRUE, PCV_USB_ATTACH_NOOP, NULL, NULL },
        { "live_present_persistent_present", DOM(USB_LIVE("0x1234", "0x5678")), DOM(USB_POL("optional", "0x1234", "0x5678")),
          TRUE, TRUE, PCV_USB_POLICY_OPTIONAL, TRUE, PCV_USB_ATTACH_NOOP, "optional", NULL },
        { "placeholder", DOM(USB_MISSING("optional", "0x1234", "0x5678")), DOM(USB_POL("optional", "0x1234", "0x5678")),
          TRUE, TRUE, PCV_USB_POLICY_OPTIONAL, FALSE, PCV_USB_ATTACH_NOOP, NULL, PLACEHOLDER },
        { "multiple_in_persistent", NO_USB, DOM(USB_POL("optional", "0x1234", "0x5678") USB_POL("optional", "0x1234", "0x5678")),
          TRUE, TRUE, PCV_USB_POLICY_OPTIONAL, FALSE, PCV_USB_ATTACH_NOOP, NULL, MULTI },
                                                        
        { "stopped_none", NULL, NO_USB, FALSE, TRUE, PCV_USB_POLICY_OPTIONAL, FALSE, PCV_USB_ATTACH_NOOP, NULL, STOPPED },
        { "stopped_present", NULL, DOM(USB_POL("optional", "0x1234", "0x5678")), FALSE, TRUE, PCV_USB_POLICY_OPTIONAL,
          FALSE, PCV_USB_ATTACH_NOOP, NULL, STOPPED },
                                                           
        { "transient_none", NO_USB, NULL, TRUE, FALSE, PCV_USB_POLICY_OPTIONAL, TRUE, PCV_USB_ATTACH_LIVE_ONLY, NULL, NULL },
        { "transient_present", DOM(USB_LIVE("0x1234", "0x5678")), NULL, TRUE, FALSE, PCV_USB_POLICY_OPTIONAL,
          TRUE, PCV_USB_ATTACH_NOOP, NULL, NULL },
                                                                                               
        { "persistent_policy_differs_skipped", NO_USB, DOM(USB_POL("mandatory", "0x1234", "0x5678")), TRUE, TRUE,
          PCV_USB_POLICY_OPTIONAL, TRUE, PCV_USB_ATTACH_LIVE_ONLY, "mandatory", NULL },
    };
    check_attach_cases(cases, G_N_ELEMENTS(cases), FALSE);
}

                                                                         
static void test_hostdev_xml_build(void)
{
    g_autofree gchar *plain = pcv_usb_hostdev_xml(0x1234, 0xABCD, NULL);
    g_assert_true(g_str_has_prefix(plain, "<hostdev mode='subsystem' type='usb'>"));
    g_assert_null(strstr(plain, "startupPolicy"));
    g_assert_nonnull(strstr(plain, "<vendor id='0x1234'/>"));
    g_assert_nonnull(strstr(plain, "<product id='0xabcd'/>"));

    PcvUsbPolicy pol = PCV_USB_POLICY_REQUISITE;
    g_autofree gchar *with = pcv_usb_hostdev_xml(0x0001, 0x0002, &pol);
    g_assert_nonnull(strstr(with, "<source startupPolicy='requisite'>"));
    g_assert_nonnull(strstr(with, "<vendor id='0x0001'/>"));
    g_assert_nonnull(strstr(with, "<product id='0x0002'/>"));

    g_autofree gchar *dom = g_strdup_printf("<domain><devices>%s</devices></domain>", with);
    GPtrArray *entries = NULL;
    GError *error = NULL;
    g_assert_true(pcv_usb_hostdev_list(NULL, dom, &entries, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(entries->len, ==, 1);
    const PcvUsbHostdevEntry *e = g_ptr_array_index(entries, 0);
    g_assert_cmpuint(e->vendor, ==, 1);
    g_assert_cmpuint(e->product, ==, 2);
    g_assert_cmpstr(e->startup_policy, ==, "requisite");
    g_ptr_array_unref(entries);
}

                                                            
static void test_startup_policy_parse(void)
{
    static const struct { const gchar *s; PcvUsbPolicy p; } good[] = {
        { "mandatory", PCV_USB_POLICY_MANDATORY },
        { "requisite", PCV_USB_POLICY_REQUISITE },
        { "optional", PCV_USB_POLICY_OPTIONAL },
    };
    for (guint i = 0; i < G_N_ELEMENTS(good); i++) {
        PcvUsbPolicy out = good[i].p == PCV_USB_POLICY_MANDATORY ? PCV_USB_POLICY_OPTIONAL : PCV_USB_POLICY_MANDATORY;
        g_assert_true(pcv_usb_policy_parse(good[i].s, &out));
        g_assert_cmpint(out, ==, good[i].p);
        g_assert_cmpstr(pcv_usb_policy_name(good[i].p), ==, good[i].s);
    }
    static const gchar *const bad[] = { "", "Optional", "optional ", " optional", "OPTIONAL", "none", "optional\n" };
    for (guint i = 0; i < G_N_ELEMENTS(bad); i++) {
        PcvUsbPolicy out = PCV_USB_POLICY_REQUISITE;
        g_test_message("bad policy %u", i);
        g_assert_false(pcv_usb_policy_parse(bad[i], &out));
        g_assert_cmpint(out, ==, PCV_USB_POLICY_REQUISITE);
    }
    PcvUsbPolicy out = PCV_USB_POLICY_REQUISITE;
    g_assert_false(pcv_usb_policy_parse(NULL, &out));
    g_assert_cmpint(out, ==, PCV_USB_POLICY_REQUISITE);
}

                                                                
static void test_id_value(void)
{
    g_assert_cmpuint(pcv_usb_id_value("0x1234"), ==, 0x1234);
    g_assert_cmpuint(pcv_usb_id_value("0XABCD"), ==, 0xabcd);
    g_assert_cmpuint(pcv_usb_id_value("0x00aF"), ==, 0x00af);
    g_assert_cmpuint(pcv_usb_id_value("0xffff"), ==, 0xffff);
    g_assert_cmpuint(pcv_usb_id_value("0x0000"), ==, 0);
}

                                                                          

#define NOT_ATTACHED_MSG "USB device 0x1234:0x5678 is not attached to VM 'vm1' (live or persistent)"

                                                                  
typedef struct {
    const gchar *name;
    const gchar *live_xml;
    const gchar *config_xml;
    gboolean     is_active;
    gboolean     has_persistent;
    gboolean     ok;
    gboolean     live;
    gboolean     config;
    gint         code;                                           
    const gchar *message;
} DetachCase;

                                                                              
static void test_detach_plan_table(void)
{
    static const DetachCase cases[] = {
        { "running_both", DOM(USB_LIVE("0x1234", "0x5678")), DOM(USB_POL("optional", "0x1234", "0x5678")),
          TRUE, TRUE, TRUE, TRUE, TRUE, 0, NULL },
        { "running_placeholder_both", DOM(USB_MISSING("optional", "0x1234", "0x5678")),
          DOM(USB_POL("optional", "0x1234", "0x5678")), TRUE, TRUE, TRUE, TRUE, TRUE, 0, NULL },
        { "running_legacy_live_only", DOM(USB_LIVE("0x1234", "0x5678")), NO_USB,
          TRUE, TRUE, TRUE, TRUE, FALSE, 0, NULL },
        { "running_transient_live", DOM(USB_LIVE("0x1234", "0x5678")), NULL,
          TRUE, FALSE, TRUE, TRUE, FALSE, 0, NULL },
        { "running_persistent_only", NO_USB, DOM(USB_POL("optional", "0x1234", "0x5678")),
          TRUE, TRUE, TRUE, FALSE, TRUE, 0, NULL },
        { "stopped_persistent", NULL, DOM(USB_POL("requisite", "0x1234", "0x5678")),
          FALSE, TRUE, TRUE, FALSE, TRUE, 0, NULL },
        { "nowhere", NO_USB, DOM(PCI_HOSTDEV USB("0x1234", "0x9999")), TRUE, TRUE,
          FALSE, FALSE, FALSE, PCV_USB_ERROR_NOT_ATTACHED, NOT_ATTACHED_MSG },
        { "stopped_ignores_live_xml", DOM(USB("0x1234", "0x5678")), NO_USB, FALSE, TRUE,
          FALSE, FALSE, FALSE, PCV_USB_ERROR_NOT_ATTACHED, NOT_ATTACHED_MSG },
        { "multiple_in_persistent", NO_USB, DOM(USB("0x1234", "0x5678") USB("0x1234", "0x5678")), TRUE, TRUE,
          FALSE, FALSE, FALSE, PCV_USB_ERROR_CONFLICT, MULTI },
        { "multiple_in_live", DOM(USB("0x1234", "0x5678") USB_LIVE("0x1234", "0x5678")), NO_USB, TRUE, TRUE,
          FALSE, FALSE, FALSE, PCV_USB_ERROR_CONFLICT, MULTI },
        { "uppercase_live_matches", DOM(USB("0X1234", "0X5678")), NO_USB, TRUE, TRUE,
          TRUE, TRUE, FALSE, 0, NULL },
        { "policy_ignored", DOM(USB("0x1234", "0x5678")), DOM(USB_POL("mandatory", "0x1234", "0x5678")),
          TRUE, TRUE, TRUE, TRUE, TRUE, 0, NULL },
    };
    for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
        const DetachCase *c = &cases[i];
        gboolean live = TRUE, config = TRUE;
        GError *error = NULL;
        g_test_message("detach case: %s", c->name);
        gboolean ok = pcv_usb_detach_plan(c->live_xml, c->config_xml, c->is_active, c->has_persistent,
                                          0x1234, 0x5678, "vm1", &live, &config, &error);
        g_assert_cmpint(ok, ==, c->ok);
        g_assert_cmpint(live, ==, c->live);
        g_assert_cmpint(config, ==, c->config);
        if (c->ok) {
            g_assert_no_error(error);
        } else {
            g_assert_error(error, PCV_USB_ERROR, c->code);
            g_assert_cmpstr(error->message, ==, c->message);
            g_clear_error(&error);
        }
    }

    gboolean live = TRUE, config = TRUE;
    GError *error = NULL;
    g_assert_false(pcv_usb_detach_plan(NO_USB, "<domain>", TRUE, TRUE, 0x1234, 0x5678, "vm1",
                                       &live, &config, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_assert_false(live);
    g_assert_false(config);
    g_clear_error(&error);
}

void test_hotplug_usb_xml_register(void)
{
    g_test_add_func("/hotplug/usb_xml/list_merge_table", test_list_merge_table);
    g_test_add_func("/hotplug/usb_xml/list_duplicate_pairing", test_list_duplicate_pairing);
    g_test_add_func("/hotplug/usb_xml/list_parse_errors", test_list_parse_errors);
    g_test_add_func("/hotplug/usb_xml/list_live_address", test_list_live_address);
    g_test_add_func("/hotplug/usb_xml/attach_plan_table", test_attach_plan_table);
    g_test_add_func("/hotplug/usb_xml/attach_plan_live_only_table", test_attach_plan_live_only_table);
    g_test_add_func("/hotplug/usb_xml/hostdev_xml_build", test_hostdev_xml_build);
    g_test_add_func("/hotplug/usb_xml/startup_policy_parse", test_startup_policy_parse);
    g_test_add_func("/hotplug/usb_xml/id_value", test_id_value);
    g_test_add_func("/hotplug/usb_xml/detach_plan_table", test_detach_plan_table);
}
