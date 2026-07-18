/**
 * @file security_store.h
 * @brief 보안 이벤트·설정·HIPS pending 액션의 로컬 SQLite 저장소 계약
 *
 * Web UI·CLI·UDS 핸들러·감사·HIPS 승인 상태가 모두 같은 이벤트 스트림을 읽도록,
 * 단일 로컬 SQLite DB 한 곳에 SG 영속 상태를 모은다.
 *
 * [소유권 계약]
 *   반환되는 JsonObject/JsonArray 는 호출자 소유(해제 책임 호출자). 입력
 *   PcvSecurityEvent* 는 호출자 소유로 남는다(store 는 빌려 읽기만 한다).
 *
 * [불변식]
 *   - 읽기(list/get/health)는 store 실패 시 빈 컨테이너/NULL 을 돌려주고 내부
 *     degraded 플래그로 '조사 필요'를 기록한다(데몬을 죽이지 않는 fail-soft).
 *   - HIPS pending 액션은 TTL 이 있으며, 만료 판정은 fail-secure(조회 실패 시 만료
 *     취급) — 만료된 pending 은 승인·실행되지 않는다(SEC-4, ADR-0024).
 *   관련: ADR-0024.
 */
#ifndef PURECVISOR_SECURITY_STORE_H
#define PURECVISOR_SECURITY_STORE_H

#include "modules/security/security_event.h"

G_BEGIN_DECLS

/* 기본 security 이벤트 DB 경로. config("security","db_path")로 override 가능.
 * (store 모듈이 소유 — handler_security 와 SG restore 가 공유하도록 헤더에 둔다.) */
#define PCV_SECURITY_DB_DEFAULT "/var/lib/purecvisor/pcv_security.db"

/*
 * Store API ownership contract: returned JsonObject/JsonArray values are owned
 * by the caller; PcvSecurityEvent input structs stay owned by the caller.
 */
gboolean pcv_security_store_open(const gchar *path);
/* config 경로로 store 를 1회 open (이미 열렸으면 no-op). 부팅 경로(SG restore 등)에서
 * 첫 보안 RPC 이전에 이벤트를 기록해야 할 때 호출. 동시 호출 안전(내부 직렬화). */
gboolean pcv_security_store_ensure_open(void);
void pcv_security_store_close(void);
gboolean pcv_security_submit_event(PcvSecurityEvent *ev, GError **error);
gboolean pcv_security_store_insert_event(const PcvSecurityEvent *ev, GError **error);
JsonArray *pcv_security_store_list_events(gint offset, gint limit,
                                           const gchar *severity,
                                           const gchar *source,
                                           const gchar *status);
JsonObject *pcv_security_store_get_event(const gchar *event_id);
gboolean pcv_security_store_update_event_status(const gchar *event_id,
                                                PcvSecurityStatus status,
                                                GError **error);
gint pcv_security_store_count_by_coalesce_key(const gchar *coalesce_key);
gboolean pcv_security_store_get_bool_config(const gchar *key, gboolean def);
gboolean pcv_security_store_set_bool_config(const gchar *key,
                                            gboolean value,
                                            const gchar *admin_user,
                                            GError **error);
JsonObject *pcv_security_store_health(void);
gboolean pcv_security_store_upsert_pending_action(const PcvSecurityEvent *ev,
                                                  const gchar *action,
                                                  gint ttl_sec,
                                                  GError **error);
JsonArray *pcv_security_store_list_pending_actions(void);
JsonObject *pcv_security_store_get_action(const gchar *event_id);
/* [SEC-4] pending && ttl_sec>0 && expires_at<=now → TRUE (만료 규약은 list_pending과 동일). */
gboolean pcv_security_store_action_is_expired(const gchar *event_id);
gboolean pcv_security_store_update_action_status(const gchar *event_id,
                                                 const gchar *status,
                                                 const gchar *admin_user,
                                                 const gchar *reason,
                                                 GError **error);

G_END_DECLS

#endif
