/* src/modules/virt/vm_config_builder.h */

#ifndef PURECVISOR_VM_CONFIG_BUILDER_H
#define PURECVISOR_VM_CONFIG_BUILDER_H

#include <glib.h>
#include <libvirt-gobject/libvirt-gobject.h>

// 불투명 구조체 (Opaque struct)로 사용하거나 정의 필요. 
// 여기서는 헤더에 구조체 정의를 포함하거나 typedef만 하고 .c에서 정의.
// 컴파일을 위해 typedef만 선언
typedef struct _PureCVisorVmConfig PureCVisorVmConfig;

PureCVisorVmConfig *purecvisor_vm_config_new(const gchar *name, gint vcpu, gint ram_mb);
void purecvisor_vm_config_free(PureCVisorVmConfig *config);

void purecvisor_vm_config_set_disk(PureCVisorVmConfig *config, const gchar *path);
void purecvisor_vm_config_set_iso(PureCVisorVmConfig *config, const gchar *path);

// [Added] Bridge 설정 함수
void purecvisor_vm_config_set_network_bridge(PureCVisorVmConfig *config, const gchar *bridge_name);

GVirConfigDomain *purecvisor_vm_config_build(PureCVisorVmConfig *config);

#endif