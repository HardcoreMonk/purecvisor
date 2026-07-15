# 서비스 기능 테스트 시나리오

> **대상:** PureCVisor Single Edge 서비스 기능 검증
> **목적:** 성능, 장시간 실행, 단순 API 성공 응답으로는 보장되지 않는 기능 정합성을 시나리오 단위로 검증하기 위한 기준
> **현행화 기준:** 2026-05-04
> **관련 문서:** [DEVELOPMENT_VERIFICATION_POLICY.md](DEVELOPMENT_VERIFICATION_POLICY.md), [GUIDE.md](GUIDE.md), [ADR_INDEX.md](ADR_INDEX.md), [2026-04-29-patch-work-report.md](2026-04-29-patch-work-report.md)

---

## 1. 목적

서비스 기능 테스트는 기능이 실제 운영자가 기대하는 상태 변화를 만들었는지 확인한다.

성능 테스트, 장시간 실행 테스트, API 응답 시간 측정은 서비스 안정성의 신호이지만 기능 정합성의 증거가 아니다. 기능 완료 판정은 반드시 입력, 응답, 영속 상태, 데이터 무결성, 부작용, audit/log, 실패 경로, 정리 결과를 함께 확인한 뒤 내린다.

---

## 2. 이번 `vm.clone` 사례에서 확인한 공백

다수의 성능 테스트 시나리오와 실행 검증이 있었어도 결함을 조기에 잡지 못한 이유는 다음과 같다.

- 성능 테스트는 처리량, 응답 시간, CPU 안정성을 주로 보며 guest 내부 identity, UUID, boot artifact 같은 기능 결과를 검사하지 않았다.
- `accepted: true` 응답 확인이 worker 완료, clone domain 정의, audit `result=ok`, 최종 리소스 상태 확인과 분리되지 않았다.
- host의 disk/XML 결과만 확인하면 guest 내부의 machine-id, hostname, filesystem UUID, LVM PV/VG UUID, `/etc/fstab`, initramfs/grub 상태를 보장할 수 없었다.
- 준비된 zvol 템플릿 clone 성공을 일반 VM clone, qcow2/raw file disk clone, Ubuntu LVM clone 성공으로 확장 해석했다.
- storage format, guest layout, distro별 boot 처리, SELinux 처리 범위가 테스트 매트릭스로 명시되지 않았다.
- 실패 후 target dataset/file/domain cleanup, source 임시 snapshot cleanup, negative guard가 기능 완료 조건에 항상 포함되지 않았다.

따라서 이후 서비스 기능은 "빠르게 동작했다"가 아니라 "정해진 상태를 만들고, 잘못된 입력은 막고, 실패 후에도 정리됐다"를 기준으로 검증한다.

---

## 3. 시나리오 작성 단위

서비스 기능 시나리오는 하나의 기능 claim에서 시작한다.

```text
기능 claim: 특정 사전 조건에서 특정 입력을 실행하면, 사용자가 관측 가능한 응답과 영속 상태, 데이터 무결성, 감사 기록이 기대값과 일치한다.
```

각 시나리오는 최소 다음 항목을 가진다.

| 항목 | 필수 내용 |
|------|-----------|
| ID/이름 | 기능군, 조건, 성공/실패 경로를 드러내는 이름 |
| 기능 claim | 이 시나리오가 증명하려는 단일 기능 문장 |
| 사전 조건 | 서비스 상태, 권한, VM/storage/network 준비 상태, 필요한 패키지 |
| 실행 입력 | CLI, REST, UDS/RPC, UI 조작 중 실제 입력 |
| 외부 응답 | HTTP/RPC status, error code, accepted/job id, UI 표시 |
| 최종 상태 | libvirt, ZFS, file, DB, metadata, 설정 파일 등 영속 상태 |
| 데이터 무결성 | guest 내부 파일, UUID, 네트워크 연결, backup/restore 결과 |
| 부작용 | 자동 기동 여부, snapshot/file/domain 생성과 삭제, 권한 변화 |
| 관측성 | audit log, journal, WebSocket job completion, metrics |
| 실패/거부 경로 | 잘못된 입력, 권한 부족, 사전 조건 미충족, 중간 실패 |
| 정리 결과 | 테스트 리소스 삭제, orphan 리소스 없음, 반복 실행 가능성 |
| 증거 | 명령, 환경, exit code, 기대/실제 결과, 로그 위치 |

---

## 4. 공통 체크리스트

서비스 기능을 추가하거나 고칠 때는 다음 축을 누락하지 않는다.

| 축 | 확인 질문 |
|----|-----------|
| 권한 | role, owner-scope, UDS 우회 정책이 기대대로 동작하는가 |
| 입력 검증 | 이름, 경로, 모드, destructive ack, capability guard가 잘못된 값을 거부하는가 |
| 동기/비동기 경계 | accepted 응답과 실제 worker 성공/실패 판정이 분리돼 있는가 |
| 영속 상태 | 서비스 재조회 또는 재기동 후에도 기대 상태가 남는가 |
| 데이터 무결성 | guest 내부, storage 내용, network reachability, backup 복원 결과가 검증됐는가 |
| 부작용 | 자동 시작, 임시 파일, snapshot, metadata, lock, audit가 의도대로 남거나 정리되는가 |
| 실패 원자성 | 중간 실패 후 target 리소스와 임시 리소스가 best-effort로 정리되는가 |
| 관측성 | audit, journal, job completion, metrics가 성공/실패를 구분해 남는가 |
| 회귀 범위 | format, distro, 권한, 상태 조합 중 검증하지 않은 영역을 `미확인`으로 표시했는가 |

---

## 5. 기능군별 강화 기준

### 5.1 VM lifecycle

- start/stop/reboot/delete는 libvirt domain 상태와 API/UI 표시가 일치해야 한다.
- guest boot smoke는 단순 domain `running`이 아니라 guest agent, SSH, console, 내부 서비스 중 시나리오가 요구한 신호로 확인한다.
- 실패 경로는 이미 stopped/running/deleted 상태에서 멱등성 또는 명시적 error code를 확인한다.

### 5.2 VM clone

`vm.clone`은 storage format과 guest layout별로 별도 시나리오를 둔다.

Guest reset이 포함된 모든 `vm.clone` 시나리오의 host에는 필수 패키지 `libguestfs-tools`가 설치되어 있어야 한다. 미설치 host에서 일반 VM clone이 도구 없음으로 거부되는 경로도 실패/거부 시나리오에 포함한다.

필수 성공 시나리오:

- 준비된 ZFS zvol 템플릿 CoW clone
- ZFS zvol full clone
- Ubuntu 24.04 non-LVM qcow2 full clone + guest reset
- Ubuntu 24.04 non-LVM raw full clone + guest reset
- Ubuntu 24.04 LVM qcow2 full clone + guest reset
- Ubuntu 24.04 LVM raw full clone + guest reset
- Ubuntu 24.04 LVM ZFS zvol full clone + guest reset

각 성공 시나리오는 다음을 확인한다.

- accepted 응답의 `source_disk`, `target_disk`, `job_id`, `guest_reset`, `storage_type`
- clone domain `shut off` persistent 상태와 자동 기동 금지
- 원본/clone name, UUID, MAC, disk source 분리
- zvol CoW origin 또는 zvol full clone `origin=-`
- qcow2/raw target file 신규 생성과 원본 파일 미공유
- source 임시 snapshot과 실패 target 리소스 cleanup
- guest reset 경로의 machine-id, hostname, SSH/DHCP/cloud-init 상태, ext filesystem UUID, LVM PV/VG UUID, `/etc/fstab`, boot artifact
- clone VM boot smoke와 source/target 독립성
- `/var/log/purecvisor/audit.log`의 `vm.clone result=ok`와 WebSocket job completion

필수 거부/실패 시나리오:

- 일반 VM clone에서 `template_prepared=true` 또는 `clone_safety_ack="template-prepared"` 없이 `guest_reset=false` 요청 거부
- `libguestfs-tools` 미설치 host에서 일반 VM `guest_reset=true` clone 거부
- source VM power on 상태의 zvol/qcow2/raw clone 거부
- data disk 0개, data disk 2개 이상, unsupported disk source 거부
- source/target 동일 path, 상대 path, 이미 존재하는 target file 거부
- worker 중간 실패 시 target dataset/file/domain과 source 임시 snapshot 정리

Rocky/RHEL LVM, SELinux enforcing boot smoke는 현재 제품 완료 범위가 아니므로 후속 실환경 시나리오로 유지한다. 해당 계약을 바꾸면 검증 매트릭스에 명시적으로 추가한다.

VM 게스트 디스크 사용량 조회:

- Web UI `대시보드 > 요약 > <VM>` Storage 카드의 `디스크 사용량` 버튼은 실행 중 VM에서 `/api/v1/vms/{name}/disk-usage`를 호출해야 한다.
- 백엔드는 `vm.guest.fsinfo`에서 qemu-guest-agent의 고정 `guest-get-fsinfo` 명령만 사용해야 하며, 임의 `guest-exec` 명령으로 대체하지 않는다.
- 응답은 mountpoint별 `used_bytes`, `total_bytes`, `usage_percent`와 전체 aggregate를 포함해야 한다. `tmpfs`, `devtmpfs` 같은 pseudo filesystem은 전체 aggregate에서 제외한다.
- VM 중지, guest agent channel 누락, guest agent 미응답은 UI에서 Guest Agent 상태 확인 경로로 연결한다.

### 5.3 VM import/export

OVA import/export는 파일 생성이나 `accepted` 응답만으로 완료 처리하지 않는다.

필수 성공 시나리오:

- `vm.export.ova`: accepted 응답의 `job_id`와 worker가 완료 처리한 job이 같고, 최종 OVA 파일이 존재하며 0바이트가 아니다.
- `vm.export.ova`: job `completed`, audit `result=ok`, WebSocket `completed`, 출력 디렉터리의 OVA 파일 경로가 일치한다.
- Web UI `Export OVA` 버튼: 확인창에는 VM 이름이 escape된 일반 텍스트로 표시되고, `<br>`, `<b>` 같은 HTML 태그가 사용자에게 그대로 노출되지 않는다.
- Web UI `Export OVA` 버튼: 첫 번째 행처럼 index `0`을 누른 경우에도 현재 선택 VM으로 대체되지 않고 해당 행의 VM을 대상으로 요청한다.
- `vm.import.ova`: accepted 응답의 `job_id`, target VM domain 정의, target disk/zvol 생성, job `completed`, audit `result=ok`, WebSocket `completed`가 일치한다.

필수 거부/실패 시나리오:

- `vm.export.ova`: 없는 VM, 디스크 source 없음, `qemu-img convert` 실패, OVF/MF 쓰기 실패, `tar` 실패가 모두 job `failed`, audit `fail`, WebSocket `failed`로 끝난다.
- `vm.import.ova`: target domain, zvol, qcow2, raw `.img`, raw `.raw` 충돌은 accepted 전에 거부한다.
- `vm.import.ova`: `zfs create`, `qemu-img convert`, `virt-install` 실패 후 생성된 target zvol/file과 임시 디렉터리가 정리된다.
- 실패 cleanup 이후 같은 target name으로 재시도했을 때 orphan 리소스 때문에 실패하지 않아야 한다.

### 5.4 Storage

- 생성, 삭제, resize, snapshot은 실제 backend 상태와 API 상태가 일치해야 한다.
- ZFS 변경은 dataset, origin, used/referenced, lock metric, cleanup을 함께 확인한다.
- file disk 변경은 path validation, target 존재 여부, source 공유 여부를 확인한다.

### 5.5 Network

- bridge, OVS/OVN, NIC hotplug는 host 링크 상태와 guest 내부 reachability를 함께 확인한다.
- 단순 인터페이스 목록 응답이 아니라 DHCP, gateway, DNS, 외부 통신 중 시나리오가 요구한 신호를 검증한다.
- 실패 경로는 잘못된 bridge, 중복 NIC, owner-scope 불일치, rollback 후 domain XML 상태를 포함한다.

### 5.6 Backup/restore

- backup 성공은 archive 생성만으로 완료하지 않는다.
- restore 후 VM boot, 데이터 checksum, network identity, 기존 리소스 충돌 처리, cleanup을 확인한다.
- partial failure에서는 임시 restore path와 lock이 남지 않아야 한다.

### 5.7 Auth/RBAC

- viewer/operator/admin별 허용/거부를 같은 기능 입력으로 비교한다.
- operator 대상 VM action은 owner metadata 일치/불일치 케이스를 모두 둔다.
- audit는 성공과 거부를 구분해 남아야 한다.

### 5.8 UI/API

- UI 시나리오는 버튼 클릭 성공만 보지 않고 API 결과, 화면 재조회 결과, backend 영속 상태를 대조한다.
- 공통 모달과 확인창은 raw HTML 노출 여부를 눈으로 확인한다. 확인 메시지는 escape된 텍스트여야 하며, 줄바꿈은 helper가 통제하는 안전한 렌더링 경로만 사용한다.
- WebSocket, polling, job history는 같은 `job_id`로 accepted 응답과 completion을 연결해야 한다.
- 배포 검증은 로컬 bundle과 공개 URL bundle hash를 비교해 stale asset을 배제한다. 표준 도메인과 호환 도메인이 함께 열려 있으면 두 도메인의 `app.bundle.js`, `guide-content.md`, `sw.js` 해시를 모두 비교한다.

### 5.9 Native Host HIDS/HIPS

- Security Guard 상태 조회는 Web UI, `pcvctl security status`, UDS `security.config.get`의 `enabled`, `baseline_status`, `open_risk`, `pending_actions`가 일치해야 한다.
- 보안 이벤트 조회는 Web UI, `pcvctl security events`, UDS `security.event.list`가 같은 최근 이벤트를 보여야 한다.
- HIPS action은 `pending` 상태에서만 승인할 수 있으며, `block_ip`와 `revoke_api_key` 외 action은 수동 runbook으로 남아야 한다.
- 승인 경로는 `pcvctl security approve <event_id>` 후 accepted/job id, worker completion, audit `security.action.approve`, 최종 action/event 상태를 함께 확인한다.
- 거부 경로는 `pcvctl security dismiss <event_id>` 후 action/event 상태와 감사 기록을 확인한다.
- baseline refresh는 admin의 명시 입력으로만 수행하며, `pcvctl security baseline-refresh --path ...`, UDS `security.baseline.refresh`가 `trusted` 상태와 audit 기록을 남겨야 한다.
- CLI 표면 회귀는 `bash tests/integration/test_security_cli_surface.sh`로 확인한다.

---

## 6. 성능 테스트와 기능 테스트 분리

성능 테스트는 다음 조건을 만족할 때만 서비스 기능 검증의 보조 증거로 사용한다.

1. 성능 측정 전에 같은 입력의 기능 시나리오가 통과했다.
2. 성능 측정 후에도 핵심 영속 상태와 데이터 무결성이 유지됐다.
3. throughput, latency, CPU, memory 수치와 별도로 audit/log/job 결과가 기대값과 일치했다.
4. 성능 테스트가 발견하지 않는 영역을 문서에 `미확인`으로 남겼다.

성능 테스트가 아무리 많아도 기능 시나리오의 최종 상태, 데이터 무결성, 실패 cleanup 확인이 없으면 해당 기능은 완료로 판정하지 않는다.

---

## 7. 완료 판정

서비스 기능 시나리오는 다음이 모두 확인되어야 완료다.

- 성공 경로가 외부 응답과 최종 상태 양쪽에서 통과했다.
- 실패/거부 경로가 명시적 error code와 cleanup 결과로 통과했다.
- 비동기 기능은 accepted 응답, worker completion, audit, 최종 리소스 상태를 모두 대조했다.
- guest, storage, network처럼 시스템 경계를 넘는 기능은 실제 경계 안쪽 상태를 확인했다.
- 검증하지 않은 format, distro, 권한, 배포 조합은 `정상`이 아니라 `미확인`으로 기록했다.
