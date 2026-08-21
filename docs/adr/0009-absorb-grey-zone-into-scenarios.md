# ADR-0009: 회색지대 22 RPC를 시나리오 S1/S5/S7에 분류 흡수한다

날짜: 2026-04-10
상태: accepted

## 맥락
ADR-0005는 "회색지대 ~52건은 6개월간 사용 메트릭 수집 후 결정"이라고
했으나, 상세 재분류(2026-04-10) 결과 실제 회색지대는 22건으로 축소됨.
(나머지는 S1~S16에 이미 자연스럽게 매핑됨.)

22건을 분석하면 4개 그룹이 보임:

**그룹 1 — 내부 인프라 (7건)**: jobs.*, webhook.dlq.*, vm.event.webhook.list
이들은 사용자가 직접 호출하기보다 fire-and-forget 워커와 알림 전송 체인의
*가시성 창구*다. S7(운영 가시성)의 "알림 전송 인프라"로 분류 가능.

**그룹 2 — AI/자가치유 설정 (3건)**: agent.config.get/set, agent.history
S7에 이미 ai.healing.approve/reject가 매핑됨. agent 설정은 그 전제조건.

**그룹 3 — VM 런타임 튜닝 (7건)**: vm.blkio.get/set, vm.pin_vcpu,
vm.set_bandwidth, vm.usb.attach/detach/list
S1(VM 대량 생성)의 "생성 후 런타임 조정". 별도 시나리오를 만들 만큼의
독립적 운영 가치는 없지만, S1의 §통과조건에 "런타임 튜닝 후 재부팅 없이
적용 확인"을 한 줄 추가하면 정당화됨.

**그룹 4 — VM Guest Agent (3건)**: vm.guest.exec/ping/shutdown
운영 자동화의 핵심이지만 보안 표면이 큼(exec). S7의 "가시성"보다는
"운영 자동화"에 가까움. 별도 시나리오까지는 불필요 — S1에 "Guest Agent
연동" 단락 추가로 충분.

**나머지 (2건)**: network.dhcp_toggle
S1(VM 네트워크 환경 구성)의 부분. 생성 시 브릿지에 DHCP on/off.

## 결정
ADR-0005의 "6개월 메트릭 수집" 경로를 회색지대 22건에 대해 조기 종료하고,
다음과 같이 시나리오에 흡수:

| RPC | 흡수 대상 | 추가되는 통과 조건 |
|---|---|---|
| jobs.cancel/get/list/persist.list (4) | **S7** | `jobs.list` 호출 시 fire-and-forget 작업 상태 조회 PASS |
| webhook.dlq.list/retry (2) | **S7** | DLQ 재시도 후 웹훅 재전송 확인 |
| vm.event.webhook.list (1) | **S7** | 이벤트 웹훅 목록 조회 PASS |
| agent.config.get/set (2) | **S7** | AI Agent 설정 ADMIN만 변경 가능 |
| agent.history (1) | **S7** | 합의 이력 30일 조회 PASS |
| vm.blkio.get/set (2) | **S1** | I/O 제한 적용 후 재부팅 없이 반영 확인 |
| vm.pin_vcpu (1) | **S1** | vCPU 피닝 후 NUMA 노드 확인 |
| vm.set_bandwidth (1) | **S1** | tc qdisc 적용 확인 |
| vm.usb.attach/detach/list (3) | **S1** | USB 핫플러그 attach→list→detach 라운드트립 |
| vm.guest.exec/ping/shutdown (3) | **S1** | guest-ping 응답 + guest-shutdown graceful 확인 |
| network.dhcp_toggle (1) | **S1** | 브릿지 DHCP on/off 전환 후 dnsmasq 상태 확인 |

**vm.guest.exec 보안 조건** (N4 적용):
- RBAC ADMIN만 허용 (OPERATOR/VIEWER 거부)
- 실행 명령 감사 로그 기록 (명령 전문 포함)
- 타임아웃 30초 강제
- 이 조건이 충족되지 않으면 vm.guest.exec는 다시 격리 대상

ADR-0005의 상태는 그대로 accepted 유지 — 본 ADR이 다루는 22건은 "조기
결정"이며, ADR-0005의 원칙(데이터 기반 결정)은 ADR-0008이 편입시킨 7건에
여전히 적용됨.

## 결과
- 좋음: 고아 22건 → 자산 전환, 총 매핑률 250 중 204 (82%)
- 좋음: ADR-0005의 6개월 대기 비용 절감 (22건분)
- 좋음: 각 RPC에 검증 가능한 통과 조건이 DESIGN.md에 박제됨
- 나쁨: S1과 S7이 비대해짐 (각각 +7, +10 RPC)
- 나쁨: vm.guest.exec의 보안 조건은 별도 관리 부담
- 포기한 것: 데이터 기반 사후 결정의 엄밀함 (22건에 한해 직관 적용)

## 하지 않기로 한 것
- vm.guest.exec를 무조건 흡수. 보안 조건 3개가 충족되지 않으면 격리 회귀.
- jobs.*를 외부 API에서 숨기기. 사용자가 fire-and-forget 작업을 추적할
  수단이 필요하며, jobs.list는 그 유일한 창구.
- vm.usb를 S14(GPU)에 묶기. USB와 GPU는 별개 하드웨어 패스스루이며
  S14의 범위 확장을 초래.
- 별도 시나리오 S17 "VM 런타임 튜닝" 신설. 7건으로 시나리오를 만들면
  문서 비용이 기능 가치를 초과. S1의 부분으로 충분.
