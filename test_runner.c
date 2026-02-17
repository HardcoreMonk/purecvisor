/* test_runner.c */
#include <glib.h>
#include <stdio.h>

/* 모듈 헤더 포함 (경로에 주의) */
#include "src/modules/virt/vm_config_builder.h"
#include "src/modules/storage/zfs_driver.h"

// 비동기 콜백 더미 함수
static void 
dummy_callback(GObject *source, GAsyncResult *res, gpointer user_data) {
    (void)source; (void)res; (void)user_data;
    g_print("[Async Callback] ZFS Operation Finished (Dummy)\n");
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    
    g_print("=== PureCVisor Phase 3 Compilation Test ===\n");

    /* 1. Test XML Builder */
    g_print("\n[TEST 1] Testing VmConfigBuilder...\n");
    GError *error = NULL;
    PureCVisorVmConfigBuilder *builder = purecvisor_vm_config_builder_new();
    
    PureCVisorVmConfig conf = {
        .name = "test-vm-01",
        .memory_kb = 2048 * 1024,
        .vcpus = 2,
        .disk_path = "/dev/zvol/tank/test-vm-01",
        .bridge_iface = "virbr0"
    };

    if (!purecvisor_vm_config_builder_set_config(builder, &conf, &error)) {
        g_printerr("Failed to set config: %s\n", error->message);
        return 1;
    }

    gchar *xml = purecvisor_vm_config_builder_generate_xml(builder, &error);
    if (!xml) {
        g_printerr("Failed to generate XML: %s\n", error->message);
        return 1;
    }

    g_print(">> Generated XML Length: %lu characters\n", strlen(xml));
    // g_print("%s\n", xml); // Uncomment to see full XML
    g_free(xml);
    g_object_unref(builder);
    g_print("[PASS] XML Builder test passed.\n");


    /* 2. Test ZFS Driver Linking */
    g_print("\n[TEST 2] Testing ZFS Driver Linking...\n");
    PureCVisorZfsDriver *zfs = purecvisor_zfs_driver_new();
    
    // 실제로 실행하지는 않고, 심볼 링크가 잘 되었는지 호출만 해봅니다.
    // 실제 실행 시엔 root 권한과 zfs가 필요하므로 컴파일 여부만 확인합니다.
    purecvisor_zfs_driver_create_vol_async(zfs, "tank", "testvol", 1024*1024*100, 
                                           NULL, dummy_callback, NULL);
    
    g_print(">> ZFS Driver async function called successfully.\n");
    g_object_unref(zfs);
    g_print("[PASS] ZFS Driver linking passed.\n");

    return 0;
}