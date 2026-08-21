# Changelog

버전 문자열의 단일 소스는 `include/purecvisor/version.h`의
`PCV_PRODUCT_VERSION`입니다.

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
