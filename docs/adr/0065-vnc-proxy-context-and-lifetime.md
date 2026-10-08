# ADR-0065: VNC 프록시는 REST 컨텍스트에서 콜백을 분리한 뒤 해제한다

- **상태:** Implemented
- **일자:** 2026-10-08
- **적용:** 공개 Single Edge 소스. 구현 반영이며 전체 환경 인증을 뜻하지 않는다.

## 결정

TCP watch를 WS 생성 thread-default context에 명시적으로 attach한다. VncProxy는
GSource 포인터를 소유하며 종료 때 destroy/unref한다. 글로벌 source ID 제거에 의존하지 않는다.
종료는 WS message/closed signal 분리 → watch 제거 → channel/FD/WS 참조/프록시 해제 순서다.
TCP 종료가 WS close를 요청할 때 임시 WS 참조를 보유하고 cleanup을 먼저 수행한다.
closing bool은 살아 있는 객체의 재진입 상태이며 해제 뒤 안전장치로 사용하지 않는다.


## 검증 경계

[최신 공개 인계](../operations/2026-10-08-public-source-refresh-handoff.md)를 따른다. 독립 리뷰·장비별 인수와
전체 감사 미완료 상태는 별도다.
