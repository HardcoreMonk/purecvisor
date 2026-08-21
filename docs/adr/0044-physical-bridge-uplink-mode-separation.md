# ADR-0044: 물리 브리지는 전용 업링크와 공유 업링크를 분리한다

날짜: 2026-08-13
상태: Implemented
Single Edge 적용 상태: 2026-08-14 로컬 구현·배포와 shared 실제 upstream DHCP·host/LAN 통신,
daemon restart 수렴을 완료했다. 실제 KVM VM·host reboot 검증 전 정식 Verified 승격은 보류한다.
관계: ADR-0008, ADR-0033, ADR-0040, ADR-0041, ADR-0043

## 맥락

ADR-0043은 호스트 L3가 없는 물리 NIC만 Linux bridge의 port로 편입하는
`dedicated` 계약을 정의했다. 이 계약은 관리 NIC 단절을 막지만, VMware Workstation처럼
호스트가 사용 중인 NIC를 VM과 동시에 공유하는 요구사항을 제공하지 않는다.

두 요구사항을 같은 `bridge` 문자열과 같은 `ip link set master` 경로로 처리하면 다음
충돌이 다시 발생한다.

- 전용 업링크는 물리 NIC를 bridge port로 소유해야 한다.
- 공유 업링크는 물리 NIC의 호스트 IP·route·DNS와 renderer 소유권을 보존해야 한다.
- Linux bridge migration은 외부 동작은 비슷하지만 호스트 L3의 실제 위치를 바꾸므로
  VMware식 in-place 공유와 같은 계약으로 표시할 수 없다.
- macvtap direct는 물리 NIC를 보존하지만 기본적으로 host↔guest 직접 통신을 제공하지 않는다.

따라서 제품 계약과 데이터면을 명시적으로 분리해야 한다.

## 결정

1. `mode=bridge`는 L2 외부 연결이라는 상위 모드로 유지하고, 물리 NIC 소유 방식은
   `uplink_mode=dedicated|shared`로 분리한다.
2. 기존 `safety_ack=dedicated-uplink` 요청은 `uplink_mode=dedicated`로 해석해 하위 호환을
   유지한다. 새 호출은 `uplink_mode`를 반드시 전송한다.
3. `dedicated`는 ADR-0043을 그대로 따른다. 호스트 L3가 없는 Ethernet NIC만 Linux
   bridge port로 편입하며, 전용 desired state와 rollback/reconcile을 유지한다.
4. `shared`는 물리 NIC를 Linux bridge의 port로 만들지 않고 호스트 IP·route·DNS를
   이동·삭제하지 않는다. 호스트 renderer의 connection/profile도 수정하지 않는다.
5. `shared`의 최초 데이터면은 unnumbered 내부 Linux bridge, veth portal, 물리 NIC의
   TC-BPF ingress/egress filter로 구성한다. VM tap은 내부 bridge에 연결하고 TC-BPF가
   guest MAC에 해당하는 프레임만 물리 NIC와 portal 사이에서 전달한다.
6. 물리 ingress/egress filter의 기본 동작은 항상 host traffic `PASS`다. 구성·map·세대가
   없거나 불일치하면 guest path만 닫고 호스트 네트워크는 통과시킨다.
7. `shared`는 host↔guest, guest↔LAN, guest↔guest를 모두 제공하고 guest는 upstream
   DHCP 또는 정적 설정으로 호스트와 같은 LAN prefix의 독립 주소를 사용한다.
8. 최초 공개 범위는 untagged wired Ethernet, 물리 NIC당 shared bridge 하나다. Wi-Fi,
   VLAN trunk, bond/VLAN upper, EAPOL과 다중 shared bridge는 지원하지 않는다.
9. shared Ethernet은 upstream switch가 여러 source MAC을 허용해야 한다. port security로
   guest MAC이 차단돼도 호스트 경로를 변경하거나 자동으로 NAT에 fallback하지 않는다.
10. 물리 bridge desired state는 schema v2에서 `uplink_mode`, BPF revision, portal identity,
    physical MAC과 소유 filter handle을 기록한다. 부팅 시 실제 ifindex를 다시 해석하고
    PureCVisor가 소유한 객체만 멱등 재수렴한다.
11. generic bind, live mode/DHCP 변경, unmanaged delete로 두 모드의 controller를 우회할
    수 없다. 생성·삭제·VM NIC attach/detach는 uplink controller의 단일 writer를 거친다.
12. 공유 모드 구현 전 TC-BPF packet-path spike가 host↔guest·external↔guest·broadcast,
    DHCP, checksum/GSO/GRO와 host traffic 무중단을 production kernel에서 증명해야 한다.

## 대안

- **호스트 IP를 Linux bridge로 자동 이동**: 표준적이고 기능적으로 유효하지만 renderer
  변경과 L3 migration을 수반한다. `shared`의 in-place 계약으로는 기각하며, 필요하면
  별도 `host-migrated` 운영 모드 ADR로 다룬다.
- **macvtap direct**: guest↔LAN은 간단하지만 host↔guest 기본 단절 때문에 기각한다.
- **AF_PACKET 사용자 공간 브리지**: raw socket capability, packet copy 비용과 데몬
  재시작 데이터면 의존성이 커서 기각한다.
- **전용 커널 모듈**: VMware 구조와 가깝지만 커널 ABI 유지·서명·배포 부담이 Single
  Edge 공개 범위를 크게 넓혀 기각한다.
- **Wi-Fi MAC proxy를 최초 범위에 포함**: 802.11 3-address 제약과 DHCP/ARP/NDP MAC
  translation이 별도 데이터면을 요구하므로 후속 ADR로 분리한다.

## 결과

- `dedicated`의 강한 관리 NIC 거부는 약화되지 않는다.
- `shared`는 호스트 네트워크 구성을 바꾸지 않고 VMware Workstation 유선 bridge의 핵심
  사용자 계약을 제공하도록 설계된다.
- shared 데이터면 장애나 daemon drift는 guest 연결 장애로 한정되고 host L3는 보존된다.
- TC-BPF와 NIC offload 호환성, upstream 다중 MAC 허용 여부가 새 검증·운영 전제가 된다.
- 격리 spike 실패 시 shared라는 이름으로 Linux bridge migration을 대체 구현하지 않는다.

## 구현·운영 상태

shared `pcvbr0` 검증에서 물리 NIC의 주소·route·DNS·MAC·MTU와 master 없음이 생성 전후 및
daemon restart 뒤 동일했고, 임시 guest namespace의 독립 MAC `02:16:3e:44:55:66`은
upstream DHCP에서 문서용 주소를 받아 gateway와 host에 통신했다. 이 MAC은 검증용으로만
사용했으며 namespace와 veth를 함께 제거했다. 따라서 영구 게스트 MAC이나 예약 IP로
해석하면 안 된다.

실제 KVM VM NIC와 host reboot 재수렴 전에는 `Verified`로 승격하지 않는다. 공개 재현 절차는
`docs/GUIDE.md`와 관련 통합 테스트를 따른다.
