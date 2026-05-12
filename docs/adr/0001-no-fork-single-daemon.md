# ADR-0001: 단일 프로세스 + GMainLoop, fork 금지

날짜: 2026-04-10 (사후 기록 — 결정 자체는 프로젝트 초기)
상태: accepted

## 맥락
PureCVisor는 KVM 하이퍼바이저 오케스트레이터로, libvirt/nftables/ZFS/LXC를
다루는 다수의 RPC를 동시에 처리해야 한다. 전통적인 데몬 설계에는 두 갈래가
있었다:
1. fork-per-request 또는 worker process pool
2. 단일 프로세스 + 이벤트 루프 + 스레드 풀

libvirt 커넥션, etcd 클라이언트, SQLite WAL 핸들, io_uring ring 등 상태가
무거운 자원이 많고, 이들을 프로세스 간에 공유하는 비용이 fork 모델의 이점을
상쇄한다. 또한 GLib 생태계(json-glib, gio, libsoup3)가 GMainLoop를 전제로
한다.

## 결정
현재 Single Edge 데몬(`purecvisorsd`)은 단일 프로세스로 동작한다. fork 금지. 동시성은 GMainLoop +
GTask thread pool + io_uring으로 해결한다. 장시간 작업은 fire-and-forget
패턴(응답 즉시 반환 → GTask로 백그라운드 실행)으로 처리한다.

## 결과
- 좋음: 커넥션 풀/캐시/메트릭이 단일 주소공간에서 락-프리에 가깝게 공유됨
- 좋음: systemd socket activation, sd_notify, watchdog 통합이 단순
- 좋음: 디버깅/프로파일링 대상이 1개 PID
- 나쁨: 한 모듈의 크래시가 전체 데몬을 죽임 → libvirt degraded 모드, 서킷
  브레이커, graceful drain 같은 보호장치를 별도로 구축해야 했음
- 나쁨: 핸들러 작성자가 "절대 블로킹 금지"를 항상 의식해야 함
- 포기한 것: fork 기반 격리. 대신 seccomp/권한 드롭/cap 제한으로 대체

## 하지 않기로 한 것
- worker process를 도입해서 RPC를 격리하지 않는다. 매력적으로 보이지만
  libvirt/etcd/SQLite 핸들 공유 비용이 이득을 넘는다.
- 핸들러 콜백 안에서 `pure_uds_server_send_response`를 호출하지 않는다.
  소켓이 이미 닫혀 UB가 발생한다 (fire-and-forget 규칙).
