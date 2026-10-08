# ADR-0064: Trace는 자식 회수 확인 뒤에만 완료한다

- **상태:** Implemented
- **일자:** 2026-10-08
- **적용:** 공개 Single Edge 소스. 구현 반영이며 전체 환경 인증을 뜻하지 않는다.

## 결정

stop/backstop은 종료 요청만 한다. 동일 trace id·GSubprocess의 wait_finish가 성공한
callback에서 실제 종료 상태를 요약·audit하고 `.running`·단일 trace 가드를 해제한다.
main loop 동기 wait나 GLib 경고 숨김으로 대체하지 않는다. 대기 중 중복 stop은 접수
성공이며 종료 사유는 첫 요청을 유지한다. 회수 실패는 완료로 처리하지 않는다.

기존 RPC stopped bool은 종료 요청 접수 호환 필드다. stop 응답에 stop_requested를
추가하며 status는 기존 running/idle을 유지하고 running의 stop_requested로 대기를
구분한다. 완료 관측은 idle·마커 제거·terminal audit다. 새 RPC·권한·DB 모델은 없다.
terminal audit는 pcv_trace 모듈의 실제 종료 결과다. 기존 dispatcher 봉투 감사는
접수 응답을 기록하는 별도 층으로 유지하며 캡처 완료 증거로 사용하지 않는다.


## 검증 경계

[최신 공개 인계](../operations/2026-10-08-public-source-refresh-handoff.md)를 따른다. 독립 리뷰·장비별 인수와
전체 감사 미완료 상태는 별도다.
