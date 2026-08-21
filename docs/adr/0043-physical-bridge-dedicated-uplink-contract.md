# ADR-0043: 물리 브리지는 전용 L2 업링크로만 관리한다

날짜: 2026-08-13
상태: Implemented
Single Edge 적용 상태: 2026-08-13 로컬 구현·회귀 검증·운영 배포 완료. 격리 dedicated NIC와
host reboot 검증 전 `Verified` 승격은 보류한다.
관계: ADR-0008, ADR-0033, ADR-0040, ADR-0041, ADR-0044

## 맥락

Linux에서 호스트 IP가 설정된 물리 NIC를 bridge의 slave로만 바꾸면 L3 주소와 route가
bridge로 자동 이동하지 않는다. 기존 `network.create(mode=bridge)`와
`network.bind_phys`는 이 단계를 수행하지 않고 rollback도 제공하지 않아 관리 NIC 선택 시
호스트 연결을 끊을 수 있었다. 물리 bridge 정보는 `/var/run` meta에만 있어 재부팅 뒤
desired state도 사라졌다.

관리 NIC를 안전하게 bridge로 바꾸려면 사용 중인 network renderer의 원자 apply/rollback,
IP·route·DNS 이동, out-of-band 복구 수단이 함께 필요하다. 이는 단일 RPC의 `ip link`
호출로 일반화할 수 없고 ADR-0008의 선언적 설정 우선 결정과도 맞지 않는다.

## 결정

1. Single Edge의 관리형 physical bridge는 호스트 L3가 없는 전용 Ethernet 업링크만 허용한다.
2. 서버는 IP 주소, IPv4/IPv6 기본 경로, 기존 master, loopback, wireless, bond, VLAN,
   bridge 장치 또는 판정 불능 NIC를 mutation 전에 거부한다.
3. 호출자는 `safety_ack="dedicated-uplink"`를 명시해야 한다. UI 확인만 신뢰하지 않고
   RPC validator가 강제한다.
4. bridge 모드는 호스트 CIDR을 받지 않는다. guest는 upstream L2의 DHCP 또는 정적 주소를
   사용한다.
5. 생성은 bridge create, NIC bind, desired-state commit을 하나의 보상 가능한
   트랜잭션으로 구현한다. 중간 실패는 역순 rollback한다.
6. desired state는 `/var/lib/purecvisor/networks`에 원자·비휘발 저장하고 부팅 시
   fail-closed reconcile한다. `/var/run` meta는 조회 캐시로만 사용한다.
7. `network.bind_phys`의 부분 mutation은 거부하고 안전한 `network.create` 계약으로
   수렴시킨다.
8. host uplink가 붙은 bridge의 `network.mode_set`은 live L3/firewall 부분 변경 전에
   거부하고 DHCP 활성화도 차단한다. 일반 삭제도 unmanaged host uplink가 있으면 mutation
   전에 거부하며, 관리형 physical bridge는 desired-state 삭제 경로로만 철거한다.
9. 관리 IP·route·DNS를 Linux bridge로 옮기는 기능은 Web UI/RPC에서 지원하지 않는다.
   OS의 선언적 네트워크 설정과 maintenance window를 사용한다. 물리 NIC의 host L3를
   그대로 유지하는 별도 shared 데이터면은 ADR-0044가 소유하며 이 dedicated 경로를
   우회하거나 완화하지 않는다.
10. 기존 NAT/isolated/routed, Local VPC, OVS의 소유권과 동작은 변경하지 않는다.

## 대안

- **관리 NIC의 IP/route를 자동 이동**: renderer·DNS·policy routing·원격 연결 rollback을
  일반화할 수 없어 기각.
- **경고만 표시하고 계속 허용**: 실수 한 번이 호스트 접근 상실로 이어져 기각.
- **재부팅 시 항상 강제 재바인딩**: 현장 drift가 management NIC를 위험하게 만들 수 있어
  기각. 매 부팅 안전 조건을 재검증한다.
- **런타임 meta만 유지**: 재부팅 desired state를 표현할 수 없어 기각.
- **physical bridge 기능 전면 삭제**: 전용 업링크의 유효한 L2 passthrough 수요가 있어
  안전 계약으로 축소한다.

## 결과

- Web UI에서 management NIC를 선택해 호스트를 끊는 경로가 서버 측에서 차단된다.
- 물리 브리지의 의미가 “호스트 IP 브리지”가 아니라 “guest용 전용 L2 업링크”로 명확해진다.
- 재부팅 뒤 자동 복구가 가능하지만 현장 상태가 바뀌면 가용성보다 호스트 안전을 우선한다.
- standalone bind, live mode 변경, generic delete로 안전 계약을 우회할 수 없다.
- Linux bridge로 관리 L3를 이동하는 작업은 별도 OS 운영 절차가 필요하다. 제품이 제공할
  shared NIC 모드는 ADR-0044의 별도 데이터면과 명시적 `uplink_mode`로만 노출한다.

## 구현·운영 상태

전용 업링크 guard, 트랜잭션 rollback, 비휘발 desired state, 부팅 reconcile과 우회 차단은
2026-08-13 Single Edge 노드에 배포됐다. 이후 ADR-0044 구현도 이 dedicated 계약을 그대로
유지한 채 별도 shared controller로 추가됐다. 현재 남은 승격 게이트는 관리 경로와 분리된
실제 dedicated Ethernet NIC의 create·guest L2·delete와 host reboot 재수렴이다.

최신 통합 상태와 shared 운영 계약은 ADR-0044를 우선한다.
