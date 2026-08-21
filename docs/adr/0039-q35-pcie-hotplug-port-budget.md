# ADR-0039: 제품 Q35 VM은 최소 두 개의 PCIe hotplug 자리를 예약한다

- **상태:** Verified
- **일자:** 2026-08-11
- **Single Edge 적용 상태:** 운영 배포와 라이브 효과 검증 완료
- **관련:** ADR-0033, N8-F1

## Context

libvirt가 자동 생성한 Q35 controller 토폴로지는 제품 기본 장치 뒤 빈 root port가 한 개뿐이다.
`vm.start(bridge_name)`가 그 자리를 쓰면 같은 실행 VM의 `device.nic.attach`가
`No more available PCI slots`로 실패한다. 이는 개별 NIC XML이 아니라 VM 정의 시점의
토폴로지 예산 문제다.

## Decision

1. 제품 Q35 정의 XML에 `pcie-root index='0'`과 `pcie-root-port` controller 8개를 명시한다.
2. 현행 기본 PCIe 소비자 6개 뒤 시작 NIC와 사용자 추가 NIC를 위한 최소 두 자리를 보장한다.
3. 보정은 Q35에만 적용하며, 기존 controller를 보존하고 부족한 수만 추가하는 멱등 변환이다.
4. 기존 VM의 전역 자동 재정의는 하지 않는다. 새 정의와 제품이 수행하는 재정의에서 수렴한다.
5. Q35 host bridge index 0은 제품이 고정하고 root-port index와 address 배치는 libvirt에 맡긴다.

## Consequences

- 새 Q35 VM은 시작 bridge attach와 추가 NIC hotplug를 함께 사용할 수 있다.
- domain XML의 controller 수가 제품 ABI가 되므로 변경은 XML 단위·실 libvirt 효과 테스트를
  요구한다.
- 두 자리를 넘는 무제한 PCIe hotplug를 보장하지 않는다.

## Verification

합성 XML의 수·멱등성·비-Q35 불변, schema validation, 그리고 한 실행 VM에서 시작 NIC 뒤
추가 NIC의 live/config 성공과 재부팅 유지·cleanup을 검증한다.

2026-08-12 Q35/비-Q35 집중 3/3, `virt-xml-validate` schema validation, 전체 C
1330/1330과 39개 계약 게이트를 통과했다. 운영 노드의 임시 제품 VM에서 시작 NIC+추가
NIC, config·MAC·MTU 9000 재시작 유지와 cleanup을 실증하고 배포 identity 검증을 통과해
`Verified`로 승격했다.
