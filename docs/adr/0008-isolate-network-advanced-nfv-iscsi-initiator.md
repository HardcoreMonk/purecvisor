# ADR-0008: 네트워크 고급/NFV/iSCSI initiator 8 RPC를 격리한다

날짜: 2026-04-10
상태: accepted

## 맥락
코드 감사(2026-04-10)에서 다음 8개 RPC가 어떤 시나리오에도 매핑 불가:

**네트워크 고급 (5)**:
- network.qos.get / set / remove — per-bridge QoS (tc qdisc)
- network.bind_phys — 물리 NIC를 브릿지에 바인딩
- network.mode_set — 브릿지 모드 전환 (nat/bridge/isolated)

**NFV (1)**:
- nfv.lb.create — OVN 기반 L4 로드밸런서

**iSCSI initiator (2)**:
- iscsi.connect — 외부 iSCSI 타겟에 연결
- iscsi.disconnect — 동상 해제

각 비매핑 이유:
- **network.qos**: S1은 VM 생성/라이프사이클이며 "네트워크 QoS 튜닝"은
  어떤 운영 시나리오도 요구하지 않음. vm.set_bandwidth와 container.
  set_bandwidth가 이미 per-VM QoS를 제공하며 S1에 매핑됨. 브릿지 레벨
  QoS는 중복.
- **network.bind_phys / mode_set**: 초기 셋업에서 한 번 사용 후 변경 없음.
  daemon.conf로 선언적 설정이 더 안전 → S11(설정관리)이 이미 담당.
- **nfv.lb.create**: OVN에 의존. ADR-0002(OVN 격리)와 운명을 같이함.
- **iscsi.connect / disconnect**: ADR-0006이 "이니시에이터로서의 PureCVisor
  는 별개"로 명시. S15(iSCSI 타겟)는 *노출*만 다룸.

## 결정
1. `nfv.lb.create`는 ADR-0002의 `-DPCV_OVN_ENABLED` 가드에 함께 포함
   (OVN 의존이므로 별도 플래그 불필요)
2. 나머지 7건은 즉시 격리하지 않고 **ADR-0005 회색지대 메트릭 수집**에
   편입. 단, 6개월 후 호출 0건이면 코드 삭제 (deprecated 윈도우 없이).
3. network.bind_phys / mode_set 기능은 daemon.conf `[network]` 섹션의
   선언적 설정으로 대체를 권장하는 deprecation 경고를 응답에 추가.

nfv.lb.create를 별도 플래그로 분리하지 않는 이유:
OVN 없이는 동작 불가한 코드. #if PCV_OVN_ENABLED 안에 자연스럽게 들어감.

## 결과
- 좋음: nfv.lb.create가 ADR-0002에 편승 → 추가 플래그 0
- 좋음: 나머지 7건은 데이터 기반 결정 경로(ADR-0005)에 합류
- 나쁨: 7건이 6개월간 코드에 잔류
- 포기한 것: 즉시 삭제의 단순함

## 하지 않기로 한 것
- network.qos를 S1에 흡수. per-VM QoS(vm.set_bandwidth)가 이미 있어 중복.
  브릿지 레벨 QoS의 운영 가치가 데이터로 증명되면 재검토.
- iSCSI initiator를 S15에 흡수. 타겟 노출과 이니시에이터 연결은 완전히
  다른 보안 표면이며 운영 책임도 다르다.
- 별도 컴파일 플래그 도입. 8건에 플래그를 만들면 플래그 인플레이션.
