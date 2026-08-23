# 운영 가이드 독립 문서 URL 운영 인계

## 배포 상태

- 상태: 운영 진입 완료
- 구현 commit: `2c7a9d8` (`feat(site): publish segmented operations guide`)
- GitHub Pages 실행: [`32652592291`](https://github.com/HardcoreMonk/purecvisor/actions/runs/32652592291)
- build와 deploy job: 성공

## 운영 계약

- 전체 운영 가이드 기본 진입은
  `https://purecvisor.site/ko/getting-started/installation/`이다.
- `docs/GUIDE.md`의 22개 장은 `/ko/<분류>/<문서>/` 형식의 독립 정적 page로 제공한다.
- 좌측 navigation은 8개 그룹과 22개 장, 중앙은 정적 본문, 우측은 현재 page 목차를 제공한다.
- landing과 Header의 운영 가이드 link는 새 정본 route를 직접 사용한다.
- `/docs.html`은 기존 bookmark와 숫자형 장 hash를 새 route로 보내는 호환 redirect만 담당한다.

## 운영 검증

- `/`, `/ko/`, `/en/`과 22개 운영 가이드 route의 HTTP 200을 확인했다.
- 22개 page 모두 제목, canonical URL과 현재 sidebar item이 해당 route와 일치한다.
- 설치 page HTML에서 H1과 `2.1 시스템 요구사항` 본문이 client fetch 없이 바로 노출된다.
- 한국어 landing의 `전체 운영 가이드`가 설치 page를 가리킨다.
- `/docs.html#3-vm-관리`가 `/ko/workloads/virtual-machines/`로 이동한다.
- desktop과 mobile viewport에서 수평 overflow 0을 확인했다.
- axe 위반과 browser console·page·request 오류는 0이다.

## 관측과 롤백

- 이후 변경은 Pages run의 `npm run check`와 deploy job을 모두 통과해야 한다.
- 22개 route의 404, 제목·canonical·active sidebar 불일치, 본문 fetch 의존,
  legacy hash 이동 실패를 회귀 신호로 본다.
- 회귀 시 원인 commit을 revert해 `main`에 push하고 새 Pages run과 운영 domain을 다시 검증한다.
- 상세 route map, 연구 근거와 browser 수용 기준은
  `docs/ui-reviews/2026-08-24-operations-guide-routes.md`를 따른다.
