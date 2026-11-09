# Changelog

## 2.1.0 기능 릴리스 — 2026-11-09

2.0.4 이후의 Single Edge 기능·보안·안정성 변경을 담은 기능 릴리스다. 기존 RPC 메서드와 REST 경로는 그대로 쓸 수
있지만 일부 기본값과 응답 의미가 바뀌었고 DB 스키마가 확장됐다. `daemon.conf`는 그대로 읽으며 새 키는 모두 선택이다.
업그레이드 전에 아래 "업그레이드 시 확인할 점"과 "2.0.4로 되돌릴 때"를 확인한다.

### 구조

- REST·디스패처·VM 계열 내부 구현을 여러 변환 단위로 분할해 유지보수성과 빌드 범위를 개선했다. 공개 API 표면은
  동일하다.

### 신규 기능

- ADMIN 전용 VM 구성 관측(`vm.inspect`, `GET /api/v1/vms/{uuid}/inspection`, `pcvctl vm inspect`)을 추가했다.
- 세션 단위 로그아웃을 추가했다. `POST /api/v1/auth/logout`은 본문 `refresh_token`의 세션 하나만 폐기하고,
  `POST /api/v1/auth/logout/all`은 그 사용자의 세션을 모두 폐기한다.
- 백업 복제 대상 SSH 호스트 키 고정(`pcvctl backup hostkey scan|trust|list|remove`)을 추가했다.
- 컨테이너 볼륨 마운트 생명주기·실행 중 스냅샷 롤백·ZFS send/recv 복제 경로를 보강했다.
- 보안 사건 운영자 종결(`security.event.close`, `pcvctl security close`), 자가치유 상태 조회(`healing.status`),
  호스트 자원 등록(`pcvctl resource …`)과 테넌트 VPC 주소 풀(`pcvctl vpc pool-set|pool-list`)을 추가했다.

### 보안·격리

- 관리면 노출을 좁히고(nginx 공유 비밀 `X-PCV-Proxy-Auth`, `[server] trusted_proxies`) VPC 관리 대역을 보호한다.
- LXC 컨테이너를 기본 unprivileged로 만들고, 컨테이너를 전용 systemd 유닛 `purecvisor-lxc@.service`로 시작한다.
- OPERATOR의 호스트 자원 경계와 컨테이너 볼륨 원본 경계, VPC 테넌트 주소 풀·자원 등록을 도입했다.
- 보안 사건·감사 증가 상한과 사건 종료(`security.event.close`)를 추가하고, 인증 주체 존재 검사를 더했다.
- 데몬에 systemd 샌드박스 드롭인 `97-sandbox.conf`를 더했다.
- 감사 기록을 비동기 행위자로 분리해 요청 처리 지연을 줄였다.

### 안정성

- 메인 루프 정지를 막는 보강과 데몬 견고성 개선을 반영했다. 오래 걸리는 일부 RPC는 자원별 실행 줄에서 돌고,
  메인 루프 정지는 로그와 `purecvisor_mainloop_stalls_total` 지표로 드러난다.
- Job 결과를 별도 journal에 먼저 내구화하고, 재시작 때 결과가 확정되지 않은 이전 실행을 `interrupted`로 표시한다.
  종료된 Job 기록은 기본 30일 뒤 정리한다(`[daemon] job_retention_days`).

### 업그레이드 시 확인할 점

- `sudo apt install ./purecvisor-single_2.1.0_amd64.deb`로 설치한 뒤 `sudo systemctl restart purecvisorsd`로 실행
  중인 데몬을 새 바이너리로 바꾼다. 기존 `daemon.conf`와 데이터는 보존된다.
- **DB 스키마는 자동 이관된다.** 데몬 시작(자원 등록 DB는 첫 사용) 때 다음이 적용된다: Job 상태 DB(`pcv_jobs.db`)의
  열 4개·인덱스·`PRAGMA user_version=2`와 결과 journal, VM 상태 DB의 `vm_locks.owner` 열, RBAC DB의
  `identity_nonce` 열·`PRAGMA user_version=2`, Security DB의 `security_events` 열 6개·인덱스, 새 파일
  `resource_registry.db`. 자동 되돌림은 없으므로 **업그레이드 전에 데몬을 정지하고 `/var/lib/purecvisor/*.db*`
  (WAL 포함)를 백업**한다. 세부는 `docs/DATABASE_STRUCTURE.md`를 따른다.
- **모든 사용자가 한 번 다시 로그인한다.** RBAC 이관이 신원 세대 값이 없는 옛 refresh 세션을 폐기한다. 저장 역할이
  사용자 역할보다 높은 API 키는 한 번 낮춘다(낮춘 수는 기동 로그에 남는다). DB에 관리자 계정이 없을 때 설정값으로
  비상 토큰을 발급하던 fallback은 없어졌다 — 관리자는 `pcvctl auth create`(UDS)나 `admin_password` 시딩으로 만든다.
  Bearer만 보내는 `POST /api/v1/auth/logout`은 이제 그 access만 무효화한다. 전체 폐기는 `/auth/logout/all`을 쓴다.
- **패키지가 systemd 파일 두 개를 새로 설치한다.** 데몬 드롭인
  `/etc/systemd/system/purecvisorsd.service.d/97-sandbox.conf`(`NotifyAccess=main`, `SystemCallArchitectures=native`,
  `LockPersonality=yes`, `RestrictRealtime=yes`, `SystemCallFilter=~@swap @obsolete @cpu-emulation @module @raw-io`)와
  컨테이너 시작 유닛 `/etc/systemd/system/purecvisor-lxc@.service`다. 둘 다 dpkg conffile이며 설치 스크립트가
  `systemctl daemon-reload`를 한다. 샌드박스는 데몬을 재시작해야 적용된다. `container.exec`와 헬스 `exec` 프로브는
  이 필터를 물려받으므로 컨테이너 안 32비트 바이너리는 SIGSYS, `setarch`는 EPERM으로 실패한다. 필요하면 드롭인을
  지우고 `daemon-reload` 뒤 데몬을 재시작한다. deb 밖 설치는 두 파일을 직접 설치하고 `daemon-reload`한다 — 유닛
  파일이 없으면 컨테이너가 시작되지 않는다.
- **컨테이너 기본 격리가 unprivileged로 바뀐다**(`[container] rootless` 기본 `true`, 2.0.4는 `false`). 새 컨테이너를
  만들려면 `/etc/subuid`·`/etc/subgid`에 `root:100000:65536`(또는 `rootless_uid_start`·`rootless_uid_count`에
  맞춘 줄)이 있어야 하며, 없거나 다른 계정 범위와 겹치면 `container.create`가 privileged로 바꾸지 않고 −32002로
  거절한다. 이전 기본이 필요하면 `[container] rootless = false`를 둔다. 이미 있는 컨테이너의 격리 방식은 바뀌지 않으며
  목록의 `PRIV` 열로 구별한다. privileged 생성과 privileged·호스트 볼륨 컨테이너의 복제는 ADMIN 전용이다.
- **실행 중이던 컨테이너는 한 번 정지·시작한다.** 2.0.4 데몬이 띄운 컨테이너는 다음 정지·시작 전까지 전용 유닛 밖에서
  돈다.
- **`container.volume.attach`는 `readonly`를 생략하면 읽기 전용이다**(2.0.4는 읽기·쓰기). 쓰기 볼륨은
  `"readonly": false`(CLI `--rw`)를 명시한다.
- 컨테이너 생성·삭제·시작·정지와 NIC 작업의 잠금 충돌은 −32004(REST 409)로 통일됐다(2.0.4는 −32603, REST 500).
  500을 재시도 신호로 쓰던 자동화는 409로 옮긴다.
- **Prometheus 수집 인증 기본이 바뀐다.** `[metrics] auth` 기본이 `none`에서 `loopback`이 되어, 같은 호스트에서
  데몬 평문 리스너(`127.0.0.1:8080`)로 직접 온 요청만 토큰 없이 받는다. 원격·nginx 경유 수집은 토큰이 필요하다 —
  `auth.apikey.create`에 `role: 0`을 명시한 API 키를 Bearer로 보낸다. 이전 동작이 필요하면 `[metrics] auth = none`을
  명시한다. `/internal/vms`·`/internal/telemetry`·`/health/recent-errors`도 같은 규칙을 따른다.
- **nginx 외부 TLS 종료 설치본**은 기존 vhost의 server 블록에 `include /etc/nginx/snippets/purecvisor-proxy-auth.conf;`를
  더한 뒤 설치기(`scripts/install-nginx-termination.sh`)를 다시 실행하고 nginx reload·데몬 재시작 순으로 반영한다.
  include가 없으면 nginx 뒤 모든 클라이언트가 `127.0.0.1` 하나로 보인다.
- **백업 복제**: 업그레이드 직후에는 등록된 호스트 키가 없어 모든 `backup.replicate`가 −32012로 거절된다. 대상마다
  `pcvctl backup hostkey scan` → 지문 대조 → `pcvctl backup hostkey trust`를 한 번 한다.
- **OPERATOR 자원 경계**: 테넌트가 없는 OPERATOR는 이미지·ISO·OVA·저장 위치·PCI·USB·브리지 참조가 −32006, VPC
  조회·변경이 −32002로 거절된다. 업그레이드 직후 ADMIN이 테넌트 지정·자원 등록·VPC 주소 풀 배정을 한다(가이드
  10.2.1·6.12.2).
- **BPF 객체가 다시 빌드됐다.** LSM 사건 레코드의 끝 여유 공간에 파일시스템 장치 필드가 더해졌다. 레코드 크기(304바이트)와
  경로 위치는 그대로라 2.0.4 BPF 객체도 그대로 적재·동작하지만(장치 필드가 0이라 사건은 이전처럼 제출된다), 감시
  경로와 무관한 tmpfs의 같은 이름 파일 열기를 사건에서 거르려면 2.1.0 BPF tarball을 설치한다.
- 웹 UI 정적 자산의 버전 표기가 `2.1.0`로 바뀐다. 업그레이드 뒤 브라우저를 새로고침해 새 UI를 받는다.

### 2.0.4로 되돌릴 때

- 자동 되돌림(down-migration)은 없다. 확실한 방법은 2.0.4 패키지를 다시 설치하고 업그레이드 전에 떠 둔 DB 백업을
  복원하는 것이다. 그 사이 생긴 Job·감사·보안 사건 이력은 따로 보관해 대조한다. 되돌리기 전에 볼륨을 쓰는 컨테이너를
  정지한다(2.1.0 데몬이 정지 때 staging을 정리한다).
- 백업 없이 2.0.4 바이너리로 돌아가도 DB는 열린다. 더해진 열은 모두 기본값이 있고 2.0.4는 열 이름을 명시해 읽고
  쓰기 때문이다. 다만 다음이 남는다.
  - Job 상태 DB: 2.0.4는 `interrupted` 상태를 알 수 없는 값으로 읽고, 그 사이 쓴 Job 행에는 세대·method 값이 비어
    있다.
  - RBAC DB: 낮춘 API 키 역할은 되돌아가지 않는다. 2.0.4에서 만든 계정과 세션은 다시 2.1.0으로 올릴 때 이관되어 그
    세션이 폐기된다(재로그인).
  - `resource_registry.db`: 2.0.4는 이 파일을 쓰지 않으므로 OPERATOR 호스트 자원 경계와 테넌트 VPC 주소 풀이 적용되지
    않는다. 파일은 지우지 말고 보존한다 — 다시 올리면 그대로 열린다.
  - `[metrics] auth`, 컨테이너 `rootless`, 볼륨 `readonly` 기본값은 2.0.4 기본으로 돌아간다.
- dpkg는 새 패키지에 없는 conffile을 지우지 않으므로 2.0.4 deb로 내려도 `97-sandbox.conf`와
  `purecvisor-lxc@.service`가 남는다. 2.0.4 데몬은 컨테이너를 직접 띄우므로 `97-sandbox.conf`가 남아 있으면 그 필터가
  컨테이너에 상속된다. 두 파일을 지우고 `systemctl daemon-reload` 뒤 데몬을 재시작한다.

## 2.0.4 보안 수정 — 2026-11-08

2.0.3의 시작 볼륨 읽기 전용 결함을 고친 보안 패치다. 새 기능은 없으며 데이터 형식과 설정 파일 형식, RPC·REST API는
그대로다. 결함은 2.0.3 이하의 공개 릴리스에 모두 있으므로 업그레이드를 권장한다.

### 시작 볼륨 읽기 전용

- 읽기 전용으로 붙인 컨테이너 볼륨이 컨테이너 안에서 읽기 전용 약속을 지키지 못할 수 있던 결함을 고쳤다. 이제 데몬은
  컨테이너를 시작할 때 각 볼륨 원본을 직접 확인한 뒤 검사한 지점만 컨테이너에 넘기므로, 읽기 전용으로 붙인 볼륨은
  컨테이너 안에서 읽기 전용으로 유지된다.
- 볼륨이 있는 컨테이너는 purecvisor를 거쳐 시작해야 한다. 데몬 밖에서 `lxc-start`로 직접 켜거나 옛 방식으로 켜려 하면
  시작이 거부된다.

### 업그레이드 시 확인할 점

- `sudo apt install ./purecvisor-single_2.0.4_amd64.deb`로 설치한 뒤 `sudo systemctl restart purecvisorsd`로 실행
  중인 데몬을 새 바이너리로 바꾼다. 기존 `daemon.conf`와 데이터는 보존된다.
- 볼륨을 쓰는 컨테이너가 반영 전에 시작해 실행 중이면 옛 방식으로 돌고 있다. 한 번 정지했다가 다시 시작하면 새 동작이
  적용된다.
- 웹 UI 정적 자산의 버전 표기가 `2.0.4`로 바뀐다. 업그레이드 뒤 브라우저를 새로고침해 새 UI를 받는다.

## 2.0.3 보안 수정 — 2026-10-25

2.0.2의 컨테이너 명령 실행 결함을 고친 보안 패치다. 새 기능은 없으며 데이터 형식과 설정 파일 형식은 그대로다.
API에는 아래 "응답 변경"의 하위 호환 필드만 더해진다. 결함은 2.0.2 이하의 공개 릴리스에 모두 있으므로
업그레이드를 권장한다.

### 컨테이너 명령 실행

- 컨테이너 안에서 실행하는 명령(`container.exec`, `container.nic.list`·`nic.attach`·`nic.detach`,
  `container.set_bandwidth`, 헬스 프로브와 자동 재시작)이 끝나지 않으면 데몬의 요청 처리가 멈출 수 있던 결함을
  고쳤다. 네 컨테이너 네트워크 RPC와 헬스 프로브는 워커에서 실행되고, 모든 실행에 시간·출력 상한이 있으며,
  상한을 넘기면 명령의 프로세스 그룹 전체를 종료하고 회수한다. 데몬이 종료를 시작하면 진행 중인 실행을
  기다리지 않고 정리한다.
- 데몬이 띄우는 자식 프로세스와 컨테이너 안에서 실행되는 명령이 데몬 환경 변수를 그대로 물려받던 결함을
  고쳤다. 자식 프로세스는 허용 목록의 환경 키만 받고, 컨테이너 안 실행은 환경을 비운 뒤 고정 `PATH`로 시작한다.
- 컨테이너 안 실행은 셸 문자열을 조립하지 않고 인자 목록으로만 실행한다. `container.exec`의 RPC 계약
  (컨테이너 안 `/bin/sh -c <cmd>`)은 그대로다.
- 상한: `container.exec` 60초·출력 4 MiB, `nic.list`의 IP 조회 3초, `nic.detach`의 실행 중 제거 5초,
  `set_bandwidth`의 컨테이너 안 설정 5초, 실행 중 `nic.attach` 배선 전체 6초(조회·변경 출력은 64 KiB).
  헬스 프로브는 동시에 최대 4개를 실행하고 프로브 `timeout_sec`는 [1, 30]으로 제한한다. 클라이언트 대기도
  맞췄다 — REST·CLI의 `container.exec`는 65초, 웹 UI 터미널은 70초(UI의 자동 조회는 기본 12초).
- 실행 중 컨테이너의 `nic.attach` 배선은 컨테이너 init 프로세스의 네트워크 네임스페이스를 쓰는 단계별 명령으로
  바꿨다. 배선을 확인하지 못하면 성공으로 답하지 않는다.

### 응답 변경(하위 호환)

- `container.nic.list` 원소에 `ipv4_unavailable`(bool)을 항상 더한다. 컨테이너가 실행 중인데 IP를 시간 안에
  조회하지 못하면 `true`이고 `ipv4`는 빈 문자열이다.
- `container.nic.detach` 응답에 `warning`이 붙을 수 있다. 설정에서는 제거됐지만 실행 중 인터페이스 제거 명령이
  0이 아닌 값으로 끝난 경우다.
- 실행 중 배선·제거를 확인하지 못하면 −32000 `config saved; live wiring not confirmed: <단계>: <원인>`·
  `config removed; live interface removal not confirmed: <원인>`으로 답한다. `nic.attach`·`nic.detach`·
  `set_bandwidth`는 같은 컨테이너에 다른 변경 작업(시작 등)이 진행 중이면 −32603 잠금 오류
  (`VM '<name>' is already locked (op: …, pid: …)`, 원문이 없으면 `Container is busy`)로 답한다.
- `container.exec`는 상한 초과뿐 아니라 프로세스 생성 실패·시그널 종료도 `exec failed (timeout or error): …`로
  답한다. 오류 코드와 응답 필드는 그대로다.

### 시험

- 상한 실행·자식 환경 허용 목록·컨테이너 네임스페이스 실행·헬스 순찰 단위 시험(`/spawn_launcher`,
  `/lxc_ns_exec`, `/container_health`)과 서버·클라이언트 상한 정렬 계약 시험
  `tests/test_container_exec_bounds_contract.py`를 더했다. 계약 시험은 기존 `check-all` 42개 게이트 안에서
  실행된다.

### 업그레이드 시 확인할 점

- `sudo apt install ./purecvisor-single_2.0.3_amd64.deb`로 설치한 뒤 `sudo systemctl restart purecvisorsd`로
  실행 중인 데몬을 새 바이너리로 바꾼다. 기존 `daemon.conf`와 데이터는 보존된다.
- 데몬이 띄우는 자식 프로세스(`virsh`·`zfs`·`ip`·`lxc-*` 등)는 **허용 목록 환경만** 받는다: `USER`, `TZ`,
  `TMPDIR`, `LIBVIRT_DEFAULT_URI`, `LIBVIRT_LOG_OUTPUTS`, `LIBVIRT_LOG_FILTERS`, `ZFS_MODULE_TIMEOUT`,
  `http_proxy`·`https_proxy`·`no_proxy`(대문자 포함), `SSL_CERT_FILE`, `SSL_CERT_DIR`, `CURL_CA_BUNDLE`.
  `LC_*`는 전달되지 않으며(자식 로캘은 `LANG=C.UTF-8`), `PATH`는 `/usr/sbin:/usr/bin:/sbin:/bin`, `HOME`은
  `/root`로 고정된다. 데몬 환경(systemd 드롭인 등)으로 자식 도구에 값을 넘기던 설정이 있으면 확인한다.
- `container.exec`는 60초 안에 끝나야 한다. 넘으면 명령과 그 하위 프로세스가 종료되고 오류로 답한다.
- exec 헬스 프로브도 `timeout_sec`(기본 5초, [1, 30]으로 제한) 안에 끝나야 성공이다. 이전에는 exec 프로브에
  상한이 없었고 tcp 프로브는 1초 고정이었다. 5초 넘게 걸리는 exec 점검을 `auto_restart`와 함께 쓰고 있었다면
  `timeout_sec`을 늘려 다시 등록한다(`pcvctl container health-set`에는 `--timeout` 옵션이 없으므로 RPC·REST로
  지정한다).
- 헬스 자동 재시작 도중 데몬이 종료 중이면 정지 뒤 시작을 건너뛰어 컨테이너가 정지 상태로 남는다. 데몬을 다시
  올린 뒤 직접 시작한다.
- 웹 UI 정적 자산의 버전 표기가 `2.0.3`으로 바뀐다. 업그레이드 뒤 브라우저를 새로고침해 새 UI를 받는다.

## 2.0.2 보안 수정 — 2026-10-16

2.0.1의 REST 요청 처리 결함 두 건을 고친 보안 패치다. 새 기능은 없으며 데이터 형식, 설정 파일
형식과 API 응답은 그대로다. 두 결함은 2.0.1 이하의 공개 릴리스에 모두 있으므로 업그레이드를 권장한다.

### REST

- 인증 없는 preflight 요청이 데몬을 종료시킬 수 있던 결함을 고쳤다. 요청 처리 함수가 끝에서 해제하는
  값을 함수 시작에서 모두 초기화해, 인증 전 단계에서 끝나는 요청도 초기화되지 않은 값을 해제하지 않는다.
- 인증된 요청의 경로 처리에서 경로 조각 배열을 길이 확인 없이 읽어 배열 경계 밖을 읽던 결함을 고쳤다.
  조각 수를 먼저 센 뒤 있는 조각만 읽는다. 라우팅과 응답은 바뀌지 않는다.
- 인증 실패(401) 등 일찍 끝나는 응답 경로가 요청 ID와 trace 정보를 해제하지 않아 요청마다 메모리가
  조금씩 새던 문제를 고쳤다. 응답 코드·헤더·본문은 그대로다.

### 시험

- 두 결함의 재발을 막는 소스 계약 시험 `tests/test_rest_cleanup_init_contract.py`와
  `tests/test_rest_segs_bounds_contract.py`를 추가했다. 기존 `check-all` 42개 게이트 안에서 실행된다.

### 업그레이드 시 확인할 점

- `sudo apt install ./purecvisor-single_2.0.2_amd64.deb`로 설치한 뒤 `sudo systemctl restart purecvisorsd`로
  실행 중인 데몬을 새 바이너리로 바꾼다. 기존 `daemon.conf`와 데이터는 보존된다.

## 2.0.1 보안 수정 — 2026-10-11

2.0.0의 권한 판정·네트워크 게시·세션 처리 결함을 고친 보안 패치다. 새 기능은 없으며
데이터 형식과 설정 파일 형식은 그대로다.

### 권한과 인증

- REST `POST /api/v1/rpc`의 권한 판정이 요청 본문에서 처음 찾은 `"method"` 문자열이 아니라
  실제로 실행되는 최상위 `method`로 이뤄진다. `params` 안에 다른 `method` 값을 넣어 낮은 권한으로
  관리 작업을 실행할 수 있던 결함을 막는다. `method`가 없으면 400 `Missing method field`,
  문자열이 아니거나 비어 있으면 400 `Malformed method field`다.
- dispatcher 정책표에 없는 RPC는 ADMIN만 실행한다(기본 거부). 이전 기본값은 VIEWER였다.
  정책 행이 없던 기존 RPC 72개는 REST 앞단과 같은 등급으로 명시해 웹 화면 권한은 바뀌지 않는다.
- 소유자 검사를 받는 VM 요청에서 `name`·`vm_id`·`vm_name`·`vm`(복제는 `source` 포함)이 서로 다른
  VM을 가리키거나 문자열이 아니면 역할과 무관하게 `-32602`로 거부하고 audit에 실패로 남긴다.
  자기 VM으로 소유자 확인을 받고 다른 VM을 조작할 수 있던 경로를 막는다.
- WebSocket 이벤트 방송을 인증을 마친 연결로 한정했다. 토큰 없이 접속한 연결이 인증 대기 시간
  동안 VM·작업 이벤트를 받던 문제를 고쳤다.
- 웹 UI 로그아웃이 서버 로그아웃(`POST /api/v1/auth/logout`)을 호출해 그 사용자의 refresh 세션을
  폐기하고 현재 access token을 무효화한다. 로그아웃·세션 만료 때 WebSocket 재연결 예약과 열린
  대화상자, 이전 사용자 정보도 함께 정리한다.

### 네트워크

- Local VPC 서비스 게시(DNAT)를 호스트 자신의 주소로 향하고 관리 대상이 아닌 인터페이스로 들어온
  트래픽으로 한정했다. 다른 목적지로 가는 트래픽이 게시 대상으로 넘어가지 않으며, 게시 forward
  허용은 cross-VPC 차단 뒤에 평가된다.
- security group DB 파일과 WAL·SHM 파일 권한을 0600으로 맞춘다.

### VM·컨테이너·저장소

- 컨테이너 RPC의 문자열 인자를 타입까지 확인한다. 잘못된 타입 값 하나로 데몬이 종료되던 결함을
  고쳤다. 볼륨 경로와 환경 변수 값에 개행 등 제어문자를 넣어 LXC 설정 줄을 추가할 수 없다.
- REST 컨테이너 볼륨 분리가 컨테이너 안 경로(`DELETE /containers/{name}/volumes/{path}` 또는 본문의
  `container_path`)로 동작한다.
- `container.set_bandwidth`는 실제 적용에 실패하면 성공으로 응답하지 않고 사유를 오류로 돌려준다.
- `rootless=true`를 명시했는데 `/etc/subuid`·`/etc/subgid` 범위가 없으면 privileged 컨테이너로
  바꿔 만들지 않고 생성을 거부한다.
- 디스크 크기 변경은 지정한 대상 디스크(기본 `vda`)의 실제 저장소에 적용되고, 현재보다 작은
  크기는 거부한다. 웹 UI는 서버가 읽는 `new_size_gb`·`target` 키를 보낸다.
- 스냅샷 롤백은 스냅샷이 있는지 먼저 확인한 뒤에만 VM 전원을 제어한다. 롤백이 실패하면 원래 켜져
  있던 VM을 다시 시작하고, 모든 결과를 audit에 남긴다.
- 같은 이름의 OVA 가져오기와 VM 생성·삭제가 겹치지 않도록 이름 잠금을 걸고 `pool` 값을 검증한다.
- VM IP의 ARP 조회를 외부 명령 실행 대신 `/proc/net/arp` 직접 읽기로 바꿨다.

### 로그·패키지·배포

- `audit.log`가 회전된 뒤 `systemctl reload purecvisorsd`(SIGHUP)에서 새 파일을 다시 연다.
  deb가 logrotate 설정(`/etc/logrotate.d/purecvisor`)과 coredump 제한 drop-in을 conffile로 설치한다.
- deb에 라이선스(`/usr/share/doc/purecvisor-single/copyright`)와 제3자 고지
  (`THIRD_PARTY_NOTICES.md`)를 포함하고 설치 파일 권한을 정규화한다.
- 예시 nginx 설정의 요청 본문 상한을 2MB로 낮췄다. 데몬의 REST 본문 상한은 1MB다.
- `scripts/deploy.sh`는 원격 반영에 무작위 이름의 0700 임시 디렉터리를 쓰고, 설치한 UI 파일을
  root 소유·고정 권한으로 맞추며, UI 모듈 설치 인자 순서 오류를 고쳤다.
- `make clean`은 `dist/`에서 이 Makefile이 만드는 deb 산출물만 지운다.

### 웹 UI

- VM 목록 조회가 실패하면 빈 목록 대신 오류와 직전 목록을 보여 준다.
- VM 생성은 접수와 완료를 구분해 알리고, 작업이 실패하면 완료로 표시하지 않는다.
- CD/DVD 찾아보기에서 ISO를 고르면 경로가 입력된다. 데몬 설정 화면은 서버 값을 엔티티 문자열로
  바꾸지 않는다. VNC 도구 버튼 설명이 원문으로 보인다.

### 시험

- 시험 실행이 운영 감사 로그·security group DB 등 운영 경로에 쓰지 않도록 격리하고, 이를 검사하는
  게이트를 추가했다. 새 검사는 기존 `check-all` 42개 게이트 안에서 실행된다.

### 업그레이드 시 확인할 점

- `sudo apt install ./purecvisor-single_2.0.1_amd64.deb`로 2.0.0 위에 설치한다. 기존 `daemon.conf`는 보존된다.
- REST를 거치지 않는 gRPC 경로의 `audit.search`·`daemon.version`은 ADMIN role이 필요하다.
  정책 행이 없는 플러그인 메서드도 ADMIN 전용이다.
- 서로 다른 VM을 가리키는 별칭 키를 함께 보내는 외부 스크립트는 `-32602`를 받는다.
- 로그아웃하면 같은 계정의 다른 브라우저도 access token 만료 뒤 다시 로그인해야 한다.

## 2.0.0 소스 갱신 — 2026-10-08

- AWS 수동 Import/Export·Near-Live·전용 작업 API/UI/CLI와 S3 백업 제거.
- 공통 Job 접수·결과 영속화, QGA 오류 수명, Trace 자식 회수, VNC callback 수명 보강.
- VM 시작 hugepage 사전 검사·절전 복귀, OVA 디스크/크기 판독, 부팅 초기 자가 치유 rate 시정.
- 기존 공개 독립 이력과 로컬 VM·디스크·과거 DB 보존. 태그/바이너리 패키지는 재발행하지 않음.


버전 문자열의 단일 소스는 `include/purecvisor/version.h`의
`PCV_PRODUCT_VERSION`입니다.

## 2026-10-02 개발 main 소스 정합화

- 공개 제품 로직을 개발 main의 공개 허용 범위와 맞추고 자체 소스 주석·소스맵 제외 정책을 재검증했다.
- 개발 main에 없는 공개 전용 LXC Btrfs, 저장소 identity 확장과 관련 Job·시험 확장을 제외했다. 현재 LXC는 ZFS 전용이다.
- 기존 NVRAM 삭제·롤백 보호와 첫 알림 쿨다운 시정, 내부 Monitoring 제외 및 공개 검사 적응을 유지했다.
- 미병합 ISO·Windows·UI 후속 브랜치는 포함하지 않았다. 실행 중 서비스·운영 컨테이너·데이터는 변경하지 않았다.
- 과거 Btrfs 소스·ADR·지정 실기 기록은 보존하며 현재 지원 계약과 구분한다. 새 버전이나 태그는 발급하지 않았다.

## 2026-09-16 선택형 LXC Btrfs와 공개 문서 현행화

- 공개 소스 `e028ef2`부터 `[container] storage_backend=btrfs`를 명시해 ZFS 없이
  privileged LXC를 생성·시작·정지·삭제하고, 정지 상태에서 CoW 복제·스냅샷·복원할 수 있다.
- 기존 기본값은 ZFS다. 컨테이너별 실제 저장소 식별자를 대조하고, Btrfs rootfs 복원 시
  현재 설정·소유자·이미지 메타데이터를 보존한다. 기본값 변경은 저장소 이동이 아니다.
- 컨테이너 비동기 작업을 영구 Job ID와 실제 완료 결과로 추적하고, snapshot 완료 audit의
  요청자·대상·성공/실패를 보존한다. LXC 7의 cgroup v2 CPU 가중치 설정을 반영했다.
- 지정 Arch/Btrfs 호스트에서 실제 API·게스트 부팅·파일 복원·거부·복구 상태·완료 통지와
  정리를 검증했다. 범위와 미검증 조건은 [API 검증 인계](docs/operations/2026-09-16-lxc-btrfs-api-validation.md)를 따른다.
- README·공개 가이드·내장 도움말·DB 설명·검증 정책·공개 경계·사이트 운영 문서를
  같은 소스 기준으로 맞췄다. 초기 `2.0.0` 태그는 ZFS 전용이며 버전·새 태그는 발급하지 않았다.

## 2026-09-16 UEFI VM 삭제 NVRAM 오류 수정

- Ubuntu·Arch 공통 `vm.delete` 경로의 NVRAM 처리 옵션 누락을 수정했다.
- 디스크 삭제가 성공할 때까지 NVRAM을 보존해 저장소 실패 후 XML 복원 시 UEFI 설정을 유지한다.
- XML과 NVRAM 저장 형식이 불명확한 경우 삭제 전에 거부하고, 마지막 NVRAM 정리 실패를 audit `fail`로 기록한다.
- 실제 worker 회귀를 `make test`에 연결하고 기본 AppArmor 프로필에 표준 NVRAM 경로 정리 권한을 추가했다.
- 제품 버전 문자열은 `2.0.0`을 유지한다. 검증 범위와 적용 상태는 [운영 인계](docs/operations/2026-09-16-vm-delete-nvram-handoff.md)에 기록한다.

## 2026-09-15 공개 소스·GPU 테스트 영상·문서 현행화

- 호스트 CPU·메모리 self-healing의 첫 발동 여부를 분리해 부팅 후 600초 이내의 첫 알림이
  쿨다운에 막히지 않도록 수정했다. 재알림 간격과 정책별 독립성은 유지한다.
- Single Edge 이벤트·명령 연결과 공개 소스맵 부재의 반사실 검사를 `check-all`·`dev-check`에
  연결했다. 공개 `check-all`은 40개 계약 게이트다.
- [`22d6912`](https://github.com/HardcoreMonk/purecvisor/commit/22d6912fe5ee951cbc6c46e0da23e8f7971427a8)
  공개 소스 검증: C 1,479 PASS·14 SKIP, audit startup 5 PASS, UI 512 PASS·0 SKIP.
  전체 감사와 지원 환경 인증은 별도 미완료 상태다.
- 랜딩에 RTX 3070 Ti·Windows 11 GPU Passthrough 124초 영상을 추가했다.
  기존 네트워크 원본 15편을 유지하며 4개 기능·16편을 제공한다.
- 운영 가이드와 제품 내장 도움말의 로컬 HTTP 기본값을 `127.0.0.1:8080`으로 맞추고,
  현재 검증 명령·공개 문서 인덱스·검증 기록·사이트 갱신 날짜를 정리했다.
- 공개 제품 버전은 `2.0.0`을 유지한다.

## 2026-09-08 랜딩 기능 검증 원본 영상

- 편집 영상 6편을 검증 당시 MP4 15편과 촬영 WebM 원본으로 교체했습니다.
- Local VPC·OVN·VXLAN의 생성·VM 연결·통신·차단·복구·정리 과정을 전체 화면으로
  제공하고 각 장면에 한국어·영어 목적과 결과 설명을 추가했습니다.
- 기존 단일 player와 지연 재생을 유지하고 원본 링크·1440×900 화면·파일 hash를 검증했습니다.

## 2026-08-31 공개 네트워크·GitHub Pages 현행화

- 읽기 전용 host network baseline RPC·REST·Web UI를 추가하고 OVN·Local VPC 작업 전에
  관리 interface, route, Linux bridge, OVS와 tenant CIDR을 확인하도록 문서를 맞췄습니다.
- 등록된 generic OVN 공개 표면을 정확한 18개 RPC로 고정하고, switch-owned DHCP 자동
  정리, 인증 REST ACL/NAT filter와 canonical `-32602` 계약을 반영했습니다.
- 완결되지 않은 OVN/NFV Load Balancer와 production caller가 없는 VM 자동 포트 helper를
  사용자 기능·도움말·API 표면에서 제거했습니다.
- 공개 GUIDE, 릴리스 경계, 검증 정책, 기능 시나리오와 Pages Networking 장을 같은 계약으로
  동기화하고 범위 밖 멀티 제어면 참고 장은 발행 목록에서 제거했습니다.
- 공개 source에 실제 포함된 SQLite 9개 DB 경계는 유지하며 다른 내부 배포판의 저장소 수와
  혼용하지 않습니다.

## 2.0.0

PureCVisor Single Edge의 첫 독립 공개 릴리스입니다.

### 주요 기능

- C23 기반 단일 프로세스 데몬과 GMainLoop/GTask 비동기 실행 구조
- KVM 가상 머신과 LXC 컨테이너 수명주기 관리
- ZFS·파일 기반 스토리지와 백업·복원·스냅샷 관리
- Linux bridge, Open vSwitch, Local VPC 네트워크 관리
- JSON-RPC CLI, REST API, WebSocket 이벤트, Web UI
- JWT 인증, RBAC, 감사 로그, 보안 정책과 운영 상태 점검
- Prometheus 메트릭, 경보, 로그, 자가치유·운영 자동화 기능
- Debian 패키징, systemd, AppArmor, 배포·검증 스크립트

### 공개 소스 정책

- 공개 저장소는 Single Edge 제품 범위만 포함합니다.
- 자체 소스의 설명 주석과 docstring은 제거했습니다.
- Web UI 소스맵과 비공개 운영 기록은 포함하지 않습니다.
- 제3자 구성요소의 라이선스 고지와 저작권 표시는 유지합니다.

자세한 범위와 검증 방법은 `docs/PUBLIC_RELEASE_BOUNDARY.md`,
`docs/PUBLIC_SOURCE_POLICY.md`, `docs/DEVELOPMENT_VERIFICATION_POLICY.md`를
참조하십시오.
