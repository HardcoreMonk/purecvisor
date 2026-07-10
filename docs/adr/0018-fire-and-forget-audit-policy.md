# ADR-0018 — fire-and-forget RPC audit 기록 정책

- **상태**: Accepted
- **일자**: 2026-04-12
- **관련**: ADR-0001 (단일 프로세스 GMainLoop), ADR-0012 (async result channel)
- **트리거**: 운영 검증 중 vm.start가 실제로는 실패했음에도 audit DB와 UI 모두
  성공으로 표시하던 결함 발견. 5개 VM 중 1개만 실제 시작되었으나 5개 모두
  `result=ok code=0`로 기록되어 있었음.

## 컨텍스트

`dispatcher.c`의 RPC 라우팅 마지막 단계에서 모든 메서드를 무조건
audit DB에 기록한다:

```c
/* dispatcher.c — 변경 전 */
pcv_audit_log_rpc(method, "ok", 0, (gint64)dur_ms);
```

이 로깅은 핸들러가 반환된 직후에 발생한다. 그런데 fire-and-forget 패턴
(ADR-0001)을 따르는 핸들러는 `g_task_run_in_thread`로 워커를 큐잉하고
즉시 반환하므로, 위 audit 호출 시점은 **워커가 실행되기 전**이다.

결과:

1. 워커가 실패해도 audit DB는 `result=ok`로 기록함 — **거짓 성공**
2. `target` 필드는 빈 문자열로 남음 — 어떤 VM에 대한 작업인지 추적 불가
3. `duration_ms`는 dispatch 자체 시간(ms)일 뿐 실제 작업 소요 시간 아님
4. 이미 워커 콜백에서도 audit를 부르는 핸들러는 이중 기록됨
   (예: `vm.delete` `vm.stop`)

이는 다음을 침해한다:

- **컴플라이언스**: SOC2/SOX의 감사 무결성 요건 (실제 결과가 기록되어야 함)
- **운영 가시성**: UI/CLI/Grafana 모두 audit DB를 신뢰하므로 거짓 성공이 전파됨
- **회귀 탐지**: 자동 테스트가 audit DB를 검증해도 항상 PASS — 결함 은폐
- **인시던트 대응**: 사고 후 forensics 시 어떤 작업이 실제 실행되었는지 불명

## 결정

**fire-and-forget RPC의 audit 기록은 반드시 워커 콜백에서 한다.**
디스패처는 그러한 메서드의 자동 audit를 건너뛴다.

### 메커니즘

1. `dispatcher.c`에 `g_async_methods` 해시 집합 도입.
   기동 시 fire-and-forget 메서드의 이름을 등록한다.

2. `dispatcher.c`의 rpc_done 블록은 메서드가 등록되어 있으면
   `pcv_audit_log_rpc` 호출을 건너뛴다.

   ```c
   if (!pcv_dispatcher_is_async_method(method)) {
       pcv_audit_log_rpc(method, "ok", 0, (gint64)dur_ms);
   }
   ```

3. 등록된 메서드의 워커 콜백은 자체적으로 `pcv_audit_log()`를 호출하여
   진짜 결과(`ok` 또는 `fail`), `target=vm_name`, **워커 실측 소요시간**을
   기록한다.

4. 동일 콜백에서 `pcv_ws_broadcast_job_complete()`를 호출하여
   UI가 즉시 결과를 알 수 있게 한다 (ADR-0012 결합).

### 등록 대상 (Stage 1 — 본 ADR)

| 메서드 | 콜백 위치 | audit 호출 위치 |
|---|---|---|
| `vm.start` | `handler_vm_start.c::vm_start_callback` | (신규) |
| `vm.create` | `dispatcher.c::_on_vm_create_finished` | (신규) |
| `vm.delete` | `handler_vm_lifecycle.c::_vm_delete_callback` | 기존 (W1 fix) |
| `vm.stop` `vm.pause` `vm.resume` `vm.limit` | `handler_vm_lifecycle.c::vm_action_callback` | 기존 |

### Stage 2 — 조회/게스트/스냅샷/익스포트/클라우드 (2026-04-12 완료)

| 메서드 | 콜백 위치 | 비고 |
|---|---|---|
| `vm.list` | `handler_vm_lifecycle.c::vm_list_callback` | target='' (조회성, 특정 VM 아님) |
| `vm.metrics` | `handler_vm_lifecycle.c::vm_metrics_callback` | target=vm_id |
| `vm.guest.ping` | `handler_vm_lifecycle.c::_guest_ping_callback` | target=vm_id |
| `vm.guest.exec` | `handler_vm_lifecycle.c::_guest_exec_callback` | 보안 민감 (명령 실행) |
| `vm.guest.shutdown` | `handler_vm_lifecycle.c::_guest_shutdown_callback` | target=vm_id |
| `vm.snapshot.rollback` | `handler_snapshot.c::_on_rollback_done` | target=vm:snap |
| `vm.export.ova` | `dispatcher.c::_ova_export_worker` | accepted `job_id`와 worker job/audit/WS 결과 일치 |
| `cloud.import` `cloud.export` `cloud.import.finalize` | `cloud_migration.c::_update_status` (동적 `cloud.<dir>`) | `ADR-0018-audit:` 코멘트 annotation |

2026-04-25 후속에서 VM snapshot 계열은 GTask worker 경로로 정리되었고,
`target=vm:snap` 또는 `target=vm_id` audit를 직접 기록한다.

### Stage 3 — 추가 accepted fire-and-forget 표면 (2026-04-25 완료)

Stage 2 이후 코드상 `accepted` 응답을 먼저 반환하지만 worker-result audit과 WS 완료
이벤트가 없던 표면을 추가로 정리했다.

| 메서드 | 콜백/워커 위치 | 비고 |
|---|---|---|
| `backup.restore` | `handler_backup.c::_restore_worker` | target=`vm@snapshot` |
| `backup.replicate` | `handler_backup.c::_replicate_worker` | target=`vm:target_node` |
| `backup.export_s3` | `dispatcher.c::_s3_export_worker` | S3 export 실제 결과 기록 |
| `container.create` | `handler_container.c::_on_create_done` | LXC create callback 결과 기록 |
| `container.destroy` | `handler_container.c::_on_destroy_done` | LXC destroy callback 결과 기록 |
| `container.clone` | `dispatcher.c::_on_container_clone_done` | LXC clone callback 결과 기록 |
| `vm.disk.live_resize` | `handler_vm_hotplug.c::vm_disk_live_resize_worker` | target=`vm:disk_target` |
| `vm.resize_disk` | `vm_manager.c::resize_disk_thread` | target=`vm:disk_target` |
| `vm.clone` | `dispatcher.c::_vm_clone_thread` | `virDomainDefineXML` 실패를 fail로 기록 |
| `vm.import.ova` | `dispatcher.c::_ova_import_worker` | 모든 cleanup 경로에서 결과 기록 |

검증:

- `scripts/check_audit_placement.py`는 위 메서드가 async registry,
  `pcv_audit_log()`, `pcv_ws_broadcast_job_complete()`를 모두 갖는지 검증하며 기존 pre-commit 규칙을 유지한다.
- `scripts/check_ova_async_result.py`는 `vm.export.ova`가 accepted `job_id`를 worker에 전달하고, cleanup에서 실제 성공/실패 플래그 기준으로 job/audit/WS를 기록하는지 별도로 검사한다.

## 영향

### 긍정

- **감사 무결성**: 실제 결과가 audit DB에 기록됨. SOC2 audit trail 신뢰 회복
- **운영 가시성**: UI에서 vm.start 실패 사유 즉시 surface (`/health/recent-errors`)
- **회귀 탐지**: integration test가 음성 경로(failure path)를 검증 가능
  (`tests/integration/test_vm_start_failures.sh`)

### 비용

- 신규 RPC 추가 시 fire-and-forget이면 **반드시** 워커 콜백에서 audit를 호출해야 함.
  정적 검사는 `scripts/check_audit_placement.py`,
  `scripts/check_ova_async_result.py`를 함께 사용한다.
- Stage 2/3 이후에도 새 accepted fire-and-forget 표면이 생기면 async registry,
  worker-result audit, WS completion을 한 번에 추가해야 함

### 리스크

- 새 fire-and-forget 핸들러가 g_async_methods에 등록되었지만 콜백에서
  audit를 부르지 않으면 audit가 완전히 사라짐
  → mitigations: Python 정적 게이트 + pre-commit hook + ADR-0018 통합 테스트

## 검증

### B2 통합 테스트 (`tests/integration/test_vm_start_failures.sh`)

- 존재하지 않는 VM start → audit DB `result=fail` 기록 확인
- `/health/recent-errors` 엔드포인트 정상 반환 확인
- 정상 vm.start → `result=ok target=vm_name` 기록 확인
- dispatcher 자동 audit 회귀 방지 (target='' 발생 0건)

### B3 smoke test (`tests/integration/test_smoke_audit_libvirt.sh`)

- 최근 5분 내 vm.start 거짓 'ok' 회귀 검증
- audit ok 레코드의 VM이 libvirt에 실제 정의되어 있는지
- audit fail 레코드의 VM 상태 일관성

### 수동 테스트 결과 (2026-04-12)

```
[1] vm.start 거짓 'ok' 회귀 (최근 5분)        [PASS] 0건
[2] 최근 vm.start result=ok ↔ libvirt 일치    [PASS]
[3] vm.start result=fail ↔ libvirt 일관성     [PASS]
[4] vm.start E2E — 정상 호출 → 즉시 audit    [PASS] BEFORE=3 AFTER=4

B2 fail audit: 5/5 PASSED
B3 smoke:      4/4 PASSED
```

## 후속 작업

1. **pre-commit hook**: `scripts/check_audit_placement.py` — dispatcher.c의 g_async_methods 와 핸들러 콜백 audit 호출을 정적 검사한다.
2. 신규 fire-and-forget RPC는 `AGENTS.md`의 ADR-0018 규칙에 따라 worker callback audit과 `pcv_ws_broadcast_job_complete`를 함께 추가한다.
