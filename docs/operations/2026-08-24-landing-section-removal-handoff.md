# 공개 랜딩 세 구역 제거 운영 인계

## 배포 상태

- 상태: 운영 반영·검증 완료
- 구현 commit: `39f94c5` (`feat(site): simplify public landing`)
- GitHub Pages 실행: [`32653904395`](https://github.com/HardcoreMonk/purecvisor/actions/runs/32653904395)
- build와 deploy job: 성공

## 운영 계약

- `/`, `/ko/`, `/en/` landing은 Hero 다음에 문서 directory를 바로 제공한다.
- 서비스 기능 소개, 설치·실행·상태 확인 시작 흐름과 공개 범위 상세 구역은 landing에 두지 않는다.
- 상단 `서비스`, `시작하기`, `공개 범위`, `문서` disclosure는 유지하며 제거된 home anchor 대신
  대응 운영 가이드 정적 route를 사용한다.
- 문서 directory의 8개 category·22개 chapter와 역할별 추천 경로는 유지한다.

## 운영 검증

- root·한국어·영어 landing의 직접 section 순서가 `_top → documentation → pcv-final-cta`임을
  확인했다.
- `capabilities`, `quickstart`, `scope` section과 해당 stale anchor는 0건이다.
- Header의 서비스 개요, 서비스 시작 흐름, 공개 범위 개요가 시작하기 문서의 개요·5분
  퀵스타트·공개 범위 절로 연결된다.
- desktop과 mobile viewport에서 수평 overflow 0을 확인했다.
- Header keyboard `ArrowDown` 진입과 `Escape` focus 복원을 확인했다.
- axe 위반과 browser console·page·request 오류는 0이다.

## 관측과 롤백

- 이후 변경은 `npm run check`의 제거 section·anchor·CSS selector 부재 gate를 통과해야 한다.
- 제거 문구 재노출, Hero와 문서 directory 사이의 빈 section, stale anchor, 8개 category·22개
  chapter 누락을 회귀 신호로 본다.
- 회귀 시 원인 commit을 revert해 `main`에 push하고 새 Pages run과 세 landing을 재검증한다.
- 상세 연구 근거와 캡처는 `docs/ui-reviews/2026-08-24-landing-section-removal.md`를 따른다.
