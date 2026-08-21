# ADR-0033: 브리지 NIC 게스트 MTU는 attach 시점 브리지 실측값을 XML에 항상 명시한다

날짜: 2026-08-06
상태: Verified
Single Edge 적용 상태: 활성

## 맥락

`network.create`는 점보 MTU(최대 9216)를 브리지에는 적용하지만, NIC XML에는
`<mtu>`가 없어 게스트 virtio-net이 QEMU 기본값 1500에 고정됐다. 점보 네트워크를
구성해도 게스트만 1500으로 남는 host/guest MTU 불일치가 발생한다. 이 결함은
KVM 기법 도입 완전성 검수(N8)가 지목했다.

NIC XML을 발행하는 경로는 셋이다.

1. VM 정의 XML — `vm_manager.c`의 `_build_bridge_iface_xml()`
2. `vm.start` 라이브 attach — `handler_vm_start.c`의 bridge interface 조립
3. 사용자 핫플러그 — `handler_vm_hotplug.c`의 bridge interface 조립

셋 다 브리지 실측 MTU를 조회하지 않았고, 게스트에 전달할 MTU 값을 결정하는
단일 판정 소스도 없었다.

## 결정

### 1. 판정 소스는 sysfs 단일 소스

브리지의 실측 MTU는 `/sys/class/net/<bridge>/mtu`에서만 읽는다. network 레코드에
저장된 MTU 값은 판정에 쓰지 않는다 — 저장값과 실측값이 어긋나며 만드는 드리프트를
이원화 자체를 없애 원천 차단한다.

### 2. 3경로 모두 `<mtu size='N'/>`를 항상 명시

정의 XML, `vm.start` 라이브 attach, 사용자 핫플러그 세 경로 모두 attach/정의
시점에 브리지 실측 MTU를 조회해 `<mtu size='N'/>`를 발행한다. 값이 QEMU
기본값과 같은 1500이어도 **생략하지 않고 항상 명시**한다. 유효 대역은
68–9216으로, `network.create`의 검증 대역과 동일하게 맞춘다.

명시 근거는 N13(vhost 드라이버 명시)과 동형이다 — 기본값 위임("1500 가정")을
계약 밖에 남기지 않고, 드리프트를 XML에서 가시화한다. virtio-net의
`host_mtu` 피처를 쓰면 게스트가 DHCP 옵션 없이도 링크 MTU를 받는다
(libvirt formatdomain `<mtu>`, 3.1.0+).

### 3. 에러 처리 — 명시적 fail-open

sysfs 부재, 파싱 실패, 유효 대역(68–9216) 밖인 경우 `<mtu>`를 생략하고
`g_warning`을 남긴 뒤 attach는 계속 진행한다.

- MTU는 정합/성능 속성이지 보안 속성이 아니다 — N13(vhost)식 fail-closed 하드
  실패는 과잉이다.
- 브리지 자체가 없으면 attach는 어차피 libvirt에서 실패한다 — 리더가 선제
  하드 실패할 이유가 없다.

attach 후 브리지 MTU가 바뀌어도 기존 NIC은 attach 시점 계약을 유지한다.
브리지 MTU는 network 라이프사이클에서 생성 시 고정되므로 실질 드리프트
창은 없다.

### 4. 배제한 대안

- **점보일 때만 명시**: 1500 기본값을 가정하는 부분이 계약 밖에 남아, 브리지
  MTU가 나중에 바뀌는 경우를 XML만으로 구분할 수 없다.
- **VM별 opt-in `mtu` 파라미터**: RPC API 확장이 필요하고, 점보 네트워크에서도
  사용자가 매번 수동으로 지정해야 하는 구조가 남는다.

### 5. 범위 밖

- **dpdk(vhostuser)**: MTU는 OVS-DPDK 포트 설정의 별도 경로(`dpdk_manager`)를
  거친다 — XML `<mtu>` 대상이 아니다.
- **SR-IOV(hostdev)**: XML `<mtu>` 자체를 libvirt가 지원하지 않는다. VF MTU는
  호스트 `ip link` 경로로 별도 설정한다. 도입 시 별건 설계가 필요하다.
- **레거시 VM**: 재정의 시점에 새 정의 XML로 수렴한다 — N2~N4·N13과 동일
  정책("신규 VM부터"). 실제로 2026-08-06 가동 VM 5대 재정의로 반영 완료했다.

## 결과

- 점보 브리지 구성에서 게스트가 host_mtu를 DHCP 옵션 없이 정확히 전달받는다.
- 저장값·실측값 이원화가 만드는 드리프트를 구조적으로 없앴다.
- 레거시 VM 5대는 2026-08-06 재정의로 계약에 수렴했고, 게스트 측에서
  `host_mtu=1500`이 디바이스 인자로 명시 전달되는 것을 실측했다.
- 2026-08-11 MTU 9000 격리 bridge에서 정의 XML, `vm.start(bridge_name)` 라이브
  attach, 사용자 핫플러그 세 경로를 실제 KVM으로 통과시켰다. libvirt live/config
  XML과 QEMU `info qtree`가 모든 대상 NIC의 `host_mtu=9000`을 독립적으로 보였다.

## 검증

- 리더(`pcv_bridge_mtu_read`) 단위 테스트: 정상값 / 파일 부재 / 쓰레기값 /
  대역 밖(67·9217) — tmpdir 픽스처로 sysfs 루트를 주입한다.
- 빌더 테스트: `mtu>0` 발행 / `0` 생략 / 기존 virtualport·vlan·vhost 조합
  회귀 — `tests/test_vm_config.c`의 extern forward-decl 관례.
- 게이트: 2026-08-11 현재 `make test` 1300/1300 + audit startup 5/5,
  `make check-all` 39게이트 GREEN.
- 레거시 VM 재정의 검증(2026-08-06)에서 QEMU 인자 `host_mtu:1500` 전달과
  게스트 `enp1s0 … mtu 1500`을 실측했다. 이 값 자체는 QEMU 기본값과 같아
  계약 성립의 단독 증거는 아니며, 증거는 `host_mtu`가 디바이스 인자로
  **명시 전달**됐다는 사실이다.
- 점보 구분 실증(2026-08-11): `pcvn8smk` bridge의 sysfs MTU 9000에서 일회성
  `pcvn8smkvm`·`pcvn8hpvm`을 사용했다. 정의 경로는 inactive XML 1/1,
  시작 attach는 live XML 2/2, 사용자 핫플러그는 live/config XML 2/2가 각각
  `<mtu size="9000"/>`를 보유했다. QEMU qtree에서도 각 NIC의
  `host_mtu=9000`을 확인했다.
- 시작 attach의 선재 `tx_queue_size=1024`가 QEMU 10.2.1의 TX ring 고정값 256과
  충돌해 첫 실증을 막았으며, 실패를 먼저 재현한 뒤 TX만 256으로
  수정하고 같은 요청을 성공시켰다. MTU 계약 밖에서 발견한 Q35 root-port 예산과
  동기 RPC audit 문제는 `N8-F1`·`AUDIT-F2`로 분리해 추적한다.

## 관련

- 본 ADR의 결정·구현·검증 계약
- `src/modules/network/network_manager.c`의 `pcv_bridge_mtu_read`
- `src/modules/virt/vm_manager.c`의 `_build_bridge_iface_xml`
- `handler_vm_start.c`, `handler_vm_hotplug.c`
- `tests/`의 MTU 계약 및 세 경로 회귀 테스트
