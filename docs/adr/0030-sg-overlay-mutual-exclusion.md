# ADR-0030: SG × tenant-overlay 상호 배타

날짜: 2026-07-07
상태: accepted

## 맥락
SG nft 규칙은 root netns 의 vnet 에 바인딩되는데(vm_vnet_cache 해석)
tenant-overlay VM 의 tap 은 ep netns 안에 있다 — 바인딩이 성립해도 root
netns 훅은 오버레이 트래픽을 구조적으로 볼 수 없다. 의미 없는 SG 바인딩이
조용히 성립(의미론적 fail-open)하고 virt_events sg_sync 가 spurious 실패를
반복한다. M-4 로 식별, 2026-07-07 사용자 확정 (integrated-decisions 결정 1).

## 결정
1. 상호 배타 강제: overlay 멤버 VM 의 SG 바인딩 거부(fail-closed·명시
   에러), SG 바인딩 VM 의 overlay attach 거부(역방향, PCV_TOVL_ERR_SG_BOUND).
2. 부팅 감사: SG restore + overlay rehydrate 완료 후 위반 바인딩 WARN +
   audit + 제거. virt_events 기동 전 실행(spurious sync 원천 소멸). audit
   레코드는 detach 실제 결과를 그대로 반영한다("removed"/"detach_failed")
   — detach 실패를 "removed"로 뭉개면 audit DB 와 운영 로그가 모순되므로
   낙관적 기록을 피한다.
3. 제품 스탠스: "암호 격리 = tenant-overlay, per-VM L3/L4 정책 = SG(bridge
   모드)" — 에러 메시지와 문서에 명시.

## 결과
- 좋음: 가짜 보안(무의미 바인딩) 제거, spurious 이벤트 소멸.
- 나쁨: overlay + per-VM 정책 동시 적용 불가 — 수요 발생 시 "SG-in-netns"
  별도 에픽(트리거 게이트).

## 하지 않기로 한 것
- ep netns 안 SG 재구현을 지금 하지 않는다(수요 미실증).
- 동시 bind↔attach 미세 경합 창의 락 통합을 하지 않는다 — 부팅 감사가
  보정하고, 락 중첩(교착 위험)이 더 큰 비용.
