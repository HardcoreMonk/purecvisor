# Omarchy 소스 컴파일 설치 가이드 인계

> 기준일: 2026-09-16 · 문서 시작점: `90a0a58` · 제품 버전: `2.0.0`
> 범위: 공개 가이드·내장 도움말·설치 진입점·Pages
> 단계: operate — 공개 main·Pages 반영과 HTTPS 검증 완료

## 요청과 작성 기준

Arch 계열 Omarchy에서 소스 컴파일로 설치하는 절차를 추가한다.
제품 동작·설치 도구를 변경하지 않고 기존 명령과 실제 설치 증거를 설명한다.
기존 설치의 업그레이드와 최초 설치를 구분하고 Ubuntu 패키지·Netplan 절차의 혼용을 막는다.

기준 소스는 NVRAM 수정 `5e84387`을 포함한 Btrfs 구현 `e028ef2`다.
초기 `2.0.0` 태그와 현재 소스의 차이를 명시하며 버전·태그는 변경하지 않는다.
새 설계 결정을 도입하지 않으며 ADR-0058의 실제 저장소 identity와 지원 제한을 유지한다.

## 근거와 작성 검토

- 기존 비공개 Arch 최초 설치·네이티브 빌드 기록을 현재 공개 소스와 대조했다.
  내부 주소·계정·인증정보·원시 로그는 문서에 옮기지 않았다.
- [Btrfs API 검증](2026-09-16-lxc-btrfs-api-validation.md)과
  [NVRAM 수정 검증](2026-09-16-vm-delete-nvram-handoff.md)을 현재 실기 근거로 연결했다.
- `Makefile`의 C23·release·BPF target, runtime helper의 manifest/JWT 처리,
  패키징 sample·systemd unit과 VM 생성의 OVMF 탐색 경로를 확인했다.
- Arch 공식 패키지 페이지에서 GLib 개발 파일, BPF 도구, 분리된 QEMU 장치와
  OVMF 파일 경로를 대조했다. ArchWiki 직접 열기는 접근 제어로 제한되어 패키지
  파일 목록과 로컬 설치 증거를 우선 사용했다.
- 최초 설치는 기존 설정·바이너리 감지 시 중단하고, `sudoedit`로 비밀번호를 설정한다.
  Btrfs는 배포판 이름으로 추정하지 않고 실제 경로를 검사하며 OVMF 링크는 기존 파일을
  강제 교체하지 않는다. HTTPS 확인은 인증서 검증을 사용한다.

## 변경과 검증 계획

1. `docs/GUIDE.md` 2.10절에 의존성·빌드·일반 UEFI·최초 설치·Btrfs·systemd·검증을 추가한다.
2. `ui/guide-content.md`를 같은 내용으로 동기화하고 서비스 워커 캐시를 다시 생성한다.
3. README·문서 인덱스·systemd 문서에 진입 링크를 추가한다.
4. 명령 블록 문법·소스 경로·본문 동기화, 공개 소스·디자인 연결·번들·사이트 검사를 수행한다.
5. 공개 `main` 반영과 Pages 성공 후 실제 HTTPS 페이지를 로컬 산출물과 대조한다.

## 운영 범위

이번 작업은 기존 설치 증거를 이용한 문서화다. 새 호스트에 설치 명령을 다시 실행하거나
운영 서비스·패키지·커널·네트워크를 바꾸지 않는다. 내장 도움말은 공개 소스의 표시 사본이며
운영 노드에 설치된 도움말의 교체는 이번 사이트 게시와 별개다.

## 로컬 검증 결과

| 검사 | 결과 |
|---|---|
| 명령·설정 | Bash 블록 7개 `bash -n` 통과, INI 중복 섹션 없음·설정값 확인, 참조 소스 경로 7개 존재 |
| 소스 기준 | `e028ef2`가 현재 HEAD의 조상임을 확인 |
| 도움말 동기화 | 상대 문서 경로를 정규화한 공개 가이드·내장 도움말 2.10절 본문 동일 |
| 번들·캐시 | 공식 bundle 명령, freshness·JS 구문 통과. JS source `9b630eef` 유지, 도움말 캐시 `8da980e9` |
| 공개 게이트 | `make -j1 check-public-comments` 통과, 자체 소스 설명 주석 0 |
| 디자인 연결 | `check_design_md.py`, `test_design_md_surface.sh` 통과 |
| 사이트 | `npm --prefix site run check` 통과. HTML 26개·산출물 154개와 기존 영상 16개 검사 통과 |
| 생성 본문 | 2.10절 anchor·진입 링크·패키지/빌드/OVMF/Btrfs/systemd/HTTPS 문구 확인, 실제 서버 주소 미포함 |
| diff | `git diff --check` 통과 |

명령 블록은 문법과 기존 소스·실기 기록을 대조한 것이며 이번 회차에서 새 Arch 설치를
실행한 결과가 아니다. 원시 검사 결과는 로컬 Git 메타데이터에 보존한다.

## 게시 결과

- 문서·도움말·캐시 7개 파일을 [`85e9e55`](https://github.com/HardcoreMonk/purecvisor/commit/85e9e55b505aa1d593f4de18a1e566a75ad32e2d)로
  공개 `main`에 반영했다. 병합된 로컬 작업 브랜치는 정리했다.
- [Pages 실행 35060030720](https://github.com/HardcoreMonk/purecvisor/actions/runs/35060030720)의
  build·deploy가 성공했다.
- 공개 HTML 27개를 HTTPS로 조회해 전부 HTTP 200과 로컬 산출물 SHA-256 일치를 확인했다.
- 실제 [Omarchy 설치 가이드](https://purecvisor.site/ko/getting-started/installation/#210-arch-계열-omarchy-소스-컴파일-설치)의
  anchor·QEMU 패키지·OVMF 경로가 배포 결과에 포함됨을 확인했다.
