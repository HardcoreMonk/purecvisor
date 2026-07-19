
#include <glib.h>
#include <json-glib/json-glib.h>
#include <stdio.h>

#include "src/modules/virt/vm_manager.h"

#define TEST_VM_NAME "purec-test-vm"
#define TEST_VM_VCPU 1
#define TEST_VM_MEM  1024

static GMainLoop *loop;
static PureCVisorVmManager *manager;

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

    g_print("[INFO] Requesting Delete...\n");
    purecvisor_vm_manager_delete_vm_async(manager, TEST_VM_NAME, NULL, on_delete_finished, NULL);
}

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

    g_print("[INFO] Requesting Stop (Force)...\n");
    purecvisor_vm_manager_stop_vm_async(manager, TEST_VM_NAME, TRUE, NULL, on_stop_finished, NULL);
}

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

    g_print("[INFO] Requesting List...\n");
    purecvisor_vm_manager_list_vms_async(manager, NULL, on_list_finished, NULL);
}

static void
on_create_finished(GObject *source, GAsyncResult *res, gpointer user_data) {
    GError *error = NULL;

    if (!purecvisor_vm_manager_create_vm_finish(PURECVISOR_VM_MANAGER(source), res, &error)) {
        g_printerr("[FAIL] Create Failed: %s\n", error->message);
        g_error_free(error);
        g_main_loop_quit(loop);
        return;
    }
    g_print("[PASS] 1. VM Created successfully (ZFS + XML).\n");

    g_print("[INFO] Requesting Start...\n");
    purecvisor_vm_manager_start_vm_async(manager, TEST_VM_NAME, NULL, on_start_finished, NULL);
}

int main(int argc, char *argv[]) {
    g_print("=== PureCVisor Core Logic Verification ===\n");

    gvir_init_object(NULL, NULL);

    manager = purecvisor_vm_manager_new();
    if (!manager) {
        g_printerr("[FATAL] Failed to create VmManager.\n");
        return 1;
    }

    loop = g_main_loop_new(NULL, FALSE);

    g_print("[INFO] Requesting Create (Name: %s)...\n", TEST_VM_NAME);

    purecvisor_vm_manager_create_vm_async(manager, TEST_VM_NAME,
        TEST_VM_VCPU, TEST_VM_MEM, 10,
        "/var/lib/libvirt/images/alpine.iso",
        NULL, 0,
        0, FALSE,
        0, FALSE,
        NULL,
        NULL, NULL,
        NULL, NULL, NULL,
        on_create_finished, NULL);

    g_main_loop_run(loop);

    g_object_unref(manager);
    g_main_loop_unref(loop);
    return 0;
}
