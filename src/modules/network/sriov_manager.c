
#include "sriov_manager.h"
#include "utils/pcv_spawn.h"
#include "utils/pcv_log.h"
#include "../../include/purecvisor/pcv_validate.h"
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <glib/gstdio.h>

#define SRIOV_LOG_DOM    "sriov_manager"
#define SRIOV_SYSFS_NET  "/sys/class/net"
#define SRIOV_MAX_VFS    64

static struct {
    gboolean initialized;
    GMutex   mu;
} G = {0};

static gboolean
_run_shell(const gchar *cmd, gchar **out, GError **error)
{
    const gchar *argv[] = {"/bin/sh", "-c", cmd, NULL};
    gchar *std_err = NULL;
    gboolean ok = pcv_spawn_sync(argv, out, &std_err, error);
    if (!ok)
        PCV_LOG_WARN(SRIOV_LOG_DOM, "cmd(shell) failed: %s  stderr=%s", cmd,
                     std_err ? std_err : "(null)");
    g_free(std_err);
    return ok;
}

static const gchar *
_sriov_sysfs_net(void)
{
    const gchar *env = g_getenv("PCV_SRIOV_SYSFS_ROOT");
    return (env && *env) ? env : SRIOV_SYSFS_NET;
}

static gboolean
_write_sysfs(const gchar *path, const gchar *val, GError **error)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        g_set_error(error, g_quark_from_static_string("sriov"), 10,
                    "sysfs open failed: %s (%s)", path, g_strerror(errno));
        return FALSE;
    }
    size_t len = strlen(val);
    gboolean ok = (fwrite(val, 1, len, f) == len);
    if (fclose(f) != 0) ok = FALSE;
    if (!ok && error && !*error)
        g_set_error(error, g_quark_from_static_string("sriov"), 11,
                    "sysfs write failed: %s", path);
    return ok;
}

static gboolean
_pf_supports_sriov(const gchar *pf)
{
    gchar *path = g_strdup_printf("%s/%s/device/sriov_totalvfs", SRIOV_SYSFS_NET, pf);
    gchar *content = NULL;
    gboolean ok = g_file_get_contents(path, &content, NULL, NULL);
    g_free(path);
    if (ok && content) {
        gint64 total = g_ascii_strtoll(g_strstrip(content), NULL, 10);
        g_free(content);
        return total > 0;
    }
    g_free(content);
    return FALSE;
}

static gint
_pf_current_vfs(const gchar *pf)
{
    gchar *path = g_strdup_printf("%s/%s/device/sriov_numvfs", SRIOV_SYSFS_NET, pf);
    gchar *content = NULL;
    gint num = 0;
    if (g_file_get_contents(path, &content, NULL, NULL) && content)
        num = (gint)g_ascii_strtoll(g_strstrip(content), NULL, 10);
    g_free(content);
    g_free(path);
    return num;
}

static gint
_pf_max_vfs(const gchar *pf)
{
    gchar *path = g_strdup_printf("%s/%s/device/sriov_totalvfs", SRIOV_SYSFS_NET, pf);
    gchar *content = NULL;
    gint num = 0;
    if (g_file_get_contents(path, &content, NULL, NULL) && content)
        num = (gint)g_ascii_strtoll(g_strstrip(content), NULL, 10);
    g_free(content);
    g_free(path);
    return num;
}

void
pcv_sriov_init(void)
{
    g_mutex_init(&G.mu);
    G.initialized = TRUE;
    PCV_LOG_INFO(SRIOV_LOG_DOM, "SR-IOV manager initialized");
}

void
pcv_sriov_shutdown(void)
{
    g_mutex_clear(&G.mu);
    G.initialized = FALSE;
}

JsonObject *
pcv_sriov_status(void)
{
    JsonObject *obj = json_object_new();
    JsonArray *pfs = json_array_new();

    GDir *dir = g_dir_open(SRIOV_SYSFS_NET, 0, NULL);
    gboolean any_sriov = FALSE;
    if (dir) {
        const gchar *name;
        while ((name = g_dir_read_name(dir)) != NULL) {
            if (!_pf_supports_sriov(name))
                continue;
            any_sriov = TRUE;

            JsonObject *pf = json_object_new();
            json_object_set_string_member(pf, "name", name);
            json_object_set_int_member(pf, "max_vfs", _pf_max_vfs(name));
            json_object_set_int_member(pf, "current_vfs", _pf_current_vfs(name));

            gchar *pci_link = g_strdup_printf("%s/%s/device", SRIOV_SYSFS_NET, name);
            gchar *pci_real = g_file_read_link(pci_link, NULL);
            if (pci_real) {
                gchar *pci_base = g_path_get_basename(pci_real);
                json_object_set_string_member(pf, "pci_addr", pci_base);
                g_free(pci_base);
                g_free(pci_real);
            }
            g_free(pci_link);

            gchar *drv_link = g_strdup_printf("%s/%s/device/driver", SRIOV_SYSFS_NET, name);
            gchar *drv_real = g_file_read_link(drv_link, NULL);
            if (drv_real) {
                gchar *drv_name = g_path_get_basename(drv_real);
                json_object_set_string_member(pf, "driver", drv_name);
                g_free(drv_name);
                g_free(drv_real);
            }
            g_free(drv_link);

            gchar *iommu_path = g_strdup_printf("%s/%s/device/iommu_group",
                                                 SRIOV_SYSFS_NET, name);
            gboolean iommu_ok = g_file_test(iommu_path, G_FILE_TEST_EXISTS);
            json_object_set_boolean_member(pf, "iommu_enabled", iommu_ok);
            g_free(iommu_path);

            json_array_add_object_element(pfs, pf);
        }
        g_dir_close(dir);
    }

    json_object_set_boolean_member(obj, "available", any_sriov);
    json_object_set_array_member(obj, "physical_functions", pfs);
    return obj;
}

gboolean
pcv_sriov_enable(const gchar *pf, gint num_vfs, GError **error)
{
    if (!pf || strlen(pf) == 0) {
        g_set_error(error, g_quark_from_static_string("sriov"), 1,
                    "PF name required");
        return FALSE;
    }

    if (!pcv_validate_iface_name(pf)) {
        g_set_error(error, g_quark_from_static_string("sriov"), 2,
                    "Invalid PF name: %s", pf);
        return FALSE;
    }

    if (!_pf_supports_sriov(pf)) {
        g_set_error(error, g_quark_from_static_string("sriov"), 2,
                    "PF '%s' does not support SR-IOV", pf);
        return FALSE;
    }

    gint max_vfs = _pf_max_vfs(pf);
    if (num_vfs < 1 || num_vfs > max_vfs) {
        g_set_error(error, g_quark_from_static_string("sriov"), 3,
                    "num_vfs must be 1~%d for PF '%s'", max_vfs, pf);
        return FALSE;
    }

    gchar *path = g_strdup_printf("%s/%s/device/sriov_numvfs", SRIOV_SYSFS_NET, pf);
    gchar *val = g_strdup_printf("%d", num_vfs);

    g_mutex_lock(&G.mu);

    gint current = _pf_current_vfs(pf);
    if (current > 0)
        _write_sysfs(path, "0", NULL);

    gboolean ok = _write_sysfs(path, val, error);
    g_mutex_unlock(&G.mu);

    g_free(path);
    g_free(val);

    if (ok)
        PCV_LOG_INFO(SRIOV_LOG_DOM, "Enabled %d VFs on PF '%s'", num_vfs, pf);
    return ok;
}

/* PCV_SAFETY_CONTROL: sriov-disable — sriov_numvfs=0으로 PF의 모든 VF 실제 제거 (NET-3) */
gboolean
pcv_sriov_disable(const gchar *pf, GError **error)
{
    if (!pf || strlen(pf) == 0) {
        g_set_error(error, g_quark_from_static_string("sriov"), 1,
                    "PF name required");
        return FALSE;
    }

    if (!pcv_validate_iface_name(pf)) {
        g_set_error(error, g_quark_from_static_string("sriov"), 2,
                    "Invalid PF name: %s", pf);
        return FALSE;
    }

    gchar *path = g_strdup_printf("%s/%s/device/sriov_numvfs", _sriov_sysfs_net(), pf);

    g_mutex_lock(&G.mu);
    GError *werr = NULL;
    gboolean ok = _write_sysfs(path, "0", &werr);
    g_mutex_unlock(&G.mu);
    g_free(path);

    if (!ok) {
        if (g_error_matches(werr, g_quark_from_static_string("sriov"), 10)) {

            PCV_LOG_DEBUG(SRIOV_LOG_DOM, "sriov_numvfs absent, idempotent no-op: %s", werr->message);
            g_clear_error(&werr);
        } else {

            PCV_LOG_WARN(SRIOV_LOG_DOM, "sriov disable failed on PF '%s': %s", pf, werr ? werr->message : "(write)");
            g_propagate_error(error, werr);
            return FALSE;
        }
    }

    PCV_LOG_INFO(SRIOV_LOG_DOM, "Disabled VFs on PF '%s'", pf);
    return TRUE;
}

JsonArray *
pcv_sriov_list(const gchar *pf)
{
    JsonArray *arr = json_array_new();

    GDir *net_dir = g_dir_open(SRIOV_SYSFS_NET, 0, NULL);
    if (!net_dir)
        return arr;

    const gchar *iface;
    while ((iface = g_dir_read_name(net_dir)) != NULL) {
        if (pf && g_strcmp0(iface, pf) != 0)
            continue;
        if (!_pf_supports_sriov(iface))
            continue;

        gint num = _pf_current_vfs(iface);
        for (gint i = 0; i < num; i++) {
            JsonObject *vf = json_object_new();
            json_object_set_string_member(vf, "pf", iface);
            json_object_set_int_member(vf, "vf_index", i);

            gchar *vf_link = g_strdup_printf("%s/%s/device/virtfn%d",
                                              SRIOV_SYSFS_NET, iface, i);
            gchar *vf_real = g_file_read_link(vf_link, NULL);
            if (vf_real) {
                gchar *pci = g_path_get_basename(vf_real);
                json_object_set_string_member(vf, "pci_addr", pci);

                gchar *drv_link = g_strdup_printf("/sys/bus/pci/devices/%s/driver", pci);
                gchar *drv_real = g_file_read_link(drv_link, NULL);
                if (drv_real) {
                    gchar *drv = g_path_get_basename(drv_real);
                    json_object_set_string_member(vf, "driver", drv);
                    g_free(drv);
                    g_free(drv_real);
                }
                g_free(drv_link);
                g_free(pci);
                g_free(vf_real);
            }
            g_free(vf_link);

            gchar *mac_cmd = g_strdup_printf(
                "ip link show %s 2>/dev/null | grep 'vf %d' | "
                "sed -n 's/.*MAC \\([^ ]*\\).*/\\1/p'", iface, i);
            gchar *mac_out = NULL;
            if (_run_shell(mac_cmd, &mac_out, NULL) && mac_out) {
                g_strstrip(mac_out);
                if (strlen(mac_out) > 0)
                    json_object_set_string_member(vf, "mac", mac_out);
            }
            g_free(mac_out);
            g_free(mac_cmd);

            json_array_add_object_element(arr, vf);
        }
    }
    g_dir_close(net_dir);
    return arr;
}

gboolean
pcv_sriov_set(const gchar *pf, gint vf_index,
              const gchar *mac, gint vlan,
              gint spoofchk, GError **error)
{
    if (!pf || strlen(pf) == 0) {
        g_set_error(error, g_quark_from_static_string("sriov"), 1,
                    "PF name required");
        return FALSE;
    }

    if (!pcv_validate_iface_name(pf)) {
        g_set_error(error, g_quark_from_static_string("sriov"), 2,
                    "Invalid PF name: %s", pf);
        return FALSE;
    }
    if (mac && strlen(mac) > 0 && !pcv_validate_mac(mac)) {
        g_set_error(error, g_quark_from_static_string("sriov"), 5,
                    "Invalid MAC address: %s", mac);
        return FALSE;
    }

    gint current = _pf_current_vfs(pf);
    if (vf_index < 0 || vf_index >= current) {
        g_set_error(error, g_quark_from_static_string("sriov"), 3,
                    "VF index %d out of range (0~%d) for PF '%s'",
                    vf_index, current - 1, pf);
        return FALSE;
    }

    gchar *vf_str = g_strdup_printf("%d", vf_index);

    g_mutex_lock(&G.mu);
    gboolean ok = TRUE;

    if (mac && strlen(mac) > 0) {
        const gchar *argv[] = {"ip", "link", "set", pf, "vf", vf_str,
                               "mac", mac, NULL};
        gchar *serr = NULL;
        ok = pcv_spawn_sync(argv, NULL, &serr, error);
        if (!ok)
            PCV_LOG_WARN(SRIOV_LOG_DOM, "ip link set mac failed: %s",
                         serr ? serr : "(null)");
        g_free(serr);
    }

    if (ok && vlan >= 0) {
        gchar *vlan_str = g_strdup_printf("%d", vlan);
        const gchar *argv[] = {"ip", "link", "set", pf, "vf", vf_str,
                               "vlan", vlan_str, NULL};
        gchar *serr = NULL;
        ok = pcv_spawn_sync(argv, NULL, &serr, error);
        if (!ok)
            PCV_LOG_WARN(SRIOV_LOG_DOM, "ip link set vlan failed: %s",
                         serr ? serr : "(null)");
        g_free(serr);
        g_free(vlan_str);
    }

    if (ok && spoofchk >= 0) {
        const gchar *argv[] = {"ip", "link", "set", pf, "vf", vf_str,
                               "spoofchk", spoofchk ? "on" : "off", NULL};
        gchar *serr = NULL;
        ok = pcv_spawn_sync(argv, NULL, &serr, error);
        if (!ok)
            PCV_LOG_WARN(SRIOV_LOG_DOM, "ip link set spoofchk failed: %s",
                         serr ? serr : "(null)");
        g_free(serr);
    }

    g_mutex_unlock(&G.mu);
    g_free(vf_str);

    if (ok)
        PCV_LOG_INFO(SRIOV_LOG_DOM, "Set VF %d on PF '%s'", vf_index, pf);
    return ok;
}

gchar *
pcv_sriov_vf_pci_addr(const gchar *pf, gint vf_index)
{
    gchar *vf_link = g_strdup_printf("%s/%s/device/virtfn%d",
                                      SRIOV_SYSFS_NET, pf, vf_index);
    gchar *vf_real = g_file_read_link(vf_link, NULL);
    g_free(vf_link);
    if (!vf_real)
        return NULL;
    gchar *pci = g_path_get_basename(vf_real);
    g_free(vf_real);
    return pci;
}

gboolean
pcv_sriov_attach_vm(const gchar *vm_name, const gchar *pf,
                     gint vf_index, GError **error)
{
    if (!vm_name || !pf) {
        g_set_error(error, g_quark_from_static_string("sriov"), 1,
                    "vm_name and pf required");
        return FALSE;
    }

    if (!pcv_validate_vm_name(vm_name)) {
        g_set_error(error, g_quark_from_static_string("sriov"), 5,
                    "Invalid VM name: %s", vm_name);
        return FALSE;
    }
    if (!pcv_validate_iface_name(pf)) {
        g_set_error(error, g_quark_from_static_string("sriov"), 6,
                    "Invalid PF name: %s", pf);
        return FALSE;
    }

    gchar *pci = pcv_sriov_vf_pci_addr(pf, vf_index);
    if (!pci) {
        g_set_error(error, g_quark_from_static_string("sriov"), 4,
                    "Cannot resolve PCI address for VF %d on PF '%s'",
                    vf_index, pf);
        return FALSE;
    }

    g_mutex_lock(&G.mu);

    gchar *drv_link = g_strdup_printf("/sys/bus/pci/devices/%s/driver", pci);
    gchar *drv_real = g_file_read_link(drv_link, NULL);
    if (drv_real) {
        gchar *drv_name = g_path_get_basename(drv_real);
        if (g_strcmp0(drv_name, "vfio-pci") != 0) {
            gchar *cmd = g_strdup_printf(
                "echo %s > /sys/bus/pci/drivers/%s/unbind 2>/dev/null; "
                "echo vfio-pci > /sys/bus/pci/devices/%s/driver_override && "
                "echo %s > /sys/bus/pci/drivers/vfio-pci/bind",
                pci, drv_name, pci, pci);
            if (!_run_shell(cmd, NULL, error)) {
                g_free(cmd);
                g_free(drv_name);
                g_free(drv_real);
                g_free(drv_link);
                g_free(pci);
                g_mutex_unlock(&G.mu);
                return FALSE;
            }
            g_free(cmd);
        }
        g_free(drv_name);
        g_free(drv_real);
    }
    g_free(drv_link);

    guint domain = 0, bus = 0, slot = 0, func = 0;
    sscanf(pci, "%x:%x:%x.%x", &domain, &bus, &slot, &func);

    gchar *xml = g_strdup_printf(
        "<hostdev mode='subsystem' type='pci' managed='yes'>\n"
        "  <source>\n"
        "    <address domain='0x%04x' bus='0x%02x' slot='0x%02x' function='0x%x'/>\n"
        "  </source>\n"
        "</hostdev>", domain, bus, slot, func);

    gchar *xml_path = NULL;
    gint fd = g_file_open_tmp("pcv-sriov-attach-XXXXXX.xml", &xml_path, error);
    gboolean ok = (fd >= 0);
    if (ok) {
        g_close(fd, NULL);
        ok = g_file_set_contents(xml_path, xml, -1, error);
    }
    g_free(xml);

    if (ok) {

        const gchar *argv[] = {"virsh", "attach-device", vm_name, xml_path,
                               "--live", NULL};
        gchar *serr = NULL;
        ok = pcv_spawn_sync(argv, NULL, &serr, error);
        if (!ok)
            PCV_LOG_WARN(SRIOV_LOG_DOM, "virsh attach-device failed: %s",
                         serr ? serr : "(null)");
        g_free(serr);
    }

    if (xml_path) {
        g_unlink(xml_path);
        g_free(xml_path);
    }
    g_mutex_unlock(&G.mu);
    g_free(pci);

    if (ok)
        PCV_LOG_INFO(SRIOV_LOG_DOM, "Attached VF %d (PF %s) to VM '%s'",
                     vf_index, pf, vm_name);
    return ok;
}

gboolean
pcv_sriov_detach_vm(const gchar *vm_name, const gchar *pci_addr, GError **error)
{
    if (!vm_name || !pci_addr) {
        g_set_error(error, g_quark_from_static_string("sriov"), 1,
                    "vm_name and pci_addr required");
        return FALSE;
    }

    if (!pcv_validate_vm_name(vm_name)) {
        g_set_error(error, g_quark_from_static_string("sriov"), 5,
                    "Invalid VM name: %s", vm_name);
        return FALSE;
    }

    if (!pcv_validate_pci_addr(pci_addr)) {
        g_set_error(error, g_quark_from_static_string("sriov"), 2,
                    "Invalid PCI address: %s", pci_addr);
        return FALSE;
    }

    guint domain = 0, bus = 0, slot = 0, func = 0;
    sscanf(pci_addr, "%x:%x:%x.%x", &domain, &bus, &slot, &func);

    gchar *xml = g_strdup_printf(
        "<hostdev mode='subsystem' type='pci' managed='yes'>\n"
        "  <source>\n"
        "    <address domain='0x%04x' bus='0x%02x' slot='0x%02x' function='0x%x'/>\n"
        "  </source>\n"
        "</hostdev>", domain, bus, slot, func);

    gchar *xml_path = NULL;
    gint fd = g_file_open_tmp("pcv-sriov-detach-XXXXXX.xml", &xml_path, error);
    if (fd < 0) {
        g_free(xml);
        return FALSE;
    }
    g_close(fd, NULL);
    gboolean ok = g_file_set_contents(xml_path, xml, -1, error);
    g_free(xml);

    if (ok) {

        const gchar *argv[] = {"virsh", "detach-device", vm_name, xml_path,
                               "--live", NULL};
        gchar *serr = NULL;
        if (!pcv_spawn_sync(argv, NULL, &serr, NULL))
            PCV_LOG_DEBUG(SRIOV_LOG_DOM,
                          "virsh detach-device (ignored, idempotent): %s",
                          serr ? serr : "(null)");
        g_free(serr);
    }

    g_unlink(xml_path);
    g_free(xml_path);

    if (ok)
        PCV_LOG_INFO(SRIOV_LOG_DOM, "Detached PCI %s from VM '%s'", pci_addr, vm_name);
    return ok;
}
