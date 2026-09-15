   
                       
                                           
  
                           
                                                   
                                                    
                                        
  
                                                
                                                   
                                                         
                                                      
                                                  
                                                  
  
                                                                       
            
                                            
  
                                                  
                                                   
                                        
  
                                        
                                                 
                                                               
                                          
                                           
                                            
                                         
                                                                   
  
                                   




  
               
                                 
                                                               
                                               
                                                                         
                                             
  
                         
                                                               
                          
                             
                                           
                                        
                             
  
                  
                                                         
                                                 


  
          
                                                      
                                         
                                  
  
         
                                    
                                              
                                           
                                              
                                                  
                                
                                                                       
   
#include "dpdk_manager.h"
#include "utils/pcv_spawn.h"
#include "utils/pcv_log.h"
#include "../../include/purecvisor/pcv_validate.h"
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define DPDK_LOG_DOM    "dpdk_manager"
#define DPDK_VHOST_RUNTIME_DIR "/run/libvirt/qemu"
#define DPDK_LEGACY_SOCK_DIR   "/var/run/purecvisor"
#define DPDK_HUGEPAGE   "/sys/kernel/mm/hugepages"
#define DPDK_PCI_SYSFS  "/sys/bus/pci"
#define DPDK_DEVBIND_TIMEOUT_SEC 5u
#define DPDK_OVS_PROBE_TIMEOUT_SEC 2u
#define DPDK_OVS_COMMAND_TIMEOUT_SEC 12u

#define DPDK_OWNER_KEY       "purecvisor-owner"
#define DPDK_BRIDGE_KEY      "purecvisor-bridge"
#define DPDK_MTU_KEY         "purecvisor-mtu"
#define DPDK_PCI_KEY         "purecvisor-pci"
#define DPDK_VM_KEY          "purecvisor-vm"
#define DPDK_OWNER_BRIDGE    "dpdk-bridge"
#define DPDK_OWNER_PHYSICAL  "dpdk-physical"
#define DPDK_OWNER_VHOST     "dpdk-vhost"

typedef gboolean (*PcvDpdkTestOvsRunner)(const gchar * const *argv,
                                         gchar **stdout_out,
                                         gchar **stderr_out,
                                         GError **error,
                                         gpointer user_data);

static struct {
    gboolean available;
    gboolean initialized;
    GMutex   mu;
    gchar   *test_pci_sysfs_root;
    gchar   *test_devbind_path;
    gint     test_vswitchd_state;
    PcvDpdkTestOvsRunner test_ovs_runner;
    gpointer test_ovs_runner_data;
    gint test_available_override;
} G = { .test_vswitchd_state = -1, .test_available_override = -1 };

                                                                     

   
                              
                                                
                     
                                  
                                
  
                                       
                                  
  
                    
   
static gboolean
_run_cmd(const gchar *cmd, gchar **out, GError **error)
{
                                                        
                                                                       
    const gchar *argv[] = {"/bin/sh", "-c", cmd, NULL};
    gchar *std_err = NULL;
    gboolean ok = pcv_spawn_sync(argv, out, &std_err, error);
    if (!ok)
        PCV_LOG_WARN(DPDK_LOG_DOM, "cmd failed: %s  stderr=%s", cmd,
                     std_err ? std_err : "(null)");
    g_free(std_err);
    return ok;
}

static gboolean
_dpdk_available(void)
{
    return G.test_available_override >= 0
        ? G.test_available_override == 1 : G.available;
}



static gboolean
_ovs_run_locked(const gchar * const *argv, gchar **stdout_out,
                gchar **stderr_out, GError **error)
{
    if (G.test_ovs_runner)
        return G.test_ovs_runner(argv, stdout_out, stderr_out, error,
                                 G.test_ovs_runner_data);
    return pcv_spawn_sync_timeout(argv, stdout_out, stderr_out,
                                  DPDK_OVS_COMMAND_TIMEOUT_SEC, error);
}

static gchar *
_ovs_clean_value(gchar *raw)
{
    if (!raw)
        return g_strdup("");
    gchar *trimmed = g_strstrip(raw);
    gsize len = strlen(trimmed);
    if (len == 2 && g_strcmp0(trimmed, "[]") == 0)
        return g_strdup("");
    if (len >= 2 && ((trimmed[0] == '"' && trimmed[len - 1] == '"') ||
                     (trimmed[0] == '\'' && trimmed[len - 1] == '\'')))
        return g_strndup(trimmed + 1, len - 2);
    return g_strdup(trimmed);
}

static gboolean
_ovs_find_name_locked(const gchar *table, const gchar *name,
                      gboolean *exists_out, GError **error)
{
    gchar *condition = g_strdup_printf("name=%s", name);
    const gchar *argv[] = {
        "ovs-vsctl", "--timeout=10", "--bare", "--columns=name",
        "find", table, condition, NULL
    };
    gchar *out = NULL;
    gboolean ok = _ovs_run_locked(argv, &out, NULL, error);
    if (ok) {
        gchar *clean = _ovs_clean_value(out);
        *exists_out = clean && *clean;
        g_free(clean);
    }
    g_free(out);
    g_free(condition);
    return ok;
}

static gboolean
_ovs_bridge_external_id_locked(const gchar *bridge, const gchar *key,
                               gchar **value_out, GError **error)
{
    const gchar *argv[] = {
        "ovs-vsctl", "--timeout=10", "br-get-external-id", bridge, key, NULL
    };
    gchar *out = NULL;
    if (!_ovs_run_locked(argv, &out, NULL, error)) {
        g_free(out);
        return FALSE;
    }
    *value_out = _ovs_clean_value(out);
    g_free(out);
    return TRUE;
}

static gboolean
_ovs_bridge_value_locked(const gchar *bridge, const gchar *column,
                         gchar **value_out, GError **error)
{
    const gchar *argv[] = {
        "ovs-vsctl", "--timeout=10", "get", "Bridge", bridge, column, NULL
    };
    gchar *out = NULL;
    if (!_ovs_run_locked(argv, &out, NULL, error)) {
        g_free(out);
        return FALSE;
    }
    *value_out = _ovs_clean_value(out);
    g_free(out);
    return TRUE;
}

static gboolean
_ovs_iface_value_locked(const gchar *iface, const gchar *column,
                        gchar **value_out, GError **error)
{
    const gchar *argv[] = {
        "ovs-vsctl", "--timeout=10", "get", "Interface", iface, column, NULL
    };
    gchar *out = NULL;
    if (!_ovs_run_locked(argv, &out, NULL, error)) {
        g_free(out);
        return FALSE;
    }
    *value_out = _ovs_clean_value(out);
    g_free(out);
    return TRUE;
}

static gboolean
_ovs_iface_bridge_locked(const gchar *iface, gchar **bridge_out, GError **error)
{
    const gchar *argv[] = {
        "ovs-vsctl", "--timeout=10", "iface-to-br", iface, NULL
    };
    gchar *out = NULL;
    if (!_ovs_run_locked(argv, &out, NULL, error)) {
        g_free(out);
        return FALSE;
    }
    *bridge_out = _ovs_clean_value(out);
    g_free(out);
    return TRUE;
}

static gboolean
_dpdk_expect_value(const gchar *actual, const gchar *expected,
                   const gchar *resource, const gchar *field, GError **error)
{
    if (g_strcmp0(actual, expected) == 0)
        return TRUE;
    g_set_error(error, g_quark_from_static_string("dpdk"), 3,
                "DPDK %s ownership conflict: %s is '%s' (expected '%s')",
                resource, field, actual ? actual : "", expected ? expected : "");
    return FALSE;
}

                           
                                                               
                                                                    
                                                               
        
  
                       
                                                  
                                         
static const gchar *
_dpdk_pci_sysfs_root(void)
{
    return G.test_pci_sysfs_root ? G.test_pci_sysfs_root : DPDK_PCI_SYSFS;
}

static gboolean
_dpdk_driver_is_userspace(const gchar *driver)
{
    return g_strcmp0(driver, "vfio-pci") == 0 ||
           g_strcmp0(driver, "igb_uio") == 0 ||
           g_strcmp0(driver, "uio_pci_generic") == 0;
}








static gboolean
_dpdk_vswitchd_holds_dpdk_devices(void)
{
    if (G.test_vswitchd_state >= 0)
        return G.test_vswitchd_state == 1;
    if (!G.available)
        return FALSE;

    const gchar *argv[] = {"ovs-appctl", "-t", "ovs-vswitchd", "version", NULL};
    GError *probe_error = NULL;
    gboolean running = pcv_spawn_sync_timeout(
        argv, NULL, NULL, DPDK_OVS_PROBE_TIMEOUT_SEC, &probe_error);
    if (probe_error) {
        PCV_LOG_DEBUG(DPDK_LOG_DOM, "ovs-vswitchd DPDK ownership probe: %s",
                      probe_error->message);
        g_clear_error(&probe_error);
    }
    return running;
}

                                                           
                                                     
static gchar *
_dpdk_current_driver(const gchar *pci_addr, gboolean *device_exists)
{
    gchar *device = g_build_filename(_dpdk_pci_sysfs_root(), "devices",
                                     pci_addr, NULL);
    *device_exists = g_file_test(device, G_FILE_TEST_IS_DIR);
    if (!*device_exists) {
        g_free(device);
        return NULL;
    }

    gchar *driver_link = g_build_filename(device, "driver", NULL);
    gchar *driver_target = g_file_read_link(driver_link, NULL);
    gchar *driver = driver_target ? g_path_get_basename(driver_target) : NULL;
    g_free(driver_target);
    g_free(driver_link);
    g_free(device);
    return driver;
}

                                                     
                                               
static gboolean
_dpdk_write_sysfs(const gchar *path, const gchar *value, GError **error)
{
    FILE *file = fopen(path, "w");
    if (!file) {
        g_set_error(error, g_quark_from_static_string("dpdk"), 3,
                    "sysfs open failed: %s (%s)", path, g_strerror(errno));
        return FALSE;
    }

    gsize length = strlen(value);
    gboolean ok = fwrite(value, 1, length, file) == length;
    if (fclose(file) != 0)
        ok = FALSE;
    if (!ok)
        g_set_error(error, g_quark_from_static_string("dpdk"), 3,
                    "sysfs write failed: %s", path);
    return ok;
}

                                                               
                                                         
                                                    
                          
static gboolean
_dpdk_clear_driver_override(const gchar *path, GError **error)
{
    if (!_dpdk_write_sysfs(path, "\n", error))
        return FALSE;

    gchar *contents = NULL;
    if (!g_file_get_contents(path, &contents, NULL, error))
        return FALSE;

    const gchar *trimmed = g_strstrip(contents);
    gboolean clear = *trimmed == '\0' || g_strcmp0(trimmed, "(null)") == 0;
    if (!clear)
        g_set_error(error, g_quark_from_static_string("dpdk"), 3,
                    "driver_override did not clear: %s", path);
    g_free(contents);
    return clear;
}

                                                          
                                                                 
static gboolean
_dpdk_run_unbind_tool(const gchar *pci_addr, GError **error)
{
    const gchar *override = G.test_devbind_path;
    if (override && *override) {
        const gchar *argv[] = {override, "--unbind", pci_addr, NULL};
        gchar *stderr_text = NULL;
        gboolean ok = pcv_spawn_sync_timeout(
            argv, NULL, &stderr_text, DPDK_DEVBIND_TIMEOUT_SEC, error);
        if (!ok)
            PCV_LOG_WARN(DPDK_LOG_DOM, "devbind unbind failed: %s",
                         stderr_text ? stderr_text : "(null)");
        g_free(stderr_text);
        return ok;
    }

    const gchar *primary[] = {"dpdk-devbind.py", "--unbind", pci_addr, NULL};
    gchar *stderr_text = NULL;
    GError *primary_error = NULL;
    if (pcv_spawn_sync_timeout(primary, NULL, &stderr_text,
                               DPDK_DEVBIND_TIMEOUT_SEC, &primary_error)) {
        g_free(stderr_text);
        return TRUE;
    }
    PCV_LOG_DEBUG(DPDK_LOG_DOM, "PATH devbind unavailable/failed: %s",
                  stderr_text ? stderr_text : "(null)");
    g_free(stderr_text);
    if (g_error_matches(primary_error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT)) {
        if (error)
            g_propagate_error(error, primary_error);
        else
            g_clear_error(&primary_error);
        return FALSE;
    }
    g_clear_error(&primary_error);

    const gchar *fallback[] = {"python3",
        "/usr/share/dpdk/usertools/dpdk-devbind.py", "--unbind", pci_addr, NULL};
    stderr_text = NULL;
    gboolean ok = pcv_spawn_sync_timeout(
        fallback, NULL, &stderr_text, DPDK_DEVBIND_TIMEOUT_SEC, error);
    if (!ok)
        PCV_LOG_WARN(DPDK_LOG_DOM, "fallback devbind unbind failed: %s",
                     stderr_text ? stderr_text : "(null)");
    g_free(stderr_text);
    return ok;
}

   
                                         
                                                         
  
                                                           
                           
                                                   
  
                                
   
static gboolean
_check_dpdk_init(void)
{
    gchar *out = NULL;
    if (!_run_cmd("ovs-vsctl get Open_vSwitch . other_config:dpdk-init 2>/dev/null",
                  &out, NULL)) {
        g_free(out);
        return FALSE;
    }
    gboolean yes = (out && (g_str_has_prefix(g_strstrip(out), "\"true") ||
                            g_strcmp0(g_strstrip(out), "true") == 0));
    g_free(out);
    return yes;
}

                                                                     

   
                                   
                                                    
  
                                                       
                                                       
                       
   
void
pcv_dpdk_init(void)
{
    g_mutex_init(&G.mu);
    G.available = _check_dpdk_init();
    G.initialized = TRUE;
    PCV_LOG_INFO(DPDK_LOG_DOM, "OVS-DPDK %s",
                 G.available ? "available" : "not available (dpdk-init != true)");
}

                                                   
void
pcv_dpdk_shutdown(void)
{
    g_mutex_lock(&G.mu);
    g_clear_pointer(&G.test_pci_sysfs_root, g_free);
    g_clear_pointer(&G.test_devbind_path, g_free);
    G.test_vswitchd_state = -1;
    G.test_ovs_runner = NULL;
    G.test_ovs_runner_data = NULL;
    G.test_available_override = -1;
    g_mutex_unlock(&G.mu);
    g_mutex_clear(&G.mu);
    G.initialized = FALSE;
}

   
                                                        
                                                                   
                                                                         
  
                           
                                                        
                                                         
                                                             
  
                       
                                                     
                                                          
                                                                
   
void
pcv_dpdk_test_set_unbind_paths(const gchar *pci_sysfs_root,
                               const gchar *devbind_path)
{
    gchar *new_root = pci_sysfs_root ? g_strdup(pci_sysfs_root) : NULL;
    gchar *new_devbind = devbind_path ? g_strdup(devbind_path) : NULL;

    g_mutex_lock(&G.mu);
    g_free(G.test_pci_sysfs_root);
    g_free(G.test_devbind_path);
    G.test_pci_sysfs_root = new_root;
    G.test_devbind_path = new_devbind;
    g_mutex_unlock(&G.mu);
}








void
pcv_dpdk_test_set_vswitchd_running(gint state)
{
    g_return_if_fail(state >= -1 && state <= 1);
    g_mutex_lock(&G.mu);
    G.test_vswitchd_state = state;
    g_mutex_unlock(&G.mu);
}










void
pcv_dpdk_test_set_ovs_runner(PcvDpdkTestOvsRunner runner, gpointer user_data,
                             gint available_override)
{
    g_return_if_fail(available_override >= -1 && available_override <= 1);
    g_mutex_lock(&G.mu);
    G.test_ovs_runner = runner;
    G.test_ovs_runner_data = user_data;
    G.test_available_override = available_override;
    g_mutex_unlock(&G.mu);
}

                                                                   
gboolean
pcv_dpdk_is_available(void)
{
    return _dpdk_available();
}

                                                                     

   
                                  
                                                       
  
                 
                             
                                 
                                                
                                  
  
                                             
                                            
  
                                                     
   
JsonObject *
pcv_dpdk_status(void)
{
    JsonObject *obj = json_object_new();
    json_object_set_boolean_member(obj, "available", G.available);

    if (!G.available) {
        json_object_set_int_member(obj, "vdev_count", 0);
        json_object_set_string_member(obj, "pmd_cpu_mask", "");
        json_object_set_string_member(obj, "socket_mem", "");
        return obj;
    }

                      
    gchar *pmd = NULL;
    if (_run_cmd("ovs-vsctl get Open_vSwitch . other_config:pmd-cpu-mask 2>/dev/null",
                 &pmd, NULL) && pmd) {
        g_strstrip(pmd);
                                     
        gchar *clean = g_strdup(pmd);
        g_strdelimit(clean, "\"", ' ');
        g_strstrip(clean);
        json_object_set_string_member(obj, "pmd_cpu_mask", clean);
        g_free(clean);
    } else {
        json_object_set_string_member(obj, "pmd_cpu_mask", "0x0");
    }
    g_free(pmd);

                       
    gchar *smem = NULL;
    if (_run_cmd("ovs-vsctl get Open_vSwitch . other_config:dpdk-socket-mem 2>/dev/null",
                 &smem, NULL) && smem) {
        g_strstrip(smem);
        gchar *clean = g_strdup(smem);
        g_strdelimit(clean, "\"", ' ');
        g_strstrip(clean);
        json_object_set_string_member(obj, "socket_mem", clean);
        g_free(clean);
    } else {
        json_object_set_string_member(obj, "socket_mem", "");
    }
    g_free(smem);

                         
    gchar *ports = NULL;
    gint vdev_count = 0;
    if (_run_cmd("ovs-vsctl --columns=name,type find interface type=dpdk 2>/dev/null",
                 &ports, NULL) && ports) {
                                   
        gchar **lines = g_strsplit(ports, "\n", -1);
        for (gint i = 0; lines[i]; i++)
            if (g_str_has_prefix(g_strstrip(lines[i]), "name"))
                vdev_count++;
        g_strfreev(lines);
    }
    g_free(ports);
    json_object_set_int_member(obj, "vdev_count", vdev_count);

    return obj;
}

   
                                             
                                                         
                                         
  
                                                  
                                                  
  
                 
                                                       
                                                    
                              
  
                                                              
   
JsonObject *
pcv_dpdk_hugepage_info(void)
{
    JsonObject *obj = json_object_new();

                       
    gchar *nr1g = NULL;
    gint64 total_1g = 0, free_1g = 0;
    if (g_file_get_contents(DPDK_HUGEPAGE "/hugepages-1048576kB/nr_hugepages",
                            &nr1g, NULL, NULL) && nr1g)
        total_1g = g_ascii_strtoll(g_strstrip(nr1g), NULL, 10);
    g_free(nr1g);

    gchar *fr1g = NULL;
    if (g_file_get_contents(DPDK_HUGEPAGE "/hugepages-1048576kB/free_hugepages",
                            &fr1g, NULL, NULL) && fr1g)
        free_1g = g_ascii_strtoll(g_strstrip(fr1g), NULL, 10);
    g_free(fr1g);

    json_object_set_int_member(obj, "hugepage_1g_total", total_1g);
    json_object_set_int_member(obj, "hugepage_1g_free", free_1g);
    json_object_set_int_member(obj, "hugepage_1g_size_mb", 1024);

                       
    gchar *nr2m = NULL;
    gint64 total_2m = 0, free_2m = 0;
    if (g_file_get_contents(DPDK_HUGEPAGE "/hugepages-2048kB/nr_hugepages",
                            &nr2m, NULL, NULL) && nr2m)
        total_2m = g_ascii_strtoll(g_strstrip(nr2m), NULL, 10);
    g_free(nr2m);

    gchar *fr2m = NULL;
    if (g_file_get_contents(DPDK_HUGEPAGE "/hugepages-2048kB/free_hugepages",
                            &fr2m, NULL, NULL) && fr2m)
        free_2m = g_ascii_strtoll(g_strstrip(fr2m), NULL, 10);
    g_free(fr2m);

    json_object_set_int_member(obj, "hugepage_2m_total", total_2m);
    json_object_set_int_member(obj, "hugepage_2m_free", free_2m);
    json_object_set_int_member(obj, "hugepage_2m_size_mb", 2);

                                                            
    gint64 total_mb = total_1g * 1024 + total_2m * 2;
    gint64 free_mb = free_1g * 1024 + free_2m * 2;
    json_object_set_int_member(obj, "total_mb", total_mb);
    json_object_set_int_member(obj, "free_mb", free_mb);

    return obj;
}

                                                                     

   
                                      
                                                   
                                                 
                                        
                                                     
                    
  
                                                       
                                                
  
                                          
                        
  
                    
   
gboolean
pcv_dpdk_bind(const gchar *pci_addr, const gchar *driver, GError **error)
{
    if (!pcv_validate_pci_addr(pci_addr)) {
        g_set_error(error, g_quark_from_static_string("dpdk"), 2,
                    "Invalid PCI address: %s", pci_addr ? pci_addr : "(null)");
        return FALSE;
    }

    const gchar *drv = driver ? driver : "vfio-pci";
                                                         
    if (!pcv_validate_bridge_name(drv)) {
        g_set_error(error, g_quark_from_static_string("dpdk"), 2,
                    "Invalid driver name: %s", drv);
        return FALSE;
    }
                                                             
                                                           
                                                
    if (!_dpdk_driver_is_userspace(drv)) {
        g_set_error(error, g_quark_from_static_string("dpdk"), 2,
                    "Unsupported DPDK driver: %s", drv);
        return FALSE;
    }
    if (!G.available) {
        g_set_error(error, g_quark_from_static_string("dpdk"), 1,
                    "OVS-DPDK not available (dpdk-init != true)");
        return FALSE;
    }
    gchar *cmd = g_strdup_printf(
        "dpdk-devbind.py --bind=%s %s 2>&1 || "
        "python3 /usr/share/dpdk/usertools/dpdk-devbind.py --bind=%s %s 2>&1",
        drv, pci_addr, drv, pci_addr);

    g_mutex_lock(&G.mu);
    gboolean ok = _run_cmd(cmd, NULL, error);
    g_mutex_unlock(&G.mu);

    g_free(cmd);
    if (ok)
        PCV_LOG_INFO(DPDK_LOG_DOM, "Bound %s to %s", pci_addr, drv);
    return ok;
}

   
                                                      
                                               
                                                      
                                              
                    
                    
  
                                                                     
                                                                 
                                        
  
                    
   
gboolean
pcv_dpdk_unbind(const gchar *pci_addr, GError **error)
{
    if (!pcv_validate_pci_addr(pci_addr)) {
        g_set_error(error, g_quark_from_static_string("dpdk"), 2,
                    "Invalid PCI address: %s", pci_addr ? pci_addr : "(null)");
        return FALSE;
    }

    gboolean device_exists = FALSE;
    gboolean ok = FALSE;
    gchar *driver = NULL;
    gchar *device = NULL;
    gchar *override_path = NULL;
    gchar *probe_path = NULL;

                                                          
                                                             
                                                          
    g_mutex_lock(&G.mu);
    driver = _dpdk_current_driver(pci_addr, &device_exists);

                                  
    if (!device_exists) {
        PCV_LOG_INFO(DPDK_LOG_DOM, "PCI %s is absent; unbind already complete",
                     pci_addr);
        ok = TRUE;
        goto out;
    }
    if (driver && !_dpdk_driver_is_userspace(driver)) {
                                                                
                                                                          
                                                       
        device = g_build_filename(_dpdk_pci_sysfs_root(), "devices",
                                  pci_addr, NULL);
        override_path = g_build_filename(device, "driver_override", NULL);
        if (!_dpdk_clear_driver_override(override_path, error))
            goto out;

        g_clear_pointer(&driver, g_free);
        driver = _dpdk_current_driver(pci_addr, &device_exists);
        ok = device_exists && driver && !_dpdk_driver_is_userspace(driver);
        if (!ok) {
            g_set_error(error, g_quark_from_static_string("dpdk"), 4,
                        "PCI %s lost its kernel driver while clearing driver_override",
                        pci_addr);
            goto out;
        }
        PCV_LOG_INFO(DPDK_LOG_DOM, "PCI %s already uses kernel driver %s",
                     pci_addr, driver);
        goto out;
    }




    if (driver && _dpdk_vswitchd_holds_dpdk_devices()) {
        g_set_error(error, g_quark_from_static_string("dpdk"), 5,
                    "PCI %s is still owned by OVS-DPDK; delete its DPDK port, "
                    "stop openvswitch-switch, retry unbind, then restart OVS",
                    pci_addr);
        goto out;
    }

                               
                                                               
                                                            
                                         
      
                           
                                                       
                                             
    if (driver && !_dpdk_run_unbind_tool(pci_addr, error)) {
        goto out;
    }
    g_clear_pointer(&driver, g_free);

    device = g_build_filename(_dpdk_pci_sysfs_root(), "devices", pci_addr, NULL);
    override_path = g_build_filename(device, "driver_override", NULL);
    probe_path = g_build_filename(_dpdk_pci_sysfs_root(), "drivers_probe", NULL);
    ok = _dpdk_clear_driver_override(override_path, error) &&
         _dpdk_write_sysfs(probe_path, pci_addr, error);
    if (!ok)
        goto out;

    driver = _dpdk_current_driver(pci_addr, &device_exists);
    ok = device_exists && driver && !_dpdk_driver_is_userspace(driver);
    if (!ok)
        g_set_error(error, g_quark_from_static_string("dpdk"), 4,
                    "PCI %s did not return to a kernel driver after unbind%s%s",
                    pci_addr, driver ? ": still bound to " : "",
                    driver ? driver : "");
    if (ok)
        PCV_LOG_INFO(DPDK_LOG_DOM, "Restored %s to kernel driver %s",
                     pci_addr, driver);

out:
    g_free(probe_path);
    g_free(override_path);
    g_free(device);
    g_free(driver);
    g_mutex_unlock(&G.mu);
    return ok;
}

   
                                            
                                                     
  
                                            
                                                     
                       
  
                                                   
                      
  
                                              
   
JsonArray *
pcv_dpdk_list(void)
{
    JsonArray *arr = json_array_new();

    if (!G.available)
        return arr;

    gchar *out = NULL;
    if (!_run_cmd(
            "dpdk-devbind.py --status-dev net 2>/dev/null || "
            "python3 /usr/share/dpdk/usertools/dpdk-devbind.py --status-dev net 2>/dev/null",
            &out, NULL) || !out) {
        g_free(out);
        return arr;
    }

      
                           
                                                   
                                                   
                                                       
       
    gboolean in_dpdk_section = FALSE;
    gchar **lines = g_strsplit(out, "\n", -1);
    for (gint i = 0; lines[i]; i++) {
        gchar *line = g_strstrip(lines[i]);
        if (g_str_has_prefix(line, "Network devices using DPDK")) {
            in_dpdk_section = TRUE;
            continue;
        }
                                                       
                                                  
                                             
        if (g_str_has_prefix(line, "Network devices using kernel") ||
            g_str_has_prefix(line, "No 'network'") ||
            (line[0] == '\0' && in_dpdk_section && json_array_get_length(arr) > 0)) {
            in_dpdk_section = FALSE;
            continue;
        }
        if (line[0] == '=' || line[0] == '\0')
            continue;

        if (in_dpdk_section && strlen(line) > 12) {
            JsonObject *dev = json_object_new();
                                                                
                                                       
            gchar pci[16] = {0};
            g_strlcpy(pci, line, MIN((gsize)13, strlen(line) + 1));
            g_strstrip(pci);
            json_object_set_string_member(dev, "pci_addr", pci);

                         
            gchar *drv_pos = strstr(line, "drv=");
            if (drv_pos) {
                drv_pos += 4;
                gchar *end = strpbrk(drv_pos, " \t");
                gchar *drv = end ? g_strndup(drv_pos, (gsize)(end - drv_pos))
                                 : g_strdup(drv_pos);
                json_object_set_string_member(dev, "driver", drv);
                g_free(drv);
            }

            json_object_set_string_member(dev, "status", "dpdk-bound");
            json_array_add_object_element(arr, dev);
        }
    }
    g_strfreev(lines);
    g_free(out);
    return arr;
}

                                                                     

   



                                                        
                
  



   
gboolean
pcv_dpdk_bridge_create(const gchar *name, const gchar *dpdk_port,
                       guint mtu, GError **error)
{
    if (!_dpdk_available()) {
        g_set_error(error, g_quark_from_static_string("dpdk"), 1,
                    "OVS-DPDK not available");
        return FALSE;
    }
    if (!pcv_validate_bridge_name(name)) {
        g_set_error(error, g_quark_from_static_string("dpdk"), 2,
                    "Invalid bridge name: 1-16 chars [a-zA-Z0-9_-]");
        return FALSE;
    }
    if (mtu < PCV_DPDK_MTU_MIN || mtu > PCV_DPDK_MTU_MAX) {
        g_set_error(error, g_quark_from_static_string("dpdk"), 2,
                    "Invalid DPDK MTU: must be between %u and %u",
                    PCV_DPDK_MTU_MIN, PCV_DPDK_MTU_MAX);
        return FALSE;
    }
    if (dpdk_port && *dpdk_port && !pcv_validate_pci_addr(dpdk_port)) {
        g_set_error(error, g_quark_from_static_string("dpdk"), 2,
                    "Invalid dpdk_port PCI address: %s", dpdk_port);
        return FALSE;
    }

    gboolean ok = FALSE, bridge_exists = FALSE, physical_exists = FALSE;
    gboolean bridge_created = FALSE, physical_created = FALSE;
    gchar *port_name = g_strdup_printf("dpdk-p-%s", name);
    gchar *mtu_arg = g_strdup_printf("mtu_request=%u", mtu);
    gchar *bridge_mtu = g_strdup_printf("external_ids:%s=%u", DPDK_MTU_KEY, mtu);
    gchar *bridge_owner = g_strdup_printf("external_ids:%s=%s",
                                           DPDK_OWNER_KEY, DPDK_OWNER_BRIDGE);
    gchar *physical_owner = g_strdup_printf("external_ids:%s=%s",
                                             DPDK_OWNER_KEY, DPDK_OWNER_PHYSICAL);
    gchar *physical_bridge = g_strdup_printf("external_ids:%s=%s", DPDK_BRIDGE_KEY, name);
    gchar *physical_pci = dpdk_port && *dpdk_port
        ? g_strdup_printf("external_ids:%s=%s", DPDK_PCI_KEY, dpdk_port) : NULL;
    gchar *devargs = dpdk_port && *dpdk_port
        ? g_strdup_printf("options:dpdk-devargs=%s", dpdk_port) : NULL;

    g_mutex_lock(&G.mu);


    if (!_ovs_find_name_locked("Bridge", name, &bridge_exists, error))
        goto out;
    if (bridge_exists) {
        gchar *owner = NULL;
        if (!_ovs_bridge_external_id_locked(name, DPDK_OWNER_KEY, &owner, error)) {
            g_free(owner);
            goto out;
        }
        gboolean owned = _dpdk_expect_value(owner, DPDK_OWNER_BRIDGE,
                                             name, "owner", error);
        g_free(owner);
        if (!owned)
            goto out;
    }

    if (!_ovs_find_name_locked("Interface", port_name, &physical_exists, error))
        goto out;
    if (physical_exists) {
        gchar *owner = NULL, *bridge = NULL, *pci = NULL, *parent = NULL;
        gboolean verified =
            _ovs_iface_value_locked(port_name, "external_ids:" DPDK_OWNER_KEY,
                                    &owner, error) &&
            _ovs_iface_value_locked(port_name, "external_ids:" DPDK_BRIDGE_KEY,
                                    &bridge, error) &&
            _ovs_iface_value_locked(port_name, "external_ids:" DPDK_PCI_KEY,
                                    &pci, error) &&
            _ovs_iface_bridge_locked(port_name, &parent, error);
        if (verified)
            verified = _dpdk_expect_value(owner, DPDK_OWNER_PHYSICAL, port_name,
                                          "owner", error) &&
                       _dpdk_expect_value(bridge, name, port_name, "bridge", error) &&
                       _dpdk_expect_value(parent, name, port_name, "parent bridge", error);
        if (verified && !pcv_validate_pci_addr(pci)) {
            g_set_error(error, g_quark_from_static_string("dpdk"), 3,
                        "DPDK physical port '%s' has invalid PCI ownership state",
                        port_name);
            verified = FALSE;
        }
        if (verified && dpdk_port && *dpdk_port)
            verified = _dpdk_expect_value(pci, dpdk_port, port_name, "PCI BDF", error);
        g_free(owner); g_free(bridge); g_free(pci); g_free(parent);
        if (!verified)
            goto out;
    }

    if (!bridge_exists) {
        const gchar *argv[] = {
            "ovs-vsctl", "--timeout=10", "--", "add-br", name,
            "--", "set", "Bridge", name, "datapath_type=netdev",
            bridge_owner, bridge_mtu, NULL
        };
        if (!_ovs_run_locked(argv, NULL, NULL, error))
            goto out;
        bridge_created = TRUE;
    } else {
        const gchar *argv[] = {
            "ovs-vsctl", "--timeout=10", "set", "Bridge", name,
            "datapath_type=netdev", bridge_owner, bridge_mtu, NULL
        };
        if (!_ovs_run_locked(argv, NULL, NULL, error))
            goto rollback;
    }

    if (dpdk_port && *dpdk_port) {
        if (!physical_exists) {
            const gchar *argv[] = {
                "ovs-vsctl", "--timeout=10", "--", "add-port", name, port_name,
                "--", "set", "Interface", port_name, "type=dpdk", devargs,
                mtu_arg, physical_owner, physical_bridge, physical_pci, NULL
            };
            if (!_ovs_run_locked(argv, NULL, NULL, error))
                goto rollback;
            physical_created = TRUE;
        } else {
            const gchar *argv[] = {
                "ovs-vsctl", "--timeout=10", "set", "Interface", port_name,
                "type=dpdk", devargs, mtu_arg, physical_owner, physical_bridge,
                physical_pci, NULL
            };
            if (!_ovs_run_locked(argv, NULL, NULL, error))
                goto rollback;
        }
    } else if (physical_exists) {
        const gchar *argv[] = {
            "ovs-vsctl", "--timeout=10", "set", "Interface", port_name,
            mtu_arg, physical_owner, physical_bridge, NULL
        };
        if (!_ovs_run_locked(argv, NULL, NULL, error))
            goto rollback;
    }

    if (physical_exists || physical_created) {
        gchar *actual_mtu = g_strdup_printf("mtu=%u", mtu);
        const gchar *argv[] = {
            "ovs-vsctl", "--timeout=10", "wait-until", "Interface", port_name,
            actual_mtu, NULL
        };
        gboolean mtu_ok = _ovs_run_locked(argv, NULL, NULL, error);
        g_free(actual_mtu);
        if (!mtu_ok)
            goto rollback;
    }

    ok = TRUE;
    goto out;

rollback:

    if (physical_created) {
        const gchar *del_port[] = {
            "ovs-vsctl", "--timeout=10", "--if-exists", "del-port",
            name, port_name, NULL
        };
        (void)_ovs_run_locked(del_port, NULL, NULL, NULL);
    }
    if (bridge_created) {
        const gchar *del_bridge[] = {
            "ovs-vsctl", "--timeout=10", "--if-exists", "del-br", name, NULL
        };
        (void)_ovs_run_locked(del_bridge, NULL, NULL, NULL);
    }

out:
    g_mutex_unlock(&G.mu);
    g_free(devargs); g_free(physical_pci); g_free(physical_bridge);
    g_free(physical_owner); g_free(bridge_owner); g_free(bridge_mtu);
    g_free(mtu_arg); g_free(port_name);
    if (ok)
        PCV_LOG_INFO(DPDK_LOG_DOM, "DPDK bridge '%s' reconciled (mtu=%u)", name, mtu);
    return ok;
}

   
                                                
                                                  
                    
                    
  
                                        
                         
  
                    
   
gboolean
pcv_dpdk_bridge_delete(const gchar *name, GError **error)
{
    if (!pcv_validate_bridge_name(name)) {
        g_set_error(error, g_quark_from_static_string("dpdk"), 2,
                    "Invalid bridge name: 1-16 chars [a-zA-Z0-9_-]");
        return FALSE;
    }

    g_mutex_lock(&G.mu);
    gboolean exists = FALSE;
    gboolean ok = _ovs_find_name_locked("Bridge", name, &exists, error);
    if (!ok || !exists)
        goto out;

    gchar *owner = NULL;
    ok = _ovs_bridge_external_id_locked(name, DPDK_OWNER_KEY, &owner, error);
    if (ok)
        ok = _dpdk_expect_value(owner, DPDK_OWNER_BRIDGE, name, "owner", error);
    g_free(owner);
    if (!ok)
        goto out;




    const gchar *list_ports[] = {
        "ovs-vsctl", "--timeout=10", "list-ports", name, NULL
    };
    gchar *ports_text = NULL;
    ok = _ovs_run_locked(list_ports, &ports_text, NULL, error);
    gchar *physical_name = g_strdup_printf("dpdk-p-%s", name);
    gchar **ports = ok ? g_strsplit(ports_text ? ports_text : "", "\n", -1) : NULL;
    for (guint i = 0; ok && ports && ports[i]; i++) {
        gchar *port = g_strstrip(ports[i]);
        if (!*port)
            continue;
        if (g_strcmp0(port, physical_name) != 0) {
            g_set_error(error, g_quark_from_static_string("dpdk"), 4,
                        "DPDK bridge '%s' still has a VM or foreign port: %s",
                        name, port);
            ok = FALSE;
            break;
        }

        gchar *port_owner = NULL, *port_bridge = NULL, *port_pci = NULL;
        gchar *parent = NULL;
        gboolean verified =
            _ovs_iface_value_locked(port, "external_ids:" DPDK_OWNER_KEY,
                                    &port_owner, error) &&
            _ovs_iface_value_locked(port, "external_ids:" DPDK_BRIDGE_KEY,
                                    &port_bridge, error) &&
            _ovs_iface_value_locked(port, "external_ids:" DPDK_PCI_KEY,
                                    &port_pci, error) &&
            _ovs_iface_bridge_locked(port, &parent, error);
        if (verified)
            verified = _dpdk_expect_value(port_owner, DPDK_OWNER_PHYSICAL,
                                          port, "owner", error) &&
                       _dpdk_expect_value(port_bridge, name,
                                          port, "bridge", error) &&
                       _dpdk_expect_value(parent, name,
                                          port, "parent bridge", error);
        if (verified && !pcv_validate_pci_addr(port_pci)) {
            g_set_error(error, g_quark_from_static_string("dpdk"), 3,
                        "DPDK physical port '%s' has invalid PCI ownership state",
                        port);
            verified = FALSE;
        }
        g_free(port_owner); g_free(port_bridge); g_free(port_pci); g_free(parent);
        ok = verified;
    }
    g_strfreev(ports);
    g_free(physical_name);
    g_free(ports_text);
    if (!ok)
        goto out;

    const gchar *del_bridge[] = {
        "ovs-vsctl", "--timeout=10", "--if-exists", "del-br", name, NULL
    };
    ok = _ovs_run_locked(del_bridge, NULL, NULL, error);

out:
    g_mutex_unlock(&G.mu);

    if (ok)
        PCV_LOG_INFO(DPDK_LOG_DOM, "DPDK bridge '%s' delete reconciled", name);
    return ok;
}

                                                                     

   
                                                       
                                                       
                  
  
                                             
                                             

  

   
gchar *
pcv_dpdk_vhost_socket_path(const gchar *vm_name)
{
    if (!pcv_validate_vm_name(vm_name))
        return NULL;
    gchar *path = g_strdup_printf(
        "%s/purecvisor-vhost-%s.sock", DPDK_VHOST_RUNTIME_DIR, vm_name);
    if (strlen(path) + 1 > sizeof(((struct sockaddr_un *)0)->sun_path)) {
        g_free(path);
        return NULL;
    }
    return path;
}

static gchar *
_dpdk_vhost_socket_path_for_endpoint(const gchar *vm_name,
                                     PcvDpdkVhostEndpoint endpoint)
{
    if (endpoint == PCV_DPDK_VHOST_ENDPOINT_CANONICAL)
        return pcv_dpdk_vhost_socket_path(vm_name);
    if (endpoint != PCV_DPDK_VHOST_ENDPOINT_ACTIVE_LEGACY ||
        !pcv_validate_vm_name(vm_name))
        return NULL;

    gchar *path = g_strdup_printf(
        "%s/vhost-%s.sock", DPDK_LEGACY_SOCK_DIR, vm_name);
    if (strlen(path) + 1 > sizeof(((struct sockaddr_un *)0)->sun_path)) {
        g_free(path);
        return NULL;
    }
    return path;
}



static gboolean
_dpdk_vhost_runtime_preflight_at(const gchar *runtime_dir, GError **error)
{
    struct stat st = {0};
    if (!runtime_dir || runtime_dir[0] != '/') {
        g_set_error(error, g_quark_from_static_string("dpdk"), 7,
                    "DPDK vhost runtime path is invalid");
        return FALSE;
    }
    if (lstat(runtime_dir, &st) != 0) {
        gint saved_errno = errno;
        g_set_error(error, g_quark_from_static_string("dpdk"), 7,
                    "DPDK vhost runtime '%s' is unavailable: %s",
                    runtime_dir, g_strerror(saved_errno));
        return FALSE;
    }
    if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
        g_set_error(error, g_quark_from_static_string("dpdk"), 7,
                    "DPDK vhost runtime '%s' is not a real directory",
                    runtime_dir);
        return FALSE;
    }
    return TRUE;
}

gboolean
pcv_dpdk_vhost_runtime_preflight(GError **error)
{
    return _dpdk_vhost_runtime_preflight_at(DPDK_VHOST_RUNTIME_DIR, error);
}


gboolean
pcv_dpdk_test_vhost_runtime_preflight_at(const gchar *runtime_dir,
                                         GError **error)
{
    return _dpdk_vhost_runtime_preflight_at(runtime_dir, error);
}

static gchar *
_dpdk_vhost_port_name(const gchar *vm_name)
{
    return g_strdup_printf("dpdk-v-%s", vm_name);
}

static gboolean
_dpdk_bridge_contract_locked(const gchar *bridge_name, guint *mtu_out,
                             GError **error)
{
    gboolean exists = FALSE;
    if (!_ovs_find_name_locked("Bridge", bridge_name, &exists, error))
        return FALSE;
    if (!exists) {
        g_set_error(error, g_quark_from_static_string("dpdk"), 5,
                    "Managed DPDK bridge '%s' does not exist", bridge_name);
        return FALSE;
    }
    gchar *owner = NULL, *datapath = NULL, *mtu_text = NULL;
    gboolean ok = _ovs_bridge_external_id_locked(
        bridge_name, DPDK_OWNER_KEY, &owner, error);
    if (ok)
        ok = _dpdk_expect_value(owner, DPDK_OWNER_BRIDGE,
                                bridge_name, "owner", error);
    if (ok)
        ok = _ovs_bridge_value_locked(bridge_name, "datapath_type",
                                      &datapath, error);
    if (ok)
        ok = _dpdk_expect_value(datapath, "netdev", bridge_name,
                                "datapath_type", error);
    if (ok)
        ok = _ovs_bridge_external_id_locked(
            bridge_name, DPDK_MTU_KEY, &mtu_text, error);
    if (ok) {
        gchar *end = NULL;
        guint64 parsed = g_ascii_strtoull(mtu_text, &end, 10);
        if (!mtu_text || !*mtu_text || end == mtu_text || *end != '\0' ||
            parsed < PCV_DPDK_MTU_MIN || parsed > PCV_DPDK_MTU_MAX) {
            g_set_error(error, g_quark_from_static_string("dpdk"), 6,
                        "Managed DPDK bridge '%s' has invalid MTU ownership state",
                        bridge_name);
            ok = FALSE;
        } else {
            *mtu_out = (guint)parsed;
        }
    }
    g_free(owner); g_free(datapath); g_free(mtu_text);
    return ok;
}

gboolean
pcv_dpdk_vm_port_ensure_endpoint(const gchar *bridge_name,
                                 const gchar *vm_name,
                                 PcvDpdkVhostEndpoint endpoint,
                                 GError **error)
{
    if (!_dpdk_available()) {
        g_set_error(error, g_quark_from_static_string("dpdk"), 1,
                    "OVS-DPDK not available");
        return FALSE;
    }
    if (!pcv_validate_bridge_name(bridge_name) || !pcv_validate_vm_name(vm_name)) {
        g_set_error(error, g_quark_from_static_string("dpdk"), 2,
                    "Invalid DPDK bridge or VM name");
        return FALSE;
    }
    if (endpoint != PCV_DPDK_VHOST_ENDPOINT_CANONICAL &&
        endpoint != PCV_DPDK_VHOST_ENDPOINT_ACTIVE_LEGACY) {
        g_set_error(error, g_quark_from_static_string("dpdk"), 2,
                    "Invalid DPDK vhost endpoint class");
        return FALSE;
    }
    if (endpoint == PCV_DPDK_VHOST_ENDPOINT_CANONICAL &&
        !pcv_dpdk_vhost_runtime_preflight(error))
        return FALSE;

    gboolean ok = FALSE, exists = FALSE, added_this_call = FALSE;
    guint mtu = 0;
    gchar *port_name = _dpdk_vhost_port_name(vm_name);
    gchar *socket_path = _dpdk_vhost_socket_path_for_endpoint(vm_name, endpoint);
    if (!socket_path) {
        g_set_error(error, g_quark_from_static_string("dpdk"), 2,
                    "Invalid or overlong DPDK vhost socket path");
        g_free(port_name);
        return FALSE;
    }
    gchar *socket_arg = g_strdup_printf("options:vhost-server-path=%s", socket_path);
    gchar *mtu_arg = NULL, *owner_arg = NULL, *bridge_arg = NULL, *vm_arg = NULL;

    g_mutex_lock(&G.mu);
    if (!_dpdk_bridge_contract_locked(bridge_name, &mtu, error))
        goto out;

    if (!_ovs_find_name_locked("Interface", port_name, &exists, error))
        goto out;
    if (exists) {
        gchar *owner = NULL, *bridge = NULL, *vm = NULL, *parent = NULL;
        gboolean verified =
            _ovs_iface_value_locked(port_name, "external_ids:" DPDK_OWNER_KEY,
                                    &owner, error) &&
            _ovs_iface_value_locked(port_name, "external_ids:" DPDK_BRIDGE_KEY,
                                    &bridge, error) &&
            _ovs_iface_value_locked(port_name, "external_ids:" DPDK_VM_KEY,
                                    &vm, error) &&
            _ovs_iface_bridge_locked(port_name, &parent, error);
        if (verified)
            verified = _dpdk_expect_value(owner, DPDK_OWNER_VHOST, port_name,
                                          "owner", error) &&
                       _dpdk_expect_value(bridge, bridge_name, port_name,
                                          "bridge", error) &&
                       _dpdk_expect_value(parent, bridge_name, port_name,
                                          "parent bridge", error) &&
                       _dpdk_expect_value(vm, vm_name, port_name, "VM", error);
        g_free(owner); g_free(bridge); g_free(vm); g_free(parent);
        if (!verified)
            goto out;
    }

    mtu_arg = g_strdup_printf("mtu_request=%u", mtu);
    owner_arg = g_strdup_printf("external_ids:%s=%s", DPDK_OWNER_KEY, DPDK_OWNER_VHOST);
    bridge_arg = g_strdup_printf("external_ids:%s=%s", DPDK_BRIDGE_KEY, bridge_name);
    vm_arg = g_strdup_printf("external_ids:%s=%s", DPDK_VM_KEY, vm_name);
    if (!exists) {
        const gchar *argv[] = {
            "ovs-vsctl", "--timeout=10", "--", "add-port", bridge_name, port_name,
            "--", "set", "Interface", port_name, "type=dpdkvhostuserclient",
            socket_arg, mtu_arg, owner_arg, bridge_arg, vm_arg, NULL
        };
        ok = _ovs_run_locked(argv, NULL, NULL, error);
        added_this_call = ok;
    } else {
        const gchar *argv[] = {
            "ovs-vsctl", "--timeout=10", "set", "Interface", port_name,
            "type=dpdkvhostuserclient", socket_arg, mtu_arg, owner_arg,
            bridge_arg, vm_arg, NULL
        };
        ok = _ovs_run_locked(argv, NULL, NULL, error);
    }
    if (ok) {
        gchar *actual_type = NULL, *actual_path = NULL;
        ok = _ovs_iface_value_locked(port_name, "type", &actual_type, error) &&
             _ovs_iface_value_locked(port_name, "options:vhost-server-path",
                                     &actual_path, error);
        if (ok)
            ok = _dpdk_expect_value(actual_type, "dpdkvhostuserclient",
                                    port_name, "type", error) &&
                 _dpdk_expect_value(actual_path, socket_path,
                                    port_name, "vhost endpoint", error);
        g_free(actual_type);
        g_free(actual_path);
    }
    if (!ok && added_this_call) {


        const gchar *rollback_argv[] = {
            "ovs-vsctl", "--timeout=10", "--if-exists", "del-port", port_name, NULL
        };
        GError *rollback_error = NULL;
        if (!_ovs_run_locked(rollback_argv, NULL, NULL, &rollback_error)) {
            PCV_LOG_WARN(DPDK_LOG_DOM,
                         "DPDK vhost '%s' postcondition rollback failed: %s",
                         port_name,
                         rollback_error ? rollback_error->message : "unknown");
        }
        g_clear_error(&rollback_error);
    }

out:
    g_mutex_unlock(&G.mu);
    g_free(vm_arg); g_free(bridge_arg); g_free(owner_arg); g_free(mtu_arg);
    g_free(socket_arg); g_free(socket_path); g_free(port_name);
    return ok;
}

gboolean
pcv_dpdk_vm_port_ensure(const gchar *bridge_name, const gchar *vm_name,
                        GError **error)
{
    return pcv_dpdk_vm_port_ensure_endpoint(
        bridge_name, vm_name, PCV_DPDK_VHOST_ENDPOINT_CANONICAL, error);
}

gboolean
pcv_dpdk_vm_port_delete(const gchar *vm_name, GError **error)
{
    if (!pcv_validate_vm_name(vm_name)) {
        g_set_error(error, g_quark_from_static_string("dpdk"), 2,
                    "Invalid DPDK VM name");
        return FALSE;
    }

    gboolean ok = FALSE, exists = FALSE;
    gchar *port_name = _dpdk_vhost_port_name(vm_name);
    gchar *bridge = NULL;
    g_mutex_lock(&G.mu);
    if (!_ovs_find_name_locked("Interface", port_name, &exists, error))
        goto out;
    if (!exists) {
        ok = TRUE;
        goto out;
    }

    gchar *owner = NULL, *owned_vm = NULL, *parent = NULL;
    gboolean verified =
        _ovs_iface_value_locked(port_name, "external_ids:" DPDK_OWNER_KEY,
                                &owner, error) &&
        _ovs_iface_value_locked(port_name, "external_ids:" DPDK_BRIDGE_KEY,
                                &bridge, error) &&
        _ovs_iface_value_locked(port_name, "external_ids:" DPDK_VM_KEY,
                                &owned_vm, error) &&
        _ovs_iface_bridge_locked(port_name, &parent, error);
    if (verified)
        verified = _dpdk_expect_value(owner, DPDK_OWNER_VHOST, port_name,
                                      "owner", error) &&
                   _dpdk_expect_value(owned_vm, vm_name, port_name, "VM", error) &&
                   _dpdk_expect_value(parent, bridge, port_name, "parent bridge", error);
    if (verified) {
        gchar *bridge_owner = NULL;
        verified = pcv_validate_bridge_name(bridge) &&
                   _ovs_bridge_external_id_locked(
                       bridge, DPDK_OWNER_KEY, &bridge_owner, error) &&
                   _dpdk_expect_value(bridge_owner, DPDK_OWNER_BRIDGE,
                                      bridge, "owner", error);
        if (!verified && error && !*error)
            g_set_error(error, g_quark_from_static_string("dpdk"), 3,
                        "Invalid DPDK bridge ownership for VM '%s'", vm_name);
        g_free(bridge_owner);
    }
    g_free(owner); g_free(owned_vm); g_free(parent);
    if (!verified)
        goto out;

    const gchar *argv[] = {
        "ovs-vsctl", "--timeout=10", "--if-exists", "del-port",
        bridge, port_name, NULL
    };
    ok = _ovs_run_locked(argv, NULL, NULL, error);

out:
    g_mutex_unlock(&G.mu);
    g_free(bridge); g_free(port_name);
    return ok;
}

                                                                

                                                                         
                                                    
                                                    
gboolean pcv_dpdk_route_is_default_dev(const gchar *netdev, const gchar *proc_base)
{
    if (!netdev) return FALSE;
    gchar *path = g_strdup_printf("%s/proc/net/route", proc_base ? proc_base : "");
    gchar *content = NULL;
    gboolean is_def = FALSE;
    if (g_file_get_contents(path, &content, NULL, NULL)) {
        gchar **lines = g_strsplit(content, "\n", -1);
        for (gint i = 1; lines[i]; i++) {                    
            gchar **f = g_strsplit_set(lines[i], "\t ", -1);
            gchar *iface = NULL, *dest = NULL; gint n = 0;
            for (gchar **c = f; *c; c++) {
                if (**c == '\0') continue;                 
                if (n == 0) iface = *c; else if (n == 1) dest = *c;
                n++;
            }
                                                                        
                                                     
            if (iface && dest && g_strcmp0(dest, "00000000") == 0 &&
                g_strcmp0(iface, netdev) == 0)
                is_def = TRUE;
            g_strfreev(f);
        }
        g_strfreev(lines);
        g_free(content);
    }
    g_free(path);
    return is_def;
}

                                                    
                                                    
                                     
static GList *_dpdk_pci_netdevs(const gchar *pci_addr)
{
    gchar *dir = g_strdup_printf("/sys/bus/pci/devices/%s/net", pci_addr);
    GList *out = NULL;
    GDir *d = g_dir_open(dir, 0, NULL);
    if (d) {
        const gchar *n;
        while ((n = g_dir_read_name(d))) out = g_list_prepend(out, g_strdup(n));
        g_dir_close(d);
    }
    g_free(dir);
    return out;
}

                                                                
                                                         
                                                    
                                                 
                   
                                                       
                                                     
static gboolean _dpdk_up_with_ipv4(const gchar *netdev, gboolean *out_ifaddr_err)
{
    if (out_ifaddr_err) *out_ifaddr_err = FALSE;
    struct ifaddrs *ifa = NULL;
    if (getifaddrs(&ifa) != 0) {
        if (out_ifaddr_err) *out_ifaddr_err = TRUE;
        return TRUE;                    
    }
    gboolean prot = FALSE;
    for (struct ifaddrs *p = ifa; p; p = p->ifa_next) {
        if (p->ifa_name && g_strcmp0(p->ifa_name, netdev) == 0 &&
            p->ifa_addr && p->ifa_addr->sa_family == AF_INET &&
            (p->ifa_flags & IFF_UP)) { prot = TRUE; break; }
    }
    freeifaddrs(ifa);
    return prot;
}

                                                                   
                                                        
                                                      
                                                           
                                                  
                                                   
                                                     
gboolean pcv_dpdk_nic_is_protected(const gchar *pci_addr, gchar **reason)
{
    if (reason) *reason = NULL;
    if (!pci_addr || !*pci_addr) return TRUE;                          
                                                                        
    if (!pcv_validate_pci_addr(pci_addr)) {
        if (reason) *reason = g_strdup("refusing to bind: invalid PCI address");
        return TRUE;
    }
    GList *devs = _dpdk_pci_netdevs(pci_addr);
    if (!devs) return FALSE;                                             
    gboolean prot = FALSE;
    for (GList *l = devs; l && !prot; l = l->next) {
        const gchar *nd = l->data;
        gboolean ifaddr_err = FALSE;
        if (_dpdk_up_with_ipv4(nd, &ifaddr_err)) {
                                                                  
                                                                   
                                                          
            if (reason) *reason = ifaddr_err
                ? g_strdup_printf(
                    "refusing to bind: interface enumeration failed for %s (fail-secure)", nd)
                : g_strdup_printf(
                    "refusing to bind: NIC %s is up with an IPv4 address", nd);
            prot = TRUE;
        } else if (pcv_dpdk_route_is_default_dev(nd, "")) {
            if (reason) *reason = g_strdup_printf(
                "refusing to bind: NIC %s carries the default route", nd);
            prot = TRUE;
        }
    }
    g_list_free_full(devs, g_free);
    return prot;
}
