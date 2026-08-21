# ADR-0040: Local VPC는 Linux bridge 우선 backend와 비중첩 IPv4 CIDR을 사용한다

- **상태:** Approved
- **일자:** 2026-08-11
- **승인:** 2026-08-12 사용자 명시 승인
- **Single Edge 적용 상태:** production 구현·실환경 검증 완료
- **후속 결정:** ADR-0045가 생성 시 고정하는 선택형 OVN adapter를
  추가했다. 본 ADR의 Linux 기본값·전역 비중첩 CIDR 계약은 그대로 유효하다.
- **관련:** ADR-0030, ADR-0033, ADR-0045

## Context

Single Edge에는 bridge 단위 network 기능은 있지만 VPC, 다중 subnet, tenant attachment를
소유하는 aggregate가 없다. 대상 노드에는 OVS가 설치돼 있으나 구성 bridge가 없고 OVN은
available하지 않다. per-VPC namespace나 OVN logical router는 중첩 CIDR을 지원할 수 있지만
단일 호스트 최초 범위의 lifecycle과 장애 복구 비용을 크게 늘린다.

## Decision

1. Local VPC 하나는 하나 이상의 IPv4 subnet을 소유한다.
2. 최초이자 생략 시 기본 데이터면 backend는 subnet별 Linux bridge, host gateway,
   dnsmasq다. 후속 OVN backend는 ADR-0045가 별도로 규정한다.
3. 같은 VPC subnet 사이의 L3 routing은 host root network namespace에서 수행한다.
4. Local VPC 전체 subnet과 host connected/on-link prefix는 서로 중첩될 수 없다.
5. 기존 `pcvnat0`와 임의 bridge는 자동으로 VPC에 편입하지 않는다.
6. OVS/OVN과 per-VPC network namespace는 public VPC 계약을 유지하는 후속 backend 후보다.
   이 중 OVN adapter는 ADR-0045에 따라 구현됐지만 공개 지원 gate는 아직 남아 있다.

## Consequences

- 한 호스트에서 tenant·subnet·attachment를 일관되게 관리할 수 있다.
- 중첩 CIDR, 멀티 호스트, HA router는 지원하지 않는다.
- CIDR validation은 DB row뿐 아니라 host route·기존 bridge actual state를 확인해야 한다.
- bridge, dnsmasq, nft 상태는 SQLite desired state에서 재생성 가능해야 한다.

## Verification

둘 이상의 subnet을 가진 VPC routing, global overlap 거부, host route overlap 거부,
legacy bridge 불변과 재시작 reconcile을 검증한다. overlap 검사를 제거하면 충돌 subnet
생성이 성공해 테스트가 RED가 되는 반사실을 유지한다.
