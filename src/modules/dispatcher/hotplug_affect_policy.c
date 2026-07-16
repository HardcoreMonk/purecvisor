/**
 * @file hotplug_affect_policy.c
 * @brief vCPU/메모리 핫플러그 libvirt affect-flag 결정 (순수 함수, ADR-0025)
 *
 * pcv_hotplug_compute_affect_flags 의 구현. 데몬(handler_vm_hotplug.c)과
 * test_runner(tests/test_hotplug_flags.c)가 둘 다 이 TU 를 링크한다 —
 * 유닛테스트가 손복제 없이 실 프로덕션 함수를 직접 호출하도록 하는 ADR-0025
 * 패턴(vm_batch_policy.c / snapshot_verify_probe.c 와 동일).
 */
#include <libvirt/libvirt.h>

#include "modules/dispatcher/hotplug_affect_policy.h"

/*
 * pcv_hotplug_compute_affect_flags:
 * 실행 상태(is_active)와 config_only 의도로부터 libvirt affect 플래그를 계산하는
 * 순수 함수 (I/O·libvirt 핸들 무의존 — ADR-0025 반사실 유닛테스트 대상).
 *
 * CONFIG 는 항상 포함(persistent config 반영). LIVE 는 "실행 중(is_active)이고
 * config_only 가 아닐 때"만 포함한다. 따라서 config_only=TRUE 면 실행 중이어도
 * LIVE 를 빼서 CONFIG-only(다음 부팅에 반영)로 강등한다 — vm.set_vcpu/set_memory 의
 * apply="config" 하위호환 계약(Fix B). 실행 중 vCPU 를 부팅 수 아래로 줄이는
 * live decrease(-32000 "failed to find appropriate hotpluggable vcpus")를 피하려는
 * 프론트의 재전송 경로가 이 계약을 사용한다.
 *
 * [반사실] `&& !config_only` 를 제거하면 (is_active=TRUE, config_only=TRUE) 케이스가
 * LIVE 를 포함하게 되어 tests/test_hotplug_flags.c 의 config_only_active_no_live 가
 * RED 가 된다 (실행 중이어도 CONFIG-only 라는 계약 붕괴를 감지).
 */
unsigned int pcv_hotplug_compute_affect_flags(gboolean is_active, gboolean config_only)
{
    return VIR_DOMAIN_AFFECT_CONFIG
         | ((is_active && !config_only) ? VIR_DOMAIN_AFFECT_LIVE : 0u);
}
