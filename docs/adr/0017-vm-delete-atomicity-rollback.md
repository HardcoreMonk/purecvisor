# ADR-0017: vm.delete 원자성 복구 — XML 스냅샷 기반 롤백

날짜: 2026-04-11
상태: accepted

## 맥락

VM 라이프사이클 수직 슬라이스 감사(커밋 `94e394a`)에서 `vm.delete` 경로의
원자성 결함 C1이 확인되었다. 기존 `_vm_delete_worker` (handler_vm_lifecycle.c)
의 실행 순서는:

1. libvirt `virDomainUndefineFlags` → VM 정의 제거
2. ZFS 엑소시즘 (fuser / wipefs / dd / partx)
3. `zfs destroy -R <dataset>` → zvol 제거

문제: 단계 3(ZFS destroy)가 실패하면 VM 정의는 이미 사라졌지만 zvol이
여전히 존재하는 **반쪽 삭제 상태**가 된다. 이 상태에서 사용자가 vm.delete
재시도하면 단계 1의 libvirt 도메인이 없어서 NOT_FOUND로 거부되고(W2
멱등성 수정 전), 그대로 방치되면 orphan zvol이 쌓여 ZFS 풀 공간 고갈을
초래한다.

실제 현장에서 보고된 시나리오:
- VM 내부에 LVM 생성 → `zfs destroy`가 "dataset is busy"로 실패
- libvirt에 XML이 없으므로 운영자는 수동으로 zvol 이름을 찾아 정리해야 함
- 동일 이름의 VM을 재생성하려 하면 zvol 존재로 create가 실패

## 결정

**libvirt undefine 이전에 VM XML을 스냅샷으로 저장하고, ZFS destroy 실패
시 `virDomainDefineXML`로 정의를 재생성(롤백)한다.**

### 새 실행 순서 (handler_vm_lifecycle.c:1126+)

```c
// 0. XML 스냅샷 저장 (롤백 대비)
char *xml = virDomainGetXMLDesc(dom, 0);
gchar *saved_xml = g_strdup(xml);  // 메모리 복사
free(xml);

// 1. libvirt destroy (if running) + undefine
if (running) virDomainDestroy(dom);
virDomainUndefineFlags(dom, SNAPSHOTS_METADATA|MANAGED_SAVE);

// 2. ZFS 엑소시즘 (fuser/wipefs/dd) — 각 단계 실패 로깅 (M2)

// 3. zfs destroy -R
if (!pcv_spawn_sync(zfs_argv, ...)) {
    // ROLLBACK: 정의 재생성
    virConnectPtr rc = virt_conn_pool_acquire();
    virDomainPtr rdom = virDomainDefineXML(rc, saved_xml);
    if (rdom) {
        PCV_LOG_WARN("vm_delete", "ZFS destroy failed, definition rolled back");
    } else {
        PCV_LOG_ERROR("vm_delete",
            "ZFS destroy failed AND redefine failed — manual recovery needed");
    }
    return error;
}

// 4. 파일 디스크(qcow2/raw) unlink (C3: 실패 시 에러 전파)
//    단, device='disk' 블록의 source file만 삭제한다. CD-ROM ISO는 삭제 대상이 아니다.
```

### 롤백 시나리오 매트릭스

| ZFS destroy | virDomainDefineXML (재정의) | 상태 |
|---|---|---|
| 성공 | — | 정상 삭제 완료 |
| 실패 | 성공 | 원래 상태로 복구 (사용자 재시도 가능) |
| 실패 | 실패 | `PCV_LOG_ERROR` + 수동 복구 안내 |

### 실패한 재정의 수동 복구 절차

로그에 다음 메시지가 찍히면:
```
[ERROR] vm_delete: VM 'X': ZFS destroy failed AND redefine failed: ...
Manual recovery required: restore XML and investigate zvol state.
```

1. journalctl에서 해당 VM의 저장된 XML 문자열을 찾는다 (필요 시 백업 XML 경로에서 추출)
2. `virsh define -` 으로 XML 재주입
3. ZFS 풀 상태 점검: `zfs list -t all | grep <vm_name>`
4. zvol 참조 원인 해결 (fuser, systemctl stop, kernel 재시작 등)
5. `pcvctl vm delete <name>` 재시도

## 결과

- **좋음**: "XML 없음 + zvol 잔존" 불일치 상태를 거의 모든 경우에 회피
- **좋음**: 롤백 실패 시에도 명확한 로그 메시지 + 복구 절차로 운영자 부담 경감
- **좋음**: 기존 fire-and-forget 패턴 유지 (응답 지연 없음, XML 스냅샷은 in-memory)
- **나쁨**: libvirt 연결 풀 커넥션을 롤백 단계에서 재획득 필요 — 서킷 브레이커 OPEN 상태면 롤백 불가
- **나쁨**: saved_xml 메모리 오버헤드 — 일반적 VM XML은 ~5-10 KB
- **중립**: 재정의는 VM 상태를 복원하지만 zvol 참조/파티션 상태는 복원하지 않음 — 그건 운영자 몫

## 테스트 전략
- 단위 테스트: `zfs destroy` 인위 실패 주입 시 재정의 성공 검증 (C1 회귀)
- 통합 테스트: VM 내 LVM 생성 → 동일 이름 vm.delete 재시도 → 첫 시도 실패,
  두 번째 시도 성공
- 카오스 테스트: delete 중 libvirt 재시작 시나리오

## 관련 수정 사항
- **C3**: 디스크 파일 `unlink()` 실패 시 `g_task_return_new_error` (이전엔 WARN만)
- **W1**: `_vm_delete_callback`에 `pcv_audit_log` 추가 (감사 로그 누락)
- **W2**: 미존재 VM 삭제 시 idempotent success 반환
- **M1**: `GCancellable` + `cmap_register` 통합
- **M2**: ZFS 엑소시즘 단계별 로깅 (fuser/wipefs/dd 실패 추적)
- **ISO-SAFE**: libvirt XML의 `device='cdrom'` ISO `<source file>`을 qcow2/raw 디스크로 오인하지 않도록, `device='disk'` 블록의 source만 삭제 대상으로 인정

## 참고
- 커밋 `94e394a` fix(vm): 라이프사이클 감사 18건
- CLAUDE.md "멱등성" 원칙 (network.delete 등)
- ADR-0001 단일 프로세스 + GMainLoop (fire-and-forget 패턴 근거)
- ADR-0012 async 결과 채널 (Job ID + WS)
