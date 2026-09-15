# 공개 문서 전체 현행화 인계

> 기준일: 2026-09-16 KST
> 제품 소스 기준: `e028ef2` · 문서 시작점: `4cca6b3` · 제품 버전: `2.0.0`
> 범위: PureCVisor Single Edge 공개 저장소의 문서·내장 도움말·Pages 작성 정본

## 점검 범위와 근거

시작 commit의 추적 Markdown·MDX 128개, Mermaid 원본 2개, 텍스트 기록·계약 9개를
목록화하고 상대 파일 링크와 최근 변경 관련 용어를 전수 점검했다. 이 중 날짜별
운영·연구·spec·plan·UI 리뷰와 ADR 105개는 당시 맥락을 보존하는 기록이다.
제3자 README 1개와 라이선스 고지는 제품 지원 안내로 다시 쓰지 않는다.

현행 문서는 `git diff f9785a8..e028ef2`, 실제 dispatcher·LXC driver·저장소 모듈,
Job DB·Makefile·배포 설정과 대조했다. 지정 실기 결과의 정본은 다음 두 기록이다.

- [Btrfs API 검증](2026-09-16-lxc-btrfs-api-validation.md): 실제 Arch/Btrfs guest·identity·복원·완료 통지·정리.
- [NVRAM 수정 검증](2026-09-16-vm-delete-nvram-handoff.md): Ubuntu worker·Arch API의 지정 성공·실패·재시도.

이 문서 점검은 제품 소스 전체의 수동 감사나 실기 재실행을 뜻하지 않는다.

## 변경 내용

| 문서 | 현행화 내용 |
|---|---|
| [README](../../README.md) | Ubuntu 설치 절차와 Arch 지정 검증 구분, Btrfs 시작 commit, LXC 모듈·검증 명령 |
| [변경 이력](../../CHANGELOG.md) | Btrfs 구현과 문서 현행화 항목 추가, 기존 날짜별 항목 보존 |
| [문서 인덱스](../README.md) | 현재 소스·공개 현황·NVRAM·이번 인계 연결 |
| [공개 가이드](../GUIDE.md), [내장 도움말](../../ui/guide-content.md) | NVRAM 삭제·실패 복구, Btrfs 소스 경계, 통계·개발 진입점·회차별 공개 결과 동기화 |
| [DB 설명](../DATABASE_STRUCTURE.md) | 기존 jobs 테이블을 사용하는 컨테이너 비동기 작업, DB 밖 Btrfs identity·복원 journal과 잠금 경계 |
| [검증 정책](../DEVELOPMENT_VERIFICATION_POLICY.md) | Btrfs·NVRAM 게이트/실기 매핑, clone의 실제 OPERATOR 정책, worker 안 owner 기록, 순차 게이트 실행 |
| [공개 경계](../PUBLIC_RELEASE_BOUNDARY.md) | 지원 Btrfs 범위와 미지원 기능·별도 실기 조건 |
| [소스 정책](../PUBLIC_SOURCE_POLICY.md) | 초기 공개 스냅샷 이후 독립 공개 이력 설명 |
| [ADR 인덱스](../ADR_INDEX.md) | ADR-0017 NVRAM 보강과 ADR-0058의 구현·실기·잔여 범위 |
| [사이트 운영 기준](../PUBLIC_DOCUMENTATION_SITE.md) | 현재 공개 소스·Btrfs 게시와 이전 배포 회차 분리 |
| [systemd 요구사항](../../packaging/systemd/README.md) | backend별 런타임, Ubuntu 패키지 예시와 Arch native 빌드 구분, 관리 IPv4 health 확인 |
| [에이전트 규칙](../../AGENTS.md) | LXC identity 계약·검증 진입점, 순차 `check-all` |
| 한국어·영어 landing MDX, `site/scripts/check-site.mjs` | 갱신일과 기존 날짜 검사 일치 |
| `ui/sw.js` | 내장 도움말 변경을 반영한 공식 번들 명령의 캐시 식별자 재생성 |
| 이전 저장소 안내·NVRAM 인계 | 당시 ZFS 필수/구현 전 상태와 이후 Btrfs 배포를 구분하는 후속 링크 |

현행 가이드의 공개 현황은 날짜 없는 `#228-공개-소스문서-현황`으로 연결한다.
기존 `#228-2026-09-15-공개-소스문서-현황` anchor도 보존한다.
사이트가 생성하는 한국어 장별 본문은 직접 편집하지 않고 정본에서 재생성한다.

기능 시나리오·ADR-0058·Btrfs 연구/spec/plan/API 인계는 이전 구현에서 이미 현행화됐다.
CLAUDE·DESIGN·제3자 고지·아키텍처 원본과 나머지 날짜별 기록에서는 이번 소스 변경으로
고쳐야 할 계약을 찾지 않았다. 공개 소개의 ZFS 스토리지는 계속 제공되며 Btrfs LXC의
구체적 지원 범위는 컨테이너 장과 아키텍처 본문에서 설명한다.

## 검증

| 검사 | 결과 |
|---|---|
| Markdown·MDX 상대 링크·fragment | 새 인계 포함 129개 파일, 상대 링크 261개 점검. 없는 파일·heading 0개 |
| 공개 가이드·내장 도움말 | 4.1절의 backend 계약, 18.6절 통계, 22.8절 공개 결과 동일. 내장 도움말은 상대 경로만 보정 |
| `make check-rbac` | RPC 307개·정책 252개·조회성 기본 VIEWER 73개, 파괴적 메서드 정책·operator owner-scope 통과 |
| `npm --prefix site run check` | 26페이지·154개 artifact, 4개 기능·16편 영상·225,189,881 bytes의 기존 자산 검사 통과 |
| `make -j1 check-public-comments` | standalone 빌드·LXC 저장소/driver/snapshot audit 회귀·공개 소스 정책 통과 |
| 디자인 연결 | `python3 scripts/check_design_md.py`, `bash tests/integration/test_design_md_surface.sh` 통과 |
| 내장 도움말 캐시 | `PCV_NO_DEPLOY=1 scripts/bundle-ui.sh`, `check_ui_bundle_fresh.py` 통과. JS source `9b630eef`, 캐시 `75dcc77b`, 번들·SW 구문과 자체 소스 주석 0 재확인 |
| 문법·diff | `node --check site/scripts/check-site.mjs`, `git diff --check` 통과 |

게시 후 Pages 실행과 실제 HTTPS 본문·갱신일·기존 anchor를 대조하고 아래에 기록한다.

## 운영 경계

제품 버전 `2.0.0`과 초기 태그는 유지한다. 이번 변경은 문서·사이트 날짜 검사·도움말 캐시 식별자이며
daemon·RPC·저장소 동작을 바꾸거나 운영 호스트 서비스를 재시작하지 않는다.
제품 내장 도움말은 공개 소스에 갱신하며 이번 회차에 기존 설치 노드로 별도 배포하지 않는다.

전체 소스 감사, Btrfs host reboot·정전·ENOSPC·장시간 안정성, 실제 Ubuntu ZFS 전체 회귀와
모든 지원 환경 인증은 별도다. 과거 C/UI/메모리 시험 수치와 현재 문서 검사 수치를 합산하지 않는다.
원시 시험 로그·실제 서버 주소·계정·인증정보는 공개 문서에 포함하지 않는다.
