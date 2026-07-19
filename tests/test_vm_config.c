
#include <glib.h>
#include "modules/virt/vm_config_builder.h"

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
    g_test_add_func("/vm_config/full_scenario",          test_full_config_scenario);
    g_test_add_func("/vm_config/build_generates_domain", test_build_generates_domain);
    g_test_add_func("/vm_config/build_guest_agent_channel", test_build_includes_guest_agent_channel);
    g_test_add_func("/vm_config/hugepages_flag",         test_hugepages_flag);
    g_test_add_func("/vm_config/null_name_safe",         test_null_name_safe);

    g_test_add_func("/vm_config/build_xml_vcpu_memory",     test_build_xml_vcpu_memory);
    g_test_add_func("/vm_config/build_defaults_acpi_apic_host_cpu", test_build_defaults_acpi_apic_host_cpu);
    g_test_add_func("/vm_config/build_with_iso_cdrom",      test_build_with_iso_contains_cdrom);
    g_test_add_func("/vm_config/build_null_config_safe",    test_build_null_config_safe);
}
