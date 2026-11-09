   
                            
                                     
  
                           
                                                                   
                                                           
  
                                                                 
                                                                   
   
#include <glib.h>
#include <gio/gio.h>
#include <string.h>

#include "modules/dispatcher/vnc_port_xml.h"

static void expect_port(const gchar *xml_in, const gchar *want)
{
    GError *error = NULL;
    gchar *port = pcv_vnc_port_from_domain_xml(xml_in, &error);

    g_assert_no_error(error);
    g_assert_cmpstr(port, ==, want);
    g_free(port);
}

static void expect_error(const gchar *xml_in, GQuark domain, gint code,
                         const gchar *message)
{
    GError *error = NULL;
    gchar *port = pcv_vnc_port_from_domain_xml(xml_in, &error);

    g_assert_null(port);
    g_assert_error(error, domain, code);
    if (message)
        g_assert_cmpstr(error->message, ==, message);
    g_clear_error(&error);
}

static void test_vnc_port_5901(void)
{
    expect_port("<domain><devices>"
                "<graphics type='vnc' port='5901' autoport='yes' listen='127.0.0.1'/>"
                "</devices></domain>", "5901");
}

static void test_attribute_order_port_first(void)
{
    expect_port("<domain><devices><graphics port='5902' type='vnc'/></devices></domain>",
                "5902");
}

static void test_autoport_minus_one_verbatim(void)
{
    expect_port("<domain><devices><graphics type='vnc' port='-1' autoport='yes'/>"
                "</devices></domain>", "-1");
}

static void test_spice_only_not_found(void)
{
    expect_error("<domain><devices><graphics type='spice' port='5930'/></devices></domain>",
                 G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                 "VNC Graphics adapter not found in XML");
}

static void test_first_vnc_of_two(void)
{
    expect_port("<domain><devices>"
                "<graphics type='spice' port='5930'/>"
                "<graphics type='vnc' port='5903'/>"
                "<graphics type='vnc' port='5904'/>"
                "</devices></domain>", "5903");
}

static void test_vnc_without_port(void)
{
    expect_error("<domain><devices><graphics type='vnc' autoport='yes'/></devices></domain>",
                 G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "VNC port not found in XML");
}

static void test_malformed_and_null(void)
{
    expect_error("<domain><devices><graphics type='vnc'",
                 G_IO_ERROR, G_IO_ERROR_INVALID_DATA, NULL);
    expect_error(NULL, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Domain XML is required");
    expect_error("", G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Domain XML is required");
}

void test_vnc_port_xml_register(void)
{
    g_test_add_func("/vnc/port_xml/vnc_port_5901", test_vnc_port_5901);
    g_test_add_func("/vnc/port_xml/attribute_order_port_first",
                    test_attribute_order_port_first);
    g_test_add_func("/vnc/port_xml/autoport_minus_one_verbatim",
                    test_autoport_minus_one_verbatim);
    g_test_add_func("/vnc/port_xml/spice_only_not_found", test_spice_only_not_found);
    g_test_add_func("/vnc/port_xml/first_vnc_of_two", test_first_vnc_of_two);
    g_test_add_func("/vnc/port_xml/vnc_without_port", test_vnc_without_port);
    g_test_add_func("/vnc/port_xml/malformed_and_null", test_malformed_and_null);
}
