# 서비스 기능 테스트 시나리오

> **대상:** PureCVisor Single Edge 서비스 기능 검증
> **목적:** 성능, 장시간 실행, 단순 API 성공 응답으로는 보장되지 않는 기능 정합성을 시나리오 단위로 검증하기 위한 기준
> **현행화 기준:** 2026-08-31
> **관련 문서:** [DEVELOPMENT_VERIFICATION_POLICY.md](DEVELOPMENT_VERIFICATION_POLICY.md), [GUIDE.md](GUIDE.md), [ADR_INDEX.md](ADR_INDEX.md)

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

- 첫 생성 요청에 VPC와 subnet name/CIDR/MTU를 함께 보내면 한 Job에서 VPC row, subnet row,
  bridge, gateway, dnsmasq와 nft policy가 모두 `ACTIVE`가 된다.
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
- 생성 전 `GET /api/v1/networks/host-baseline`과 Web UI `호스트 네트워크 기준선`에서 관리
  interface/IP, main route, connected CIDR, Linux bridge, OVS와 현재 tenant의 VPC CIDR을
  기록한다. `partial` 또는 `unavailable`을 빈 inventory로 오판하면 실패다.

물리 bridge는 `dedicated`와 `shared`를 별도 시나리오로 검증한다.

- `dedicated`: 호스트 L3가 없는 격리 Ethernet만 성공해야 한다. IP/default route/master,
  wireless, bond/VLAN, 판정 불능과 ack 불일치는 mutation 전에 거부되고 bridge·NIC·desired
  state가 남지 않아야 한다. 성공 create 뒤 guest L2, delete 원상복구와 reboot reconcile을
  확인한다.
- `shared`: 생성 전 physical NIC의 master, IPv4/IPv6 주소·route, DNS, MAC, MTU와
  promiscuous 상태를 기록하고 생성·guest 트래픽·daemon restart·삭제 뒤 비교한다. VM 또는
  격리 대역의 독립 guest MAC이 upstream DHCP를 받고 gateway·host·LAN에 통신해야 하며,
  host traffic은 guest map/BPF 오류에도 계속 통과해야 한다.
- `shared` 검증용 MAC은 시나리오 자원이다. 실행마다 충돌 없는 locally administered unicast
  MAC을 사용하고 종료 시 namespace/tap/veth, lease·주소와 함께 제거한다. 운영 handoff에
  남긴 MAC은 영구 VM MAC이나 제품 DHCP 예약으로 해석하지 않는다.
- 삭제는 guest tap 존재 시 fail-closed로 거부하고, 정상 삭제는 PureCVisor 소유 TC filter,
  BPF map/pin, portal, internal bridge와 desired state만 제거한다. foreign qdisc/filter와
  host renderer profile은 보존한다.
- 실제 KVM VM과 host reboot를 통과하지 않은 임시 namespace packet 검증만으로
  `Verified`를 선언하지 않는다.

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

### 5.10 nginx 외부 TLS 종료와 프록시 신원

LAN 성공 시나리오는 `PCV_NGINX_BIND_IP=192.0.2.10`으로 전환한 예시 노드에서
`https://192.0.2.10/api/v1/health`를 호출한다. HTTP 200과 전체
`status=ok`를 확인하고, TLS check가 정확히 `mode=external_termination`,
`enabled=false`, `degraded=false`, `status=disabled_by_config`인지 대조한다.
동시에 nginx가 `192.0.2.10:443`, daemon이 루프백 HTTP만 수신하는지 확인한다.

프록시 spoof 방어 시나리오는 같은 요청 의미를 peer별로 비교한다.

`PCV-NGINX-TRUST-BOUNDARY: host-loopback`을 명시하고,
`PCV-NGINX-COUNTERFACTUAL: untrusted-local-process`인 호스트에서는 이 모드를
활성화하지 않는다. 로컬 대조군은 루프백 직접 요청이 전달 헤더를 위조할 수 있음을
보이며, 전용 호스트·sandbox/MAC·로컬 방화벽·network namespace 중 선택한
격리로 직접 접근이 제한됐는지 확인한다. 원격 대조군은 실제 peer가 비루프백이고
nginx가 헤더를 덮어써 원격 클라이언트의 spoof가 성립하지 않음을 확인한다.

- 루프백 peer(`127.0.0.1` 또는 `::1`)를 통해 들어온 `X-Forwarded-For`와
  `X-Forwarded-Proto`만 trusted 프록시 헤더로 신뢰하며, 해석된 client IP와 외부
  HTTPS scheme가 기대값과 일치해야 한다.
- 비루프백 peer가 임의의 `X-Forwarded-For` 또는 `X-Forwarded-Proto: https`를
  보내도 해당 프록시 헤더를 신뢰하지 않는다. client IP는 실제 peer로 유지하고
  외부 HTTPS 판정을 위조할 수 없어야 한다.
- IP가 아직 준비되지 않은 경우 `wait-for-local-ip`의 `exit 75`, nginx 문법
  오류의 `exit 1`, 설치 후 health 불일치의 rollback을 각각 독립 실패
  시나리오로 실행한다. 실패 뒤 기존 인증서·키와 이전 설정이 보존돼야 한다.

### 5.11 Local VPC

Local VPC는 응답 JSON만 확인하지 않고 SQLite desired state, 선택한 Linux/OVN 데이터면,
libvirt persistent XML, 게스트 reachability를 같은 시나리오에서 대조한다.

필수 성공 시나리오:

- Local VPC 생성 전에 호스트 네트워크 기준선과 `vpc.status.subnet_cidrs`를 확인하고,
  생성·삭제 뒤 제품 소유 자원만 달라졌다가 원래 기준선으로 돌아오는지 대조한다.
- Linux VPC에는 서로 겹치지 않는 subnet 둘을 만들고 각 bridge의 gateway·MTU·dnsmasq
  static lease와 재기동 후 복원을 확인한다. OVN VPC에는 LS/LR/DHCP/LSP/Port Group/ACL,
  nullable `bridge_name`, `backend_ref`와 local chassis binding을 대조한다.
- 정지 VM을 attachment로 연결한 뒤 VPC DB의 VM UUID·MAC·IP, libvirt inactive XML,
  VPC metadata가 일치하고 VM 부팅 후 DHCP 주소와 같은 VPC subnet 간 L3 통신이 된다.
- `nat` VPC에서 outbound와 응답 트래픽은 통과하고, 미게시 inbound는 차단된다.
- Security Group이 연결된 `ACTIVE` attachment에 Service Publish를 만들면 지정한 host
  address/port와 허용 source CIDR에서만 target port에 도달한다.
- 데몬 재시작 시 listener 수용 전에 quarantine이 적용되고, 실제 상태 수렴 뒤에만 full
  policy가 활성화된다. drift가 있으면 해당 attachment는 `QUARANTINED`로 남는다.
- mutation의 accepted `job_id`, Job 최종 상태, worker audit, WebSocket completion이 같은
  메서드와 대상을 가리킨다.
- 설치된 `pcvctl` 관리 표면에서도 VPC 생성→NAT 실제 통신→`isolated` 전환 차단→역순 삭제가
  terminal Job과 packet 효과로 함께 통과하고 임시 자원이 남지 않는다.
- Web UI/API는 기존 전용 REST 16개 작업과 backend capability GET만 사용하고, VIEWER
  read-only와 OPERATOR/ADMIN mutation 표시를 구분하며, accepted Job이 `completed`가 되기
  전에는 성공으로 표시하지 않는다. backend readiness/capacity와 실패 원문은 열린 모달
  안에서 입력을 보존한 채 식별 가능해야 한다.

필수 거부·반사실 시나리오:

- `vpc.create`의 첫 subnet 묶음이 일부만 있으면 actual mutation 전에 거부하고, subnet
  actual 적용 실패 시 이번 요청에서 만든 VPC/subnet/bridge/dnsmasq를 역순 rollback한다.
- 다른 VPC·기존 host connected CIDR과 겹치는 subnet, network/broadcast/gateway 주소의
  수동 할당, stale `expected_revision`을 거부한다.
- cross-VPC 신규 트래픽과 정책 변경 전 established conntrack 트래픽, MAC/IP/ARP spoof,
  VPC VM의 host SSH·REST·Prometheus 접근을 차단한다.
- `isolated` VPC Service Publish, 전체 공개 source를 요청한 non-admin, host에 없는
  `listen_address`, wildcard와 특정 주소의 같은 protocol/port 충돌을 거부한다.
- 실행 중 VM attach, cross-VPC multi-homing, tenant-overlay와 Local VPC 동시 연결,
  VPC managed bridge의 `network.*`·raw NIC 우회를 거부한다.
- 게시 중 attachment, attachment가 있는 subnet, subnet이 있는 VPC와 게시 대상 VM의
  마지막 Security Group 삭제·해제를 거부한다.
- nft/dnsmasq/libvirt 중간 실패를 주입하면 열린 경로를 남기지 않고 `last_error`와
  quarantine 또는 재시도 가능한 `DETACHING` 상태를 보존한다.

현재 model/store/policy 단위 테스트와 빌드·RBAC·audit 게이트는 구현 검증에 포함하지만,
격리 bridge와 실제 VM을 사용한 위 효과 시나리오를 통과하기 전에는 `Verified`로 판정하지
않는다.

Linux Local VPC는 실 VM 경로가 `Verified`다. 선택형 OVN 후보는 실제 LSP/chassis
namespace packet과 daemon restart까지 확인했지만
부팅 KVM, Linux/OVN 공존, controller/host reboot와 전 단계 fault injection이 남아 있어
`NET-VPC-BE-*`를 공개 지원 `LIVE-PASS`로 승격하지 않는다.

### 5.12 네트워크 전체 구성 매트릭스

네트워크 기능은 다음 상태 중 하나로 판정한다.

generic OVN은 Local VPC OVN backend와 별개로 C0~C4 다섯 chapter를 자동 실행한다.

| Chapter | 공개 검증 범위 | 완료 조건 |
|---|---|---|
| C0 | 호스트 네트워크 기준선과 OVN readiness | interface·route·Linux bridge·OVS/OVN actual을 기록하고 NB/SB/northd/controller/chassis가 모두 준비됨 |
| C1 | logical switch, port와 분산 DHCP | L2 packet, DHCP option 연결, 잘못된 입력 거부와 부분 리소스 없음 |
| C2 | logical router, multi-subnet L3와 NAT | router port 원자성, L3와 SNAT/DNAT packet 효과, 등록된 삭제 경로 cleanup |
| C3 | ACL, tenant 격리와 REST filter | ACL drop/allow 반사실, tenant 경계, `switch`/`router` query가 대상 표식만 반환 |
| C4 | switch-owned DHCP와 최종 정리 | switch 삭제가 소유 DHCP를 자동 정리하고 foreign row를 보존하며 임시 제품 자원이 남지 않음 |

2026-08-31 공개 가능한 실행 요약은 C0~C4 전 chapter PASS, 정상 경로의 수동 emergency
cleanup 0건, 종료 시 제품 소유 OVN 테스트 자원과 임시 계정 residue 0건이다. NAT packet용
external gateway port 지정은 격리 시험 fixture이며 제품의 멀티 노드 gateway scheduling
기능을 뜻하지 않는다. 이 결과는 generic OVN `NET-OVN-01~07` 근거이며, Local VPC OVN의
부팅 KVM·Linux/OVN 공존·controller/host reboot·전 단계 fault injection gate를 대체하지
않는다.

| 판정 | 의미 |
|---|---|
| `LIVE-PASS` | 제품 RPC/CLI가 만든 실제 kernel/OVS/OVN/libvirt 데이터면에서 packet 효과와 cleanup까지 통과 |
| `TEST-PASS` | 소프트웨어 계약·실패·rollback은 통과했지만 현재 장비에서 실제 peer/하드웨어 packet 효과는 미확인 |
| `SKIP-CAPABILITY` | 필수 NIC, IOMMU, DPDK 또는 외부 peer가 없어 안전하게 실행할 수 없음. 성공으로 계산하지 않음 |
| `PLANNED` | 승인된 설계의 미구현 gate. 구현·지원·성공 증거로 계산하지 않음 |
| `FAIL` | 기대한 응답·상태·packet 효과·cleanup 중 하나라도 불일치 |

구성 표의 한 행은 지원 모드 하나의 최소 기능 claim이다. 서로 직교하는
축을 무작정 카테시안 곱으로 늘리지 않고, 실제 데이터면·정책·가속기 경계가
달라지는 구성만 독립 시나리오로 둔다.

| ID | 제공 구성 | 필수 성공 효과 | 필수 거부·정리 | 근거(표 현행화 2026-08-31) |
|---|---|---|---|---|
| NET-LB-01 | Linux bridge `nat` + DHCP/DNS | `bridge → subnet → guest` DHCP, gateway, DNS, outbound/reply | CIDR 충돌 거부, dnsmasq·nft·bridge 역순 정리 | `LIVE-PASS` — `test_network_mode_live.sh`, `pcvnat0` 외부 ping/DNS |
| NET-LB-02 | Linux bridge `isolated` | guest↔gateway/guest 통신, 외부 트래픽 차단 | masquerade 없음, 모드 전환 후 stale DHCP/nft 없음 | `LIVE-PASS` — 임시 `pcvisolive` packet 효과 |
| NET-LB-03 | Linux bridge `routed` | NAT 없이 host route를 통한 packet forwarding | masquerade 생성 금지, upstream route 없음을 NAT 성공으로 오판하지 않음 | `LIVE-PASS` — 모드·rule·cleanup 실측 |
| NET-PHY-01 | `bridge/dedicated` 유선 uplink | host L3가 없는 예비 NIC를 bridge port로 편입, guest upstream L2, 삭제·reboot 복원 | IP/default route/master/Wi-Fi/bond/VLAN/ack 불일치는 mutation 전 거부 | `SKIP-CAPABILITY` — 예비 유선 NIC 없음. 관리 NIC 강제 변경 안 함; 안전 거부는 통과 |
| NET-PHY-02 | `bridge/shared` TC-BPF portal | host IP/route/DNS 보존, 독립 guest MAC의 upstream DHCP, gateway·host·LAN 양방향 | 다중 MAC 차단 시 host 경로는 보존, owned TC/BPF/portal만 정리 | `LIVE-PASS` — `pcvbr0`, 주소를 문서용 대역으로 익명화한 실제 KVM, 3/3 ping |
| NET-NIC-01 | VM boot NIC + live hotplug | 추가→목록→DHCP→packet→제거, live/config XML 동일 | VPC managed bridge raw 우회 거부, 없는 MAC·VM·bridge 실패 시 XML 무변경 | `LIVE-PASS` — `test_nic_hotplug_live.sh`; 완전한 interface XML detach |
| NET-FW-01 | host nftables firewall | 지정 protocol/port/action의 packet allow/drop, 재기동 복원 | argv injection·잘못된 chain 거부, foreign table/rule 보존 | `TEST-PASS` — manager/policy 회귀. 별도 외부 client live gate 필요 |
| NET-VLAN-01 | OVS access VLAN | VM port tag 적용, 같은 VLAN 통신·다른 VLAN 격리 | VLAN 1~4094 벗어남 거부, foreign OVS port 보존 | `TEST-PASS` — RPC/OVS 계약. 현 노드의 물리 trunk peer 없음 |
| NET-QOS-01 | interface tc QoS | rate/burst 설정, get, packet shaping counter, remove·restore | 없는 interface·잘못된 rate 거부, foreign qdisc 보존 | `LIVE-PASS` — `/qos` 119항목과 실제 tc namespace 효과 |
| NET-QOS-02 | per-VM/per-tenant HFSC+Cake SLA | 결정적 class ID, min/max 형성, stats, daemon reconcile | min>max·필수 SLA 누락 거부, chaos timebox 만료 후 Cake 복원 | `TEST-PASS` — QoS/카오스 통합 회귀. 별도 실 VM throughput 측정 필요 |
| NET-OVS-01 | Single Edge OVS VXLAN local bridge | create/list/info/delete, VNI/CIDR 영속 상태, OVS bridge 동일 | 중복 VNI·잘못된 peer 거부, tunnel/bridge 역순 정리 | `TEST-PASS` — Single 바운더리·overlay core 회귀 |
| NET-OVS-02 | Single Edge 수동 VXLAN peer | 두 노드 tunnel 상태와 양단 guest L2 packet | peer 손실 탐지, 피어 제거 후 로컬 bridge 보존 | `SKIP-CAPABILITY` — 독립 원격 peer 노드 없음; 자동 풀메시는 Single 공개 범위 밖 |
| NET-TOV-01 | per-VM WireGuard tenant overlay | tenant create/list/get, VM IP 배정, peer 간 암호화 packet, detach/delete/restart rehydrate | Local VPC 동시 연결 거부, endpoint 준비 실패 시 VM start fail-closed | `TEST-PASS` — `/tenant_overlay` 계약 통과; 외부 peer 암호 packet은 미확인 |
| NET-OVN-01 | Single node OVN 제어면 | NB/SB/northd/controller/chassis가 모두 ready일 때만 `available=true` | 바이너만 있고 DB/controller/chassis 미준비면 false | `LIVE-PASS` — C0에서 local socket, Chassis, 전체 readiness와 빈 초기 inventory 확인 |
| NET-OVN-02 | LS + LSP + 분산 DHCP | 두 LSP의 DHCP lease·같은 LS L2 packet, address/port-security/DHCP option 연결 | mac/ip 편측·잘못된 ID 거부, transaction 실패 부분 포트 없음 | `LIVE-PASS` — C1에서 lease, L2 packet, NB option과 SB binding 확인 |
| NET-OVN-03 | LR + multi-subnet L3 + NAT | 두 LS 사이 L3 packet, router port 원자성, SNAT/DNAT 제어 상태 | 부분 LRP/LSP 연결 없음, 삭제 후 LR/port/NAT 없음 | `LIVE-PASS` — C2에서 양방향 L3, SNAT와 DNAT_AND_SNAT packet 효과 확인. external gateway port는 fixture |
| NET-OVN-04 | OVN ACL | priority/direction/match/action에 따라 실제 packet drop/allow | 잘못된 direction/action/priority 거부 | `LIVE-PASS` — C3에서 UI 적용, REST 대상 filter와 ICMP drop/allow 반사실 확인 |
| NET-OVN-05 | OVN tenant bundle | LS + 양방향 ACL + DHCP가 모두 성공해야 create 성공 | ACL/DHCP 실패 시 LS rollback, 같은 CIDR을 쓰는 다른 tenant의 L2 경계 보존 | `LIVE-PASS` — C3에서 tenant bundle과 동일 주소 대역 사이의 L2 격리 확인 |
| NET-OVN-06 | switch-owned DHCP cleanup | 제품 RPC로 switch 삭제 시 같은 ownership marker의 DHCP option도 한 transaction으로 정리 | 다른 switch/foreign 행 보존, 조회 모호성·비정상 UUID·상한 초과는 mutation 전 거부 | `LIVE-PASS` — C4에서 두 switch 음성 대조, 소유 DHCP 0, 정상 경로 emergency cleanup 0건 확인 |
| NET-OVN-07 | 인증 REST ACL/NAT filter | `switch`·`router` query가 canonical RPC params에 도달해 대상 표식만 반환 | 필수 filter 누락은 `-32602`, 다른 switch/router 표식 혼입 금지 | `LIVE-PASS` — C3의 대상/비대상 분리와 C4의 임시 계정·OVN residue 0 확인 |
| NET-VPC-00 | Local VPC + 첫 subnet 일괄 생성 | 한 `vpc.create` Job으로 VPC/subnet/bridge/DHCP/policy가 ACTIVE | 부분 subnet payload 사전 거부, actual 실패 시 신규 aggregate rollback | `LIVE-PASS` — 일괄 성공·부분 입력 사전 거부·cleanup; post-row fault injection은 별도 잔여 |
| NET-VPC-01 | `bridge → LOCAL VPC → subnet → VM` | stopped VM persistent attach, boot DHCP, DB/XML/metadata 일치, 같은 subnet L2 | managed bridge raw NIC 우회·수동 reserved IP 거부 | `LIVE-PASS` — `test_vpc_live.sh` 14/14 |
| NET-VPC-02 | Local VPC multi-subnet | 한 VPC의 두 subnet 간 L3, DHCP/gateway/MTU·reconcile | 겹치는 VPC/host CIDR, stale revision 거부 | `LIVE-PASS` — 실 KVM 두 대 간 L3 |
| NET-VPC-03 | Local VPC `nat`/`isolated` | NAT outbound/reply, isolated outbound 차단, cross-VPC·host management 차단 | isolated publish 금지, 모드 전환 후 stale nft/conntrack 차단 | `LIVE-PASS` — packet 양성/반사실 대조 |
| NET-VPC-04 | Security Group + Service Publish | 허용 source/host address/port만 target VM에 도달 | 미허용 source, 없는 listen address, wildcard 충돌, 마지막 SG 제거 거부 | `LIVE-PASS` — KVM TCP publish allow/drop·spoof drop |
| NET-VPC-BE-01 | VPC backend 선택·호환 | backend 생략과 `linux`가 동일하며 `ovn`은 전체 readiness 뒤에만 생성 | 알 수 없는 값·backend 변경·한 VPC 혼합 거부, schema v1 VPC actual 불변 | `LIVE-PASS` — 자동 계약, schema v1→v2, backend readiness, 최소 OVN 생성/삭제·cleanup 확인 |
| NET-VPC-BE-02 | OVN VPC subnet·attachment | 첫 subnet 일괄 생성 뒤 LS/LR/DHCP/LSP/port-security와 실제 KVM L2·L3 | 부분 NB transaction·binding 실패 시 quarantine/rollback, Linux bridge·dnsmasq 생성 금지 | `TEST-PASS` — 제품 경로 inactive XML·실 LSP/chassis namespace L2/L3·보상 확인, 부팅 KVM 잔여 |
| NET-VPC-BE-03 | OVN VPC SG·tenant 격리 | Port Group/ACL의 same-VPC·SG allow와 cross-VPC·spoof drop | established flow 정책 우회, generic `ovn.*` managed resource 변경 거부 | `TEST-PASS` — SG ACL packet·owned mutation 거부 확인, cross-VPC/spoof/established packet 잔여 |
| NET-VPC-BE-04 | OVN host edge `nat`/`isolated`/Publish | edge route+nft의 outbound/reply, isolated 차단, source allowlist+SG publish | transit 충돌·host 관리면 접근·미게시 inbound 거부, stale route/nft 없음 | `TEST-PASS` — NAT/isolated/Publish·관리 IP/edge 차단과 cleanup 확인, 전체 음성 행렬 잔여 |
| NET-VPC-BE-05 | Linux·OVN VPC 공존 | 두 backend 동시 packet 효과, 전역 CIDR 비중첩, 서로의 resource 불변 | OVN 장애 시 Linux fallback 금지, foreign OVS/OVN resource 보존 | `PLANNED` — 혼합 backend gate 미구현 |
| NET-VPC-BE-06 | OVN reconcile·실패 원자성 | daemon/controller/host restart에서 deny 우선 후 desired generation 수렴 | NB/SB/libvirt/edge/nft fault 주입 뒤 열린 경로·고아 resource·성공 audit 없음 | `TEST-PASS` — daemon restart와 생성/attach 보상 확인, controller/host restart·전 단계 fault 잔여 |
| NET-SG-01 | 일반 VM/컨테이너 Security Group | default-deny, ingress/egress allow, 재기동 nft 복원 | 잘못된 CIDR/port/protocol, 사용 중 group 삭제 거부 | `TEST-PASS` — store/nft/dispatcher 회귀; VPC 결합 packet은 `LIVE-PASS` |
| NET-DPDK-01 | OVS-DPDK vhost-user | hugepage/PMD 준비, PCI bind, DPDK bridge/VM packet, unbind 원 드라이버 복원 | 관리 NIC·IOMMU 미준비 변경 거부, 부분 bind rollback | `SKIP-CAPABILITY` — runtime `available=false`, vdev 0 |
| NET-SRIOV-01 | SR-IOV VF direct attach | PF enable, VF MAC/VLAN, IOMMU/vfio attach, guest packet, detach·driver 복원 | PF/VF IOMMU 미준비면 첫 driver 변경 전 거부, attach 실패 rollback | `SKIP-CAPABILITY` — runtime `available=false`, PF 0 |
| NET-OBS-01 | 네트워크 audit/metrics/reconcile | accepted Job·worker·audit·WS·final kernel state 일치, 재기동 수렴 | worker 실패를 success로 집계 금지, 불일치는 quarantine/reconcile_required | `LIVE-PASS` — VPC 성공/실패 audit·daemon restart 수렴 |

전체 매트릭스 검증은 다음 순서를 표준으로 한다.

1. 시작 전 관리 IP·default route·DNS·서비스·기존 VM/VPC/OVS/OVN 기준선을 기록한다.
2. 제품 CLI/RPC로만 구성하고 `accepted` 작업은 terminal Job까지 기다린다.
3. DB/XML 조회와 함께 guest/namespace의 DHCP·ARP·TCP/ICMP packet 효과를 양성·반사실로 비교한다.
4. 재기동/reconcile을 포함한 시나리오는 listener 수용 전 quarantine와 최종 정책 수렴을 본다.
5. 역순 삭제 후 임시 domain·namespace·tap/veth·bridge·OVS/OVN record·DHCP·nft·tc·DB 행이 없고 기준선이 같은지 확인한다.
6. 하드웨어/피어 부재는 `SKIP-CAPABILITY`로 남기고, 위험한 관리 NIC 강제 전환으로 양성 결과를 만들지 않는다.

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
