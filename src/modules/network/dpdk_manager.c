   
                       
                                           
  
                           
                                                   
                                                    
                                        
  
                                                
                                                   
                                                         
                                                      
                                                  
                                                  
  
                                                                       
            
                                            
  
                                                  
                                                   
                                        
  
                                        
                                                 
                                                               
                                          
                                           
                                            
                                         
                                                                   
  
                                   
                                                                           
                                             
                                                  
                                            
                              
                                               
  
               
                                 
                                                               
                                               
                                                                         
                                             
  
                         
                                                               
                          
                             
                                           
                                        
                             
  
                  
                                                         
                                                 
                                                 
  
          
                                                      
                                         
                                  
  
         
                                    
                                              
                                           
                                              
                                                  
                                
                                                                       
   
#include "dpdk_manager.h"
#include "utils/pcv_spawn.h"
#include "utils/pcv_log.h"
#include "../../include/purecvisor/pcv_validate.h"
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <ifaddrs.h>
#include <net/if.h>

#define DPDK_LOG_DOM    "dpdk_manager"
#define DPDK_SOCK_DIR   "/var/run/purecvisor"
#define DPDK_HUGEPAGE   "/sys/kernel/mm/hugepages"
#define DPDK_PCI_SYSFS  "/sys/bus/pci"

static struct {
    gboolean available;
    gboolean initialized;
    GMutex   mu;
    gchar   *test_pci_sysfs_root;
    gchar   *test_devbind_path;
} G = {0};

                                                                     

   
                              
                                                
                     
                                  
                                
  
                                       
                                  
  
                    
   
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
        gboolean ok = pcv_spawn_sync(argv, NULL, &stderr_text, error);
        if (!ok)
            PCV_LOG_WARN(DPDK_LOG_DOM, "devbind unbind failed: %s",
                         stderr_text ? stderr_text : "(null)");
        g_free(stderr_text);
        return ok;
    }

    const gchar *primary[] = {"dpdk-devbind.py", "--unbind", pci_addr, NULL};
    gchar *stderr_text = NULL;
    GError *primary_error = NULL;
    if (pcv_spawn_sync(primary, NULL, &stderr_text, &primary_error)) {
        g_free(stderr_text);
        return TRUE;
    }
    PCV_LOG_DEBUG(DPDK_LOG_DOM, "PATH devbind unavailable/failed: %s",
                  stderr_text ? stderr_text : "(null)");
    g_free(stderr_text);
    g_clear_error(&primary_error);

    const gchar *fallback[] = {"python3",
        "/usr/share/dpdk/usertools/dpdk-devbind.py", "--unbind", pci_addr, NULL};
    stderr_text = NULL;
    gboolean ok = pcv_spawn_sync(fallback, NULL, &stderr_text, error);
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

                                                                   
gboolean
pcv_dpdk_is_available(void)
{
    return G.available;
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
pcv_dpdk_bridge_create(const gchar *name, const gchar *dpdk_port, GError **error)
{
    if (!G.available) {
        g_set_error(error, g_quark_from_static_string("dpdk"), 1,
                    "OVS-DPDK not available");
        return FALSE;
    }

    if (!pcv_validate_bridge_name(name)) {
        g_set_error(error, g_quark_from_static_string("dpdk"), 2,
                    "Invalid bridge name: 1-16 chars [a-zA-Z0-9_-]");
        return FALSE;
    }

                                                                 
    if (dpdk_port && *dpdk_port && !pcv_validate_pci_addr(dpdk_port)) {
        g_set_error(error, g_quark_from_static_string("dpdk"), 2,
                    "Invalid dpdk_port PCI address: %s", dpdk_port);
        return FALSE;
    }

    g_mutex_lock(&G.mu);

                                                        
    const gchar *br_argv[] = {
        "ovs-vsctl", "--may-exist", "add-br", name,
        "--", "set", "bridge", name, "datapath_type=netdev", NULL
    };
    gchar *serr = NULL;
    gboolean ok = pcv_spawn_sync(br_argv, NULL, &serr, error);
    if (!ok)
        PCV_LOG_WARN(DPDK_LOG_DOM, "ovs-vsctl add-br failed: %s",
                     serr ? serr : "(null)");
    g_free(serr);

    if (ok) {
        const gchar *up_argv[] = {"ip", "link", "set", name, "up", NULL};
        pcv_spawn_sync(up_argv, NULL, NULL, NULL);                    
    }

                         
    if (ok && dpdk_port && *dpdk_port) {
        gchar *port_name = g_strdup_printf("dpdk-p-%s", name);
                                                       
        gchar *devargs = g_strdup_printf("options:dpdk-devargs=%s", dpdk_port);
        const gchar *port_argv[] = {
            "ovs-vsctl", "--may-exist", "add-port", name, port_name,
            "--", "set", "interface", port_name, "type=dpdk", devargs, NULL
        };
        gchar *serr2 = NULL;
        ok = pcv_spawn_sync(port_argv, NULL, &serr2, error);
        if (!ok)
            PCV_LOG_WARN(DPDK_LOG_DOM, "ovs-vsctl add-port failed: %s",
                         serr2 ? serr2 : "(null)");
        g_free(serr2);
        g_free(devargs);
        g_free(port_name);
    }
    g_mutex_unlock(&G.mu);

    if (ok)
        PCV_LOG_INFO(DPDK_LOG_DOM, "DPDK bridge '%s' created", name);
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

               
    gchar *cmd = g_strdup_printf("ovs-vsctl --if-exists del-br %s", name);
    g_mutex_lock(&G.mu);
    gboolean ok = _run_cmd(cmd, NULL, error);
    g_mutex_unlock(&G.mu);
    g_free(cmd);

    if (ok)
        PCV_LOG_INFO(DPDK_LOG_DOM, "DPDK bridge '%s' deleted", name);
    return ok;
}

                                                                     

   
                                                       
                                                       
                  
  
                                             
                                             
                                               
  
                                                                   
   
gchar *
pcv_dpdk_vhost_socket_path(const gchar *vm_name)
{
    if (!vm_name)
        return NULL;
    return g_strdup_printf("%s/vhost-%s.sock", DPDK_SOCK_DIR, vm_name);
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
