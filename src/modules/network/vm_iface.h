/**
 * @file vm_iface.h
 * @brief VM 이름 → vnet/tap 인터페이스 목록 해석 (virsh domiflist)
 *
 * network_manager.c 의 QoS 경로와 security_group.c 의 바인딩 경로가 공유한다.
 * vnet 이름은 VM 재시작마다 바뀌는 휘발성 값 — 캐시 금지, 매번 재해석.
 */
#pragma once
#include <glib.h>

GPtrArray *pcv_vm_iface_parse_domiflist(const gchar *out);
GPtrArray *pcv_vm_iface_list(const gchar *vm_name);
