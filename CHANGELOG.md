# Changelog

## 2026-11-11 2.2.0 이후 main 변경

새 릴리스가 아니다. 아래 변경은 공개 main 소스에만 있고, 2.2.0 태그·Release 자산과 패키지는 그대로다.

- ZFS 풀 잠금의 자가 초기화를 스레드 안전하게 고쳤다. 초기화 전에 여러 스레드가 동시에 첫 `pcv_zfs_pool_lock()`을
  부르면 잠금 표와 등록 뮤텍스가 두 번 만들어져 같은 풀을 두 스레드가 함께 잠글 수 있었다. 이제 초기화와 종료가 정적
  잠금 아래 이중 확인으로 한 번만 일어난다. 데몬은 시작할 때 먼저 초기화하므로 운영 경로에서는 드러나지 않던 결함이다.
  C 시험 `/zfs_exec/lazy_init_is_thread_safe`를 더했다.
- `/rbac_totp` C 시험의 복구 코드·브루트 잠금·해제 leaf가 앞 leaf가 남긴 상태에 기대지 않고 각자 픽스처를 만든다.
  하나만 골라 실행(`-p /rbac_totp/<leaf>`)해도 통과한다. 단언은 그대로다.
- 브라우저 UI 시험 8개를 더했다(`tests/ui/*.test.mjs` 69개 → 77개): API 조회 실패와 빈 결과의 구분, 컨테이너 권한
  표시와 생성 요청, 화면 요청 본문과 서버 계약, API 카탈로그 '실행'의 확인 단계, 작업 실패 사유 표시, 목록이 바뀌거나
  다시 그려질 때 VM 작업 대상 고정(두 파일), 주기 WebSocket 메시지 처리. UI 코드는 바뀌지 않았다.
- 도움말 화면 모니터링 카드의 "노출 엔드포인트"를 `/metrics`에서 전체 경로 `/api/v1/metrics`로 고쳤다. `/metrics`는
  토큰 없이 401, 토큰을 붙여도 404다. REST 표는 머리에 보이는 `/api/v1` 기준 상대 경로라 그대로다. 도움말 계약 시험이
  카드 값을 실제 REST 경로와 대조한다.
- 시험·게이트 자료를 정리했다. 호스트 격리 게이트의 반사실 시험(`scripts/tests/test_test_hermetic.py`)은 이 저장소
  이력에 없는 커밋을 읽어 7건을 건너뛰었다 — 고친 시험의 수정 전 leaf를 고정 fixture(`tests/fixtures/test_hermetic_pre_fix/`)로
  두어 항상 돈다. 분할 대상 설정(`scripts/split/targets/*.toml`)에서 분할 도구가 쓰는 머리 주석 틀과 없는 manifest
  경로를 지웠다(시험·게이트가 읽는 값은 그대로). `scripts/fe_rpc_params_uncovered.txt` 사유·`contracts/safety_controls_baseline.txt`
  주석·JSON ingress 통합 시험 출력에서 이 저장소에 없는 시험과 내부 작업 이름을 지웠다(게이트·시험 동작은 그대로).
- 문서 사이트(purecvisor.site)와 사용 가이드(`docs/GUIDE.md`, Web UI 가이드 `ui/guide-content.md` — 두 파일에 같은
  문장)를 2.2.0 소스와 대조해 고쳤다. 따라 하면 실패하던 명령·옵션·RPC·REST 경로·설정 키·메트릭 이름·출력 예를 바로잡았다
  (빠른 시작의 VM 생성에 `--qos_min_mbps`·`--qos_max_mbps` 추가, 등록되지 않은 `telemetry.host` 대신 `monitor.fleet`,
  Ubuntu 26.04에 없는 `lxc-utils` 패키지 삭제, 없는 `completion/`을 설치하는 `make install-completion` 안내를 대화형
  셸의 Tab 완성으로 교체, `daemon.conf`에 없는 `[cpu]` 절 삭제 등). 2.2.0에서 빠졌거나 낡은 안내(2.1.0→2.2.0과 2.0.4
  이하의 업그레이드 순서, deb 설치의 BPF 객체, deb 밖 설치의 `purecvisor-lxc@.service`·`97-sandbox.conf`, 릴리스 태그
  `v2.2.0`)를 보탰고 오타·예시 이름·서식도 고쳤다. 첫 화면의 `16개 검증 영상` 띄어쓰기와 DB 구조 문서 제목의 제품
  표기(`PureCVisor`)도 고쳤다.
- 공개 경계를 정리했다. 가이드에 남아 있던 내부 작업 ID, 이 저장소에 없는 ADR 번호, Single Edge에 없는 클러스터 기능
  안내(STONITH 절·CLI 클러스터 표·`[cluster]` 설정 절 등)를 지웠다. 통합 시험 출력과 `scripts/run_auto_tests.sh` 실행
  라벨의 내부 작업 ID를 기능 이름으로 바꾸고, JSON ingress 경계 통합 시험 파일 이름도
  `tests/integration/test_json_ingress_boundary.sh`로 바꿨다(시험 동작과 판정은 그대로).
- 문서 사이트 틀을 다듬었다. 문서 22쪽과 404의 `hreflang` 대체 링크가 만들지 않는 언어판(`/getting-started/…`,
  `/en/getting-started/…`, `/404/`)을 가리키지 않는다 — 빌드된 페이지만 남기며 첫 화면 `/`·`/ko/`·`/en/`은 그대로다.
  사이트 검사가 HTML과 sitemap의 모든 `rel=alternate` 대상이 빌드 산출물에 있는지, 헤더 메뉴의 모든 `#앵커`가 대상
  페이지에 있는지 본다. 헤더 메뉴 `Single Edge 경계`·`서비스 시작 흐름`(영어 `Single Edge boundaries`·
  `Start the service`)은 1.2.5절과 2.5절(systemd 서비스 설치)로 간다. 영어 첫 화면에 "Documentation is in Korean."
  한 줄, 404에 영어 문구를 더했다. 한국어 테마 선택 글자가 잘리지 않게 폭을 넓혔고, 내용이 좁은 문서 표는 바깥
  테두리가 내용 폭에 맞는다(넓은 표는 그대로 가로 스크롤).
- 사용 가이드(`docs/GUIDE.md`·`ui/guide-content.md`)를 더 고쳤다. ISO 목록 예시를 실제 JSON 항목(`name`·`path`·`dir`·
  `size_mb`)으로, 사용자 생성 예시의 평문 비밀번호를 환경 변수로 바꿨다. Local VPC CLI 검증 기록은 당시 17개 action
  (지금은 20개)으로 적고, 16.2 제목을 "주요 설정 키"로 바꿔 다른 장에서 설명하는 설정 절을 가리킨다. 확인할 수 없는
  누적 숫자("총 63건의 보안 고도화"), 도입 버전 표기 "(v1.0)" 17곳, iSCSI 안내 안의 문서 이력, 용어집의 클러스터
  용어(CAS·keepalived·Lease TTL·VIP·VRRP)를 지웠다. Web UI 배포 자산 목록에 설치되는 자산(`ui/offline.html`·
  `ui/maintenance.html`·`ui/maintenance-status.json`·`ui/style.css`·`ui/vnc-flow.mjs`·아키텍처 그림·`ui/vendor/qrcode/qrcode.js`)을
  더했다. S3 백업 제거 문장을 다듬고 네트워크 메트릭 이름 목록을 명령 블록에서 떼어 냈다.
- 가이드에 안내를 더했다. 1.4절에 명령 표기(데몬에 요청하는 `pcvctl`과 데몬 소켓 명령은 root 권한이 필요하고 예시는
  대부분 `sudo`를 생략), 6.5절부터 6.9절까지 첫머리에 Web UI 화면 경로, 8장 첫머리에 관제 메뉴 12개 화면과 관련 절을 잇는 표,
  21.1절에 줄 수가 리팩토링 당시 값이라는 안내.
- RPC·REST·DB 스키마·`daemon.conf` 형식 변경은 없다. 제품 버전 문자열은 `2.2.0`을 유지하며 새 버전이나 태그는
  발급하지 않았다.

## 2.2.0 기능 릴리스 — 2026-11-11

2.1.0 이후의 Single Edge 기능·안정성 변경을 담은 기능 릴리스다. DB 스키마와 `daemon.conf` 형식은 바뀌지 않았고 새
설정 키도 없다. 기존 RPC 메서드와 REST 경로는 그대로 쓸 수 있지만 USB 장치 할당과 Windows 게스트 명령 실행의 동작이
바뀌었다. 업그레이드 전에 아래 "업그레이드 시 확인할 점"을 확인한다.

### 신규 기능

- 호스트 장치 화면과 조회를 추가했다. 웹 UI `인프라 > 호스트 장치`가 호스트의 GPU·USB·블루투스 장치를 장치 기준 표
  한 장으로 보이고(할당된 VM과 실행 중·영구·지금만·호스트에서 빠짐·옛 장치 번호 상태), 같은 화면에서 USB
  (`vm.usb.attach`·`detach`)와 GPU(`device.gpu.attach`·`detach`)를 할당·해제한다. 읽기 RPC `host.device.list`(VIEWER),
  REST `GET /api/v1/host/devices`, CLI `pcvctl host devices [--all]`가 같은 데이터를 준다. ADMIN이 아니면 자기 테넌트에
  허용된 장치 행만 보이고 VM 이름은 자기 VM 것만 보인다(남의 VM 할당은 "다른 VM", CLI `(other VM)`).
- USB 장치를 영구 할당한다. `vm.usb.attach`·`detach`가 실행 중 VM은 live와 영구 정의를, 꺼진 VM은 영구 정의를
  바꾼다. 선택 인자 `startup_policy`(`mandatory`·`requisite`·`optional`, 기본 `optional`)와 `persistent`(기본 `true`,
  `false`면 실행 중 VM의 live 정의에만 — CLI `--live-only`)를 더했다. `vm.usb.list`는 항목마다 `live`·`persistent`·
  `startup_policy`·`missing`을 보인다.
- Windows 게스트에서 명령을 실행한다. `vm.guest.exec`가 `guest-get-osinfo`로 Windows 게스트를 가려 PowerShell로
  실행하고(응답 `shell`·`os_id`), 여러 줄·따옴표·제어 문자가 섞인 명령을 그대로 보낸다. 출력은 늘 유효 UTF-8이며 원문이
  UTF-8이 아니면 원문 바이트를 `stdout_b64`·`stderr_b64`로 함께 싣는다.

### 안정성·동작

- USB 변경 두 RPC는 메인 루프가 아니라 새 1칸 실행 줄(VM 장치 변경, 대기 8)에서 돈다. `host.device.list`는 libvirt
  읽기 줄에서 돌고 외부 명령 없이 sysfs와 `pci.ids`·`usb.ids`만 읽는다.
- `vm.guest.exec`는 서버가 처리를 시작한 때부터 15초 안에 답한다. 그 안에 끝나지 않은 명령은 `exited: false`로
  돌아온다. 클라이언트 상한은 REST·CLI 20초, 웹 UI 25초다. 게스트 agent가 응답하지 않으면 명령을 보내지 않고
  −32000 `guest agent on '<vm>' is not responding: …`로 끝난다.
- 'GPU 장치' 화면의 패스스루 버튼은 호스트 장치 화면으로 옮기는 '호스트 장치에서 할당'이 됐고 VM 이름 입력은 빠졌다.
  같은 화면의 CLI 참조에 `pcvctl gpu attach`·`detach`를 적었다.
- `make test`의 root 단계는 운영 상태 경로를 빈 tmpfs로 가린 사설 mount 네임스페이스 안에서만 돈다. 컨테이너 rootfs
  시험은 만든 임시 트리를 지워 `/tmp`에 디렉터리를 남기지 않는다.
- 공개 문서 사이트 첫 화면을 글과 실제 Web UI 대시보드 화면(견본 데이터, 사이트 테마·언어별)의 2단 구성으로 바꿨다.

### 수정

- 웹 UI에서 숨긴 버전 배지가 빈 테두리로 보이던 것을 고쳤다(`.version-badge[hidden]`).
- `device.gpu.attach`·`detach`가 GPU의 IOMMU 그룹 링크를 장치 깊이와 무관하게 그룹 번호로 푼다. 이전에는 PCI 장치
  경로 깊이가 흔한 위치와 다른 GPU(루트 포트 바로 아래 내장 GPU, 카드 안 PCIe 스위치 뒤 GPU)가 "Cannot inspect IOMMU
  group"으로 할당·해제에 실패했다.

### 업그레이드 시 확인할 점

- `sudo apt install ./purecvisor-single_2.2.0_amd64.deb`로 설치한 뒤 `sudo systemctl restart purecvisorsd`로 실행
  중인 데몬을 새 바이너리로 바꾼다. DB 스키마 이관은 없고 기존 `daemon.conf`와 데이터는 보존된다.
- **업그레이드 전에 붙인 USB 장치는 live에만 있다**(`vm.usb.list`의 `persistent: false`). 같은 `usb-attach`를 한 번 더
  보내면 영구 정의에 저장된다. 그대로 두면 이전처럼 다음 정지 때 사라진다.
- 기본 `startup_policy`는 `optional`이다 — 장치가 빠져 있어도 VM이 시작하고 목록에 `missing: true`가 보인다. 장치가
  없으면 시작하지 않아야 하는 VM은 떼고 `--startup-policy mandatory`로 다시 붙인다.
- 같은 장치를 다시 붙여도 오류가 아니다(바꾼 것이 없으면 `live: false`·`config: false`). 2.1.0은 같은 장치를 다시
  붙이면 −32000(REST 500)이었다. 영구 정의의 정책이 다르면 −32002(REST 409)이므로 떼고 다시 붙여 정책을 바꾼다. detach는
  장치가 있는 쪽에서만 떼며 어디에도 없으면 −32000이다.
  `usb-list` plain·CSV 출력은 앞 두 칸 뒤에 `live,persistent,startup_policy,missing` 네 칸이 붙으므로 칸 수를 고정한
  스크립트는 고친다.
- Windows 게스트의 `pcvctl vm guest-exec`·`POST /api/v1/vms/{name}/guest-exec` `command`는 PowerShell 언어다. cmd 문법은
  `cmd /c "…"`로 감싼다. 종료 코드는 외부 명령 그대로이고, 외부 명령이 stderr에 쓴 줄은 텍스트로 오지만 그것만으로
  1이 되지 않는다. cmdlet 오류와 명령을 멈추는 오류(`throw`, `-ErrorAction Stop`)는 1이며 명령 안 `exit N`은 N이다.
  PowerShell 시작에 1–1.5초가 더 든다.
- 게스트 명령은 15초 안에 끝나지 않으면 `exited: false`로 돌아온다(2.1.0은 약 10초).
- 'GPU 장치' 화면의 'VFIO Passthrough' 버튼과 'VM Name' 입력이 없어졌다. `pcvctl gpu list|attach|detach`는 그대로다.
- `pcvctl host devices`의 STATE 칸은 영어 고정 토큰(`live`·`persistent`·`live-only`·`stale-address`·`not-on-host`·
  `host-display`·`driver=<이름>`·`vm-off`·`blocked=<이유>` 등)을 `;`로 잇는다. JSON은 전역 옵션
  `pcvctl --format=json host devices --all`이다.
- 케이블 재연결·펌웨어 업데이트로 장치가 다시 열거되면 실행 중 VM은 장치를 잃고 자동으로 다시 붙지 않는다. 호스트 장치
  화면은 그 상태를 "호스트에서 빠짐"·"옛 장치 번호"로 보이며 복구는 '해제' 뒤 '할당…'이다.
- BPF 소스는 2.1.0과 같다. 선택형 BPF tarball의 객체·manifest는 2.1.0 tarball과 바이트 동일하므로 2.1.0 BPF
  객체를 그대로 써도 된다.
- 웹 UI 정적 자산의 버전 표기가 `2.2.0`으로 바뀐다. 업그레이드 뒤 브라우저를 새로고침해 새 UI를 받는다.

### 2.1.0으로 되돌릴 때

- DB 스키마가 같으므로 2.1.0 패키지를 다시 설치하고 데몬을 재시작하면 된다.
- 2.2.0에서 영구 정의에 저장한 USB 항목(`startupPolicy` 포함)은 libvirt 도메인 정의에 그대로 남고, 2.1.0에서도 VM은 그
  정의대로 시작한다. 2.1.0의 `vm.usb.attach`는 live에만 붙이므로 영구 항목은 `virsh` 등으로 정리한다.

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
