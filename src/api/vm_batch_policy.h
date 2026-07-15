/**
 * @file vm_batch_policy.h
 * @brief vm.batch action whitelist 멤버십 정책 (순수) 공개 인터페이스
 *
 * 아키텍처 위치:
 *   dispatcher.c 의 _handle_vm_batch 가 whitelist 게이트 결정을 위임하는
 *   순수 멤버십 판정. vm_manager 의존 없음(fn 포인터 맵은 dispatcher.c 에 잔존).
 *   데몬(dispatcher.c)과 test_runner 가 **둘 다** 링크해 유닛 테스트가 실
 *   프로덕션 함수를 호출한다(I-2 시정, ADR-0025 반사실 확보).
 *
 * 동기 규약:
 *   허용 action 의 **단일 진리원**은 vm_batch_policy.c 의 리스트다.
 *   dispatcher.c 의 PCV_VM_BATCH_WHITELIST[] fn 배열은 "허용→vm_manager async
 *   fn" 매핑이며, 이 정책 리스트와 동기 유지해야 한다.
 *
 * 의존: <glib.h> 뿐 (순수).
 */
#ifndef PURECVISOR_VM_BATCH_POLICY_H
#define PURECVISOR_VM_BATCH_POLICY_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * pcv_vm_batch_action_is_whitelisted:
 * @action: vm.batch RPC 의 action 문자열(예: "start"). NULL 이면 FALSE.
 *
 * action 이 vm.batch 팬아웃 허용 목록(canonical: start/stop)에 속하는지 판정한다.
 *
 * Returns: 허용 action 이면 TRUE, 아니면 FALSE.
 */
gboolean pcv_vm_batch_action_is_whitelisted(const gchar *action);

G_END_DECLS

#endif /* PURECVISOR_VM_BATCH_POLICY_H */
