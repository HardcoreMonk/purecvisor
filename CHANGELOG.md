# Changelog

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
