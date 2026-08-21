# ADR-0041: Local VPC 보안 경계는 fail-closed single writer와 attachment 기반 publish다

- **상태:** Approved
- **일자:** 2026-08-11
- **승인:** 2026-08-12 사용자 명시 승인
- **Single Edge 적용 상태:** production 구현·실환경 검증 완료
- **후속 적용:** ADR-0045는 같은 single-writer·Publish 계약을 OVN Port Group/ACL과
  host edge에도 적용한다. 공개 지원 잔여 gate는 ADR-0045를 따른다.
- **관련:** ADR-0012, ADR-0018, ADR-0019, ADR-0025, ADR-0030, ADR-0045

## Context

기존 per-network NAT의 broad forward allow와 여러 저수준 network/NIC RPC를 그대로 두면
VPC 격리를 우회할 수 있다. NAT outbound만으로는 외부 사용자가 VM 서비스를 시작 연결로
접근할 수 없지만, Floating IP와 public gateway는 Single Edge 최초 범위에 비해 크다.
재시작 중 일부 정책만 복구되는 경우에는 이전 conntrack과 열린 bridge가 tenant 경계를
우회할 수 있다.

## Decision

1. VPC controller만 managed bridge, gateway, DHCP와 VPC nft policy를 변경하는 single
   writer다.
2. 같은 VPC 통신만 명시 허용하고 서로 다른 VPC는 conntrack established 허용보다 먼저
   차단한다.
3. attachment는 관리 MAC·IP binding과 anti-spoofing을 가진다.
4. 부팅 시 managed bridge를 먼저 quarantine하고 전체 desired policy 적용이 확인된 뒤에만
   traffic을 연다. policy restore 실패 시 bridge를 DOWN으로 내려 fail-closed한다.
5. 외부 inbound는 `nat` VPC의 `ACTIVE` attachment를 대상으로 하는 제한형 Service
   Publish만 제공한다. 임의 target IP, port range, cross-VPC target은 금지한다.
6. non-admin은 `0.0.0.0/0` source publish를 만들 수 없고, Security Group ingress는
   publish와 독립된 두 번째 관문이다.
7. 저수준 network mutation, raw NIC attach와 raw VM bridge 선택은 managed bridge를
   거부한다.

## Consequences

- 관리자는 범용 public network를 열지 않고 필요한 host port만 VM service에 게시할 수 있다.
- nft policy는 append가 아니라 전체 desired snapshot의 원자 transaction으로 관리한다.
- policy compiler와 single-writer guard가 동작하지 않으면 VPC API도 성공을 반환할 수 없다.
- Service Publish는 Floating IP 이동성, hairpin NAT, public IP pool을 제공하지 않는다.

## Verification

same/cross-VPC, source spoofing, host input, raw bridge mutation, startup restore failure와
Service Publish source/target 경계를 production 정책 compiler와 격리 데이터면에서 검증한다.
각 강제 rule 또는 guard를 제거하면 해당 효과 테스트가 RED가 되는 반사실을 유지한다.
