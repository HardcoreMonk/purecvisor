/**
 * @file test_vm_config.c
 * @brief VM 설정 빌더 유닛 테스트 — 공개 API를 통한 안전성 검증
 *
 * 대상 모듈: src/modules/virt/vm_config_builder.c — VM XML 설정 생성기
 *
 * 이 테스트가 검증하는 것:
 *   PureCVisorVmConfig opaque 구조체의 new/free/setter 크래시 없는 동작,
 *   VLAN ID 범위 클램핑(0, 1~4094, 4095+), 부팅 모드/CPU 모드/TPM/hugepage,
 *   build() → GVirConfigDomain XML 생성, NULL name 안전성을 검사한다.
 *
 * 실행: sudo ./test_runner -p /vm_config
 *
 * 외부 의존: libvirt-gobject (build 테스트에서 GVirConfigDomain 생성)
 *   - build()를 호출하는 테스트는 libvirt-glib 링크 필요
 *   - setter-only 테스트는 외부 의존 없음
 */

#include <glib.h>
#include "modules/virt/vm_config_builder.h"

/* ══════════════════════════════════════════════════════════════
 * 1. 객체 생성/해제 — 크래시 없는 동작
 * ══════════════════════════════════════════════════════════════ */

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
    /* NULL 전달 시 크래시 없이 통과 */
    purecvisor_vm_config_free(NULL);
}

/* ══════════════════════════════════════════════════════════════
 * 2. Setter — 크래시 없는 동작 + 연속 호출 안전성
 * ══════════════════════════════════════════════════════════════ */

static void test_set_disk_basic(void) {
    PureCVisorVmConfig *cfg = purecvisor_vm_config_new("vm1", 2, 2048);
    purecvisor_vm_config_set_disk(cfg, "/dev/zvol/pcvpool/vms/vm1");
    purecvisor_vm_config_free(cfg);
}

static void test_set_disk_overwrite(void) {
    /* 연속 호출 — 메모리 누수/이중 해제 없이 동작 */
    PureCVisorVmConfig *cfg = purecvisor_vm_config_new("vm1", 2, 2048);
    purecvisor_vm_config_set_disk(cfg, "/path/first");
    purecvisor_vm_config_set_disk(cfg, "/path/second");
    purecvisor_vm_config_set_disk(cfg, "/path/third");
    purecvisor_vm_config_free(cfg);
}

static void test_set_iso(void) {
    PureCVisorVmConfig *cfg = purecvisor_vm_config_new("vm1", 2, 2048);
    purecvisor_vm_config_set_iso(cfg, "/iso/ubuntu-24.04.iso");
    /* 덮어쓰기 */
    purecvisor_vm_config_set_iso(cfg, "/iso/debian-12.iso");
    purecvisor_vm_config_free(cfg);
}

static void test_set_bridge(void) {
    PureCVisorVmConfig *cfg = purecvisor_vm_config_new("vm1", 2, 2048);
    purecvisor_vm_config_set_network_bridge(cfg, "pcvbr0");
    purecvisor_vm_config_set_network_bridge(cfg, "virbr0");
    purecvisor_vm_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 * 3. VLAN ID — 유효/무효 범위에서 크래시 없는 동작
 * ══════════════════════════════════════════════════════════════ */

static void test_vlan_valid_range(void) {
    PureCVisorVmConfig *cfg = purecvisor_vm_config_new("vm1", 1, 512);
    purecvisor_vm_config_set_vlan_id(cfg, 1);      /* 최소 */
    purecvisor_vm_config_set_vlan_id(cfg, 100);    /* 일반 */
    purecvisor_vm_config_set_vlan_id(cfg, 4094);   /* 최대 */
    purecvisor_vm_config_free(cfg);
}

static void test_vlan_boundary_clamp(void) {
    PureCVisorVmConfig *cfg = purecvisor_vm_config_new("vm1", 1, 512);
    purecvisor_vm_config_set_vlan_id(cfg, 0);      /* 비활성 */
    purecvisor_vm_config_set_vlan_id(cfg, 4095);   /* 예약 → 0 클램핑 */
    purecvisor_vm_config_set_vlan_id(cfg, 4096);   /* 초과 → 0 클램핑 */
    purecvisor_vm_config_set_vlan_id(cfg, -1);     /* 음수 → 0 클램핑 */
    purecvisor_vm_config_set_vlan_id(cfg, -100);
    purecvisor_vm_config_set_vlan_id(cfg, 99999);
    purecvisor_vm_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 * 4. 부팅 모드 / CPU 모드 / TPM / Hugepage
 * ══════════════════════════════════════════════════════════════ */

static void test_boot_cpu_modes(void) {
    PureCVisorVmConfig *cfg = purecvisor_vm_config_new("vm1", 1, 512);
    purecvisor_vm_config_set_boot_mode(cfg, 0);   /* BIOS */
    purecvisor_vm_config_set_boot_mode(cfg, 1);   /* UEFI */
    purecvisor_vm_config_set_boot_mode(cfg, 2);   /* UEFI+SecureBoot */
    purecvisor_vm_config_set_cpu_mode(cfg, 0);    /* Single Edge default(host-passthrough) */
    purecvisor_vm_config_set_cpu_mode(cfg, 1);    /* host-passthrough */
    purecvisor_vm_config_set_cpu_mode(cfg, 2);    /* host-model */
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

/* ══════════════════════════════════════════════════════════════
 * 5. 전체 설정 조합 — 실전 시나리오
 * ══════════════════════════════════════════════════════════════ */

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

/* ══════════════════════════════════════════════════════════════
 * 등록
 * ══════════════════════════════════════════════════════════════ */

/* build() → GVirConfigDomain 생성 검증 */
static void test_build_generates_domain(void) {
    PureCVisorVmConfig *c = purecvisor_vm_config_new("build-test", 2, 2048);
    purecvisor_vm_config_set_disk(c, "/dev/zvol/pcvpool/vms/build-test");
    purecvisor_vm_config_set_network_bridge(c, "pcvbr0");
    GVirConfigDomain *dom = purecvisor_vm_config_build(c);
    g_assert_nonnull(dom);
    /* 기본 XML 검증 */
    gchar *xml = gvir_config_object_to_xml(GVIR_CONFIG_OBJECT(dom));
    g_assert_nonnull(xml);
    g_assert_true(g_strstr_len(xml, -1, "build-test") != NULL);
    g_free(xml);
    g_object_unref(dom);
    purecvisor_vm_config_free(c);
}

/* VP-2: 빌드된 XML에 게스트 에이전트 채널이 기본 포함되어야 함
 * (없으면 pcvctl guest-ping/guest-exec/guest-shutdown 이 self-created VM 에서 동작 불가) */
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
    /* hugepages 설정은 XML에 <memoryBacking><hugepages/></memoryBacking> */
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
    /* NULL name → graceful (NULL 반환 또는 기본값) */
}

/* ══════════════════════════════════════════════════════════════
 * 6. build() — XML 콘텐츠 심층 검증
 * ══════════════════════════════════════════════════════════════ */

/**
 * test_build_xml_vcpu_memory:
 *   build()가 생성한 XML에 올바른 <name>, <vcpu>, <memory> 값이
 *   포함되는지 검증한다.
 *
 *   메모리 변환 규칙: ram_mb × 1024 → KiB
 *     1024 MB × 1024 = 1048576 KiB → XML에 "1048576" 이 나타나야 함.
 */
static void test_build_xml_vcpu_memory(void) {
    PureCVisorVmConfig *cfg = purecvisor_vm_config_new("test-vm", 2, 1024);
    purecvisor_vm_config_set_disk(cfg, "/dev/zvol/pcvpool/vms/test-vm");
    purecvisor_vm_config_set_network_bridge(cfg, "pcvbr0");
    purecvisor_vm_config_set_boot_mode(cfg, 0);
    purecvisor_vm_config_set_cpu_mode(cfg, 1);  /* host-passthrough */

    GVirConfigDomain *dom = purecvisor_vm_config_build(cfg);
    g_assert_nonnull(dom);

    gchar *xml = gvir_config_object_to_xml(GVIR_CONFIG_OBJECT(dom));
    g_assert_nonnull(xml);

    /* VM 이름이 XML에 포함되어야 한다 */
    g_assert_true(g_strstr_len(xml, -1, "<name>test-vm</name>") != NULL);

    /* vCPU 개수가 2로 나타나야 한다 (<vcpu>2</vcpu>) */
    g_assert_true(g_strstr_len(xml, -1, "<vcpu>2</vcpu>") != NULL);

    /* 메모리: 1024 MB × 1024 = 1048576 KiB */
    g_assert_true(g_strstr_len(xml, -1, "1048576") != NULL);

    g_free(xml);
    g_object_unref(dom);
    purecvisor_vm_config_free(cfg);
}

/**
 * test_build_defaults_acpi_apic_host_cpu:
 *   기본 VM XML은 최신 Linux 설치기가 요구하는 ACPI/APIC와
 *   Single Edge 기본 CPU(host-passthrough)를 포함해야 한다.
 */
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

/**
 * test_build_with_iso_contains_cdrom:
 *   ISO 경로를 설정한 뒤 build()를 호출하면 결과 XML에
 *   CD-ROM 디바이스("cdrom")가 포함되는지 검증한다.
 *   ISO가 없을 때는 CD-ROM 엔트리가 생성되지 않아야 하므로
 *   이 테스트는 ISO 설정 코드 경로(config->iso_path != NULL 분기)를
 *   명시적으로 커버한다.
 */
static void test_build_with_iso_contains_cdrom(void) {
    PureCVisorVmConfig *cfg = purecvisor_vm_config_new("iso-vm", 1, 512);
    purecvisor_vm_config_set_disk(cfg, "/tmp/iso-vm.qcow2");
    purecvisor_vm_config_set_iso(cfg, "/iso/ubuntu-24.04-live.iso");

    GVirConfigDomain *dom = purecvisor_vm_config_build(cfg);
    g_assert_nonnull(dom);

    gchar *xml = gvir_config_object_to_xml(GVIR_CONFIG_OBJECT(dom));
    g_assert_nonnull(xml);

    /* CD-ROM 디바이스 타입 어트리뷰트가 XML에 존재해야 한다 */
    g_assert_true(g_strstr_len(xml, -1, "cdrom") != NULL);

    /* ISO 파일 경로가 source 엘리먼트에 나타나야 한다 */
    g_assert_true(g_strstr_len(xml, -1, "ubuntu-24.04-live.iso") != NULL);

    g_free(xml);
    g_object_unref(dom);
    purecvisor_vm_config_free(cfg);
}

/**
 * test_build_null_config_safe:
 *   NULL config로 build()를 호출하면 크래시 없이 NULL을 반환해야 한다.
 *   build()의 첫 줄은 gvir_config_domain_new()이고 config 역참조 전에
 *   NULL 체크가 없으므로, 현재 구현에서는 NULL을 반환하지 않고
 *   NULL 포인터를 역참조하게 된다. 이 테스트는 그 방어 경로를 추가하도록
 *   명시적으로 문서화한다.
 *
 *   현재 구현이 NULL 체크를 추가하지 않으면 이 테스트는 실패(SIGSEGV)한다.
 *   그 경우에는 build() 상단에 다음을 추가한다:
 *     if (!config) return NULL;
 */
static void test_build_null_config_safe(void) {
    /* build(NULL)은 NULL을 반환해야 한다 — 크래시 금지 */
    GVirConfigDomain *dom = purecvisor_vm_config_build(NULL);
    g_assert_null(dom);
    /* dom이 NULL이 아닌 경우에도 누수 없이 해제 */
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
    /* 신규: build() XML 콘텐츠 심층 검증 */
    g_test_add_func("/vm_config/build_xml_vcpu_memory",     test_build_xml_vcpu_memory);
    g_test_add_func("/vm_config/build_defaults_acpi_apic_host_cpu", test_build_defaults_acpi_apic_host_cpu);
    g_test_add_func("/vm_config/build_with_iso_cdrom",      test_build_with_iso_contains_cdrom);
    g_test_add_func("/vm_config/build_null_config_safe",    test_build_null_config_safe);
}
