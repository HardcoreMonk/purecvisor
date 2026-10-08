# ADR-0063: VM 시작에 제한된 hugepage 용량 사전 검사를 연동한다

- **상태:** Implemented
- **일자:** 2026-10-08
- **적용:** 공개 Single Edge 소스. 구현 반영이며 전체 환경 인증을 뜻하지 않는다.

## 결정

vm.start inactive 경로에서 NUMA 정의 후 실제 XML과 2MiB counter를 읽는다. 지원 범위의
부족·관측 실패는 DPDK 준비 전에 차단하고, 준비 이후 재조회하여 QEMU 시작 전에 다시
확인한다. active VM에는 재입장 검사를 적용하지 않는다. 메모리 점유를 이미 반영한 counter를
사용하며 OVS 설정값을 중복 차감하지 않는다.


## 검증 경계

[최신 공개 인계](../operations/2026-10-08-public-source-refresh-handoff.md)를 따른다. 독립 리뷰·장비별 인수와
전체 감사 미완료 상태는 별도다.
