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

## 후속 결정 (2026-10): 관리 대역 보호와 기동 복구

기동 시 전체 desired state를 다시 적용하는 이 ADR의 복구 경로(결정 4)는 위험 subnet이 영속돼 있으면 같은 단절을 되살린다. 그래서 확인 대기 subnet의 기동 원복과
콘솔 플래그 `/etc/purecvisor/vpc-reconcile.skip`에 의한 reconcile 건너뛰기를 더했다. quarantine·fail-closed 원칙은 그대로다.

## 후속 결정 (2026-10): OVN 실패의 폭발 반경

결정 4의 fail-closed는 Linux nft를 적용·확인하지 못할 때의 계약이다. 실측에서 없는 Port_Group의
`--type=port-group acl-add`·`acl-del`이 rc=1 "port group name not found"로 끝나, 생성 중 NB가 끊겨
Port_Group 없이 `DEGRADED`로 남은 OVN VPC 하나가 호스트의 모든 VPC 변경을 막았고, reconcile에서 다른
테넌트의 Linux bridge까지 DOWN시켰으며, 기동 reconcile이면 데몬을 멈췄다. 자기 자신도 API로 지울 수 없었다.
이 반경은 계약에 없던 것이라 다음처럼 좁힌다.

| 실패 | 범위 |
|---|---|
| Linux nft 적용·검증 실패, DB 조회 실패, Linux 설비 수렴 실패 | 그대로 전체 fail-closed: 관리 bridge와 OVN edge 전부 DOWN |
| reconcile의 OVN 실패(NB 장애, 준비 안 됨, foreign 충돌) | 그 VPC만 `DEGRADED` + host edge DOWN. Linux VPC는 수렴을 계속한다. reconcile은 `STATE` 오류로 끝나 기동이 멈추지 않는다 |
| Port_Group ABSENT | 격리는 통과한다. 정책 동기화는 `ACTIVE` attachment가 없을 때만 통과한다. ACL을 걸 곳도 격리할 포트도 없고, 다음 reconcile의 `ensure_vpc`가 만든다 |
| Port_Group ABSENT + `ACTIVE` attachment(외부에서 지움) | VM 포트가 정책 없이 도는 상태라 정책 동기화가 `PORT_GROUP_MISSING`으로 실패한다. 어느 경로든 그 VPC는 `DEGRADED` + host edge DOWN이 된다. 변경 대상이 아닌 VPC면 변경은 계속되고, 그 VPC 자신을 대상으로 한 변경은 실패로 돌아온다. SG 동기화도 같은 결과다. 다음 reconcile의 `ensure_vpc`가 복구하고 edge를 올린다 |
| `DEGRADED`·`CREATING` VPC의 `vpc.delete` | 자기 quarantine을 건너뛴다. 소유 확인과 삭제는 `remove_vpc`가 그대로 하므로 NB 장애면 실패하고 row가 남는다 |
| 변경 API 앞단 격리의 OVN 실패(NB 장애, `--wait=hv` 시간 초과) | API 실패 그대로다. OVN을 nft보다 먼저 걸므로 nft에는 닿지 않는다. 앞서 ACL을 건 OVN VPC와 실패한 VPC 자신을 되돌린다. 해제는 두 방향을 모두 시도한다. 이미 `DEGRADED`인 VPC는 낡은 ACL이 드러나지 않게 되돌리지 않는다 |
| 앞단 격리 뒤 변경 실패(actual 단계·저장소) | API 실패 그대로다. 전체 nft 정책을 다시 걸고 OVN ACL을 푼다. desired를 바꾼 채 실패한 `vpc.egress.set`의 OVN VPC는 이번 해제에서 건너뛴다. 이 격리와 `DEGRADED` VPC의 격리는 다른 VPC의 성공한 변경이나 reconcile이 ACL을 desired로 다시 쓸 때 풀린다. 해제가 실패하면 격리가 남는다(fail-closed) |

앞단 격리는 backend와 상관없이 모든 OVN VPC에 ACL을 건다. 그래서 그것을 건 모든 변경은 Linux VPC 대상이어도 끝날 때
OVN ACL까지 푼다. 해제는 VPC마다 계속 시도하고 첫 오류를 돌려준다.

남는 제약: NB 장애 동안에는 OVN VPC가 있는 호스트의 VPC 변경 API가 Linux VPC 대상이어도 실패한다. 격리를
확인할 수 없는 상태에서 변경하지 않기 위해서다. 응답 없는 NB면 API는 최대 10초 늦게 실패하지만 nft를 건드리지
않으므로 Linux VPC의 트래픽은 끊기지 않고, 조회에서 실패한 VPC의 트래픽도 끊기지 않는다. ovn-controller만 멈춘 경우는 다르다.
2026-11-01 측정(`ovn-nbctl 26.03`, nb_cfg를 따라오지 않는 chassis)에서 `--wait=hv --timeout=10`은 10초 뒤 rc=142(SIGALRM)로 끝나지만 ACL은
이미 커밋돼 있었고, 없는 ACL의 `acl-del`은 rc=0이었다. 그래서 그 VPC와 먼저 ACL을 건 다른 OVN VPC는 되돌릴 때까지 끊기고 API는 ACL당 10초씩 늦어지며,
그 사이 NB가 끊기면 다음 reconcile까지 남을 수 있다. Port_Group 이름이 다른 소유자의 것(FOREIGN)인 VPC가 하나 있으면 앞단 격리가 충돌로 실패해 호스트의 모든
VPC 변경이 막힌다. 이름 충돌은 운영자가 정리해야 하며 제품이 남의 row를 건드리지 않기 위한 제약이다. 외부에서 Port_Group만 지운 활성
VPC는 다음 변경이나 reconcile이 그 상태를 볼 때까지 ACL이 없다. 본 뒤에는 host edge가 내려가고, 다음 reconcile이 복구한다.

검증: `tests/test_vpc_fence.c`.

## Verification

same/cross-VPC, source spoofing, host input, raw bridge mutation, startup restore failure와
Service Publish source/target 경계를 production 정책 compiler와 격리 데이터면에서 검증한다.
각 강제 rule 또는 guard를 제거하면 해당 효과 테스트가 RED가 되는 반사실을 유지한다.
