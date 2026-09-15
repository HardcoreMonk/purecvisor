# 컨테이너 저장소 안내와 Btrfs 조사 인계

> 기준일: 2026-09-16 KST
> 문서 상태: 공개 게시·확인 완료
> Btrfs 백엔드 상태: 조사 완료 · 구현 전 제안

## 반영 내용

- [문서 커밋 `ca057bd`](https://github.com/HardcoreMonk/purecvisor/commit/ca057bd35ab4711daec0febca3755dccc52e4a96)에서 공개 가이드와 제품 내장 도움말의 컨테이너 생성 명령 바로 위에 **ZFS 필수** 강조 상자를 추가했다.
- 설치 장의 선택형 런타임 제목에도 **LXC 컨테이너 생성에는 필수**를 명시했다.
- 컨테이너용 파일시스템 데이터셋과 VM용 zvol을 구분하고, `container_pool` 설정·기본값·부모 데이터셋 자동 생성 시도와 Btrfs/일반 디렉터리 미지원 상태를 설명했다.
- [Btrfs 조사 보고서](../research/2026-09-16-lxc-btrfs-backend.md)를 작성하고 [문서 인덱스](../README.md)에 연결했다.

## 검증과 배포

| 검사 | 결과 |
|---|---|
| `npm --prefix site run check` | 통과: 26페이지·154개 배포 파일, 기존 영상 검사 통과 |
| `make check-public-comments` | 통과 |
| `python3 scripts/check_design_md.py` | 통과 |
| `bash tests/integration/test_design_md_surface.sh` | 통과 |
| `git diff --check`, 조사 문서 상대 링크 | 통과 |
| 공개 가이드·내장 도움말 강조 상자 본문 대조 | 동일 |
| 로컬 브라우저 | 1440×1000·390×844, 밝은/어두운 모드 4조합 통과. 강조 상자·굵은 제목·명령 앞 배치·가로 넘침·페이지 오류와 설치 장 제목 확인 |
| [Pages 실행 34998584396](https://github.com/HardcoreMonk/purecvisor/actions/runs/34998584396) | build·deploy 성공 |
| 공개 도메인 브라우저 | 같은 4조합과 설치 장 제목 재검증 통과, 데스크톱·모바일 화면 확인 |

초기 로컬 사이트 검사는 기존 제목 문자열을 요구하는 게이트 때문에 실패했다. 기존 **ZFS는 선택형 런타임입니다** 문구를 유지하면서 LXC 필수 조건을 덧붙였고, 검사 코드를 바꾸지 않고 재검증을 통과했다.

배포 확인 주소:

- [컨테이너 관리](https://purecvisor.site/ko/workloads/containers/)
- [설치 및 환경 구성](https://purecvisor.site/ko/getting-started/installation/)

## 조사 판정과 경계

LXC 공식 매뉴얼과 upstream 소스에서 Btrfs 생성·CoW 복제·스냅샷 경로를 확인했다. 시험 Omarchy 호스트는 Btrfs와 필요한 기본 패키지를 갖추고 있다. 현재 PureCVisor는 생성 이외에도 삭제·복제·스냅샷·마운트에서 ZFS를 직접 사용하므로 저장 계층 분리가 필요하다.

추천안은 기존 ZFS 기본값과 객체 처리를 유지하고, Btrfs를 명시적으로 선택하는 백엔드를 추가하는 것이다. report의 설정 이름·복구 절차는 설계 후보이며 승인된 ADR이나 구현 계약이 아니다.

이번 변경은 문서·조사 범위다. 제품 CSS·JS·C 동작, RPC, 스토리지, 패키지 버전과 운영 데몬을 변경하지 않았다. 제품 내장 도움말은 공개 소스에 반영했으며 기존 설치 노드에 별도 배포하지 않았다. 새 컨테이너·서브볼륨, quota, 호스트 재시작도 수행하지 않았다.

## 현재 단계와 다음 작업

- 안내 변경: 공개 문서 운영 단계. 기존 강조 상자 스타일을 사용한 prose 편집이다.
- Btrfs: 조사·도메인 분석 단계. 실제 LXC 검증과 설계/ADR·계획 검토 후 구현할 후보이며 지원 인증은 미완료다.
- 다음 기술 검증은 시험 전용 Btrfs 경로의 LXC 생성·부팅·파일 영속화·복제·복원·삭제다. 중첩 서브볼륨, owner·config 보존, 부분 실패, 재시작과 기존 Ubuntu ZFS 회귀를 포함한다.
- 안내 원복이 필요하면 `ca057bd`의 문서 변경을 되돌리고 Pages 검사를 다시 수행한다. 보고서만 수정하는 변경은 Pages 작성 정본의 변경과 구분한다.
