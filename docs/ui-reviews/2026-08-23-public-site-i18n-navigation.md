# 공개 사이트 다국어·상단 하위 메뉴 UI 리뷰

> 상태: Verified, 로컬·Pages build·운영 브라우저 검증 완료
> 대상: `https://purecvisor.site/`, `/ko/`, `/en/`
> 관련 결정: ADR-0046

## 목표와 사용자 작업

기본 주소는 한국어로 유지하고 한국어와 영어의 명시적 주소를 제공한다. 첫 방문자는 현재 언어를
확인·전환하고, 상단 `서비스`, `시작하기`, `공개 범위`, `문서`에서 관련 하위 작업을 바로 찾은 뒤
정확한 landing section 또는 운영 가이드 장으로 이동해야 한다.

핵심 작업은 다음과 같다.

1. `/`과 `/ko/`에서 한국어 landing을 보고 `/en/`에서 영어 landing을 본다.
2. KO·EN 전환으로 명시적 언어 주소를 연다.
3. 네 상단 그룹을 pointer, click 또는 keyboard로 열고 각 네 개 하위 링크를 탐색한다.
4. `문서 > 전체 운영 가이드` 또는 `Documentation > Full operations guide`에서 `/docs.html`로
   이동한다.

## 현재 상태 증거

- 관측 시각: 2026-08-23T23:09:41+09:00
- 공개 HTTP 상태: `/` 200, `/ko` 404, `/en` 404
- desktop `1440 × 1000`:
  `.scratch/ui-reviews/2026-08-23-public-site-i18n-navigation/before-live-desktop.png`
  — `85ad8a1dfd125d437c698f1433000f0ad6067f08c4353bacc0314a4277175e02`
- mobile `390 × 844`:
  `.scratch/ui-reviews/2026-08-23-public-site-i18n-navigation/before-live-mobile.png`
  — `53f895068fc23770e5fbebbb752991db560169e0b4a1ec062e81dc0a033c873a`
- 기존 header는 네 항목을 단일 anchor로 제공했고 하위 작업과 명시적 언어 전환이 없었다.
  `전체 운영 가이드` landing action은 이미 `/docs.html` 안정 URL을 사용했다.

## 연구 근거

| 출처 | ID/URL | 관찰 | 적용 |
|---|---|---|---|
| opencodex.me | [공개 landing](https://opencodex.me/) | 상단 그룹 제목, 작은 chevron, 흰 overlay 안의 단순 링크 목록, 검색·테마·언어 도구 분리 | navigation 정보 구조와 compact popup만 채택 |
| opencodex.me 캡처 | `.scratch/ui-reviews/2026-08-23-public-site-i18n-navigation/reference-opencodex-menu.png` — `2dcd5c8b2b417221368d37ab7d92b513e8f4648ba56624f48a951b2b855b73fe` | hover에서 그룹 active surface와 네 하위 링크를 노출 | disclosure 시각 위계 근거 |
| Refero Doppler Docs | [`df663f82-adda-4629-bb41-9df5639a7a69`](https://refero.design/pages/df663f82-adda-4629-bb41-9df5639a7a69) | 개발 문서 header에서 `Guides` dropdown, 검색과 category sidebar를 분리 | 그룹형 전역 navigation 근거 |
| Refero Anthropic Docs | [`f1f7a0a2-e911-4f65-a3e9-3183b79f3e04`](https://refero.design/pages/f1f7a0a2-e911-4f65-a3e9-3183b79f3e04) | 언어 dropdown, 선택 언어 표시, 3열 문서 구조 | 명시적 언어 상태 근거 |
| Starlight 공식 문서 | [Internationalization](https://starlight.astro.build/guides/i18n/) | root locale, locale별 content directory, 기본 언어 fallback과 locale UI 제공 | `/` 한국어 기본과 `/ko/`·`/en/` 정적 route 계약 |

`refero-design` 보조 스킬은 설치되어 있지 않았지만 Refero MCP 화면 검색과 상세 조회는 정상
완료했다. 별도 설치 없이 실제 공개 화면, Refero 근거와 기존 PureCVisor token을 함께 사용했다.

## 채택 결정

- root locale은 한국어로 유지한다. root `index.mdx`를 한국어 정본으로 두고 build 준비 단계에서
  `/ko/` source를 생성한다.
- `/en/`은 같은 정보 구조와 검증된 제품 사실을 영어로 제공한다.
- KO·EN은 현재 언어를 fill과 text로 함께 표시하고 desktop·mobile 모두 노출한다.
- 네 상단 항목은 button disclosure로 바꾸고 각 그룹에 네 개 native anchor를 제공한다.
- pointer hover, click, Tab·ArrowDown·Esc와 outside click을 지원하며 `aria-expanded`,
  `aria-controls`, `aria-haspopup`를 함께 갱신한다.
- popup은 현재 Starlight theme token, 1px border, 작은 radius와 제한된 shadow만 사용한다.
- 전체 운영 가이드 action은 언어와 관계없이 안정 URL `/docs.html`로 연결한다. 현재 전체 가이드
  본문은 한국어 공개 정본이며 영어 landing이 이를 영어 번역 완료로 오인하게 표시하지 않는다.

## 기각 결정

- root를 영어로 바꾸거나 browser 언어 감지로 자동 전환하지 않는다. 사용자 지정 한국어 기본을
  URL과 정적 artifact에서 결정적으로 유지한다.
- opencodex.me의 로고, pill header 전체, 배경 이미지, 브랜드 색과 문구를 복제하지 않는다.
- 네 group마다 큰 mega menu, 이미지 card 또는 2단 중첩 menu를 만들지 않는다.
- 제품 `/docs.html` reader를 별도 영어 문서로 가장하거나 자동 번역하지 않는다.
- 새로운 client framework나 외부 runtime·font·icon 의존성을 추가하지 않는다.

## 수용 기준

- `dist/index.html`, `dist/ko/index.html`, `dist/en/index.html`이 생성되고 각각 canonical `/`,
  `/ko/`, `/en/`과 `lang=ko`, `lang=ko`, `lang=en`을 가진다.
- 세 landing 모두 네 group과 각 네 하위 링크, KO·EN 전환을 제공한다.
- 한국어·영어 전체 운영 가이드 항목의 `href`가 정확히 `/docs.html`이다.
- disclosure는 hover·click·keyboard에서 열리고 Esc·focus out·outside click에서 닫힌다.
- 1440·1280px에서 dropdown이 viewport 안에 있고 390px에서는 명시적 언어 전환이 보이며 수평
  overflow가 없다.
- 세 landing의 axe 위반, browser console·page error와 failed request가 0이다.
- 기존 `/docs.html`, 22개 장 anchor, source map·내부 주소 금지 gate가 회귀하지 않는다.

## 구현 후 검증

- `npm run check`: 60개 Pages artifact와 기존 public docs 계약 통과
- Chromium `1440 × 1000`: `/`, `/ko/`, `/en/` 각각 group 4개, 첫 menu link 4개, overflow 0,
  `lang`과 한국어·영어 제목 확인
- Chromium `390 × 844`: `/ko/`, `/en/` KO·EN 전환 노출, overflow 0
- keyboard 검증: `ArrowDown`으로 첫 하위 링크에 진입하고 `Esc`로 menu를 닫은 뒤 trigger로
  focus를 복원함
- 영어 `Documentation > Full operations guide`를 실제 click해 `/docs.html` 이동과 운영 가이드
  본문 노출을 확인함
- 세 landing axe 위반 0, browser console·page error·failed request 0
- `python3 scripts/check_design_md.py`, `tests/integration/test_design_md_surface.sh`,
  `make check-public-comments`, `python3 scripts/strip_source_comments.py --check`, `git diff --check` 통과
- 한국어 menu 캡처:
  `.scratch/ui-reviews/2026-08-23-public-site-i18n-navigation/after-root-menu.png`
  — `00ddd06f3d30422eeaae9d9653db08a3b6c1324c21dd6eb6ea059457920a66ef`
- 영어 menu 캡처:
  `.scratch/ui-reviews/2026-08-23-public-site-i18n-navigation/after-en-menu.png`
  — `257f53a635fe201e3ffdbb0b9612318f1a2939fc376068532ff50d211ad81416`
- 영어 mobile 캡처:
  `.scratch/ui-reviews/2026-08-23-public-site-i18n-navigation/after-en-mobile.png`
  — `4c48c1124f5ccb8529f8114ddecb60352d1c83cc4513a6af6cca7a42e43de62c`
- commit `ca3a399`을 `main`에 push했고 GitHub Pages run
  [`32645170384`](https://github.com/HardcoreMonk/purecvisor/actions/runs/32645170384)의 build·deploy가
  모두 성공했다.
- 운영 URL `/`, `/ko/`, `/en/`, `/docs.html`은 모두 HTTP 200이며 언어, 네 menu group, canonical
  전체 운영 가이드 link를 확인했다.
- 운영 Chromium에서 keyboard `Esc` focus 복원, 영어 전체 운영 가이드 실제 이동, 390px KO·EN
  전환과 overflow 0, axe 위반·console·page·request 오류 0을 재확인했다.
