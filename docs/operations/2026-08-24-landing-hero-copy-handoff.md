# 공개 랜딩 Hero 문구 운영 인계

## 배포 상태

- 상태: 운영 반영·검증 완료
- 구현 commit: `eb8cfac` (`feat(site): refine public landing hero copy`)
- GitHub Pages 실행: [`32655982919`](https://github.com/HardcoreMonk/purecvisor/actions/runs/32655982919)
- build와 deploy job: 성공

## 운영 계약

- Hero는 `하나의 노드, 하나의 제어면` 대형 슬로건을 사용하지 않는다.
- 한국어 범위 설명은 `VM, 컨테이너, ZFS 스토리지와 네트워크 가상화를 하나의 Linux/KVM
  노드에서 운영합니다.`이다.
- 영어 범위 설명은 같은 의미의 `network virtualization` 문장을 사용한다.
- 제품 label `PURECVISOR 2.0.0 · SINGLE EDGE`는 작은 시각 크기의 유일 H1이며 Hero의
  accessible name을 제공한다.
- CTA, 제어면 구조, Hero 다음 문서 directory 순서는 유지한다.

## 운영 검증

- `/`, `/ko/`, `/en/`과 390px mobile에서 새 문장과 유일 H1을 확인했다.
- 삭제한 한국어·영어 제목과 기존 `software-defined networking` 설명은 0건이다.
- 한국어 문장은 `word-break: keep-all`로 단어 중간 줄바꿈을 방지한다.
- desktop과 mobile viewport에서 수평 overflow 0을 확인했다.
- axe 위반과 browser console·page·request 오류는 0이다.

## 관측과 롤백

- 이후 변경은 `npm run check`의 새 문장·유일 H1·기존 문구 부재 gate를 통과해야 한다.
- 기존 슬로건 재노출, 설명 불일치, H1 누락·중복 또는 한국어 단어 중간 줄바꿈을 회귀 신호로 본다.
- 회귀 시 원인 commit을 revert해 `main`에 push하고 새 Pages run과 세 landing을 재검증한다.
- 상세 근거와 캡처는 `docs/ui-reviews/2026-08-24-landing-hero-copy.md`를 따른다.
