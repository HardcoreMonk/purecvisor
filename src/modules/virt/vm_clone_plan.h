#pragma once

/**
 * @file vm_clone_plan.h
 * @brief VM clone 전 디스크 타입과 target 경로를 계산하는 작은 plan 모듈.
 *
 * dispatcher.c는 요청/권한/응답을 담당하고, 이 모듈은 libvirt XML 안의
 * data disk가 zvol/qcow2/raw 중 무엇인지 판별하고, storage별 target
 * 경로를 계산한다. qcow2/raw는 qemu-img full copy로만 실행하며,
 * 일반 VM clone은 virt-sysprep identity reset, virt-filesystems filesystem
 * 수집, guestfish filesystem UUID 보정, virt-customize fstab/boot artifact
 * 재생성 경로를 거친다.
 */

#include <glib.h>

typedef enum {
    PCV_VM_CLONE_DISK_UNSUPPORTED = 0,
    PCV_VM_CLONE_DISK_ZVOL,
    PCV_VM_CLONE_DISK_QCOW2,
    PCV_VM_CLONE_DISK_RAW,
} PcvVmCloneDiskKind;

typedef struct {
    guint                disk_count;
    PcvVmCloneDiskKind   kind;
    gchar               *source_attr;
    gchar               *source_path;
    gchar               *driver_type;
} PcvVmCloneDiskInfo;

typedef struct {
    PcvVmCloneDiskKind   kind;
    gchar               *source_disk_path;
    gchar               *target_disk_path;
    gchar               *source_dataset;
    gchar               *target_dataset;
    gchar               *zfs_pool;
    gchar               *source_zvol_name;
} PcvVmCloneDiskPlan;

const gchar *pcv_vm_clone_disk_kind_to_string(PcvVmCloneDiskKind kind);

void pcv_vm_clone_disk_info_clear(PcvVmCloneDiskInfo *info);
void pcv_vm_clone_disk_plan_clear(PcvVmCloneDiskPlan *plan);

gboolean pcv_vm_clone_extract_disk_info(const gchar *xml,
                                        PcvVmCloneDiskInfo *info,
                                        gchar **error_msg);

gboolean pcv_vm_clone_build_zvol_plan(const gchar *target_name,
                                      const PcvVmCloneDiskInfo *disk,
                                      PcvVmCloneDiskPlan *plan,
                                      gchar **error_msg);

gboolean pcv_vm_clone_build_disk_plan(const gchar *target_name,
                                      const PcvVmCloneDiskInfo *disk,
                                      PcvVmCloneDiskPlan *plan,
                                      gchar **error_msg);

gboolean pcv_vm_clone_copy_file_disk(const PcvVmCloneDiskPlan *plan,
                                     GError **error);

gboolean pcv_vm_clone_file_copy_available(void);

gboolean pcv_vm_clone_guest_reset_available(void);

GStrv pcv_vm_clone_build_guest_reset_argv(const PcvVmCloneDiskPlan *plan,
                                          const gchar *hostname,
                                          gchar **error_msg);

GStrv pcv_vm_clone_build_guest_boot_rebuild_argv(const PcvVmCloneDiskPlan *plan,
                                                 gchar **error_msg);

gboolean pcv_vm_clone_reset_guest_identity(const PcvVmCloneDiskPlan *plan,
                                           const gchar *hostname,
                                           GError **error);

gboolean pcv_vm_clone_disk_plan_beta_allowed(const PcvVmCloneDiskPlan *plan,
                                             gchar **error_msg);
