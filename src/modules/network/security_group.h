/**
 * @file security_group.h
 * @brief 네트워크 보안 그룹 공개 API — security_group.c 참조
 *
 * dispatcher.c 는 그동안 이 헤더 없이 인라인 extern 선언으로 링크해왔다
 * (동일 시그니처). 이후 dispatcher.c 를 이 헤더로 전환할 수 있다.
 */
#pragma once
#include <glib.h>
#include <json-glib/json-glib.h>

[[nodiscard]] gboolean pcv_security_group_create(const gchar *name, const gchar *description);
gboolean   pcv_security_group_delete(const gchar *name);
JsonArray *pcv_security_group_list(void);
gboolean   pcv_security_group_rule_add(const gchar *name, JsonObject *rule);
gboolean   pcv_security_group_rule_remove(const gchar *name, gint64 rule_id);
gboolean   pcv_security_group_apply_to_vm(const gchar *vm, const gchar *sg_name);
gboolean   pcv_security_group_detach_vm(const gchar *vm, const gchar *sg_name);  /* Task 7 구현 */
void       pcv_security_group_sync_vm(const gchar *vm_name);                     /* Task 8 구현 */
gboolean   pcv_security_group_vm_is_bound(const gchar *vm);  /* virsh/nft 무의존 — virt_events 게이트 */
void       pcv_security_group_restore(void);

/* [I2-R1] 바인딩된 모든 VM 의 vnet 을 fresh 재해석해 stale 캐시를 교정하고 dispatch
 * 재생성. non-empty 해석만 캐시 덮어씀(keep-old-on-empty). 바인딩 0 이면 no-op.
 * 블로킹(virsh+nft) — 워커/부팅 경로에서만 호출. */
void pcv_security_group_resync_all(void);

/* [I2-R1] 주기 resync 타이머 등록/해제 — main.c 가 호출. config
 * security_group.resync_interval_sec(기본 300, 0/음수=미등록). */
void pcv_security_group_resync_timer_init(void);
void pcv_security_group_resync_timer_shutdown(void);
