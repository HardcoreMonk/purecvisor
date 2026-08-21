   
                               
                                                
  
                           
                                                                  
                                                               
                                                              
  
                                                   
                                       
                                                         
                                
   
#include <glib.h>
#include <gio/gio.h>
#include <string.h>

#include "modules/dispatcher/hotplug_nic_xml.h"

static const gchar *DOMAIN_XML =
    "<domain><devices>"
    "<interface type='bridge'><mac address='52:54:00:11:22:33'/>"
    "<source bridge='virbr0'/><model type='virtio'/></interface>"
    "<interface type='bridge'><mac address='52:54:00:AA:BB:CC'/>"
    "<source bridge='pcvbr0'/><model type='virtio'/><mtu size='1500'/>"
    "<filterref filter='clean-traffic'/></interface>"
    "</devices></domain>";

static void test_select_preserves_complete_interface(void)
{
    GError *error = NULL;
    gchar *xml = pcv_hotplug_select_nic_xml(DOMAIN_XML,
                                             "52:54:00:aa:bb:cc", &error);

    g_assert_no_error(error);
    g_assert_nonnull(xml);
    g_assert_nonnull(strstr(xml, "bridge=\"pcvbr0\""));
    g_assert_nonnull(strstr(xml, "type=\"virtio\""));
    g_assert_nonnull(strstr(xml, "size=\"1500\""));
    g_assert_nonnull(strstr(xml, "filter=\"clean-traffic\""));
    g_assert_null(strstr(xml, "virbr0"));
    g_free(xml);
}

static void test_select_reports_missing_mac(void)
{
    GError *error = NULL;
    gchar *xml = pcv_hotplug_select_nic_xml(DOMAIN_XML,
                                             "52:54:00:00:00:01", &error);

    g_assert_null(xml);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error(&error);
}

static void test_select_rejects_malformed_xml(void)
{
    GError *error = NULL;
    gchar *xml = pcv_hotplug_select_nic_xml("<domain><devices>",
                                             "52:54:00:aa:bb:cc", &error);

    g_assert_null(xml);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
}

void test_hotplug_nic_xml_register(void)
{
    g_test_add_func("/hotplug/nic_xml/preserves_complete_interface",
                    test_select_preserves_complete_interface);
    g_test_add_func("/hotplug/nic_xml/missing_mac",
                    test_select_reports_missing_mac);
    g_test_add_func("/hotplug/nic_xml/malformed_xml",
                    test_select_rejects_malformed_xml);
}
