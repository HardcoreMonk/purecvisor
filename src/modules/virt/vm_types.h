#pragma once

#include <glib.h>

/**
 * VM 생성을 위한 설정 구조체
 * - vm_manager.c 및 vm_config_builder.c에서 공통으로 사용
 */
typedef struct {
    gchar *name;
    guint vcpu;           // (구: vcpus) -> vm_manager.c 구현과 일치시킴
    guint memory_mb;      // (구: memory_kb) -> vm_manager.c 구현과 일치시킴
    guint disk_size_gb;   // 신규 필드
    gchar *iso_path;
} PureCVisorVmConfig;