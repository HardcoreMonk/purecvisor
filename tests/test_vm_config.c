                                                                                               
                                                                                              
                                                                             
                                                                     
                                 
   
                         
                                             
  
                                                              
  
                 
                                                              
                                                                 
                                                            
  
                                       
  
                                                           
                                            
                                
   

#include <glib.h>
#include <string.h>
#include "modules/virt/vm_config_builder.h"

                                                          
                                                                           
                                                             
                                                               
                                                        
extern gchar *_build_memory_backing_xml(gboolean hugepages, gboolean want_shared);
extern gint64 _hugepage_preflight_need_pages(gint ram_mb);

                                                                   
                                                                                     
                                                                              
extern gchar *_build_bridge_iface_xml(const gchar *safe_bridge,
                                      const gchar *virtualport_xml,
                                      const gchar *vlan_xml,
                                      gint mtu);
extern gchar *_vm_xml_ensure_memballoon_stats(const gchar *xml);
extern gchar *_vm_xml_insert_virtio_rng(const gchar *xml);
extern gchar *_vm_xml_apply_cpu_invtsc(const gchar *xml);
extern gchar *_vm_xml_ensure_q35_hotplug_ports(const gchar *xml);

                                                                            
                                                                           
#include "modules/network/pcv_qos.h"
extern gchar *_qos_metadata_xml(const PcvQosSla *sla);
extern gboolean _qos_metadata_parse(const gchar *metadata_xml, PcvQosSla *out);

                                                                 
                          
                                                                    

static void test_config_new_returns_nonnull(void) {
    PureCVisorVmConfig *cfg = purecvisor_vm_config_new("test-vm", 4, 4096);
    g_assert_nonnull(cfg);
    purecvisor_vm_config_free(cfg);
}

static void test_config_new_minimal(void) {
    PureCVisorVmConfig *cfg = purecvisor_vm_config_new("a", 1, 128);
    g_assert_nonnull(cfg);
    purecvisor_vm_config_free(cfg);
}

static void test_config_free_null_safe(void) {
                             
    purecvisor_vm_config_free(NULL);
}

                                                                 
                                    
                                                                    

static void test_set_disk_basic(void) {
    PureCVisorVmConfig *cfg = purecvisor_vm_config_new("vm1", 2, 2048);
    purecvisor_vm_config_set_disk(cfg, "/dev/zvol/pcvpool/vms/vm1");
    purecvisor_vm_config_free(cfg);
}

static void test_set_disk_overwrite(void) {
                                    
    PureCVisorVmConfig *cfg = purecvisor_vm_config_new("vm1", 2, 2048);
    purecvisor_vm_config_set_disk(cfg, "/path/first");
    purecvisor_vm_config_set_disk(cfg, "/path/second");
    purecvisor_vm_config_set_disk(cfg, "/path/third");
    purecvisor_vm_config_free(cfg);
}

static void test_set_iso(void) {
    PureCVisorVmConfig *cfg = purecvisor_vm_config_new("vm1", 2, 2048);
    purecvisor_vm_config_set_iso(cfg, "/iso/ubuntu-24.04.iso");
              
    purecvisor_vm_config_set_iso(cfg, "/iso/debian-12.iso");
    purecvisor_vm_config_free(cfg);
}

static void test_set_bridge(void) {
    PureCVisorVmConfig *cfg = purecvisor_vm_config_new("vm1", 2, 2048);
    purecvisor_vm_config_set_network_bridge(cfg, "pcvbr0");
    purecvisor_vm_config_set_network_bridge(cfg, "virbr0");
    purecvisor_vm_config_free(cfg);
}

                                                                 
                                    
                                                                    

static void test_vlan_valid_range(void) {
    PureCVisorVmConfig *cfg = purecvisor_vm_config_new("vm1", 1, 512);
    purecvisor_vm_config_set_vlan_id(cfg, 1);              
    purecvisor_vm_config_set_vlan_id(cfg, 100);            
    purecvisor_vm_config_set_vlan_id(cfg, 4094);           
    purecvisor_vm_config_free(cfg);
}

static void test_vlan_boundary_clamp(void) {
    PureCVisorVmConfig *cfg = purecvisor_vm_config_new("vm1", 1, 512);
    purecvisor_vm_config_set_vlan_id(cfg, 0);               
    purecvisor_vm_config_set_vlan_id(cfg, 4095);                   
    purecvisor_vm_config_set_vlan_id(cfg, 4096);                   
    purecvisor_vm_config_set_vlan_id(cfg, -1);                     
    purecvisor_vm_config_set_vlan_id(cfg, -100);
    purecvisor_vm_config_set_vlan_id(cfg, 99999);
    purecvisor_vm_config_free(cfg);
}

                                                                 
                                     
                                                                    

static void test_boot_cpu_modes(void) {
    PureCVisorVmConfig *cfg = purecvisor_vm_config_new("vm1", 1, 512);
    purecvisor_vm_config_set_boot_mode(cfg, 0);             
    purecvisor_vm_config_set_boot_mode(cfg, 1);             
    purecvisor_vm_config_set_boot_mode(cfg, 2);                        
    purecvisor_vm_config_set_cpu_mode(cfg, 0);                                               
    purecvisor_vm_config_set_cpu_mode(cfg, 1);                          
    purecvisor_vm_config_set_cpu_mode(cfg, 2);                    
    purecvisor_vm_config_free(cfg);
}

static void test_tpm_hugepages(void) {
    PureCVisorVmConfig *cfg = purecvisor_vm_config_new("vm1", 1, 512);
    purecvisor_vm_config_set_tpm(cfg, TRUE);
    purecvisor_vm_config_set_tpm(cfg, FALSE);
    purecvisor_vm_config_set_hugepages(cfg, TRUE);
    purecvisor_vm_config_set_hugepages(cfg, FALSE);
    purecvisor_vm_config_free(cfg);
}

                                                                 
                                        
                                                                    

static void test_network_mode_tenant_overlay_roundtrip(void) {
    PureCVisorVmConfig *cfg = purecvisor_vm_config_new("vm1", 2, 2048);
                        
    g_assert_null(purecvisor_vm_config_get_network_mode(cfg));
    g_assert_null(purecvisor_vm_config_get_tenant(cfg));

    purecvisor_vm_config_set_network_mode(cfg, "tenant-overlay");
    purecvisor_vm_config_set_tenant(cfg, "acme-01");
    g_assert_cmpstr(purecvisor_vm_config_get_network_mode(cfg), ==, "tenant-overlay");
    g_assert_cmpstr(purecvisor_vm_config_get_tenant(cfg), ==, "acme-01");

                               
    purecvisor_vm_config_set_network_mode(cfg, "bridge");
    purecvisor_vm_config_set_tenant(cfg, "acme-02");
    g_assert_cmpstr(purecvisor_vm_config_get_network_mode(cfg), ==, "bridge");
    g_assert_cmpstr(purecvisor_vm_config_get_tenant(cfg), ==, "acme-02");

                                              
    purecvisor_vm_config_set_network_mode(cfg, NULL);
    g_assert_null(purecvisor_vm_config_get_network_mode(cfg));

    purecvisor_vm_config_free(cfg);
}

static void test_network_mode_null_config_safe(void) {
                                          
    purecvisor_vm_config_set_network_mode(NULL, "tenant-overlay");
    purecvisor_vm_config_set_tenant(NULL, "acme-01");
    g_assert_null(purecvisor_vm_config_get_network_mode(NULL));
    g_assert_null(purecvisor_vm_config_get_tenant(NULL));
}

                                                                 
                        
                                                                    

static void test_full_config_scenario(void) {
    PureCVisorVmConfig *cfg = purecvisor_vm_config_new("web-prod", 4, 8192);
    purecvisor_vm_config_set_disk(cfg, "/dev/zvol/pcvpool/vms/web-prod");
    purecvisor_vm_config_set_iso(cfg, "/iso/ubuntu-24.04-live.iso");
    purecvisor_vm_config_set_network_bridge(cfg, "pcvbr0");
    purecvisor_vm_config_set_vlan_id(cfg, 100);
    purecvisor_vm_config_set_boot_mode(cfg, 1);
    purecvisor_vm_config_set_tpm(cfg, TRUE);
    purecvisor_vm_config_set_cpu_mode(cfg, 1);
    purecvisor_vm_config_set_hugepages(cfg, TRUE);
    purecvisor_vm_config_free(cfg);
}

                                                                 
                                                        
                                                                       
                                                                            
                                                                    

   
                             
                                                                    
                                                         
                                           
   
static void test_memory_backing_plain(void) {
    gchar *xml = _build_memory_backing_xml(FALSE, FALSE);
    g_assert_nonnull(xml);
    g_assert_cmpstr(xml, ==, "  <memoryBacking><nosharepages/></memoryBacking>\n");
    g_free(xml);
}

   
                                      
                                                               
                                                                     
                                                                      
   
static void test_memory_backing_hugepages_only(void) {
    gchar *xml = _build_memory_backing_xml(TRUE, FALSE);
    g_assert_nonnull(xml);
    g_assert_cmpstr(xml, ==,
        "  <memoryBacking><hugepages><page size='2048' unit='KiB'/>"
        "</hugepages><nosharepages/></memoryBacking>\n");
    g_free(xml);
}

   
                                 
                                                                 
                                                                          
                                                      
   
static void test_memory_backing_dpdk_only(void) {
    gchar *xml = _build_memory_backing_xml(FALSE, TRUE);
    g_assert_nonnull(xml);
    g_assert_cmpstr(xml, ==,
        "  <memoryBacking><nosharepages/><source type='memfd'/>"
        "<access mode='shared'/></memoryBacking>\n");
    g_free(xml);
}

   
                                      
                                                                           
                                                                       
                                                                   
                                                        
   
static void test_memory_backing_hugepages_dpdk(void) {
    gchar *xml = _build_memory_backing_xml(TRUE, TRUE);
    g_assert_nonnull(xml);
    g_assert_cmpstr(xml, ==,
        "  <memoryBacking><hugepages><page size='2048' unit='KiB'/></hugepages>"
        "<nosharepages/><access mode='shared'/></memoryBacking>\n");
    g_free(xml);
}

   
                                           
                                                                  
                                                         
                                    
                                             
                                      
                            
                            
                                     
                         
   
static void test_hugepage_preflight_need_pages_ceil(void) {
    g_assert_cmpint(_hugepage_preflight_need_pages(0), ==, 0);
    g_assert_cmpint(_hugepage_preflight_need_pages(1), ==, 1);
    g_assert_cmpint(_hugepage_preflight_need_pages(2), ==, 1);
    g_assert_cmpint(_hugepage_preflight_need_pages(3), ==, 2);
    g_assert_cmpint(_hugepage_preflight_need_pages(4), ==, 2);
    g_assert_cmpint(_hugepage_preflight_need_pages(2048), ==, 1024);
    g_assert_cmpint(_hugepage_preflight_need_pages(4096), ==, 2048);
}

                                                                 
                                                
                                                                        
                                                                    

   
                                          
                                                               
                                                                
                                                          
   
static void test_n13_bridge_iface_has_vhost_driver(void) {
    gchar *xml = _build_bridge_iface_xml("pcvbr0", "", "", 0);
    g_assert_nonnull(xml);
    g_assert_nonnull(g_strstr_len(xml, -1, "<interface type='bridge'>"));
    g_assert_nonnull(g_strstr_len(xml, -1, "<source bridge='pcvbr0'/>"));
    g_assert_nonnull(g_strstr_len(xml, -1, "<model type='virtio'/>"));
                                                
    g_assert_nonnull(g_strstr_len(xml, -1, "<driver name='vhost'/>"));
    g_free(xml);

                                               
    gchar *xml2 = _build_bridge_iface_xml(
        "ovsbr0",
        "      <virtualport type='openvswitch'/>\n",
        "      <vlan><tag id=\"100\"/></vlan>\n",
        9000);
    g_assert_nonnull(g_strstr_len(xml2, -1, "<virtualport type='openvswitch'/>"));
    g_assert_nonnull(g_strstr_len(xml2, -1, "<vlan><tag id=\"100\"/></vlan>"));
    g_assert_nonnull(g_strstr_len(xml2, -1, "<driver name='vhost'/>"));
    g_free(xml2);
}

   
                                     
                                                                    
                                                               
   
static void test_n8_bridge_iface_mtu_contract(void) {
               
    gchar *xml = _build_bridge_iface_xml("pcvbr0", "", "", 9000);
    g_assert_nonnull(g_strstr_len(xml, -1, "<mtu size='9000'/>"));
    g_free(xml);

                                
    xml = _build_bridge_iface_xml("pcvbr0", "", "", 1500);
    g_assert_nonnull(g_strstr_len(xml, -1, "<mtu size='1500'/>"));
    g_free(xml);

                                         
    xml = _build_bridge_iface_xml("pcvbr0", "", "", 0);
    g_assert_null(g_strstr_len(xml, -1, "<mtu"));
    g_assert_nonnull(g_strstr_len(xml, -1, "<driver name='vhost'/>"));
    g_free(xml);
}

   
                                               
                                                                                  
                                                               
   
static void test_n2_memballoon_stats_insert_when_absent(void) {
    const gchar *base = "<domain><devices><disk/></devices></domain>";
    gchar *xml = _vm_xml_ensure_memballoon_stats(base);
    g_assert_nonnull(xml);
    g_assert_nonnull(g_strstr_len(xml, -1, "<memballoon model='virtio'>"));
    g_assert_nonnull(g_strstr_len(xml, -1, "<stats period='10'/>"));
                                                           
    g_assert_true(g_strstr_len(xml, -1, "<memballoon") < g_strstr_len(xml, -1, "</devices>"));
    g_free(xml);
}

   
                                                 
                                                                          
                                                  
   
static void test_n2_memballoon_stats_existing_selfclosing(void) {
    const gchar *base =
        "<domain><devices><memballoon model='virtio'/></devices></domain>";
    gchar *xml = _vm_xml_ensure_memballoon_stats(base);
    g_assert_nonnull(xml);
    g_assert_nonnull(g_strstr_len(xml, -1,
        "<memballoon model='virtio'><stats period='10'/></memballoon>"));
                                    
    g_assert_null(g_strstr_len(xml, -1, "<memballoon model='virtio'/>"));

                                          
    gchar *again = _vm_xml_ensure_memballoon_stats(xml);
    g_assert_cmpstr(again, ==, xml);
    g_free(again);
    g_free(xml);
}

   
                             
                                                                          
                                                                      
                      
   
static void test_n3_virtio_rng_insert(void) {
    const gchar *base = "<domain><devices><disk/></devices></domain>";
    gchar *xml = _vm_xml_insert_virtio_rng(base);
    g_assert_nonnull(xml);
    g_assert_nonnull(g_strstr_len(xml, -1, "<rng model='virtio'>"));
    g_assert_nonnull(g_strstr_len(xml, -1, "<backend model='random'>/dev/urandom</backend>"));
                                                            
    g_assert_null(g_strstr_len(xml, -1, "/dev/random<"));
    g_assert_true(g_strstr_len(xml, -1, "<rng") < g_strstr_len(xml, -1, "</devices>"));

                                 
    gchar *again = _vm_xml_insert_virtio_rng(xml);
    g_assert_cmpstr(again, ==, xml);
    g_free(again);
    g_free(xml);
}

static guint
count_text(const gchar *text, const gchar *needle)
{
    guint count = 0;
    const gchar *p = text;
    while ((p = strstr(p, needle)) != NULL) {
        count++;
        p += strlen(needle);
    }
    return count;
}

   
                                   
                                                                         
                                                           
   
static void test_n8_q35_hotplug_port_budget(void) {
    const gchar *empty =
        "<domain><os><type machine='q35'>hvm</type></os><devices><disk/></devices></domain>";
    gchar *xml = _vm_xml_ensure_q35_hotplug_ports(empty);
    g_assert_cmpuint(count_text(xml, "model='pcie-root'"), ==, 1);
    g_assert_nonnull(g_strstr_len(
        xml, -1, "<controller type='pci' index='0' model='pcie-root'/>"));
    g_assert_cmpuint(count_text(xml, "model='pcie-root-port'"), ==, 8);
    g_assert_true(g_strstr_len(xml, -1, "<controller") < g_strstr_len(xml, -1, "</devices>"));

    gchar *again = _vm_xml_ensure_q35_hotplug_ports(xml);
    g_assert_cmpstr(again, ==, xml);
    g_free(again);
    g_free(xml);

    const gchar *partial =
        "<domain><os><type machine=\"pc-q35-10.2\">hvm</type></os><devices>"
        "<controller type=\"pci\" index=\"0\" model=\"pcie-root\"/>"
        "<controller type=\"pci\" index=\"1\" model=\"pcie-root-port\"><target chassis=\"1\"/></controller>"
        "<controller type='pci' model='pcie-root-port'/><controller type='pci' model='pcie-root-port'/>"
        "</devices></domain>";
    xml = _vm_xml_ensure_q35_hotplug_ports(partial);
    g_assert_cmpuint(count_text(xml, "pcie-root-port"), ==, 8);
    g_assert_cmpuint(count_text(xml, "pcie-root\""), ==, 1);
    g_assert_nonnull(g_strstr_len(xml, -1, "index=\"1\""));
    g_assert_nonnull(g_strstr_len(xml, -1, "target chassis=\"1\""));
    g_free(xml);
}

static void test_n8_hotplug_budget_non_q35_unchanged(void) {
    const gchar *i440 =
        "<domain><os><type machine='pc-i440fx-8.2'>hvm</type></os><devices/></domain>";
    gchar *xml = _vm_xml_ensure_q35_hotplug_ports(i440);
    g_assert_cmpstr(xml, ==, i440);
    g_free(xml);
    g_assert_null(_vm_xml_ensure_q35_hotplug_ports(NULL));
}

   
                              
                                                                                         
                                                                    
                                                                   
                           
   
static void test_n4_cpu_invtsc_applied(void) {
    const gchar *base = "<domain><cpu mode=\"host-passthrough\"/><devices/></domain>";
    gchar *xml = _vm_xml_apply_cpu_invtsc(base);
    g_assert_nonnull(xml);
    g_assert_nonnull(g_strstr_len(xml, -1,
        "<cpu mode=\"host-passthrough\" migratable=\"off\">"));
    g_assert_nonnull(g_strstr_len(xml, -1,
        "<feature policy=\"require\" name=\"invtsc\"/></cpu>"));
                                                 
    g_assert_null(g_strstr_len(xml, -1, "<cpu mode=\"host-passthrough\"/>"));

                                    
    gchar *again = _vm_xml_apply_cpu_invtsc(xml);
    g_assert_cmpstr(again, ==, xml);
    g_free(again);
    g_free(xml);

                                     
    const gchar *hm = "<domain><cpu mode=\"host-model\"/><devices/></domain>";
    gchar *xml_hm = _vm_xml_apply_cpu_invtsc(hm);
    g_assert_nonnull(g_strstr_len(xml_hm, -1,
        "<cpu mode=\"host-model\" migratable=\"off\"><feature policy=\"require\" name=\"invtsc\"/></cpu>"));
    g_free(xml_hm);
}

   
                                               
                                                                               
                                                               
                                                   
   
static void test_vm_definition_completeness_integration(void) {
    PureCVisorVmConfig *c = purecvisor_vm_config_new("complete-vm", 2, 2048);
    purecvisor_vm_config_set_disk(c, "/dev/zvol/pcvpool/vms/complete-vm");
    GVirConfigDomain *dom = purecvisor_vm_config_build(c);
    g_assert_nonnull(dom);
    gchar *xml0 = gvir_config_object_to_xml(GVIR_CONFIG_OBJECT(dom));
    g_assert_nonnull(xml0);

    gchar *xml1 = _vm_xml_apply_cpu_invtsc(xml0);
    gchar *xml2 = _vm_xml_ensure_memballoon_stats(xml1);
    gchar *xml3 = _vm_xml_insert_virtio_rng(xml2);
    gchar *xml4 = _vm_xml_ensure_q35_hotplug_ports(xml3);

                                                                             
                                                                    
    g_assert_nonnull(g_strstr_len(xml3, -1, "migratable=\"off\""));
    g_assert_nonnull(g_strstr_len(xml3, -1, "name=\"invtsc\""));
    g_assert_nonnull(g_strstr_len(xml3, -1, "<memballoon model='virtio'>"));
    g_assert_nonnull(g_strstr_len(xml3, -1, "<stats period='10'/>"));
    g_assert_nonnull(g_strstr_len(xml3, -1, "<rng model='virtio'>"));
    g_assert_nonnull(g_strstr_len(xml3, -1, "/dev/urandom"));
    g_assert_nonnull(g_strstr_len(
        xml4, -1, "<controller type='pci' index='0' model='pcie-root'/>"));
    g_assert_cmpuint(count_text(xml4, "model='pcie-root-port'"), ==, 8);

    g_free(xml0);
    g_free(xml1);
    g_free(xml2);
    g_free(xml3);
    g_free(xml4);
    g_object_unref(dom);
    purecvisor_vm_config_free(c);

                                                       
    g_assert_null(_vm_xml_apply_cpu_invtsc(NULL));
    g_assert_null(_vm_xml_ensure_memballoon_stats(NULL));
    g_assert_null(_vm_xml_insert_virtio_rng(NULL));
    g_assert_null(_vm_xml_ensure_q35_hotplug_ports(NULL));
}

   
                                   
                                                                
                                                                   
                                                   
                                                                     
   
static void test_qos_metadata_xml_no_prefix(void) {
    PcvQosSla sla = { .min_mbps = 0, .max_mbps = 1000, .burst_kb = 256 };
    gchar *xml = _qos_metadata_xml(&sla);
    g_assert_nonnull(xml);
    g_assert_true(g_str_has_prefix(xml, "<qos "));
    g_assert_null(g_strstr_len(xml, -1, "pcv:"));
    g_assert_nonnull(g_strstr_len(xml, -1, "min_mbps='0'"));
    g_assert_nonnull(g_strstr_len(xml, -1, "max_mbps='1000'"));
    g_assert_nonnull(g_strstr_len(xml, -1, "burst_kb='256'"));
    g_free(xml);
}

   
                                                
                                                                                  
                                                              
                                                   
   
static void test_qos_metadata_parse_wellformed_roundtrip(void) {
    const gchar *xml =
        "<pcv:qos xmlns:pcv=\"urn:purecvisor:qos:1\" "
        "min_mbps=\"0\" max_mbps=\"1000\" burst_kb=\"256\"/>";
    PcvQosSla out;
    gboolean ok = _qos_metadata_parse(xml, &out);
    g_assert_true(ok);
    g_assert_cmpuint(out.min_mbps, ==, 0);
    g_assert_cmpuint(out.max_mbps, ==, 1000);
    g_assert_cmpuint(out.burst_kb, ==, 256);
}

   
                                               
                                                                
                                                             
                                                  
   
static void test_qos_metadata_parse_double_prefix_fails(void) {
    const gchar *malformed =
        "<pcv:pcv:qos xmlns:pcv=\"urn:purecvisor:qos:1\" "
        "min_mbps=\"0\" max_mbps=\"1000\" burst_kb=\"256\"/>";
    PcvQosSla out;
    gboolean ok = _qos_metadata_parse(malformed, &out);
    g_assert_false(ok);
}

                                                                 
     
                                                                    

                                      
static void test_build_generates_domain(void) {
    PureCVisorVmConfig *c = purecvisor_vm_config_new("build-test", 2, 2048);
    purecvisor_vm_config_set_disk(c, "/dev/zvol/pcvpool/vms/build-test");
    purecvisor_vm_config_set_network_bridge(c, "pcvbr0");
    GVirConfigDomain *dom = purecvisor_vm_config_build(c);
    g_assert_nonnull(dom);
                   
    gchar *xml = gvir_config_object_to_xml(GVIR_CONFIG_OBJECT(dom));
    g_assert_nonnull(xml);
    g_assert_true(g_strstr_len(xml, -1, "build-test") != NULL);
    g_free(xml);
    g_object_unref(dom);
    purecvisor_vm_config_free(c);
}

                                         
                                                                                  
static void test_build_includes_guest_agent_channel(void) {
    PureCVisorVmConfig *c = purecvisor_vm_config_new("ga-test", 2, 2048);
    purecvisor_vm_config_set_disk(c, "/dev/zvol/pcvpool/vms/ga-test");
    GVirConfigDomain *dom = purecvisor_vm_config_build(c);
    g_assert_nonnull(dom);
    gchar *xml = gvir_config_object_to_xml(GVIR_CONFIG_OBJECT(dom));
    g_assert_nonnull(xml);
    g_assert_true(g_strstr_len(xml, -1, "org.qemu.guest_agent.0") != NULL);
    g_assert_true(g_strstr_len(xml, -1, "<channel") != NULL);
    g_free(xml);
    g_object_unref(dom);
    purecvisor_vm_config_free(c);
}

                                                
  
                                                  
                                                                        
                                                            
                                                                      
              
  
                                                                  
                                         
static void test_build_disk_driver_io_policy(void) {
    const gchar *paths[] = { "/dev/zvol/pcvpool/vms/io-test", "/tmp/io-test.qcow2" };

    for (gsize i = 0; i < G_N_ELEMENTS(paths); i++) {
        PureCVisorVmConfig *c = purecvisor_vm_config_new("io-test", 2, 2048);
        purecvisor_vm_config_set_disk(c, paths[i]);

        GVirConfigDomain *dom = purecvisor_vm_config_build(c);
        g_assert_nonnull(dom);
        gchar *xml = gvir_config_object_to_xml(GVIR_CONFIG_OBJECT(dom));
        g_assert_nonnull(xml);

        g_assert_nonnull(g_strstr_len(xml, -1, "cache=\"none\""));
        g_assert_nonnull(g_strstr_len(xml, -1, "io=\"native\""));
        g_assert_nonnull(g_strstr_len(xml, -1, "discard=\"unmap\""));

        g_free(xml);
        g_object_unref(dom);
        purecvisor_vm_config_free(c);
    }
}

static void test_hugepages_flag(void) {
    PureCVisorVmConfig *c = purecvisor_vm_config_new("hp-test", 4, 4096);
    purecvisor_vm_config_set_hugepages(c, TRUE);
    purecvisor_vm_config_set_disk(c, "/tmp/test.qcow2");
    GVirConfigDomain *dom = purecvisor_vm_config_build(c);
    g_assert_nonnull(dom);
    gchar *xml = gvir_config_object_to_xml(GVIR_CONFIG_OBJECT(dom));
                                                                        
    g_assert_nonnull(xml);
    g_free(xml);
    g_object_unref(dom);
    purecvisor_vm_config_free(c);
}

static void test_null_name_safe(void) {
    PureCVisorVmConfig *c = purecvisor_vm_config_new(NULL, 1, 512);
    if (c) {
        purecvisor_vm_config_free(c);
    }
                                               
}

                                                                 
                             
                                                                    

   
                              
                                                      
                
  
                                   
                                                              
   
static void test_build_xml_vcpu_memory(void) {
    PureCVisorVmConfig *cfg = purecvisor_vm_config_new("test-vm", 2, 1024);
    purecvisor_vm_config_set_disk(cfg, "/dev/zvol/pcvpool/vms/test-vm");
    purecvisor_vm_config_set_network_bridge(cfg, "pcvbr0");
    purecvisor_vm_config_set_boot_mode(cfg, 0);
    purecvisor_vm_config_set_cpu_mode(cfg, 1);                        

    GVirConfigDomain *dom = purecvisor_vm_config_build(cfg);
    g_assert_nonnull(dom);

    gchar *xml = gvir_config_object_to_xml(GVIR_CONFIG_OBJECT(dom));
    g_assert_nonnull(xml);

                              
    g_assert_true(g_strstr_len(xml, -1, "<name>test-vm</name>") != NULL);

                                              
    g_assert_true(g_strstr_len(xml, -1, "<vcpu>2</vcpu>") != NULL);

                                           
    g_assert_true(g_strstr_len(xml, -1, "1048576") != NULL);

    g_free(xml);
    g_object_unref(dom);
    purecvisor_vm_config_free(cfg);
}

   
                                          
                                             
                                                   
   
static void test_build_defaults_acpi_apic_host_cpu(void) {
    PureCVisorVmConfig *cfg = purecvisor_vm_config_new("rocky10-vm", 2, 2048);
    purecvisor_vm_config_set_disk(cfg, "/dev/zvol/pcvpool/vms/rocky10-vm");

    GVirConfigDomain *dom = purecvisor_vm_config_build(cfg);
    g_assert_nonnull(dom);

    gchar *xml = gvir_config_object_to_xml(GVIR_CONFIG_OBJECT(dom));
    g_assert_nonnull(xml);

    g_assert_true(g_strstr_len(xml, -1, "<features>") != NULL);
    g_assert_true(g_strstr_len(xml, -1, "<acpi/>") != NULL);
    g_assert_true(g_strstr_len(xml, -1, "<apic/>") != NULL);
    g_assert_true(g_strstr_len(xml, -1, "host-passthrough") != NULL);

    g_free(xml);
    g_object_unref(dom);
    purecvisor_vm_config_free(cfg);
}

   
                                      
                                        
                                      
                                        
                                                      
                
   
static void test_build_with_iso_contains_cdrom(void) {
    PureCVisorVmConfig *cfg = purecvisor_vm_config_new("iso-vm", 1, 512);
    purecvisor_vm_config_set_disk(cfg, "/tmp/iso-vm.qcow2");
    purecvisor_vm_config_set_iso(cfg, "/iso/ubuntu-24.04-live.iso");

    GVirConfigDomain *dom = purecvisor_vm_config_build(cfg);
    g_assert_nonnull(dom);

    gchar *xml = gvir_config_object_to_xml(GVIR_CONFIG_OBJECT(dom));
    g_assert_nonnull(xml);

                                            
    g_assert_true(g_strstr_len(xml, -1, "cdrom") != NULL);

                                         
    g_assert_true(g_strstr_len(xml, -1, "ubuntu-24.04-live.iso") != NULL);

    g_free(xml);
    g_object_unref(dom);
    purecvisor_vm_config_free(cfg);
}

   
                               
                                                     
                                                           
                                          
                                              
                 
  
                                                   
                                 
                                
   
static void test_build_null_config_safe(void) {
                                             
    GVirConfigDomain *dom = purecvisor_vm_config_build(NULL);
    g_assert_null(dom);
                                     
    if (dom) g_object_unref(dom);
}

void test_vm_config_register(void) {
    g_test_add_func("/vm_config/new_nonnull",            test_config_new_returns_nonnull);
    g_test_add_func("/vm_config/new_minimal",            test_config_new_minimal);
    g_test_add_func("/vm_config/free_null_safe",         test_config_free_null_safe);
    g_test_add_func("/vm_config/set_disk",               test_set_disk_basic);
    g_test_add_func("/vm_config/set_disk_overwrite",     test_set_disk_overwrite);
    g_test_add_func("/vm_config/set_iso",                test_set_iso);
    g_test_add_func("/vm_config/set_bridge",             test_set_bridge);
    g_test_add_func("/vm_config/vlan_valid",             test_vlan_valid_range);
    g_test_add_func("/vm_config/vlan_clamp",             test_vlan_boundary_clamp);
    g_test_add_func("/vm_config/boot_cpu_modes",         test_boot_cpu_modes);
    g_test_add_func("/vm_config/tpm_hugepages",          test_tpm_hugepages);
    g_test_add_func("/vm_config/network_mode_tenant_overlay_roundtrip",
                    test_network_mode_tenant_overlay_roundtrip);
    g_test_add_func("/vm_config/network_mode_null_config_safe",
                    test_network_mode_null_config_safe);
    g_test_add_func("/vm_config/full_scenario",          test_full_config_scenario);
    g_test_add_func("/vm_config/build_generates_domain", test_build_generates_domain);
    g_test_add_func("/vm_config/build_guest_agent_channel", test_build_includes_guest_agent_channel);
    g_test_add_func("/vm_config/build_disk_driver_io_policy", test_build_disk_driver_io_policy);
    g_test_add_func("/vm_config/hugepages_flag",         test_hugepages_flag);
    g_test_add_func("/vm_config/null_name_safe",         test_null_name_safe);
                                   
    g_test_add_func("/vm_config/build_xml_vcpu_memory",     test_build_xml_vcpu_memory);
    g_test_add_func("/vm_config/build_defaults_acpi_apic_host_cpu", test_build_defaults_acpi_apic_host_cpu);
    g_test_add_func("/vm_config/build_with_iso_cdrom",      test_build_with_iso_contains_cdrom);
    g_test_add_func("/vm_config/build_null_config_safe",    test_build_null_config_safe);
                                                         
    g_test_add_func("/vm_config/memory_backing_plain",           test_memory_backing_plain);
    g_test_add_func("/vm_config/memory_backing_hugepages_only",  test_memory_backing_hugepages_only);
    g_test_add_func("/vm_config/memory_backing_dpdk_only",       test_memory_backing_dpdk_only);
    g_test_add_func("/vm_config/memory_backing_hugepages_dpdk",  test_memory_backing_hugepages_dpdk);
    g_test_add_func("/vm_config/hugepage_preflight_need_pages_ceil",
                    test_hugepage_preflight_need_pages_ceil);
                                                    
    g_test_add_func("/vm_config/n13_bridge_iface_vhost_driver",
                    test_n13_bridge_iface_has_vhost_driver);
    g_test_add_func("/vm_config/n8/bridge_iface_mtu", test_n8_bridge_iface_mtu_contract);
    g_test_add_func("/vm_config/n2_memballoon_stats_insert",
                    test_n2_memballoon_stats_insert_when_absent);
    g_test_add_func("/vm_config/n2_memballoon_stats_existing",
                    test_n2_memballoon_stats_existing_selfclosing);
    g_test_add_func("/vm_config/n3_virtio_rng_insert",
                    test_n3_virtio_rng_insert);
    g_test_add_func("/vm_config/n8/q35_hotplug_port_budget",
                    test_n8_q35_hotplug_port_budget);
    g_test_add_func("/vm_config/n8/non_q35_hotplug_unchanged",
                    test_n8_hotplug_budget_non_q35_unchanged);
    g_test_add_func("/vm_config/n4_cpu_invtsc_applied",
                    test_n4_cpu_invtsc_applied);
    g_test_add_func("/vm_config/vm_definition_completeness_integration",
                    test_vm_definition_completeness_integration);
    g_test_add_func("/vm_config/qos_metadata_xml_no_prefix",
                    test_qos_metadata_xml_no_prefix);
    g_test_add_func("/vm_config/qos_metadata_parse_wellformed_roundtrip",
                    test_qos_metadata_parse_wellformed_roundtrip);
    g_test_add_func("/vm_config/qos_metadata_parse_double_prefix_fails",
                    test_qos_metadata_parse_double_prefix_fails);
}
