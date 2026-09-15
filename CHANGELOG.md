# Changelog

버전 문자열의 단일 소스는 `include/purecvisor/version.h`의
`PCV_PRODUCT_VERSION`입니다.

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
