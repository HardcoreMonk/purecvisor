# ADR-0024: Native Host HIDS/HIPS는 탐지 우선, 수동 승인 대응으로 제한한다

- **상태**: Accepted
- **일자**: 2026-05-03
- **Single Edge 적용 상태**: 활성. `purecvisor` 호스트 노드 보호 기능으로 제한한다.
- **관련**: ADR-0001, ADR-0007, ADR-0018, ADR-0019

## 맥락

PureCVisor Single Edge는 하이퍼바이저 제어면이다. HIDS/HIPS 기능이 오탐으로
프로세스를 종료하거나 서비스를 차단하면 운영 장애가 보안 기능에서 발생한다.

## 결정

1. v1은 탐지, 감사, 알림 중심으로 동작한다.
2. `security_guard.enabled` 기본값은 `false`다.
3. 파일 무결성 baseline은 admin이 명시적으로 refresh해야 trusted가 된다.
4. 실제 실행 가능한 HIPS action은 `block_ip`와 `revoke_api_key`뿐이다.
5. `lock_user`, `restart_service`, `quarantine_process`, `restore_config`는
   `manual_runbook` 후보로만 노출한다.
6. WARN/CRIT security event와 audit 기록은 같은 `event_id`를 공유한다.
7. 자동 차단 모드는 별도 ADR 없이 추가하지 않는다.

## 결과

- 좋음: 보안 가시성을 추가하면서 제어면 오탐 위험을 낮춘다.
- 나쁨: v1은 적극 차단 제품이 아니라 운영자 승인 기반 대응이다.
- 유지 규칙: `make check-rbac`와 security 단위 테스트가 신규 `security.*` RPC 정책을 검증한다.

## 하지 않기로 한 것

- Suricata inline IPS, NFQUEUE, CrowdSec bouncer 자동 차단
- 게스트 VM 내부 에이전트 배포
- Wazuh, Falco, osquery 필수 의존성
- 자동 프로세스 종료, 자동 사용자 잠금, 자동 VM 중지
