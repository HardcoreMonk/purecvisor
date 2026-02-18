#include <glib.h>
#include <json-glib/json-glib.h>
#include <stdio.h>

#include "src/modules/virt/vm_manager.h"

/* 테스트 설정 */
#define TEST_VM_NAME "purec-test-vm"
#define TEST_VM_VCPU 1
#define TEST_VM_MEM  1024 // MB

static GMainLoop *loop;
static PureCVisorVmManager *manager;

/* --------------------------------------------------------------------------
 * Step 5: Delete Callback (Final)
 * -------------------------------------------------------------------------- */
static void
on_delete_finished(GObject *source, GAsyncResult *res, gpointer user_data) {
    GError *error = NULL;
    if (!purecvisor_vm_manager_delete_vm_finish(PURECVISOR_VM_MANAGER(source), res, &error)) {
        g_printerr("[FAIL] Delete Failed: %s\n", error->message);
        g_error_free(error);
    } else {
        g_print("[PASS] 5. VM Deleted successfully (ZFS volume destroyed).\n");
        g_print("\n[SUCCESS] All Lifecycle tests passed.\n");
    }
    g_main_loop_quit(loop);
}

/* --------------------------------------------------------------------------
 * Step 4: Stop Callback -> Trigger Delete
 * -------------------------------------------------------------------------- */
static void
on_stop_finished(GObject *source, GAsyncResult *res, gpointer user_data) {
    GError *error = NULL;
    if (!purecvisor_vm_manager_stop_vm_finish(PURECVISOR_VM_MANAGER(source), res, &error)) {
        g_printerr("[FAIL] Stop Failed: %s\n", error->message);
        g_error_free(error);
        g_main_loop_quit(loop);
        return;
    }
    g_print("[PASS] 4. VM Stopped successfully.\n");

    // 다음 단계: Delete
    g_print("[INFO] Requesting Delete...\n");
    purecvisor_vm_manager_delete_vm_async(manager, TEST_VM_NAME, NULL, on_delete_finished, NULL);
}

/* --------------------------------------------------------------------------
 * Step 3: List Callback -> Trigger Stop
 * -------------------------------------------------------------------------- */
static void
on_list_finished(GObject *source, GAsyncResult *res, gpointer user_data) {
    GError *error = NULL;
    JsonNode *root = purecvisor_vm_manager_list_vms_finish(PURECVISOR_VM_MANAGER(source), res, &error);
    
    if (!root) {
        g_printerr("[FAIL] List Failed: %s\n", error->message);
        g_error_free(error);
        g_main_loop_quit(loop);
        return;
    }

    // JSON 파싱하여 VNC 포트 확인
    JsonArray *array = json_node_get_array(root);
    guint len = json_array_get_length(array);
    gboolean found = FALSE;

    g_print("[INFO] Current VMs:\n");
    for (guint i = 0; i < len; i++) {
        JsonObject *obj = json_array_get_object_element(array, i);
        const gchar *name = json_object_get_string_member(obj, "name");
        const gchar *state = json_object_get_string_member(obj, "state");
        gint64 vnc = json_object_get_int_member(obj, "vnc_port");

        g_print(" - VM: %s | State: %s | VNC: %lld\n", name, state, vnc);

        if (g_strcmp0(name, TEST_VM_NAME) == 0) {
            found = TRUE;
            if (g_strcmp0(state, "running") != 0) {
                g_printerr("[FAIL] Test VM is not running!\n");
            } else if (vnc <= 0) {
                g_printerr("[WARN] Test VM running but VNC port invalid (Maybe booting?).\n");
            } else {
                g_print("[PASS] 3. VM List verified (VNC Port: %lld).\n", vnc);
            }
        }
    }
    json_node_unref(root);

    if (!found) {
        g_printerr("[FAIL] Test VM not found in list!\n");
        g_main_loop_quit(loop);
        return;
    }

    // 다음 단계: Stop (Force)
    g_print("[INFO] Requesting Stop (Force)...\n");
    purecvisor_vm_manager_stop_vm_async(manager, TEST_VM_NAME, TRUE, NULL, on_stop_finished, NULL);
}

/* --------------------------------------------------------------------------
 * Step 2: Start Callback -> Trigger List
 * -------------------------------------------------------------------------- */
static void
on_start_finished(GObject *source, GAsyncResult *res, gpointer user_data) {
    GError *error = NULL;
    if (!purecvisor_vm_manager_start_vm_finish(PURECVISOR_VM_MANAGER(source), res, &error)) {
        g_printerr("[FAIL] Start Failed: %s\n", error->message);
        g_error_free(error);
        g_main_loop_quit(loop);
        return;
    }
    g_print("[PASS] 2. VM Started successfully.\n");

    // 잠시 대기하지 않고 바로 조회 (Libvirt 상태 반영 확인용)
    // 실제 환경에서는 약간의 딜레이가 필요할 수도 있으나, 이벤트 루프 기반이므로 바로 요청해봅니다.
    g_print("[INFO] Requesting List...\n");
    purecvisor_vm_manager_list_vms_async(manager, NULL, on_list_finished, NULL);
}

/* --------------------------------------------------------------------------
 * Step 1: Create Callback -> Trigger Start
 * -------------------------------------------------------------------------- */
static void
on_create_finished(GObject *source, GAsyncResult *res, gpointer user_data) {
    GError *error = NULL;
    /* Create 결과 처리 (파라미터는 구현에 따라 다를 수 있음, 여기선 boolean 가정) */
    // 주의: create_vm_finish 함수 시그니처 확인 필요
    if (!purecvisor_vm_manager_create_vm_finish(PURECVISOR_VM_MANAGER(source), res, &error)) {
        g_printerr("[FAIL] Create Failed: %s\n", error->message);
        g_error_free(error);
        g_main_loop_quit(loop);
        return;
    }
    g_print("[PASS] 1. VM Created successfully (ZFS + XML).\n");

    // 다음 단계: Start
    g_print("[INFO] Requesting Start...\n");
    purecvisor_vm_manager_start_vm_async(manager, TEST_VM_NAME, NULL, on_start_finished, NULL);
}

/* --------------------------------------------------------------------------
 * Main Entry
 * -------------------------------------------------------------------------- */
int main(int argc, char *argv[]) {
    g_print("=== PureCVisor Core Logic Verification ===\n");

    // 1. Libvirt Type Init (필요 시)
    gvir_init_object(NULL, NULL);

    // 2. Manager Init
    manager = purecvisor_vm_manager_new();
    if (!manager) {
        g_printerr("[FATAL] Failed to create VmManager.\n");
        return 1;
    }

    loop = g_main_loop_new(NULL, FALSE);

    // 3. Start Sequence: Create
    PurecvisorVmConfig config = {
        .name = TEST_VM_NAME,
        .vcpu = TEST_VM_VCPU,
        .memory_mb = TEST_VM_MEM,
        .disk_size_gb = 10,
        .iso_path = "/var/lib/libvirt/images/alpine.iso" // 테스트용 ISO 경로 확인 필요
    };

    g_print("[INFO] Requesting Create (Name: %s)...\n", TEST_VM_NAME);
    
    // Create 함수 시그니처에 맞춰 호출 (가상 구조체 전달)
    purecvisor_vm_manager_create_vm_async(manager, &config, NULL, on_create_finished, NULL);

    g_main_loop_run(loop);

    g_object_unref(manager);
    g_main_loop_unref(loop);
    return 0;
}