                                                                                           
                                                                                             
                                                                            
                         
                                                                
                           
                   
  
                                                           
                                      
  
                                                   
                                               
           
   
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <libvirt/libvirt.h>
#include "modules/network/pcv_qos.h"
#include "modules/virt/vm_manager.h"
#include "modules/network/tenant_overlay.h"                                                            

                                                            
                                                                       
                                                       
                                                              
                                           
extern gchar *_qos_metadata_xml(const PcvQosSla *sla);
extern gboolean _qos_metadata_parse(const gchar *metadata_xml, PcvQosSla *out);

                                                                    
                                                                 
                                                      
                                           
extern void pcv_qos_set_tenant_sla_path_for_test(const gchar *path);

                                                                        
                                                                    
                                                                        

static void test_classid_deterministic(void) {
    pcv_qos_ids_clear();
    gchar *a = pcv_qos_classid("acme", "vm1");
    gchar *b = pcv_qos_classid("acme", "vm1");
    g_assert_cmpstr(a, ==, b);                                      
    gchar *c = pcv_qos_classid("acme", "vm2");
    g_assert_cmpstr(a, !=, c);                        
    g_free(a); g_free(b); g_free(c);
}

static void test_classid_format_is_hex(void) {
    pcv_qos_ids_clear();
    guint16 minor = pcv_qos_vm_minor("acme", "vm1");
    gchar *expect = g_strdup_printf("1:%x", minor);
    gchar *got = pcv_qos_classid("acme", "vm1");
    g_assert_cmpstr(got, ==, expect);
    g_free(expect); g_free(got);
}

static void test_tenant_classid_format_is_hex(void) {
    pcv_qos_ids_clear();
    guint16 minor = pcv_qos_tenant_minor("acme");
    gchar *expect = g_strdup_printf("1:%x", minor);
    gchar *got = pcv_qos_tenant_classid("acme");
    g_assert_cmpstr(got, ==, expect);
    g_free(expect); g_free(got);
}

                                                                        
                                                                       
                                                                        

static void test_tenant_minor_distinct(void) {
    pcv_qos_ids_clear();
    g_assert_cmpuint(pcv_qos_tenant_minor("acme"), !=, pcv_qos_tenant_minor("beta"));
}

static void test_vm_minor_distinct_within_tenant(void) {
    pcv_qos_ids_clear();
    g_assert_cmpuint(pcv_qos_vm_minor("acme", "vm1"), !=, pcv_qos_vm_minor("acme", "vm2"));
}

static void test_vm_minor_distinct_across_tenants(void) {
    pcv_qos_ids_clear();
                                                         
    g_assert_cmpuint(pcv_qos_vm_minor("acme", "vm1"), !=, pcv_qos_vm_minor("beta", "vm1"));
}

                                                                        
                                                                      
                                                                        

static void test_tenant_minor_range(void) {
    pcv_qos_ids_clear();
    const char *names[] = { "acme", "beta", "gamma", "tenant-with-long-name", "z" };
    for (guint i = 0; i < G_N_ELEMENTS(names); i++) {
        guint16 m = pcv_qos_tenant_minor(names[i]);
        g_assert_cmpuint(m, >=, 0x0010);
        g_assert_cmpuint(m, <=, 0x0FFF);
    }
}

static void test_vm_minor_range(void) {
    pcv_qos_ids_clear();
    const char *names[] = { "vm1", "vm2", "web-01", "db-primary", "x" };
    for (guint i = 0; i < G_N_ELEMENTS(names); i++) {
        guint16 m = pcv_qos_vm_minor("acme", names[i]);
        g_assert_cmpuint(m, >=, 0x1000);
        g_assert_cmpuint(m, <=, 0xFFFD);
    }
}

                                                                        
                                                                    
                                                                        

static void test_tenant_minor_collision_probes(void) {
    pcv_qos_ids_clear();

                                              
    guint16 x = pcv_qos_tenant_minor("acme");

                                                   
                                                 
    pcv_qos_ids_clear();

    gchar *tmpdir = g_dir_make_tmp("pcv-qos-test-XXXXXX", NULL);
    g_assert_nonnull(tmpdir);
    gchar *path = g_build_filename(tmpdir, "qos_ids.json", NULL);

    gchar *crafted = g_strdup_printf(
        "{\"tenants\":{\"other\":%u},\"vms\":{}}", (unsigned)x);
    GError *werr = NULL;
    gboolean wrote = g_file_set_contents(path, crafted, -1, &werr);
    g_assert_true(wrote);
    g_assert_no_error(werr);
    g_free(crafted);

    g_assert_true(pcv_qos_ids_load(path));

                                                       
                                                    
    g_assert_cmpuint(pcv_qos_tenant_minor("acme"), !=, x);
    g_assert_cmpuint(pcv_qos_tenant_minor("other"), ==, x);

    g_free(path);
    g_rmdir(tmpdir);
    g_free(tmpdir);

    pcv_qos_ids_clear();
}

                                                                        
                                                                      
                                                                        

static void test_ids_persist_roundtrip(void) {
    pcv_qos_ids_clear();

    guint16 t_before = pcv_qos_tenant_minor("acme");
    guint16 v_before = pcv_qos_vm_minor("acme", "vm1");

    gchar *tmpdir = g_dir_make_tmp("pcv-qos-test-XXXXXX", NULL);
    g_assert_nonnull(tmpdir);
    gchar *path = g_build_filename(tmpdir, "qos_ids.json", NULL);

    GError *err = NULL;
    gboolean saved = pcv_qos_ids_save(path, &err);
    g_assert_true(saved);
    g_assert_no_error(err);

    pcv_qos_ids_clear();
    g_assert_true(pcv_qos_ids_load(path));

                                              
    g_assert_cmpuint(pcv_qos_tenant_minor("acme"), ==, t_before);
    g_assert_cmpuint(pcv_qos_vm_minor("acme", "vm1"), ==, v_before);

    g_unlink(path);
    g_free(path);
    g_rmdir(tmpdir);
    g_free(tmpdir);

    pcv_qos_ids_clear();
}

static void test_ids_load_missing_file_is_empty_ok(void) {
    pcv_qos_ids_clear();
    gchar *tmpdir = g_dir_make_tmp("pcv-qos-test-XXXXXX", NULL);
    gchar *missing = g_build_filename(tmpdir, "does-not-exist.json", NULL);

    g_assert_true(pcv_qos_ids_load(missing));                          

                                                  
    guint16 m = pcv_qos_tenant_minor("acme");
    g_assert_cmpuint(m, >=, 0x0010);
    g_assert_cmpuint(m, <=, 0x0FFF);

    g_free(missing);
    g_rmdir(tmpdir);
    g_free(tmpdir);
    pcv_qos_ids_clear();
}

                                                           
                                               

static void test_ids_load_rejects_out_of_range_minor(void) {
    pcv_qos_ids_clear();

    gchar *tmpdir = g_dir_make_tmp("pcv-qos-test-XXXXXX", NULL);
    gchar *path = g_build_filename(tmpdir, "qos_ids.json", NULL);

                                                         
                                                               
    GError *werr = NULL;
    gboolean wrote = g_file_set_contents(
        path, "{\"tenants\":{\"acme\":65537},\"vms\":{}}", -1, &werr);
    g_assert_true(wrote);
    g_assert_no_error(werr);

    g_assert_false(pcv_qos_ids_load(path));                       

                                               
                                   
    guint16 m = pcv_qos_tenant_minor("acme");
    g_assert_cmpuint(m, >=, 0x0010);
    g_assert_cmpuint(m, <=, 0x0FFF);
    g_assert_cmpuint(m, !=, 1);

    g_free(path);
    g_rmdir(tmpdir);
    g_free(tmpdir);
    pcv_qos_ids_clear();
}

static void test_ids_load_rejects_duplicate_minor(void) {
    pcv_qos_ids_clear();

    gchar *tmpdir = g_dir_make_tmp("pcv-qos-test-XXXXXX", NULL);
    gchar *path = g_build_filename(tmpdir, "qos_ids.json", NULL);

                                                          
                 
    GError *werr = NULL;
    gboolean wrote = g_file_set_contents(
        path, "{\"tenants\":{\"acme\":20,\"beta\":20},\"vms\":{}}", -1, &werr);
    g_assert_true(wrote);
    g_assert_no_error(werr);

    g_assert_false(pcv_qos_ids_load(path));                    

                                               
                                              
                                  
    guint16 a = pcv_qos_tenant_minor("acme");
    guint16 b = pcv_qos_tenant_minor("beta");
    g_assert_cmpuint(a, !=, b);

    g_free(path);
    g_rmdir(tmpdir);
    g_free(tmpdir);
    pcv_qos_ids_clear();
}

static void test_ids_load_rejects_json_syntax_error(void) {
    pcv_qos_ids_clear();

    gchar *tmpdir = g_dir_make_tmp("pcv-qos-test-XXXXXX", NULL);
    gchar *path = g_build_filename(tmpdir, "qos_ids.json", NULL);

    GError *werr = NULL;
    gboolean wrote = g_file_set_contents(
        path, "{ this is not valid json ", -1, &werr);
    g_assert_true(wrote);
    g_assert_no_error(werr);

    g_assert_false(pcv_qos_ids_load(path));                       

    g_free(path);
    g_rmdir(tmpdir);
    g_free(tmpdir);
    pcv_qos_ids_clear();
}

                                                                        
                                                                        
                                                                        

static void test_sla_rejects_min_gt_max(void) {
    JsonObject *o = json_object_new();
    json_object_set_int_member(o, "qos_min_mbps", 500);
    json_object_set_int_member(o, "qos_max_mbps", 100);
    PcvQosSla s; GError *e = NULL;
    g_assert_false(pcv_qos_sla_from_json(o, &s, &e));
    g_assert_error(e, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&e); json_object_unref(o);
}

static void test_sla_rejects_zero_max(void) {
    JsonObject *o = json_object_new();
    json_object_set_int_member(o, "qos_min_mbps", 0);
    json_object_set_int_member(o, "qos_max_mbps", 0);
    PcvQosSla s; GError *e = NULL;
    g_assert_false(pcv_qos_sla_from_json(o, &s, &e));
    g_assert_error(e, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&e); json_object_unref(o);
}

                                                           
                                                         
                                                       
                                                
                                          
                                                        
                                
static void test_sla_rejects_negative_min(void) {
    JsonObject *o = json_object_new();
    json_object_set_int_member(o, "qos_min_mbps", -5);
    json_object_set_int_member(o, "qos_max_mbps", 100);
    PcvQosSla s; GError *e = NULL;
    g_assert_false(pcv_qos_sla_from_json(o, &s, &e));
    g_assert_error(e, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&e); json_object_unref(o);
}

static void test_sla_rejects_negative_max(void) {
    JsonObject *o = json_object_new();
    json_object_set_int_member(o, "qos_min_mbps", 10);
    json_object_set_int_member(o, "qos_max_mbps", -100);
    PcvQosSla s; GError *e = NULL;
    g_assert_false(pcv_qos_sla_from_json(o, &s, &e));
    g_assert_error(e, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&e); json_object_unref(o);
}

static void test_sla_rejects_negative_burst(void) {
    JsonObject *o = json_object_new();
    json_object_set_int_member(o, "qos_min_mbps", 10);
    json_object_set_int_member(o, "qos_max_mbps", 100);
    json_object_set_int_member(o, "qos_burst_kb", -1);
    PcvQosSla s; GError *e = NULL;
    g_assert_false(pcv_qos_sla_from_json(o, &s, &e));
    g_assert_error(e, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&e); json_object_unref(o);
}

                                                  
                                                              
                                                  
                                                
                                                     
static void test_sla_rejects_max_mbps_overflow(void) {
    JsonObject *o = json_object_new();
    json_object_set_int_member(o, "qos_min_mbps", 10);
    json_object_set_int_member(o, "qos_max_mbps", (gint64)G_MAXUINT32 + 1);
    PcvQosSla s; GError *e = NULL;
    g_assert_false(pcv_qos_sla_from_json(o, &s, &e));
    g_assert_error(e, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&e); json_object_unref(o);
}

static void test_sla_rejects_min_mbps_overflow(void) {
    JsonObject *o = json_object_new();
    json_object_set_int_member(o, "qos_min_mbps", (gint64)G_MAXUINT32 + 1);
    json_object_set_int_member(o, "qos_max_mbps", 100);
    PcvQosSla s; GError *e = NULL;
    g_assert_false(pcv_qos_sla_from_json(o, &s, &e));
    g_assert_error(e, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&e); json_object_unref(o);
}

static void test_sla_burst_default_256(void) {
    JsonObject *o = json_object_new();
    json_object_set_int_member(o, "qos_min_mbps", 10);
    json_object_set_int_member(o, "qos_max_mbps", 100);
    PcvQosSla s; GError *e = NULL;
    g_assert_true(pcv_qos_sla_from_json(o, &s, &e));
    g_assert_no_error(e);
    g_assert_cmpuint(s.min_mbps, ==, 10);
    g_assert_cmpuint(s.max_mbps, ==, 100);
    g_assert_cmpuint(s.burst_kb, ==, 256);            
    json_object_unref(o);
}

static void test_sla_accepts_explicit_burst(void) {
    JsonObject *o = json_object_new();
    json_object_set_int_member(o, "qos_min_mbps", 10);
    json_object_set_int_member(o, "qos_max_mbps", 100);
    json_object_set_int_member(o, "qos_burst_kb", 512);
    PcvQosSla s; GError *e = NULL;
    g_assert_true(pcv_qos_sla_from_json(o, &s, &e));
    g_assert_cmpuint(s.burst_kb, ==, 512);
    json_object_unref(o);
}

                                                                        
                                                                 
                                                                 
                                                                          
                                                                        

                                                            
                                                       
                                                   
                                                
static void test_apply_vm_rejects_invalid_vm_iface(void) {
    PcvQosSla sla = { "acme", "vm1", 100, 500, 256 };

    GError *e1 = NULL;
    g_assert_false(pcv_qos_apply_vm("bad/iface", &sla, &e1));                       
    g_assert_error(e1, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&e1);

    GError *e2 = NULL;
    g_assert_false(pcv_qos_apply_vm(NULL, &sla, &e2));
    g_assert_error(e2, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&e2);

    GError *e3 = NULL;
    g_assert_false(pcv_qos_apply_vm("", &sla, &e3));
    g_assert_error(e3, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&e3);
}

                                                                          
                                                        
                                                  
                                                          
                                                           
                                                                            
                                          
                            
static void test_apply_vm_before_ensure_root_returns_uplink_error(void) {
    pcv_qos_uplink_clear();
    PcvQosSla sla = { "acme", "vm1", 100, 500, 256 };
    GError *e = NULL;
    g_assert_false(pcv_qos_apply_vm("qostestiface0", &sla, &e));
    g_assert_error(e, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_nonnull(g_strstr_len(e->message, -1, "ensure_root"));
    g_clear_error(&e);
}

                                                                        
                                                                       
                                                              
                                                             
                                                                          
                                                          
                                                                 
                                                                  
                                                                  
                                                                 
                                                                        

static void test_qos_applicable_bridge_nic_unspecified_true(void) {
    g_assert_true(pcv_qos_vm_requires_sla("bridge", NULL));
}

static void test_qos_applicable_bridge_nic_bridge_true(void) {
    g_assert_true(pcv_qos_vm_requires_sla("bridge", "bridge"));
}

static void test_qos_applicable_tenant_overlay_nic_unspecified_true(void) {
    g_assert_true(pcv_qos_vm_requires_sla("tenant-overlay", NULL));
}

                                                          
                                                                 
                                                 
          
static void test_qos_applicable_tenant_overlay_nic_dpdk_still_true(void) {
    g_assert_true(pcv_qos_vm_requires_sla("tenant-overlay", "dpdk"));
}

static void test_qos_applicable_tenant_overlay_nic_sriov_still_true(void) {
    g_assert_true(pcv_qos_vm_requires_sla("tenant-overlay", "sriov"));
}

static void test_qos_applicable_network_mode_dpdk_false(void) {
    g_assert_false(pcv_qos_vm_requires_sla("dpdk", NULL));
}

static void test_qos_applicable_network_mode_sriov_false(void) {
    g_assert_false(pcv_qos_vm_requires_sla("sriov", NULL));
}

                                                      
                                                           
                                                           
                         
static void test_qos_applicable_unspecified_mode_nic_dpdk_false(void) {
    g_assert_false(pcv_qos_vm_requires_sla(NULL, "dpdk"));
    g_assert_false(pcv_qos_vm_requires_sla("", "dpdk"));
    g_assert_false(pcv_qos_vm_requires_sla("bridge", "dpdk"));
}

static void test_qos_applicable_unspecified_mode_nic_sriov_false(void) {
    g_assert_false(pcv_qos_vm_requires_sla(NULL, "sriov"));
    g_assert_false(pcv_qos_vm_requires_sla("bridge", "sriov"));
}

static void test_qos_applicable_unspecified_defaults_true(void) {
                                                             
    g_assert_true(pcv_qos_vm_requires_sla(NULL, NULL));
    g_assert_true(pcv_qos_vm_requires_sla("", ""));
}

static void test_qos_applicable_unknown_value_defaults_true(void) {
                                                          
                                                               
                                                  
                                 
    g_assert_true(pcv_qos_vm_requires_sla("typo-mode", NULL));
    g_assert_true(pcv_qos_vm_requires_sla("bridge", "typo-nic"));
}

                                                                        
                                                             
                                                                    
                                                                        

static void test_qos_metadata_build_parse_roundtrip(void) {
    PcvQosSla sla = { "", "", 100, 500, 256 };
    gchar *xml = _qos_metadata_xml(&sla);
    g_assert_nonnull(xml);
                                                 
                                                               
                                                                 
    g_assert_null(strstr(xml, "pcv:qos"));
    g_assert_nonnull(strstr(xml, "<qos "));

    PcvQosSla out;
    gboolean ok = _qos_metadata_parse(xml, &out);
    g_assert_true(ok);
    g_assert_cmpuint(out.min_mbps, ==, 100);
    g_assert_cmpuint(out.max_mbps, ==, 500);
    g_assert_cmpuint(out.burst_kb, ==, 256);
    g_free(xml);
}

static void test_qos_metadata_build_null_sla_is_empty(void) {
    gchar *xml = _qos_metadata_xml(NULL);
    g_assert_cmpstr(xml, ==, "");
    g_free(xml);
}

static void test_qos_metadata_parse_absent_returns_false(void) {
    PcvQosSla out;
    gboolean ok = _qos_metadata_parse(
        "<metadata><pcv:owner xmlns:pcv='urn:purecvisor:metadata'>x</pcv:owner></metadata>",
        &out);
    g_assert_false(ok);
}

static void test_qos_metadata_parse_malformed_missing_max_returns_false(void) {
    PcvQosSla out;
                                 
    gboolean ok = _qos_metadata_parse(
        "<pcv:qos xmlns:pcv='urn:purecvisor:qos:1' min_mbps='10'/>", &out);
    g_assert_false(ok);
}

static void test_qos_metadata_parse_null_safe(void) {
    PcvQosSla out;
    g_assert_false(_qos_metadata_parse(NULL, &out));
    g_assert_false(_qos_metadata_parse("<pcv:qos/>", NULL));
}

                                                                           
                                           
                                                   
                                                  
                      
static void test_qos_metadata_parse_libvirt_stripped_form(void) {
    PcvQosSla out;
    gboolean ok = _qos_metadata_parse(
        "<qos min_mbps=\"50\" max_mbps=\"200\" burst_kb=\"512\"/>", &out);
    g_assert_true(ok);
    g_assert_cmpuint(out.min_mbps, ==, 50);
    g_assert_cmpuint(out.max_mbps, ==, 200);
    g_assert_cmpuint(out.burst_kb, ==, 512);
}

                                                                
static void test_qos_metadata_parse_attr_reordered_and_burst_default(void) {
    PcvQosSla out;
    gboolean ok = _qos_metadata_parse(
        "<qos max_mbps=\"200\" min_mbps=\"50\"/>", &out);
    g_assert_true(ok);
    g_assert_cmpuint(out.min_mbps, ==, 50);
    g_assert_cmpuint(out.max_mbps, ==, 200);
    g_assert_cmpuint(out.burst_kb, ==, 256);
}

                                                                        
                                                                            
                                                                          
                                                          
                                                             
                                                                          
                                                                   
                                                                    
                                                      
                                                           
                                                  
                                                       
                                                             
                                                                        

static void test_qos_metadata_write_read_roundtrip_libvirt(void) {
    virConnectPtr conn = virConnectOpen("test:///default");
    if (!conn) {
        g_test_skip("libvirt test:/// 드라이버 사용 불가");
        return;
    }

    static const char *dom_xml =
        "<domain type='test'>"
        "  <name>pcv-qos-meta-test</name>"
        "  <memory unit='KiB'>65536</memory>"
        "  <os><type>hvm</type></os>"
        "</domain>";

    virDomainPtr dom = virDomainDefineXML(conn, dom_xml);
    if (!dom) {
        g_test_skip("test:///default 에서 도메인 define 실패");
        virConnectClose(conn);
        return;
    }

    PcvQosSla sla = { "", "", 100, 500, 256 };
    GError *werr = NULL;
    g_assert_true(pcv_vm_qos_metadata_write(dom, &sla, &werr));
    g_assert_no_error(werr);

    PcvQosSla out;
    memset(&out, 0, sizeof out);
    g_assert_cmpint(pcv_vm_qos_metadata_read(dom, &out), ==, PCV_QOS_META_OK);
    g_assert_cmpuint(out.min_mbps, ==, 100);
    g_assert_cmpuint(out.max_mbps, ==, 500);
    g_assert_cmpuint(out.burst_kb, ==, 256);

    virDomainUndefine(dom);
    virDomainFree(dom);
    virConnectClose(conn);
}

                                                          
                                                              
static void test_qos_metadata_read_absent_libvirt(void) {
    virConnectPtr conn = virConnectOpen("test:///default");
    if (!conn) {
        g_test_skip("libvirt test:/// 드라이버 사용 불가");
        return;
    }

                                                          
                                                       
    virDomainPtr dom = virDomainLookupByName(conn, "test");
    if (!dom) {
        g_test_skip("test:///default 기본 도메인 조회 실패");
        virConnectClose(conn);
        return;
    }

    PcvQosSla out;
    g_assert_cmpint(pcv_vm_qos_metadata_read(dom, &out), ==, PCV_QOS_META_ABSENT);

    virDomainFree(dom);
    virConnectClose(conn);
}

                                                    
                                                    
                                                               
                                                      
                                                 
                                                        
static void test_qos_metadata_read_invalid_unparseable_libvirt(void) {
    virConnectPtr conn = virConnectOpen("test:///default");
    if (!conn) {
        g_test_skip("libvirt test:/// 드라이버 사용 불가");
        return;
    }

    static const char *dom_xml =
        "<domain type='test'>"
        "  <name>pcv-qos-meta-invalid-test</name>"
        "  <memory unit='KiB'>65536</memory>"
        "  <os><type>hvm</type></os>"
        "</domain>";

    virDomainPtr dom = virDomainDefineXML(conn, dom_xml);
    if (!dom) {
        g_test_skip("test:///default 에서 도메인 define 실패");
        virConnectClose(conn);
        return;
    }

                                                                     
                                                
    int ret = virDomainSetMetadata(dom, VIR_DOMAIN_METADATA_ELEMENT,
                                    "<qos garbage='true'/>", "pcv",
                                    PCV_QOS_METADATA_URI, VIR_DOMAIN_AFFECT_CONFIG);
    g_assert_cmpint(ret, ==, 0);

    PcvQosSla out;
    g_assert_cmpint(pcv_vm_qos_metadata_read(dom, &out), ==, PCV_QOS_META_INVALID);

    virDomainUndefine(dom);
    virDomainFree(dom);
    virConnectClose(conn);
}

                                                                        
                                                                       
                                                                        

                                                                  
                                                           
                                                        
                                                        
                            
static void test_reconcile_diff_orphan_and_missing(void) {
    GPtrArray *exp = g_ptr_array_new();
    g_ptr_array_add(exp, (gpointer)"2:16"); g_ptr_array_add(exp, (gpointer)"2:17");
    GPtrArray *act = g_ptr_array_new();
    g_ptr_array_add(act, (gpointer)"2:16"); g_ptr_array_add(act, (gpointer)"2:99");

    GPtrArray *diff = pcv_qos_reconcile_diff(exp, act);
    g_assert_cmpuint(diff->len, ==, 3);                                     

    gboolean saw_missing_17 = FALSE, saw_orphan_99 = FALSE, saw_ok_16 = FALSE;
    for (guint i = 0; i < diff->len; i++) {
        PcvQosReconEntry *e = g_ptr_array_index(diff, i);
        if (g_strcmp0(e->classid, "2:17") == 0 && e->action == PCV_QOS_RECON_MISSING)
            saw_missing_17 = TRUE;
        if (g_strcmp0(e->classid, "2:99") == 0 && e->action == PCV_QOS_RECON_ORPHAN)
            saw_orphan_99 = TRUE;
        if (g_strcmp0(e->classid, "2:16") == 0 && e->action == PCV_QOS_RECON_OK)
            saw_ok_16 = TRUE;
    }
    g_assert_true(saw_missing_17);
    g_assert_true(saw_orphan_99);
    g_assert_true(saw_ok_16);

    g_ptr_array_unref(diff);
    g_ptr_array_unref(exp);
    g_ptr_array_unref(act);
}

static void test_reconcile_diff_empty_both_is_empty(void) {
    GPtrArray *exp = g_ptr_array_new();
    GPtrArray *act = g_ptr_array_new();
    GPtrArray *diff = pcv_qos_reconcile_diff(exp, act);
    g_assert_nonnull(diff);
    g_assert_cmpuint(diff->len, ==, 0);
    g_ptr_array_unref(diff);
    g_ptr_array_unref(exp);
    g_ptr_array_unref(act);
}

static void test_reconcile_diff_full_overlap_all_ok(void) {
    GPtrArray *exp = g_ptr_array_new();
    g_ptr_array_add(exp, (gpointer)"1:1001"); g_ptr_array_add(exp, (gpointer)"1:1002");
    GPtrArray *act = g_ptr_array_new();
    g_ptr_array_add(act, (gpointer)"1:1001"); g_ptr_array_add(act, (gpointer)"1:1002");

    GPtrArray *diff = pcv_qos_reconcile_diff(exp, act);
    g_assert_cmpuint(diff->len, ==, 2);
    for (guint i = 0; i < diff->len; i++) {
        PcvQosReconEntry *e = g_ptr_array_index(diff, i);
        g_assert_cmpint(e->action, ==, PCV_QOS_RECON_OK);
    }

    g_ptr_array_unref(diff);
    g_ptr_array_unref(exp);
    g_ptr_array_unref(act);
}

                                                                        
                                                                       
                                                                        

                                                                
                                                               
                                                           
#define T3_CANNED_CLASS_SHOW \
    "class hfsc 1:1001 parent 1:10 leaf 801b: sc m1 0bit d 0us m2 100Mbit ul m1 0bit d 0us m2 500Mbit \n" \
    "class hfsc 1: root \n" \
    "class hfsc 1:1 parent 1: ls m1 0bit d 0us m2 1Gbit ul m1 0bit d 0us m2 1Gbit \n" \
    "class hfsc 1:fffe parent 1:1 ls m1 0bit d 0us m2 1Mbit ul m1 0bit d 0us m2 1Gbit \n" \
    "class hfsc 1:10 parent 1:1 ls m1 0bit d 0us m2 1Gbit ul m1 0bit d 0us m2 1Gbit \n" \
    "class hfsc 1:1002 parent 1:10 leaf 801c: ls m1 0bit d 0us m2 200Mbit ul m1 0bit d 0us m2 200Mbit \n"

static void test_parse_class_show_extracts_vm_leaves_only(void) {
    GPtrArray *ids = pcv_qos_parse_class_show(T3_CANNED_CLASS_SHOW);
    g_assert_cmpuint(ids->len, ==, 2);                                          

    gboolean saw_1001 = FALSE, saw_1002 = FALSE;
    for (guint i = 0; i < ids->len; i++) {
        const gchar *id = g_ptr_array_index(ids, i);
        if (g_strcmp0(id, "1:1001") == 0) saw_1001 = TRUE;
        if (g_strcmp0(id, "1:1002") == 0) saw_1002 = TRUE;
                                                                      
        g_assert_cmpstr(id, !=, "1:1");
        g_assert_cmpstr(id, !=, "1:fffe");
        g_assert_cmpstr(id, !=, "1:10");
        g_assert_cmpstr(id, !=, "1:");
    }
    g_assert_true(saw_1001);
    g_assert_true(saw_1002);
    g_ptr_array_unref(ids);
}

                                                          
                                              
                                    
static void test_parse_class_show_empty_and_null_output(void) {
    GPtrArray *ids_empty = pcv_qos_parse_class_show("");
    g_assert_nonnull(ids_empty);
    g_assert_cmpuint(ids_empty->len, ==, 0);
    g_ptr_array_unref(ids_empty);

    GPtrArray *ids_null = pcv_qos_parse_class_show(NULL);
    g_assert_nonnull(ids_null);
    g_assert_cmpuint(ids_null->len, ==, 0);
    g_ptr_array_unref(ids_null);
}

                                                                        
                                                                           
                                                                        

                                                             
                                                                     
                                                                    
                                                                
                                                 
#define T6_CANNED_CLASS_STATS \
    "class hfsc 1: root \n" \
    " Sent 0 bytes 0 pkt (dropped 0, overlimits 0 requeues 0) \n" \
    " backlog 0b 0p requeues 0\n" \
    " period 0 level 3 \n" \
    "\n" \
    "class hfsc 1:fffe parent 1:1 ls m1 0bit d 0us m2 1Mbit ul m1 0bit d 0us m2 100Mbit \n" \
    " Sent 70 bytes 1 pkt (dropped 0, overlimits 0 requeues 0) \n" \
    " backlog 0b 0p requeues 0\n" \
    " period 1 work 70 bytes level 0 \n" \
    "\n" \
    "class hfsc 1:1 parent 1: ls m1 0bit d 0us m2 100Mbit ul m1 0bit d 0us m2 100Mbit \n" \
    " Sent 0 bytes 0 pkt (dropped 0, overlimits 0 requeues 0) \n" \
    " backlog 0b 0p requeues 0\n" \
    " period 41 work 81750 bytes level 2 \n" \
    "\n" \
    "class hfsc 1:10 parent 1:1 ls m1 0bit d 0us m2 100Mbit ul m1 0bit d 0us m2 100Mbit \n" \
    " Sent 0 bytes 0 pkt (dropped 0, overlimits 0 requeues 0) \n" \
    " backlog 0b 0p requeues 0\n" \
    " period 40 work 81680 bytes level 1 \n" \
    "\n" \
    "class hfsc 1:1000 parent 1:10 leaf 803a: sc m1 0bit d 0us m2 10Mbit ul m1 0bit d 0us m2 50Mbit \n" \
    " Sent 1698670 bytes 442 pkt (dropped 7, overlimits 0 requeues 0) \n" \
    " backlog 0b 0p requeues 0\n" \
    " period 40 work 81680 bytes rtwork 40840 bytes level 0 \n" \
    "\n" \
    "class cake 803a:3e8 parent 803a: \n" \
    " (dropped 0, overlimits 0 requeues 0) \n" \
    " backlog 0b 0p requeues 0\n" \
    "  deficit -1438 count 0 blue_prob 0\n"

static void test_parse_class_stats_extracts_vm_leaf_bytes_and_drops(void) {
    GPtrArray *stats = pcv_qos_parse_class_stats(T6_CANNED_CLASS_STATS);
    g_assert_cmpuint(stats->len, ==, 1);                                                  

    PcvQosClassStat *s = g_ptr_array_index(stats, 0);
    g_assert_cmpstr(s->classid, ==, "1:1000");
    g_assert_cmpuint(s->bytes, ==, 1698670);
    g_assert_cmpuint(s->drops, ==, 7);
                                                                       
    g_assert_null(s->tenant);
    g_assert_null(s->vm);

    g_ptr_array_unref(stats);
}

static void test_parse_class_stats_empty_and_null_output(void) {
    GPtrArray *empty = pcv_qos_parse_class_stats("");
    g_assert_nonnull(empty);
    g_assert_cmpuint(empty->len, ==, 0);
    g_ptr_array_unref(empty);

    GPtrArray *null_out = pcv_qos_parse_class_stats(NULL);
    g_assert_nonnull(null_out);
    g_assert_cmpuint(null_out->len, ==, 0);
    g_ptr_array_unref(null_out);
}

                                                                        
                                                                       
                                                                        

static void test_reverse_lookup_vm_resolves_known_minor(void) {
    pcv_qos_ids_clear();
    guint16 minor = pcv_qos_vm_minor("acme", "vm1");

    gchar *tenant = NULL, *vm = NULL;
    g_assert_true(pcv_qos_reverse_lookup_vm(minor, &tenant, &vm));
    g_assert_cmpstr(tenant, ==, "acme");
    g_assert_cmpstr(vm, ==, "vm1");
    g_free(tenant);
    g_free(vm);
}

static void test_reverse_lookup_vm_unknown_minor_returns_false(void) {
    pcv_qos_ids_clear();
    gchar *tenant = NULL, *vm = NULL;
                                                   
    g_assert_false(pcv_qos_reverse_lookup_vm(0x1234, &tenant, &vm));
    g_assert_null(tenant);
    g_assert_null(vm);
}

                                                                        
                                                                
                                                                        

static void test_tenant_sla_persist_roundtrip(void) {
    pcv_qos_tenant_sla_clear();

                                                                       
                                                                
                                                         
                                                                 
                            
    gchar *tmpdir = g_dir_make_tmp("pcv-qos-tenant-test-XXXXXX", NULL);
    g_assert_nonnull(tmpdir);
    gchar *path = g_build_filename(tmpdir, "qos_tenants.json", NULL);
    pcv_qos_set_tenant_sla_path_for_test(path);

    GError *err = NULL;
                                                                  
                                                          
                                                                                
                      
    g_assert_true(pcv_qos_tenant_sla_set("acme", 50, 200, NULL, NULL, &err));
    g_assert_no_error(err);

    PcvQosTenantSla before;
    g_assert_true(pcv_qos_tenant_sla_get("acme", &before));
    g_assert_cmpuint(before.min_mbps, ==, 50);
    g_assert_cmpuint(before.max_mbps, ==, 200);

    g_assert_true(pcv_qos_tenant_sla_save(path, &err));
    g_assert_no_error(err);

    pcv_qos_tenant_sla_clear();
    g_assert_false(pcv_qos_tenant_sla_get("acme", &before));                 

    g_assert_true(pcv_qos_tenant_sla_load(path));
    PcvQosTenantSla after;
    g_assert_true(pcv_qos_tenant_sla_get("acme", &after));
    g_assert_cmpuint(after.min_mbps, ==, 50);
    g_assert_cmpuint(after.max_mbps, ==, 200);

    g_unlink(path);
    g_free(path);
    g_rmdir(tmpdir);
    g_free(tmpdir);
    pcv_qos_tenant_sla_clear();
    pcv_qos_set_tenant_sla_path_for_test(NULL);
}

static void test_tenant_sla_load_missing_file_is_empty_ok(void) {
    pcv_qos_tenant_sla_clear();
    g_assert_true(pcv_qos_tenant_sla_load("/nonexistent/pcv-qos-tenant-test/qos_tenants.json"));
    PcvQosTenantSla out;
    g_assert_false(pcv_qos_tenant_sla_get("anything", &out));
}

static void test_tenant_sla_rejects_min_gt_max(void) {
    pcv_qos_tenant_sla_clear();
    GError *err = NULL;
    g_assert_false(pcv_qos_tenant_sla_set("acme", 200, 50, NULL, NULL, &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&err);
}

static void test_tenant_sla_rejects_zero_max(void) {
    pcv_qos_tenant_sla_clear();
    GError *err = NULL;
    g_assert_false(pcv_qos_tenant_sla_set("acme", 0, 0, NULL, NULL, &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&err);
}

static void test_tenant_sla_load_rejects_malformed_json(void) {
    pcv_qos_tenant_sla_clear();
    gchar *tmpdir = g_dir_make_tmp("pcv-qos-tenant-bad-XXXXXX", NULL);
    g_assert_nonnull(tmpdir);
    gchar *path = g_build_filename(tmpdir, "qos_tenants.json", NULL);

                                                     
    const gchar *bad = "{\"acme\": {\"min_mbps\": 500, \"max_mbps\": 100}}";
    g_assert_true(g_file_set_contents(path, bad, -1, NULL));

    g_assert_false(pcv_qos_tenant_sla_load(path));
    PcvQosTenantSla out;
    g_assert_false(pcv_qos_tenant_sla_get("acme", &out));

    g_unlink(path);
    g_free(path);
    g_rmdir(tmpdir);
    g_free(tmpdir);
}

                                                                        
                                                                     
                                                                        
                                                                        

static void test_iface_is_managed_untracked_returns_false(void) {
    g_assert_false(pcv_qos_iface_is_managed("never-applied-iface-d09t6"));
    g_assert_false(pcv_qos_iface_is_managed(NULL));
    g_assert_false(pcv_qos_iface_is_managed(""));
}

                                                                        
                                                                    
                                                                        

static void test_reconcile_noop_when_provider_unset(void) {
    pcv_qos_set_expected_provider(NULL);                              
    GError *err = NULL;
    g_assert_true(pcv_qos_reconcile(&err));
    g_assert_no_error(err);
}

                                                                        
                                                                     
                                                                        

                                                        
                                                          
                                                        
                                                           
                                                   
                               
static void test_qos_lookup_applied_untracked_returns_false(void) {
    gchar *tenant = NULL, *iface = NULL;
    g_assert_false(pcv_qos_lookup_applied("never-applied-vm-d09t5", &tenant, &iface));
    g_assert_null(tenant);
    g_assert_null(iface);
}

                                                                        
                                                                          
                                                                        

                                                            
                                                         
                                                                
                                                                 
                                                                
                                                     
                                                                           
                                                        
                                                     
                                               
static void test_derive_context_no_metadata_no_iface_returns_false(void) {
    virConnectPtr conn = virConnectOpen("test:///default");
    if (!conn) {
        g_test_skip("libvirt test:/// 드라이버 사용 불가");
        return;
    }

    static const char *dom_xml =
        "<domain type='test'>"
        "  <name>pcv-qos-derive-no-iface-test</name>"
        "  <memory unit='KiB'>65536</memory>"
        "  <os><type>hvm</type></os>"
        "</domain>";
    virDomainPtr dom = virDomainDefineXML(conn, dom_xml);
    if (!dom) {
        g_test_skip("test:///default 에서 도메인 define 실패");
        virConnectClose(conn);
        return;
    }

    gchar *tenant = NULL, *iface = NULL;
    PcvQosSla sla;
    gboolean ok = pcv_vm_qos_derive_context(dom, "pcv-qos-derive-no-iface-test",
                                            &tenant, &iface, &sla);
    g_assert_false(ok);
    g_assert_null(tenant);
    g_assert_null(iface);

    virDomainUndefine(dom);
    virDomainFree(dom);
    virConnectClose(conn);
}

                                                                 
                                                              
                                                   
                                                    
                                                              
                                                               
static void test_derive_context_overlay_declared_but_not_joined_returns_false(void) {
    virConnectPtr conn = virConnectOpen("test:///default");
    if (!conn) {
        g_test_skip("libvirt test:/// 드라이버 사용 불가");
        return;
    }

    static const char *dom_xml =
        "<domain type='test'>"
        "  <name>pcv-qos-derive-unjoined-test</name>"
        "  <memory unit='KiB'>65536</memory>"
        "  <os><type>hvm</type></os>"
        "</domain>";
    virDomainPtr dom = virDomainDefineXML(conn, dom_xml);
    if (!dom) {
        g_test_skip("test:///default 에서 도메인 define 실패");
        virConnectClose(conn);
        return;
    }

    GError *terr = NULL;
    gboolean created = pcv_tenant_overlay_create("qos-t5-unjoined-test", &terr);
    g_assert_true(created);
    g_assert_no_error(terr);

    gchar *meta = g_strdup_printf(
        "<pcv:overlay network_mode='tenant-overlay' tenant='%s'/>",
        "qos-t5-unjoined-test");
    int ret = virDomainSetMetadata(dom, VIR_DOMAIN_METADATA_ELEMENT, meta, "pcv",
                                    PCV_OVERLAY_METADATA_URI, VIR_DOMAIN_AFFECT_CONFIG);
    g_free(meta);
    g_assert_cmpint(ret, ==, 0);

    gchar *tenant = NULL, *iface = NULL;
    PcvQosSla sla;
    gboolean ok = pcv_vm_qos_derive_context(dom, "pcv-qos-derive-unjoined-test",
                                            &tenant, &iface, &sla);
    g_assert_false(ok);
    g_assert_null(tenant);
    g_assert_null(iface);

    GError *derr = NULL;
    pcv_tenant_overlay_delete("qos-t5-unjoined-test", &derr);                      
    g_clear_error(&derr);

    virDomainUndefine(dom);
    virDomainFree(dom);
    virConnectClose(conn);
}

void test_qos_register(void) {
    g_test_add_func("/qos/classid_deterministic", test_classid_deterministic);
    g_test_add_func("/qos/classid_format_is_hex", test_classid_format_is_hex);
    g_test_add_func("/qos/tenant_classid_format_is_hex", test_tenant_classid_format_is_hex);
    g_test_add_func("/qos/tenant_minor_distinct", test_tenant_minor_distinct);
    g_test_add_func("/qos/vm_minor_distinct_within_tenant", test_vm_minor_distinct_within_tenant);
    g_test_add_func("/qos/vm_minor_distinct_across_tenants", test_vm_minor_distinct_across_tenants);
    g_test_add_func("/qos/tenant_minor_range", test_tenant_minor_range);
    g_test_add_func("/qos/vm_minor_range", test_vm_minor_range);
    g_test_add_func("/qos/tenant_minor_collision_probes", test_tenant_minor_collision_probes);
    g_test_add_func("/qos/ids_persist_roundtrip", test_ids_persist_roundtrip);
    g_test_add_func("/qos/ids_load_missing_file_is_empty_ok", test_ids_load_missing_file_is_empty_ok);
    g_test_add_func("/qos/ids_load_rejects_out_of_range_minor", test_ids_load_rejects_out_of_range_minor);
    g_test_add_func("/qos/ids_load_rejects_duplicate_minor", test_ids_load_rejects_duplicate_minor);
    g_test_add_func("/qos/ids_load_rejects_json_syntax_error", test_ids_load_rejects_json_syntax_error);
    g_test_add_func("/qos/sla_min_gt_max", test_sla_rejects_min_gt_max);
    g_test_add_func("/qos/sla_zero_max", test_sla_rejects_zero_max);
    g_test_add_func("/qos/sla_rejects_negative_min", test_sla_rejects_negative_min);
    g_test_add_func("/qos/sla_rejects_negative_max", test_sla_rejects_negative_max);
    g_test_add_func("/qos/sla_rejects_negative_burst", test_sla_rejects_negative_burst);
    g_test_add_func("/qos/sla_rejects_max_mbps_overflow", test_sla_rejects_max_mbps_overflow);
    g_test_add_func("/qos/sla_rejects_min_mbps_overflow", test_sla_rejects_min_mbps_overflow);
    g_test_add_func("/qos/sla_burst_default_256", test_sla_burst_default_256);
    g_test_add_func("/qos/sla_accepts_explicit_burst", test_sla_accepts_explicit_burst);

                                                       
    g_test_add_func("/qos/apply_vm_rejects_invalid_vm_iface", test_apply_vm_rejects_invalid_vm_iface);
    g_test_add_func("/qos/apply_vm_before_ensure_root_returns_uplink_error", test_apply_vm_before_ensure_root_returns_uplink_error);

                                                                         
    g_test_add_func("/qos/qos_applicable_bridge_nic_unspecified_true", test_qos_applicable_bridge_nic_unspecified_true);
    g_test_add_func("/qos/qos_applicable_bridge_nic_bridge_true", test_qos_applicable_bridge_nic_bridge_true);
    g_test_add_func("/qos/qos_applicable_tenant_overlay_nic_unspecified_true", test_qos_applicable_tenant_overlay_nic_unspecified_true);
    g_test_add_func("/qos/qos_applicable_tenant_overlay_nic_dpdk_still_true", test_qos_applicable_tenant_overlay_nic_dpdk_still_true);
    g_test_add_func("/qos/qos_applicable_tenant_overlay_nic_sriov_still_true", test_qos_applicable_tenant_overlay_nic_sriov_still_true);
    g_test_add_func("/qos/qos_applicable_network_mode_dpdk_false", test_qos_applicable_network_mode_dpdk_false);
    g_test_add_func("/qos/qos_applicable_network_mode_sriov_false", test_qos_applicable_network_mode_sriov_false);
    g_test_add_func("/qos/qos_applicable_unspecified_mode_nic_dpdk_false", test_qos_applicable_unspecified_mode_nic_dpdk_false);
    g_test_add_func("/qos/qos_applicable_unspecified_mode_nic_sriov_false", test_qos_applicable_unspecified_mode_nic_sriov_false);
    g_test_add_func("/qos/qos_applicable_unspecified_defaults_true", test_qos_applicable_unspecified_defaults_true);
    g_test_add_func("/qos/qos_applicable_unknown_value_defaults_true", test_qos_applicable_unknown_value_defaults_true);

                                               
    g_test_add_func("/qos/metadata_build_parse_roundtrip", test_qos_metadata_build_parse_roundtrip);
    g_test_add_func("/qos/metadata_build_null_sla_is_empty", test_qos_metadata_build_null_sla_is_empty);
    g_test_add_func("/qos/metadata_parse_absent_returns_false", test_qos_metadata_parse_absent_returns_false);
    g_test_add_func("/qos/metadata_parse_malformed_missing_max_returns_false", test_qos_metadata_parse_malformed_missing_max_returns_false);
    g_test_add_func("/qos/metadata_parse_null_safe", test_qos_metadata_parse_null_safe);
    g_test_add_func("/qos/metadata_parse_libvirt_stripped_form", test_qos_metadata_parse_libvirt_stripped_form);
    g_test_add_func("/qos/metadata_parse_attr_reordered_and_burst_default", test_qos_metadata_parse_attr_reordered_and_burst_default);
    g_test_add_func("/qos/metadata_write_read_roundtrip_libvirt", test_qos_metadata_write_read_roundtrip_libvirt);
    g_test_add_func("/qos/metadata_read_absent_libvirt", test_qos_metadata_read_absent_libvirt);
    g_test_add_func("/qos/metadata_read_invalid_unparseable_libvirt", test_qos_metadata_read_invalid_unparseable_libvirt);

                                 
    g_test_add_func("/qos/reconcile_diff_orphan_and_missing", test_reconcile_diff_orphan_and_missing);
    g_test_add_func("/qos/reconcile_diff_empty_both_is_empty", test_reconcile_diff_empty_both_is_empty);
    g_test_add_func("/qos/reconcile_diff_full_overlap_all_ok", test_reconcile_diff_full_overlap_all_ok);
    g_test_add_func("/qos/parse_class_show_extracts_vm_leaves_only", test_parse_class_show_extracts_vm_leaves_only);
    g_test_add_func("/qos/parse_class_show_empty_and_null_output", test_parse_class_show_empty_and_null_output);
    g_test_add_func("/qos/reconcile_noop_when_provider_unset", test_reconcile_noop_when_provider_unset);

                                                                    
    g_test_add_func("/qos/lookup_applied_untracked_returns_false", test_qos_lookup_applied_untracked_returns_false);
    g_test_add_func("/qos/derive_context_no_metadata_no_iface_returns_false", test_derive_context_no_metadata_no_iface_returns_false);
    g_test_add_func("/qos/derive_context_overlay_declared_but_not_joined_returns_false", test_derive_context_overlay_declared_but_not_joined_returns_false);

                                                            
    g_test_add_func("/qos/parse_class_stats_extracts_vm_leaf_bytes_and_drops", test_parse_class_stats_extracts_vm_leaf_bytes_and_drops);
    g_test_add_func("/qos/parse_class_stats_empty_and_null_output", test_parse_class_stats_empty_and_null_output);
    g_test_add_func("/qos/reverse_lookup_vm_resolves_known_minor", test_reverse_lookup_vm_resolves_known_minor);
    g_test_add_func("/qos/reverse_lookup_vm_unknown_minor_returns_false", test_reverse_lookup_vm_unknown_minor_returns_false);
    g_test_add_func("/qos/tenant_sla_persist_roundtrip", test_tenant_sla_persist_roundtrip);
    g_test_add_func("/qos/tenant_sla_load_missing_file_is_empty_ok", test_tenant_sla_load_missing_file_is_empty_ok);
    g_test_add_func("/qos/tenant_sla_rejects_min_gt_max", test_tenant_sla_rejects_min_gt_max);
    g_test_add_func("/qos/tenant_sla_rejects_zero_max", test_tenant_sla_rejects_zero_max);
    g_test_add_func("/qos/tenant_sla_load_rejects_malformed_json", test_tenant_sla_load_rejects_malformed_json);
    g_test_add_func("/qos/iface_is_managed_untracked_returns_false", test_iface_is_managed_untracked_returns_false);
}
