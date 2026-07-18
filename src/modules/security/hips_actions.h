/**
 * @file hips_actions.h
 * @brief HIPS 대응 액션 — 운영자 승인 상태 관리 + 승인 후에만 실행되는 부작용
 *
 * 탐지된 위협에 대한 대응(HIPS)의 결정·실행 계약. v1 은 자동 차단을 하지 않는다:
 * build/list/approve/dismiss 는 운영자 결정 상태를 관리할 뿐이고, execute 만 실제
 * 부작용(nftables DROP·API 키 폐기)을 일으킨다.
 *
 * [불변식 — ADR-0024]
 *   - 실제로 실행 가능한 액션은 block_ip 와 revoke_api_key 뿐이다. lock_user·
 *     restart_service·quarantine_process·restore_config 는 manual_runbook 후보로만
 *     노출되며 execute 되지 않는다(pcv_hips_action_is_executable 가 게이트).
 *   - execute 는 ADR-0018 워커에서 admin 승인이 끝난 뒤에만 호출되어야 한다.
 *   - 자동 차단 모드는 별도 ADR 없이 추가하지 않는다.
 *
 * [승인 오케스트레이션 — SEC-4]
 *   pcv_hips_action_run_approval 이 부작용 실행 앞에서 TTL 만료를 먼저 확인한다.
 *   만료된 pending 은 nft/RBAC 를 절대 execute 하지 않는다. execute_fn 은 주입 가능한
 *   부작용 seam(프로덕션은 pcv_hips_action_execute, 테스트는 spy 전달).
 *
 * Operator note:
 *   이 파일은 "탐지된 위협을 실제로 차단할지"를 사람이 승인하는 게이트다. execute 를
 *   승인 없이 통과시키면 오탐 하나가 정상 IP 차단이나 API 키 폐기로 이어져 서비스
 *   장애를 유발할 수 있다 — 그래서 v1 은 자동 실행을 두지 않는다.
 */
#ifndef PURECVISOR_HIPS_ACTIONS_H
#define PURECVISOR_HIPS_ACTIONS_H

#include "modules/security/security_event.h"

G_BEGIN_DECLS

/*
 * HIPS action contract: build/list/approve/dismiss manage operator decision
 * state; execute performs the side effect and must be called only after admin
 * approval from an ADR-0018 worker.
 */
gboolean pcv_hips_action_is_executable(const gchar *action);
JsonObject *pcv_hips_action_build_pending(const PcvSecurityEvent *ev);
JsonArray *pcv_hips_action_list_pending(void);
gboolean pcv_hips_action_approve(const gchar *event_id,
                                  const gchar *admin_user,
                                  GError **error);
gboolean pcv_hips_action_dismiss(const gchar *event_id,
                                  const gchar *admin_user,
                                  const gchar *reason,
                                  GError **error);
gboolean pcv_hips_action_execute(const gchar *action,
                                 const gchar *target,
                                 GError **error);
/*
 * Approval orchestrator: check TTL expiry BEFORE running the side effect, so an
 * expired pending action never executes nft/RBAC (SEC-4). execute_fn is the
 * injectable side-effect seam (production passes pcv_hips_action_execute; tests
 * pass a spy).
 */
typedef gboolean (*PcvHipsExecuteFn)(const gchar *action,
                                     const gchar *target,
                                     GError **error);
gboolean pcv_hips_action_run_approval(const gchar *event_id,
                                      const gchar *action,
                                      const gchar *target,
                                      const gchar *admin_user,
                                      PcvHipsExecuteFn execute_fn,
                                      GError **error);
gboolean pcv_hips_action_ensure_nft_input_chain(GError **error);
gboolean pcv_hips_action_build_block_ip_argv(const gchar *ip,
                                             const gchar **argv,
                                             gsize argv_len);
gboolean pcv_hips_action_validate_api_key_target(const gchar *client_name);

G_END_DECLS

#endif
