# ADR-0011: 클러스터 I/O 펜싱 — etcd 기반 zvol 소유권 잠금

날짜: 2026-04-10
상태: accepted

## 맥락
3노드 HA 클러스터에서 네트워크 분할 시 이전 리더와 새 리더가 동시에
같은 ZFS zvol에 쓰기를 시도할 수 있다. 현재 방어:
- etcd lease TTL 10초 + keepalive 3초 루프
- keepalive 실패 시 is_leader 즉시 리셋 (ADR-0001 보완, N6 패치)
- split-brain cross-check (cluster_manager.c)

그러나 네트워크 분할 시 리더가 etcd에는 접근 가능하고 팔로워만 고립된 경우,
팔로워가 TTL 만료 후 캠페인에 성공하면 최대 7초(TTL-keepalive) 동안
두 노드가 동시에 zvol에 접근할 수 있다. STONITH(IPMI 펜싱)는 선택적이라
하드웨어 미지원 환경에서는 보호가 없다.

코드 리뷰(2026-04-10)에서 CRITICAL #6으로 식별.

## 결정
etcd 기반 zvol 소유권 잠금을 도입한다:
1. 리더는 etcd 키 `/purecvisor/zvol_owner/{pool}` 에 자기 node_name을 CAS(Compare-And-Swap)로 기록
2. zvol 쓰기(vm.create, snapshot, replication) 전에 소유권 키 확인
3. 소유권 키의 lease를 리더 lease와 동일하게 바인딩 — lease 만료 시 자동 삭제
4. 팔로워가 페일오버 시 CAS로 소유권 획득 — 이전 값이 자기가 아니면 실패
5. STONITH는 여전히 선택적이나, 미설정 시 로그 경고 + /health에 `fencing: none` 표시

## 결과
- 좋음: IPMI 없는 환경에서도 논리적 펜싱으로 동시 쓰기 방지
- 좋음: etcd 의존이므로 추가 하드웨어 불필요
- 나쁨: etcd 자체가 분할되면 양쪽 모두 쓰기 불가 (안전 쪽 실패)
- 나쁨: zvol 쓰기 경로마다 소유권 확인 오버헤드 (+1 etcd GET)
- 포기한 것: STONITH 필수화 — IPMI 없는 환경에서 HA 사용 불가가 됨

## 하지 않기로 한 것
- SCSI reservation 기반 펜싱을 구현하지 않는다. ZFS zvol은 로컬 디스크이므로 해당 없다.
- STONITH를 필수로 강제하지 않는다. 개발/테스트 환경에서 IPMI가 없는 경우가 대부분이다.
