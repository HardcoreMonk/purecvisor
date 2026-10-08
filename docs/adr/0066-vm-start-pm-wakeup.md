# ADR-0066: vm.start에서 게스트 절전을 깨우고 실제 실행 상태를 확인한다

- **상태:** Implemented
- **일자:** 2026-10-08
- **적용:** 공개 Single Edge 소스. 구현 반영이며 전체 환경 인증을 뜻하지 않는다.

## 결정

virDomainIsActive는 guest 절전도 active로 판정한다. 기존 vm.start는 이 경우 아무것도
깨우지 않고 성공을 기록한다. active DPDK reconcile 뒤 상태를 조회하고 PMSUSPENDED만
virDomainPMWakeup(flags=0)으로 한 번 깨운다. 최대 2초 동안 50ms 간격으로 RUNNING을
확인한다. 조회·깨우기 실패와 미복귀는 기존 callback의 audit/WS 실패 결과로 전달한다.

active는 CPU 재할당을 건너뛰며, callback은 요청의 할당 목록이 없는 경우 기존 예약을
반납하지 않는다. inactive의 capacity·DPDK·기동 경로와 CPU 실패 반납은 유지한다.
PAUSED의 vm.resume, 강제 종료·재생성, 게스트 전원 정책과 UI 계약은 변경하지 않는다.


## 검증 경계

[최신 공개 인계](../operations/2026-10-08-public-source-refresh-handoff.md)를 따른다. 독립 리뷰·장비별 인수와
전체 감사 미완료 상태는 별도다.
