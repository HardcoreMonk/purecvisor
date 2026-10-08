# ADR-0062: DPDK·hugepage 수용량을 읽기 전용 스냅샷으로 진단한다

- **상태:** Implemented
- **일자:** 2026-10-08
- **적용:** 공개 Single Edge 소스. 구현 반영이며 전체 환경 인증을 뜻하지 않는다.

## 결정

로컬 첫 단계는 명시적 host NUMA·2MiB plan의 읽기 전용 CLI다. 노드별 free에서 전역 예약
최대치를 차감하고 소비자별 올림한 신규 OVS·guest·headroom 요구를 대조한다. 이미 반영된
OVS 메모리를 다시 차감하지 않도록 pending/accounted를 구분하며 이 값이 plan 가정임을
출력한다. 측정 불가·불일치는 부족과 구분하고 어떤 경우에도 성공으로 추정하지 않는다.


## 검증 경계

[최신 공개 인계](../operations/2026-10-08-public-source-refresh-handoff.md)를 따른다. 독립 리뷰·장비별 인수와
전체 감사 미완료 상태는 별도다.
