# ADR-0045: Local VPC backend는 생성 시 고정하고 OVN 자원은 VPC controller가 소유한다

- **상태:** Implemented
- **일자:** 2026-08-14
- **승인:** 2026-08-14 사용자 명시 승인
- **Single Edge 적용 상태:** 후보 구현과 제품 경로 검증 완료. 공개 지원과 기존 Linux VPC의
  운영 migration은 아직 없음
- **관련:** ADR-0002, ADR-0018, ADR-0025, ADR-0030, ADR-0033, ADR-0040, ADR-0041

## Context

ADR-0040은 Linux bridge를 최초 Local VPC backend로 채택하면서 OVN을 public 계약을 유지하는
후속 adapter 후보로 남겼다. 현재 Single Edge OVN 코어는 LS/LSP, LR, DHCP, ACL, NAT와 local
chassis packet 효과를 제공하지만 Local VPC store, aggregate revision, Service Publish와
single-writer ownership에는 연결되지 않았다.

OVN을 단순히 subnet별 선택지로 추가하거나 장애 때 Linux bridge로 자동 전환하면 같은 VPC의
MAC/IP, routing과 보안 정책을 둘 이상의 writer가 소유한다. 반대로 모든 north-south 정책을
OVN native NAT로 바꾸면 기존 host address/port 기반 Service Publish 계약을 함께 바꾸게 된다.

## Decision

1. Local VPC 생성 요청은 `backend=linux|ovn`을 선택할 수 있다. 생략과 기존 VPC migration은
   `linux`다.
2. backend는 VPC aggregate의 불변 필드다. 모든 subnet과 attachment가 상속하며 같은 VPC 안의
   backend 혼합과 자동 전환을 금지한다.
3. bare `ovs`는 public VPC backend가 아니다. `ovn` backend가 OVS `br-int` 데이터면을 사용한다.
4. OVN backend는 LS/LR/DHCP/Port Group/ACL과 LSP port security로 동서 트래픽을 구현한다.
   host address/port 기반 Service Publish, `nat`/`isolated` 외부 경계는 관리형 OVN host edge와
   공통 nft compiler로 기존 계약을 보존한다.
5. Local VPC OVN row에는 `purecvisor-owner=local-vpc`와 aggregate UUID/generation을 기록한다.
   VPC controller만 이를 변경하며 generic `ovn.*`는 managed resource mutation을 거부한다.
6. OVN backend가 준비되지 않거나 actual state를 확인할 수 없으면 신규 mutation을 거부하고 기존
   VPC를 quarantine한다. Linux fallback이나 삭제 성공 무동작을 허용하지 않는다.
7. schema v2는 `vpcs.backend`, subnet의 backend-neutral ref와 OVN edge binding을 영속화한다.
   기존 Linux actual resource는 자동 변환하지 않는다.
8. OVN backend는 Linux Local VPC의 CIDR 비중첩, RBAC, revision, async Job/audit, anti-spoofing,
   SG, Service Publish와 restart reconcile 계약을 모두 통과한 뒤에만 공개 지원한다.

## Consequences

- 운영자는 새 VPC마다 단순 Linux 데이터면과 OVN SDN 데이터면을 명시적으로 선택할 수 있다.
- OVN 장애가 다른 데이터면으로 숨겨지지 않고 tenant 경계가 fail-closed로 유지된다.
- Linux와 OVN VPC는 한 호스트에 공존할 수 있지만 CIDR은 계속 전역 비중첩이다.
- VPC manager는 하나의 논리적 single writer를 유지하되 Linux/OVN/nft enforcement adapter를
  조정한다.
- host edge transit pool과 OVN resource ownership/reconcile이 새 운영 책임이 된다.
- 기존 Linux VPC를 OVN으로 옮기는 작업은 별도 ADR과 중단·rollback 계획이 필요하다.

## Rejected alternatives

- subnet별 backend 혼합: routing·policy·cleanup의 단일 소유권을 깨므로 기각한다.
- `backend=ovs`: VPC L3/DHCP/ACL intent를 다시 별도 조립해야 하므로 기각한다.
- OVN 장애 시 Linux 자동 fallback: 동일 identity의 중복 actual resource와 정책 우회를 만들므로
  기각한다.
- 기존 VPC 자동 변환: VM XML·lease·conntrack·publish 중단을 암묵적으로 발생시키므로 기각한다.
- 최초 단계부터 OVN native external gateway로 Service Publish 대체: 기존 public 계약을 보존하지
  못하므로 후속 결정으로 미룬다.

## Verification

- schema v1→v2 migration 뒤 기존 VPC packet path와 actual resource가 변하지 않아야 한다.
- backend 생략과 명시 Linux가 동등하고 backend 변경·혼합이 actual mutation 전에 거부돼야 한다.
- OVN LS/LR/DHCP/LSP/Port Group/ACL, host edge nft와 실제 KVM packet 효과가 desired revision과
  일치해야 한다.
- OVN·libvirt·nft 단계별 실패와 daemon/controller/host restart에서 deny가 allow보다 먼저
  적용돼야 한다.
- generic OVN RPC는 VPC-owned row를 변경하지 못하고 cleanup은 foreign row를 보존해야 한다.
- 자동 fallback·ownership guard·quarantine을 제거하면 대응 반사실 테스트가 RED여야 한다.

후보 구현은 schema v1→v2 보존, `linux|ovn` 생성 계약, backend readiness와
RFC 6598 `/30` capacity, OVN LS/LR/DHCP/LSP/Port Group/ACL, Open vSwitch inactive VM XML,
host edge nft, generic `ovn.*` 소유권 거부와 실패 보상을 자동 검증했다. 같은 후보를
검증 환경에서 제품 API로 OVN VPC·첫 subnet·attachment를 만들고, 실제 LSP/chassis에
연결한 격리 namespace에서 gateway·외부 NAT·`isolated` 차단·SG·Service Publish·관리 IP 차단,
daemon restart 수렴과 완전 cleanup을 확인했다.

후보 검증은 Linux VPC 검증과 별도로 수행했으며 Linux/OVN 공존 증거로 사용하지 않는다.

다만 실제 부팅 KVM 게스트의 DHCP/L2·L3, Linux/OVN VPC 동시 공존 패킷, host/OVN controller
reboot와 NB/SB/libvirt/nft 전 단계 fault injection은 남아 있다. 따라서 ADR lifecycle은
`Implemented`이며 `Verified`나 공개 지원으로 승격하지 않는다.
