# 공개 문서 공통 읽기 축 UI 리뷰

> **일자:** 2026-08-30
> **판정:** LOCAL-PASS — GitHub Pages·custom domain 검증 전
> **승인:** 2026-08-30 사용자 명시 승인
> **대상:** Starlight reader의 H1~H6, 본문, 표, code block과 아키텍처 figure
> **관련 결정:** ADR-0047

## 1. 설계 brief

PureCVisor 공개 기술 문서에서 문장 줄 길이를 유지하면서 표와 code의 공간을 확보하되,
콘텐츠 종류가 바뀔 때 독자의 시작 시선이 좌우로 이동하지 않게 한다. 기존 Starlight shell,
Pretendard, 색상, 좌·우 navigation, 표 focus와 모바일 내부 scroll은 변경하지 않는다.

## 2. 라이브 문제와 측정

중앙 50/68rem 배치는 1920px에서 일반 본문을 `x=536`, 표·code를 `x=392`에 배치했다.
기술 자료가 본문보다 좌우로 각각 144px 돌출되며, 데이터베이스 아키텍처 page의 top-level
콘텐츠 사이에서 이 좌측축 변경이 121회 반복되었다. 표 52개와 code block 9개는 실제 콘텐츠
길이와 관계없이 모두 1,088px를 채웠다.

문제는 1440px에서는 드러나지 않지만 1536px에서 40px, 1600px에서 72px, 1728px에서
136px, 1824px 이상에서 144px로 커졌다. 기존 수용 기준은 page overflow와 표 높이를
검사했지만 반복되는 읽기 축 이동을 포함하지 않았다.

## 3. 연구 근거와 reference lock

| 근거 | 확인한 패턴 | 적용 |
|---|---|---|
| Refero OpenAI Developers·PlanetScale·SST style | 흰 기술 문서 canvas, restrained color, 명확한 typography와 구조 자료의 기능적 확장 | 현재 PureCVisor token·type·surface를 그대로 유지 |
| Refero Cursor Docs `Models` table | 제목·설명·표가 같은 콘텐츠 좌측축을 공유 | 표가 넓어져도 heading·본문 시작점과 일치 |
| Refero Asana Developers `Overview` | 3열 reader에서 중앙 article의 heading·본문이 일관된 좌측축을 사용 | H1~H6와 본문을 하나의 rail에 배치 |
| Refero Anthropic `Quickstart` | 본문과 code block이 같은 시작축을 유지하고 code 자체 overflow를 사용 | code도 표와 같은 적응형 technical 규칙 사용 |
| 사용자 라이브 검토 | 표 박스 확장과 나머지 콘텐츠의 가독성이 어긋남 | 중앙 대칭 breakout을 폐기하고 공통 좌측축 채택 |

Reference lock은 현재 Starlight 3열 shell과 50rem 문장 줄 길이다. 표·code는 같은 좌측축에서
오른쪽으로만 필요한 만큼 확장하고 아키텍처 figure만 설명 media로서 중앙 75rem을 사용한다.

## 4. 대안 비교

1920px 데이터베이스 아키텍처 page의 실제 DOM에 후보 CSS를 주입해 비교했다.

| 후보 | 최대 좌측축 이동 | 축 이동 횟수 | 표 전체 높이 | 판정 |
|---|---:|---:|---:|---|
| 기존 중앙 50/68rem | 144px | 121회 | 18,747px | 기각 — 기술 자료마다 읽기 축 변경 |
| 중앙 50/60rem | 80px | 121회 | 19,447px | 기각 — 폭만 줄여 축 문제 잔존 |
| 공통축 고정 50rem | 0px | 0회 | 20,763px | 기각 — 넓은 표의 줄바꿈·내부 scroll 증가 |
| 공통축 50/68rem | 0px | 0회 | 18,747px | 보류 — 표가 본문보다 과도하게 지배적 |
| 공통축 적응형 50~60rem | 0px | 0회 | 19,447px | 채택 — 읽기 축과 기술 자료 효율의 균형 |

60rem은 약 110개의 monospace 문자를 표시해 기존 code 분석의 99백분위 104자를 수용한다.
표 전체 높이는 50rem 고정 대비 6.3% 줄고, 기존 68rem 대비 세로 증가량은 3.7%에 그친다.

## 5. 결정 ledger

| 결정 | 출처 | 이유 |
|---|---|---|
| H1~H6·본문·표·code 공통 좌측축 | 사용자 피드백, Cursor·Asana·Anthropic | section 전환 때 시작 시선 이동 제거 |
| 일반 본문 최대 50rem | 기존 승인 계약 | 줄 길이와 명시적 릴리스 세 줄 유지 |
| 표·code 기본 50rem, 최대 60rem | 측정 비교 | 짧은 자료의 빈 박스를 막고 긴 자료만 확장 |
| `fit-content` 기반 적응형 폭 | 사용자 문제와 실제 콘텐츠 분포 | 52개 표·9개 code를 모두 최대 폭으로 강제하지 않음 |
| 아키텍처 figure 최대 75rem 중앙 확장 | 기존 SVG 판독성 분석 | 일반 reader 축과 구분되는 설명 media 역할 유지 |
| 1440px 이하 가용 폭 사용 | 기존 responsive·reflow 계약 | page-level overflow 없이 모바일 내부 scroll 유지 |

## 6. 수용 기준

| 우선순위 | 기준 |
|---|---|
| P0 | 1536·1600·1728·1824·1920px에서 heading·본문·표·code의 좌측축 차이가 0이다. |
| P0 | 1920px에서 prose는 800px, 표·code는 콘텐츠에 따라 800~960px, 아키텍처는 최대 1,200px다. |
| P0 | 데이터베이스 문서의 좌측축 전환이 121회에서 0회로 줄어든다. |
| P0 | `최신 릴리스 기준`은 800px 폭과 명시적 세 줄을 유지한다. |
| P1 | 1440px·390px에서 page-level overflow가 없고 표의 focus·방향키 내부 scroll이 유지된다. |
| P1 | light·dark theme, 좌·우 navigation, pagination과 landing 아키텍처가 회귀하지 않는다. |
| P1 | Axe WCAG A/AA와 browser console·page·request 오류가 0이다. |

## 7. 구현

- technical token을 68rem에서 60rem으로 조정했다.
- 75rem reader canvas 안에 최대 60rem rail을 만들고 모든 일반 콘텐츠의 시작 여백을 그
  좌측축에 맞췄다.
- 표·Expressive Code·직접 code block은 `fit-content`, 최소 50rem, 최대 60rem으로 설정했다.
- figure와 `.pcv-architecture-wide`는 일반 rail 여백을 재정의해 최대 75rem 중앙 breakout을
  유지한다.
- H1과 pagination도 같은 rail inset을 사용한다.
- artifact gate가 token, 공통축, 적응형 기술 자료와 아키텍처 breakout 선언을 검사한다.

## 8. 로컬 검증

`npm run check`, `python3 scripts/check_design_md.py`, `make check-public-comments`와
`git diff --check`가 통과했다.

Chromium 152에서 데이터베이스 아키텍처 page를 1536·1600·1728·1824·1920px로 측정한
결과 H1·본문·52개 표·9개 code block·pagination의 좌측축 차이와 축 전환은 모두 0이었다.
1920px의 본문은 `x=456`, `width=800`, 표는 800~960px, code는 800px 또는 881px였다.
아키텍처 probe는 outer canvas와 같은 `x=336`, `width=1200`을 사용했다.

1440px에서는 모든 읽기 요소가 `x=328`, `width=784`, 1280px에서는 `x=328`, `width=624`,
390px에서는 `x=16`, `width=358`로 축소됐다. 모바일에서 내부 scroll이 필요한 표는 29개였고
첫 표의 방향키 scroll은 `0 -> 80px`로 동작했다. 모든 viewport에서 page overflow,
console·page·request 오류와 Axe WCAG A/AA 위반은 0이었다.

`최신 릴리스 기준`은 1920px에서 `x=456`, `width=800`과 명시적 세 줄을 유지했다. 배포 전
live 기준의 동일 문단은 `x=536`, `width=800`이므로 줄 길이는 유지하면서 읽기 축만 80px
왼쪽으로 이동한다. landing 아키텍처는 figure 1,200px, image 1,198px와 원본 SVG link를
유지했으며 page overflow는 0이었다. Firefox 154에서도 1920px 시각 결과를 별도로 확인했다.

로컬 시각 증거 SHA-256은 다음과 같다.

- Chromium 1920px 데이터베이스:
  `ff4630eef3ea454fcaa2483527d41f131c6248eb02b1a8820137afe25e651bb8`
- Chromium 390px 데이터베이스:
  `e03da3b5bca81b746f27ed2c58c0c2184fca03b7054103d8cede1e19cfb902b3`
- Chromium 1920px 릴리스 문단:
  `21b955e5295d407c15e86c413f45b2a3db05743ff0c1dd9aaef775763b7401fc`
- Firefox 154 1920px 데이터베이스:
  `17061bb64704d595d8e71128cdad23618725556acba80f52a50e39b2c6fb97dc`

GitHub Pages와 custom domain 확인 뒤 최종 판정을 갱신한다.
