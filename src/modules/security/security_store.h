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
