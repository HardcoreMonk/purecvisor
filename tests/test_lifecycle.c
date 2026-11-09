                                                                               
                                                                              
                                                                                    
                                                                                  
                                                                              
                                                               
                                                                  
   
                         
                                                                    
  
                                                                               
                 
                                                                               
                                                 
                                     
  
                                              
                                         
                                         
                                          
                                                                    
  
                                                             
                                                               
                                                             
  
                                               
                                                             
                                                       
                                                                               
   
#include <gio/gio.h>
#include <glib.h>
#include <libvirt/libvirt.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "src/modules/dispatcher/vnc_port_xml.h"
#include "src/modules/virt/virt_conn_pool.h"
#include "src/modules/virt/vm_manager.h"
#include "src/utils/pcv_config.h"

                  
#define TEST_VM_VCPU 1
#define TEST_VM_MEM  1024         

static GMainLoop *loop;
static PureCVisorVmManager *manager;
static gchar *test_vm_name;
static gboolean vm_created;
static gboolean cleanup_in_progress;
static gint exit_status;
static guint delete_poll_count;

static gint last_destroy_rc;
static gchar *last_destroy_stderr;

static guint next_destroy_poll;
static guint destroy_backoff_polls;
static const gchar *test_uri;

static const gchar *test_pool;

static gboolean
destroy_test_dataset(const gchar *dataset)
{
    const gchar *settle[] = { "udevadm", "settle", "--timeout=5", NULL };
    const gchar *destroy[] = { "zfs", "destroy", "-r", dataset, NULL };
    g_autoptr(GError) error = NULL;
    gint wait_status = 0;

    (void)g_spawn_sync(NULL, (gchar **)settle, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
                       G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL, NULL, NULL);
    g_clear_pointer(&last_destroy_stderr, g_free);
    if (!g_spawn_sync(NULL, (gchar **)destroy, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL,
                      NULL, NULL, NULL, &last_destroy_stderr, &wait_status, &error)) {
        last_destroy_rc = -1;
        last_destroy_stderr = g_strdup(error->message);
        return FALSE;
    }
    g_strstrip(last_destroy_stderr);
    if (g_spawn_check_wait_status(wait_status, &error)) {
        last_destroy_rc = 0;
        return TRUE;
    }
    last_destroy_rc = error->domain == G_SPAWN_EXIT_ERROR ? error->code : -1;
    return FALSE;
}

                                                                           
                                                                         
static gboolean
poll_delete_status(gpointer user_data)
{
    (void)user_data;
    g_autofree gchar *dataset = g_strdup_printf("%s/%s", test_pool, test_vm_name);
    const gchar *argv[] = { "zfs", "list", "-H", "-o", "name", dataset, NULL };
    gint wait_status = 0;
    gboolean ran = g_spawn_sync(NULL, (gchar **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
                                G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL, &wait_status, NULL);
    if (ran && !g_spawn_check_wait_status(wait_status, NULL)) {
        vm_created = FALSE;
        if (cleanup_in_progress)
            g_print("[PASS] Failure cleanup removed dataset %s.\n", dataset);
        else {
            g_print("[PASS] 5. Test cleanup removed the definition and dataset %s.\n", dataset);
            g_print("\n[SUCCESS] All Lifecycle tests passed.\n");
        }
        g_main_loop_quit(loop);
        return G_SOURCE_REMOVE;
    }
    if (++delete_poll_count >= 400) {
        g_printerr("[FAIL] Delete storage cleanup did not remove dataset %s (zfs list %s; "
                   "last zfs destroy rc=%d stderr: %s).\n",
                   dataset, ran ? "still lists it" : "could not run", last_destroy_rc,
                   (last_destroy_stderr && *last_destroy_stderr) ? last_destroy_stderr : "(empty)");
        exit_status = 1;
        g_main_loop_quit(loop);
        return G_SOURCE_REMOVE;
    }
    if (delete_poll_count >= next_destroy_poll && last_destroy_rc > 0 && last_destroy_stderr &&
        strstr(last_destroy_stderr, "dataset is busy")) {
        (void)destroy_test_dataset(dataset);
        destroy_backoff_polls = MIN(destroy_backoff_polls * 2, 16);
        next_destroy_poll = delete_poll_count + destroy_backoff_polls;
    }
    return G_SOURCE_CONTINUE;
}

static void
request_cleanup(void)
{
    g_autofree gchar *dataset = g_strdup_printf("%s/%s", test_pool, test_vm_name);
    const gchar *stop_domain[] = { "virsh", "-c", test_uri, "destroy", test_vm_name, NULL };
    const gchar *undefine[] = { "virsh", "-c", test_uri, "undefine", test_vm_name, "--nvram", NULL };
    (void)g_spawn_sync(NULL, (gchar **)stop_domain, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
                       G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL, NULL, NULL);
    (void)g_spawn_sync(NULL, (gchar **)undefine, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
                       G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL, NULL, NULL);
    delete_poll_count = 0;
    destroy_backoff_polls = 2;
    next_destroy_poll = destroy_backoff_polls;
    (void)destroy_test_dataset(dataset);
    g_timeout_add(100, poll_delete_status, NULL);
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
        request_cleanup();
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

                    
    g_print("[INFO] Requesting Cleanup...\n");
    request_cleanup();
}

                                                                             
                                        
                                                                                
static void
verify_running_with_vnc(void)
{
    g_autofree gchar *problem = NULL;
    gint64 port = 0;
    virConnectPtr vconn = virt_conn_pool_acquire();
    virDomainPtr dom = vconn ? virDomainLookupByName(vconn, test_vm_name) : NULL;
    
    if (!vconn) {
        problem = g_strdup("no pooled libvirt connection");
    } else if (!dom) {
        problem = g_strdup_printf("domain %s not found", test_vm_name);
    } else if (virDomainIsActive(dom) != 1) {
        problem = g_strdup("Test VM is not running");
    } else {
        g_autofree gchar *xml = virDomainGetXMLDesc(dom, 0);
        g_autofree gchar *port_str = NULL;
        g_autoptr(GError) xml_err = NULL;
        if (!xml)
            problem = g_strdup("virDomainGetXMLDesc failed");
        else if (!(port_str = pcv_vnc_port_from_domain_xml(xml, &xml_err)))
            problem = g_strdup_printf("VNC port unavailable: %s", xml_err ? xml_err->message : "unexpected result");
        else if ((port = g_ascii_strtoll(port_str, NULL, 10)) < 5900)
            problem = g_strdup_printf("VNC port %s is below 5900", port_str);
    }
    if (dom)
        virDomainFree(dom);
    virt_conn_pool_release(vconn);

    if (problem) {
        fail_and_cleanup("List", g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, problem));
        return;
    }
    g_print("[PASS] 3. VM is running with VNC port %" G_GINT64_FORMAT ".\n", port);

                          
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

                                           
    g_print("[INFO] Verifying running state and VNC port...\n");
    verify_running_with_vnc();
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
    test_uri = uri;
    test_pool = (storage_pool && *storage_pool) ? storage_pool : pcv_config_get_zvol_pool();

    {
        const gchar *pool_check[] = { "zfs", "list", "-H", "-o", "name", test_pool, NULL };
        gint pool_status = 0;
        gboolean pool_ran = g_spawn_sync(NULL, (gchar **)pool_check, NULL,
                                         G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
                                         G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL,
                                         &pool_status, NULL);
        if (!pool_ran || !g_spawn_check_wait_status(pool_status, NULL)) {
            g_printerr("[FATAL] Storage pool dataset '%s' not found (zfs list failed)\n", test_pool);
            return 2;
        }
    }

    g_print("=== PureCVisor Core Logic Verification ===\n");

                                  
    gvir_init_object(NULL, NULL);
    virt_conn_pool_init(4);

                                     
    manager = purecvisor_vm_manager_new();
    if (!manager) {
        g_printerr("[FATAL] Failed to create VmManager.\n");
        virt_conn_pool_shutdown();
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
        "zvol", test_pool, NULL,
        NULL, NULL,                                         
        NULL, NULL, "lifecycle-test",
        "bridge", NULL,                                       
        FALSE, NULL,                                           
        NULL,                                        
        on_create_finished, NULL);

    g_main_loop_run(loop);

    g_object_unref(manager);
    g_main_loop_unref(loop);
    g_free(test_vm_name);
    g_clear_pointer(&last_destroy_stderr, g_free);
    virt_conn_pool_shutdown();
    return exit_status;
}
