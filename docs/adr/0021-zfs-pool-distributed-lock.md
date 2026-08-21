# ADR-0021 — ZFS pool 분산 락 (Phase 2 — etcd lease 기반)

- **상태**: Accepted / Approved (2026-04-14; 사용자 승인, 2026-04-26)
- **일자**: 2026-04-14
- **관련**: ADR-0011 (zvol I/O 펜싱), ADR-0001 (단일 데몬), BUG-18
- **트리거**: BUG-18 후속 — Phase 1 (intra-node GMutex)이 같은 노드의 동시 ZFS 작업은 막지만, **노드간** 동시 작업 (예: Node1의 manual `zfs create` + Node2의 replication `zfs recv`)은 여전히 OpenZFS pool-level 교착을 유발할 수 있음.

## 컨텍스트

BUG-18에서 발견된 시나리오:
- Node2의 `zfs recv -F pcvpool/vms/img-db-3` (cluster replication, 수 시간 D-state)
- Node2의 `zfs rollback` (cluster manager의 자동 복구)
- Node2의 manual `zfs create` (vm.create RPC)

이 셋이 동시에 Node2 pool에서 경합 → OpenZFS pool txg 락이 deadlock → 모든 ZFS 작업 D-state.

**Phase 1** (`pcv_zfs_lock` GMutex)는 같은 프로세스(같은 노드)의 동시 호출만 직렬화. 다른 노드에서 SSH로 들어오는 `zfs send | ssh node2 zfs recv` 같은 외부 호출은 여전히 동시 진입 가능.

## 결정

**etcd CAS lease lock** 도입. ADR-0011의 zvol owner와 분리된 별도 단기 락:

### 키 스페이스
```
/purecvisor/zvol_owner/<pool>     ADR-0011 — 노드 단위 pool 소유권 (수분~수시간)
/purecvisor/zfs/inflight/<pool>    ADR-0021 — 단일 작업 배타 락 (수십 초)
```

두 키는 **목적과 수명이 다름**:
- **owner**: "이 pool은 node X가 책임진다" — 리더 lease 바인딩, 노드 다운 시 자동 회수
- **inflight**: "이 pool에 지금 작업이 진행 중" — 짧은 TTL (5~300초), 작업 종료 후 즉시 release

### API

```c
// etcd_client.h
gboolean pcv_etcd_acquire_inflight_lock(client, pool, node, op, ttl_sec, error);
gboolean pcv_etcd_release_inflight_lock(client, pool, error);
```

### 통합

`zfs_driver.c::purecvisor_zfs_create_volume()` (그리고 destroy)에서:

```c
1. pcv_zfs_pool_lock(pool, ...)              // Phase 1 intra-node mutex
   ↓ 성공
2. pcv_etcd_acquire_inflight_lock(...)        // Phase 2 inter-node CAS
   ↓ 성공: 작업 실행
   ↓ 실패: 5초 후 1회 재시도 → 그래도 실패하면 best-effort로 작업 진행
   (etcd 다운 시에도 intra-node 락은 보호하므로 graceful degradation)
3. ZFS operation
4. pcv_etcd_release_inflight_lock(...)
5. pcv_zfs_pool_unlock(pool)
```

### TTL 규약

| 작업 | TTL | 이유 |
|------|-----|------|
| `zfs create -V` | 60s | 일반 zvol 생성 (sparse, 즉시 완료) |
| `zfs destroy -r` | 60s | 스냅샷 포함 재귀 삭제 |
| `zfs send/recv` | 300s | replication, 큰 dataset 가능 |
| `zfs rollback -r` | 60s | 스냅샷 롤백 |

크래시 시 etcd가 TTL 후 자동 삭제 → 다른 노드가 인계 가능.

## 근거

- **분산 락 의 정통 패턴**: etcd CAS는 etcd가 분산 합의 알고리즘으로 보장. 단일 키 PUT-IF-ABSENT는 강한 일관성.
- **기존 인프라 재활용**: `pcv_etcd_claim_zvol_owner`와 동일 CAS Txn 패턴. 새 요소 없음.
- **ADR-0011과 보완 관계**: owner는 정책적 책임자, inflight는 동시성 제어. 직교 개념.
- **graceful degradation**: etcd 다운 시 intra-node 락만으로도 80% 시나리오 보호 (Node2 incident가 intra-node였음).

## 대안 검토

### 대안 A — etcd lock 모듈 (`v3lock` API)

etcd v3에는 `Lock`/`Unlock` 전용 API 존재. lease가 만료될 때까지 대기하는 blocking semantics.

**기각 사유**: blocking lock은 daemon main loop를 점유 (BUG-17의 watchdog 크래시 패턴 재현 위험). CAS put-if-absent는 비-blocking + 상한 시간 보장.

### 대안 B — 응용 계층 lease 토큰

자체 구현 토큰 + heartbeat. 노드 다운 감지는 etcd lease의 절반.

**기각 사유**: 바퀴 재발명. etcd lease가 이미 동등 기능 + 검증된 분산 합의.

### 대안 C — Phase 2 보류 (intra-node만)

등록된 BUG-18 시나리오 80%가 intra-node였으므로 Phase 1만으로 충분.

**기각 사유**: production cluster에서 cross-node replication이 운영의 핵심. 보호 누락 시 향후 동일 문제 재발 가능. 현재 구현 비용이 낮음 (etcd_client에 60줄 추가).

## 영향

### 긍정적
- BUG-18의 cross-node 시나리오까지 보호
- ADR-0011의 zvol owner와 직교 개념으로 깔끔한 분리
- etcd 다운 시 graceful degradation (intra-node 보호 유지)

### 부정적
- 모든 zvol create/destroy에 etcd 1 round-trip (~5-50ms latency)
- etcd 부하 증가: zvol op 빈도 × 2 (acquire + release)
- 락 timeout 정책 운영 부담 (60s가 적절한지 모니터링 필요)

### 모니터링

Prometheus exporter에 ADR-0021 관측 메트릭을 추가했다.

- `purecvisor_zfs_inflight_lock_acquired_total{op,result}`:
  `result="ok|busy|error"` 획득 결과 카운터
- `purecvisor_zfs_inflight_lock_wait_ms_bucket{op,result,le}`:
  락 획득 대기 시간 histogram-compatible bucket
- `purecvisor_zfs_inflight_lock_wait_ms_sum{op,result}`
- `purecvisor_zfs_inflight_lock_wait_ms_count{op,result}`

Web UI Storage Monitor는 위 메트릭을 `/api/v1/metrics`에서 파싱해 `ZFS inflight lock` 패널로 표시한다. 패널은 전체 획득 수, `ok|busy|error` 결과 수, 평균 대기 시간, op별 breakdown을 보여준다. 아직 샘플이 없으면 빈 상태를 표시한다.

## 승인 이력

- 2026-04-14: etcd CAS lease 기반 inflight lock을 ADR-0021 구현 기준으로 채택했다.
- 2026-04-26: 사용자 승인 완료. 승인 범위는 같은 노드의 `pcv_zfs_pool_lock()` 필수 보호선, cluster build의 etcd inflight lock 2차 보호선, etcd 실패 시 best-effort 진행, create/destroy 경로 관측 메트릭 유지 규칙이다.

## 적용 이력

| 날짜 | 액션 | 관련 |
|------|------|------|
| 2026-04-14 | Phase 1 구현 (intra-node GMutex) — `pcv_zfs_lock` | BUG-18 (F-1) |
| 2026-04-14 | Phase 2 구현 (etcd CAS lease) — `pcv_etcd_acquire_inflight_lock` | BUG-18 (이 ADR) |
| 2026-04-26 | Prometheus inflight lock 결과/대기 시간 메트릭 추가 | P2 후속 |
| 2026-04-26 | Web UI Storage Monitor의 ZFS inflight lock 패널 추가 | P2 후속 |
| 2026-04-26 | TTL 동적 조정 검토 완료 — Single Edge 공개 빌드에서는 추가 런타임 변경 없음 | P2 후속 |
