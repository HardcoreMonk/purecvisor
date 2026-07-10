/**
 * @file vm_vnet_cache.h
 * @brief vm_name → vnet[] 캐시 (security_group 디스패치용). virsh/nft 무의존 순수 자료구조.
 *
 * [왜?] _rebuild_dispatch 가 매 SG 변이마다 바인딩된 모든 VM 을 virsh domiflist 로
 *   해석하던 O(N) sweep(I-2)을 제거한다. 캐시는 VM 라이프사이클로 키잉되며
 *   (실행 중 VM = vnet 보유), sync_vm 의 evict + _rebuild_dispatch 의 lazy-miss 로
 *   정합을 유지한다. spec: docs/superpowers/specs/2026-07-04-sg-vnet-cache-design.md
 */
#pragma once
#include <glib.h>

/* vnets 의 gchar* 요소들을 캐시가 복사 소유. 호출자 배열은 그대로 유지. */
void        pcv_vm_vnet_cache_put(const gchar *vm, GPtrArray *vnets);
/* 반환: (transfer full) 스냅샷 복사본 — 호출자가 g_ptr_array_unref. miss 시 NULL. */
GPtrArray  *pcv_vm_vnet_cache_get(const gchar *vm);
void        pcv_vm_vnet_cache_evict(const gchar *vm);
