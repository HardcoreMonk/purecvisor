/**
 * @file iscsi_manager.h
 * @brief iSCSI 타겟/이니시에이터 매니저 — tgtadm/iscsiadm 래퍼
 *
 * == 아키텍처에서의 위치 ==
 *   handler_overlay.c(iscsi.* RPC) → pcv_iscsi_*() → tgtadm / iscsiadm CLI
 *
 * == 타겟 관리 (서버 측) ==
 *   ZFS zvol을 iSCSI LUN으로 익스포트합니다.
 *   IQN 형식: iqn.2026-03.purecvisor:<vm_name>
 *   타겟 생성 → LUN 추가 → 이니시에이터 바인딩
 *
 * == 이니시에이터 관리 (클라이언트 측) ==
 *   원격 iSCSI 타겟에 연결하여 로컬 블록 디바이스를 얻습니다.
 *   discovery → login → /dev/disk/by-path/ 에서 디바이스 경로 탐색
 *
 * == 멱등성 ==
 *   target_create: 이미 존재하면 TRUE 반환 (재생성 안 함)
 *   target_delete: 없으면 TRUE 반환 (에러 아님)
 *
 * == 런타임 의존성 ==
 *   - tgt (iSCSI 타겟 데몬): apt install tgt
 *   - open-iscsi (이니시에이터): apt install open-iscsi
 */
#ifndef PURECVISOR_ISCSI_MANAGER_H
#define PURECVISOR_ISCSI_MANAGER_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* Lifecycle */
void pcv_iscsi_init(void);
void pcv_iscsi_shutdown(void);

/* Target management (export zvol as iSCSI LUN) */
gboolean    pcv_iscsi_target_create(const gchar *vm_name, const gchar *zvol_path, GError **error);
gboolean    pcv_iscsi_target_delete(const gchar *vm_name, GError **error);
JsonArray  *pcv_iscsi_target_list(void);

/* CHAP authentication */
gboolean pcv_iscsi_target_set_chap(const gchar *vm_name, const gchar *chap_user,
                                    const gchar *chap_password, GError **error);

/* Initiator management (connect to remote iSCSI target) */
gboolean pcv_iscsi_initiator_connect(const gchar *target_ip, const gchar *vm_name,
                                      gchar **device_path, GError **error);
gboolean pcv_iscsi_initiator_disconnect(const gchar *target_ip, const gchar *vm_name,
                                         GError **error);

G_END_DECLS

#endif /* PURECVISOR_ISCSI_MANAGER_H */
