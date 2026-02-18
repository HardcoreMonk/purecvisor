#pragma once

#include <glib-object.h>
#include <libvirt-gconfig/libvirt-gconfig.h>
#include "vm_types.h" // 공통 타입 포함

G_BEGIN_DECLS

// 구조체 정의 제거됨 (vm_types.h로 이동)

/**
 * VM 설정을 담은 GVirConfigDomain 객체를 생성하여 반환합니다.
 * Caller는 반환된 객체를 g_object_unref() 해야 합니다.
 */
GVirConfigDomain *purecvisor_vm_config_builder_create_config(PureCVisorVmConfig *config, GError **error);

G_END_DECLS