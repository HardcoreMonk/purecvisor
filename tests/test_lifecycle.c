                                                                               
                                                                              
                                                                                    
                                                                                  
                                                                              
                                                               
                                                                  
   
                         
                                                                    
  
                                                                               
                 
                                                                               
                                                 
                                     
  
                                              
                                         
                                         
                                          
                                                                    
  
                                                             
                                                               
                                                             
  
                                               
                                                             
                                                       
                                                                               
   
#include <glib.h>
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <unistd.h>

#include "src/modules/virt/vm_manager.h"

                  
#define TEST_VM_VCPU 1
#define TEST_VM_MEM  1024         

static GMainLoop *loop;
static PureCVisorVmManager *manager;
static gchar *test_vm_name;
static gboolean vm_created;
static gboolean cleanup_in_progress;
static gint exit_status;
static guint delete_poll_count;

static void on_delete_finished(GObject *source, GAsyncResult *res, gpointer user_data);

                                                                           
                                                                         
static gboolean
poll_delete_status(gpointer user_data)
{
    (void)user_data;
    const gchar *status = pcv_vm_delete_status_get(test_vm_name);
    if (g_strcmp0(status, "done") == 0) {
        vm_created = FALSE;
        if (cleanup_in_progress)
            g_print("[PASS] Failure cleanup reached terminal delete status=done.\n");
        else {
            g_print("[PASS] 5. VM definition and storage cleanup reached status=done.\n");
            g_print("\n[SUCCESS] All Lifecycle tests passed.\n");
        }
        g_main_loop_quit(loop);
        return G_SOURCE_REMOVE;
    }
    if (g_strcmp0(status, "failed") == 0) {
        g_printerr("[FAIL] Delete storage cleanup reached status=failed.\n");
        exit_status = 1;
        g_main_loop_quit(loop);
        return G_SOURCE_REMOVE;
    }
    if (++delete_poll_count >= 400) {
        g_printerr("[FAIL] Delete storage cleanup did not reach a terminal status (last=%s).\n",
                   status ? status : "unknown");
        exit_status = 1;
        g_main_loop_quit(loop);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

                                                        
                                                          
static void
fail_and_cleanup(const gchar *stage, GError *error)
{
    g_printerr("[FAIL] %s: %s\n", stage,
               error ? error->message : "unexpected result");
    g_clear_error(&error);
    exit_status = 1;

    if (vm_created && !cleanup_in_progress) {
        cleanup_in_progress = TRUE;
        g_printerr("[CLEANUP] Deleting %s after failure...\n", test_vm_name);
        purecvisor_vm_manager_delete_vm_async(
            manager, test_vm_name, on_delete_finished, NULL);
        return;
    }
    g_main_loop_quit(loop);
}

                                                                             
                                  
                                                                                
static void
on_delete_finished(GObject *source, GAsyncResult *res, gpointer user_data) {
    GError *error = NULL;
    if (!purecvisor_vm_manager_delete_vm_finish(PURECVISOR_VM_MANAGER(source), res, &error)) {
        g_printerr("[FAIL] Delete/cleanup failed: %s\n",
                   error ? error->message : "unexpected result");
        g_clear_error(&error);
        exit_status = 1;
    } else {
        delete_poll_count = 0;
        g_print("[INFO] VM definition removed; waiting for storage cleanup status...\n");
        g_timeout_add(100, poll_delete_status, NULL);
        return;
    }
    g_main_loop_quit(loop);
}

                                                                             
                                          
                                                                                
static void
on_stop_finished(GObject *source, GAsyncResult *res, gpointer user_data) {
    GError *error = NULL;
    if (!purecvisor_vm_manager_stop_vm_finish(PURECVISOR_VM_MANAGER(source), res, &error)) {
        fail_and_cleanup("Stop failed", error);
        return;
    }
    g_print("[PASS] 4. VM Stopped successfully.\n");

                    
    g_print("[INFO] Requesting Delete...\n");
    purecvisor_vm_manager_delete_vm_async(
        manager, test_vm_name, on_delete_finished, NULL);
}

                                                                             
                                        
                                                                                
static void
on_list_finished(GObject *source, GAsyncResult *res, gpointer user_data) {
    GError *error = NULL;
    JsonNode *root = purecvisor_vm_manager_list_vms_finish(PURECVISOR_VM_MANAGER(source), res, &error);
    
    if (!root) {
        fail_and_cleanup("List failed", error);
        return;
    }

                          
    JsonArray *array = json_node_get_array(root);
    guint len = json_array_get_length(array);
    gboolean found = FALSE;
    gboolean running = FALSE;
    gboolean vnc_valid = FALSE;

    g_print("[INFO] Current VMs:\n");
    for (guint i = 0; i < len; i++) {
        JsonObject *obj = json_array_get_object_element(array, i);
        const gchar *name = json_object_get_string_member(obj, "name");
        const gchar *state = json_object_get_string_member(obj, "state");
        gint64 vnc = json_object_get_int_member(obj, "vnc_port");

        g_print(" - VM: %s | State: %s | VNC: %" G_GINT64_FORMAT "\n",
                name, state, vnc);

        if (g_strcmp0(name, test_vm_name) == 0) {
            found = TRUE;
            if (g_strcmp0(state, "running") != 0) {
                g_printerr("[FAIL] Test VM is not running!\n");
            } else {
                running = TRUE;
            }
            if (running && vnc <= 0) {
                g_printerr("[FAIL] Test VM is running but VNC port is invalid.\n");
            } else if (running) {
                vnc_valid = TRUE;
                g_print("[PASS] 3. VM List verified (VNC Port: %" G_GINT64_FORMAT ").\n",
                        vnc);
            }
        }
    }
    json_node_unref(root);

    if (!found) {
        fail_and_cleanup("Temporary VM not found in list", NULL);
        return;
    }
    if (!running) {
        fail_and_cleanup("Temporary VM list state is not running", NULL);
        return;
    }
    if (!vnc_valid) {
        fail_and_cleanup("Temporary VM list VNC port is invalid", NULL);
        return;
    }

                          
    g_print("[INFO] Requesting Stop (Force)...\n");
    purecvisor_vm_manager_stop_vm_async(
        manager, test_vm_name, on_stop_finished, NULL);
}

                                                                             
                                         
                                                                                
static void
on_start_finished(GObject *source, GAsyncResult *res, gpointer user_data) {
    GError *error = NULL;
    if (!purecvisor_vm_manager_start_vm_finish(PURECVISOR_VM_MANAGER(source), res, &error)) {
        fail_and_cleanup("Start failed", error);
        return;
    }
    g_print("[PASS] 2. VM Started successfully.\n");

                                           
                                                            
    g_print("[INFO] Requesting List...\n");
    purecvisor_vm_manager_list_vms_async(manager, on_list_finished, NULL);
}

                                                                             
                                           
                                                                                
static void
on_create_finished(GObject *source, GAsyncResult *res, gpointer user_data) {
    GError *error = NULL;
                                                                     
    if (!purecvisor_vm_manager_create_vm_finish(PURECVISOR_VM_MANAGER(source), res, &error)) {
        fail_and_cleanup("Create failed", error);
        return;
    }
    vm_created = TRUE;
    g_print("[PASS] 1. VM Created successfully (ZFS + XML).\n");

                   
    g_print("[INFO] Requesting Start...\n");
    purecvisor_vm_manager_start_vm_async(
        manager, test_vm_name, on_start_finished, NULL);
}

                                                                             
             
                                                                                
int main(int argc, char *argv[]) {
    const gchar *enabled = g_getenv("PCV_RUN_DESTRUCTIVE_LIFECYCLE_TEST");
    const gchar *iso_path = g_getenv("PCV_LIFECYCLE_TEST_ISO");
    const gchar *uri = g_getenv("PCV_LIBVIRT_URI");
    const gchar *storage_pool = g_getenv("PCV_LIFECYCLE_TEST_STORAGE_POOL");

    if (g_strcmp0(enabled, "1") != 0) {
        g_printerr("[SKIP] Set PCV_RUN_DESTRUCTIVE_LIFECYCLE_TEST=1 only on a disposable host.\n");
        return 77;
    }
    if (!iso_path || !*iso_path || !g_file_test(iso_path, G_FILE_TEST_IS_REGULAR)
        || access(iso_path, R_OK) != 0) {
        g_printerr("[FATAL] PCV_LIFECYCLE_TEST_ISO must name a readable ISO file.\n");
        return 2;
    }
    if (!uri || !*uri)
        uri = "qemu:///system";

    g_print("=== PureCVisor Core Logic Verification ===\n");

                                  
    gvir_init_object(NULL, NULL);

                                     
    GError *error = NULL;
    GVirConnection *conn = gvir_connection_new(uri);
    if (!conn || !gvir_connection_open(conn, NULL, &error)) {
        g_printerr("[FATAL] Failed to open %s: %s\n", uri,
                   error ? error->message : "unexpected result");
        g_clear_error(&error);
        g_clear_object(&conn);
        return 1;
    }
    manager = purecvisor_vm_manager_new(conn);
    g_object_unref(conn);                             
    if (!manager) {
        g_printerr("[FATAL] Failed to create VmManager.\n");
        return 1;
    }

    loop = g_main_loop_new(NULL, FALSE);

                                                         
    test_vm_name = g_strdup_printf("pcv-lifecycle-%ld", (long)getpid());
    g_print("[INFO] Requesting Create (Name: %s)...\n", test_vm_name);

                                                          
    purecvisor_vm_manager_create_vm_async(manager, test_vm_name,
        TEST_VM_VCPU, TEST_VM_MEM, 10,
        iso_path,
        "none", 0,                               
        0, FALSE,                       
        0, FALSE,                                                 
        "zvol", storage_pool, NULL,                                            
        NULL, NULL,                                         
        NULL, "lifecycle-test",                            
        "bridge", NULL,                                       
        FALSE, NULL,                                           
        NULL,                                        
        on_create_finished, NULL);

    g_main_loop_run(loop);

    g_object_unref(manager);
    g_main_loop_unref(loop);
    g_free(test_vm_name);
    return exit_status;
}
