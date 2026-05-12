

































#include "gpu_manager.h"
#include "utils/pcv_spawn.h"
#include "utils/pcv_log.h"
#include <string.h>
#include <stdio.h>
#include <glib/gstdio.h>

#define GPU_LOG_DOM "gpu_manager"













static gboolean
_run(const gchar *cmd, gchar **out, GError **error)
{
    gchar **parsed = NULL;
    GError *pe = NULL;
    if (!g_shell_parse_argv(cmd, NULL, &parsed, &pe)) {
        if (pe) { if (error) g_propagate_error(error, pe); else g_error_free(pe); }
        return FALSE;
    }
    gchar *se = NULL;
    gboolean ok = pcv_spawn_sync((const gchar * const *)parsed, out, &se, error);
    if (!ok) PCV_LOG_WARN(GPU_LOG_DOM, "cmd failed: %s  err=%s", cmd, se ? se : "");
    g_free(se);
    g_strfreev(parsed);
    return ok;
}

static gboolean
_run_shell(const gchar *cmd, gchar **out, GError **error)
{
    const gchar *argv[] = {"/bin/sh", "-c", cmd, NULL};
    gchar *se = NULL;
    gboolean ok = pcv_spawn_sync(argv, out, &se, error);
    if (!ok) PCV_LOG_WARN(GPU_LOG_DOM, "cmd failed: %s  err=%s", cmd, se ? se : "");
    g_free(se);
    return ok;
}


void pcv_gpu_init(void)  { PCV_LOG_INFO(GPU_LOG_DOM, "GPU manager initialized"); }

void pcv_gpu_shutdown(void) {}










JsonArray *pcv_gpu_list(void)
{
    JsonArray *arr = json_array_new();

    gchar *out = NULL;
    if (_run_shell("lspci -nn 2>/dev/null | grep -iE 'VGA|3D|Display'", &out, NULL) && out) {
        gchar **lines = g_strsplit(out, "\n", -1);
        for (gint i = 0; lines[i] && lines[i][0]; i++) {
            JsonObject *gpu = json_object_new();

            gchar pci[16] = {0};
            g_strlcpy(pci, lines[i], 8);
            json_object_set_string_member(gpu, "pci_addr", g_strstrip(pci));
            json_object_set_string_member(gpu, "description", lines[i]);

            gchar *sriov_cmd = g_strdup_printf(
                "test -f /sys/bus/pci/devices/0000:%s/sriov_totalvfs && cat /sys/bus/pci/devices/0000:%s/sriov_totalvfs 2>/dev/null || echo 0", pci, pci);
            gchar *vfs = NULL;
            if (_run(sriov_cmd, &vfs, NULL) && vfs)
                json_object_set_int_member(gpu, "sriov_vfs", g_ascii_strtoll(g_strstrip(vfs), NULL, 10));
            g_free(vfs); g_free(sriov_cmd);

            gchar *mdev_cmd = g_strdup_printf("test -d /sys/bus/pci/devices/0000:%s/mdev_supported_types && echo yes || echo no", pci);
            gchar *mdev = NULL;
            if (_run(mdev_cmd, &mdev, NULL) && mdev)
                json_object_set_boolean_member(gpu, "mdev_supported", g_strcmp0(g_strstrip(mdev), "yes") == 0);
            g_free(mdev); g_free(mdev_cmd);
            json_array_add_object_element(arr, gpu);
        }
        g_strfreev(lines);
    }
    g_free(out);
    return arr;
}









JsonObject *pcv_gpu_info(const gchar *pci_addr)
{
    JsonObject *obj = json_object_new();
    if (!pci_addr) return obj;
    json_object_set_string_member(obj, "pci_addr", pci_addr);
    gchar *cmd = g_strdup_printf("lspci -v -s %s 2>/dev/null", pci_addr);
    gchar *out = NULL;
    if (_run(cmd, &out, NULL) && out)
        json_object_set_string_member(obj, "detail", out);
    g_free(out); g_free(cmd);
    return obj;
}










JsonArray *pcv_gpu_vgpu_types(const gchar *pci_addr)
{
    JsonArray *arr = json_array_new();
    if (!pci_addr) return arr;
    gchar *cmd = g_strdup_printf("ls /sys/bus/pci/devices/0000:%s/mdev_supported_types/ 2>/dev/null", pci_addr);
    gchar *out = NULL;
    if (_run(cmd, &out, NULL) && out) {
        gchar **types = g_strsplit(g_strstrip(out), "\n", -1);
        for (gint i = 0; types[i] && types[i][0]; i++)
            json_array_add_string_element(arr, types[i]);
        g_strfreev(types);
    }
    g_free(out); g_free(cmd);
    return arr;
}













gboolean pcv_gpu_vgpu_create(const gchar *pci_addr, const gchar *type,
                              gchar **uuid_out, GError **error)
{
    if (!pci_addr || !type) {
        g_set_error(error, g_quark_from_static_string("gpu"), 1, "pci_addr and type required");
        return FALSE;
    }
    gchar *cmd = g_strdup_printf("mdevctl start --parent 0000:%s --type %s 2>&1", pci_addr, type);
    gchar *out = NULL;
    gboolean ok = _run(cmd, &out, error);
    if (ok && out && uuid_out) *uuid_out = g_strdup(g_strstrip(out));
    g_free(out); g_free(cmd);
    return ok;
}











gboolean pcv_gpu_vgpu_delete(const gchar *uuid, GError **error)
{
    if (!uuid) { g_set_error(error, g_quark_from_static_string("gpu"), 1, "uuid required"); return FALSE; }
    gchar *cmd = g_strdup_printf("mdevctl stop --uuid %s 2>&1; true", uuid);
    gboolean ok = _run(cmd, NULL, error);
    g_free(cmd);
    return ok;
}









JsonArray *pcv_gpu_vgpu_list(void)
{
    JsonArray *arr = json_array_new();
    gchar *out = NULL;
    if (_run_shell("mdevctl list 2>/dev/null", &out, NULL) && out) {
        gchar **lines = g_strsplit(out, "\n", -1);
        for (gint i = 0; lines[i] && lines[i][0]; i++) {
            JsonObject *v = json_object_new();
            json_object_set_string_member(v, "entry", lines[i]);
            json_array_add_object_element(arr, v);
        }
        g_strfreev(lines);
    }
    g_free(out);
    return arr;
}


















gboolean pcv_gpu_attach(const gchar *vm_name, const gchar *pci_addr, GError **error)
{
    if (!vm_name || !pci_addr) {
        g_set_error(error, g_quark_from_static_string("gpu"), 1, "vm_name and pci_addr required");
        return FALSE;
    }
    guint d=0,b=0,s=0,f=0;
    sscanf(pci_addr, "%x:%x:%x.%x", &d, &b, &s, &f);
    gchar *xml = g_strdup_printf(
        "<hostdev mode='subsystem' type='pci' managed='yes'>\n"
        "  <source><address domain='0x%04x' bus='0x%02x' slot='0x%02x' function='0x%x'/></source>\n"
        "</hostdev>", d, b, s, f);
    gchar *path = g_strdup_printf("/tmp/pcv-gpu-%s.xml", vm_name);
    g_file_set_contents(path, xml, -1, NULL);
    gchar *cmd = g_strdup_printf("virsh attach-device %s %s --live 2>&1", vm_name, path);
    gboolean ok = _run(cmd, NULL, error);
    g_unlink(path);
    g_free(cmd); g_free(path); g_free(xml);
    return ok;
}













gboolean pcv_gpu_detach(const gchar *vm_name, const gchar *pci_addr, GError **error)
{
    if (!vm_name || !pci_addr) {
        g_set_error(error, g_quark_from_static_string("gpu"), 1, "vm_name and pci_addr required");
        return FALSE;
    }
    guint d=0,b=0,s=0,f=0;
    sscanf(pci_addr, "%x:%x:%x.%x", &d, &b, &s, &f);
    gchar *xml = g_strdup_printf(
        "<hostdev mode='subsystem' type='pci' managed='yes'>\n"
        "  <source><address domain='0x%04x' bus='0x%02x' slot='0x%02x' function='0x%x'/></source>\n"
        "</hostdev>", d, b, s, f);
    gchar *path = g_strdup_printf("/tmp/pcv-gpu-detach-%s.xml", vm_name);
    g_file_set_contents(path, xml, -1, NULL);
    gchar *cmd = g_strdup_printf("virsh detach-device %s %s --live 2>/dev/null; true", vm_name, path);
    gboolean ok = _run(cmd, NULL, error);
    g_unlink(path);
    g_free(cmd); g_free(path); g_free(xml);
    return ok;
}
