# 공개 랜딩 Single Edge 경로 탐색기 운영 인계

## 배포 상태

- 상태: 운영 반영·검증 완료
- 구현 commit: `9b14302` (`feat(site): redesign Single Edge architecture map`)
- GitHub Pages 실행: [`32672519611`](https://github.com/HardcoreMonk/purecvisor/actions/runs/32672519611)
- build와 deploy job: 성공
- 운영 URL: `https://purecvisor.site/`, `https://purecvisor.site/ko/`,
  `https://purecvisor.site/en/`

## 운영 계약

- Hero의 version label은 eyebrow이며 실제 H1은 한국어 `하나의 Linux/KVM 노드, 하나의 제어면.`,
  영어 `One Linux/KVM node. One control plane.`이다.
- architecture figure는 Access, Control plane, Capability services, Runtime adapters,
  Linux host의 5계층과 3개 Access·4개 capability 정본 link를 유지한다.
- capability 4개는 문서 link와 분리된 `aria-pressed` 선택 control이다. 선택 상태, live label,
  runtime·host node는 하나의 path 상태로 함께 전환한다.
- keyboard는 네 방향키와 `Home`·`End`를 지원하고 roving `tabindex`로 선택 control 하나만
  tab stop으로 유지한다.
- figure 내부는 mono typography를 사용한다. 외곽은 32px, active route는 16px radius와
  선택 path 색상의 단일 `6px 6px 0` offset shadow를 사용한다.
- 390px에서는 path control을 2열로 유지하고 중복 secondary copy와 icon을 생략하되 04→05의
  세로 관계와 선택 runtime·host node는 유지한다.

## 운영 검증

- 배포 전 `npm run check`: 26 pages, 86 files artifact 검증 성공
- `make check-public-comments`, `python3 scripts/check_design_md.py`, `git diff --check`: 성공
- 배포 후 `/`, `/ko/`, `/en/` × light/dark × 1440/1024/768/390px의 24개 조합을 검증했다.
- 운영 도메인에서 Axe WCAG A/AA violation, console·page·request error와 page·map·route·control
  가로 overflow가 모두 0이다.
- map 높이는 1440px 1,018px, 1024px 1,141px, 768px 1,141px, 390px 한국어 980px·영어
  996px이다.
- 외곽 32px, active route 16px, shadow 6px/6px/0, 최소 label 12px와 figure 단일 mono family를
  계산 style에서 확인했다.
- Workloads·Storage·Network Fabric·Virtual Network의 pointer 선택, 단일 pressed/tab stop,
  live label과 runtime·host mapping을 모두 확인했다.
- 방향키·`Home`·`End` 순환과 reduced motion을 확인했다.

## 관측과 롤백

- 이후 변경은 `site/scripts/check-site.mjs`의 5계층·7개 link·4개 path control·H1·geometry·motion
  계약과 `npm run check`를 통과해야 한다.
- path 선택과 표시 node 불일치, pressed/tab stop 중복, 문서 link 누락, 390px map 1,000px 초과,
  가로 overflow, Axe 위반 또는 dark/light 대비 회귀를 운영 신호로 본다.
- 회귀 시 구현 commit `9b14302`를 revert해 `main`에 push하고 새 Pages run의 build·deploy 성공 후
  세 landing과 네 path mapping을 다시 검증한다.
- 상세 근거와 캡처 SHA-256은
  `docs/ui-reviews/2026-08-24-landing-architecture-route-explorer.md`를 따른다.
