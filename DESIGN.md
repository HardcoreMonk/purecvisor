# PureCVisor Single Edge DESIGN.md

이 문서는 PureCVisor Single Edge Web UI의 시각 규격이다. 제품 기능, 설치,
운영 절차는 `docs/GUIDE.md`에 두고, UI 작업 전 색상, 타이포그래피, 밀도,
컴포넌트 상태, 프리뷰 기준은 이 파일을 먼저 확인한다.

- 적용 범위: `ui/index.html`, `ui/style.css`, `ui/modules/*.js`,
  `ui/docs.html`, `ui/guide.html`, `ui/guide-content.md`, `ui/samples/*.html`
- 기본 프리뷰: `ui/samples/design-system-preview.html`
- 정적 검증: `python3 scripts/check_design_md.py`
- 테마 근거: `docs/adr/0016-supanova-theme-reduction.md`
- 프론트엔드 구조 근거: `docs/adr/0013-frontend-iife-module-scope.md`

## 1. Visual Theme & Atmosphere

PureCVisor Web UI는 단일 노드 운영자가 반복적으로 보는 작업 콘솔이다. 시각
톤은 장식보다 판독성, 상태 식별, 밀도, 빠른 비교를 우선한다.

- 기본 인상: 문서 포털과 이어지는 흰 운영 canvas, soft-gray shell, 제한된 teal accent,
  짙은 본문과 명확한 상태색. 선택형 high-contrast/mockup 테마는 dark surface를 유지한다.
- 화면 구성: 상단 toolbar, activity/sidebar, breadcrumb, 콘텐츠 영역이 먼저 보이고
  카드와 표는 운영 데이터를 담는 단위로만 사용한다.
- 장식 제한: discrete orb, bokeh blob, 과한 gradient hero, neon glow, clip-path
  장식, cyan-to-magenta gradient를 새로 추가하지 않는다.
- 모션: 상태 전환과 hover에만 짧게 사용한다. 기본 easing은
  `--pcv-spring`(`cubic-bezier(0.16,1,0.3,1)`)이다.
- 운영 문맥: 장애, 보안, 스토리지, VM lifecycle 화면은 마케팅형 hero가 아니라
  조작 가능한 데이터와 다음 action을 첫 화면에 둔다.

## 2. Color Tokens & Roles

색상은 `ui/style.css`의 CSS 변수와 Supanova theme override를 기준으로 사용한다.
새 색을 직접 박기 전에 기존 token으로 표현 가능한지 먼저 확인한다.

| Token | 기본값 | 역할 |
|---|---:|---|
| `--bg` | `#ffffff` | 기본 Supanova 전체 배경, docs canvas와 동일 |
| `--bg2` | `#f5f7fa` | sidebar와 soft shell, docs soft와 동일 |
| `--bg3` | `#edf1f5` | selected row, progress track, field 보조 surface |
| `--bg-panel` | `#ffffff` | card, toolbar, dropdown surface |
| `--border` | `#dce1e8` | 기본 divider, docs line과 동일 |
| `--border-panel` | `#dce1e8` | panel hairline |
| `--fg` | `#171c24` | 본문 primary text, docs ink와 동일 |
| `--fg2` | `#5c6675` | secondary text, helper, muted value |
| `--accent` | `#12627a` | focus, selected, primary interactive, docs accent와 동일 |
| `--cyan` | `#12627a` | brand continuity and links |
| `--green` | `#18794e` | success, running, healthy |
| `--yellow` | `#8a6100` | warning, pending, caution |
| `--red` | `#b42332` | error, destructive, blocked, docs error와 동일 |
| `--peach` | `#9a5b13` | rare highlight only |
| `--magenta` | `#9b3d6e` | rare accent only, not gradient partner |

운영 규칙:

- `--accent`는 focus ring, selected tab, primary affordance에 제한한다.
- 기본 `supanova`의 canvas/soft/ink/muted/line/accent/error는 `ui/docs.html`의
  `--docs-*` 역할과 같은 값을 사용한다. dashboard와 docs 중 한쪽만 바꾸지 않는다.
- 상태색은 의미를 고정한다. success는 `--green`, warning은 `--yellow`, error와
  destructive action은 `--red`를 사용한다.
- 표와 카드 배경은 `--bg2`, `--bg3`, `--bg-panel` 안에서 해결한다.
- 한 화면이 teal/cyan 계열로만 채워지면 안 된다. neutral surface와 semantic
  state color를 함께 사용한다.
- alpha overlay는 data가 사라질 정도로 어둡게 덮지 않는다.
- 현재 허용 theme id는 `supanova`, `supanova-cyan`, `supanova-hicontrast`,
  `supanova-mockup` 4개다. `supanova-mockup`은 R4에서 승인된 운영 콘솔 목업 팔레트이며
  Prefs의 select와 preview에서 다른 허용 테마와 같은 방식으로 선택한다.
  `supanova-emerald`, `supanova-light` 같은 제거된 theme id를 UI 선택지나 CSS
  override에 다시 추가하지 않는다.

## 3. Typography Rules

타이포그래피는 운영 데이터 판독과 반복 작업 속도를 우선한다.

| Token | 역할 |
|---|---|
| `--font-sans` | 기본 본문, form, table, 운영 console copy |
| `--font-display` | 제목, shell label, metric title |
| `--font-mono` | 숫자, UUID, IP, path, code, token |

- 기본 본문은 self-hosted Pretendard(`ui/vendor/pretendard/`)를 우선하고,
  display는 Outfit fallback을 허용한다. 본문 기준 line-height는 `1.5`를
  사용한다.
- viewport width에 따라 font-size를 직접 scaling하지 않는다.
- letter-spacing은 기본 0을 유지한다. 좁은 uppercase label만 예외적으로 CSS
  기존 값을 따른다.
- 공통 command/navigation icon은 로컬 Coolicons 스프라이트
  `ui/vendor/coolicons/coolicons.svg`를 우선한다. 런타임 외부 아이콘 API나 CDN은
  사용하지 않는다.
- dashboard/card heading은 작고 단단하게 둔다. hero-scale type은 landing 또는
  login pitch 같은 진짜 hero에만 사용한다.
- 숫자, byte, percent, port, IP, UUID는 `--font-mono` 또는 기존 monospace helper를
  사용해 열 정렬과 scan을 돕는다.
- 버튼 내부 텍스트는 줄바꿈과 최소 너비를 고려해 overflow가 나지 않게 한다.

## 4. Component States

모든 interactive component는 normal, hover, focus, active, disabled, loading,
empty, error 상태를 구분해야 한다.

| State | 시각 규칙 |
|---|---|
| Normal | 배경과 border는 낮은 대비, 텍스트는 `--fg`/`--fg2` |
| Hover | border나 text에 `--accent`, 필요 시 `translateY(-1px)` 이하 |
| Focus | keyboard focus가 보이는 outline 또는 accent border 필수 |
| Active/Selected | accent border와 더 진한 surface를 함께 사용 |
| Disabled | opacity만 낮추지 말고 cursor/contrast/tooltip copy도 함께 고려 |
| Loading | spinner, skeleton, progress text 중 하나를 안정된 크기로 표시 |
| Empty | 원인과 다음 action을 한 줄로 제시하되 장식 card를 늘리지 않는다 |
| Error | `--red`와 plain text message를 사용하고 retry action을 분리 |

상태 구현은 Vanilla JS와 `PCV.*` namespace 규칙을 유지한다. API 경로는
`ui/modules/endpoints.js`의 `EP` 레지스트리를 사용하고, `innerHTML`은 sanitizer
helper를 거친다.

## 5. Dashboard Density

대시보드는 운영자가 한 화면에서 상태를 비교하는 공간이다. 정보량을 줄이는 대신
반복 항목의 크기와 gap을 일관되게 유지한다.

- 기본 grid는 기존 `.sg`, `.grid-*`, responsive breakpoint를 사용한다.
- 카드 gap은 10-16px 범위를 기본으로 한다. 한 화면에 들어가는 카드 수를 임의로
  줄이는 oversized 카드 패턴은 피한다.
- metric card는 label, value, delta/status를 같은 위치에 반복 배치한다.
- toolbar, filter, action row는 콘텐츠 위에 밀도 있게 배치하고 card 안에 다시
  toolbar card를 넣지 않는다.
- dashboard summary에는 핵심 상태, 경고, 최근 activity, primary action이 첫
  viewport에 보여야 한다.
- fixed-format UI(boards, counters, tiles, icon button groups)는 `minmax`,
  `aspect-ratio`, 고정 button size 등으로 layout shift를 막는다.

## 6. Table Rules

표는 반복 운영 데이터의 기본 표현이다. 카드 목록으로 바꾸기 전에 행 비교가 더
중요한지 먼저 판단한다.

- 기본 table은 full width, compact row, sticky header가 가능한 구조를 유지한다.
- `th`는 짧은 label, `td`는 값 중심으로 둔다. 긴 설명문을 cell 안에 넣지 않는다.
- 행 hover는 border/accent로 충분하다. row background를 과하게 바꾸지 않는다.
- 수치 열은 오른쪽 정렬 또는 monospace를 사용한다.
- 행 action은 우측 끝에 icon 또는 짧은 button group으로 묶는다.
- 모바일에서 카드형 전환이 필요한 표는 `table.card-mobile`을 사용한다.
- 빈 표는 empty state 한 줄과 primary next action만 둔다.

### 6.1 알림 severity lane

- 알림 이력 행은 `alert-row-crit`, `alert-row-warn`, `alert-row-idle` modifier와
  `border-inline-start: 3px`를 사용한다. 색은 각각 `--st-crit`, `--st-warn`,
  `--st-idle` semantic token으로만 지정한다.

| 행 modifier | lane token | pill과 텍스트 |
|---|---|---|
| `alert-row-crit` | `--st-crit` | `pill-crit` / `CRIT` |
| `alert-row-warn` | `--st-warn` | `pill-warn` / `WARN` |
| `alert-row-idle` | `--st-idle` | `pill-idle` / `UNKNOWN` |

- lane과 `HN.statusPill`의 `CRIT`, `WARN`, `UNKNOWN` 텍스트를 함께 표시하며
  색상만으로 상태를 전달하지 않는다.
- lane 때문에 행 배경 tint, glow, shadow를 추가하지 않는다.
- Time/Metric/Value는 `--font-mono`, Value는 오른쪽 정렬한다.
- 유효하지 않은 시간과 값은 `—`, 알 수 없는 심각도는 `UNKNOWN`으로 표시한다.

## 7. Card Rules

`.hc`가 기본 card shell이다. 카드 남용을 피하고 반복 항목, metric, modal 내부
섹션처럼 실제로 frame이 필요한 곳에만 사용한다.

- 기본 card는 `.hc`를 사용한다. 배경은 `--bg-panel`, border는 `--border-panel`,
  radius는 `--r` 또는 최대 8px를 기준으로 한다.
- 카드 안에 또 다른 floating card를 넣지 않는다. 필요하면 heading, divider,
  compact row로 계층을 만든다.
- 카드 hover는 border accent와 1px 이하 이동 정도로 제한한다.
- card title은 12-16px 범위의 짧은 명사구로 둔다.
- status badge, progress bar, action row는 card 하단 또는 우측 상단에 일관 배치한다.
- 숫자 라벨이 있는 progress bar는 낮은 수치에서도 라벨 전체가 잘리지 않아야 한다.

## 8. Button Rules

`.btn`이 기본 button class이고, primary action에는 `.btn-primary`를 사용한다.

- command button은 가능하면 icon 또는 icon+짧은 label을 사용한다.
- destructive button은 `--red` 의미를 분명히 하고 확인 modal과 audit context를
  분리한다.
- disabled button은 비활성 이유가 화면 맥락에서 드러나야 한다.
- loading button은 label 폭을 유지해서 layout shift를 만들지 않는다.
- button radius는 기존 pill style을 따르되, 카드/모달 container radius를 키우는
  방식으로 균형을 맞추지 않는다.
- 모바일 터치 target은 40px 이상을 유지한다.

## 9. Modal Rules

`.modal`은 사용자의 현재 task를 잠시 중단시키는 surface다. 단순 정보 노출은
가능하면 inline panel이나 drawer를 먼저 고려한다.

- 기본 width는 `min(700px, 94vw)` 패턴을 따른다. 넓은 form은 `.modal-wide`를 쓴다.
- 제목, 요약, form/control, footer action 순서를 유지한다.
- destructive confirm은 plain text와 명확한 대상 이름을 사용한다.
- destructive confirm의 초기 focus는 위험 action이 아니라 취소 또는 안전한 control에 둔다.
- 모든 dialog는 현재 언어의 고유 accessible name을 가져야 한다. 콘텐츠 제목은 `aria-labelledby`,
  시각 제목이 없는 bare dialog는 명시적 `aria-label`로 연결하고 generic `Dialog` 이름을 남기지 않는다.
- focus trap, Esc 닫기, backdrop 닫기 정책은 기존 공통 modal helper를 따른다.
- 중첩 dialog를 닫으면 부모의 입력 상태와 focus를 복원하고, `noDismiss` surface는 소유자의
  명시적 닫기 action으로만 종료한다.
- command palette는 input `combobox`, 결과 `listbox`/`option`,
  `aria-activedescendant`/`aria-selected`를 함께 사용하고 Enter·방향키·Esc hint를 노출한다.
- 파일 선택 행은 click 가능한 `div` 대신 native button을 사용한다. 480px에서는 dialog 좌우
  inset과 40px action target을 유지하고 shortcut 목록은 한 열로 전환한다.
- modal 안의 table은 max-height와 overflow를 지정해 footer action이 밀리지 않게 한다.
- 성공/실패 결과는 toast 또는 job completion 패턴으로 이어지게 한다.
- modal mutation 실패는 열린 dialog 내부 `role=alert`에 원문을 표시하고 입력값과 재시도
  action을 보존한다. native dialog backdrop 뒤의 toast만으로 실패를 전달하지 않는다.
- 생성 한도·주소 범위처럼 제출 전 판단에 필요한 계약은 form 내부 compact note로 표시하고,
  backend에 없는 고정 상한이나 추정 용량은 만들지 않는다.
- VPC와 첫 subnet처럼 한 Job에서 함께 생성되는 복합 form은 wizard로 분리하지 않고 의미별
  `fieldset`과 `legend`로 묶는다. 넓은 화면은 compact 2열, 768px 이하는 1열로 전환하며
  primary action은 전체 작업(`VPC + Subnet 생성`)을 정확히 명명한다.

## 10. Responsive & Accessibility

- <= 1024px: sidebar와 grid를 축소하고 핵심 action을 toolbar에 남긴다.
- <= 768px: 단일 컬럼, hamburger/mobile nav, `table.card-mobile` opt-in을 사용한다.
- <= 480px: 버튼과 label overflow를 먼저 확인하고, 필요하면 action을 menu로 묶는다.
- 모든 form control과 icon button은 label 또는 aria-label을 가진다.
- focus outline은 제거하지 않는다.
- 색상만으로 상태를 구분하지 않는다. badge text, icon, label을 함께 사용한다.
- theme allowlist 또는 high-contrast theme 변경은 `scripts/check_supanova_themes.py`와 함께 검증한다.

## 11. Do's and Don'ts

Do:

- `ui/style.css` token과 기존 Supanova theme override를 먼저 사용한다.
- 운영 데이터는 table, compact grid, metric card로 비교 가능하게 만든다.
- modal, toast, job completion, audit context는 같은 사용자 흐름으로 연결한다.
- `ui/samples/design-system-preview.html`을 갱신해 새 visual rule을 눈으로 확인한다.
- CSS/HTML 변경 후 `python3 scripts/check_design_md.py`를 실행한다.

Don't:

- `docs/GUIDE.md` 안에 상세 색상표와 component state를 중복 관리하지 않는다.
- 새로운 frontend framework, runtime CDN, external font/API fetch를 추가하지 않는다.
- landing page식 hero, decorative card-heavy layout, orb/blob background를 운영
  콘솔 화면에 넣지 않는다.
- sanitizer 없는 `innerHTML` 대입이나 `EP` registry 우회 fetch를 추가하지 않는다.
- 검증 없이 Supanova theme allowlist, CSS token, button/modal/table 규칙을 바꾸지 않는다.

## 12. Reference Pattern Borrowing

외부 디자인 레퍼런스는 브랜드 복제가 아니라 운영 UX 패턴 차용으로만 사용한다.
색상, 로고, 고유 일러스트, 문구, 브랜드형 hero를 가져오지 않고 PureCVisor의
Supanova token, Pretendard, Coolicons, Single Edge 운영 문맥 안에서 재해석한다.

우선 차용 가능한 패턴:

- Linear: 명확한 surface ladder, 1px hairline, compact status badge, 고밀도 리스트.
  VM/컨테이너/작업 목록처럼 빠른 스캔과 비교가 필요한 화면에만 적용한다.
- Sentry: severity lane, 이벤트 그룹화, triage 우선순위, 원인/대상/시간을 같은 행에
  둔 장애 분석 구조. 알림, 보안 이벤트, audit, job completion 화면에 적용한다.
- IBM Carbon: 표/폼/상태 색상/접근성의 엄격한 규율. RBAC, API 관리, 설정 관리,
  운영 테이블처럼 실수 비용이 큰 화면에 적용한다.
- Raycast와 OpenCode: command palette, key hint, terminal-like command row.
  Ctrl+K, 운영 명령 실행, 빠른 이동 UX에 적용하되 모든 조작은 RBAC와 audit 흐름을
  벗어나지 않는다.
- HashiCorp: 인프라 제품 설명의 구조화, 기능별 identity surface, 아키텍처 문서화.
  OVN 설명과 GUIDE/ADR의 기술 신뢰도 표현에 적용한다.

적용 금지:

- 외부 브랜드의 로고, 마스코트, 고유 일러스트, full-bleed photography를 복제하지 않는다.
- warm cream, consumer hero, cinematic automotive layout처럼 운영 콘솔과 충돌하는
  분위기를 도입하지 않는다.
- 새 레퍼런스가 기존 `DESIGN.md` token, 테마 allowlist, 접근성 규칙보다 우선하지
  않는다.

차용 패턴 샘플은 `ui/samples/design-borrowing-mockup.html`에서 확인한다. 실제
제품 화면으로 이식할 때는 샘플의 독립 shell을 그대로 쓰지 않고, 기존 Web UI shell의
콘텐츠 영역에만 패턴을 적용한다.

`ui/docs.html` 문서 shell은 PureCVisor Pretendard·Coolicons와 정적 설치 예시만 사용한다.
운영 dashboard와 전역 console topbar는 문서 shell 패턴의 적용 범위가 아니며, 문서용
흑백 active state가 상태·경고 색상의 의미를 덮지 않게 한다.

## 13. Agent Prompt Guide

UI 작업을 맡은 에이전트는 다음 순서로 진행한다.

1. `AGENTS.md`, `DESIGN.md`, `docs/ADR_INDEX.md`를 확인한다.
2. 기능/운영 설명은 `docs/GUIDE.md`, 시각 규격은 `DESIGN.md`에 분리한다.
3. 화면 변경 전 `ui/style.css`의 token, `.hc`, `.btn`, `.modal`, table 규칙을 재사용한다.
4. 필요한 경우 `ui/samples/design-system-preview.html` 또는 관련 `ui/samples/*.html`을
   함께 갱신한다.
5. 변경 후 최소 검증으로 다음을 실행한다.

```bash
python3 scripts/check_design_md.py
bash tests/integration/test_design_md_surface.sh
PCV_NO_DEPLOY=1 scripts/bundle-ui.sh
node --check ui/app.bundle.js
git diff --check
```

## 14. Documentation Portal

`ui/docs.html`은 운영 콘솔이 아니라 긴 Single Edge 가이드를 찾고 읽는 통합 문서 shell이다.
따라서 landing 규모 제목과 카테고리 grid를 허용하지만 다음 계약을 지킨다.

- 콘텐츠 정본은 `ui/guide-content.md`이며, `ui/docs.html`이 목록·검색·상세 reader를 한 shell에서
  제공한다. `ui/guide.html`은 기존 hash를 보존해 `docs.html`로 보내는 호환 진입점이다.
- 정보 구조는 global navigation, 문서 검색, 소개 band, 빠른 진입, 작업별 category 순서다.
- 흰 본문 surface와 graphite 소개 band, 1px neutral border를 사용한다. 전역 문서 header는
  최대 `1248×51px`, `999px` radius, `blur(18px)`의 floating glass pill로 고정하고 문서 navigation의
  active state는 `#0d0d0d` 배경과 흰 글자를 사용한다. cyan은 focus·본문 link에 제한한다.
  외부 문서 사이트의 로고, 고유 색, 문구와 일러스트는 복제하지 않는다.
- 8개 작업 category를 제목·설명으로 구분하고, 그 아래 `guide-content.md`의 숫자형 H2
  22개를 각각 제목·한 줄 설명·정확한 deep link로 모두 노출한다. 상위 category 하나만
  연결해 나머지 장을 검색 안에 숨기지 않는다.
- chapter card는 desktop 3열, 1120px 이하 2열, 520px 이하 1열로 전환하며 radius는 8px을
  넘기지 않는다. 그림자보다 border와 surface 차이를 사용하고, 전체 본문은 상세 reader에 둔다.
- 상세 reader는 desktop에서 좌측 `300px` 전체 장 navigation, 중앙 `780px` 이하 본문, 우측
  현재 장 목차의 3열이다. `1320px` 이하에서는 우측 목차를, `1100px` 이하에서는 좌측 navigation을
  닫고 `reader-mobile-toc`과 drawer로 바꾼다. active 장은 검정 배경·흰 글자로 표시한다.
- code block은 중립 1px frame 안에 language toolbar와 자체 horizontal scroll을 두고,
  복사 button은 모든 breakpoint에서 높이 `40px` 이상을 유지한다. deep link 정렬은 local font
  로딩이 끝난 뒤에도 요청한 heading을 header와 mobile TOC 아래에 유지한다.
- 검색 결과 단위는 `##`/`###` heading이고 본문 용어도 해당 heading으로 연결한다. loading,
  일치 없음, fetch 오류를 구분하며 오류 중에도 정적 category 링크와 전체 가이드 fallback을 유지한다.
- 검색 suggestion은 `combobox`/`listbox`/`option`, `aria-activedescendant`, 방향키, Enter, Esc와
  `Ctrl+K`/`⌘K`를 지원한다. Markdown 문자열은 DOM text node로만 표시한다.
- `docs.html`, `guide.html`, `guide-content.md`는 배포 목록과 service worker 정적 자산에 함께 둔다.
  `docs.html` 같은 독립 정적 자산 변경도 `UI_CACHE_INPUTS` identity를 바꿔 기존 precache를
  무효화해야 한다.
- REST API 장문 가이드는 `docs.html#14-rest-api`만 소유한다. 제품 shell에는 별도 REST reader,
  navigation item, 전용 style을 두지 않는다. 과거 `#/restguide` bookmark는 resource suffix를
  버리고 canonical REST 장으로 즉시 이동시키는 호환 진입점으로만 처리한다.
