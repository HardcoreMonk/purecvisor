/**
 * @file vm_batch_policy.c
 * @brief vm.batch action whitelist 멤버십 정책 (순수) 구현
 *
 * whitelist = vm_manager 에 public `_async` fn 이 실존하는 action(start/stop).
 * pause/resume/reboot 는 vm_manager 에 `_async` public fn 이 없어 제외,
 * delete 는 파괴적이라 whitelist 후보집합 밖(근거는 dispatcher.c
 * PCV_VM_BATCH_WHITELIST[] 주석 참조).
 *
 * 동기 규약(단일 진리원):
 *   아래 WHITELIST[] 가 "허용" 판정의 단일 진리원이다.
 *   dispatcher.c 의 PCV_VM_BATCH_WHITELIST[] fn 배열은 "허용→vm_manager async
 *   fn" 매핑이며, 이 리스트와 동기 유지해야 한다(양쪽 모두 {start,stop}).
 *
 * (I-2 시정: 데몬과 test_runner 가 둘 다 링크하는 순수 TU 로 추출 — 유닛
 *  테스트가 손복제 대신 이 실 함수를 호출한다. vm_manager 의존 절대 금지.)
 */
#include "vm_batch_policy.h"

gboolean pcv_vm_batch_action_is_whitelisted(const gchar *action)
{
    /* canonical 허용 action 리스트(단일 진리원). fn 매핑은 dispatcher.c 에 잔존. */
    static const gchar *WHITELIST[] = { "start", "stop", NULL };
    if (!action)
        return FALSE;
    for (int i = 0; WHITELIST[i]; i++)
        if (g_strcmp0(action, WHITELIST[i]) == 0)
            return TRUE;
    return FALSE;
}
