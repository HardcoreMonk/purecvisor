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

**Single Edge 현황(2026-10-31):** 생산 소스의 `zfs create`·`zfs destroy`는 모두 `src/utils/pcv_zfs_exec.c`의 `pcv_zfs_create_locked`·`pcv_zfs_destroy_locked`를 거치고, 그 창구만 `pcv_zfs_pool_lock`을 잡는다. 전에는 락을 부르는 `purecvisor_zfs_create_volume`·`purecvisor_zfs_destroy_volume`에 생산 호출자가 없어 어떤 경로도 직렬화되지 않았다. 두 함수와 `purecvisor_vm_manager_delete_vm_async` 경로는 지웠다. `scripts/check_zfs_mutation_argv.py`(`check-zpool-suspend-recover` 게이트)가 창구 밖 argv와 직접 락 호출을 막는다.

```c
1. pcv_zfs_pool_lock(pool, op, 대기 상한)      // Phase 1 intra-node mutex. 첫 '/'·'@' 앞이 풀 이름
   ↓ 실패: zfs를 실행하지 않고 G_IO_ERROR_TIMED_OUT
2. pcv_spawn_sync(zfs create|destroy ...)      // 락 안에서는 이 명령 하나만 돈다
3. pcv_zfs_pool_unlock(pool)                   // 성공·실패와 무관하게 해제
```

- **Job 경로의 대기 상한은 5분(`PCV_ZFS_EXEC_LOCK_WAIT_MS`)이다.** 긴 `zfs destroy` 동안 같은 풀의 다른 작업은 기다리고, 상한을 넘기면 그 zfs 명령을 실행하지 않은 채 실패로 돌아온다. 여러 단계로 된 Job은 앞 단계가 이미 끝났을 수 있다.
- **동기 응답 RPC의 대기 상한은 5초(`PCV_ZFS_EXEC_SHORT_LOCK_WAIT_MS`, `_locked_short` 창구)다.** `storage.zvol.create`·`delete`, `vm.snapshot.delete`·`delete_all`, `container.snapshot.delete`가 해당한다. 클라이언트는 REST 8초·CLI 10초·UI 12초에 끊기므로, 5분을 기다리면 사용자는 실패를 받고 락이 풀린 뒤 삭제가 실제로 일어난다. 5초를 넘기면 zfs를 실행하지 않고 `-32004`로 "풀 사용 중, 다시 시도"를 돌려준다. 다른 호출자(Job·백그라운드 스케줄러·실패 정리)는 긴 대기다.
- **메인 루프에서는 기다리지 않는다.** 창구는 호출 스레드가 메인 컨텍스트 소유자면 한 번만 시도한다. 생산 호출자는 모두 GTask 워커다. 메인 루프에 있던 `storage.zvol.create`·`delete`는 워커로 옮겼고 실결과 감사도 워커가 남긴다(ADR-0018).
- **락 안에서 기다리지 않는다.** 재시도 대기(`pcv_lxc_zfs_destroy_retry`), 사전 조회, `qemu-img`, `udevadm`은 락 밖이다. 재시도와 스냅샷 일괄 삭제는 락 대기 시간 초과를 만나면 멈춘다.
- **범위는 `zfs create`·`zfs destroy`다.** 원격 호스트의 `sudo zfs destroy`(복제 보존 정리)는 이 프로세스의 락이 보호할 수 없어 예외다. `zfs snapshot`·`rollback`·`clone`·`promote`·`rename`·`set`·`send|recv`, `lxc-create -B zfs`는 아직 락 밖이다.

**Phase 2(etcd inflight):** Single Edge 빌드에는 etcd가 없어 배선되어 있지 않다. 아래 Phase 2 관측 지표도 Single Edge에서는 값이 없다.

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

풀 락(Phase 1)은 획득·시간 초과 때마다 다음 값을 내보낸다. Single Edge에서 실제로 값이 움직이는 지표는 이 셋이다.

- `purecvisor_zfs_pool_lock_acquired_total{pool}`: 누적 획득 수. 풀에 변경이 있었는데 0이면 생산 경로가 락을 안 거치는 것이다.
- `purecvisor_zfs_pool_lock_wait_ms{pool}`: 마지막 획득의 대기 시간. 경합이 없으면 0으로 돌아간다.
- `purecvisor_zfs_pool_lock_contentions_total`: 대기 상한을 넘겨 실패한 누적 횟수.

풀 라벨은 RPC 입력에서도 오므로 칸 수를 묶는다. 64자를 넘는 이름과 표(64칸)가 찬 뒤의 새 이름은 `pool="(other)"` 공용 잠금으로 모은다. 서로 다른 풀이 한 줄로 설 뿐 안전 쪽이다.

Prometheus exporter에 ADR-0021 Phase 2 관측 메트릭을 추가했다. Phase 2가 배선되지 않은 Single Edge에서는 값이 없다.

- `purecvisor_zfs_inflight_lock_acquired_total{op,result}`:
  `result="ok|busy|error"` 획득 결과 카운터
- `purecvisor_zfs_inflight_lock_wait_ms_bucket{op,result,le}`:
  락 획득 대기 시간 histogram-compatible bucket
- `purecvisor_zfs_inflight_lock_wait_ms_sum{op,result}`
- `purecvisor_zfs_inflight_lock_wait_ms_count{op,result}`

Web UI Storage Monitor는 위 inflight 메트릭을 `/api/v1/metrics`에서 파싱해 `ZFS inflight lock` 패널로 표시한다. 패널은 전체 획득 수, `ok|busy|error` 결과 수, 평균 대기 시간, op별 breakdown을 보여준다. 샘플이 없으면 빈 상태를 표시하므로 Single Edge에서는 늘 비어 있다.

## 승인 이력

- 2026-04-14: etcd CAS lease 기반 inflight lock을 ADR-0021 구현 기준으로 채택했다.
- 2026-04-26: 사용자 승인 완료. 승인 범위는 같은 노드의 `pcv_zfs_pool_lock()` 필수 보호선, cluster build의 etcd inflight lock 2차 보호선, etcd 실패 시 best-effort 진행, create/destroy 경로 관측 메트릭 유지 규칙이다.

## 적용 이력

| 날짜 | 액션 | 관련 |
|------|------|------|
| 2026-04-14 | Phase 1 구현 (intra-node GMutex) — `pcv_zfs_lock`. 호출자가 죽은 경로뿐이어서 생산 경로는 2026-10-31까지 보호되지 않았다 | BUG-18 (F-1) |
| 2026-04-14 | Phase 2 구현 (etcd CAS lease) — `pcv_etcd_acquire_inflight_lock`. Single Edge 빌드에는 없다 | BUG-18 (이 ADR) |
| 2026-04-26 | Prometheus inflight lock 결과/대기 시간 메트릭 추가 | P2 후속 |
| 2026-04-26 | Web UI Storage Monitor의 ZFS inflight lock 패널 추가 | P2 후속 |
| 2026-04-26 | TTL 동적 조정 검토 완료 — Single Edge 공개 빌드에서는 추가 런타임 변경 없음 | P2 후속 |
| 2026-10-31 | 생산 `zfs create`·`destroy`를 `pcv_zfs_exec.c` 창구로 모으고 죽은 경로 삭제, `storage.zvol.*` 워커 이동, 획득 지표 추가, 재발 방지 게이트 | 기능 결함 시정 |
