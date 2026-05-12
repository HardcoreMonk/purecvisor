# ADR-0012: fire-and-forget 비동기 결과 채널 도입 (ADR-0001 보완)

날짜: 2026-04-10
상태: accepted

## 맥락
ADR-0001에서 fire-and-forget 패턴을 확립했다: "응답 즉시 반환 → GTask
백그라운드 실행". 이 패턴은 REST 스레드 비점유에 최적이지만, 클라이언트가
작업의 최종 성공/실패를 알 수 없는 근본적 한계가 있다.

현재 상태:
- vm.delete에만 `vm.delete.status` 폴링 RPC가 존재
- vm.create/start/stop/snapshot 등은 결과 확인 불가
- WebSocket 이벤트는 메트릭 push만 있고 작업 결과 브로드캐스트 없음
- pcv_job_queue.c(SQLite 기반)가 이미 있으나 Cloud Migration에만 사용

코드 리뷰(2026-04-10)에서 MAJOR #15로 식별.

## 결정
Job ID 반환 + WebSocket 푸시 + 폴링 폴백의 3중 채널을 도입한다:

1. **Job ID 반환**: fire-and-forget 핸들러가 `{"result":{"job_id":"uuid","status":"accepted"}}` 반환
2. **WebSocket 푸시**: 작업 완료 시 `{"event":"job.complete","job_id":"uuid","method":"vm.create","status":"ok|failed","error":"..."}` 브로드캐스트
3. **폴링 폴백**: `jobs.status` RPC로 job_id 조회 (기존 pcv_job_queue.c 확장)
4. **적용 대상**: vm.create, vm.delete, vm.snapshot.create, cloud.import 등 Single Edge에서 실제 제공하는 장시간(>2초) 작업. 공개판 범위 밖 RPC는 적용 대상이 아니다.
5. **단기 작업**: vm.start/stop은 현행 fire-and-forget 유지 (libvirt API가 동기)

## 결과
- 좋음: 클라이언트가 작업 결과를 확인할 수 있음 (UI 진행바, CLI 대기)
- 좋음: 기존 pcv_job_queue.c 인프라 재활용
- 좋음: ADR-0001의 비동기 성능 이점은 유지
- 나쁨: 핸들러마다 job 등록 + 완료 갱신 코드 추가 필요
- 나쁨: SQLite job 테이블 쓰기 오버헤드 (건당 ~0.5ms)
- 포기한 것: 전 RPC를 동기로 전환 — ADR-0001 위반, REST 스레드 고갈

## 하지 않기로 한 것
- 모든 RPC에 Job ID를 부여하지 않는다. vm.list 같은 읽기 쿼리에는 불필요.
- gRPC 스트리밍으로 결과를 전달하지 않는다. Single Edge 공개 API의 결과 채널은 Job ID, WebSocket push, polling 조합을 우선한다.
