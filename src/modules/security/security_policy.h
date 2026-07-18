/**
 * @file security_policy.h
 * @brief 보안 이벤트 정책 — severity 정규화·권장 액션·감사 여부·coalesce 키
 *
 * SG 가 수집한 원시 이벤트를 저장 전에 정규화하는 정책 계층. 모든 소비자(UI·감사·
 * HIPS)가 동일한 severity 와 권장 액션을 보도록, 정규화는 submit 시점 한 곳
 * (pcv_security_submit_event)에서 일괄 적용된다.
 *
 * [불변식 — ADR-0024]
 *   - recommend_action 이 돌려주는 것은 '권장' 라벨일 뿐 실행 허가가 아니다. 실제
 *     실행 가능 여부는 pcv_hips_action_is_executable 가 별도로 판정한다(block_ip·
 *     revoke_api_key 만 실행 가능, 나머지는 manual_runbook 후보).
 *   - should_audit 이 TRUE 인 WARN/CRIT 이벤트는 감사 로그·알림과 같은 event_id 를
 *     공유한다(단일 상관관계 키).
 */
#ifndef PURECVISOR_SECURITY_POLICY_H
#define PURECVISOR_SECURITY_POLICY_H

#include "modules/security/security_event.h"

G_BEGIN_DECLS

/* 저장용 최종 severity 로 정규화한다(submit 시점 단일 적용 — 소비자 간 일관성 보장). */
PcvSecuritySeverity pcv_security_policy_normalize_severity(const PcvSecurityEvent *ev);
/* normalize_severity 의 runtime source 특화 경로(런타임 프로브 이벤트용). */
PcvSecuritySeverity pcv_security_policy_normalize_runtime_severity(const PcvSecurityEvent *ev);
/* 권장 대응 라벨을 반환한다. 실행 허가가 아님 — 위 [불변식] 참조. */
const gchar *pcv_security_policy_recommend_action(const PcvSecurityEvent *ev);
/* 동일 위험 관측을 하나로 합치기 위한 정책 coalesce 키. */
gchar *pcv_security_policy_coalesce_key(const PcvSecurityEvent *ev);
/* 감사 로그·알림 기록 여부(TRUE 인 WARN/CRIT 은 이벤트와 같은 event_id 공유). */
gboolean pcv_security_policy_should_audit(const PcvSecurityEvent *ev);

G_END_DECLS

#endif
