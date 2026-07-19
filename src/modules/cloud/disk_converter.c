
#include "cloud_migration.h"
#include "../../utils/pcv_spawn.h"
#include "../../utils/pcv_log.h"
#include "../../utils/pcv_config.h"

#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#define DISK_LOG "disk_converter"

gchar *
pcv_disk_convert_raw_to_qcow2(const gchar *raw_path, const gchar *vm_name,
                                 const gchar *output_dir, GError **error)
{
    gchar *out_path = g_strdup_printf("%s/%s.qcow2",
        output_dir ? output_dir : "/var/lib/libvirt/images", vm_name);

    if (g_file_test(out_path, G_FILE_TEST_EXISTS)) {
        g_set_error(error, g_quark_from_static_string("disk_converter"), 1,
                    "target disk already exists, refusing to overwrite: %s", out_path);
        g_free(out_path);
        return NULL;
    }

    const gchar *argv[] = {
        "qemu-img", "convert",
        "-f", "raw", "-O", "qcow2",
        "-p",
        raw_path, out_path,
        NULL
    };

    PCV_LOG_INFO(DISK_LOG, "Converting RAW → qcow2: %s → %s", raw_path, out_path);

    gchar *out = NULL, *err_out = NULL;
    if (!pcv_spawn_sync(argv, &out, &err_out, error)) {
        PCV_LOG_WARN(DISK_LOG, "qemu-img convert failed: %s",
                     err_out ? err_out : "unknown");
        g_free(out); g_free(err_out);
        unlink(out_path);
        g_free(out_path);
        return NULL;
    }
    g_free(out); g_free(err_out);

    PCV_LOG_INFO(DISK_LOG, "Conversion complete: %s", out_path);
    return out_path;
}

gchar *
pcv_disk_convert_qcow2_to_raw(const gchar *qcow2_path, const gchar *vm_name,
                                 const gchar *output_dir, GError **error)
{
    gchar *out_path = g_strdup_printf("%s/%s-export.raw",
        output_dir ? output_dir : PCV_CLOUD_EXPORT_DIR, vm_name);

    g_mkdir_with_parents(output_dir ? output_dir : PCV_CLOUD_EXPORT_DIR, 0755);

    const gchar *argv[] = {
        "qemu-img", "convert",
        "-f", "qcow2", "-O", "raw",
        "-p",
        qcow2_path, out_path,
        NULL
    };

    PCV_LOG_INFO(DISK_LOG, "Converting qcow2 → RAW: %s → %s", qcow2_path, out_path);

    gchar *out = NULL, *err_out = NULL;
    if (!pcv_spawn_sync(argv, &out, &err_out, error)) {
        PCV_LOG_WARN(DISK_LOG, "qemu-img convert failed: %s",
                     err_out ? err_out : "unknown");
        g_free(out); g_free(err_out);
        unlink(out_path);
        g_free(out_path);
        return NULL;
    }
    g_free(out); g_free(err_out);

    PCV_LOG_INFO(DISK_LOG, "Conversion complete: %s", out_path);
    return out_path;
}

gchar *
pcv_disk_find_vm_disk(const gchar *vm_name, GError **error)
{

    const gchar *pool = pcv_config_get_string("storage", "zvol_pool", "pcvpool/vms");
    gchar *zvol_path = g_strdup_printf("/dev/zvol/%s/%s", pool, vm_name);
    if (access(zvol_path, F_OK) == 0) return zvol_path;
    g_free(zvol_path);

    const gchar *img_dir = pcv_config_get_string("storage", "image_dir",
                                                   "/var/lib/libvirt/images");
    gchar *qcow2_path = g_strdup_printf("%s/%s.qcow2", img_dir, vm_name);
    if (access(qcow2_path, F_OK) == 0) return qcow2_path;
    g_free(qcow2_path);

    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                "No disk found for VM '%s'", vm_name);
    return NULL;
}

gboolean
pcv_disk_inject_virtio(const gchar *disk_path, GError **error)
{

    const gchar *check_argv[] = {"which", "virt-customize", NULL};
    gchar *which_out = NULL;
    if (!pcv_spawn_sync(check_argv, &which_out, NULL, NULL) || !which_out || !*which_out) {
        g_free(which_out);
        PCV_LOG_WARN(DISK_LOG, "virt-customize not found — skipping virtio injection. "
                     "Install libguestfs-tools for automatic driver conversion.");
        return TRUE;
    }
    g_free(which_out);

    const gchar *argv[] = {
        "virt-customize", "-a", disk_path,
        "--install", "linux-image-generic",
        "--run-command", "dracut -f 2>/dev/null || update-initramfs -u 2>/dev/null || true",
        NULL
    };

    PCV_LOG_INFO(DISK_LOG, "Injecting virtio drivers: %s", disk_path);

    gchar *out = NULL, *err_out = NULL;
    if (!pcv_spawn_sync(argv, &out, &err_out, error)) {
        PCV_LOG_WARN(DISK_LOG, "virt-customize failed (non-fatal): %s",
                     err_out ? err_out : "unknown");
        g_free(out); g_free(err_out);

        return TRUE;
    }
    g_free(out); g_free(err_out);

    PCV_LOG_INFO(DISK_LOG, "Virtio driver injection complete");
    return TRUE;
}

gboolean
pcv_disk_apply_delta(const gchar *base_qcow2, const gchar *delta_raw,
                       GError **error)
{

    gchar *merged = g_strdup_printf("%s.merged", base_qcow2);
    const gchar *rebase_argv[] = {
        "qemu-img", "convert", "-f", "raw", "-O", "qcow2",
        delta_raw, merged, NULL
    };

    PCV_LOG_INFO(DISK_LOG, "Applying delta: %s → %s (merged: %s)",
                 delta_raw, base_qcow2, merged);

    gchar *verr = NULL;
    gboolean ok = pcv_spawn_sync(rebase_argv, NULL, &verr, error);
    if (ok) {

        if (rename(merged, base_qcow2) != 0) {
            PCV_LOG_WARN(DISK_LOG, "rename(%s, %s) failed: %s",
                         merged, base_qcow2, g_strerror(errno));
            unlink(merged);
            ok = FALSE;
            if (error && !*error) {
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Failed to rename merged image");
            }
        } else {
            PCV_LOG_INFO(DISK_LOG, "Delta applied successfully: %s", base_qcow2);
        }
    } else {
        PCV_LOG_WARN(DISK_LOG, "Delta conversion failed: %s",
                     verr ? verr : "unknown");
        unlink(merged);
    }
    g_free(merged);
    g_free(verr);
    return ok;
}
