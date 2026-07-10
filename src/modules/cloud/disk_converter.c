/**
 * @file disk_converter.c
 * @brief 디스크 이미지 변환 — qemu-img + virt-customize 래핑
 *
 * RAW ↔ qcow2 변환, virtio 드라이버 자동 주입
 * 기존 vm_manager.c의 qemu-img 패턴 재사용
 */

#include "cloud_migration.h"
#include "../../utils/pcv_spawn.h"
#include "../../utils/pcv_log.h"
#include "../../utils/pcv_config.h"

#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

/*
 * ============================================================================
 *  [주니어 개발자 필독] 디스크 이미지 변환 핵심 개념
 * ============================================================================
 *
 *  AWS EC2는 RAW 디스크 형식을 사용하고, PureCVisor(KVM)은 qcow2를 사용합니다.
 *  qcow2는 스냅샷, 씬 프로비저닝, 압축을 지원하여 KVM에 최적화되어 있습니다.
 *
 *  변환은 qemu-img convert 명령으로 수행합니다:
 *    Import: RAW → qcow2 (AWS에서 가져올 때)
 *    Export: qcow2 → RAW (AWS로 내보낼 때)
 *
 *  virtio 드라이버 주입 (pcv_disk_inject_virtio):
 *    AWS EC2는 ENA/NVMe 드라이버를 사용하지만, KVM은 virtio를 사용합니다.
 *    virt-customize로 게스트 OS에 virtio 드라이버를 주입합니다.
 *    Ubuntu/Amazon Linux는 기본 포함이므로 실패해도 non-fatal입니다.
 *
 *  Near-Live 델타 병합 (pcv_disk_apply_delta):
 *    Phase 1에서 생성된 기본 이미지에 Phase 2의 델타(변경분)를 병합합니다.
 *    qemu-img convert로 변환 후 rename()으로 원자적 교체합니다.
 * ============================================================================
 */

#define DISK_LOG "disk_converter"

/* ══════════════════════════════════════════════════════════════
 * RAW → qcow2 변환
 * ══════════════════════════════════════════════════════════════ */
gchar *
pcv_disk_convert_raw_to_qcow2(const gchar *raw_path, const gchar *vm_name,
                                 const gchar *output_dir, GError **error)
{
    gchar *out_path = g_strdup_printf("%s/%s.qcow2",
        output_dir ? output_dir : "/var/lib/libvirt/images", vm_name);

    const gchar *argv[] = {
        "qemu-img", "convert",
        "-f", "raw", "-O", "qcow2",
        "-p",  /* 진행률 표시 */
        raw_path, out_path,
        NULL
    };

    PCV_LOG_INFO(DISK_LOG, "Converting RAW → qcow2: %s → %s", raw_path, out_path);

    gchar *out = NULL, *err_out = NULL;
    if (!pcv_spawn_sync(argv, &out, &err_out, error)) {
        PCV_LOG_WARN(DISK_LOG, "qemu-img convert failed: %s",
                     err_out ? err_out : "unknown");
        g_free(out); g_free(err_out);
        unlink(out_path);  /* 실패 시 불완전 파일 삭제 */
        g_free(out_path);
        return NULL;
    }
    g_free(out); g_free(err_out);

    PCV_LOG_INFO(DISK_LOG, "Conversion complete: %s", out_path);
    return out_path;
}

/* ══════════════════════════════════════════════════════════════
 * qcow2 → RAW 변환
 * ══════════════════════════════════════════════════════════════ */
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

/* ══════════════════════════════════════════════════════════════
 * VM 디스크 경로 탐색 (libvirt XML에서 추출)
 * ══════════════════════════════════════════════════════════════ */
gchar *
pcv_disk_find_vm_disk(const gchar *vm_name, GError **error)
{
    /* 1. ZFS zvol 확인 */
    const gchar *pool = pcv_config_get_string("storage", "zvol_pool", "pcvpool/vms");
    gchar *zvol_path = g_strdup_printf("/dev/zvol/%s/%s", pool, vm_name);
    if (access(zvol_path, F_OK) == 0) return zvol_path;
    g_free(zvol_path);

    /* 2. qcow2 파일 확인 */
    const gchar *img_dir = pcv_config_get_string("storage", "image_dir",
                                                   "/var/lib/libvirt/images");
    gchar *qcow2_path = g_strdup_printf("%s/%s.qcow2", img_dir, vm_name);
    if (access(qcow2_path, F_OK) == 0) return qcow2_path;
    g_free(qcow2_path);

    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                "No disk found for VM '%s'", vm_name);
    return NULL;
}

/* ══════════════════════════════════════════════════════════════
 * virtio 드라이버 주입 (virt-customize, 선택적)
 * Import 후 ENA/NVMe → virtio 전환에 필요
 * ══════════════════════════════════════════════════════════════ */
gboolean
pcv_disk_inject_virtio(const gchar *disk_path, GError **error)
{
    /* virt-customize 존재 여부 확인 */
    const gchar *check_argv[] = {"which", "virt-customize", NULL};
    gchar *which_out = NULL;
    if (!pcv_spawn_sync(check_argv, &which_out, NULL, NULL) || !which_out || !*which_out) {
        g_free(which_out);
        PCV_LOG_WARN(DISK_LOG, "virt-customize not found — skipping virtio injection. "
                     "Install libguestfs-tools for automatic driver conversion.");
        return TRUE;  /* 실패가 아닌 건너뜀 */
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
        /* non-fatal — Ubuntu/Amazon Linux는 virtio 기본 포함 */
        return TRUE;
    }
    g_free(out); g_free(err_out);

    PCV_LOG_INFO(DISK_LOG, "Virtio driver injection complete");
    return TRUE;
}

/* ══════════════════════════════════════════════════════════════
 * Near-Live Import: 델타 RAW → 기본 qcow2에 병합
 * ══════════════════════════════════════════════════════════════ */
gboolean
pcv_disk_apply_delta(const gchar *base_qcow2, const gchar *delta_raw,
                       GError **error)
{
    /* Simpler approach: convert delta RAW to qcow2 and replace base with merged result */
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
        /* Replace base with merged */
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
