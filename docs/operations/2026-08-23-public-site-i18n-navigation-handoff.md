# 공개 사이트 다국어·상단 메뉴 운영 인계

## 배포 상태

- 상태: 운영 진입 완료
- 배포 commit: `ca3a399` (`feat(site): add localized navigation routes`)
- GitHub Pages 실행: [`32645170384`](https://github.com/HardcoreMonk/purecvisor/actions/runs/32645170384)
- build와 deploy job: 성공

## 운영 계약

- `https://purecvisor.site/`: 한국어 기본 landing
- `https://purecvisor.site/ko/`: 명시적 한국어 landing
- `https://purecvisor.site/en/`: 영어 landing
- `https://purecvisor.site/docs.html`: 전체 운영 가이드 안정 URL
- 한국어 `전체 운영 가이드`와 영어 `Full operations guide`는 모두 `/docs.html`로 이동한다.

## 운영 검증

- 네 URL 모두 HTTP 200을 확인했다.
- root와 `/ko/`의 `lang=ko`, `/en/`의 `lang=en`을 확인했다.
- desktop에서 네 상단 menu group과 keyboard `ArrowDown`·`Esc` 동작을 확인했다.
- 영어 전체 운영 가이드 link를 실제 click해 `/docs.html` 본문 노출을 확인했다.
- 390px viewport에서 KO·EN 전환 노출, 수평 overflow 0을 확인했다.
- axe 위반과 browser console·page·request 오류는 0이다.

## 관측과 롤백

- 이후 변경은 Pages run의 `npm run check`와 deploy job을 모두 통과해야 한다.
- `/ko/`·`/en/`의 404, 언어 불일치, menu 미노출 또는 `/docs.html` 이동 실패를 회귀 신호로 본다.
- 회귀 시 원인 commit을 revert해 `main`에 push하고 새 Pages run과 네 운영 URL을 다시 검증한다.
- 상세 근거와 캡처는 `docs/ui-reviews/2026-08-23-public-site-i18n-navigation.md`를 따른다.
