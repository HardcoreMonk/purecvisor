/**
 * @file hotplug_affect_policy.h
 * @brief vCPU/메모리 핫플러그 libvirt affect-flag 결정 (순수 함수, ADR-0025)
 *
 * ──────────────────────────────────────────────────────────────
 * [아키텍처 내 위치]
 *   handler_vm_hotplug.c (vm.set_vcpu / vm.set_memory 워커)
 *       → hotplug_get_affect_flags() (실행 상태를 virDomainIsActive 로 읽음)
 *           → pcv_hotplug_compute_affect_flags() (순수 결정, 이 TU)
 *
 * 순수 결정 로직을 데몬·test_runner 가 **둘 다** 링크하는 소형 TU 로 분리한다
 * (vm_batch_policy.c / snapshot_verify_probe.c 와 동일한 ADR-0025 패턴). 이렇게
 * 하면 유닛테스트가 손복제 없이 실 프로덕션 함수를 직접 호출하고, test_runner 가
 * handler_vm_hotplug.c 전체(+ 데몬 전용 심볼 스텁)를 링크할 필요가 없다.
 *
 * 헤더는 <glib.h> 만 의존한다(반환형은 unsigned int; VIR_DOMAIN_AFFECT_* 상수는
 * 구현부 .c 에서 <libvirt/libvirt.h> 로 해소).
 * ──────────────────────────────────────────────────────────────
 */
#ifndef PURECVISOR_DISPATCHER_HOTPLUG_AFFECT_POLICY_H
#define PURECVISOR_DISPATCHER_HOTPLUG_AFFECT_POLICY_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * @brief 실행 상태와 config_only 의도로부터 libvirt affect 플래그를 계산한다 (순수 함수).
 *
 * CONFIG 는 항상 포함(persistent config 반영). LIVE 는 "실행 중(is_active)이고
 * config_only 가 아닐 때"만 포함한다. 따라서 config_only=TRUE 면 실행 중이어도
 * LIVE 를 빼서 CONFIG-only(다음 부팅에 반영)로 강등한다 — vm.set_vcpu/set_memory 의
 * apply="config" 하위호환 계약(Fix B).
 *
 * @param is_active   VM 이 실행 중이면 TRUE (virDomainIsActive==1)
 * @param config_only apply="config" 의도이면 TRUE (실행 중이어도 LIVE 제외)
 * @return VIR_DOMAIN_AFFECT_* 비트 조합
 */
unsigned int pcv_hotplug_compute_affect_flags(gboolean is_active, gboolean config_only);

G_END_DECLS

#endif /* PURECVISOR_DISPATCHER_HOTPLUG_AFFECT_POLICY_H */
